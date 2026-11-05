/**
 * @file    wiz_transport.c
 * @brief   Connections on the WIZnet hardware TCP/IP stack. See wiz_transport.h.
 *
 * Step 1 of the server_toe.c split (2026-11-05). The socket machine, the TLS
 * steps, the send/receive helpers and the connection diagnostics moved here
 * unchanged in behaviour; what used to be direct calls into the HTTP code
 * (http_req_init, serve_request, the 408/503 statuses, the stream sender,
 * release_stream_socket) are now the callbacks in wt_handlers_t.
 *
 * Socket numbers stay inside this file: connection c is hardware socket
 * cfg.first_socket + c, and conn.c indexes by c.
 */

#include "wiz_transport.h"

#if defined(NET_STACK_TOE)

#include <stdio.h>
#include <string.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"

#include "pico/stdlib.h"
#include "pico/time.h"

#include "conn.h"
#if defined(STREAM_HTTPS)
#include "tls_server.h"
#endif

#define TCP_CHUNK_MAX       1460        /* one segment worth per send() call    */
#define SEND_TIMEOUT_MS     2000        /* no progress for this long -> drop    */

#if _WIZCHIP_ >= W6100
    #define TCP_SOCK_MODE   Sn_MR_TCP4
#else
    #define TCP_SOCK_MODE   Sn_MR_TCP
#endif

#if CONN_MAX_SOCKETS < WT_MAX_CONN
#error "CONN_MAX_SOCKETS must cover WT_MAX_CONN"
#endif
#if CONN_REQ_POOL != WT_MAX_REQUEST_SLOTS
#error "conn.c request pool and WT_MAX_REQUEST_SLOTS must be the same size"
#endif
#if defined(STREAM_HTTPS) && CONN_REQ_POOL < TLS_MAX_CONN
#error "one request context per TLS session: CONN_REQ_POOL < TLS_MAX_CONN"
#endif

/* The two enums are the same list; the layer above sees only the wt_ one. */
#define SAME_REASON(w, c) _Static_assert((int)(w) == (int)(c), #w " != " #c)
SAME_REASON(WT_CLOSE_DONE,        CONN_CLOSE_DONE);
SAME_REASON(WT_CLOSE_PARSE,       CONN_CLOSE_PARSE);
SAME_REASON(WT_CLOSE_TIMEOUT_HS,  CONN_CLOSE_TIMEOUT_HS);
SAME_REASON(WT_CLOSE_TIMEOUT_REQ, CONN_CLOSE_TIMEOUT_REQ);
SAME_REASON(WT_CLOSE_TLS,         CONN_CLOSE_TLS);
SAME_REASON(WT_CLOSE_TLS_REFUSED, CONN_CLOSE_TLS_REFUSED);
SAME_REASON(WT_CLOSE_PEER,        CONN_CLOSE_PEER);
SAME_REASON(WT_CLOSE_TAKEOVER,    CONN_CLOSE_TAKEOVER);
SAME_REASON(WT_CLOSE_SEND,        CONN_CLOSE_SEND);
SAME_REASON(WT_CLOSE_POOL,        CONN_CLOSE_POOL);
SAME_REASON(WT_CLOSE_REASONS_,    CONN_CLOSE_REASONS_);

/* -------------------------------- Variables ------------------------------- */
static wt_config_t   g_cfg;
static wt_handlers_t g_h;
static conn_table_t  g_conn;
/* on_closed still owed to the layer above: set when a connection is handed
 * up (registered with conn.c), cleared by the first close or socket CLOSED. */
static bool          g_live[WT_MAX_CONN];
static uint32_t      g_quick_close;     /* LAST_ACK/TIME_WAIT closed at once */

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static uint8_t  sn_of(wt_conn_t c) { return (uint8_t)(g_cfg.first_socket + c); }

static void notify_closed(wt_conn_t c, wt_close_reason_t why)
{
    if (!g_live[c]) {
        return;
    }
    g_live[c] = false;
    if (g_h.on_closed) {
        g_h.on_closed(g_h.user, c, why);
    }
}

/* Connection c just got a request slot: let the layer above set it up. */
static void notify_ready(wt_conn_t c)
{
    if (g_h.on_ready) {
        g_h.on_ready(g_h.user, c, (wt_slot_t)conn_req(&g_conn, c));
    }
}

/* No request slot for c: the layer above may still answer (503). */
static void refuse(wt_conn_t c)
{
    if (g_h.on_refused) {
        g_h.on_refused(g_h.user, c);
    }
    wt_close(c, WT_CLOSE_POOL);
}

/* ------------------------------- Send / recv ------------------------------- */
/*
 * Send the whole buffer, chunked to what the WIZnet TX buffer will take.
 *
 * This spins rather than yielding, so a response going out is time the other
 * sockets are not being polled - including their deadlines, which are checked
 * on the next pass. Fine for pages and JSON; the MJPEG stream uses
 * wt_send_some() instead.
 */
int32_t wt_send_all(wt_conn_t c, const void *data, uint32_t len)
{
    uint8_t sn = sn_of(c);
#if defined(STREAM_HTTPS)
    /* Same contract, through the TLS record layer: encrypt, frame, then the
     * same chunked TOE sends underneath (tls_server.c bio_send). */
    return tls_send_all(sn, (const uint8_t *)data, len);
#else
    const uint8_t  *buf = (const uint8_t *)data;
    uint32_t        sent = 0;
    absolute_time_t deadline = make_timeout_time_ms(SEND_TIMEOUT_MS);

    while (sent < len) {
        uint16_t freesize = 0;
        uint32_t remain;
        uint16_t chunk;
        int32_t  ret;

        if (getSn_SR(sn) != SOCK_ESTABLISHED) {
            return -1;
        }

        getsockopt(sn, SO_SENDBUF, &freesize);
        if (freesize == 0) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
                return -2;               /* client is not draining */
            }
            continue;
        }

        remain = len - sent;
        chunk  = (remain > freesize) ? freesize : (uint16_t)remain;
        if (chunk > TCP_CHUNK_MAX) {
            chunk = TCP_CHUNK_MAX;
        }

        ret = send(sn, (uint8_t *)(buf + sent), chunk);
        if (ret < 0) {
            return ret;
        }

        sent    += (uint32_t)ret;
        deadline = make_timeout_time_ms(SEND_TIMEOUT_MS);
    }

    return (int32_t)sent;
#endif
}

/*
 * Queue as much as the socket takes right now and return without waiting.
 * 0: TX buffer full, or the previous SEND still in flight (send() answers
 * SOCK_BUSY).
 */
int32_t wt_send_some(wt_conn_t c, const void *data, uint32_t len)
{
    uint8_t sn = sn_of(c);
#if defined(STREAM_HTTPS)
    return tls_send_some(sn, (const uint8_t *)data, len);
#else
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t sent = 0;

    while (sent < len) {
        uint16_t freesize = 0;
        uint32_t remain;
        uint16_t chunk;
        int32_t  ret;

        if (getSn_SR(sn) != SOCK_ESTABLISHED) {
            return -1;
        }
        getsockopt(sn, SO_SENDBUF, &freesize);
        if (freesize == 0) {
            break;
        }
        remain = len - sent;
        chunk  = (remain > freesize) ? freesize : (uint16_t)remain;
        if (chunk > TCP_CHUNK_MAX) {
            chunk = TCP_CHUNK_MAX;
        }
        ret = send(sn, (uint8_t *)(buf + sent), chunk);
        if (ret < 0) {
            return ret;
        }
        if (ret == 0) {
            break;
        }
        sent += (uint32_t)ret;
    }
    return (int32_t)sent;
#endif
}

int32_t wt_recv(wt_conn_t c, void *buf, uint32_t cap)
{
    uint8_t sn = sn_of(c);
    int32_t n;
#if defined(STREAM_HTTPS)
    n = tls_recv(sn, (uint8_t *)buf, cap);   /* <0: close_notify or TLS error */
#else
    uint16_t avail = 0;
    getsockopt(sn, SO_RECVBUF, &avail);
    if (avail == 0) {
        return 0;
    }
    if (avail > cap) {
        avail = (uint16_t)cap;
    }
    n = recv(sn, (uint8_t *)buf, avail);
#endif
    if (n > 0) {
        conn_on_bytes(&g_conn, c, now_ms());
    }
    return n;
}

/* ------------------------------ Connection end ----------------------------- */
/*
 * The one way a connection ends. Safe to call more than once: only the first
 * call for a connection does the work.
 *
 * disconnect() on a blocking socket waits in the driver until the FIN
 * exchange finishes - bounded only by a 200,000-read spin in the modified
 * ioLibrary, i.e. seconds against a silent peer. The socket is switched to
 * non-blocking just for this call, so disconnect() asks for the FIN and
 * returns; conn's CLOSING deadline then forces close() if the exchange never
 * completes. socket() and close() reset the mode, so the next connection on
 * this socket is blocking again as before.
 */
void wt_close(wt_conn_t c, wt_close_reason_t why)
{
    uint8_t sn = sn_of(c);
    if (conn_phase(&g_conn, c) == CONN_FREE) {
        /* never registered (closed before we looked): register to get the
           CLOSING deadline; TLS phase takes no request context */
        conn_on_established(&g_conn, c, now_ms(), true);
    }
    if (!conn_close(&g_conn, c, now_ms(), (conn_close_reason_t)why)) {
        return;                                     /* already closing */
    }
    notify_closed(c, why);
#if defined(STREAM_HTTPS)
    tls_close(sn);
#endif
    uint8_t nb = SOCK_IO_NONBLOCK;
    ctlsocket(sn, CS_SET_IOMODE, &nb);
    disconnect(sn);
}

void wt_hold(wt_conn_t c)
{
    conn_on_stream(&g_conn, c, now_ms());           /* request slot back to the pool */
}

bool wt_is_held(wt_conn_t c)
{
    return conn_phase(&g_conn, c) == CONN_STREAM;
}

const char *wt_close_reason_name(wt_close_reason_t why)
{
    return conn_reason_name((conn_close_reason_t)why);
}

/* The socket is CLOSED: free what it held and reopen it as a listener in the
 * same pass - a pass can be held up hundreds of ms by a login or a handshake
 * on another socket, and a socket left in INIT meanwhile answers no SYN
 * (2026-10-26). */
static void recycle(wt_conn_t c)
{
    uint8_t sn = sn_of(c);
    notify_closed(c, WT_CLOSE_PEER);
#if defined(STREAM_HTTPS)
    tls_close(sn);                                  /* peer reset: free the session */
#endif
    conn_on_closed(&g_conn, c);
    if (socket(sn, TCP_SOCK_MODE, g_cfg.port, 0x00) != sn) {
        printf("Socket %u open failed\n", sn);
        return;
    }
    if (getSn_SR(sn) == SOCK_INIT && listen(sn) != SOCK_OK) {
        printf("Socket %u listen failed\n", sn);
    }
}

/* ------------------------------ Socket machine ----------------------------- */
static void socket_run(wt_conn_t c)
{
    uint8_t  sn  = sn_of(c);
    uint8_t  sr  = getSn_SR(sn);
    uint32_t now = now_ms();

    /* Deadlines first, whatever the socket state. */
    conn_action_t act = conn_tick(&g_conn, c, now);
    if (act == CONN_ACT_TIMEOUT) {
        conn_close_reason_t why = conn_timeout_reason(&g_conn, c);
        printf("[conn] s%u %s timeout in phase %s\n", sn, conn_reason_name(why),
               conn_phase_name(conn_phase(&g_conn, c)));
        if (conn_phase(&g_conn, c) == CONN_REQ && g_h.on_request_timeout) {
            /* best effort, bounded by the send stall */
            g_h.on_request_timeout(g_h.user, c, (wt_slot_t)conn_req(&g_conn, c));
        }
        wt_close(c, (wt_close_reason_t)why);
        return;
    }
    if (act == CONN_ACT_FORCE_CLOSE) {
        printf("[conn] s%u FIN exchange did not finish in %u ms (SR 0x%02X) - close()\n",
               sn, (unsigned)CONN_CLOSING_MS, sr);
        conn_note_forced(&g_conn, c);
        close(sn);                                  /* SOCK_CLOSED -> reopened below */
        sr = getSn_SR(sn);
    }

    switch (sr) {
    case SOCK_ESTABLISHED:
        if (getSn_IR(sn) & Sn_IR_CON) {
            setSn_IR(sn, Sn_IR_CON);
        }
        if (conn_phase(&g_conn, c) == CONN_CLOSING) {
            return;                                 /* our FIN is in flight */
        }
        if (conn_phase(&g_conn, c) == CONN_FREE) {
#if defined(STREAM_HTTPS)
            conn_on_established(&g_conn, c, now, true);
            g_live[c] = true;
#else
            if (!conn_on_established(&g_conn, c, now, false)) {
                refuse(c);
                return;
            }
            g_live[c] = true;
            notify_ready(c);
#endif
        }

#if defined(STREAM_HTTPS)
        if (conn_phase(&g_conn, c) == CONN_TLS) {
            /* TLS first: nothing below runs until the handshake is done. The
             * ECDHE step stalls this loop ~130-220 ms for a new client and
             * ~5 ms for one resuming from a ticket. */
            if (!tls_active(sn) && !tls_accept(sn)) {
                wt_close(c, WT_CLOSE_TLS_REFUSED);
                return;
            }
            int hs = tls_handshake_step(sn);
            if (hs < 0) {
                wt_close(c, WT_CLOSE_TLS);
                return;
            }
            if (hs == 0) {
                return;
            }
            if (!conn_on_tls_ready(&g_conn, c, now_ms())) {
                refuse(c);
                return;
            }
            notify_ready(c);
        }
#endif

        if (conn_phase(&g_conn, c) == CONN_STREAM) {
            if (g_h.on_hold) {
                g_h.on_hold(g_h.user, c);
            }
            return;
        }
        {
            int slot = conn_req(&g_conn, c);
            if (slot < 0) {
                wt_close(c, WT_CLOSE_POOL);
                return;
            }
            if (g_h.on_readable) {
                g_h.on_readable(g_h.user, c, (wt_slot_t)slot);
            }
        }
        return;

    /*
     * CLOSE_WAIT is the state a browser leaves behind when it closes a tab
     * without finishing the exchange. Recycling it here rather than waiting is
     * what keeps a listener from being lost one tab at a time over an
     * afternoon of visitors.
     */
    case SOCK_CLOSE_WAIT:
        wt_close(c, WT_CLOSE_PEER);
        return;

    case SOCK_CLOSED:
        recycle(c);
        return;

    case SOCK_INIT:
        if (listen(sn) != SOCK_OK) {
            printf("Socket %u listen failed\n", sn);
        }
        return;

    /*
     * LAST_ACK and TIME_WAIT: our FIN is out and the peer's FIN has arrived -
     * the response was delivered, all that is left is the peer's ACK of our
     * FIN (or the 2*MSL wait). Close now instead of waiting. Measured
     * 2026-10-26: back-to-back status polls left seven sockets in LAST_ACK
     * for ~65 ms at a time with none in LISTEN, and the PC's SYN in that gap
     * went unanswered until its retry 0.5 s / 1 s later.
     */
    case SOCK_LAST_ACK:
    case SOCK_TIME_WAIT:
        g_quick_close++;
        close(sn);
        recycle(c);                       /* back in LISTEN within this pass */
        return;

    default:
        return;                            /* LISTEN / SYNRECV / closing states */
    }
}

/* ------------------------------- Diagnostics ------------------------------- */
/*
 * Longest single socket_run() since the last status report: core 0 serves the
 * sockets one after another, so a run that takes long is time every other
 * connection waits - the suspect for status polls that took ~1 s on the LH
 * board (2026-10-26). Reported and reset by each status document; runs over
 * SLOW_LOG_MS are also logged as they happen.
 */
#ifndef CAM_NET_DIAG
#define CAM_NET_DIAG 0            /* 1: sample listener starvation (see listen_sample) */
#endif
#define SLOW_LOG_MS 300u
static uint32_t g_slow_us;
static uint8_t  g_slow_sn, g_slow_held, g_slow_phase;
static uint32_t g_slow_total;           /* runs over SLOW_LOG_MS since boot */

/*
 * Listener starvation: a client's SYN that finds no socket in LISTEN is not
 * answered, and the PC retries after 0.5 s / 1 s - exactly the connect times
 * of the slow status polls (2026-10-26). Sampled once a millisecond; the
 * longest span with no listener since the last status report, and the socket
 * states when a span starts, are reported.
 */
static uint32_t g_nolisten_max_ms, g_nolisten_spans;
static uint32_t g_nolisten_since;       /* ms, 0 = a listener exists */
static uint32_t g_listen_sample_at;

static void listen_sample(void)
{
#if !CAM_NET_DIAG
    return;                       /* 8 SPI reads a millisecond: diagnostic builds only */
#endif
    uint32_t t = now_ms();
    if (t == g_listen_sample_at) {
        return;
    }
    g_listen_sample_at = t;
    uint8_t sr[WT_MAX_CONN];
    unsigned listening = 0;
    for (wt_conn_t i = 0; i < WT_MAX_CONN; i++) {
        sr[i] = getSn_SR(sn_of(i));
        if (sr[i] == SOCK_LISTEN) listening++;
    }
    if (listening) {
        if (g_nolisten_since) {
            uint32_t span = t - g_nolisten_since;
            if (span > g_nolisten_max_ms) g_nolisten_max_ms = span;
            if (span >= 20u) {
                printf("[listen] no listener for %lu ms\n", (unsigned long)span);
            }
            g_nolisten_since = 0;
        }
        return;
    }
    if (!g_nolisten_since) {
        g_nolisten_since = t ? t : 1u;
        g_nolisten_spans++;
        printf("[listen] none listening: SR %02X %02X %02X %02X %02X %02X %02X %02X\n",
               sr[0], sr[1], sr[2], sr[3], sr[4], sr[5], sr[6], sr[7]);
    }
}

/* "role" keeps its old values ("stream" for a held connection) so the status
 * readers in test/ need no change. */
int wt_status_json(char *buf, size_t cap)
{
    const uint32_t *c = g_conn.closes;
    uint32_t slow_ms = g_slow_us / 1000u;
    uint8_t  ssn = g_slow_sn, sheld = g_slow_held, sphase = g_slow_phase;
    uint32_t nl = g_nolisten_max_ms;
    g_slow_us = 0;
    g_nolisten_max_ms = 0;
    return snprintf(buf, cap,
        "\"slow\":{\"ms\":%lu,\"sn\":%u,\"role\":\"%s\",\"phase\":%u,\"count\":%lu,"
        "\"nolisten_ms\":%lu,\"nolisten_spans\":%lu,\"quick_close\":%lu},"
        "\"http\":{\"done\":%lu,\"parse\":%lu,\"to_hs\":%lu,\"to_req\":%lu,"
        "\"tls_err\":%lu,\"refused\":%lu,\"peer\":%lu,\"takeover\":%lu,"
        "\"send\":%lu,\"pool\":%lu,\"forced\":%lu,\"req_peak\":%u},",
        (unsigned long)slow_ms, ssn, sheld ? "stream" : "http", sphase,
        (unsigned long)g_slow_total, (unsigned long)nl, (unsigned long)g_nolisten_spans, (unsigned long)g_quick_close,
        (unsigned long)c[CONN_CLOSE_DONE], (unsigned long)c[CONN_CLOSE_PARSE],
        (unsigned long)c[CONN_CLOSE_TIMEOUT_HS], (unsigned long)c[CONN_CLOSE_TIMEOUT_REQ],
        (unsigned long)c[CONN_CLOSE_TLS], (unsigned long)c[CONN_CLOSE_TLS_REFUSED],
        (unsigned long)c[CONN_CLOSE_PEER], (unsigned long)c[CONN_CLOSE_TAKEOVER],
        (unsigned long)c[CONN_CLOSE_SEND], (unsigned long)c[CONN_CLOSE_POOL],
        (unsigned long)g_conn.forced, (unsigned)g_conn.req_peak);
}

/* --------------------------------- Public ---------------------------------- */
bool wt_init(const wt_config_t *cfg, const wt_handlers_t *h)
{
    g_cfg = *cfg;
    g_h   = *h;

#if defined(STREAM_HTTPS)
    if (!g_cfg.tls) {
        printf("[wt] config says plain TCP but this image is built for TLS\n");
        return false;
    }
#else
    if (g_cfg.tls) {
        printf("[wt] config asks for TLS but this image is built without it\n");
        return false;
    }
#endif

    wiz_NetInfo ni = {0};
    memcpy(ni.mac, g_cfg.mac, sizeof(ni.mac));
    memcpy(ni.ip,  g_cfg.ip,  sizeof(ni.ip));
    memcpy(ni.sn,  g_cfg.mask, sizeof(ni.sn));
    memcpy(ni.gw,  g_cfg.gw,  sizeof(ni.gw));
    memcpy(ni.dns, g_cfg.dns, sizeof(ni.dns));
#if _WIZCHIP_ > W5500
    static const uint8_t lla[16]  = {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x02, 0x08, 0xdc, 0xff, 0xfe, 0x57, 0x57, 0x26};
    static const uint8_t sn6[16]  = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t dns6[16] = {0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x88};
    memcpy(ni.lla,  lla,  sizeof(ni.lla));
    memcpy(ni.sn6,  sn6,  sizeof(ni.sn6));
    memcpy(ni.dns6, dns6, sizeof(ni.dns6));
    ni.ipmode = NETINFO_STATIC_ALL;
#else
    ni.dhcp = NETINFO_STATIC;
#endif

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    network_initialize(ni);
    print_network_information(ni);
#if defined(STREAM_HTTPS)
    if (!tls_server_init()) {
        printf("[tls] NOT READY - every connection will be refused\n");
    }
#endif

    conn_init(&g_conn);
    memset(g_live, 0, sizeof(g_live));
    return true;
}

void wt_service(void)
{
    for (wt_conn_t i = 0; i < WT_MAX_CONN; i++) {
        uint8_t  held  = wt_is_held(i);
        uint8_t  phase = (uint8_t)conn_phase(&g_conn, i);
        uint64_t t0    = time_us_64();
        socket_run(i);
        uint32_t d = (uint32_t)(time_us_64() - t0);
        if (d > g_slow_us) {
            g_slow_us = d; g_slow_sn = sn_of(i); g_slow_held = held; g_slow_phase = phase;
        }
        listen_sample();
        if (d >= SLOW_LOG_MS * 1000u) {
            g_slow_total++;
            printf("[slow] s%u %s phase %u: %lu ms\n", sn_of(i),
                   held ? "stream" : "http", phase, (unsigned long)(d / 1000u));
        }
    }
}

#endif /* NET_STACK_TOE */
