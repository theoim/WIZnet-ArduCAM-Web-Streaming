# ArduCAM MEGA — Web streaming over hardwired TCP/IP (TOE)

JPEG video from the camera straight to a browser. The WIZnet chip terminates TCP in
hardware; the MCU only reads and writes socket buffers.

No PC-side application. Open the address and the video is there.

```
http://192.168.11.3/
```

---

## What it serves

| Endpoint | Purpose |
|---|---|
| `GET /` | The control page (embedded in `web_page.h`) |
| `GET /stream` | `multipart/x-mixed-replace` MJPEG stream |
| `GET /logo.png` | Logo, cached by the browser |
| `GET /api/start` · `/api/stop` | Start / stop capture |
| `GET /api/res?v=1280x720` | Change resolution |
| `GET /api/clk?div=1&pll=1` | Change the sensor clock dividers live |
| `GET /api/reset` | Reset and reinitialise the sensor |
| `GET /api/status` | Everything above as JSON |

MJPEG is what makes the browser-only viewer possible: an `<img src='/stream'>` is all
the client needs, and every browser has decoded that format for two decades.

Four sockets listen on port 80. One of them is promoted to carry the stream; a later
`/stream` request takes the camera over rather than being refused, because the page
reopens the stream whenever the frame size changes and the previous connection is
often still half-open at that moment.

---

## The page

- **Live view** — the MJPEG stream
- **Control** — start/stop, resolution, frame and drop counters
- **Live performance** — two fixed-scale charts, frame rate (0–30 fps) and link
  throughput (0–10 Mbps), 60 s of history
- **Sensor clock** — the two divider registers, adjustable while streaming

The axes are fixed rather than auto-scaling on purpose. The point of the charts is to
be read against the lwIP board running next to it, and an axis that rescales itself
per board would defeat that.

Below the charts, the frame period is broken into **VSYNC wait**, **sensor readout**
and **network send**, with the largest called out. A frame rate on its own does not
say what to fix.

---

## Tuning the sensor clock

`SYSTEM_CLK_DIV` scales readout time directly — halving it halves the time — but the
faster pixel clock corrupts some modes. Measured on this hardware:

| Resolution | Divider | Why |
|---|---|---|
| 320 × 240 | 2 | 1 halves readout but tears the picture |
| 640 × 480 | 1 | Clean at 1 |
| 1280 × 720 | 1 | 22 ms against 44 ms at 2, and clean |
| 1600 × 1200 | 2 | 1 loses bytes from the start of the frame |
| 1920 × 1080 | 2 | Same |

The table lives in `clk_div_for_res()` in `arducam_mega.c`. Different camera modules
may want different values, which is why the sliders are on the page: sweep them while
watching the chart, then fold the result into the firmware.

A divider of **0 stops the sensor clock**, and writing a valid value afterwards does
not restart it — the sensor has to be reset. The firmware rejects 0, and the sliders
start at 1.

---

## Capture notes

`arducam_capture_frame()` is where the frame rate is won or lost. Things that are not
obvious from the code:

- **The DMA is armed before VSYNC rises**, during blanking. Arming after the frame
  starts loses the first bytes — including the JPEG SOI marker — and sends the capture
  into a retry.
- **Lines land in `image_buff` directly.** Copying each line separately doubled the
  per-line CPU budget, and at 720p the 32-byte PIO FIFO does not cover that: bytes go
  missing and the picture tears in bands.
- **Capture stops at the JPEG EOI marker.** The sensor keeps driving data for the
  whole active frame period, long after the image ends. Reading to VSYNC instead
  overran the buffer at 1600 × 1200 and above and threw every frame away.
- **Finding EOI needs the marker structure, not a byte scan.** Byte stuffing only
  applies after SOS, so a quantisation table can legitimately contain `FF D9`.
- **A frame without an EOI is never sent.** A truncated JPEG makes the browser drop
  the frame, and the view blinks — worse than skipping it.

---

## Build

```bash
cmake --build build --target toe_web_streaming
```

Output: `build/example/WIZnet_ArduCAMMega_TOE_Web_Streaming/toe_web_streaming.uf2`

Address, MAC and stack label are at the top of `main.c`.
