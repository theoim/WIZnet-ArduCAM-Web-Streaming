# ArduCAM MEGA — Web streaming over lwIP (software TCP/IP)

The same camera, the same page, the same endpoints as the
[TOE example](../WIZnet_ArduCAMMega_TOE_Web_Streaming) — with TCP terminated in
software instead of in the chip.

```
http://192.168.11.5/
```

The WIZnet chip runs in **MACRAW** mode: it hands raw Ethernet frames to the MCU and
lwIP does the rest. Run this board next to the TOE one and the two pages are directly
comparable — that is the whole point of the example existing.

---

## What differs from the TOE example

Only the transport. The camera driver is compiled from the TOE example's directory
rather than copied, so both demos capture identically and any difference on screen
comes from the network stack.

| | TOE | lwIP |
|---|---|---|
| Socket mode | `Sn_MR_TCP4` × 4 | `Sn_MR_MACRAW` × 1 |
| TCP state | In the chip | In MCU RAM |
| HTTP server | Blocking socket writes | lwIP raw API callbacks |
| Address | `192.168.11.3` | `192.168.11.5` |
| Badge | **TOE**, red | **lwIP**, charcoal |

---

## The part that makes lwIP hard here

`arducam_capture_frame()` blocks for 22–120 ms depending on resolution. With a
hardwired stack that is fine: the chip keeps acknowledging and buffering while the
MCU is busy. Here the MCU **is** the stack, so for that whole window nothing is
acknowledged, no retransmit timer runs, and the receive buffer just fills.

Three things follow from that, and all three are in the code:

**The receive buffer is drained, not sampled.** `net_service()` loops until the chip
has nothing left rather than taking one frame per pass. Anything still queued when the
next capture starts risks being overwritten, and the lost segments come back as
retransmissions.

**The stack is serviced while waiting for send window space.** When `tcp_sndbuf()`
returns zero, returning to the main loop would mean nothing runs the stack until after
the next capture — tens of milliseconds later. Instead the send loop calls
`net_service()` in place and retries.

**The next capture waits for the frame to be acknowledged.** The JPEG is written
without `TCP_WRITE_FLAG_COPY`, so lwIP holds a pointer into `image_buff` for
retransmission rather than a copy. Capturing as soon as the last byte is *queued*
would overwrite data lwIP may still need to resend, and the peer would receive a JPEG
spliced from two frames — which decodes to a flickering mess. So `TX_DRAINING` holds
the buffer until `unacked` reaches zero.

Copying instead is not an option: a 100 KB frame does not fit in the lwIP heap
(`MEM_SIZE` is 16 KB). Waiting costs frame rate and buys a picture that does not
flicker.

**The response body is sent across iterations too.** The page is around 10 KB and
`TCP_SND_BUF` is eight segments, so a response does not necessarily fit in the send
buffer when the request arrives. `resp_pump()` drains the remainder as space frees;
writing what fits and dropping the rest would truncate the page.

---

## What to expect

Lower frame rate and lower throughput than the TOE board, from the same camera at the
same resolution. That is the honest result, and it is the demonstration — not
something to tune away.

Measure it yourself with the two charts side by side rather than trusting a number
here; it depends on the network, the client, and the resolution.

---

## Build

```bash
cmake --build build --target lwip_web_streaming
```

Output: `build/example/WIZnet_ArduCAMMega_Lwip_Web_Streaming/lwip_web_streaming.uf2`

Address and MAC are at the top of `main.c`. lwIP options live in
`port/lwip/lwipopts.h`; `MEM_SIZE` and `TCP_SND_BUF` are the two that matter for
streaming.

## Files

| | |
|---|---|
| `main.c` | Clock, chip init, MACRAW socket, netif, main loop |
| `httpd_stream.c` | HTTP server and MJPEG sender on the lwIP raw API |
| `web_page.h` | The UI, identical to the TOE example |
| `logo_png.h` | Logo, embedded |
