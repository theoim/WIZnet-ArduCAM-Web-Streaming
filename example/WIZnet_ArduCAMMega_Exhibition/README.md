# ArduCAM MEGA — web streaming, exhibition build

> **Running on hardware.** The load comparison and the link-drop recovery are
> measured. The side-by-side two-board run and the timing-bar breakdown are not
> done yet, and are called out where they belong.

Two firmware images from one source tree. Flash one board with each, put them
side by side, and let a visitor load the link while both are streaming.

```
http://192.168.11.3/     exhibition_toe    the WIZnet chip terminates TCP
http://192.168.11.5/     exhibition_lwip   lwIP terminates TCP on the MCU
```

Same camera driver, same page, same measurement code, same object files for all
of it. The only difference between the two images is which server file is
compiled and one `-D`, so a difference on screen came from the network stack.

---

## What this adds over the two streaming examples

`WIZnet_ArduCAMMega_TOE_Web_Streaming` and `..._Lwip_Web_Streaming` stay as they
are — they are the reference implementations and this build does not touch them.
It compiles their camera driver, PSRAM, UART and PIO program rather than copying
them, for the same reason those two share: the boards must capture identically
or the comparison is measuring two cameras.

| | Streaming examples | This |
|---|---|---|
| Load generator | — | `GET /load?kb=N`, one connection, 32/64/128 KB |
| States shown | streaming / stopped | streaming / **loaded** / **no frames** / **no reply** |
| Link drops | picture freezes, stays frozen | says so, and comes back on its own |
| Sensor controls | CLK_DIV, PLL_DIV | those, plus a panel built from `/api/controls` — **off by default, see below** |
| Clamped setting | slider silently snaps back | 409, and the page says the sensor refused it |
| Stack selection | two directories | one directory, two targets |

---

## The exhibit: what a visitor does

1. Both boards are streaming. The frame-rate charts sit at whatever the sensor
   and the link give them.
2. They press **64** under *Link load* on one page. The browser starts pulling
   64 KB of filler from that device, over and over, on **one** connection,
   while the video keeps running.
3. The page turns blue and says so: *"Link under load — a lower frame rate here
   is the load, not a fault."*
4. They do the same on the other board and watch which frame-rate line falls
   further, and which timing bar grew.

### Measured side by side, 2026-09-07

Two boards, one switch, both at 1280x720, no load, one browser each.

| | TOE | lwIP |
|---|---:|---:|
| Frame rate | **24.9 fps** (avg 21.0) | **7.7 fps** (avg 9.0) |
| Stream | 12.1 Mbps | 2.9 Mbps |
| Frame | 61 KB | 47 KB |
| VSYNC wait | 6 ms | 12 ms |
| Sensor readout | **22 ms** | **22 ms** |
| Network send | **11 ms** | **11 ms** |
| **ACK wait** | **0 ms** | **82 ms** |

The four timings add up to the frame rate on both boards - 39 ms against a
measured 24.9 fps, 127 ms against 7.7 - which is the point at which this became
a measurement rather than an impression.

**Readout and network send are identical on the two boards.** Same camera, same
bytes, same time to put them on the wire. The frame rate differs by 3.2x, and
the whole of that difference sits in one column.

That column is what a hardwired stack buys. When the chip terminates TCP it owns
the buffer, so the MCU captures the next frame immediately - ACK wait is
structurally zero. When lwIP terminates TCP it holds a pointer into the capture
buffer rather than a copy, so the camera has to stand still until the peer
confirms receipt. A datasheet can tell you one stack is faster; it cannot tell
you that the cost is the camera waiting on an acknowledgement, which is the
thing a visitor can be shown.

### Measured under load, 2026-09-07 (earlier build)

First run, 1280x720, one browser per board, load pulled at 128 KB per request.
Read off the page's own charts.

| Stack | No load | Load 128 KB | Retained |
|---|---:|---:|---:|
| **TOE** (hardware TCP/IP) | 29+ fps | 20+ fps | ~69 % |
| **lwIP** (software TCP/IP) | 9+ fps | ~5 fps | ~56 % |

At the 128 KB setting the device reported **8.9 Mbps** of filler actually
served, alongside the video. That is measured output, not the slider position -
a client that cannot keep the requests coming pulls this number down with it,
which is the honest reading and the reason the device reports it rather than
trusting the page.

Two things at once, and the second is the one worth having built this for. The
idle gap is about 3.2x, which a datasheet could have told you. The *additional*
gap under load - 31 % lost against 44 % - is what a datasheet cannot: the
software stack pays for those bytes out of the same MCU the camera is using.

Both boards used exactly one extra socket for the load, so none of this is a
socket-budget difference. See "Why the load is one connection".

> Still to record: the three timing bars at OFF and at 128 KB on each board.
> The claim below rests on them, and they have not been written down yet.

The timing legend is what carries the argument. Under load only **network send**
should move — VSYNC wait and sensor readout are the sensor and do not know the
link is busy. If those move too, the load is stealing MCU time, which is what a
software stack does and a hardwired one does not.

### Why the load is one connection

Loading with *more connections* would have been easier and would have measured
the wrong thing. The TOE has four hardware sockets on port 80 and lwIP has as
many pcbs as memory allows, so a connection-count load compares socket budgets,
and a visitor reads "the TOE ran out of sockets" as "the TOE is slower".

One connection on both boards leaves exactly one variable: what a byte costs the
stack. That is the comparison worth putting in front of someone.

### Why three states and not two

A frozen picture at a booth reads as a broken product, and the old page drew
"slowed down by load", "sensor stopped" and "cable unplugged" identically. Each
now has its own colour, its own wording and its own recovery:

| State | Colour | Means | The page does |
|---|---|---|---|
| STREAMING | green | frames arriving | — |
| STREAMING + LOAD | blue | frames arriving, filler on the link | says the frame rate is the load |
| NO FRAMES | red, blinking | device answers, frame counter stopped | reopens the stream; suggests **Recover** |
| NO REPLY | amber, blinking | two polls in a row unanswered | labels the numbers as stale; reopens the stream when it comes back |

The last one matters most. Pull the cable and plug it back in: the old page
stayed frozen, because a broken `multipart` response is not retried by the
browser and `img.src` was only ever set on START. The device reports its frame
counter, so the page can tell that nothing is arriving and ask again.

**Measured 2026-09-07, cable pulled at the device end:** amber within 5 s, video
back within 2-3 s of plugging in.

Every request carries a deadline (`POLL_TIMEOUT`, via `AbortController`), and it
is load-bearing rather than tidy. Pulling the cable at the *device* end breaks
nothing on the viewer's machine: the browser's connection simply stops being
answered, and `fetch()` then neither resolves nor rejects while the local stack
retransmits - for tens of seconds. The entire link-down path hangs off
`.catch()`, so without the deadline the page never noticed at all. It sat on a
still picture looking healthy, which is precisely the failure this page was
built to prevent.

The 2-3 s to come back is four things in order, not slack: the 1 s poll
interval, a 300 ms wait before reopening the stream, the browser's new
connection, and the first frame being captured and sent.

---

## Build

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --target exhibition_toe
cmake --build build --target exhibition_lwip
```

```
build/example/WIZnet_ArduCAMMega_Exhibition/exhibition_toe.uf2
build/example/WIZnet_ArduCAMMega_Exhibition/exhibition_lwip.uf2
```

Board selection is `BOARD_NAME` at the top of the root `CMakeLists.txt`, as for
every other example here.

### The recovery loop that fed itself (lwIP)

Worth reading before touching the reconnect logic, because it cost most of a
day and none of it was where it looked.

The page reopens the stream when it decides the device came back. On the TOE
image that is free. On the lwIP image every reopen is a new TCP connection plus
a close on one the browser has already walked away from, so the old one lingers
in a pcb pool of five (`MEMP_NUM_TCP_PCB`, lwIP's default - the port does not
raise it). Reopen once a second and the pool is gone; the next connection is
never accepted, the page sees `ERR_CONNECTION_TIMED_OUT`, decides the device is
gone, and answers by reopening the stream.

**The recovery was manufacturing the fault it was recovering from.** The serial
log said so plainly once it was asked the right question: every `/api/status`
was received, answered and closed - `[resp] done` each time - while the browser
timed out on the connections that never got a pcb.

Four separate attempts were made to fix the lwIP *server* first: a four-slot
response table, `TCP_WRITE_FLAG_COPY` on the body, a double-close guard. Each
was reasoning from a predicted problem rather than a measured one, and one of
them was a real regression - the device stopped answering `/api/status`
entirely. All three are reverted; the response path is now byte-for-byte the
shape from `WIZnet_ArduCAMMega_Lwip_Web_Streaming`, which had been working the
whole time.

What actually fixed it, all in `web_page.h`:

| Change | Why |
|---|---|
| 12 s cooldown on `restartStream()` | Breaks the loop. A cable pull is one-off and still recovers at once; a loop gets one reopen per 12 s, which the pool absorbs |
| `force` flag for user actions | START, resolution, Recover and Apply skip the cooldown - somebody is waiting on those and cannot click once a second |
| `POLL_TIMEOUT` 2.5 s -> 6 s | On lwIP a status response shares the send path with a JPEG frame and legitimately takes over a second. 2.5 s turned ordinary slowness into "gone" |
| 3 missed polls instead of 2 | Same reason |

The general lesson, since it will come up again: **an automatic recovery is a
control loop, and a control loop on a resource-constrained stack has to be rate
limited against the resource it consumes.** Reopening a connection to prove a
connection works is not free.

Two pieces of instrumentation stayed because they are what ended this:
`EXHIBITION_HTTP_LOG` prints every request and every response transition, and
the page reports a `render()` exception as **PAGE ERROR** rather than letting it
fall through to the link-down path - a page bug used to present as "the device
is not answering", which is what sent four investigations into the firmware.

**Turn `EXHIBITION_HTTP_LOG` off for the exhibition.** One printf per request is
real work on the side where the MCU is also the TCP stack, and any frame rate
measured with it on is lower than the real one.

### Build type: use `-Og`, not `-O3`

**`CMAKE_BUILD_TYPE=Release` makes the camera slower.** Measured on 2026-09-07:
the same source built `Debug` (`-Og`) reached the frame rate this hardware
normally gives, and built `Release` (`-g -O3 -DNDEBUG`) sat at about 15 fps at
720p. Not the page, not the server, not the driver - three different examples
with three different page sizes all converged on the same number, which is what
a shared cause looks like.

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
```

Why `-O3` is slower has not been confirmed. The capture path arms its DMA during
the blanking interval before VSYNC rises, and missing that window sends the
capture into a retry rather than into a frame; optimisation changes the code
laid out in front of that window. Treat it as a live trap rather than an
explanation: **a capture loop that gets slower when the optimiser gets better is
depending on something it has not written down**, and it will bite again when
the toolchain or the SDK moves.

Addresses, MAC addresses and the load ceiling are in `exhibition_config.h`. The
two images must not share an address — they are meant to be on one network at
the same time.

---

## Files

| | |
|---|---|
| `exhibition_config.h` | which stack, which address, how big the load gets |
| `main.c` | orchestration only: clock, camera, hand over to the server |
| `cam_state.c` | capture state, the one-second window, the status document |
| `cam_controls.c` | the sensor control table — see the warning below |
| `net_load.c` | the load generator's payload and byte accounting |
| `web_page.h` | the page |
| `server_toe.c` | hardware TCP/IP, four listening sockets |
| `server_lwip.c` | software TCP/IP over MACRAW, lwIP raw API |
| `net_server.h` | the two-function seam between `main.c` and either server |

Both servers build the status document by calling the same function, so the two
boards emit byte-identical JSON. That is not tidiness — a field that differed in
name or rounding between them would put the difference in the page instead of in
the network stack.

---

## Endpoints

| Endpoint | Does |
|---|---|
| `GET /` | the page |
| `GET /stream` | `multipart/x-mixed-replace`, one JPEG per part |
| `GET /logo.png` | logo, cached by the browser |
| `GET /load?kb=N` | N KB of filler — the load generator |
| `GET /api/status` | the status document; every control returns it too |
| `GET /api/start` · `/api/stop` | start and stop capture |
| `GET /api/res?v=640x480` | frame size |
| `GET /api/clk?div=1&pll=1` | sensor clock dividers |
| `GET /api/cam?brightness=4&contrast=2` | any subset of the control table |
| `GET /api/controls` | the control descriptors — name, label, group, range |
| `GET /api/reset` | reinitialise a wedged sensor |

Every control answers with the status document, so the page feeds each response
through one render path and never has to work out what changed. A value outside
its range is clamped and answered **409**, which is how the page knows to say the
sensor refused it rather than letting a slider snap back unexplained.

---

## The sensor control table

`cam_controls.c` holds one table — name, label, group, register, range — and the
query parser, the status JSON and the browser panel are all generated from it.
Adding a row makes a new slider appear in the browser with no edit to
`web_page.h` and no edit to either server.

> **Off by default.** `EXHIBITION_SENSOR_CONTROLS` in `exhibition_config.h` is
> `0`, and at `0` the sensor is never written to: `/api/controls` answers an
> empty list, `/api/cam` does nothing, and the panel says so. Turn it on only
> after the ranges below have been checked against the module in front of you.
>
> The first version of this build pushed the whole table into the sensor at
> boot, and both images got slower. `IMAGE_QUALITY_REG` written with 0 is the
> likely culprit — on this part 0 is the highest quality setting, so every JPEG
> got bigger, which costs frame rate on the TOE image and costs stability on the
> lwIP one, where a bigger frame has to clear a 16 KB heap and the drain wait.
> One unverified register write, two different-looking symptoms.

> **The ranges are provisional and have not been checked against a module.**
> They follow the ArduCAM MEGA level enumerations, where a control is a level
> index rather than a signed amount — brightness 0 is "default" and 1..8 step
> away from it — which is why they start at 0 and are not symmetric. If a value
> does nothing on your module, correct the row; the panel follows on its own.

Deliberately absent, and the reason is worth keeping: `AGC_MODE_REG`,
`MANUAL_AGC_REG` and `MANUAL_EXP_H/L_REG` are mode selects rather than plain
values, and their bit layout cannot be confirmed from anything in this
repository. Writing a guessed value into a sensor mode register is how a camera
ends up wedged halfway through an exhibition. They stay out until they can be
checked against hardware. Everything in the table writes one register with one
value.

---

## Show build, 2026-09-16/17

Changes made after running the demo on the laptop that actually goes to the
show, rather than the desk monitor it was built on. All of it is measured on
hardware unless the section says otherwise.

### Simplified page — `EXHIBITION_SIMPLE_UI`

On by default. The full page assumes a sidebar and 1440p; on 1366x768 the
control column pushed the picture down until the graphs fell below the fold, so
the one thing the exhibit is about - the stream degrading - was off screen.

Same markup, same script. Only what gets painted changes, so switching back is
one `#define` in `exhibition_config.h` and not a merge. The page is logo, stack
badge, picture edge to edge, then the two graphs. START/STOP, the resolution
dropdown, the link load row and the bring-up controls are hidden: the board
autostarts and none of it is a visitor's decision.

The picture is not given a size. It takes whatever is left after the header and
the graph row, which is the only version that survives an unknown laptop.
Verified at 1280x720, 1366x768 and 1920x1080.

**Hiding the link load hides the comparison this build was made for.** With it
off a visitor sees one stack running clean, not the TOE holding up while lwIP
gives way. If the load is meant to be part of the demo, either set the switch
to 0 or drive the load from the host instead of the page.

### Resolution bar — HD and FHD only

A wide segmented row between the picture and the graphs, plus the running mode
over the picture. The other three modes do not separate the two stacks: at QVGA
and VGA both keep up, and UXGA sits close enough to FHD that the two runs look
alike. Five buttons where three show the same thing is three chances to pick the
one that proves nothing.

The lit button and the cleared history are both driven off the status reply, not
off the click, so they follow what the camera actually did.

**The graph history is cleared on any resolution change.** min/avg/max are
computed over the whole buffer, so HD samples left in it after a switch to FHD
dragged the average toward a frame size that was no longer being sent - the page
reported a number the device never achieved in either mode.

### Boots at HD

`cam_state.c` defaults to `RES_1280X720`. Changing the default alone was not
enough and the way it failed is worth keeping:

`arducam_mega.init()` sets JPEG and 320x240 itself, with a 200 ms settle between
and after. `main.c` overrode it by calling `set_pixel_format()` and
`set_frame_size()` back to back with no wait, and the resolution write was
silently dropped. The console printed

```
Setting initial resolution to 320x240...     (driver, inside init)
Initial resolution: 1280x720                 (ours, from our own state)
```

while the sensor sent 4 KB frames at 0.3 Mbps. Neither line was wrong; neither
was asking the sensor. `main.c` now uses the same settles and checks
`set_frame_size()`'s return value instead of throwing it away.

### SRAM reported live

`/api/status` carries `sram_used` and `sram_total`; the legend shows both. A map
file says what was reserved at link time, and the question worth answering is
what a stream costs on top of that while it runs.

`used = statics + mallinfo().uordblks`. The stack is not counted, so it is a
floor.

| image | static | of 520 KB | flash |
|---|---:|---:|---:|
| `exhibition_toe` | 217.4 KB | 41.8 % | 124.3 KB |
| `exhibition_lwip` | 291.3 KB | 56.0 % | 171 KB |

The 74 KB difference is what terminating TCP on the MCU costs here, and it
breaks down cleanly:

| object | size |
|---|---:|
| `memp_memory_PBUF_POOL_base` | 47.9 KB |
| `ram_heap` (`MEM_SIZE`) | 16.0 KB |
| `memp_memory_TCP_PCB_base` | 2.4 KB |
| `memp_memory_TCP_SEG_base` | 1.9 KB |
| other lwIP structures | ~5.7 KB |

`image_buff`, the 200 KB JPEG capture buffer, is in both and dominates each. It
does not grow with the selected resolution - a 1080p frame lands in the same
array a QVGA frame does - so the resolution buttons move the network numbers,
not this one.

### The pcb pool ran out

Found by pressing the HD/FHD buttons the way a visitor would.

```
[lwip] pcb 10/10 e0 ...
[lwip] active 1  time_wait 9
```

One live connection and nine finished ones holding the rest of the pool. It
never reported an error, because `tcp_alloc()` kills the oldest TIME_WAIT pcb
before it gives up - which is the problem rather than the reassurance. With the
pool full every new connection is served by killing something, and nothing
guarantees it picks a finished connection over the live stream.

A resolution change is more expensive on the device than it looks from the page:
the stream connection is torn down and reopened and the connections the browser
had in flight go with it, leaving six or seven pcbs in TIME_WAIT.

Three changes, in order of how much they mattered:

- **`TCP_MSL` 5000 -> 2000.** TIME_WAIT is `2*MSL`, so it was ten seconds. At one
  change every three seconds, three changes' worth overlapped before the first
  expired. Four seconds is still four thousand times the round trip on a local
  link, and it costs nothing. This is the one that did the work.
- **`MEMP_NUM_TCP_PCB` 10 -> 16.** Headroom for when the burst gets through
  anyway. About 900 bytes of static RAM, 0.2 % of the part.
- **3 s rate limit on the resolution buttons.** Page side, no RAM. Stops the
  burst rather than absorbing it.

Measured after, same test:

```
before   peak 10/10,  6-7 held while clicking,  no headroom
after    peak  8/16,  3   held while clicking,  8 spare
```

Everything else had room throughout: TCP_SEG peaked at 14 of 96, PBUF at 15 of
96, PBUF_POOL at 1 of 32, the lwIP heap at 3,724 of 16,384.

### Security: what would fit

Reviewed, not implemented, and **not measured.** RAM and flash are not the
constraint:

- Four concurrent TLS sessions with 4 KB record buffers is roughly 40 KB. The
  TOE image has 294.6 KB of heap free at boot, the lwIP image 221.6 KB.
- Flash would go to roughly 220-270 KB against 4 MB.

The open question is throughput. **RP2350 has hardware SHA-256 and a TRNG but no
AES accelerator**, and the HD stream is about 574 KB/s (41 KB frames at ~14 fps)
that would all have to pass through software AES-GCM. Two numbers decide it and
neither has been taken:

1. AES-128-GCM throughput in KB/s - does the stream fit?
2. ECDSA P-256 sign time - how long does a connection take?

If the first falls short, ChaCha20-Poly1305 is usually 2-4x faster than AES-GCM
on a 32-bit MCU without AES hardware, and mbedTLS supports it on TLS 1.2 and 1.3.

---

## Known limits

Where this build stops. None of these are accidents.

- **Not measured.** No frame rates are quoted here because none have been taken
  on a bench with both boards. Take them with the charts before quoting any.
  The RAM figures under "Show build" above ARE measured, but at link time plus
  idle heap - the number a running stream adds is what the live SRAM readout is
  for, and it has not been recorded here.
- **The TOE side blocks while it sends.** `tcp_send_all()` spins until the chip
  takes the bytes, so a frame going out is time the other sockets are not
  polled. The load demo makes that visible, which is useful — but it also means
  the TOE number under load is partly a property of this loop rather than of the
  chip. Worth measuring before it appears on a slide.
- **One deferred response at a time on the lwIP side.** Two large responses
  overlapping would clobber each other's state. Inherited from the streaming
  example and not made worse here, but the load generator makes large responses
  ordinary rather than rare.
- **No authentication, no TLS.** Anything that can reach the address can watch
  the camera and change its settings. What would fit is reviewed under "Show
  build" above; the throughput measurement that decides it has not been taken.
- **The load is generated by the browser.** A laptop that cannot keep the
  requests coming lowers the load without saying so — which is why the device
  reports the bytes it actually served rather than the slider position. If the
  reported load will not reach the setting, the client is the limit.
