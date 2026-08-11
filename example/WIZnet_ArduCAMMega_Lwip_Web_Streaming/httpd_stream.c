/**
 * @file    httpd_stream.c
 * @brief   HTTP + MJPEG server on the lwIP raw TCP API.
 *
 * Endpoints match the TOE example exactly:
 *
 *   GET /              single-page UI (see web_page.h)
 *   GET /stream        multipart/x-mixed-replace MJPEG stream
 *   GET /api/start     begin streaming     -> status JSON
 *   GET /api/stop      stop streaming      -> status JSON
 *   GET /api/res?v=WxH change resolution   -> status JSON
 *   GET /api/clk?div=&pll=  sensor clock sweep -> status JSON
 *   GET /api/reset     recover the sensor  -> status JSON
 *   GET /api/status    current state       -> status JSON
 *
 * Sending a frame is a state machine rather than a loop: lwIP hands back
 * ERR_MEM when its send buffer is full, and the space only frees up once ACKs
 * are processed back in the main loop. So a large JPEG is written across
 * several iterations, tracked in s_tx.
 */

#include <stdio.h>
#include <string.h>

#include "httpd_stream.h"
#include "web_page.h"
#include "logo_png.h"

#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"

#include "pico/stdlib.h"

/* --------------------------------- Config --------------------------------- */
#define HTTP_PORT       80
#define TCP_CHUNK_MAX   1400            /* stay inside one MSS per write */
#define STACK_NAME      "lwIP"

#define MJPEG_BOUNDARY  "wiznetframe"

/* ---------------------------- External (ArduCAM) --------------------------- */
extern uint8_t image_buff[];

/* -------------------------------- Variables ------------------------------- */
volatile bool  g_streaming  = false;
volatile res_t g_resolution = RES_320X240;

static uint32_t g_frame_count = 0;
static uint32_t g_drop_count  = 0;
static uint32_t g_fail_streak = 0;

/* Live metrics, averaged over a one-second window - same fields as the TOE
 * example so the two pages can be compared directly. */
static uint32_t g_fps_x10  = 0;
static uint32_t g_vsync_ms = 0;
static uint32_t g_read_ms  = 0;
static uint32_t g_send_ms  = 0;
static uint32_t g_frame_kb = 0;

static uint32_t        g_win_frames   = 0;
static uint64_t        g_win_vsync_us = 0;
static uint64_t        g_win_read_us  = 0;
static uint64_t        g_win_send_us  = 0;
static uint64_t        g_win_bytes    = 0;
static absolute_time_t g_win_start;

static struct tcp_pcb *s_listen_pcb = NULL;
static struct tcp_pcb *s_stream_pcb = NULL;   /* connection serving /stream */

static char g_tx_hdr[256];

/*
 * Supplied by main.c: drain the chip's receive buffer into lwIP and run its
 * timers. Called while waiting for send-buffer space so that ACKs keep being
 * processed during a large frame - otherwise the window never reopens and the
 * connection stalls until a retransmit timeout.
 */
static void (*s_net_poll)(void) = NULL;

/* How many times to service the network before giving up on this iteration. */
#define SNDBUF_WAIT_ROUNDS  64

/**
 * Frame transmission progress, spread over several main-loop iterations.
 *
 * TX_DRAINING exists because the JPEG is written without TCP_WRITE_FLAG_COPY:
 * lwIP keeps a pointer into image_buff for retransmission rather than a copy.
 * Capturing the next frame as soon as the last byte is *queued* would overwrite
 * data lwIP may still need to resend, and the peer would receive a JPEG spliced
 * from two frames - which the decoder throws away, and the view blinks.
 *
 * Copying instead is not an option: a 100 KB frame does not fit in the lwIP
 * heap. So the capture waits until everything has been acknowledged. That costs
 * frame rate, and buys a picture that does not flicker.
 */
typedef enum { TX_IDLE, TX_SENDING, TX_DRAINING } tx_state_t;

static struct {
    tx_state_t state;
    uint32_t   offset;      /* JPEG bytes already handed to lwIP */
    uint32_t   size;        /* total JPEG size */
    uint32_t   unacked;     /* bytes lwIP still references in image_buff */
    absolute_time_t started;
    absolute_time_t drain_start;
} s_tx = { .state = TX_IDLE };

/* Give up on a drain that never completes; the peer is gone. */
#define DRAIN_TIMEOUT_MS    3000

/* --------------------------- Function Prototypes --------------------------- */
static void metrics_reset(void);
static void metrics_update(void);
static void close_conn(struct tcp_pcb *pcb);
static void resp_pump(void);

/* ------------------------------- Resolution -------------------------------- */
static const char *res_to_string(res_t res)
{
    switch (res) {
    case RES_320X240:   return "320x240";
    case RES_640X480:   return "640x480";
    case RES_1280X720:  return "1280x720";
    case RES_1600X1200: return "1600x1200";
    case RES_1920X1080: return "1920x1080";
    default:            return "unknown";
    }
}

static bool string_to_res(const char *s, size_t len, res_t *out)
{
    struct { const char *name; res_t res; } table[] = {
        { "320x240",   RES_320X240   },
        { "640x480",   RES_640X480   },
        { "1280x720",  RES_1280X720  },
        { "1600x1200", RES_1600X1200 },
        { "1920x1080", RES_1920X1080 },
    };
    size_t i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        size_t n = strlen(table[i].name);
        if (n == len && memcmp(s, table[i].name, n) == 0) {
            *out = table[i].res;
            return true;
        }
    }
    return false;
}

/** Pull an unsigned decimal out of a query string slice, e.g. "div=3&pll=1". */
static bool parse_query_u32(const char *q, size_t qlen, const char *key, uint32_t *out)
{
    size_t klen = strlen(key);
    size_t i;

    for (i = 0; i + klen + 1 <= qlen; i++) {
        if ((i == 0 || q[i - 1] == '&') &&
            memcmp(q + i, key, klen) == 0 && q[i + klen] == '=') {
            size_t   p     = i + klen + 1;
            uint32_t value = 0;
            bool     digit = false;

            while (p < qlen && q[p] >= '0' && q[p] <= '9') {
                value = value * 10u + (uint32_t)(q[p] - '0');
                digit = true;
                p++;
                if (value > 0xFFFFu) {
                    return false;
                }
            }
            if (!digit) {
                return false;
            }
            *out = value;
            return true;
        }
    }
    return false;
}

static void apply_resolution(res_t res)
{
    if (res == g_resolution) {
        return;
    }
    g_resolution = res;
    arducam_mega.set_frame_size(res);
    sleep_ms(200);
    metrics_reset();
    printf("Resolution changed to %s\n", res_to_string(res));
}

/* -------------------------------- Metrics ---------------------------------- */
static void metrics_reset(void)
{
    g_win_frames   = 0;
    g_win_vsync_us = 0;
    g_win_read_us  = 0;
    g_win_send_us  = 0;
    g_win_bytes    = 0;
    g_win_start    = get_absolute_time();
}

static void metrics_update(void)
{
    int64_t elapsed_us = absolute_time_diff_us(g_win_start, get_absolute_time());

    if (elapsed_us < 1000000) {
        return;
    }

    if (g_win_frames) {
        g_fps_x10  = (uint32_t)(((uint64_t)g_win_frames * 10000000ULL) /
                                (uint64_t)elapsed_us);
        g_vsync_ms = (uint32_t)(g_win_vsync_us / g_win_frames / 1000ULL);
        g_read_ms  = (uint32_t)(g_win_read_us  / g_win_frames / 1000ULL);
        g_send_ms  = (uint32_t)(g_win_send_us  / g_win_frames / 1000ULL);
        g_frame_kb = (uint32_t)(g_win_bytes    / g_win_frames / 1024ULL);
    } else {
        g_fps_x10 = 0;
    }

    g_win_frames   = 0;
    g_win_vsync_us = 0;
    g_win_read_us  = 0;
    g_win_send_us  = 0;
    g_win_bytes    = 0;
    g_win_start    = get_absolute_time();
}

/* ------------------------------ TCP helpers -------------------------------- */
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

/*
 * Pending response.
 *
 * The UI page is around 10 KB and TCP_SND_BUF is 8 segments, so a response does
 * not necessarily fit in the send buffer at the moment the request arrives.
 * Writing what fits and dropping the rest would truncate the page, so the
 * remainder is drained from httpd_stream_poll() as ACKs free space.
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
            s_resp.idx++;
            s_resp.off = 0;
            continue;
        }

        avail = tcp_sndbuf(s_resp.pcb);
        if (avail == 0) {
            break;                      /* wait for ACKs */
        }

        chunk = (remain > (uint32_t)avail) ? avail : (uint16_t)remain;
        if (chunk > TCP_CHUNK_MAX) {
            chunk = TCP_CHUNK_MAX;
        }

        /* The body is static; the header lives in s_resp_hdr until finished. */
        if (tcp_write(s_resp.pcb, s_resp.seg[s_resp.idx] + s_resp.off,
                      chunk, TCP_WRITE_FLAG_MORE) != ERR_OK) {
            break;                      /* ERR_MEM - retry next iteration */
        }
        s_resp.off += chunk;
    }

    tcp_output(s_resp.pcb);

    if (s_resp.idx >= 2) {
        struct tcp_pcb *pcb   = s_resp.pcb;
        bool            close = s_resp.close_when_done;

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
static void send_response(struct tcp_pcb *pcb, const char *status,
                          const char *content_type, const char *cache,
                          const void *body, uint32_t body_len)
{
    int n = snprintf(s_resp_hdr, sizeof(s_resp_hdr),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Cache-Control: %s\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, content_type, (unsigned long)body_len, cache);
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
    s_resp.close_when_done = true;

    resp_pump();
}

static void send_simple_response(struct tcp_pcb *pcb, const char *status,
                                 const char *content_type,
                                 const char *body, uint32_t body_len)
{
    send_response(pcb, status, content_type, "no-store", body, body_len);
}

static void send_status_json(struct tcp_pcb *pcb)
{
    char body[256];
    int  n = snprintf(body, sizeof(body),
                      "{\"streaming\":%s,\"res\":\"%s\","
                      "\"frames\":%lu,\"dropped\":%lu,"
                      "\"fps\":%lu.%lu,\"vsync_ms\":%lu,\"read_ms\":%lu,"
                      "\"send_ms\":%lu,\"kb\":%lu,"
                      "\"clk_div\":%u,\"pll_div\":%u,\"stack\":\"" STACK_NAME "\"}",
                      g_streaming ? "true" : "false",
                      res_to_string(g_resolution),
                      (unsigned long)g_frame_count,
                      (unsigned long)g_drop_count,
                      (unsigned long)(g_fps_x10 / 10),
                      (unsigned long)(g_fps_x10 % 10),
                      (unsigned long)g_vsync_ms,
                      (unsigned long)g_read_ms,
                      (unsigned long)g_send_ms,
                      (unsigned long)g_frame_kb,
                      (unsigned)arducam_clk_div,
                      (unsigned)arducam_pll_div);
    if (n > 0) {
        send_simple_response(pcb, "200 OK", "application/json", body, (uint32_t)n);
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
     * whenever the frame size changes, and the previous connection is often
     * still half-open at that moment.
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

    /* Small enough to always fit; no deferred send needed. */
    tcp_write(pcb, stream_hdr, (u16_t)(sizeof(stream_hdr) - 1),
              TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);

    s_stream_pcb = pcb;
    reset_tx();
    printf("Stream opened\n");
}

/* --------------------------------- Routing --------------------------------- */
static void http_route(struct tcp_pcb *pcb, const char *path, size_t path_len)
{
    if ((path_len == 1 && path[0] == '/') ||
        (path_len == 11 && memcmp(path, "/index.html", 11) == 0)) {
        send_simple_response(pcb, "200 OK", "text/html; charset=utf-8",
                             HTTP_INDEX_PAGE,
                             (uint32_t)(sizeof(HTTP_INDEX_PAGE) - 1));
        return;
    }

    /* The one asset the page pulls in, and it never changes - let it cache. */
    if (path_len == 9 && memcmp(path, "/logo.png", 9) == 0) {
        send_response(pcb, "200 OK", "image/png", "max-age=86400",
                      LOGO_PNG, (uint32_t)sizeof(LOGO_PNG));
        return;
    }

    if (path_len >= 7 && memcmp(path, "/stream", 7) == 0 &&
        (path_len == 7 || path[7] == '?')) {
        begin_stream(pcb);
        return;
    }

    if (path_len == 10 && memcmp(path, "/api/start", 10) == 0) {
        g_streaming   = true;
        g_frame_count = 0;
        g_drop_count  = 0;
        metrics_reset();
        printf("Streaming STARTED\n");
        send_status_json(pcb);
        return;
    }

    if (path_len == 9 && memcmp(path, "/api/stop", 9) == 0) {
        g_streaming = false;
        printf("Streaming STOPPED (frames=%lu, dropped=%lu)\n",
               (unsigned long)g_frame_count, (unsigned long)g_drop_count);
        send_status_json(pcb);
        return;
    }

    if (path_len == 11 && memcmp(path, "/api/status", 11) == 0) {
        send_status_json(pcb);
        return;
    }

    if (path_len == 10 && memcmp(path, "/api/reset", 10) == 0) {
        arducam_recover();
        g_fail_streak = 0;
        metrics_reset();
        send_status_json(pcb);
        return;
    }

    if (path_len > 11 && memcmp(path, "/api/res?v=", 11) == 0) {
        res_t res;
        if (string_to_res(path + 11, path_len - 11, &res)) {
            apply_resolution(res);
        }
        send_status_json(pcb);
        return;
    }

    if (path_len > 9 && memcmp(path, "/api/clk?", 9) == 0) {
        const char *q    = path + 9;
        size_t      qlen = path_len - 9;
        uint32_t    div, pll;

        if (parse_query_u32(q, qlen, "div", &div) &&
            parse_query_u32(q, qlen, "pll", &pll) &&
            div <= 0xFF && pll <= 0xFF) {
            arducam_set_clock_div((uint8_t)div, (uint8_t)pll);
            metrics_reset();
        }
        send_status_json(pcb);
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

    /*
     * Track how much of image_buff lwIP no longer needs. Only once this
     * reaches zero is the buffer free to be captured into again.
     */
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
    was_stream = (s_stream_pcb == pcb);
    if (!was_stream && s_resp.pcb != pcb) {
        close_conn(pcb);
    }

    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    (void)arg;
    printf("TCP error: %d\n", (int)err);
    /* lwIP has already freed the pcb. */
    s_stream_pcb = NULL;
    reset_tx();
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !newpcb) {
        return ERR_VAL;
    }

    tcp_setprio(newpcb, TCP_PRIO_MIN);
    tcp_recv(newpcb, tcp_recv_cb);
    tcp_sent(newpcb, tcp_sent_cb);
    tcp_err (newpcb, tcp_err_cb);

    return ERR_OK;
}

/* ------------------------------- Public API -------------------------------- */
void httpd_stream_init(void)
{
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
    metrics_reset();
    printf("HTTP camera server listening on port %d\n", HTTP_PORT);
}

void httpd_stream_set_net_poll(void (*fn)(void))
{
    s_net_poll = fn;
}

void httpd_stream_poll(void)
{
    /* Finish any response still draining before touching the camera. */
    resp_pump();

    if (!s_stream_pcb) {
        return;
    }

    /*
     * Waiting for the peer to acknowledge the frame just sent. Until it does,
     * lwIP still points into image_buff and capturing would corrupt what it
     * may need to retransmit.
     */
    if (s_tx.state == TX_DRAINING) {
        if (s_tx.unacked == 0 ||
            absolute_time_diff_us(s_tx.drain_start, get_absolute_time())
                > (int64_t)DRAIN_TIMEOUT_MS * 1000) {
            reset_tx();
        }
        metrics_update();
        return;
    }

    /* Idle and not streaming: hold the connection open, send nothing. */
    if (s_tx.state == TX_IDLE && !g_streaming) {
        metrics_update();
        return;
    }

    /* ---- Capture a frame and queue its part header ---- */
    if (s_tx.state == TX_IDLE) {
        uint32_t jpeg_size;
        int      n;

        if (arducam_mega.get_frame() != 0) {
            g_drop_count++;
            if (++g_fail_streak >= 10) {
                printf("No frames for %u attempts - recovering sensor\n",
                       (unsigned)g_fail_streak);
                arducam_recover();
                g_fail_streak = 0;
                metrics_reset();
            }
            return;
        }
        g_fail_streak = 0;

        jpeg_size = arducam_mega.frame.frame_length;
        if (jpeg_size < 2 || image_buff[0] != 0xFF || image_buff[1] != 0xD8) {
            g_drop_count++;
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
             * The send window is full. Returning here would mean nothing runs
             * the stack until the next main-loop pass, and the capture at the
             * top of that pass blocks for tens of milliseconds - long enough
             * for the peer to give up. Pump the network in place instead.
             */
            if (!s_net_poll || ++waits > SNDBUF_WAIT_ROUNDS) {
                break;
            }
            tcp_output(s_stream_pcb);
            s_net_poll();
            if (!s_stream_pcb) {
                return;                  /* peer went away while we waited */
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

        if (tcp_write(s_stream_pcb, image_buff + s_tx.offset, chunk, flags) != ERR_OK) {
            break;                       /* ERR_MEM - retry next iteration */
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

        g_frame_count++;
        g_win_frames++;
        g_win_vsync_us += arducam_vsync_wait_us;
        g_win_read_us  += arducam_readout_us;
        g_win_send_us  += (uint64_t)absolute_time_diff_us(s_tx.started,
                                                          get_absolute_time());
        g_win_bytes    += s_tx.size;
        metrics_update();
    }
}
