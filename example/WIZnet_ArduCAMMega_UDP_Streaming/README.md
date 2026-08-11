# ArduCAM MEGA — JPEG streaming over UDP

The original example: JPEG frames pushed over UDP to a Python receiver that
reassembles and displays them with OpenCV.

For a viewer that needs nothing installed, use the
[TOE web example](../WIZnet_ArduCAMMega_TOE_Web_Streaming) instead — it serves the
video to a browser. This one is still the right choice when you want the raw frames
in a Python process for recording or processing.

---

## Protocol

JPEG sizes vary from a few KB to over a hundred, so each frame is split across
several UDP packets with a 4-byte header:

| Byte | Field | Meaning |
|---|---|---|
| 0 | Frame ID | Which frame this belongs to |
| 1 | Packet ID | Sequence within the frame |
| 2 | Total packets | How many to expect |
| 3 | End flag | `0x01` on the last packet |
| 4… | Payload | Slice of the JPEG |

The receiver buckets packets by frame ID and concatenates once it has them all.

UDP does not retransmit, so a lost packet loses the frame. That is usually the right
trade for live video — a dropped frame is better than a late one — but it does mean
the viewer must tolerate gaps.

---

## Commands

Sent as UDP payloads to the same port:

| Command | Effect |
|---|---|
| `START` | Begin streaming |
| `STOP` | Stop streaming |
| `RES_1280X720` | Change resolution (`RES_320X240`, `RES_640X480`, `RES_1280X720`, `RES_1600X1200`, `RES_1920X1080`) |

---

## Run

**Board**

```bash
cmake --build build --target main
```

Output: `build/example/WIZnet_ArduCAMMega_UDP_Streaming/main.uf2`

Addresses are at the top of `main.c`: the board is `192.168.11.3`, and frames are sent
to `192.168.11.4:5000`. Set that to the PC running the viewer.

**Viewer**

```bash
python jpeg_stream_gui.py
```

Needs Python 3.9+, OpenCV 4.8+, NumPy, Pillow, Tkinter.

---

## Note on capture

This example predates the capture work done for the web examples and keeps its own
copy of `arducam_mega.c`. The web examples' driver stops at the JPEG EOI marker, DMAs
lines straight into the frame buffer, and picks the sensor clock per resolution —
which is worth porting across if you extend this one.
