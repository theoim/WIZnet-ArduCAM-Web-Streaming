# OV2640 web streaming — exhibition build

Two firmware images from one source tree, differing by a single `-D`, so that a
difference on screen came from the network stack and not from the camera, the
page or the measurement.

```
ov2640_toe     NET_STACK_TOE    TCP/IP terminated in the W6300
ov2640_lwip    NET_STACK_LWIP   TCP/IP in software on the RP2350
```

Board: W6300-EVB-Pico2, same base board as
`example/WIZnet_ArduCAMMega_Exhibition`, with an OV2640 module in place of the
ArduCAM Mega CCM.

## The measurement

800x600 JPEG, same sensor settings, same page, boards side by side:

| | TOE | lwIP |
|---|---:|---:|
| frame rate | **17 fps** | **8 fps** |
| frame size | 22 KB | 22 KB |
| sensor readout | 71 ms | 71 ms |
| dropped | 0 in 1075 | — |

The sensor does identical work in both. What differs is what happens after the
frame exists.

---

## What this port actually was

Less than it looked like. The exhibition driver is named `arducam_mega` and that
name is why this was expected to be a transport rewrite - an ArduCAM Mega is an
SPI device. It is not what the file does. `image_mega.pio` reads HREF, PCLK and
eight data lines: a DVP parallel capture path, with a JPEG marker walk already
in it. An OV2640 is also DVP. **The capture path was kept, unchanged.**

What was replaced is the control layer:

| | ArduCAM Mega CCM | OV2640 |
|---|---|---|
| SCCB address | `0x1F` | `0x30` (7-bit) |
| register width | 16-bit | 8-bit, two banks |
| configuration | one register per setting | register tables |
| XCLK | module oscillator | supplied by the RP2350 |
| ceiling | 1920x1080 | 1600x1200 |

`0x1F` with sixteen-bit registers is a controller sitting in front of a sensor,
answering things like "resolution number 3". The OV2640 is the bare part.

---

## Wiring

The inherited DVP wiring maps one-to-one onto the OV2640's twenty-pin
connector. Nothing was rerouted.

| GPIO | OV2640 pin | Signal |
|---|---|---|
| 0 | 4 | SIOD (I2C0 SDA) |
| 1 | 3 | SIOC (I2C0 SCL) |
| 4 | 5 | VSYNC |
| 5–12 | 16–9 | D0–D7 (DOUT2–DOUT9; 10-bit mode's DOUT0/1 unused) |
| 13 | 7 | PCLK |
| 14 | 6 | HREF |
| 27 | 20 | PWDN |
| **26** (header 31) | **8** | **XCLK — soldered by hand** |
| 15–22 | — | W6300 QSPI, untouched |

### XCLK is not on the board

Connector pin 8 is left unconnected, so it needs a wire to a GPIO. GP26 was
chosen because header 31/32/33/34 are GP26, GP27 (already the camera's PWDN),
GND and GP28 - the camera's control signals end up in one corner with a ground
between them. GP2/GP3 were left free because they are the only consecutive pair
on this board, which a PIO side-set needs.

GP26 was verified unused: the only reference to it anywhere in the tree was a
dead `#define SPI_SCLK_PIN 26` that no `.c` file read, left from the SPI
ArduCAM era. It has been deleted - two names for one pin is how the next change
collides with this one silently.

Driving a 20 MHz square wave down a flying wire:

- keep it under 10 cm, and do not run it alongside the D0–D7/PCLK bundle
- twist it with a ground wire; header 33 is GND and sits next to GP26
- a 22–47 Ω series resistor at the GPIO is worth fitting from the start

The pad, not the PWM, is the limit here. At the default 4 mA into the
capacitance of a flying wire the edge takes about as long as the half period,
so the driver raises the pin to 8 mA with the fast slew rate. If a scope shows
a rounded triangle rather than a square, drop `XCLK_HZ` to 10 MHz - the sensor
runs from 6 MHz up and bring-up continues, just slower.

### RESET is optional

`PIN_CAM_RESETB` is `-1` and nothing is wired to it. The init sequence resets
through COM7 (`0x12` bit 7) instead, which is what the reference drivers do and
is enough. Set it to a pin number if one is ever soldered; the code will use it
and fall back to the register when it is not there.

The register path depends on SCCB, and SCCB depends on XCLK. It is not a way
out of a dead clock.

---

## Frame rate: where it comes from

This took most of the bring-up and almost none of it was where it was expected.

### The array mode is the whole story

Every resolution table that ships with the reference sets COM7 to UXGA. The
array therefore reads 1600x1200 for every frame and the DSP scaler produces the
requested size afterwards. **The output size never touches the thing that takes
the time**, which is why 800x600, 1024x768, 1280x1024 and 1600x1200 all ran at
the same 5.9 fps.

The small tables do not work that way. 320x240 sets COM7 to `0x40`, the SVGA
array mode, which reads 800x600 - a quarter of the pixels.

SVGA mode reads 800x600 *natively*, which is the size this exhibit wants. So
`OV2640_800x600_SVGA_JPEG` in `ov2640.c` is the 320x240 table with the scaler
taken out of the path:

| | from | to |
|---|---|---|
| `0x5a`/`0x5b` output | `0x50`/`0x3c` (320x240) | `0xc8`/`0x96` (800x600) |
| `0x50` CTRLI | `0x89` (divide by two) | `0x80` (no division) |

The CTRLI value is not a guess: esp32-camera's `ov2640_settings_to_svga[]` ends
in `{CTRLI, CTRLI_LP_DP | 0x00}`, which is `0x80`, and keeps `0x00` for UXGA.
A `/2` divider with a 1:1 output is a contradiction the DSP does not resolve -
it stops emitting VSYNC, which downstream reads as a dead sensor.

Result: **5.9 fps → 17 fps at the same resolution and the same clock.**

### A setting that is not a setting

The first version of that table gave 11.4 fps from reset but 17 fps after
switching to 640x480 and back, with nothing else changed.

It omitted two registers the UXGA tables write: `COM1` (`0x03`) and `0x3D`.
Neither the 320x240 table nor `JPEG_INIT` writes `COM1` at all, so its value was
whatever the last table to run had left there. Visiting 640x480 was quietly
configuring the SVGA mode on the way past.

Both are pinned now. A configuration that only appears after visiting a
different one is not a configuration.

(esp32-camera uses the other pair for SVGA - `COM1 0x0A` with `0x3D 0x38`,
reserving `0x0F`/`0x34` for UXGA. Measured on this module its SVGA pair is the
slower one. If the picture ever comes out short or cropped rather than merely
slower, that is the first thing to put back.)

### The clock dividers are at their limit

Measured at 800x600 by writing the registers from the browser:

| CLKRC | R_DVP_SP | result |
|---:|---:|---|
| 1 | 2 | 154 ms, 5.9 fps. Stable. 7 dropped in 1465 (0.5%) |
| 0 | 2 | 77 ms, 11.9 fps. 2 dropped in 188 (1.1%), **and the picture goes black on its own after a few minutes** |
| 0 | 1 | no frames at all - every capture fails the SOI check and the recovery path resets the sensor in a loop |

Doubling the clock is outside what the capture path can follow. The failure is
not gradual: the rejected 1.1% is the visible part, and what is not visible is
the fraction that passes every check the code makes and is still corrupt. One
of those reaching the browser ends the MJPEG stream - Chrome stops updating the
`<img>` at that frame and never resumes, while the connection stays open and
the board goes on sending at full rate.

So the shipped build leaves the tables' own dividers alone. The remaining
headroom is not in the dividers: deferring the JPEG marker walk until after the
frame would cut the per-chunk work in the capture loop, which is what runs out
of time when PCLK doubles. That is the experiment worth doing before touching
CLKRC again.

### Resolution is fixed at 800x600

The show UI has no controls at all - no resolution bar, no load buttons. On
this sensor every resolution change rewrites around forty registers and can
switch the array between its SVGA and UXGA modes, and that is where a day of
faults came from: recovery loops, the frame rate depending on what had been
selected before it, and at 1600x1200 a board that stopped answering the page.

There is also nothing to choose:

| mode | array | fps | frame |
|---|---|---:|---:|
| 320x240 | SVGA | 17.1 | 4 KB |
| **800x600** | **SVGA** | **17** | **22 KB** |
| 640x480 | UXGA, PCLK/4 | 5.1 | 14 KB |
| 1024x768 / 1280x1024 | UXGA | ~6 | 30–40 KB |
| 1600x1200 | UXGA | 0.5, page stops answering | 58 KB |

800x600 is the fastest mode at a size worth projecting, the only one read
natively, and it ran a thousand frames without dropping one. Everything below
it is smaller as well as slower, which puts *less* on the link - the wrong
direction for an exhibit about what a stack does under load.

The full set is still there for anyone who wants it. Build with
`-DEXHIBITION_SIMPLE_UI=0` and the dropdown, the load generator and the sensor
controls all come back; they are hidden with CSS, not removed.

---

## Traps

Things that cost real time here, recorded so they cost less next time.

**SCCB reads are not I2C reads.** A standard I2C read issues a REPEATED START
between writing the register index and reading. SCCB does not: the index write
is a complete transaction ending in STOP, and the read is a second one. That is
the final `false` (the SDK's `nostop` flag) in both calls in `ov2640_reg_read`.
Changing either to `true` makes the code look more like textbook I2C and stops
the sensor answering.

**`0xff` means two things.** It is the bank-select register, which every table
writes several times, and it is also half of the `{0xff, 0xff}` end marker.
Both bytes have to match or the terminator gets written into the bank select.

**fps and read_ms are windowed averages.** Reading them immediately after
writing a register reports the previous setting. Three separate clock
combinations were recorded as "no effect" that way, including the one that
turned out to be the answer. Let a change run a few hundred frames before
believing the number.

**PWDN was never being driven.** The inherited code called `gpio_put()` on the
pin without claiming it or setting a direction, so it stayed an input and the
write went nowhere. Survivable on the Mega; here it decides whether the part
answers at all.

**CLKRC = 0 is a legal value.** The Mega driver rejected a zero divider because
zero stopped its clock. On an OV2640 it means divide by one and the stock QVGA
table writes exactly that, so the check was refusing a setting the sensor ships
with. Only `R_DVP_SP = 0` is rejected now.

**An identical stream URL breaks reloads.** `seq` started at zero, so the first
stream URL of every page load was `/stream?1` and a reload asked for a resource
the browser had been streaming under that exact name. Chrome does not treat
`multipart/x-mixed-replace` like an ordinary response and `Cache-Control:
no-store` did not change its mind: reloading gave a live picture or a black one,
alternating. `seq` starts from `Date.now()` now.

**Assigning `img.src` fires `error` on the element.** It aborts whatever load
was running, and the abort arrives as an error event - so an `onerror` handler
that restarts the stream restarts it again immediately. Two opens a millisecond
apart on every page load, and on the lwIP image, where a stream can fail as it
opens, a restart storm that did more damage than the fault. The handler now
ignores anything within 1.5 s of a restart it asked for, and rate-limits the
rest.

**Four listeners were one short of a reload.** The budget was stream + status +
load + spare, but a reload asks for the page, the logo, the stream and a status
poll at once while the abandoned stream connection is still held. Raised to
eight, which the W6300 has and this statically-addressed build was not using.
That is slack, not a fix: the cause is that capturing and serving share one
loop.

---

## Layout

```
main.c            clock, camera, hand over to the server, loop
ov2640.c/.h       the driver: SCCB, XCLK, tables, DVP capture
ov2640_defs.h     types, bank and register names
ov2640_regs.c/.h  register tables, taken from a working build
image_mega.pio    DVP capture program - HREF, PCLK, 8 data lines
cam_state.c       capture state, the one-second window, status JSON
cam_controls.c    sensor control table (not yet mapped to OV2640)
server_toe.c      hardware TCP/IP
server_lwip.c     software TCP/IP
web_page.h        the page
```

The driver is this example's own copy rather than a shared compile. The Mega
exhibition pulls `arducam_mega.c` out of the TOE streaming example so both
boards capture with identical object code, which is right when the only
variable is meant to be the network stack. Here the sensor itself is the
change, so sharing would mean editing the Mega's driver to suit a part it does
not have.

`ov2640_regs.c` is verbatim from
[theoim/ArduCAM_RP2040_C](https://github.com/theoim/ArduCAM_RP2040_C) -
`W55RP20_ArduCAM_Example/ArduCAM/ov2640_regs.c`. That project drives an ArduCAM
Mini over SPI with an ArduChip and a FIFO; none of that transport applies here
and none of it was copied. What was copied is the part that is about the
OV2640 itself, which is identical whether the bytes leave over SPI or over DVP.
`OV2640_800x600_SVGA_JPEG` lives in `ov2640.c` instead, because it is ours and
nobody else has shipped it.

## Build

```
cmake --build build --target ov2640_toe ov2640_lwip
```

Produces `ov2640_toe.uf2` and `ov2640_lwip.uf2` under
`build/example/WIZnet_OV2640_Exhibition/`.

Addresses and the stack choice are in `exhibition_config.h`.

## Known issues

**Capturing blocks the socket loop.** `net_server_service()` services sockets
and then calls `get_frame()`, which takes 71 ms at 800x600 and does not return
until the frame is out of the sensor. For TOE this costs latency on the other
connections. For lwIP it costs correctness: incoming ACKs are not processed and
`sys_check_timeouts()` does not run for the whole window, which is most of why
the lwIP image looks as bad as it does. The water tank firmware in this
repository already runs the camera on core 1 for exactly this reason, and that
is the repair.

**lwIP clobbers its own responses.** With logging on, `[resp] CLOBBER idx=1
off=11554 remaining` appears while the 25 KB page is still draining and a status
poll arrives. The response state is global rather than per-connection, so a
second request truncates the first. `server_toe.c` keeps it per socket; the lwIP
side needs the same. Until then the lwIP numbers include a server bug and are
not purely a stack comparison.

**The sensor control panel is not mapped.** Every register in
`cam_controls.c` is a Mega CCM controller address (`0x0100 | 0xNN`), none of
which exist on an OV2640. The write path refuses and logs rather than putting
arbitrary values into whatever eight-bit register shares the low byte. The
panel is off by default (`EXHIBITION_SENSOR_CONTROLS 0`).

**Aging not yet run.** Everything above was measured over minutes, not hours.

## References

- [esp32-camera `sensors/ov2640.c`](https://github.com/espressif/esp32-camera/blob/master/sensors/ov2640.c)
- [esp32-camera `sensors/private_include/ov2640_settings.h`](https://github.com/espressif/esp32-camera/blob/master/sensors/private_include/ov2640_settings.h)
- [theoim/ArduCAM_RP2040_C](https://github.com/theoim/ArduCAM_RP2040_C)
