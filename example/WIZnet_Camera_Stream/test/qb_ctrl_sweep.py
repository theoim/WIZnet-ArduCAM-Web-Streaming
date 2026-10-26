#!/usr/bin/env python3
"""
Phase 2b range sweep for the Arducam Quick-Bootup controller (LH module).

Needs the diagnostic image (CAM_QB_DIAG=ON, GET /api/diag/reg). For each
value of each register: write it, watch capture for a few seconds through
/api/status (cap_frames must keep rising, dropped must not), then write the
value it read before back. It does not judge the picture - only whether a value
stops or corrupts capture.

    python qb_ctrl_sweep.py [--host 192.168.11.7] [--hold 3]
"""
import argparse
import json
import os
import ssl
import time
import urllib.request

# The module keeps at least 0x012A across a full power cycle (2026-10-26), so
# every register is put back to the value it read before the sweep - that is
# the value left stored in the module.
SWEEP = [
    # register, name, values to try
    (0x0126, "awb",        range(0, 5)),
    (0x0124, "saturation", range(0, 7)),
    (0x0123, "contrast",   range(0, 7)),
    (0x0122, "brightness", range(0, 9)),
]

# A write to 0x012A drops ~10 frames at once and then none (2026-10-26), so
# the burst right after a write and the steady state are measured apart.
SETTLE_S = 2.0


def make_ctx():
    ca = os.path.expanduser("~/.camera-stream-ca/ca.crt")
    if os.path.exists(ca):
        ctx = ssl.create_default_context(cafile=ca)
        ctx.check_hostname = False          # cert SAN is the IP; keep it simple
        return ctx
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


class Board:
    def __init__(self, host):
        self.base = f"https://{host}"
        self.ctx = make_ctx()

    def get(self, path):
        with urllib.request.urlopen(self.base + path, context=self.ctx, timeout=10) as r:
            return json.loads(r.read().decode())

    def reg(self, a, v=None):
        q = f"/api/diag/reg?a=0x{a:04X}" + ("" if v is None else f"&v={v}")
        return self.get(q)

    def status(self):
        return self.get("/api/status")


def watch(b, hold):
    s0 = b.status()
    t0 = time.monotonic()
    time.sleep(hold)
    s1 = b.status()
    dt = time.monotonic() - t0
    cap = s1["cap_frames"] - s0["cap_frames"]
    return cap / dt, s1["dropped"] - s0["dropped"], s1.get("kb")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.11.7")
    ap.add_argument("--hold", type=float, default=3.0)
    args = ap.parse_args()
    b = Board(args.host)

    q = b.reg(0x012A)
    print(f"start: 0x012A reads {q['value']} (0x5F = fresh boot)")
    base_fps, base_drop, base_kb = watch(b, args.hold)
    print(f"baseline: capture {base_fps:.1f} fps, dropped +{base_drop}, {base_kb} KB")
    floor = base_fps * 0.5

    bad = []
    for reg, name, values in SWEEP:
        orig = int(b.reg(reg)["value"], 16)
        print(f"\n== {name} (0x{reg:04X}), reads 0x{orig:02X} before")
        for v in values:
            d0 = b.status()["dropped"]
            w = b.reg(reg, v)
            time.sleep(SETTLE_S)
            burst = b.status()["dropped"] - d0
            fps, dd, kb = watch(b, args.hold)
            ok = fps >= floor and dd == 0
            print(f"  v={v}: read_back {w['read_back']}  burst +{burst}  then capture"
                  f" {fps:5.1f} fps  dropped +{dd}  {kb} KB  {'ok' if ok else 'PROBLEM'}")
            if not ok:
                bad.append((name, v, fps, dd))
                time.sleep(6)               # let core 1's auto-recovery finish
        b.reg(reg, orig)
        time.sleep(SETTLE_S)
        print(f"  restored: reads {b.reg(reg)['value']}")

    fps, dd, kb = watch(b, args.hold)
    print(f"\nend: capture {fps:.1f} fps, dropped +{dd}, {kb} KB")
    print("problems:", bad if bad else "none")


if __name__ == "__main__":
    main()
