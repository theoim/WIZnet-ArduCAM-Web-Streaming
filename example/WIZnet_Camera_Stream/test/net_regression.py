#!/usr/bin/env python3
"""
Network regression for the camera server's HTTP layer (Phase 1b).

Runs against a board on the LAN - no browser needed, and none should be open
on the stream while it runs (the stream holds one of the four TLS sessions).

    python test/net_regression.py                 # https://192.168.11.7
    python test/net_regression.py --host 192.168.11.7 --plain   # port 80 image
    python test/net_regression.py --quick         # skip the slow timeout cases

What it checks, and why each matters:
  routes      every existing endpoint still answers as before; 404 vs 405+Allow
  split       a request fed 1 byte at a time across many TLS records completes
  limits      header block at 1.2 KB passes, over 1536 B gets 431
  policy      505 / 400 / 501 / 417 / 411 for the default-reject cases
  pipeline    two requests in one write: exactly one response, then close
  timeouts    raw TCP with no handshake, idle TLS, partial request, 1 byte/s
              drip: each closed by the server inside its deadline
  slots       four idle TLS connections, a fifth is refused, service returns
              once the deadlines reclaim them
  status      200 status polls: all 200, latency p50/p95/max
Counters from /api/status "http" are printed before and after.

The CA certificate (public) is read from ~/.camera-stream-ca/ca.crt when it
exists; otherwise certificate checks are off. No private key is touched.
"""

import argparse
import json
import os
import socket
import ssl
import statistics
import sys
import time

CA = os.path.expanduser("~/.camera-stream-ca/ca.crt")


class Board:
    def __init__(self, host, port, plain):
        self.host, self.port, self.plain = host, port, plain
        if not plain:
            if os.path.exists(CA):
                self.ctx = ssl.create_default_context(cafile=CA)
                self.ctx.check_hostname = False      # cert SAN is the IP; fine either way
            else:
                self.ctx = ssl.create_default_context()
                self.ctx.check_hostname = False
                self.ctx.verify_mode = ssl.CERT_NONE
            self.ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        # Reuse the TLS session like a browser does. Without it every request
        # is a full ECDHE handshake (~200 ms of board CPU), which measures the
        # handshake and stalls a running stream instead of testing HTTP.
        self.session = None

    def connect(self, handshake=True, timeout=15.0):
        s = socket.create_connection((self.host, self.port), timeout=timeout)
        # Browsers and curl send with Nagle off. Leaving it on here made every
        # request wait ~200 ms for the board's delayed ACK of the TLS Finished
        # (measured 2026-10-23: 203 ms with Nagle, 0-16 ms without) - a client
        # artifact, not server latency.
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if self.plain or not handshake:
            return s
        t = self.ctx.wrap_socket(s, server_hostname=self.host, session=self.session)
        if t.session is not None:
            self.session = t.session
        return t

    def roundtrip(self, raw, timeout=15.0, chunks=None, gap=0.0):
        """Send raw bytes (optionally in chunks with a gap), read until close."""
        s = self.connect(timeout=timeout)
        try:
            if chunks:
                for i in range(0, len(raw), chunks):
                    s.sendall(raw[i:i + chunks])
                    if gap:
                        time.sleep(gap)
            else:
                s.sendall(raw)
            return read_all(s, timeout)
        finally:
            s.close()

    def get(self, path, extra=b""):
        req = b"GET " + path.encode() + b" HTTP/1.1\r\nHost: " + self.host.encode() + b"\r\n" + extra + b"\r\n"
        return self.roundtrip(req)

    def status(self):
        st, hdr, body = parse(self.get("/api/status"))
        return json.loads(body) if st == 200 else None


def read_all(s, timeout):
    s.settimeout(timeout)
    out = b""
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            out += d
    except (socket.timeout, ssl.SSLError, ConnectionResetError, OSError):
        pass
    return out


def parse(resp):
    if not resp:
        return 0, {}, b""
    head, _, body = resp.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    try:
        status = int(lines[0].split()[1])
    except (IndexError, ValueError):
        return -1, {}, body
    hdr = {}
    for ln in lines[1:]:
        k, _, v = ln.partition(b":")
        hdr[k.strip().lower().decode()] = v.strip().decode(errors="replace")
    return status, hdr, body


RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))


def wait_closed(s, limit):
    """Seconds until the server closes s (or None if it stays open past limit)."""
    t0 = time.monotonic()
    s.settimeout(limit)
    try:
        while True:
            d = s.recv(4096)
            if not d:
                return time.monotonic() - t0
    except socket.timeout:
        return None
    except (ssl.SSLError, ConnectionResetError, OSError):
        return time.monotonic() - t0


def t_routes(b):
    print("routes")
    for path, want, ctype in [("/", 200, "text/html"), ("/index.html", 200, "text/html"),
                              ("/logo.png", 200, "image/png"), ("/api/status", 200, "application/json"),
                              ("/api/controls", 200, "application/json"), ("/api/cam?brightness=1", 405, None),
                              ("/load?kb=1", 200, None), ("/load", 200, None),
                              ("/nope", 404, None), ("/api/status/x", 404, None)]:
        st, hdr, body = parse(b.get(path))
        ok = st == want and (ctype is None or hdr.get("content-type", "").startswith(ctype))
        check(f"GET {path} -> {want}", ok, f"got {st}")
    st, hdr, _ = parse(b.roundtrip(b"POST /api/status HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"))
    check("POST /api/status -> 405 + Allow: GET", st == 405 and hdr.get("allow") == "GET", f"got {st} {hdr.get('allow')}")
    st, _, _ = parse(b.roundtrip(b"DELETE / HTTP/1.1\r\nHost: x\r\n\r\n"))
    check("DELETE / -> 405", st == 405, f"got {st}")
    st, _, body = parse(b.get("/api/controls"))
    try:
        ok = isinstance(json.loads(body), list)
    except ValueError:
        ok = False
    check("/api/controls is a JSON list", ok, repr(body[:40]))
    s = b.status()
    check("/api/status has http counters", bool(s) and "http" in s, "")


def post_form(b, body, ctype=b"application/x-www-form-urlencoded"):
    head = b"POST /api/cam HTTP/1.1\r\nHost: x\r\n"
    if ctype:
        head += b"Content-Type: " + ctype + b"\r\n"
    head += b"Content-Length: %d\r\n\r\n" % len(body)
    return parse(b.roundtrip(head + body))


def t_controls(b):
    """Phase 2c. Only value 0 is ever written: on the LH module that is the
    value every tested register already holds, and the module keeps settings
    across power cycles, so the test must not leave anything else behind."""
    print("controls (2c)")
    st, hdr, _ = parse(b.get("/api/cam"))
    check("GET /api/cam -> 405 + Allow: POST", st == 405 and hdr.get("allow") == "POST",
          f"got {st} {hdr.get('allow')}")
    st, _, body = parse(b.get("/api/controls"))
    ctrls = {c["name"]: c for c in json.loads(body)}
    print(f"  driver offers: {', '.join(ctrls) or 'none'}")
    st, _, _ = post_form(b, b"brightness=0", ctype=b"text/plain")
    check("text/plain -> 415", st == 415, f"got {st}")
    st, _, _ = post_form(b, b"brightness=0", ctype=None)
    check("no Content-Type -> 415", st == 415, f"got {st}")
    st, _, body = post_form(b, b"")
    check("empty body -> 400", st == 400 and b"no control" in body, f"got {st} {body[:60]!r}")
    st, _, body = post_form(b, b"zoom=1")
    check("unknown control -> 400 names it", st == 400 and b'"control":"zoom"' in body,
          f"got {st} {body[:80]!r}")
    if "brightness" not in ctrls:
        print("  (no brightness control: write checks skipped)")
        return
    hi = ctrls["brightness"]["max"] + 1
    before = b.status().get("ctrl_applied")
    st, _, body = post_form(b, b"brightness=%d" % hi)
    check(f"brightness={hi} -> 400 out of range, not clamped", st == 400 and b"out of range" in body,
          f"got {st} {body[:80]!r}")
    st, _, body = post_form(b, b"brightness=0&zoom=1")
    check("one bad pair -> 400, nothing written",
          st == 400 and b.status().get("ctrl_applied") == before, f"got {st}")
    st, _, body = post_form(b, b"brightness=0&brightness=0")
    check("duplicate -> 400", st == 400 and b"twice" in body, f"got {st}")
    st, _, body = post_form(b, b"brightness=abc")
    check("brightness=abc -> 400", st == 400, f"got {st}")
    names = [n for n in ("brightness", "flip", "mirror") if n in ctrls]
    form = "&".join(f"{n}=0" for n in names).encode()
    t0 = time.monotonic()
    st, _, body = post_form(b, form)
    ms = (time.monotonic() - t0) * 1000
    try:
        s = json.loads(body)
    except ValueError:
        s = {}
    ok = st == 200 and all(s.get("ctrl", {}).get(n) == 0 for n in names)
    check(f"{form.decode()} -> 200, ctrl shows written 0 ({ms:.0f} ms)", ok,
          f"got {st} {body[-120:]!r}")
    check("ctrl_applied +1", s.get("ctrl_applied") == (before or 0) + 1,
          f"{before} -> {s.get('ctrl_applied')}")


def t_split(b):
    print("split")
    req = b"GET /api/status HTTP/1.1\r\nHost: 192.168.11.7\r\nUser-Agent: split-test\r\n\r\n"
    st, _, body = parse(b.roundtrip(req, chunks=1, gap=0.01))
    check("1-byte records, 10 ms apart", st == 200 and body.startswith(b"{"), f"got {st}")
    st, _, _ = parse(b.roundtrip(req, chunks=7, gap=0.05))
    check("7-byte records, 50 ms apart", st == 200, f"got {st}")


def t_limits(b):
    print("limits")
    st, _, _ = parse(b.get("/api/status", b"Cookie: " + b"a" * 1200 + b"\r\n"))
    check("1.2 KB cookie -> 200", st == 200, f"got {st}")
    st, _, _ = parse(b.get("/api/status", b"X-Pad: " + b"a" * 1600 + b"\r\n"))
    check("header block > 1536 B -> 431", st == 431, f"got {st}")
    st, _, _ = parse(b.get("/" + "p" * 300))
    check("path > 255 B -> 414", st == 414, f"got {st}")


def t_policy(b):
    print("policy")
    cases = [
        (b"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505, "HTTP/2.0"),
        (b"GET / HTTP/1.1\r\n\r\n", 400, "no Host"),
        (b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, "duplicate Host"),
        (b"POST /api/status HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 501, "chunked"),
        (b"POST /api/status HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\n\r\n", 417, "Expect"),
        (b"POST /api/status HTTP/1.1\r\nHost: x\r\n\r\n", 411, "POST without Content-Length"),
        (b"POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na", 400, "duplicate Content-Length"),
        (b"GET /api/../x HTTP/1.1\r\nHost: x\r\n\r\n", 400, "dot-dot"),
        (b"GET /a%00 HTTP/1.1\r\nHost: x\r\n\r\n", 400, "%00 in path"),
        (b"GET / HTTP/1.1\nHost: x\n\n", 400, "LF-only line ends"),
    ]
    for raw, want, name in cases:
        t0 = time.monotonic()
        st, _, _ = parse(b.roundtrip(raw, timeout=12))
        el = time.monotonic() - t0
        check(f"{name} -> {want}", st == want and el < 2.0, f"got {st} in {el:.1f} s")


def t_pipeline(b):
    print("pipeline")
    two = (b"GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n"
           b"GET /logo.png HTTP/1.1\r\nHost: x\r\n\r\n")
    resp = b.roundtrip(two)
    n = resp.count(b"HTTP/1.1 ")
    check("two requests in one write -> one response", n == 1 and resp.startswith(b"HTTP/1.1 200"), f"{n} responses")


def t_timeouts(b, quick):
    print("timeouts")
    s = b.connect(handshake=False)
    t = wait_closed(s, 20)
    s.close()
    lim = 10.0 if not b.plain else 10.0
    check("TCP connect, nothing sent -> closed", t is not None and t <= lim + 2.5,
          f"{t:.1f} s" if t else "still open after 20 s")
    if b.plain:
        pass
    else:
        s = b.connect()                        # handshake done, no request
        t = wait_closed(s, 20)
        s.close()
        check("TLS idle, no request -> closed ~10 s", t is not None and 8.0 <= t <= 12.5,
              f"{t:.1f} s" if t else "still open after 20 s")
    s = b.connect()
    s.sendall(b"GET /api/status HTTP/1.1\r\nHo")
    resp = read_all(s, 12)
    s.close()
    st, _, _ = parse(resp)
    check("partial request then silence -> 408", st == 408, f"got {st}")
    if quick:
        return
    # One byte a second, and between bytes wait up to 1 s for the server to
    # answer or close - so the 408 is read before our next byte could draw a
    # reset, and the close time is measured rather than inferred.
    s = b.connect()
    req = b"GET /api/status HTTP/1.1\r\nHost: x\r\nX-Slow: " + b"a" * 40 + b"\r\n\r\n"
    t0 = time.monotonic()
    got, closed_at = b"", None
    s.settimeout(1.0)
    try:
        for i in range(len(req)):
            s.sendall(req[i:i + 1])
            try:
                d = s.recv(4096)
                got += d
                if not d or got:
                    closed_at = time.monotonic() - t0
                    break
            except socket.timeout:
                pass
            if time.monotonic() - t0 > 25:
                break
    except (OSError, ssl.SSLError):
        closed_at = time.monotonic() - t0
    got += read_all(s, 2)
    s.close()
    st, _, _ = parse(got)
    check("1 byte/s drip -> 408 at the total deadline (~10 s)",
          st == 408 and closed_at is not None and 9.0 <= closed_at <= 12.5,
          f"status {st}, closed at {closed_at:.1f} s" if closed_at else f"status {st}, not closed")


def t_slots(b, in_use):
    if b.plain:
        return
    print("slots")
    want = 4 - in_use
    print(f"  (TLS sessions already in use by others: {in_use}; holding {want})")
    held = []
    for i in range(want):
        try:
            held.append(b.connect())
        except Exception as e:
            check(f"idle TLS connection {i + 1} accepted", False, str(e))
    check(f"{want} idle TLS connections accepted (fills all 4 sessions)", len(held) == want, f"{len(held)}")
    refused = False
    try:
        s5 = b.connect(timeout=5)
        s5.sendall(b"GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n")
        refused = not read_all(s5, 5)
        s5.close()
    except Exception:
        refused = True
    check("fifth connection refused while four are held", refused, "")
    t0 = time.monotonic()
    ok = False
    while time.monotonic() - t0 < 20:
        try:
            if b.status():
                ok = True
                break
        except Exception:
            pass
        time.sleep(1)
    for s in held:
        s.close()
    check("service returns once deadlines reclaim the slots", ok, f"{time.monotonic() - t0:.1f} s")


def t_status(b, n):
    print("status")
    lat, bad = [], 0
    for _ in range(n):
        t0 = time.monotonic()
        try:
            st, _, _ = parse(b.get("/api/status"))
        except Exception:
            st = 0
        lat.append((time.monotonic() - t0) * 1000)
        bad += st != 200
    lat.sort()
    p = lambda q: lat[min(len(lat) - 1, int(q * len(lat)))]
    check(f"{n} status polls all 200", bad == 0,
          f"{bad} failed, p50 {p(0.5):.0f} ms, p95 {p(0.95):.0f} ms, max {lat[-1]:.0f} ms")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.11.7")
    ap.add_argument("--plain", action="store_true", help="port 80 image (camera_stream)")
    ap.add_argument("--quick", action="store_true", help="skip the drip test")
    ap.add_argument("--polls", type=int, default=200)
    a = ap.parse_args()
    b = Board(a.host, 80 if a.plain else 443, a.plain)

    before = b.status()
    if not before:
        print("board not answering /api/status"); return 2
    print(f"board: res {before.get('res')}, stack {before.get('stack')}, tls_conns {before.get('tls_conns')}, "
          f"http {before.get('http')}")
    t_routes(b)
    t_controls(b)
    t_split(b)
    t_limits(b)
    t_policy(b)
    t_pipeline(b)
    t_timeouts(b, a.quick)
    # tls_conns counts the status request's own session too
    t_slots(b, max(0, int(before.get("tls_conns", 1)) - 1))
    t_status(b, a.polls)
    after = b.status() or {}
    print(f"http counters after: {after.get('http')}")
    print(f"sram_used before {before.get('sram_used')} after {after.get('sram_used')}, "
          f"tls_conns {after.get('tls_conns')}, tls_fail {after.get('tls_fail')}")
    fails = [r for r in RESULTS if not r[1]]
    print(f"{'PASS' if not fails else 'FAIL'}: {len(RESULTS) - len(fails)} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
