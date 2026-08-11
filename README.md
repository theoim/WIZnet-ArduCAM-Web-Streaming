# WIZnet ArduCAM Web Streaming

Real-time JPEG video from an **ArduCAM Quick-Bootup 3MP DVP camera** over Ethernet,
on a **WIZnet Pico (RP2040 / RP2350)** board.

Three examples, one camera driver:

| Example | Transport | Viewer | Network stack |
|---|---|---|---|
| [`WIZnet_ArduCAMMega_TOE_Web_Streaming`](example/WIZnet_ArduCAMMega_TOE_Web_Streaming) | HTTP / MJPEG | **Any browser** | Hardwired TCP/IP in the WIZnet chip |
| [`WIZnet_ArduCAMMega_Lwip_Web_Streaming`](example/WIZnet_ArduCAMMega_Lwip_Web_Streaming) | HTTP / MJPEG | **Any browser** | lwIP in software on the MCU |
| [`WIZnet_ArduCAMMega_UDP_Streaming`](example/WIZnet_ArduCAMMega_UDP_Streaming) | Raw UDP | Python + OpenCV | Hardwired UDP sockets |

The two web examples are byte-for-byte the same UI and the same capture path. The only
difference is where TCP is terminated — which makes them a direct, side-by-side
measurement of what a hardwired TCP/IP stack buys you. See
[Comparing the two](#comparing-the-two).

---

## Hardware

### WIZnet Pico (RP2040 / RP2350)

- [WIZnet Pico (RP2040)](https://docs.wiznet.io/Product/Modules/Open-Source-Hardware/rp2040_based)
- [WIZnet Pico (RP2350)](https://docs.wiznet.io/Product/Modules/Open-Source-Hardware/rp2350_based)

| | |
|---|---|
| System clock | 200 MHz |
| Ethernet | W5100S / W5500 / W6100 (SPI 40 MHz), **W6300 (QSPI Quad 37.5 MHz)** |
| Concurrent | DVP camera + Ethernet + SPI flash + UART |

Measurements below were taken on **W6300 in QSPI Quad mode**.

### Arducam Quick-Bootup 3MP DVP Camera for IoT

[Product page](https://www.arducam.com/arducam-quick-bootup-3mp-dvp-camera-for-iot.html)

| Specification | Value |
|---|---|
| Sensor | 3MP Mega DVP colour (2048 × 1536) |
| Lens | 88° FOV, fixed focus, F/2.0 |
| Output | JPEG / YUV / RGB |
| Boot | 300 ms |
| Size | 12.9 × 17 × 5.3 mm |

### Pin mapping

| Pico | ArduCAM | Function |
|---|---|---|
| GP00 | SDA | SCCB (I2C data) |
| GP01 | SCL | SCCB (I2C clock) |
| GP04 | VSYNC | Frame sync |
| GP05–GP12 | D0–D7 | 8-bit pixel data |
| GP13 | PCLK | Pixel clock |
| GP14 | HREF | Line sync |
| VCC / GND | — | 3.3 V / GND |

Each rising PCLK edge, qualified by HREF, samples one byte. VSYNC high marks the
active frame.

---

## Measured performance

W6300 QSPI, 200 MHz, TOE example, one browser client. `read` is the time spent
pulling pixels out of the sensor; `frame` is the JPEG size.

| Resolution | CLK_DIV | fps | read | frame |
|---|---|---|---|---|
| 320 × 240 | 2 | 7 | 59 ms | 9 KB |
| 640 × 480 | 1 | 12.5 | 29 ms | 24 KB |
| **1280 × 720** | **1** | **22 – 29** | **22 ms** | 47 KB |
| 1600 × 1200 | 2 | 3.7 | 122 ms | 100 KB |
| 1920 × 1080 | 2 | 7.5 | 68 ms | 88 KB |

Two things surprise people here, and both are the sensor rather than the MCU:

- **720p is the fast mode.** It is neither the largest nor the smallest frame, but it
  is the one the sensor reads out quickest. Lower resolutions go through the full
  readout plus a downscaler, so they are not faster.
- **1080p beats 1600×1200** despite having more pixels, because 16:9 is a crop of the
  sensor while 4:3 reads the whole array. You can see the field of view narrow when
  you switch to it.

The sensor clock divider is set per resolution in `set_framesize()` — a single shared
value leaves most modes running at half speed, and the fastest value corrupts the
high-resolution modes. The web UI exposes the divider live so it can be re-tuned on
different hardware.

---

## Comparing the two

Flash one board with each web example and open both pages side by side. They use
different addresses so they can share a network:

| | TOE | lwIP |
|---|---|---|
| Address | `192.168.11.3` | `192.168.11.5` |
| Header badge | **TOE**, red | **lwIP**, charcoal |
| Socket mode | `Sn_MR_TCP4` | `Sn_MR_MACRAW` |
| TCP terminated by | The WIZnet chip | The MCU, in lwIP |

Both pages show the same two fixed-scale charts — frame rate (0–30 fps) and link
throughput (0–10 Mbps) — so the lines can be read against each other directly.

The interesting part is what happens during a capture. `arducam_capture_frame()`
blocks for 22–120 ms depending on resolution. With the hardwired stack the chip keeps
acknowledging and buffering throughout; with lwIP the MCU *is* the stack, so for that
whole window nothing is acknowledged, no timer runs, and the receive buffer just
fills. That difference is what the charts make visible.

---

## Build

Requires the Pico SDK (vendored in `libraries/pico-sdk`) and an ARM GCC toolchain.

```bash
git clone --recurse-submodules https://github.com/theoim/WIZnet-ArduCAM-Web-Streaming.git
cd WIZnet-ArduCAM-Web-Streaming
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Artifacts:

```
build/example/WIZnet_ArduCAMMega_TOE_Web_Streaming/toe_web_streaming.uf2
build/example/WIZnet_ArduCAMMega_Lwip_Web_Streaming/lwip_web_streaming.uf2
build/example/WIZnet_ArduCAMMega_UDP_Streaming/main.uf2
```

Hold BOOTSEL, plug in the board, copy the `.uf2` across.

Set the target chip and interface in the top-level `CMakeLists.txt`
(`WIZNET_CHIP`, `_WIZCHIP_QSPI_MODE_`).

---

## Repository layout

```
example/
├─ WIZnet_ArduCAMMega_TOE_Web_Streaming/    HTTP + MJPEG, hardwired TCP/IP
│  ├─ arducam_mega/                          camera driver (shared)
│  ├─ web_page.h                             the UI, embedded
│  └─ logo_png.h                             logo, embedded
├─ WIZnet_ArduCAMMega_Lwip_Web_Streaming/   HTTP + MJPEG, lwIP
└─ WIZnet_ArduCAMMega_UDP_Streaming/        raw UDP, Python viewer
libraries/
├─ ioLibrary_Driver/                         WIZnet chip driver
└─ pico-sdk/
port/
├─ ioLibrary_Driver/                         SPI/QSPI transport
└─ lwip/                                     MACRAW glue + lwipopts.h
```

The lwIP example deliberately compiles the camera driver from the TOE example's
directory rather than keeping its own copy. The two demos have to capture
identically, or the comparison measures the wrong thing.
