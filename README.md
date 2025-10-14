# ArduCAM × WIZnet Pico — High-Speed JPEG Streaming over Ethernet

### Real-time embedded camera streaming using RP2040/RP2350 and ArduCAM DVP

---

## 🧩 Project Overview

This project demonstrates a **real-time Ethernet-based JPEG streaming system** built using  
the **WIZnet Pico (RP2040/RP2350)** board combined with the **ArduCAM Quick-Bootup 3MP DVP Camera**.

### System Flow
- **Camera (Pico):** Captures JPEG frames via 8-bit DVP → splits → sends via UDP.  
- **PC (Python):** Receives packets → reassembles → decodes → displays in real time using OpenCV.

> 🧠 **Core Keywords:**  
> RP2040 DVP PIO DMA / UDP JPEG Streaming / Frame Reassembly / OpenCV Real-Time Decoding

---

## ⚙️ Hardware Modules

### **WIZnet Pico (RP2040 / RP2350)**

[WIZnet Pico(RP2040)](https://docs.wiznet.io/Product/Modules/Open-Source-Hardware/rp2040_based)
[WIZnet Pico(RP2350)](https://docs.wiznet.io/Product/Modules/Open-Source-Hardware/rp2350_based)

- Supports **RP2040** and **RP2350** (up to 200 MHz Sys Clock)  
- Integrated Ethernet chip variants:
  - W5100S / W5500 / W6100 — SPI @ 40 MHz  
  - W6300 — QSPI Quad @ 37.5 MHz  
- Fully compatible with **Pico SDK 1.5.1**  
- Supports concurrent **DVP Camera + Ethernet + SPI Flash + UART**

---

### **Arducam Quick-Bootup 3MP DVP Camera for IoT**

[Arducam Quick-Bootup 3MP DVP Camera for IoT](https://www.arducam.com/arducam-quick-bootup-3mp-dvp-camera-for-iot.html)


| Specification | Description |
|----------------|--------------|
| **Sensor** | 3MP Mega DVP Color (2048×1536) |
| **Lens** | 88° FOV, Fixed Focus, F/2.0 |
| **Output Format** | JPEG / YUV / RGB |
| **Boot Speed** | 300 ms Instant Boot |
| **Power** | Idle Off / 300 ms Wake-up |
| **Size** | 12.9 × 17 × 5.3 mm |
| **Compatible MCUs** | RP2040, Arduino, STM32, ESP32, Renesas, etc. |
| **Max Frame Rate** | 2048×1536 @ 12 fps |

> 💡 *Instant-on (300 ms) and low-power design — ideal for IoT vision projects requiring fast response.*

---

## 🚀 Performance (Sys Clock 200 MHz)

| MCU Module | Ethernet Interface | 1280×720 (HD) | 1920×1080 (FHD) |
|-------------|--------------------|---------------|-----------------|
| W5100S / W5500 / W6100 | SPI 40 MHz | 10 – 17 fps | 2 – 6 fps |
| W6300 | QSPI Quad 37.5 MHz | 22 – 30 fps | 6 – 8 fps |

> ✅ The **W6300 QSPI Pico** delivers smooth HD streaming with stable real-time transfer.

---

## 📡 Pin Mapping (Pico ↔ ArduCAM)

| Pico Pin | ArduCAM Pin | Function |
|-----------|--------------|-----------|
| GP00 | SDA | SCCB (I2C Data) |
| GP01 | SCL | SCCB (I2C Clock) |
| GP04 | VSYNC | Frame Sync |
| GP05–GP12 | D0–D7 | 8-bit Pixel Data |
| GP13 | PCLK | Pixel Clock |
| GP14 | HREF | Line Sync |
| VCC / GND | — | 3.3 V / GND |

> Each **PCLK rising edge** samples one pixel (8 bit).  
> **VSYNC HIGH** defines the active frame duration.

---

## 🎞️ JPEG Capture Sequence (PIO + DMA)
① VSYNC ↑ → Frame Start
② HSYNC ↑ → New Line Start
③ PCLK ↑ → Sample D0–D7
④ DMA stores 32-bit chunks to buffer
⑤ HSYNC ↓ → Line End
⑥ VSYNC ↓ → Frame End

- PIO handles signal timing.  
- DMA transfers 32-bit blocks (512 B each) to RAM.  
- If the JPEG SOI (0xFFD8) isn’t detected in the first 4 lines, capture is retried.

---

## 📤 UDP Streaming Protocol

Because JPEG sizes vary (a few KB ~ tens of KB), frames are split into multiple UDP packets.  
Each packet includes a 4-byte header.

| Byte | Field | Description |
|------|--------|-------------|
| [0] | Frame ID | Frame identifier |
| [1] | Packet ID | Sequence within frame |
| [2] | Total Packets | Total count |
| [3] | End Flag | 0x01 = last packet |
| [4 ~] | JPEG Data | Partial image data |

---

### **Sender (Pico)**

```c
total_packets = (jpeg_size + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;

for (pkt_id = 0; pkt_id < total_packets; pkt_id++) {
    tx_packet[0] = frame_id;
    tx_packet[1] = pkt_id;
    tx_packet[2] = total_packets;
    tx_packet[3] = (pkt_id == total_packets - 1) ? 0x01 : 0x00;

    memcpy(tx_packet + 4, jpeg_data + offset, chunk_size);
    sendto(socket, tx_packet, chunk_size + 4, destip, destport);
}
```
Receiver (Python)

```python
fid, pid, tot = pkt[0], pkt[1], pkt[2]
self.buf.setdefault(fid, {})[pid] = pkt[4:]
if len(self.buf[fid]) == tot:
    data = b"".join(self.buf[fid][i] for i in range(tot))
    return data  # Reconstructed JPEG frame

- The Assembler class reassembles packets by Frame ID and Packet ID.

- OpenCV decodes and displays frames in real time.
```

### 🧠 Key Advantages
Feature	Description
⚡ Ultra-Low Latency	Real-time 1-frame streaming via UDP + PIO + DMA
🧩 Modular Design	Camera, network, and viewer are fully decoupled
💡 Customizable	Adjust JPEG quality, resolution, and frame rate
🧠 Scalable	Ideal for IoT vision, robotics, and inspection systems
🪄 Summary

Real-time JPEG streaming using ArduCAM + WIZnet Pico.
Capture via DVP, stream over UDP, and decode live with Python OpenCV.

### 🧩 Requirements


Pico SDK 1.5.1+

Python 3.9+

OpenCV 4.8+

NumPy, Pillow, Tkinter

WIZnet Ethernet-enabled Pico (W5100S/W5500/W6100/W6300)

### 🔧 Build & Run
Pico Firmware

```bash
cd firmware
mkdir build && cd build
cmake ..
make -j
```
Python Viewer
```bash
cd viewer
python stream_viewer.py
```