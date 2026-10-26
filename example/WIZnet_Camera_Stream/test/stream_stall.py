#!/usr/bin/env python3
"""
How much does a stream client that cannot keep up cost everyone else?

Core 0 serves the sockets one after another. While it sends a frame to the
stream socket, a status poll waits. This times status polls in three cases:

  reading  - the stream client reads everything (the normal case)
  slow     - it reads ~32 KB/s, slower than the frames arrive (a weak Wi-Fi viewer)
  stalled  - it reads nothing after the response head (a frozen tab)

and reports each poll's total time plus the board's own "slow" field (the
longest single socket service since the previous status). At the end it says
whether the board kept or cut the slow and stalled streams.

usage: stream_stall.py [--host 192.168.11.7] [--polls 40] [--plain]
Credentials come from the environment, never from this file:
  CAM_ADMIN_USER / CAM_ADMIN_PASS
"""
import argparse, json, os, socket, ssl, sys, threading, time, importlib.util

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "net_regression.py")
spec = importlib.util.spec_from_file_location("nr", here)
nr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nr)


class StreamClient:
    """GET /stream with a small receive window, read at a chosen rate."""
    def __init__(self, b, rate):
        self.rate, self.got, self.dead, self.stop = rate, 0, None, False
        raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        raw.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)   # before connect
        raw.settimeout(8)
        raw.connect((b.host, b.port))
        raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s = raw if b.plain else b.ctx.wrap_socket(raw, server_hostname=b.host, session=b.session)
        req = b"GET /stream?r=stall HTTP/1.1\r\nHost: x\r\n"
        if b.sid:
            req += b"Cookie: cam_sid=" + b.sid.encode() + b"\r\n"
        self.s.sendall(req + b"\r\n")
        head = b""
        while b"\r\n\r\n" not in head and len(head) < 4096:
            d = self.s.recv(4096)
            if not d:
                break
            head += d
        self.status = int(head.split(b"\r\n", 1)[0].split()[1]) if head else 0
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        self.s.settimeout(0.5)
        while not self.stop:
            if self.rate == 0:            # stalled: never read
                time.sleep(0.1)
                continue
            try:
                d = self.s.recv(65536 if self.rate < 0 else 8192)
                if not d:
                    self.dead = "closed by board"
                    return
                self.got += len(d)
            except socket.timeout:
                continue
            except (ssl.SSLError, OSError) as e:
                self.dead = "error " + type(e).__name__
                return
            if self.rate > 0:
                time.sleep(8192 / self.rate)

    def alive(self):
        """Still open? For a stalled client, peek without consuming much."""
        if self.dead:
            return False
        if self.rate != 0:
            return True
        # A stalled stream the board gave up on ends in FIN/RST behind the
        # buffered frames; read through them to find out.
        self.s.settimeout(0.3)
        try:
            end = time.monotonic() + 3
            while time.monotonic() < end:
                d = self.s.recv(65536)
                if not d:
                    return False
        except socket.timeout:
            return True
        except (ssl.SSLError, OSError):
            return False
        return True

    def close(self):
        self.stop = True
        try:
            self.s.close()
        except OSError:
            pass


def poll(b):
    """One status poll: total ms, and the board's slow.ms from the reply."""
    t0 = time.monotonic()
    try:
        st, hdr, body = nr.parse(b.get("/api/status"))
        ms = (time.monotonic() - t0) * 1000
        doc = json.loads(body) if st == 200 else {}
        return ms, doc.get("slow", {}).get("ms"), st
    except Exception:
        return (time.monotonic() - t0) * 1000, None, 0


def run_case(b, name, rate, polls, gap):
    sc = StreamClient(b, rate)
    if sc.status != 200:
        print(f"{name:8s}: stream answered {sc.status}, skipped")
        sc.close()
        return
    time.sleep(1.0)                       # let the window fill
    rows = [poll(b) for _ in range(polls) if time.sleep(gap) is None]
    alive = sc.alive()
    sc.close()
    lat = sorted(r[0] for r in rows)
    slow = [r[1] for r in rows if r[1] is not None]
    bad = sum(r[2] != 200 for r in rows)
    p = lambda q: lat[min(len(lat) - 1, int(q * len(lat)))]
    print(f"{name:8s}: poll p50 {p(0.5):5.0f}  p95 {p(0.95):5.0f}  max {lat[-1]:5.0f} ms"
          f" | board slow max {max(slow) if slow else '-':>5} ms"
          f" | failed {bad} | stream {'kept' if alive else 'cut'} ({sc.got // 1024} KB read)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.11.7")
    ap.add_argument("--polls", type=int, default=40)
    ap.add_argument("--gap", type=float, default=0.2)
    ap.add_argument("--plain", action="store_true")
    a = ap.parse_args()
    b = nr.Board(a.host, 80 if a.plain else 443, a.plain)
    user, pw = os.environ.get("CAM_ADMIN_USER"), os.environ.get("CAM_ADMIN_PASS")
    if user and pw:
        st, _ = b.login(user, pw)
        if st != 200:
            sys.exit(f"login failed: {st}")
    st, _, body = nr.parse(b.post("/api/start"))
    doc = b.status() or {}
    print(f"build {doc.get('build', '?')}; start: {st}; polls {a.polls} every {a.gap}s per case")
    run_case(b, "reading", -1, a.polls, a.gap)
    run_case(b, "slow", 32 * 1024, a.polls, a.gap)
    run_case(b, "stalled", 0, a.polls, a.gap)


if __name__ == "__main__":
    main()
