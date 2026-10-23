/**
 * @file    server_toe.c
 * @brief   HTTP + MJPEG server on the WIZnet hardware TCP/IP offload engine.
 *
 * Built only when NET_STACK_TOE is defined.
 *
 * Four hardware sockets all listen on port 80. That is not a throughput choice:
 * on this chip a listening socket *becomes* the connection when a client
 * arrives, so a single listener could hold the MJPEG stream or the status poll
 * but never both. The exhibition build needs three at once - stream, poll and
 * the load generator - plus one spare for a reload that arrives before the old
 * socket has been recycled.
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

#define HTTP_REQ_BUF_SIZE   1024        /* request line + headers we care about */
#define TCP_CHUNK_MAX       1460        /* one segment worth per send() call    */
#define SEND_TIMEOUT_MS     2000        /* no progress for this long -> drop    */

#if _WIZCHIP_ >= W6100
    #define TCP_SOCK_MODE   Sn_MR_TCP4
#else
    #define TCP_SOCK_MODE   Sn_MR_TCP
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

static uint8_t g_req_buf[HTTP_REQ_BUF_SIZE];
static char    g_tx_hdr[256];
static char    g_json[CAM_STATUS_JSON_MAX];

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

/* ------------------------------- TCP helpers ------------------------------- */
/* Close a connection. On the HTTPS image the TLS session is ended first -
 * close_notify if it got that far - and its ~37 KB of heap released. */
static void net_disconnect(uint8_t sn)
{
#if defined(STREAM_HTTPS)
    tls_close(sn);
#endif
    disconnect(sn);
}

/**
 * Send the whole buffer, chunked to what the WIZnet TX buffer will take.
 *
 * This spins rather than yielding, so a frame going out is time the other
 * sockets are not being polled. That is a real cost and the load demo makes it
 * visible - it is also why the load generator is one connection: three of these
 * interleaving would be measuring the loop, not the stack.
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

static int32_t send_response(uint8_t sn, const char *status,
                             const char *content_type,
                             const char *body, uint32_t body_len)
{
    int n = snprintf(g_tx_hdr, sizeof(g_tx_hdr),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, content_type, (unsigned long)body_len);
    if (n <= 0) {
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
    g_role[sn - SOCK_HTTP_BASE] = SOCK_ROLE_HTTP;
    if (g_stream_sn == (int8_t)sn) {
        g_stream_sn = -1;
        stream_summary("Stream closed on", sn);
    }
}

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
        g_role[old - SOCK_HTTP_BASE] = SOCK_ROLE_HTTP;
        g_stream_sn = -1;
        net_disconnect(old);
    }

    if (tcp_send_all(sn, (const uint8_t *)stream_hdr,
                     (uint32_t)(sizeof(stream_hdr) - 1)) < 0) {
        return -1;
    }

    g_role[sn - SOCK_HTTP_BASE] = SOCK_ROLE_STREAM;
    g_stream_sn = (int8_t)sn;
    g_stream_frames = 0;
    g_stream_opened_at = get_absolute_time();
    g_stream_last_sent = g_stream_opened_at;
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

static int32_t http_route(uint8_t sn, const char *path, size_t path_len)
{
#if EXHIBITION_HTTP_LOG
    printf("[req s%u] %.*s\n", sn, (int)path_len, path);
#endif

    if (path_is(path, path_len, "/") || path_is(path, path_len, "/index.html")) {
        return send_response(sn, "200 OK", "text/html; charset=utf-8",
                             HTTP_INDEX_PAGE,
                             (uint32_t)(sizeof(HTTP_INDEX_PAGE) - 1));
    }

    /*
     * The logo is the one asset the page pulls in. Unlike the status responses
     * it never changes, so let the browser keep it rather than refetch it on
     * every reload - and on a show floor the page gets reloaded a lot.
     */
    if (path_is(path, path_len, "/logo.png")) {
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

    /* The page appends a counter to force the browser to reopen the stream. */
    if (path_len >= 7 && memcmp(path, "/stream", 7) == 0 &&
        (path_len == 7 || path[7] == '?')) {
        /* The page tags each open with why it opened: /stream?<n>&r=<why>. */
        const char *why = "";
        size_t      why_len = 0;
        for (size_t i = 7; i + 3 <= path_len; i++) {
            if (memcmp(path + i, "&r=", 3) == 0) {
                why = path + i + 3;
                while (i + 3 + why_len < path_len && why_len < 8 &&
                       why[why_len] != '&' && why[why_len] != ' ') {
                    why_len++;
                }
                break;
            }
        }
        const char *page = "";
        size_t      page_len = 0;
        for (size_t i = 7; i + 3 <= path_len; i++) {
            if (memcmp(path + i, "&p=", 3) == 0) {
                page = path + i + 3;
                while (i + 3 + page_len < path_len && page_len < 24 &&
                       page[page_len] != '&' && page[page_len] != ' ') {
                    page_len++;
                }
                break;
            }
        }
        return begin_stream(sn, why, why_len, page, page_len);
    }

    if (path_starts(path, path_len, "/load?")) {
        return serve_load(sn, path + 6, path_len - 6);
    }

    if (path_is(path, path_len, "/api/start")) {
        cam_state_set_streaming(true);
        printf("Streaming STARTED\n");
        return send_status(sn, false);
    }

    if (path_is(path, path_len, "/api/stop")) {
        cam_state_set_streaming(false);
        printf("Streaming STOPPED\n");
        return send_status(sn, false);
    }

    if (path_is(path, path_len, "/api/status")) {
        return send_status(sn, false);
    }

    if (path_is(path, path_len, "/api/reset")) {
        cam_state_recover();
        return send_status(sn, false);
    }

    /* No sensor control table in this build: an empty list, and /api/cam is
     * accepted and ignored, so the page's panel stays empty instead of failing. */
    if (path_is(path, path_len, "/api/controls")) {
        static const char empty[] = "[]";
        return send_response(sn, "200 OK", "application/json",
                             empty, (uint32_t)(sizeof(empty) - 1));
    }
    if (path_starts(path, path_len, "/api/cam?")) {
        return send_status(sn, false);
    }

    /* /api/res?v=<WxH> */
    if (path_starts(path, path_len, "/api/res?v=")) {
        res_t res;
        if (cam_state_res_parse(path + 11, path_len - 11, &res)) {
            cam_state_apply_resolution(res);
        }
        return send_status(sn, false);
    }

    /*
     * /api/clk?div=<n>&pll=<n>
     *
     * Sweep the sensor clock dividers without reflashing. Frame rate is set
     * almost entirely by how fast the sensor emits a frame, so this is the knob
     * worth searching.
     */
    if (path_starts(path, path_len, "/api/clk?")) {
        const char *q    = path + 9;
        size_t      qlen = path_len - 9;
        uint32_t    div  = 0, pll = 0;
        size_t      i;
        bool        got_div = false, got_pll = false;

        /* Two keys, both plain integers; a local scan keeps this file free of
           the control-table parser. */
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
#if STREAM_PIPELINE
            cam_pipeline_pause();         /* core 1 off the sensor first */
#endif
            cam_set_clock_div((uint8_t)div, (uint8_t)pll);
#if STREAM_PIPELINE
            cam_pipeline_resume();
#endif
            cam_state_metrics_reset();
        }
        return send_status(sn, false);
    }

    {
        static const char nf[] = "Not Found";
        return send_response(sn, "404 Not Found", "text/plain",
                             nf, (uint32_t)(sizeof(nf) - 1));
    }
}

/* ------------------------------ Socket machine ----------------------------- */
static int32_t socket_run(uint8_t sn)
{
    uint8_t  sr = getSn_SR(sn);
    uint16_t received;
    int32_t  ret;

    switch (sr) {
    case SOCK_ESTABLISHED:
        if (getSn_IR(sn) & Sn_IR_CON) {
            setSn_IR(sn, Sn_IR_CON);
        }

#if defined(STREAM_HTTPS)
        /* TLS first: nothing below runs until the handshake is done. The
         * ECDHE step stalls this loop ~129 ms for a new client and ~3 ms for
         * one resuming from a ticket. */
        if (!tls_active(sn) && !tls_accept(sn)) {
            net_disconnect(sn);
            return 1;
        }
        if (!tls_ready(sn)) {
            if (tls_handshake_step(sn) < 0) {
                net_disconnect(sn);
            }
            return 1;
        }
#endif

        if (g_role[sn - SOCK_HTTP_BASE] == SOCK_ROLE_STREAM) {
            if (push_stream_frame(sn) < 0) {
                release_stream_socket(sn);
                net_disconnect(sn);
            }
            return 1;
        }

#if defined(STREAM_HTTPS)
        (void)received;
        ret = tls_recv(sn, g_req_buf, HTTP_REQ_BUF_SIZE - 1);
        if (ret == 0) {
            return 1;
        }
        if (ret < 0) {
            net_disconnect(sn);
            return 1;
        }
#else
        getsockopt(sn, SO_RECVBUF, &received);
        if (received == 0) {
            return 1;
        }
        if (received > HTTP_REQ_BUF_SIZE - 1) {
            received = HTTP_REQ_BUF_SIZE - 1;
        }

        ret = recv(sn, g_req_buf, received);
        if (ret <= 0) {
            return ret;
        }
#endif
        g_req_buf[ret] = '\0';

        /* Only GET is served; the request line is all this example needs. */
        if (memcmp(g_req_buf, "GET ", 4) == 0) {
            const char *path = (const char *)g_req_buf + 4;
            const char *end  = strpbrk(path, " \r\n");
            size_t      len  = end ? (size_t)(end - path) : strlen(path);

            if (http_route(sn, path, len) < 0) {
                net_disconnect(sn);
                return 1;
            }
        } else {
            static const char na[] = "Method Not Allowed";
            send_response(sn, "405 Method Not Allowed", "text/plain",
                          na, (uint32_t)(sizeof(na) - 1));
        }

        /* Every response except the stream is Connection: close. */
        if (g_role[sn - SOCK_HTTP_BASE] != SOCK_ROLE_STREAM) {
            net_disconnect(sn);
        }
        break;

    /*
     * CLOSE_WAIT is the state a browser leaves behind when it closes a tab
     * without finishing the exchange. Recycling it here rather than waiting is
     * what keeps a listener from being lost one tab at a time over an
     * afternoon of visitors.
     */
    case SOCK_CLOSE_WAIT:
        release_stream_socket(sn);
        net_disconnect(sn);
        break;

    case SOCK_CLOSED:
        release_stream_socket(sn);
#if defined(STREAM_HTTPS)
        tls_close(sn);                    /* peer reset: free the session */
#endif
        if (socket(sn, TCP_SOCK_MODE, HTTP_PORT, 0x00) != sn) {
            printf("Socket %u open failed\n", sn);
            return -1;
        }
        break;

    case SOCK_INIT:
        if (listen(sn) != SOCK_OK) {
            printf("Socket %u listen failed\n", sn);
            return -1;
        }
        break;

    default:
        break;                            /* LISTEN / SYNSENT / closing states */
    }

    return 1;
}

/* --------------------------------- Public ---------------------------------- */
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
    tls_server_init();
#endif

    for (i = 0; i < HTTP_SOCK_COUNT; i++) {
        g_role[i] = SOCK_ROLE_HTTP;
    }

    printf("HTTP camera server ready (" STACK_NAME ", %u listeners)\n",
           (unsigned)HTTP_SOCK_COUNT);
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
