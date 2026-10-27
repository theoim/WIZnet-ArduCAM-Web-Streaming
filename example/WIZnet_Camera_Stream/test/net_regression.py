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
  auth        (HTTPS) 401 without a session, 403 for a viewer on admin routes
              and for a POST without the CSRF token, the one-stream rules,
              logout, and the login throttle (slow; skipped with --quick)
Counters from /api/status "http" are printed before and after.

The HTTPS image needs a login. Credentials come from the command line or the
environment, never from this file:
    CAM_ADMIN_USER / CAM_ADMIN_PASS   (--user / --password)
    CAM_VIEWER_USER / CAM_VIEWER_PASS (--vuser / --vpass, optional)
Every request below then carries the admin session cookie and CSRF token,
unless a test asks for an anonymous one.

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
        self.sid = None                       # login cookie value
        self.csrf = None

    def auth_lines(self):
        if not self.sid:
            return b""
        return (b"Cookie: cam_sid=" + self.sid.encode() + b"\r\n" +
                b"X-CSRF-Token: " + self.csrf.encode() + b"\r\n")

    def login(self, user, password):
        """Returns (status, auth document); keeps the cookie on success."""
        body = ("user=%s&pass=%s" % (user, password)).encode()
        st, hdr, data = parse(self.roundtrip(
            b"POST /api/login HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\n"
            b"Content-Length: %d\r\n\r\n" % len(body) + body, anon=True))
        doc = {}
        try:
            doc = json.loads(data)
        except ValueError:
            pass
        if st == 200:
            ck = hdr.get("set-cookie", "")
            self.sid = ck.split("cam_sid=", 1)[1].split(";", 1)[0] if "cam_sid=" in ck else None
            self.csrf = doc.get("csrf")
        return st, doc

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

    def roundtrip(self, raw, timeout=15.0, chunks=None, gap=0.0, anon=False):
        """Send raw bytes (optionally in chunks with a gap), read until close.
        The session lines go in after the request line unless anon, or the
        request brings its own Cookie, or it has no CRLF line end at all."""
        i = raw.find(b"\r\n")
        if not anon and self.sid and i > 0 and b"\r\nCookie:" not in raw:
            raw = raw[:i + 2] + self.auth_lines() + raw[i + 2:]
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

    def get(self, path, extra=b"", anon=False):
        req = b"GET " + path.encode() + b" HTTP/1.1\r\nHost: " + self.host.encode() + b"\r\n" + extra + b"\r\n"
        return self.roundtrip(req, anon=anon)

    def post(self, path, body=b"", extra=b"", anon=False):
        req = (b"POST " + path.encode() + b" HTTP/1.1\r\nHost: " + self.host.encode() + b"\r\n" + extra +
               b"Content-Length: %d\r\n\r\n" % len(body) + body)
        return self.roundtrip(req, anon=anon)

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


def snap(b):
    t0 = time.monotonic()
    st, hdr, body = parse(b.get("/snapshot.jpg"))
    return st, hdr, body, (time.monotonic() - t0) * 1000


def jpeg_ok(st, hdr, body):
    return (st == 200 and hdr.get("content-type") == "image/jpeg"
            and hdr.get("content-length") == str(len(body))
            and body[:2] == b"\xff\xd8" and body[-2:] == b"\xff\xd9")


def t_snapshot(b):
    """Phase 2d. Stopped-stream case stops the stream for a moment and puts it
    back the way it was."""
    print("snapshot (2d)")
    was = bool((b.status() or {}).get("streaming"))
    if was:
        for i in range(3):
            st, hdr, body, ms = snap(b)
            check(f"streaming: snapshot {i + 1} -> 200 JPEG ({len(body)} B, {ms:.0f} ms)",
                  jpeg_ok(st, hdr, body) and ms < 2500, f"got {st} {hdr.get('content-type')}")
        check("streaming flag untouched", (b.status() or {}).get("streaming") is True)
    parse(b.post("/api/stop"))
    try:
        for i in range(2):
            st, hdr, body, ms = snap(b)
            check(f"stopped: snapshot {i + 1} -> 200 JPEG ({len(body)} B, {ms:.0f} ms)",
                  jpeg_ok(st, hdr, body) and ms < 2500, f"got {st} {hdr.get('content-type')}")
        check("stopped: stream stays stopped", (b.status() or {}).get("streaming") is False)
    finally:
        if was:
            parse(b.post("/api/start"))
    st, _, _ = parse(b.roundtrip(b"POST /snapshot.jpg HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"))
    check("POST /snapshot.jpg -> 405", st == 405, f"got {st}")


def t_split(b):
    print("split")
    req = b"GET /api/status HTTP/1.1\r\nHost: 192.168.11.7\r\nUser-Agent: split-test\r\n\r\n"
    st, _, body = parse(b.roundtrip(req, chunks=1, gap=0.01))
    check("1-byte records, 10 ms apart", st == 200 and body.startswith(b"{"), f"got {st}")
    st, _, _ = parse(b.roundtrip(req, chunks=7, gap=0.05))
    check("7-byte records, 50 ms apart", st == 200, f"got {st}")


def t_limits(b):
    print("limits")
    sid = (b"; cam_sid=" + b.sid.encode()) if b.sid else b""
    st, _, _ = parse(b.get("/api/status", b"Cookie: a=" + b"a" * 1150 + sid + b"\r\n"))
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


def timed_status(b):
    """One status poll, timed per phase: TCP connect, TLS handshake (and
    whether the session was resumed), request to end of response."""
    t0 = time.monotonic()
    s = socket.create_connection((b.host, b.port), timeout=15)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    t1 = time.monotonic()
    resumed = None
    if not b.plain:
        s = b.ctx.wrap_socket(s, server_hostname=b.host, session=b.session)
        if s.session is not None:
            b.session = s.session
        resumed = s.session_reused
    t2 = time.monotonic()
    try:
        s.sendall(b"GET /api/status HTTP/1.1\r\nHost: x\r\n" + b.auth_lines() + b"\r\n")
        st = parse(read_all(s, 15))[0]
    finally:
        s.close()
    t3 = time.monotonic()
    return st, (t1 - t0) * 1000, (t2 - t1) * 1000, (t3 - t2) * 1000, resumed


def t_status(b, n):
    print("status")
    lat, bad, rows = [], 0, []
    for i in range(n):
        t0 = time.monotonic()
        try:
            st, c, h, q, res = timed_status(b)
            rows.append((c + h + q, i, c, h, q, res))
        except Exception:
            st = 0
        lat.append((time.monotonic() - t0) * 1000)
        bad += st != 200
        # A SYN that lands within ~0.1 ms of the previous connection's last
        # packet goes unanswered and the PC retries after 1 s. Captured
        # 2026-10-27: back-to-back polls hit it 3 in 300 times, polls 5 ms
        # apart never (also not at 50 ms). A browser polls once a second, so
        # this is a property of the W6300 listener hand-over, not something
        # this test should keep rediscovering.
        time.sleep(0.005)
    # where the time of the slowest polls went (2026-10-26: ~1 s outliers)
    for tot, i, c, h, q, res in sorted(rows, reverse=True)[:3]:
        print(f"  slow poll #{i}: {tot:.0f} ms = connect {c:.0f} + tls {h:.0f}"
              f"{' (resumed)' if res else ' (FULL)' if res is False else ''} + request {q:.0f}")
    lat.sort()
    p = lambda q: lat[min(len(lat) - 1, int(q * len(lat)))]
    check(f"{n} status polls all 200", bad == 0,
          f"{bad} failed, p50 {p(0.5):.0f} ms, p95 {p(0.95):.0f} ms, max {lat[-1]:.0f} ms")


class Stream:
    """An open /stream: request sent, response head read, socket kept."""
    def __init__(self, b, sid):
        self.s = b.connect(timeout=8)
        req = b"GET /stream?r=test HTTP/1.1\r\nHost: x\r\n"
        if sid:
            req += b"Cookie: cam_sid=" + sid.encode() + b"\r\n"
        self.s.sendall(req + b"\r\n")
        self.s.settimeout(5)
        head = b""
        try:
            while b"\r\n\r\n" not in head and len(head) < 4096:
                d = self.s.recv(4096)
                if not d:
                    break
                head += d
        except (socket.timeout, ssl.SSLError, OSError):
            pass
        self.head = head
        try:
            self.status = int(head.split(b"\r\n", 1)[0].split()[1])
        except (IndexError, ValueError):
            self.status = 0

    def drain(self):
        """Read whatever has arrived, without waiting."""
        self.s.settimeout(0.0)
        try:
            while self.s.recv(65536):
                pass
        except (BlockingIOError, socket.timeout, ssl.SSLError, OSError):
            pass

    def closed(self, wait=3.0, also=()):
        """Did the server close it within wait s? Frames already buffered are
        read through first; a live stream keeps sending until the deadline.
        Streams in also are drained meanwhile: one left unread fills its TCP
        window and the server gives it up once it takes nothing for 5 s."""
        end = time.monotonic() + wait
        while time.monotonic() < end:
            for o in also:
                o.drain()
            self.s.settimeout(0.2)
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            except (ssl.SSLError, OSError):
                return True
            if not d:
                return True
        return False

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def t_auth(b, a):
    if b.plain or not b.sid:
        print("auth: skipped (plain image or no login)")
        return
    print("auth (3b)")
    st, _, body = parse(b.get("/api/auth", anon=True))
    doc = json.loads(body) if st == 200 else {}
    check("GET /api/auth anonymous -> provisioned, role none",
          doc.get("provisioned") is True and doc.get("role") == "none" and not doc.get("csrf"), str(doc))
    for path in ("/api/status", "/api/controls", "/snapshot.jpg", "/stream"):
        st, _, _ = parse(b.get(path, anon=True))
        check(f"GET {path} without a session -> 401", st == 401, f"got {st}")
    st, _, _ = parse(b.get("/", anon=True))
    check("GET / without a session -> 200 (page shell is public)", st == 200, f"got {st}")
    st, _, _ = parse(b.get("/api/status", b"Cookie: cam_sid=" + b"0" * 32 + b"\r\n", anon=True))
    check("unknown session cookie -> 401", st == 401, f"got {st}")
    st, _, body = parse(b.post("/api/start", extra=b"Cookie: cam_sid=" + b.sid.encode() + b"\r\n", anon=True))
    check("admin POST without X-CSRF-Token -> 403", st == 403 and b"CSRF" in body, f"got {st} {body[:60]!r}")
    st, _, _ = parse(b.post("/api/start", extra=b"Cookie: cam_sid=" + b.sid.encode() +
                            b"\r\nX-CSRF-Token: " + b"f" * 32 + b"\r\n", anon=True))
    check("admin POST with a wrong CSRF token -> 403", st == 403, f"got {st}")
    st, _, _ = parse(b.post("/api/start"))
    check("admin POST with the token -> 200", st == 200, f"got {st}")
    st, _, _ = parse(b.post("/api/setup", b"user=x&pass=xxxxxxxxxx"))
    check("POST /api/setup once set up -> 409", st == 409, f"got {st}")
    st, _, _ = parse(b.get("/api/start"))
    check("GET /api/start -> 405 (state changes are POST)", st == 405, f"got {st}")

    v = None
    if a.vuser and a.vpass:
        v = Board(b.host, b.port, b.plain)
        st, doc = v.login(a.vuser, a.vpass)
        check("viewer login -> 200, role viewer", st == 200 and doc.get("role") == "viewer", f"got {st}")
        st, _, _ = parse(v.get("/api/status"))
        check("viewer GET /api/status -> 200", st == 200, f"got {st}")
        st, _, _ = parse(v.get("/snapshot.jpg"))
        check("viewer GET /snapshot.jpg -> 200", st == 200, f"got {st}")
        st, _, body = parse(v.post("/api/cam", b"brightness=0",
                                   b"Content-Type: application/x-www-form-urlencoded\r\n"))
        check("viewer POST /api/cam -> 403 admin only", st == 403 and b"admin" in body, f"got {st}")
        for path in ("/api/stop", "/api/settings/save", "/api/account"):
            st, _, _ = parse(v.post(path))
            check(f"viewer POST {path} -> 403", st == 403, f"got {st}")
        st, _, _ = parse(v.get("/load?kb=1"))
        check("viewer GET diagnostic /load -> 403", st == 403, f"got {st}")
        st, _, _ = parse(b.post("/api/stop", extra=b"Cookie: cam_sid=" + b.sid.encode() +
                                b"\r\nX-CSRF-Token: " + v.csrf.encode() + b"\r\n", anon=True))
        check("admin cookie with the viewer's CSRF token -> 403", st == 403, f"got {st}")

        # one stream at a time (Q4)
        sv = Stream(b, v.sid)
        check("viewer opens the stream -> 200", sv.status == 200, f"got {sv.status}")
        sv2 = Stream(b, v.sid)
        check("same viewer session reconnects -> 200, replaces its own",
              sv2.status == 200 and sv.closed(3, also=[sv2]), f"got {sv2.status}")
        sv.close()
        sa = Stream(b, b.sid)
        check("admin opens while the viewer watches -> 200, viewer cut",
              sa.status == 200 and sv2.closed(3, also=[sa]), f"got {sa.status}")
        sv2.close()
        sv3 = Stream(b, v.sid)
        check("viewer opens while the admin watches -> 409, admin keeps it",
              sv3.status == 409 and not sa.closed(3), f"got {sv3.status}")
        sv3.close()
        sa.close()
        time.sleep(1)

        st, _, _ = parse(v.post("/api/logout"))
        check("viewer logout -> 200", st == 200, f"got {st}")
        st, _, _ = parse(v.get("/api/status"))
        check("after logout the cookie is dead -> 401", st == 401, f"got {st}")
    else:
        print("  (no viewer credentials: viewer and stream-rule checks skipped)")

    if a.quick:
        return
    print("  throttle: 5 wrong passwords, then locked for 30 s")
    t = Board(b.host, b.port, b.plain)
    codes = [t.login(a.user, "wrong-password-%d" % i)[0] for i in range(5)]
    check("5 wrong passwords -> 401 each", codes == [401] * 5, str(codes))
    st, _ = t.login(a.user, a.password)
    check("6th try, even the right password -> 429", st == 429, f"got {st}")
    time.sleep(31)
    st, _ = t.login(a.user, a.password)
    check("after 30 s the right password works", st == 200, f"got {st}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.11.7")
    ap.add_argument("--plain", action="store_true", help="port 80 image (camera_stream)")
    ap.add_argument("--quick", action="store_true", help="skip the drip test")
    ap.add_argument("--polls", type=int, default=200)
    ap.add_argument("--user", default=os.environ.get("CAM_ADMIN_USER"))
    ap.add_argument("--password", default=os.environ.get("CAM_ADMIN_PASS"))
    ap.add_argument("--vuser", default=os.environ.get("CAM_VIEWER_USER"))
    ap.add_argument("--vpass", default=os.environ.get("CAM_VIEWER_PASS"))
    a = ap.parse_args()
    b = Board(a.host, 80 if a.plain else 443, a.plain)
    if not a.plain and a.user and a.password:
        st, doc = b.login(a.user, a.password)
        if st != 200:
            print(f"admin login failed: {st} {doc}"); return 2
        print(f"logged in as {doc.get('user')} ({doc.get('role')})")

    before = b.status()
    if not before:
        print("board not answering /api/status"); return 2
    print(f"board: res {before.get('res')}, stack {before.get('stack')}, tls_conns {before.get('tls_conns')}, "
          f"http {before.get('http')}")
    t_routes(b)
    t_controls(b)
    t_snapshot(b)
    t_split(b)
    t_limits(b)
    t_policy(b)
    t_pipeline(b)
    t_timeouts(b, a.quick)
    # tls_conns counts the status request's own session too
    t_slots(b, max(0, int(before.get("tls_conns", 1)) - 1))
    t_status(b, a.polls)
    t_auth(b, a)
    after = b.status() or {}
    print(f"http counters after: {after.get('http')}")
    print(f"sram_used before {before.get('sram_used')} after {after.get('sram_used')}, "
          f"tls_conns {after.get('tls_conns')}, tls_fail {after.get('tls_fail')}")
    fails = [r for r in RESULTS if not r[1]]
    print(f"{'PASS' if not fails else 'FAIL'}: {len(RESULTS) - len(fails)} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
