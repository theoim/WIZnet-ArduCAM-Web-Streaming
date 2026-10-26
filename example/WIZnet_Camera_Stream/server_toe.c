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
#include "cam_pipeline.h"       /* frame buffers, and the control command slot */
#include "settings.h"
#if CAM_AUTH
#include "session.h"
#include "tls_server.h"
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

#if CAM_AUTH
/* ------------------------------ Login state -------------------------------- */
static sess_table_t  g_sess;
static auth_db_t     g_db;              /* copy of the saved accounts        */
static uint64_t      g_setup_until;     /* setup mode open until (ms), 0 shut */
static const sess_t *g_req_sess;        /* session of the request in dispatch */
static uint32_t      g_stream_owner;    /* session id streaming, 0 = none     */
static auth_role_t   g_stream_role;

static uint64_t now64(void) { return time_us_64() / 1000u; }
static bool setup_open(void) { return g_setup_until && now64() < g_setup_until; }
#endif
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
#if CAM_AUTH
        g_stream_owner = 0;
#endif
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
#if CAM_AUTH
    /*
     * One viewer at a time (Q4): the same session reconnecting takes its own
     * stream back - the old connection is usually half-open after a reload -
     * an admin takes it from a viewer, and anyone else is refused while the
     * current stream stays up.
     */
    if (g_stream_sn >= 0 && g_stream_sn != (int8_t)sn && g_req_sess) {
        sess_stream_t p = sess_stream_policy(g_stream_owner, g_stream_role,
                                             g_req_sess->id, g_req_sess->role);
        if (p == SESS_STREAM_REFUSE) {
            printf("Stream refused on socket %u: %s session %lu is watching\n", sn,
                   auth_role_name(g_stream_role), (unsigned long)g_stream_owner);
            static const char busy[] = "{\"error\":\"the stream is in use by another session\"}";
            return send_response_hdrs(sn, "409 Conflict", "application/json", "Retry-After: 10\r\n",
                                      busy, (uint32_t)(sizeof(busy) - 1));
        }
    }
#endif
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
#if CAM_AUTH
    g_stream_owner = g_req_sess ? g_req_sess->id : 0;
    g_stream_role  = g_req_sess ? g_req_sess->role : AUTH_NONE;
#endif
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
    int crc = cam_capture_frame();
    if (crc == -5) {
        cam_state_note_too_big();
        return 1;
    }
    if (crc != 0) {
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

/* ------------------------------ Snapshot (2d) ------------------------------ */
/*
 * GET /snapshot.jpg - one JPEG. Streaming or not, the streaming flag and the
 * stream's frame rate are left alone: while the stream runs, the frame comes
 * out of the capture buffer it is already in; while it is stopped, core 1
 * captures one frame for this request. No frame within 2.5 s is 504.
 *
 * Core 0 waits here, like a control write does, so the other sockets are not
 * served until the frame is out.
 */
static int32_t h_snapshot(uint8_t sn, const http_req_t *r)
{
    (void)r;
    uint32_t t0 = now_ms();
#if STREAM_PIPELINE
    cam_frame_t fr;
    bool fresh = false;
    if (!cam_pipeline_snapshot_hold(&fr, CAM_SNAPSHOT_TIMEOUT_MS, &fresh)) {
        printf("[snap] no frame within %u ms (%s)\n", (unsigned)CAM_SNAPSHOT_TIMEOUT_MS,
               cam_state_streaming() ? "streaming" : "stopped");
        return send_error(sn, 504, NULL);
    }
    uint32_t waited = now_ms() - t0;
    int32_t rc = send_response(sn, "200 OK", "image/jpeg",
                               (const char *)fr.data, fr.len);
    cam_pipeline_snapshot_release(&fr);
    printf("[snap] %lu B seq %lu (%s) waited %lu ms, sent in %lu ms\n",
           (unsigned long)fr.len, (unsigned long)fr.seq,
           fresh ? "captured for it" : "latest", (unsigned long)waited,
           (unsigned long)(now_ms() - t0 - waited));
    return rc;
#else
    /* Sequential build: capture on this core, into the one buffer. */
    int cap = cam_capture_frame();
    uint32_t len = camera.frame_length;
    if (cap != 0 || now_ms() - t0 > CAM_SNAPSHOT_TIMEOUT_MS) {
        printf("[snap] no frame (rc %d, %lu ms)\n", cap, (unsigned long)(now_ms() - t0));
        return send_error(sn, 504, NULL);
    }
    if (len < 2 || image_buff[0] != 0xFF || image_buff[1] != 0xD8) {
        return send_error(sn, 504, NULL);
    }
    return send_response(sn, "200 OK", "image/jpeg", (const char *)image_buff, len);
#endif
}

/* --------------------------- Camera controls (2c) --------------------------- */
/*
 * GET /api/controls - what the driver offers, for the page to build its panel.
 * An empty list means the driver offers none (OV2640 until the new PCB).
 */
static int32_t h_controls(uint8_t sn, const http_req_t *r)
{
    (void)r;
    unsigned cnt = 0;
    const cam_ctrl_desc_t *t = cam_sensor_controls(&cnt);
    size_t used = 0, cap = sizeof(g_json);
    int n = snprintf(g_json, cap, "[");
    used = (size_t)n;

    for (unsigned i = 0; t && i < cnt; i++) {
        const cam_ctrl_desc_t *d = &t[i];
        const char *type = d->type == CAM_CTRL_BOOL ? "bool"
                         : d->type == CAM_CTRL_ENUM ? "enum" : "int";
        n = snprintf(g_json + used, cap - used,
                     "%s{\"name\":\"%s\",\"label\":\"%s\",\"group\":\"Camera\","
                     "\"type\":\"%s\",\"min\":%d,\"max\":%d,\"provisional\":%s",
                     i ? "," : "", d->name, d->label, type, d->min, d->max,
                     (d->flags & CAM_CTRL_F_PROVISIONAL) ? "true" : "false");
        if (n < 0 || (size_t)n >= cap - used) goto overflow;
        used += (size_t)n;
        if (d->type == CAM_CTRL_ENUM && d->enum_names) {
            n = snprintf(g_json + used, cap - used, ",\"options\":[");
            if (n < 0 || (size_t)n >= cap - used) goto overflow;
            used += (size_t)n;
            for (int k = 0; k <= d->max; k++) {
                n = snprintf(g_json + used, cap - used, "%s\"%s\"", k ? "," : "",
                             d->enum_names[k]);
                if (n < 0 || (size_t)n >= cap - used) goto overflow;
                used += (size_t)n;
            }
            n = snprintf(g_json + used, cap - used, "]");
            if (n < 0 || (size_t)n >= cap - used) goto overflow;
            used += (size_t)n;
        }
        n = snprintf(g_json + used, cap - used, "}");
        if (n < 0 || (size_t)n >= cap - used) goto overflow;
        used += (size_t)n;
    }
    n = snprintf(g_json + used, cap - used, "]");
    if (n < 0 || (size_t)n >= cap - used) goto overflow;
    used += (size_t)n;
    return send_response(sn, "200 OK", "application/json", g_json, (uint32_t)used);

overflow:
    printf("[ctrl] /api/controls does not fit in %u bytes\n", (unsigned)cap);
    return send_error(sn, 500, NULL);
}

static int32_t send_json_error(uint8_t sn, const char *status, const char *extra,
                               const char *msg, const char *control)
{
    char body[160];
    int n;
    if (control && control[0]) {
        n = snprintf(body, sizeof(body), "{\"error\":\"%s\",\"control\":\"%s\"}", msg, control);
    } else {
        n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    }
    if (n < 0 || (size_t)n >= sizeof(body)) {
        n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    }
    return send_response_hdrs(sn, status, "application/json", extra, body, (uint32_t)n);
}

/* Content-Type must be form-urlencoded; parameters after ';' are allowed. */
static bool is_form_content(const http_req_t *r)
{
    static const char want[] = "application/x-www-form-urlencoded";
    size_t len;
    const uint8_t *v = http_req_header(r, HTTP_H_CONTENT_TYPE, &len);
    size_t wl = sizeof(want) - 1;
    if (!v || len < wl) return false;
    for (size_t i = 0; i < wl; i++) {
        uint8_t c = v[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        if (c != (uint8_t)want[i]) return false;
    }
    return len == wl || v[wl] == ';' || v[wl] == ' ';
}

/*
 * POST /api/cam   Content-Type: application/x-www-form-urlencoded
 *                 body: brightness=3&flip=1
 *
 *   200  every control written -> status document (ctrl = written values)
 *   400  unknown control, bad value, out of range, duplicate - nothing written
 *   415  not form-urlencoded
 *   502  a write failed: the controls before it were written, the rest not
 *   503  the previous command is still in the slot (Retry-After: 1)
 *   504  core 1 did not finish in time (withdrawn, or outcome unknown)
 *
 * Values are refused, never clamped; a write is reported as written, never as
 * a value read back from the sensor.
 */
static int32_t h_cam(uint8_t sn, const http_req_t *r)
{
    if (!is_form_content(r)) {
        return send_json_error(sn, "415 Unsupported Media Type", NULL,
                               "use application/x-www-form-urlencoded", NULL);
    }

    unsigned cnt = 0;
    const cam_ctrl_desc_t *t = cam_sensor_controls(&cnt);
    size_t blen = 0;
    const uint8_t *body = http_req_body(r, &blen);
    cam_ctrl_batch_t b;
    char bad[CAM_CTRL_NAME_MAX + 1];
    cam_ctrl_err_t e = cam_ctrl_parse_form(body, blen, t, cnt, &b, bad, sizeof(bad));
    if (e != CAM_CTRL_OK) {
        /* the name came from the request: keep only safe JSON characters */
        for (char *c = bad; *c; c++) {
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) *c = '?';
        }
        printf("[ctrl] refused: %s (%s)\n", cam_ctrl_err_name(e), bad);
        return send_json_error(sn, "400 Bad Request", NULL, cam_ctrl_err_name(e), bad);
    }

    cam_ctrl_result_t res = { 0, -1, 0 };
    cam_cmd_rc_t rc = cam_pipeline_set_controls(&b, &res, CAM_CMD_TIMEOUT_MS);
    switch (rc) {
    case CAM_CMD_OK:
        for (unsigned i = 0; i < b.n; i++) {
            printf("[ctrl] %s = %d written\n", t[b.item[i].idx].name, b.item[i].value);
        }
        return send_status(sn, false);
    case CAM_CMD_FAILED: {
        char msg[64];
        snprintf(msg, sizeof(msg), "write failed; %u written before it", (unsigned)res.n_ok);
        return send_json_error(sn, "502 Bad Gateway", NULL, msg,
                               t[b.item[res.failed_at].idx].name);
    }
    case CAM_CMD_BUSY:
        return send_json_error(sn, "503 Service Unavailable", "Retry-After: 1\r\n",
                               cam_cmd_rc_name(rc), NULL);
    case CAM_CMD_TIMEOUT_NOT_RUN:
    case CAM_CMD_TIMEOUT_RUNNING:
    default:
        return send_json_error(sn, "504 Gateway Timeout", NULL, cam_cmd_rc_name(rc), NULL);
    }
}

/*
 * POST /api/settings/save - keep the current frame size and the written
 * control values across a reboot. Saving is the only way anything reaches
 * flash. The body is ignored. Core 0 is held for the erase and program.
 */
static int32_t h_settings_save(uint8_t sn, const http_req_t *r)
{
    (void)r;
    char why[64];
    uint32_t ms = 0;
    if (settings_save_current(why, sizeof(why), &ms) != 0) {
        return send_json_error(sn, "500 Internal Server Error", NULL, why, NULL);
    }
    return send_status(sn, false);
}

#if defined(STREAM_HTTPS)
#include "mbedtls/pkcs5.h"
/*
 * GET /api/diag/pbkdf2?n=<iterations> - time one PBKDF2-HMAC-SHA256 on this
 * core, to pick the stored-password iteration count from a measurement
 * (Phase 3b). Core 0 is held for the duration.
 */
static int32_t h_diag_pbkdf2(uint8_t sn, const http_req_t *r)
{
    char v[8];
    size_t vn = query_value(r, "n", v, sizeof(v));
    uint32_t n = 0;
    for (size_t i = 0; i < vn; i++) {
        if (v[i] < '0' || v[i] > '9') { n = 0; break; }
        n = n * 10u + (uint32_t)(v[i] - '0');
    }
    if (n == 0 || n > 200000u) {
        return send_error(sn, 400, NULL);
    }
    static const uint8_t salt[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t out[32];
    absolute_time_t t0 = get_absolute_time();
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const uint8_t *)"benchmark", 9,
                                           salt, sizeof(salt), n, sizeof(out), out);
    uint32_t us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
    printf("[auth] PBKDF2-SHA256 %lu iterations: %lu ms (rc %d)\n",
           (unsigned long)n, (unsigned long)(us / 1000), rc);
    int k = snprintf(g_json, sizeof(g_json), "{\"iterations\":%lu,\"us\":%lu,\"rc\":%d}",
                     (unsigned long)n, (unsigned long)us, rc);
    return send_response(sn, "200 OK", "application/json", g_json, (uint32_t)k);
}
#endif

#if CAM_AUTH
/* ------------------------------- Login (3b) -------------------------------- */
#define SID_COOKIE_ATTR "; Path=/; Secure; HttpOnly; SameSite=Strict"

static size_t body_value(const http_req_t *r, const char *key, char *out, size_t cap)
{
    size_t blen = 0;
    const uint8_t *b = http_req_body(r, &blen);
    int n = http_query_get(b, blen, key, out, cap);
    if (n < 0) {
        out[0] = 0;
        return 0;
    }
    return (size_t)n;
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

/* {"provisioned":..,"setup_open":..,"role":..,"user":..,"csrf":..,"viewer":{..}} */
static int auth_json(const sess_t *s, char *buf, size_t cap)
{
    char csrf[SESS_HEX_LEN + 1] = "";
    const char *user = "";
    if (s) {
        sess_hex(s->csrf, csrf);
        user = g_db.acct[s->role == AUTH_ADMIN ? 0 : 1].user;
    }
    int n = snprintf(buf, cap,
                     "{\"provisioned\":%s,\"setup_open\":%s,\"role\":\"%s\",\"user\":\"%s\",\"csrf\":\"%s\"",
                     auth_provisioned(&g_db) ? "true" : "false", setup_open() ? "true" : "false",
                     auth_role_name(s ? s->role : AUTH_NONE), user, csrf);
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t used = (size_t)n;
    if (s && s->role == AUTH_ADMIN) {
        n = snprintf(buf + used, cap - used, ",\"viewer\":{\"present\":%s,\"enabled\":%s,\"user\":\"%s\"}",
                     g_db.acct[1].present ? "true" : "false",
                     g_db.acct[1].enabled ? "true" : "false", g_db.acct[1].user);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    n = snprintf(buf + used, cap - used, "}");
    if (n < 0 || (size_t)n >= cap - used) return -1;
    return (int)(used + (size_t)n);
}

/* A new session for @p role, answered with its cookie and the auth document. */
static int32_t start_session(uint8_t sn, auth_role_t role)
{
    uint8_t rnd[2 * SESS_TOKEN_LEN];
    if (tls_random(rnd, sizeof(rnd)) != 0) {
        return send_json_error(sn, "500 Internal Server Error", NULL, "no randomness", NULL);
    }
    const sess_t *s = sess_create(&g_sess, role, rnd, now64());
    wipe(rnd, sizeof(rnd));
    char tok[SESS_HEX_LEN + 1], cookie[128];
    sess_hex(s->token, tok);
    snprintf(cookie, sizeof(cookie), "Set-Cookie: " SESS_COOKIE "=%s" SID_COOKIE_ATTR "; Max-Age=%lu\r\n",
             tok, (unsigned long)(SESS_ABS_MS / 1000u));
    wipe(tok, sizeof(tok));
    int n = auth_json(s, g_json, sizeof(g_json));
    int32_t rc = send_response_hdrs(sn, "200 OK", "application/json", cookie, g_json, (uint32_t)n);
    wipe(cookie, sizeof(cookie));
    return rc;
}

/* GET /api/auth - who is this, and does the device need setting up. Public:
 * the page asks before it knows whether it is logged in. */
static int32_t h_auth(uint8_t sn, const http_req_t *r)
{
    size_t clen = 0;
    const char *ck = (const char *)http_req_header(r, HTTP_H_COOKIE, &clen);
    const sess_t *s = sess_lookup(&g_sess, ck, clen, now64(), true);
    int n = auth_json(s, g_json, sizeof(g_json));
    return send_response(sn, "200 OK", "application/json", g_json, (uint32_t)n);
}

/* POST /api/login  user=..&pass=.. */
static int32_t h_login(uint8_t sn, const http_req_t *r)
{
    if (!auth_provisioned(&g_db)) {
        return send_json_error(sn, "403 Forbidden", NULL, "setup required", NULL);
    }
    uint32_t retry = 0;
    if (!sess_login_allowed(&g_sess, now64(), &retry)) {
        char ra[32];
        snprintf(ra, sizeof(ra), "Retry-After: %lu\r\n", (unsigned long)((retry + 999u) / 1000u));
        return send_json_error(sn, "429 Too Many Requests", ra, "too many failed logins; wait", NULL);
    }
    char user[AUTH_USER_MAX + 2], pass[AUTH_PASS_MAX + 2];
    size_t ul = body_value(r, "user", user, sizeof(user));
    size_t pl = body_value(r, "pass", pass, sizeof(pass));
    absolute_time_t t0 = get_absolute_time();
    auth_role_t role = auth_verify(&g_db, user, ul, pass, pl);
    wipe(pass, sizeof(pass));
    sess_login_result(&g_sess, role != AUTH_NONE, now64());
    printf("[auth] login '%s': %s (%lu ms)\n", role != AUTH_NONE ? user : "?",
           role != AUTH_NONE ? auth_role_name(role) : "refused",
           (unsigned long)(absolute_time_diff_us(t0, get_absolute_time()) / 1000));
    if (role == AUTH_NONE) {
        return send_json_error(sn, "401 Unauthorized", NULL, "wrong user name or password", NULL);
    }
    return start_session(sn, role);
}

/* POST /api/logout */
static int32_t h_logout(uint8_t sn, const http_req_t *r)
{
    (void)r;
    uint32_t id = g_req_sess->id;
    sess_end(&g_sess, id);
    g_req_sess = NULL;
    if (g_stream_owner == id && g_stream_sn >= 0) {
        conn_teardown((uint8_t)g_stream_sn, CONN_CLOSE_TAKEOVER);
    }
    static const char ok[] = "{\"ok\":true}";
    return send_response_hdrs(sn, "200 OK", "application/json",
                              "Set-Cookie: " SESS_COOKIE "=" SID_COOKIE_ATTR "; Max-Age=0\r\n",
                              ok, (uint32_t)(sizeof(ok) - 1));
}

/* Salt, set, save; g_db only changes when the save succeeded. */
static int set_account(auth_role_t role, const char *user, size_t ul, const char *pass, size_t pl,
                       char *why, size_t why_cap)
{
    auth_db_t db = g_db;
    uint8_t salt[AUTH_SALT_LEN];
    if (tls_random(salt, sizeof(salt)) != 0) {
        snprintf(why, why_cap, "no randomness");
        return -1;
    }
    auth_err_t e = auth_set(&db, role, user, ul, pass, pl, salt, AUTH_ITER_DEFAULT);
    if (e != AUTH_OK) {
        snprintf(why, why_cap, "%s", auth_err_name(e));
        return -1;
    }
    uint32_t ms = 0;
    if (settings_save_auth(&db, why, why_cap, &ms) != 0) {
        return -1;
    }
    g_db = db;
    wipe(&db, sizeof(db));
    return 0;
}

/* POST /api/setup  user=..&pass=..  - only while setup mode is open and no
 * admin exists. Creates the admin and logs it in. */
static int32_t h_setup(uint8_t sn, const http_req_t *r)
{
    if (auth_provisioned(&g_db)) {
        return send_json_error(sn, "409 Conflict", NULL, "already set up", NULL);
    }
    if (!setup_open()) {
        return send_json_error(sn, "403 Forbidden", NULL, "setup mode is not open", NULL);
    }
    char user[AUTH_USER_MAX + 2], pass[AUTH_PASS_MAX + 2], why[64];
    size_t ul = body_value(r, "user", user, sizeof(user));
    size_t pl = body_value(r, "pass", pass, sizeof(pass));
    int rc = set_account(AUTH_ADMIN, user, ul, pass, pl, why, sizeof(why));
    wipe(pass, sizeof(pass));
    if (rc != 0) {
        return send_json_error(sn, "400 Bad Request", NULL, why, NULL);
    }
    g_setup_until = 0;
    printf("[auth] admin '%s' created; setup mode closed\n", user);
    return start_session(sn, AUTH_ADMIN);
}

/* End every session of @p role except @p keep. */
static void end_role_except(auth_role_t role, uint32_t keep)
{
    for (unsigned i = 0; i < SESS_MAX; i++) {
        if (g_sess.s[i].used && g_sess.s[i].role == role && g_sess.s[i].id != keep) {
            sess_end(&g_sess, g_sess.s[i].id);
        }
    }
}

/*
 * POST /api/account (admin)
 *   role=admin&old=..&pass=..[&user=..]   change the admin password (old required)
 *   role=viewer&user=..&pass=..           set the viewer
 *   role=viewer&enabled=0|1               switch the viewer off/on
 * Sessions of the changed account end; the admin's own session stays.
 */
static int32_t h_account(uint8_t sn, const http_req_t *r)
{
    char role_s[8], user[AUTH_USER_MAX + 2], pass[AUTH_PASS_MAX + 2], old[AUTH_PASS_MAX + 2];
    char en[4], why[64];
    body_value(r, "role", role_s, sizeof(role_s));
    size_t ul = body_value(r, "user", user, sizeof(user));
    size_t pl = body_value(r, "pass", pass, sizeof(pass));
    size_t ol = body_value(r, "old", old, sizeof(old));
    size_t el = body_value(r, "enabled", en, sizeof(en));
    int rc = -1;
    uint32_t me = g_req_sess->id;

    if (strcmp(role_s, "admin") == 0) {
        const char *name = ul ? user : g_db.acct[0].user;
        size_t nl = ul ? ul : strlen(g_db.acct[0].user);
        if (auth_verify(&g_db, g_db.acct[0].user, strlen(g_db.acct[0].user), old, ol) != AUTH_ADMIN) {
            snprintf(why, sizeof(why), "current password is wrong");
        } else {
            rc = set_account(AUTH_ADMIN, name, nl, pass, pl, why, sizeof(why));
            if (rc == 0) end_role_except(AUTH_ADMIN, me);
        }
    } else if (strcmp(role_s, "viewer") == 0 && el && !pl) {
        if (!g_db.acct[1].present) {
            snprintf(why, sizeof(why), "no viewer account");
        } else {
            auth_db_t db = g_db;
            uint32_t ms;
            auth_viewer_enable(&db, en[0] == '1');
            rc = settings_save_auth(&db, why, sizeof(why), &ms);
            if (rc == 0) {
                g_db = db;
                if (!db.acct[1].enabled) sess_end_role(&g_sess, AUTH_VIEWER);
            }
        }
    } else if (strcmp(role_s, "viewer") == 0) {
        rc = set_account(AUTH_VIEWER, user, ul, pass, pl, why, sizeof(why));
        if (rc == 0) sess_end_role(&g_sess, AUTH_VIEWER);
    } else {
        snprintf(why, sizeof(why), "role must be admin or viewer");
    }
    wipe(pass, sizeof(pass));
    wipe(old, sizeof(old));
    if (rc != 0) {
        return send_json_error(sn, "400 Bad Request", NULL, why, NULL);
    }
    printf("[auth] account %s updated\n", role_s);
    int n = auth_json(g_req_sess, g_json, sizeof(g_json));
    return send_response(sn, "200 OK", "application/json", g_json, (uint32_t)n);
}
#endif /* CAM_AUTH */

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

#if CAM_QB_DIAG
/*
 * GET /api/diag/reg?a=<addr>[&v=<value>]   diagnostic image only
 *
 *   a   register, hex: 0x012A or 012A (0x0122-0x0134 only)
 *   v   value to write, hex 0x32 or decimal 50; leave out to just read
 *
 * Answers what was there before, what was written and what reads back. The
 * picture and frame size are for the person running the test to judge.
 */
int cam_qb_diag_reg(uint16_t reg, bool do_write, uint8_t val,
                    uint8_t *before, uint8_t *after);

static bool parse_num(const char *s, size_t n, bool hex_default, uint32_t max, uint32_t *out)
{
    bool hex = hex_default;
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        hex = true;
        s += 2;
        n -= 2;
    }
    if (n == 0 || n > 5) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9')                d = (uint32_t)(c - '0');
        else if (hex && c >= 'a' && c <= 'f')    d = (uint32_t)(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F')    d = (uint32_t)(c - 'A' + 10);
        else return false;
        v = v * (hex ? 16u : 10u) + d;
    }
    if (v > max) return false;
    *out = v;
    return true;
}

static int32_t h_diag_reg(uint8_t sn, const http_req_t *r)
{
    char a[16], v[16], body[160];
    size_t an = query_value(r, "a", a, sizeof(a));
    size_t vn = query_value(r, "v", v, sizeof(v));
    uint32_t reg, val = 0;

    if (!parse_num(a, an, true, 0xFFFF, &reg) ||
        (vn && !parse_num(v, vn, false, 0xFF, &val))) {
        static const char msg[] = "use /api/diag/reg?a=0x012A to read, "
                                  "/api/diag/reg?a=0x012A&v=0x32 to write\n";
        return send_response(sn, "400 Bad Request", "text/plain", msg,
                             (uint32_t)(sizeof(msg) - 1));
    }

    uint8_t before = 0, after = 0;
#if STREAM_PIPELINE
    cam_pipeline_pause();               /* core 1 off the sensor first */
#endif
    int rc = cam_qb_diag_reg((uint16_t)reg, vn != 0, (uint8_t)val, &before, &after);
#if STREAM_PIPELINE
    cam_pipeline_resume();
#endif

    int n;
    if (rc == -1) {
        n = snprintf(body, sizeof(body),
                     "{\"error\":\"0x%04lX is outside 0x0122-0x0134\"}", (unsigned long)reg);
        return send_response(sn, "400 Bad Request", "application/json", body, (uint32_t)n);
    }
    if (rc != 0) {
        n = snprintf(body, sizeof(body),
                     "{\"error\":\"SCCB failed on 0x%04lX\"}", (unsigned long)reg);
        return send_response(sn, "502 Bad Gateway", "application/json", body, (uint32_t)n);
    }
    if (vn) {
        n = snprintf(body, sizeof(body),
                     "{\"reg\":\"0x%04lX\",\"before\":\"0x%02X\",\"wrote\":\"0x%02lX\","
                     "\"read_back\":\"0x%02X\"}",
                     (unsigned long)reg, before, (unsigned long)val, after);
    } else {
        n = snprintf(body, sizeof(body), "{\"reg\":\"0x%04lX\",\"value\":\"0x%02X\"}",
                     (unsigned long)reg, after);
    }
    return send_response(sn, "200 OK", "application/json", body, (uint32_t)n);
}
#endif /* CAM_QB_DIAG */

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
    { "/snapshot.jpg", M_GET, ROUTE_VIEW,   h_snapshot },
    { "/api/status",   M_GET, ROUTE_VIEW,   h_status   },
    { "/api/controls", M_GET, ROUTE_VIEW,   h_controls },
    /* State changes are POST (Q1): with a session, a POST also needs the
     * X-CSRF-Token header. The frame size still rides in the query. */
    { "/api/start",    M_POST, ROUTE_ADMIN, h_start    },
    { "/api/stop",     M_POST, ROUTE_ADMIN, h_stop     },
    { "/api/reset",    M_POST, ROUTE_ADMIN, h_reset    },
    { "/api/res",      M_POST, ROUTE_ADMIN, h_res      },
#if CAM_AUTH
    { "/api/auth",     M_GET, ROUTE_PUBLIC, h_auth     },
    { "/api/login",    M_POST, ROUTE_PUBLIC, h_login   },
    { "/api/setup",    M_POST, ROUTE_PUBLIC, h_setup   },
    { "/api/logout",   M_POST, ROUTE_VIEW,  h_logout   },
    { "/api/account",  M_POST, ROUTE_ADMIN, h_account  },
#endif
    { "/api/cam",      M_POST, ROUTE_ADMIN, h_cam      },
    { "/api/settings/save", M_POST, ROUTE_ADMIN, h_settings_save },
    { "/api/clk",      M_GET, ROUTE_DIAG,   h_clk      },
    { "/load",         M_GET, ROUTE_DIAG,   h_load     },
#if defined(STREAM_HTTPS)
    { "/api/diag/pbkdf2", M_GET, ROUTE_DIAG, h_diag_pbkdf2 },
#endif
#if CAM_QB_DIAG
    { "/api/diag/reg", M_GET, ROUTE_DIAG,   h_diag_reg },
#endif
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
/* 0 to serve, else the status code; *why for the JSON body. */
static int route_authorize(const route_t *rt, const http_req_t *r, const char **why)
{
#if CAM_AUTH
    static const sess_need_t need[] = {
        [ROUTE_PUBLIC] = SESS_NEED_NONE, [ROUTE_VIEW] = SESS_NEED_VIEWER,
        [ROUTE_ADMIN] = SESS_NEED_ADMIN, [ROUTE_DIAG] = SESS_NEED_ADMIN,
    };
    size_t clen = 0, xlen = 0;
    const char *ck = (const char *)http_req_header(r, HTTP_H_COOKIE, &clen);
    const char *x  = (const char *)http_req_header(r, HTTP_H_CSRF, &xlen);
    g_req_sess = sess_lookup(&g_sess, ck, clen, now64(), true);
    bool post = http_req_method(r) == HTTP_METHOD_POST;
    return sess_policy(need[rt->access], auth_provisioned(&g_db), g_req_sess, post,
                       sess_csrf_ok(g_req_sess, x, xlen), why);
#else
    (void)rt;
    (void)r;
    *why = "ok";
    return 0;
#endif
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
    int deny;
    const char *why = "";

    if (!rt) {
        rc = send_error(sn, 404, NULL);
    } else if (!(rt->methods & (1u << http_req_method(r)))) {
        char allow[32];
        allow_header(rt, allow, sizeof(allow));
        rc = send_error(sn, 405, allow);
    } else if ((deny = route_authorize(rt, r, &why)) != 0) {
        rc = send_json_error(sn, deny == 401 ? "401 Unauthorized" : "403 Forbidden", NULL, why, NULL);
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
    settings_print();               /* USB serial is usually attached by now */
#if CAM_AUTH
    sess_init(&g_sess);
    g_db = *settings_auth();
    if (auth_provisioned(&g_db)) {
        printf("[auth] admin '%s'%s; login required\n", g_db.acct[0].user,
               g_db.acct[1].present ? (g_db.acct[1].enabled ? ", viewer on" : ", viewer off") : "");
    } else {
        printf("[auth] NO ADMIN ACCOUNT - only the login page answers.\n"
#if CAM_SETUP_SERIAL
               "[auth] type 'setup' + Enter on this console to open setup mode for 5 minutes\n"
#endif
               );
    }
#endif

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

#if CAM_AUTH
/*
 * Once a second: a stream whose session ended (expired, logged out elsewhere,
 * password changed) is closed - a long-lived connection must not outlive
 * the login that opened it. And the setup-mode console.
 */
static void auth_housekeeping(void)
{
    static uint32_t last;
    uint32_t t = now_ms();
#if CAM_SETUP_SERIAL
    /* Development build only: no setup button on this PCB yet (Q3 asks for a
     * physical one). 'setup' typed on the USB console opens setup mode. */
    static char line[16];
    static uint8_t len;
    int c;
    while ((c = getchar_timeout_us(0)) >= 0) {
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            if (strcmp(line, "setup") == 0) {
                if (auth_provisioned(&g_db)) {
                    printf("[auth] already set up - setup mode not opened\n");
                } else {
                    g_setup_until = now64() + 5u * 60u * 1000u;
                    printf("[auth] setup mode OPEN for 5 minutes - create the admin in the browser\n");
                }
            }
            len = 0;
        } else if (len + 1u < sizeof(line)) {
            line[len++] = (char)c;
        }
    }
#endif
    if (t - last < 1000u) {
        return;
    }
    last = t;
    if (g_stream_owner && g_stream_sn >= 0 && !sess_alive(&g_sess, g_stream_owner, now64())) {
        printf("[auth] session %lu ended - closing its stream\n", (unsigned long)g_stream_owner);
        conn_teardown((uint8_t)g_stream_sn, CONN_CLOSE_TAKEOVER);
    }
    if (g_setup_until && now64() >= g_setup_until) {
        g_setup_until = 0;
        printf("[auth] setup mode closed (timeout)\n");
    }
}
#endif

void net_server_service(void)
{
    uint8_t i;
    for (i = 0; i < HTTP_SOCK_COUNT; i++) {
        socket_run((uint8_t)(SOCK_HTTP_BASE + i));
    }
#if CAM_AUTH
    auth_housekeeping();
#endif
}

#endif /* NET_STACK_TOE */
