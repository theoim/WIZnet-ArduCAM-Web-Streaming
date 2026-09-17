/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Simon Goldschmidt
 *
 */
#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

/* Prevent having to link sys_arch.c (we don't test the API layers in unit tests) */
#define NO_SYS 1
#define MEM_ALIGNMENT 4
#define LWIP_RAW 1
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_ICMP 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define MEM_SIZE 16384

/*
 * Connection turnover, sized for a browser talking to a camera.
 *
 * A page load opens six connections at once (the page, the logo, the favicon,
 * the control list, the first status poll and the stream) and every one of them
 * is Connection: close. With the stock TCP_MSL of 60 s each finished connection
 * holds a pcb in TIME_WAIT for 2*MSL - two minutes - against a pool of five.
 * The observable result on the ArduCAM exhibition build was a device that
 * needed several minutes after boot before it would hold a stream at all, and
 * then ran fine: the pool was full of connections that had been closed since
 * the page loaded.
 *
 * TCP_MSL is 2000, so TIME_WAIT is 2*MSL = 4 s. It was 5000 - ten seconds -
 * which is what made the pool the binding constraint: a resolution change
 * leaves six or seven pcbs waiting, and at one change every three seconds
 * (the page's own rate limit) three changes' worth overlap before the first
 * has expired. Shortening the wait cuts the overlap without costing a byte.
 *
 * Four seconds is still four thousand times the round trip on a local link.
 *
 * Pool raised from 10 to 16 after measuring it.
 *
 * Idle costs one pcb. A resolution change costs six or seven: the stream
 * connection is torn down and reopened, and the connections the browser had in
 * flight are closed with it, each leaving a pcb in TIME_WAIT for 2*MSL. Two
 * changes in quick succession reached 8/10 and four reached 10/10 - the whole
 * pool, with one active connection and nine waiting.
 *
 * It never errored, because tcp_alloc() kills the oldest TIME_WAIT pcb before
 * it gives up. That is the problem rather than the reassurance: with the pool
 * full, the next connection is served by killing something, and there is no
 * rule that says the something will be a finished connection rather than the
 * live stream. At an exhibition the buttons get pressed repeatedly by people
 * who want to see what they do.
 *
 * Six more pcbs cost about 900 bytes of static RAM - 0.2 % of the 520 KB on
 * this part. The page also rate-limits the resolution buttons now, so this is
 * the second line of defence rather than the first.
 */
#define TCP_MSL             2000
#define MEMP_NUM_TCP_PCB    16

// disable ACD to avoid build errors
// http://lwip.100.n7.nabble.com/Build-issue-if-LWIP-DHCP-is-set-to-0-td33280.html
#define LWIP_DHCP_DOES_ACD_CHECK 0

#define ETH_PAD_SIZE 0
#define LWIP_IP_ACCEPT_UDP_PORT(p) ((p) == PP_NTOHS(67))

#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_STATUS_CALLBACK 1

#define TCP_MSS (1500 /*mtu*/ - 20 /*iphdr*/ - 20 /*tcphhr*/)
/*
 * The send window, and the reason a 47 KB frame took 94 ms to be acknowledged.
 *
 * At 8 MSS only 11.6 KB can be unacknowledged at once, so a 720p JPEG has to
 * fill and drain that window four times over - four round trips, plus whatever
 * the peer's delayed-ACK timer adds to each. Measured on the exhibition build:
 * VSYNC 18 + readout 22 + send 11 + ACK wait 94 ms. Two thirds of the frame
 * period spent waiting rather than working, with lwIP's retransmit timer firing
 * on top of it - the capture shows spurious retransmissions.
 *
 * 16 MSS puts 23 KB in flight, half a frame, which halves the round trips.
 * Nothing here holds payload: the JPEG stays in image_buff and goes out without
 * TCP_WRITE_FLAG_COPY.
 */
/* REVERTED to 8. Raising this to 16 was tried and measured worse - ACK wait
   went from 94 ms to 1492 ms and the frame rate from 7.9 to 0.9 fps - which is
   the clue that the constraint is on the receive side, not the send side. A
   bigger window puts more data in flight, which brings more ACKs back, which
   starves the receive path faster. See PBUF_POOL_SIZE below. */
#define TCP_SND_BUF     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN (4 * TCP_SND_BUF / TCP_MSS)   /* = 32 */
/*
 * Global, and it has to leave room for something other than the stream.
 *
 * TCP_SND_QUEUELEN is per-pcb; this pool is shared. A 720p JPEG is about 46 KB,
 * which at one MSS per write is 33 segments - so one frame in flight took 33 of
 * the 36 there used to be, and lwIP then had nothing left to build a SYN,ACK
 * with. A packet capture showed exactly that: the browser's SYNs retransmitting
 * unanswered while the stream ran at full rate, and connecting fine in the gaps
 * between frames.
 *
 * MEMP_NUM_PBUF is the matching limit on the other side of the same write: the
 * JPEG goes out without TCP_WRITE_FLAG_COPY, so every tcp_write() takes a
 * PBUF_ROM pbuf from this pool, and the stock 16 cannot describe a 33-chunk
 * frame at all.
 *
 * Both are small structs - a few hundred bytes each at these counts - because
 * neither owns payload. The payload stays in image_buff.
 */
/* Sized above TCP_SND_QUEUELEN (now 64) rather than at it, so that filling the
   stream window still leaves segments for a SYN,ACK and for the status poll. */
#define MEMP_NUM_TCP_SEG 96
#define MEMP_NUM_PBUF    96

/*
 * The receive path, and where the retransmissions were coming from.
 *
 * Every Ethernet frame the MACRAW socket hands up takes a pbuf from this pool -
 * net_service() calls pbuf_alloc(PBUF_RAW, len, PBUF_POOL) - and when the pool
 * is empty it gives up and leaves the data in the chip. What gets dropped that
 * way is mostly ACKs, so lwIP never learns that the peer received the frame and
 * its retransmit timer fires: the capture shows the device sending spurious
 * retransmissions of data the PC had already acknowledged.
 *
 * Unlike the two pools above this one owns payload - about 1.5 KB each - so 32
 * costs roughly 24 KB. That is the price of not dropping acknowledgements while
 * a 47 KB frame is going out.
 */
#define PBUF_POOL_SIZE   32

/*
 * Pool accounting. Small counters, and the only way to answer "which pool ran
 * out" without guessing - which on this example has been the expensive way to
 * answer anything.
 */
#define LWIP_STATS       1
#define MEMP_STATS       1
#define MEM_STATS        1
#define LINK_STATS       1
#define LWIP_STATS_DISPLAY 0

#define LWIP_HTTPD_CGI 0
#define LWIP_HTTPD_SSI 0
#define LWIP_HTTPD_SSI_INCLUDE_TAG 0

#define LWIP_RAND_WIZ() ((u32_t)rand())

#if 1
#define LWIP_DEBUG 1
#define TCP_DEBUG LWIP_DBG_OFF
#define ETHARP_DEBUG LWIP_DBG_OFF
#define PBUF_DEBUG LWIP_DBG_OFF
#define IP_DEBUG LWIP_DBG_OFF
#define TCPIP_DEBUG LWIP_DBG_OFF
#define DHCP_DEBUG LWIP_DBG_OFF
#define UDP_DEBUG LWIP_DBG_OFF
#endif

#endif /* __LWIPOPTS_H__ */
