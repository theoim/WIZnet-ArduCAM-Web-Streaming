/**
 * @file    server_toe.c
 * @brief   HTTP + MJPEG server on the WIZnet hardware TCP/IP offload engine.
 *
 * Built only when NET_STACK_TOE is defined.
 *
 * All HTTP_SOCK_COUNT hardware sockets listen on HTTP_PORT. That is not a
 * throughput choice: on this chip a listening socket *becomes* the connection
 * when a client arrives, so a single listener could hold the MJPEG stream or
 * the status poll but never both.
 *
 * Request path (Phase 1b, 2026-10-23):
 *
 *   socket ESTABLISHED
 *     -> conn: TLS phase (HTTPS) / WAIT_REQ (plain)          http/conn.c
 *     -> TLS handshake steps                                  tls/tls_server.c
 *     -> request bytes, any split, into one pooled context   http/http_req.c
 *     -> complete + validated request
 *     -> route lookup (path, then method -> 404 / 405)
 *     -> authorization hook (always allows in Phase 1)
 *     -> handler -> response -> close, or -> STREAM
 *
 *   Every way a connection ends goes through conn_teardown(): response done,
 *   parse error, timeout, TLS error, peer FIN/RST, stream takeover, send
 *   failure. It is safe to call twice.
 *
 * What did not change: the MJPEG sender (push_stream_frame), the single-viewer
 * takeover policy, Connection: close on every response, the TLS suites and
 * tickets, and the stream diagnostics.
 */

#include "stream_config.h"
#if defined(NET_STACK_TOE)

#include <stdio.h>
#include <string.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"

#include "pico/stdlib.h"
#include "pico/time.h"

#include "net_server.h"
#include "cam_state.h"
#include "net_load.h"
#include "cam_driver.h"
#include "dvp_capture.h"
#if STREAM_PIPELINE
#include "cam_pipeline.h"
#endif
#include "web_page.h"
#include "logo_png.h"
#if defined(STREAM_HTTPS)
#include "tls_server.h"
#endif
#include "http_req.h"
#include "conn.h"

#define TCP_CHUNK_MAX       1460        /* one segment worth per send() call    */
#define SEND_TIMEOUT_MS     2000        /* no progress for this long -> drop    */
#define HTTP_RX_CHUNK       512         /* bytes read per receive call          */
#define HTTP_RX_PASSES      4           /* receive calls per socket per pass    */

#if _WIZCHIP_ >= W6100
    #define TCP_SOCK_MODE   Sn_MR_TCP4
#else
    #define TCP_SOCK_MODE   Sn_MR_TCP
#endif

#if CONN_MAX_SOCKETS < HTTP_SOCK_COUNT
#error "CONN_MAX_SOCKETS must cover HTTP_SOCK_COUNT"
#endif
#if defined(STREAM_HTTPS) && CONN_REQ_POOL < TLS_MAX_CONN
#error "one request context per TLS session: CONN_REQ_POOL < TLS_MAX_CONN"
#endif


/* -------------------------------- Variables ------------------------------- */
static wiz_NetInfo g_net_info =
{
    .mac = NET_MAC_ADDR,
    .ip  = NET_IP_ADDR,
    .sn  = NET_SUBNET_MASK,
    .gw  = NET_GATEWAY,
    .dns = NET_DNS_ADDR,
#if _WIZCHIP_ > W5500
    .lla = {0xfe, 0x80, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x02, 0x08, 0xdc, 0xff,
            0xfe, 0x57, 0x57, 0x26},
    .gua = {0},
    .sn6 = {0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00},
    .gw6 = {0},
    .dns6 = {0x20, 0x01, 0x48, 0x60,
             0x48, 0x60, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x88, 0x88},
    .ipmode = NETINFO_STATIC_ALL
#else
    .dhcp = NETINFO_STATIC
#endif
};

static char    g_tx_hdr[256];
static char    g_json[CAM_STATUS_JSON_MAX];

/*
 * Connection state and the request contexts. The pool replaces the single
 * 1 KB g_req_buf the server used to parse from: a request now accumulates in
 * the context its connection owns until it is complete, however the bytes
 * were split. g_rx is only a staging buffer for one receive call; nothing in
 * it outlives the http_req_feed() that copies it.
 */
static conn_table_t g_conn;
static http_req_t   g_req[CONN_REQ_POOL];
static uint8_t      g_rx[HTTP_RX_CHUNK];

/** Per-socket role. A socket becomes STREAM after answering GET /stream. */
typedef enum {
    SOCK_ROLE_HTTP = 0,
    SOCK_ROLE_STREAM
} sock_role_t;

static sock_role_t g_role[HTTP_SOCK_COUNT];
/* How often the stream socket was serviced - if this stops while the
 * stream is open, the loop is not reaching push_stream_frame() at all. */
uint32_t g_push_calls;
static int8_t      g_stream_sn = -1;
/* What the current stream actually carried, printed when it ends. When the
 * page reopens because it drew nothing for 5 s (r=idle), "last sent N ms ago"
 * says which side stopped: a few ms means the board was still delivering and
 * the browser was not drawing; seconds means the board had stopped. */
static uint32_t        g_stream_frames;
static absolute_time_t g_stream_opened_at, g_stream_last_sent;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static uint8_t  sock_idx(uint8_t sn) { return (uint8_t)(sn - SOCK_HTTP_BASE); }

/* ------------------------------- TCP helpers ------------------------------- */
/**
 * Send the whole buffer, chunked to what the WIZnet TX buffer will take.
 *
 * This spins rather than yielding, so a frame going out is time the other
 * sockets are not being polled - including their deadlines, which are checked
 * on the next pass. That is a known limit of Phase 1, not something the HTTP
 * layer can hide.
 *
 * @return bytes sent, or negative when the peer stalls or disconnects.
 */
static int32_t tcp_send_all(uint8_t sn, const uint8_t *buf, uint32_t len)
{
#if defined(STREAM_HTTPS)
    /* Same contract, through the TLS record layer: encrypt, frame, then the
     * same chunked TOE sends underneath (tls_server.c bio_send). */
    return tls_send_all(sn, buf, len);
#endif
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
}

static int32_t send_response_hdrs(uint8_t sn, const char *status,
                                  const char *content_type, const char *extra,
                                  const char *body, uint32_t body_len)
{
    int n = snprintf(g_tx_hdr, sizeof(g_tx_hdr),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Cache-Control: no-store\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     status, content_type, (unsigned long)body_len,
                     extra ? extra : "");
    if (n <= 0 || n >= (int)sizeof(g_tx_hdr)) {
        return -1;
    }
    if (tcp_send_all(sn, (const uint8_t *)g_tx_hdr, (uint32_t)n) < 0) {
        return -1;
    }
    if (body_len && tcp_send_all(sn, (const uint8_t *)body, body_len) < 0) {
        return -1;
    }
    return 1;
}

static int32_t send_response(uint8_t sn, const char *status,
                             const char *content_type,
                             const char *body, uint32_t body_len)
{
    return send_response_hdrs(sn, status, content_type, NULL, body, body_len);
}

/** An error status with its reason phrase as a text/plain body. */
static int32_t send_error(uint8_t sn, int code, const char *extra)
{
    char status[48];
    const char *text = http_req_status_text(code);
    snprintf(status, sizeof(status), "%d %s", code, text);
    return send_response_hdrs(sn, status, "text/plain", extra, text, (uint32_t)strlen(text));
}

/** Status document, optionally as 409 so the page can say a value was clamped. */
static int32_t send_status(uint8_t sn, bool refused)
{
    int n = cam_state_status_json(g_json, sizeof(g_json));
    if (n < 0) {
        return -1;
    }
    return send_response(sn, refused ? "409 Conflict" : "200 OK",
                         "application/json", g_json, (uint32_t)n);
}

/* ------------------------------ Load generator ----------------------------- */
/**
 * Answer GET /load?kb=N with N KB of filler.
 *
 * Written a kilobyte at a time out of one constant block; the point is to
 * occupy the link and the stack, not to move any particular bytes.
 */
static int32_t serve_load(uint8_t sn, const char *q, size_t qlen)
{
    uint32_t kb = net_load_parse_kb(q, qlen);
    uint32_t i;
    int      n;

    if (kb == 0) {
        static const char none[] = "load=0";
        return send_response(sn, "200 OK", "text/plain", none,
                             (uint32_t)(sizeof(none) - 1));
    }

    n = snprintf(g_tx_hdr, sizeof(g_tx_hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/octet-stream\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 (unsigned long)(kb * LOAD_CHUNK_SIZE));
    if (n <= 0) {
        return -1;
    }
    if (tcp_send_all(sn, (const uint8_t *)g_tx_hdr, (uint32_t)n) < 0) {
        return -1;
    }

    for (i = 0; i < kb; i++) {
        if (tcp_send_all(sn, net_load_chunk(), LOAD_CHUNK_SIZE) < 0) {
            return -1;
        }
        /* Account as it goes, so a load that is cut off halfway still reports
           the half that was actually carried. */
        net_load_account(LOAD_CHUNK_SIZE);
    }
    return 1;
}

/* --------------------------------- Stream ---------------------------------- */
static void stream_summary(const char *what, uint8_t sn)
{
    absolute_time_t now = get_absolute_time();
    uint32_t life_ms = (uint32_t)(absolute_time_diff_us(g_stream_opened_at, now) / 1000);
    if (g_stream_frames == 0) {
        printf("%s socket %u: 0 frames in %lu ms\n", what, sn, (unsigned long)life_ms);
        return;
    }
    printf("%s socket %u: %lu frames in %lu ms, last sent %lu ms ago\n", what, sn,
           (unsigned long)g_stream_frames, (unsigned long)life_ms,
           (unsigned long)(absolute_time_diff_us(g_stream_last_sent, now) / 1000));
}

static void release_stream_socket(uint8_t sn)
{
    g_role[sock_idx(sn)] = SOCK_ROLE_HTTP;
    if (g_stream_sn == (int8_t)sn) {
        g_stream_sn = -1;
        stream_summary("Stream closed on", sn);
    }
}

/* ------------------------------ Connection end ----------------------------- */
/**
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
static void conn_teardown(uint8_t sn, conn_close_reason_t why)
{
    uint8_t idx = sock_idx(sn);
    if (conn_phase(&g_conn, idx) == CONN_FREE) {
        /* never registered (closed before we looked): register to get the
           CLOSING deadline; TLS phase takes no request context */
        conn_on_established(&g_conn, idx, now_ms(), true);
    }
    if (!conn_close(&g_conn, idx, now_ms(), why)) {
        return;                                     /* already closing */
    }
    release_stream_socket(sn);
#if defined(STREAM_HTTPS)
    tls_close(sn);
#endif
    uint8_t nb = SOCK_IO_NONBLOCK;
    ctlsocket(sn, CS_SET_IOMODE, &nb);
    disconnect(sn);
}

/* --------------------------------- Stream ---------------------------------- */
static int32_t begin_stream(uint8_t sn, const char *why, size_t why_len,
                            const char *page, size_t page_len)
{
    static const char stream_hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
        "Cache-Control: no-store\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    /*
     * Hand the camera to the newest request rather than refusing it. The page
     * reopens the stream whenever the frame size changes or the link comes
     * back, and the previous connection is often still half-open at that
     * moment; refusing the new one would leave a dead picture on screen, which
     * at an exhibition is the failure this whole build exists to avoid.
     */
    if (g_stream_sn >= 0 && g_stream_sn != (int8_t)sn) {
        uint8_t old = (uint8_t)g_stream_sn;
        printf("Stream taken over: socket %u -> %u (r=%.*s)\n", old, sn,
               (int)why_len, why);
        stream_summary("  old stream on", old);
        g_role[sock_idx(old)] = SOCK_ROLE_HTTP;
        g_stream_sn = -1;
        conn_teardown(old, CONN_CLOSE_TAKEOVER);
    }

    if (tcp_send_all(sn, (const uint8_t *)stream_hdr,
                     (uint32_t)(sizeof(stream_hdr) - 1)) < 0) {
        return -1;
    }

    g_role[sock_idx(sn)] = SOCK_ROLE_STREAM;
    g_stream_sn = (int8_t)sn;
    g_stream_frames = 0;
    g_stream_opened_at = get_absolute_time();
    g_stream_last_sent = g_stream_opened_at;
    conn_on_stream(&g_conn, sock_idx(sn), now_ms());   /* request context back to the pool */
    if (page_len) {
        printf("Stream opened on socket %u (r=%.*s, page: %.*s)\n", sn,
               (int)why_len, why, (int)page_len, page);
    } else {
        printf("Stream opened on socket %u (r=%.*s)\n", sn, (int)why_len, why);
    }
    cam_state_note_stream_open(why, why_len);
    return 1;
}

static int32_t push_stream_frame(uint8_t sn)
{
    uint32_t        jpeg_size;
    int             n;
    absolute_time_t send_start;

    if (!cam_state_streaming()) {
        cam_state_metrics_update();      /* keep the window honest while idle */
        return 1;
    }

#if STREAM_PIPELINE
    /* Core 1 captured it already. Nothing new yet means go and serve the
     * other sockets instead of waiting for VSYNC here. */
    static absolute_time_t s_last_sent, s_last_stall_log;
    g_push_calls++;
    cam_frame_t fr;
    if (!cam_pipeline_take(&fr)) {
        cam_state_metrics_update();
        /* Nothing new for 3 s on an open stream: say what the buffers hold,
         * every 5 s, so a freeze explains itself in the console. */
        if (absolute_time_diff_us(s_last_sent, get_absolute_time()) > 3000000 &&
            absolute_time_diff_us(s_last_stall_log, get_absolute_time()) > 5000000) {
            char d[200];
            cam_pipeline_debug(d, sizeof(d));
            printf("[pipe] s%u no new frame for 3 s: %s push_calls=%lu\n", sn, d,
                   (unsigned long)g_push_calls);
            s_last_stall_log = get_absolute_time();
        }
        return 1;
    }
    s_last_sent = get_absolute_time();
    const uint8_t *jpeg = fr.data;
    jpeg_size = fr.len;
#else
    const uint8_t *jpeg = image_buff;
    if (cam_capture_frame() != 0) {
        if (cam_state_note_capture_fail()) {
            printf("No frames for %u attempts - sensor recovered\n",
                   FAIL_STREAK_RECOVER);
        }
        return 1;
    }
    cam_state_note_capture_ok();

    jpeg_size = camera.frame_length;

    /* A truncated capture would desync the multipart parser - skip it. */
    if (jpeg_size < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        cam_state_note_dropped();
        return 1;
    }
#endif

    n = snprintf(g_tx_hdr, sizeof(g_tx_hdr),
                 "--" MJPEG_BOUNDARY "\r\n"
                 "Content-Type: image/jpeg\r\n"
                 "Content-Length: %lu\r\n"
                 "\r\n",
                 (unsigned long)jpeg_size);
    if (n <= 0) {
#if STREAM_PIPELINE
        cam_pipeline_release(&fr);
#endif
        return -1;
    }

    send_start = get_absolute_time();

    int32_t r = tcp_send_all(sn, (const uint8_t *)g_tx_hdr, (uint32_t)n);
    if (r >= 0) r = tcp_send_all(sn, jpeg, jpeg_size);
    if (r >= 0) r = tcp_send_all(sn, (const uint8_t *)"\r\n", 2);
#if STREAM_PIPELINE
    cam_pipeline_release(&fr);
#endif
    if (r < 0) {
        return -1;
    }

    g_stream_frames++;
    g_stream_last_sent = get_absolute_time();
    cam_state_note_frame(
        (uint64_t)absolute_time_diff_us(send_start, g_stream_last_sent),
        jpeg_size);
    cam_state_metrics_update();

    return 1;
}

/* --------------------------------- Handlers -------------------------------- */
/*
 * Handlers receive a request that is complete and has passed the parser's
 * policy. Path matching and query decoding are done once, before them: the
 * path is percent-decoded by the parser and every query value comes out of
 * http_query_get(). No handler parses text of its own.
 */
typedef int32_t (*route_fn_t)(uint8_t sn, const http_req_t *r);

static size_t query_value(const http_req_t *r, const char *key, char *out, size_t cap)
{
    size_t qlen;
    const uint8_t *q = http_req_query(r, &qlen);
    int n = http_query_get(q, qlen, key, out, cap);
    if (n < 0) {
        out[0] = 0;
        return 0;
    }
    return (size_t)n;
}

/* Strict small unsigned: 1..3 digits, value <= 255. */
static bool query_u8(const http_req_t *r, const char *key, uint8_t *out)
{
    char v[8];
    size_t n = query_value(r, key, v, sizeof(v));
    if (n == 0 || n > 3) return false;
    unsigned x = 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') return false;
        x = x * 10u + (unsigned)(v[i] - '0');
    }
    if (x > 255u) return false;
    *out = (uint8_t)x;
    return true;
}

static int32_t h_index(uint8_t sn, const http_req_t *r)
{
    (void)r;
    return send_response(sn, "200 OK", "text/html; charset=utf-8",
                         HTTP_INDEX_PAGE, (uint32_t)(sizeof(HTTP_INDEX_PAGE) - 1));
}

/*
 * The logo is the one asset the page pulls in. Unlike the status responses
 * it never changes, so let the browser keep it rather than refetch it on
 * every reload - and on a show floor the page gets reloaded a lot.
 */
static int32_t h_logo(uint8_t sn, const http_req_t *r)
{
    (void)r;
    int n = snprintf(g_tx_hdr, sizeof(g_tx_hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: image/png\r\n"
                     "Content-Length: %lu\r\n"
                     "Cache-Control: max-age=86400\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     (unsigned long)sizeof(LOGO_PNG));
    if (n <= 0) {
        return -1;
    }
    if (tcp_send_all(sn, (const uint8_t *)g_tx_hdr, (uint32_t)n) < 0) {
        return -1;
    }
    return tcp_send_all(sn, LOGO_PNG, (uint32_t)sizeof(LOGO_PNG));
}

/* The page tags each open: /stream?<counter>&r=<why>&p=<drawn>,<ms since last>. */
static int32_t h_stream(uint8_t sn, const http_req_t *r)
{
    char why[16], page[32];
    size_t why_len  = query_value(r, "r", why, sizeof(why));
    size_t page_len = query_value(r, "p", page, sizeof(page));
    return begin_stream(sn, why, why_len, page, page_len);
}

static int32_t h_load(uint8_t sn, const http_req_t *r)
{
    size_t qlen;
    const uint8_t *q = http_req_query(r, &qlen);
    return serve_load(sn, (const char *)q, qlen);
}

static int32_t h_start(uint8_t sn, const http_req_t *r)
{
    (void)r;
    cam_state_set_streaming(true);
    printf("Streaming STARTED\n");
    return send_status(sn, false);
}

static int32_t h_stop(uint8_t sn, const http_req_t *r)
{
    (void)r;
    cam_state_set_streaming(false);
    printf("Streaming STOPPED\n");
    return send_status(sn, false);
}

static int32_t h_status(uint8_t sn, const http_req_t *r)
{
    (void)r;
    return send_status(sn, false);
}

static int32_t h_reset(uint8_t sn, const http_req_t *r)
{
    (void)r;
    cam_state_recover();
    return send_status(sn, false);
}

/* No sensor control table in this build: an empty list, and /api/cam is
 * accepted and ignored, so the page's panel stays empty instead of failing.
 * Kept as compatibility endpoints until the camera HAL controls exist. */
static int32_t h_controls(uint8_t sn, const http_req_t *r)
{
    (void)r;
    static const char empty[] = "[]";
    return send_response(sn, "200 OK", "application/json",
                         empty, (uint32_t)(sizeof(empty) - 1));
}

static int32_t h_cam(uint8_t sn, const http_req_t *r)
{
    (void)r;
    return send_status(sn, false);
}

/* /api/res?v=<WxH> */
static int32_t h_res(uint8_t sn, const http_req_t *r)
{
    char v[16];
    size_t n = query_value(r, "v", v, sizeof(v));
    res_t res;
    if (n && cam_state_res_parse(v, n, &res)) {
        cam_state_apply_resolution(res);
    }
    return send_status(sn, false);
}

/*
 * /api/clk?div=<n>&pll=<n>
 *
 * Sweep the sensor clock dividers without reflashing. Frame rate is set
 * almost entirely by how fast the sensor emits a frame, so this is the knob
 * worth searching. Diagnostic.
 */
static int32_t h_clk(uint8_t sn, const http_req_t *r)
{
    uint8_t div, pll;
    if (query_u8(r, "div", &div) && query_u8(r, "pll", &pll)) {
#if STREAM_PIPELINE
        cam_pipeline_pause();         /* core 1 off the sensor first */
#endif
        cam_set_clock_div(div, pll);
#if STREAM_PIPELINE
        cam_pipeline_resume();
#endif
        cam_state_metrics_reset();
    }
    return send_status(sn, false);
}

/* --------------------------------- Routing --------------------------------- */
/*
 * Access class, for the authorization hook that later phases fill in. Phase 1
 * records it and allows everything; the point of having it now is that no
 * route can be added without saying which class it belongs to, and that the
 * check sits in one place in front of every handler.
 */
typedef enum {
    ROUTE_PUBLIC = 0,   /* page shell and its assets                          */
    ROUTE_VIEW,         /* watching: stream, status, control list             */
    ROUTE_ADMIN,        /* changes camera state                               */
    ROUTE_DIAG,         /* bench diagnostics, off in a product build          */
} route_access_t;

#define M_GET   (1u << HTTP_METHOD_GET)
#define M_POST  (1u << HTTP_METHOD_POST)

typedef struct {
    const char     *path;       /* exact match on the decoded path */
    uint8_t         methods;    /* M_GET | M_POST                  */
    uint8_t         access;     /* route_access_t                  */
    route_fn_t      fn;
} route_t;

/* Existing endpoints and the query shapes the page sends, all GET. */
static const route_t g_routes[] = {
    { "/",             M_GET, ROUTE_PUBLIC, h_index    },
    { "/index.html",   M_GET, ROUTE_PUBLIC, h_index    },
    { "/logo.png",     M_GET, ROUTE_PUBLIC, h_logo     },
    { "/stream",       M_GET, ROUTE_VIEW,   h_stream   },
    { "/api/status",   M_GET, ROUTE_VIEW,   h_status   },
    { "/api/controls", M_GET, ROUTE_VIEW,   h_controls },
    { "/api/start",    M_GET, ROUTE_ADMIN,  h_start    },
    { "/api/stop",     M_GET, ROUTE_ADMIN,  h_stop     },
    { "/api/reset",    M_GET, ROUTE_ADMIN,  h_reset    },
    { "/api/res",      M_GET, ROUTE_ADMIN,  h_res      },
    { "/api/cam",      M_GET, ROUTE_ADMIN,  h_cam      },
    { "/api/clk",      M_GET, ROUTE_DIAG,   h_clk      },
    { "/load",         M_GET, ROUTE_DIAG,   h_load     },
};

static const route_t *route_find(const char *path, size_t len)
{
    for (size_t i = 0; i < sizeof(g_routes) / sizeof(g_routes[0]); i++) {
        size_t n = strlen(g_routes[i].path);
        if (n == len && memcmp(g_routes[i].path, path, n) == 0) {
            return &g_routes[i];
        }
    }
    return NULL;
}

/* Phase 1: everything is allowed. Later phases decide here, and only here. */
static bool route_authorize(const route_t *rt, const http_req_t *r)
{
    (void)rt;
    (void)r;
    return true;
}

static void allow_header(const route_t *rt, char *out, size_t cap)
{
    snprintf(out, cap, "Allow: %s%s%s\r\n",
             (rt->methods & M_GET) ? "GET" : "",
             ((rt->methods & M_GET) && (rt->methods & M_POST)) ? ", " : "",
             (rt->methods & M_POST) ? "POST" : "");
}

/* A complete, validated request: look it up, check it, run it, end it. */
static void dispatch(uint8_t sn, const http_req_t *r)
{
    size_t plen;
    const char *path = http_req_path(r, &plen);
#if EXHIBITION_HTTP_LOG
    printf("[req s%u] %.*s\n", sn, (int)plen, path);
#endif
    const route_t *rt = route_find(path, plen);
    int32_t rc;

    if (!rt) {
        rc = send_error(sn, 404, NULL);
    } else if (!(rt->methods & (1u << http_req_method(r)))) {
        char allow[32];
        allow_header(rt, allow, sizeof(allow));
        rc = send_error(sn, 405, allow);
    } else if (!route_authorize(rt, r)) {
        rc = send_error(sn, 403, NULL);
    } else {
        rc = rt->fn(sn, r);
    }

    if (rc < 0) {
        conn_teardown(sn, CONN_CLOSE_SEND);
    } else if (conn_phase(&g_conn, sock_idx(sn)) != CONN_STREAM) {
        conn_teardown(sn, CONN_CLOSE_DONE);         /* Connection: close */
    }
}

/* ------------------------------ Request intake ----------------------------- */
/* Read what has arrived into this connection's request context. */
static void serve_request(uint8_t sn)
{
    uint8_t idx = sock_idx(sn);
    int ri = conn_req(&g_conn, idx);
    if (ri < 0) {
        conn_teardown(sn, CONN_CLOSE_POOL);
        return;
    }
    http_req_t *r = &g_req[ri];

    for (int pass = 0; pass < HTTP_RX_PASSES; pass++) {
        int32_t n;
#if defined(STREAM_HTTPS)
        n = tls_recv(sn, g_rx, sizeof(g_rx));
        if (n < 0) {
            conn_teardown(sn, CONN_CLOSE_PEER);     /* close_notify or TLS error */
            return;
        }
#else
        uint16_t avail = 0;
        getsockopt(sn, SO_RECVBUF, &avail);
        if (avail == 0) {
            n = 0;
        } else {
            if (avail > sizeof(g_rx)) avail = sizeof(g_rx);
            n = recv(sn, g_rx, avail);
            if (n < 0) {
                conn_teardown(sn, CONN_CLOSE_PEER);
                return;
            }
        }
#endif
        if (n == 0) {
            return;                                 /* nothing more this pass */
        }
        conn_on_bytes(&g_conn, idx, now_ms());

        size_t used = 0;
        http_req_result_t res = http_req_feed(r, g_rx, (size_t)n, &used);
        if (res == HTTP_REQ_NEED_MORE) {
            continue;
        }
        if (res == HTTP_REQ_ERROR) {
            int st = http_req_status(r);
            printf("[http] s%u rejected: %d %s\n", sn, st,
                   http_req_err_name(http_req_error(r)));
            send_error(sn, st, NULL);
            conn_teardown(sn, CONN_CLOSE_PARSE);
            return;
        }
        /* COMPLETE. Anything after the request in g_rx (a pipelined second
           request) is dropped: one request per connection. */
        dispatch(sn, r);
        return;
    }
}

/* ------------------------------ Socket machine ----------------------------- */
static void socket_run(uint8_t sn)
{
    uint8_t  idx = sock_idx(sn);
    uint8_t  sr  = getSn_SR(sn);
    uint32_t now = now_ms();

    /* Deadlines first, whatever the socket state. */
    conn_action_t act = conn_tick(&g_conn, idx, now);
    if (act == CONN_ACT_TIMEOUT) {
        conn_close_reason_t why = conn_timeout_reason(&g_conn, idx);
        printf("[conn] s%u %s timeout in phase %s\n", sn, conn_reason_name(why),
               conn_phase_name(conn_phase(&g_conn, idx)));
        if (conn_phase(&g_conn, idx) == CONN_REQ) {
            send_error(sn, 408, NULL);              /* best effort, bounded by the send stall */
        }
        conn_teardown(sn, why);
        return;
    }
    if (act == CONN_ACT_FORCE_CLOSE) {
        printf("[conn] s%u FIN exchange did not finish in %u ms (SR 0x%02X) - close()\n",
               sn, (unsigned)CONN_CLOSING_MS, sr);
        conn_note_forced(&g_conn, idx);
        close(sn);                                  /* SOCK_CLOSED -> reopened below */
        sr = getSn_SR(sn);
    }

    switch (sr) {
    case SOCK_ESTABLISHED:
        if (getSn_IR(sn) & Sn_IR_CON) {
            setSn_IR(sn, Sn_IR_CON);
        }
        if (conn_phase(&g_conn, idx) == CONN_CLOSING) {
            return;                                 /* our FIN is in flight */
        }
        if (conn_phase(&g_conn, idx) == CONN_FREE) {
#if defined(STREAM_HTTPS)
            conn_on_established(&g_conn, idx, now, true);
#else
            if (!conn_on_established(&g_conn, idx, now, false)) {
                send_error(sn, 503, NULL);
                conn_teardown(sn, CONN_CLOSE_POOL);
                return;
            }
            http_req_init(&g_req[conn_req(&g_conn, idx)]);
#endif
        }

#if defined(STREAM_HTTPS)
        if (conn_phase(&g_conn, idx) == CONN_TLS) {
            /* TLS first: nothing below runs until the handshake is done. The
             * ECDHE step stalls this loop ~130-220 ms for a new client and
             * ~5 ms for one resuming from a ticket. */
            if (!tls_active(sn) && !tls_accept(sn)) {
                conn_teardown(sn, CONN_CLOSE_TLS_REFUSED);
                return;
            }
            int hs = tls_handshake_step(sn);
            if (hs < 0) {
                conn_teardown(sn, CONN_CLOSE_TLS);
                return;
            }
            if (hs == 0) {
                return;
            }
            if (!conn_on_tls_ready(&g_conn, idx, now_ms())) {
                send_error(sn, 503, NULL);
                conn_teardown(sn, CONN_CLOSE_POOL);
                return;
            }
            http_req_init(&g_req[conn_req(&g_conn, idx)]);
        }
#endif

        if (conn_phase(&g_conn, idx) == CONN_STREAM) {
            if (push_stream_frame(sn) < 0) {
                conn_teardown(sn, CONN_CLOSE_SEND);
            }
            return;
        }
        serve_request(sn);
        return;

    /*
     * CLOSE_WAIT is the state a browser leaves behind when it closes a tab
     * without finishing the exchange. Recycling it here rather than waiting is
     * what keeps a listener from being lost one tab at a time over an
     * afternoon of visitors.
     */
    case SOCK_CLOSE_WAIT:
        conn_teardown(sn, CONN_CLOSE_PEER);
        return;

    case SOCK_CLOSED:
        release_stream_socket(sn);
#if defined(STREAM_HTTPS)
        tls_close(sn);                    /* peer reset: free the session */
#endif
        conn_on_closed(&g_conn, idx);
        if (socket(sn, TCP_SOCK_MODE, HTTP_PORT, 0x00) != sn) {
            printf("Socket %u open failed\n", sn);
        }
        return;

    case SOCK_INIT:
        if (listen(sn) != SOCK_OK) {
            printf("Socket %u listen failed\n", sn);
        }
        return;

    default:
        return;                            /* LISTEN / SYNRECV / closing states */
    }
}

/* --------------------------------- Public ---------------------------------- */
int net_server_status_json(char *buf, size_t cap)
{
    const uint32_t *c = g_conn.closes;
    return snprintf(buf, cap,
        "\"http\":{\"done\":%lu,\"parse\":%lu,\"to_hs\":%lu,\"to_req\":%lu,"
        "\"tls_err\":%lu,\"refused\":%lu,\"peer\":%lu,\"takeover\":%lu,"
        "\"send\":%lu,\"pool\":%lu,\"forced\":%lu,\"req_peak\":%u},",
        (unsigned long)c[CONN_CLOSE_DONE], (unsigned long)c[CONN_CLOSE_PARSE],
        (unsigned long)c[CONN_CLOSE_TIMEOUT_HS], (unsigned long)c[CONN_CLOSE_TIMEOUT_REQ],
        (unsigned long)c[CONN_CLOSE_TLS], (unsigned long)c[CONN_CLOSE_TLS_REFUSED],
        (unsigned long)c[CONN_CLOSE_PEER], (unsigned long)c[CONN_CLOSE_TAKEOVER],
        (unsigned long)c[CONN_CLOSE_SEND], (unsigned long)c[CONN_CLOSE_POOL],
        (unsigned long)g_conn.forced, (unsigned)g_conn.req_peak);
}

void net_server_init(void)
{
    uint8_t i;

    sleep_ms(3000);

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    network_initialize(g_net_info);
    print_network_information(g_net_info);
#if defined(STREAM_HTTPS)
    if (!tls_server_init()) {
        printf("[tls] NOT READY - every connection will be refused\n");
    }
#endif

    conn_init(&g_conn);
    for (i = 0; i < HTTP_SOCK_COUNT; i++) {
        g_role[i] = SOCK_ROLE_HTTP;
    }

    printf("HTTP camera server ready (" STACK_NAME ", %u listeners, %u request contexts)\n",
           (unsigned)HTTP_SOCK_COUNT, (unsigned)CONN_REQ_POOL);
#if defined(STREAM_HTTPS)
    printf("Open https://%d.%d.%d.%d/ in a browser (trust the demo CA first)\n",
#else
    printf("Open http://%d.%d.%d.%d/ in a browser\n",
#endif
           g_net_info.ip[0], g_net_info.ip[1],
           g_net_info.ip[2], g_net_info.ip[3]);
}

void net_server_service(void)
{
    uint8_t i;
    for (i = 0; i < HTTP_SOCK_COUNT; i++) {
        socket_run((uint8_t)(SOCK_HTTP_BASE + i));
    }
}

#endif /* NET_STACK_TOE */
