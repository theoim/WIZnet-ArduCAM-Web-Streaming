/**
 * @file    server_lwip.c
 * @brief   HTTP + MJPEG server on the lwIP raw TCP API, over MACRAW.
 *
 * Built only when NET_STACK_LWIP is defined.
 *
 * The chip is put in MACRAW mode and hands raw Ethernet frames to lwIP, which
 * terminates TCP on the MCU. Same page, same endpoints, same camera driver as
 * the TOE image - the difference is entirely underneath, which is the point of
 * running the two boards side by side.
 *
 * What makes this side hard, and why the code is shaped the way it is:
 * arducam_capture_frame() blocks for 22-120 ms. With a hardwired stack that is
 * fine, because the chip keeps acknowledging while the MCU is busy. Here the
 * MCU *is* the stack, so for that whole window nothing is acknowledged and no
 * retransmit timer runs. Three consequences, all of them visible below - the
 * receive path is drained rather than sampled, the stack is serviced in place
 * while waiting for send-window space, and the next capture waits until the
 * last frame has been acknowledged.
 *
 * The load generator makes all three of those cost something measurable, which
 * is exactly what it is for.
 */

#include "exhibition_config.h"
#if defined(NET_STACK_LWIP)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"
#include "w5x00_lwip.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/stats.h"
/* tcp_active_pcbs / tcp_tw_pcbs live here. Reaching into lwIP's internals is
   normally a bad idea; it is worth it for a diagnostic that answers "how many
   connections is this thing actually holding" without inference. */
#include "lwip/priv/tcp_priv.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "net_server.h"
#include "cam_state.h"
#include "cam_controls.h"
#include "net_load.h"
#include "arducam_mega.h"
#include "web_page.h"
#include "logo_png.h"

#define SOCKET_MACRAW       0
#define TCP_CHUNK_MAX       1400        /* stay inside one MSS per write */
#define SNDBUF_WAIT_ROUNDS  64
#define DRAIN_TIMEOUT_MS    3000

extern uint8_t image_buff[];

/* -------------------------------- Variables ------------------------------- */
static uint8_t   g_mac[6] = NET_MAC_ADDR;
static ip_addr_t g_ip;
static ip_addr_t g_mask;
static ip_addr_t g_gateway;

struct netif g_netif;

static uint8_t *g_frame_buf;

static struct tcp_pcb *s_listen_pcb = NULL;
static struct tcp_pcb *s_stream_pcb = NULL;

static char g_tx_hdr[256];
static char g_json[CAM_STATUS_JSON_MAX];

static void net_service(void);
static void close_conn(struct tcp_pcb *pcb);
static void resp_pump(void);

/* ------------------------------ Frame transmit ----------------------------- */
/*
 * TX_DRAINING exists because the JPEG is written without TCP_WRITE_FLAG_COPY:
 * lwIP keeps a pointer into image_buff for retransmission rather than a copy.
 * Capturing the next frame as soon as the last byte is *queued* would overwrite
 * data lwIP may still need to resend, and the peer would get a JPEG spliced
 * from two frames - which the decoder throws away, so the view blinks.
 *
 * Copying instead is not an option: a 100 KB frame does not fit in the lwIP
 * heap. So the capture waits until everything has been acknowledged. That costs
 * frame rate and buys a picture that does not flicker.
 */
typedef enum { TX_IDLE, TX_SENDING, TX_DRAINING } tx_state_t;

static struct {
    tx_state_t      state;
    uint32_t        offset;
    uint32_t        size;
    uint32_t        unacked;
    absolute_time_t started;
    absolute_time_t drain_start;
} s_tx = { .state = TX_IDLE };

static void reset_tx(void)
{
    s_tx.state   = TX_IDLE;
    s_tx.offset  = 0;
    s_tx.size    = 0;
    s_tx.unacked = 0;
}

static void release_stream(struct tcp_pcb *pcb)
{
    if (s_stream_pcb == pcb) {
        s_stream_pcb = NULL;
        reset_tx();
    }
}

/* ----------------------------- Pending response ---------------------------- */
/*
 * The page does not fit in the send buffer at the moment the request arrives,
 * so the remainder is drained from stream_poll() as ACKs free space. Writing
 * what fits and dropping the rest would truncate it.
 *
 * THIS IS THE SHAPE FROM examples/WIZnet_ArduCAMMega_Lwip_Web_Streaming, and
 * it is deliberately unchanged. A four-slot version, a per-slot header buffer
 * and TCP_WRITE_FLAG_COPY on the body were all tried here first, on the
 * reasoning that a browser opens several connections at once and one slot must
 * therefore be losing responses. On hardware the result was a device that
 * received every /api/status and answered none of them, while the stock
 * example on the same board answered all of them. Predicted problems, real
 * regression. Reverted.
 *
 * The one addition kept is `reps`, and only because the load generator cannot
 * work without it: a 128 KB response is one 1 KB block written 128 times over
 * rather than 128 KB of storage nobody has.
 *
 * Two segments: the generated header, then the body. The body is always static
 * storage, so only the header needs its own buffer.
 */
static struct {
    struct tcp_pcb *pcb;
    const uint8_t  *seg[2];
    uint32_t        len[2];
    uint32_t        off;                /* bytes written of the current segment */
    uint8_t         idx;                /* segment being written; 2 = finished  */
    uint32_t        reps;               /* extra times to repeat segment 1      */
    bool            is_load;            /* account the bytes as link load       */
    bool            close_when_done;
} s_resp;

static char s_resp_hdr[256];

static void resp_pump(void)
{
    if (!s_resp.pcb || s_resp.idx >= 2) {
        return;
    }

    while (s_resp.idx < 2) {
        uint32_t remain = s_resp.len[s_resp.idx] - s_resp.off;
        uint16_t avail;
        uint16_t chunk;

        if (remain == 0) {
            if (s_resp.idx == 1 && s_resp.reps > 0) {
                s_resp.reps--;          /* same block again */
                s_resp.off = 0;
                continue;
            }
            s_resp.idx++;
            s_resp.off = 0;
            continue;
        }

        avail = tcp_sndbuf(s_resp.pcb);
        if (avail == 0) {
#if EXHIBITION_HTTP_LOG
            printf("[resp] sndbuf 0 at idx=%u off=%lu\n",
                   s_resp.idx, (unsigned long)s_resp.off);
#endif
            break;                      /* wait for ACKs */
        }

        chunk = (remain > (uint32_t)avail) ? avail : (uint16_t)remain;
        if (chunk > TCP_CHUNK_MAX) {
            chunk = TCP_CHUNK_MAX;
        }

        {
            err_t we = tcp_write(s_resp.pcb, s_resp.seg[s_resp.idx] + s_resp.off,
                                 chunk, TCP_WRITE_FLAG_MORE);
            if (we != ERR_OK) {
#if EXHIBITION_HTTP_LOG
                printf("[resp] tcp_write %d at idx=%u off=%lu chunk=%u\n",
                       (int)we, s_resp.idx, (unsigned long)s_resp.off, chunk);
#endif
                break;                  /* ERR_MEM - retry next iteration */
            }
        }
        s_resp.off += chunk;

        /* Counted as it goes, so a load cut off halfway still reports the half
           that was actually carried. */
        if (s_resp.is_load && s_resp.idx == 1) {
            net_load_account(chunk);
        }
    }

    tcp_output(s_resp.pcb);

    if (s_resp.idx >= 2) {
        struct tcp_pcb *pcb   = s_resp.pcb;
        bool            close = s_resp.close_when_done;

#if EXHIBITION_HTTP_LOG
        printf("[resp] done, close=%d\n", (int)close);
#endif
        s_resp.pcb = NULL;
        if (close) {
            close_conn(pcb);
        }
    }
}

/**
 * Queue a response. @p cache is the Cache-Control value: status data must never
 * be cached, the logo should be.
 */
static void send_response_ex(struct tcp_pcb *pcb, const char *status,
                             const char *content_type, const char *cache,
                             const void *body, uint32_t body_len,
                             uint32_t reps, bool is_load)
{
    uint32_t total = body_len * (reps + 1u);
    int      n;

#if EXHIBITION_HTTP_LOG
    if (s_resp.pcb && s_resp.idx < 2) {
        /* The single-slot hazard, made visible instead of assumed: a response
           still draining is about to be thrown away by this one. */
        printf("[resp] CLOBBER idx=%u off=%lu remaining\n",
               s_resp.idx, (unsigned long)s_resp.off);
    }
#endif

    n = snprintf(s_resp_hdr, sizeof(s_resp_hdr),
                              "HTTP/1.1 %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %lu\r\n"
                              "Cache-Control: %s\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              status, content_type,
                              (unsigned long)total, cache);
    if (n <= 0) {
        return;
    }

    s_resp.pcb             = pcb;
    s_resp.seg[0]          = (const uint8_t *)s_resp_hdr;
    s_resp.len[0]          = (uint32_t)n;
    s_resp.seg[1]          = (const uint8_t *)body;
    s_resp.len[1]          = body_len;
    s_resp.off             = 0;
    s_resp.idx             = 0;
    s_resp.reps            = reps;
    s_resp.is_load         = is_load;
    s_resp.close_when_done = true;

#if EXHIBITION_HTTP_LOG
    printf("[resp] queued hdr=%lu body=%lu reps=%lu\n",
           (unsigned long)s_resp.len[0], (unsigned long)s_resp.len[1],
           (unsigned long)s_resp.reps);
#endif

    resp_pump();
}

static void send_simple_response(struct tcp_pcb *pcb, const char *status,
                                 const char *content_type,
                                 const char *body, uint32_t body_len)
{
    send_response_ex(pcb, status, content_type, "no-store",
                     body, body_len, 0, false);
}

/** Status document, optionally as 409 so the page can say a value was clamped. */
static void send_status(struct tcp_pcb *pcb, bool refused)
{
    int n = cam_state_status_json(g_json, sizeof(g_json));
    if (n > 0) {
        send_simple_response(pcb, refused ? "409 Conflict" : "200 OK",
                             "application/json", g_json, (uint32_t)n);
    }
}

/* --------------------------------- Stream ---------------------------------- */
static void begin_stream(struct tcp_pcb *pcb)
{
    static const char stream_hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
        "Cache-Control: no-store\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    /*
     * Hand the camera to the newest request. The page reopens the stream
     * whenever the frame size changes or the link comes back, and the previous
     * connection is often still half-open at that moment.
     */
    if (s_stream_pcb && s_stream_pcb != pcb) {
        struct tcp_pcb *old = s_stream_pcb;
        s_stream_pcb = NULL;
        reset_tx();
        tcp_recv(old, NULL);
        tcp_sent(old, NULL);
        tcp_err (old, NULL);
        tcp_close(old);
        printf("Stream taken over\n");
    }

    tcp_write(pcb, stream_hdr, (u16_t)(sizeof(stream_hdr) - 1),
              TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);

    s_stream_pcb = pcb;
    reset_tx();

    /*
     * Raise this one above the short-lived request connections.
     *
     * tcp_accept_cb() puts every accepted connection at TCP_PRIO_MIN, so when
     * lwIP runs out of pcbs - a pool of five, and a status poll is a whole
     * connection - tcp_kill_prio() is free to abort any of them, and the one it
     * finds is the oldest: the stream. The serial log said so directly once the
     * error callback started naming things, as ERR_ABRT (-13) arriving out of
     * nowhere while the video was running.
     *
     * The stream is the long-lived connection and the one a visitor is looking
     * at; the request connections are the disposable ones. NORMAL is enough,
     * because an incoming connection is allocated at the listener's priority
     * (also NORMAL) and tcp_kill_prio() only takes pcbs strictly below it - so
     * the pool is still reclaimable, just not from the video.
     */
    tcp_setprio(pcb, TCP_PRIO_NORMAL);

    printf("Stream opened\n");
}

/* --------------------------------- Routing --------------------------------- */
static bool path_is(const char *path, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    return len == n && memcmp(path, lit, n) == 0;
}

static bool path_starts(const char *path, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    return len > n && memcmp(path, lit, n) == 0;
}

static void http_route(struct tcp_pcb *pcb, const char *path, size_t path_len)
{
#if EXHIBITION_HTTP_LOG
    printf("[req] %.*s\n", (int)path_len, path);
#endif

    if (path_is(path, path_len, "/") || path_is(path, path_len, "/index.html")) {
        send_simple_response(pcb, "200 OK", "text/html; charset=utf-8",
                             HTTP_INDEX_PAGE,
                             (uint32_t)(sizeof(HTTP_INDEX_PAGE) - 1));
        return;
    }

    /* The one asset the page pulls in, and it never changes - let it cache. */
    if (path_is(path, path_len, "/logo.png")) {
        send_response_ex(pcb, "200 OK", "image/png", "max-age=86400",
                         LOGO_PNG, (uint32_t)sizeof(LOGO_PNG), 0, false);
        return;
    }

    if (path_len >= 7 && memcmp(path, "/stream", 7) == 0 &&
        (path_len == 7 || path[7] == '?')) {
        begin_stream(pcb);
        return;
    }

    if (path_starts(path, path_len, "/load?")) {
        uint32_t kb = net_load_parse_kb(path + 6, path_len - 6);
        if (kb == 0) {
            static const char none[] = "load=0";
            send_simple_response(pcb, "200 OK", "text/plain", none,
                                 (uint32_t)(sizeof(none) - 1));
        } else {
            send_response_ex(pcb, "200 OK", "application/octet-stream",
                             "no-store", net_load_chunk(), LOAD_CHUNK_SIZE,
                             kb - 1u, true);
        }
        return;
    }

    if (path_is(path, path_len, "/api/start")) {
        cam_state_set_streaming(true);
        printf("Streaming STARTED\n");
        send_status(pcb, false);
        return;
    }

    if (path_is(path, path_len, "/api/stop")) {
        cam_state_set_streaming(false);
        printf("Streaming STOPPED\n");
        send_status(pcb, false);
        return;
    }

    if (path_is(path, path_len, "/api/status")) {
        send_status(pcb, false);
        return;
    }

    if (path_is(path, path_len, "/api/reset")) {
        cam_state_recover();
        send_status(pcb, false);
        return;
    }

    if (path_is(path, path_len, "/api/controls")) {
        int n = cam_controls_json_descriptors(g_json, sizeof(g_json));
        if (n > 0) {
            send_simple_response(pcb, "200 OK", "application/json",
                                 g_json, (uint32_t)n);
        }
        return;
    }

    if (path_starts(path, path_len, "/api/cam?")) {
        bool refused = false;
        cam_controls_apply_query(path + 9, path_len - 9, &refused);
        send_status(pcb, refused);
        return;
    }

    if (path_starts(path, path_len, "/api/res?v=")) {
        res_t res;
        if (cam_state_res_parse(path + 11, path_len - 11, &res)) {
            cam_state_apply_resolution(res);
        }
        send_status(pcb, false);
        return;
    }

    if (path_starts(path, path_len, "/api/clk?")) {
        const char *q    = path + 9;
        size_t      qlen = path_len - 9;
        uint32_t    div  = 0, pll = 0;
        bool        got_div = false, got_pll = false;
        size_t      i;

        for (i = 0; i + 4 <= qlen; i++) {
            if ((i == 0 || q[i - 1] == '&') && memcmp(q + i, "div=", 4) == 0) {
                size_t p = i + 4; div = 0; got_div = false;
                while (p < qlen && q[p] >= '0' && q[p] <= '9') {
                    div = div * 10u + (uint32_t)(q[p++] - '0'); got_div = true;
                }
            } else if ((i == 0 || q[i - 1] == '&') &&
                       memcmp(q + i, "pll=", 4) == 0) {
                size_t p = i + 4; pll = 0; got_pll = false;
                while (p < qlen && q[p] >= '0' && q[p] <= '9') {
                    pll = pll * 10u + (uint32_t)(q[p++] - '0'); got_pll = true;
                }
            }
        }
        if (got_div && got_pll && div <= 0xFF && pll <= 0xFF) {
            arducam_set_clock_div((uint8_t)div, (uint8_t)pll);
            cam_controls_apply_all();   /* the sensor was re-initialised */
            cam_state_metrics_reset();
        }
        send_status(pcb, false);
        return;
    }

    {
        static const char nf[] = "Not Found";
        send_simple_response(pcb, "404 Not Found", "text/plain",
                             nf, (uint32_t)(sizeof(nf) - 1));
    }
}

/* -------------------------------- Callbacks -------------------------------- */
static void close_conn(struct tcp_pcb *pcb)
{
    release_stream(pcb);
    if (s_resp.pcb == pcb) {
        s_resp.pcb = NULL;
    }
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err (pcb, NULL);
    tcp_close(pcb);
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg;

    /* Track how much of image_buff lwIP no longer needs. Only once this reaches
       zero is the buffer free to be captured into again. */
    if (pcb == s_stream_pcb) {
        s_tx.unacked = (s_tx.unacked > len) ? (s_tx.unacked - len) : 0;
    }
    return ERR_OK;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    char   req[256];
    u16_t  copy_len;
    bool   was_stream;

    (void)arg;

    if (!p) {                            /* peer closed */
        close_conn(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    copy_len = (p->tot_len < sizeof(req) - 1) ? p->tot_len
                                              : (u16_t)(sizeof(req) - 1);
    pbuf_copy_partial(p, req, copy_len, 0);
    req[copy_len] = '\0';
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    /* Only GET is served; the request line is all this example needs. */
    if (memcmp(req, "GET ", 4) == 0) {
        const char *path = req + 4;
        const char *end  = strpbrk(path, " \r\n");
        size_t      len  = end ? (size_t)(end - path) : strlen(path);

        http_route(pcb, path, len);
    } else {
        static const char na[] = "Method Not Allowed";
        send_simple_response(pcb, "405 Method Not Allowed", "text/plain",
                             na, (uint32_t)(sizeof(na) - 1));
    }

    /*
     * Everything except the stream closes when its response has been written.
     * resp_pump() does that once the last byte is queued - closing here would
     * cut off a response that is still draining.
     */
    /*
     * Everything except the stream closes when its response has been written.
     * resp_pump() does that once the last byte is queued - closing here would
     * cut off a response that is still draining.
     */
    was_stream = (s_stream_pcb == pcb);
    if (!was_stream && s_resp.pcb != pcb) {
        close_conn(pcb);
    }

    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    /*
     * arg is the pcb this connection was accepted on - registered by
     * tcp_arg() below purely so this callback can tell WHICH connection died.
     * lwIP has already freed it, so the pointer is an identity and must not be
     * dereferenced.
     *
     * It used to clear s_stream_pcb unconditionally, which meant any failing
     * connection killed the video. That was survivable while errors were rare;
     * once the page started aborting requests that pass their deadline it was
     * not, because every abort arrives here as an error. The stream was being
     * torn down by the status poll giving up on itself, and the response slot
     * that connection held was never given back.
     */
    struct tcp_pcb *pcb = (struct tcp_pcb *)arg;

    printf("TCP error: %d\n", (int)err);

    if (pcb) {
        release_stream(pcb);
        if (s_resp.pcb == pcb) {
            s_resp.pcb = NULL;
        }
    } else {
        s_stream_pcb = NULL;
        reset_tx();
    }
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !newpcb) {
        return ERR_VAL;
    }

    tcp_setprio(newpcb, TCP_PRIO_MIN);
    /* Identity for tcp_err_cb - see the note there. */
    tcp_arg(newpcb, newpcb);
    tcp_recv(newpcb, tcp_recv_cb);
    tcp_sent(newpcb, tcp_sent_cb);
    tcp_err (newpcb, tcp_err_cb);

    return ERR_OK;
}

/* ------------------------------- Network pump ------------------------------ */
/**
 * Move everything the chip has received into lwIP, then run its timers.
 *
 * Drains rather than taking one frame per call: the capture that follows blocks
 * for tens of milliseconds, and anything still queued in the chip during that
 * window risks being overwritten. Left undrained, the lost segments come back
 * as retransmissions and the stream stutters.
 */
static void net_service(void)
{
    for (;;) {
        uint16_t     pack_len = 0;
        struct pbuf *p;

        getsockopt(SOCKET_MACRAW, SO_RECVBUF, &pack_len);
        if (pack_len == 0) {
            break;
        }

        pack_len = recv_lwip(SOCKET_MACRAW, g_frame_buf, pack_len);
        if (pack_len == 0) {
            break;
        }

        p = pbuf_alloc(PBUF_RAW, pack_len, PBUF_POOL);
        if (!p) {
            break;                       /* pool exhausted; drain again later */
        }

        pbuf_take(p, g_frame_buf, pack_len);
        if (g_netif.input(p, &g_netif) != ERR_OK) {
            pbuf_free(p);
        }
    }

    sys_check_timeouts();
}

/* ------------------------------ Stream pumping ----------------------------- */
static void stream_poll(void)
{
    /* Finish any response still draining before touching the camera. Under load
       this is where most of the work happens. */
    resp_pump();

    if (!s_stream_pcb) {
        cam_state_metrics_update();
        return;
    }

    /*
     * Waiting for the peer to acknowledge the frame just sent. Until it does,
     * lwIP still points into image_buff and capturing would corrupt what it may
     * need to retransmit.
     */
    if (s_tx.state == TX_DRAINING) {
        int64_t waited = absolute_time_diff_us(s_tx.drain_start,
                                               get_absolute_time());
        if (s_tx.unacked == 0 || waited > (int64_t)DRAIN_TIMEOUT_MS * 1000) {
            /* Charge the wait to the frame it belongs to, so the reported frame
               period adds up to the reported frame rate. */
            cam_state_note_drain((uint64_t)waited);
            reset_tx();
        }
        cam_state_metrics_update();
        return;
    }

    /* Idle and not streaming: hold the connection open, send nothing. */
    if (s_tx.state == TX_IDLE && !cam_state_streaming()) {
        cam_state_metrics_update();
        return;
    }

    /* ---- Capture a frame and queue its part header ---- */
    if (s_tx.state == TX_IDLE) {
        uint32_t jpeg_size;
        int      n;

        if (arducam_mega.get_frame() != 0) {
            if (cam_state_note_capture_fail()) {
                printf("No frames for %u attempts - sensor recovered\n",
                       FAIL_STREAK_RECOVER);
            }
            return;
        }
        cam_state_note_capture_ok();

        jpeg_size = arducam_mega.frame.frame_length;
        if (jpeg_size < 2 || image_buff[0] != 0xFF || image_buff[1] != 0xD8) {
            cam_state_note_dropped();
            return;
        }

        n = snprintf(g_tx_hdr, sizeof(g_tx_hdr),
                     "--" MJPEG_BOUNDARY "\r\n"
                     "Content-Type: image/jpeg\r\n"
                     "Content-Length: %lu\r\n"
                     "\r\n",
                     (unsigned long)jpeg_size);
        if (n <= 0) {
            return;
        }

        s_tx.started = get_absolute_time();

        if (tcp_sndbuf(s_stream_pcb) < (uint16_t)n) {
            return;                      /* try again next iteration */
        }
        if (tcp_write(s_stream_pcb, g_tx_hdr, (u16_t)n,
                      TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE) != ERR_OK) {
            return;
        }

        s_tx.size   = jpeg_size;
        s_tx.offset = 0;
        s_tx.state  = TX_SENDING;
    }

    /* ---- Push the JPEG, servicing the network whenever the window closes ---- */
    {
        uint32_t waits = 0;

        while (s_tx.offset < s_tx.size) {
            uint16_t avail = tcp_sndbuf(s_stream_pcb);
            uint32_t remain;
            uint16_t chunk;
            u8_t     flags;

            if (avail == 0) {
                /*
                 * The send window is full. Returning here would mean nothing
                 * runs the stack until the next main-loop pass, and the capture
                 * at the top of that pass blocks for tens of milliseconds -
                 * long enough for the peer to give up. Pump the network in
                 * place instead.
                 */
                if (++waits > SNDBUF_WAIT_ROUNDS) {
                    break;
                }
                tcp_output(s_stream_pcb);
                net_service();
                resp_pump();            /* the load shares this window */
                if (!s_stream_pcb) {
                    return;              /* peer went away while we waited */
                }
                continue;
            }
            waits = 0;

            remain = s_tx.size - s_tx.offset;
            chunk  = (remain > (uint32_t)avail) ? avail : (uint16_t)remain;
            if (chunk > TCP_CHUNK_MAX) {
                chunk = TCP_CHUNK_MAX;
            }

            /*
             * No copy: image_buff is static and stays valid until this frame is
             * fully queued, which avoids exhausting the lwIP heap on a 100 KB
             * frame. MORE keeps lwIP from flushing a partial segment early.
             */
            flags = (s_tx.offset + chunk < s_tx.size) ? TCP_WRITE_FLAG_MORE : 0;

            if (tcp_write(s_stream_pcb, image_buff + s_tx.offset,
                          chunk, flags) != ERR_OK) {
                break;                   /* ERR_MEM - retry next iteration */
            }
            s_tx.offset  += chunk;
            s_tx.unacked += chunk;
        }
    }

    tcp_output(s_stream_pcb);

    if (s_tx.offset >= s_tx.size) {
        tcp_write(s_stream_pcb, "\r\n", 2, TCP_WRITE_FLAG_COPY);
        tcp_output(s_stream_pcb);

        /* Queued, not yet acknowledged - hold image_buff until it is. */
        s_tx.state       = TX_DRAINING;
        s_tx.drain_start = get_absolute_time();

        cam_state_note_frame(
            (uint64_t)absolute_time_diff_us(s_tx.started, get_absolute_time()),
            s_tx.size);
        cam_state_metrics_update();
    }
}

/* --------------------------------- Public ---------------------------------- */
void net_server_init(void)
{
    /* clk_peri is retimed in main.c, before the camera is brought up - see the
       note there. Doing it here would move the SPI clock under a camera that
       was already initialised against the old one. */
    sleep_ms(3000);

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    setSHAR(g_mac);
    ctlwizchip(CW_RESET_PHY, 0);

    {
        static const uint8_t ip[4]   = NET_IP_ADDR;
        static const uint8_t mask[4] = NET_SUBNET_MASK;
        static const uint8_t gw[4]   = NET_GATEWAY;
        IP4_ADDR(&g_ip,      ip[0],   ip[1],   ip[2],   ip[3]);
        IP4_ADDR(&g_mask,    mask[0], mask[1], mask[2], mask[3]);
        IP4_ADDR(&g_gateway, gw[0],   gw[1],   gw[2],   gw[3]);
    }

    lwip_init();

    netif_add(&g_netif, &g_ip, &g_mask, &g_gateway,
              NULL, netif_initialize, netif_input);
    g_netif.name[0] = 'e';
    g_netif.name[1] = '0';

    netif_set_link_callback(&g_netif, netif_link_callback);
    netif_set_status_callback(&g_netif, netif_status_callback);

    /* MACRAW hands raw Ethernet frames to lwIP instead of terminating TCP. */
    if (socket(SOCKET_MACRAW, Sn_MR_MACRAW, 0, 0x00) < 0) {
        printf("[ERR] MACRAW socket open failed\n");
        return;
    }

    netif_set_link_up(&g_netif);
    netif_set_up(&g_netif);

    g_frame_buf = malloc(ETHERNET_MTU);
    if (!g_frame_buf) {
        printf("[ERR] frame buffer allocation failed\n");
        return;
    }

    s_listen_pcb = tcp_new();
    if (!s_listen_pcb) {
        printf("[ERR] tcp_new() failed\n");
        return;
    }
    if (tcp_bind(s_listen_pcb, IP_ADDR_ANY, HTTP_PORT) != ERR_OK) {
        printf("[ERR] tcp_bind() failed\n");
        tcp_close(s_listen_pcb);
        s_listen_pcb = NULL;
        return;
    }
    s_listen_pcb = tcp_listen(s_listen_pcb);
    if (!s_listen_pcb) {
        printf("[ERR] tcp_listen() failed\n");
        return;
    }
    tcp_accept(s_listen_pcb, tcp_accept_cb);

    cam_state_metrics_reset();

    printf("HTTP camera server ready (" STACK_NAME ", 1 listener)\n");
    printf("Open http://%s/ in a browser\n", ipaddr_ntoa(&g_ip));
}

#if EXHIBITION_LWIP_STATS
/**
 * Report what each lwIP pool is holding.
 *
 * `used` is now, `max` is the high-water mark since boot, `err` counts the
 * times an allocation from that pool failed. A pool whose `used` climbs and
 * never comes back down is a leak; one whose `err` climbs is simply too small.
 * The two need different fixes, and this line is what tells them apart.
 */
static void lwip_pool_report(void)
{
    static uint32_t last = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (now - last < 10000u) {
        return;
    }
    last = now;

    printf("[lwip] pcb %u/%u e%u  seg %u/%u e%u  pbuf %u/%u e%u  "
           "pool %u/%u e%u  mem %u/%u e%u\n",
           (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->used,
           (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->max,
           (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->err,
           (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->used,
           (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->max,
           (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->err,
           (unsigned)lwip_stats.memp[MEMP_PBUF]->used,
           (unsigned)lwip_stats.memp[MEMP_PBUF]->max,
           (unsigned)lwip_stats.memp[MEMP_PBUF]->err,
           (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->used,
           (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->max,
           (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->err,
           (unsigned)lwip_stats.mem.used,
           (unsigned)lwip_stats.mem.max,
           (unsigned)lwip_stats.mem.err);

    /* TIME_WAIT is the one that a per-poll connection habit fills up, and it is
       counted separately from the active list. */
    {
        struct tcp_pcb *p;
        unsigned tw = 0, act = 0;
        for (p = tcp_tw_pcbs; p != NULL; p = p->next) { tw++; }
        for (p = tcp_active_pcbs; p != NULL; p = p->next) { act++; }
        printf("[lwip] active %u  time_wait %u\n", act, tw);
    }
}
#endif

void net_server_service(void)
{
    net_service();
    stream_poll();
#if EXHIBITION_LWIP_STATS
    lwip_pool_report();
#endif
}

#endif /* NET_STACK_LWIP */
