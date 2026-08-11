/**
 * @file    main.c
 * @brief   ArduCAM MEGA -> WIZnet TCP web streaming (one-chip solution).
 *
 * The device serves its own control page and streams JPEG frames as
 * multipart/x-mixed-replace (MJPEG), so any browser can view the camera with no
 * PC-side application. This is the difference from the UDP example, which needs
 * a Python receiver.
 *
 * Endpoints
 *   GET /              single-page UI (see web_page.h)
 *   GET /stream        multipart/x-mixed-replace MJPEG stream
 *   GET /api/start     begin streaming     -> status JSON
 *   GET /api/stop      stop streaming      -> status JSON
 *   GET /api/res?v=WxH change resolution   -> status JSON
 *   GET /api/status    current state       -> status JSON
 *
 * Sockets
 *   SOCK_HTTP_BASE .. SOCK_HTTP_BASE + HTTP_SOCK_COUNT - 1 all listen on port 80.
 *   One of them may be promoted to the streaming socket; the camera is a single
 *   resource, so a second /stream request is refused with 503.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"

#include "arducam_mega.h"
#include "web_page.h"
#include "logo_png.h"

/* --------------------------------- Config --------------------------------- */
#define PLL_SYS_KHZ         (200 * 1000)

#define HTTP_PORT           80
#define SOCK_HTTP_BASE      0
#define HTTP_SOCK_COUNT     4

#define HTTP_REQ_BUF_SIZE   1024        /* request line + headers we care about */
#define TCP_CHUNK_MAX       1460        /* one segment worth per send() call    */
#define SEND_TIMEOUT_MS     2000        /* no progress for this long -> drop     */
#define FAIL_STREAK_RECOVER 10          /* failed captures before a sensor reset */

#define MJPEG_BOUNDARY      "wiznetframe"

/*
 * Printed at boot and reported in the status JSON. Bump it when changing
 * capture or streaming behaviour, so a measurement can be tied to the build
 * that produced it.
 */
#define FW_BUILD_TAG        "toe " __DATE__ " " __TIME__

/*
 * Shown in the page header and reported in the status JSON.
 *
 * The lwIP example next door serves the same page and the same endpoints; this
 * is what tells the two apart when both are on screen side by side.
 */
#define STACK_NAME          "TOE"

#if _WIZCHIP_ >= W6100
    #define TCP_SOCK_MODE   Sn_MR_TCP4
#else
    #define TCP_SOCK_MODE   Sn_MR_TCP
#endif

/* ---------------------------- External (ArduCAM) --------------------------- */
extern uint8_t image_buff[];            /* JPEG capture buffer owned by the driver */

/* -------------------------------- Variables ------------------------------- */
static wiz_NetInfo g_net_info =
{
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x57},
    .ip  = {192, 168, 11, 3},
    .sn  = {255, 255, 255, 0},
    .gw  = {192, 168, 11, 1},
    .dns = {8, 8, 8, 8},
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

static uint8_t  g_req_buf[HTTP_REQ_BUF_SIZE];
static char     g_tx_hdr[256];          /* response / part headers               */

/** Per-socket role. A socket becomes STREAM after answering GET /stream. */
typedef enum {
    SOCK_ROLE_HTTP = 0,
    SOCK_ROLE_STREAM
} sock_role_t;

static sock_role_t g_role[HTTP_SOCK_COUNT];
static int8_t      g_stream_sn = -1;    /* socket currently streaming, -1 = none */

static volatile bool  g_streaming    = false;
static volatile res_t g_resolution   = RES_320X240;
static uint32_t       g_frame_count  = 0;
static uint32_t       g_drop_count   = 0;
static uint32_t       g_fail_streak  = 0;   /* consecutive failed captures */

/*
 * Live metrics, averaged over a one-second window.
 *
 * A frame period is made of three parts, measured separately so the bottleneck
 * is identifiable rather than guessed:
 *
 *   vsync_ms  waiting for the sensor to start the next frame. High values mean
 *             the frame rate is quantised by the sensor, not by our work.
 *   read_ms   pulling pixels out of the sensor (PIO + DMA + memcpy).
 *   send_ms   pushing the JPEG over TCP.
 *
 *   1000 / fps  ~=  vsync_ms + read_ms + send_ms
 */
static uint32_t g_fps_x10   = 0;        /* frames/s * 10, so JSON keeps 1 decimal */
static uint32_t g_vsync_ms  = 0;
static uint32_t g_read_ms   = 0;
static uint32_t g_send_ms   = 0;
static uint32_t g_frame_kb  = 0;        /* average JPEG size this window          */

/* Accumulators for the window in progress. */
static uint32_t        g_win_frames    = 0;
static uint64_t        g_win_vsync_us  = 0;
static uint64_t        g_win_read_us   = 0;
static uint64_t        g_win_send_us   = 0;
static uint64_t        g_win_bytes     = 0;
static absolute_time_t g_win_start;

/* --------------------------- Function Prototypes --------------------------- */
static void        set_clock_khz(void);
static void        network_init(void);

static const char *res_to_string(res_t res);
static bool        string_to_res(const char *s, size_t len, res_t *out);
static bool        parse_query_u32(const char *q, size_t qlen, const char *key,
                                   uint32_t *out);
static void        apply_resolution(res_t res);

static void        metrics_reset(void);
static void        metrics_update(void);

static int32_t     tcp_send_all(uint8_t sn, const uint8_t *buf, uint32_t len);
static int32_t     send_simple_response(uint8_t sn, const char *status,
                                        const char *content_type,
                                        const char *body, uint32_t body_len);
static int32_t     send_status_json(uint8_t sn);
static int32_t     begin_stream(uint8_t sn);
static int32_t     push_stream_frame(uint8_t sn);

static int32_t     http_route(uint8_t sn, const char *path, size_t path_len);
static int32_t     http_server_run(uint8_t sn);
static void        release_stream_socket(uint8_t sn);

/* ---------------------------------- Main ---------------------------------- */
int main(void)
{
    uint8_t i;

    set_clock_khz();

    printf("Initializing ArduCAM MEGA...\n");
    arducam_mega.init();
    arducam_mega.set_pixel_format(PIXFORMAT_JPEG);
    arducam_mega.set_frame_size(g_resolution);
    printf("Initial resolution: %s\n", res_to_string(g_resolution));

    printf("Initializing network...\n");
    network_init();

    for (i = 0; i < HTTP_SOCK_COUNT; i++) {
        g_role[i] = SOCK_ROLE_HTTP;
    }

    g_win_start = get_absolute_time();

    printf("Firmware build: " FW_BUILD_TAG "\n");
    printf("HTTP camera server ready\n");
    printf("Open http://%d.%d.%d.%d/ in a browser\n",
           g_net_info.ip[0], g_net_info.ip[1], g_net_info.ip[2], g_net_info.ip[3]);

    while (1) {
        for (i = 0; i < HTTP_SOCK_COUNT; i++) {
            http_server_run((uint8_t)(SOCK_HTTP_BASE + i));
        }
    }

    return 0;
}

/* ------------------------------- Platform --------------------------------- */
static void set_clock_khz(void)
{
    set_sys_clock_khz(PLL_SYS_KHZ, true);
    stdio_init_all();
    printf("System clock set to %d MHz\n", PLL_SYS_KHZ / 1000);
}

static void network_init(void)
{
    sleep_ms(3000);

    wizchip_spi_initialize();
    printf("WIZnet TCP Web Streaming (ArduCAM MEGA)\r\n");
    wizchip_cris_initialize();

    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    network_initialize(g_net_info);
    print_network_information(g_net_info);
}

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

/** Match a resolution token that is not NUL-terminated (it comes from a URL). */
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

/**
 * Pull an unsigned decimal value out of a query string, e.g. "div=3&pll=1".
 * The string is not NUL-terminated - it is a slice of the request line.
 */
static bool parse_query_u32(const char *q, size_t qlen, const char *key, uint32_t *out)
{
    size_t klen = strlen(key);
    size_t i;

    for (i = 0; i + klen + 1 <= qlen; i++) {
        /* Match "key=" at a parameter boundary. */
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
    sleep_ms(200);                       /* let the sensor settle */
    metrics_reset();                     /* old window describes the old mode */
    printf("Resolution changed to %s\n", res_to_string(res));
}

/* ------------------------------- TCP helpers ------------------------------- */
/**
 * Send the whole buffer, chunked to what the WIZnet TX buffer can take.
 * Returns bytes sent, or a negative value when the peer stalls or disconnects.
 */
static int32_t tcp_send_all(uint8_t sn, const uint8_t *buf, uint32_t len)
{
    uint32_t        sent = 0;
    absolute_time_t deadline = make_timeout_time_ms(SEND_TIMEOUT_MS);

    while (sent < len) {
        uint16_t freesize = 0;
        uint32_t remain;
        uint16_t chunk;
        int32_t  ret;

        if (getSn_SR(sn) != SOCK_ESTABLISHED) {
            return -1;                   /* peer went away mid-transfer */
        }

        getsockopt(sn, SO_SENDBUF, &freesize);
        if (freesize == 0) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
                return -2;               /* client is not draining the stream */
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
        deadline = make_timeout_time_ms(SEND_TIMEOUT_MS);  /* progress -> extend */
    }

    return (int32_t)sent;
}

static int32_t send_simple_response(uint8_t sn, const char *status,
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

static int32_t send_status_json(uint8_t sn)
{
    char body[224];
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
    if (n <= 0) {
        return -1;
    }
    return send_simple_response(sn, "200 OK", "application/json", body, (uint32_t)n);
}

/* --------------------------------- Stream ---------------------------------- */
/** Answer GET /stream: send the multipart preamble and promote the socket. */
static int32_t begin_stream(uint8_t sn)
{
    static const char stream_hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
        "Cache-Control: no-store\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    /*
     * Hand the camera over to the newest request rather than refusing it.
     *
     * The browser has to reopen the stream whenever the frame size changes -
     * an MJPEG decoder does not expect the dimensions to change mid-stream -
     * and the old connection is often still half-open at that moment. Refusing
     * the new one would leave the page showing a dead stream.
     */
    if (g_stream_sn >= 0 && g_stream_sn != (int8_t)sn) {
        uint8_t old = (uint8_t)g_stream_sn;
        printf("Stream taken over: socket %u -> %u\n", old, sn);
        g_role[old - SOCK_HTTP_BASE] = SOCK_ROLE_HTTP;
        g_stream_sn = -1;
        disconnect(old);
    }

    if (tcp_send_all(sn, (const uint8_t *)stream_hdr,
                     (uint32_t)(sizeof(stream_hdr) - 1)) < 0) {
        return -1;
    }

    g_role[sn - SOCK_HTTP_BASE] = SOCK_ROLE_STREAM;
    g_stream_sn = (int8_t)sn;
    printf("Stream opened on socket %u\n", sn);
    return 1;
}

/**
 * Capture one frame and write it as a multipart part.
 * Called from the main loop while the socket stays in SOCK_ROLE_STREAM.
 */
/** Discard the window in progress; used when the workload changes. */
static void metrics_reset(void)
{
    g_win_frames   = 0;
    g_win_vsync_us = 0;
    g_win_read_us  = 0;
    g_win_send_us  = 0;
    g_win_bytes    = 0;
    g_win_start    = get_absolute_time();
}

/** Roll the one-second measurement window over into the reported values. */
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

static int32_t push_stream_frame(uint8_t sn)
{
    uint32_t        jpeg_size;
    int             n;
    absolute_time_t send_start;

    if (!g_streaming) {
        metrics_update();                /* keep the window honest while idle */
        return 1;                        /* connection stays open, no new frames */
    }

    if (arducam_mega.get_frame() != 0) {
        g_drop_count++;

        /*
         * A run of failures means the sensor stopped emitting frames rather
         * than that one capture went bad - typically a clock setting it could
         * not take. Reset it instead of spinning on a dead sensor.
         */
        if (++g_fail_streak >= FAIL_STREAK_RECOVER) {
            printf("No frames for %u attempts - recovering sensor\n",
                   (unsigned)g_fail_streak);
            arducam_recover();
            g_fail_streak = 0;
            metrics_reset();
        }
        return 1;
    }
    g_fail_streak = 0;

    jpeg_size = arducam_mega.frame.frame_length;

    /* A truncated capture would desync the multipart parser - skip it. */
    if (jpeg_size < 2 || image_buff[0] != 0xFF || image_buff[1] != 0xD8) {
        g_drop_count++;
        return 1;
    }

    n = snprintf(g_tx_hdr, sizeof(g_tx_hdr),
                 "--" MJPEG_BOUNDARY "\r\n"
                 "Content-Type: image/jpeg\r\n"
                 "Content-Length: %lu\r\n"
                 "\r\n",
                 (unsigned long)jpeg_size);
    if (n <= 0) {
        return -1;
    }

    send_start = get_absolute_time();

    if (tcp_send_all(sn, (const uint8_t *)g_tx_hdr, (uint32_t)n) < 0) {
        return -1;
    }
    if (tcp_send_all(sn, image_buff, jpeg_size) < 0) {
        return -1;
    }
    if (tcp_send_all(sn, (const uint8_t *)"\r\n", 2) < 0) {
        return -1;
    }

    g_frame_count++;

    g_win_frames++;
    g_win_vsync_us += arducam_vsync_wait_us;
    g_win_read_us  += arducam_readout_us;
    g_win_send_us  += (uint64_t)absolute_time_diff_us(send_start, get_absolute_time());
    g_win_bytes    += jpeg_size;
    metrics_update();

    return 1;
}

static void release_stream_socket(uint8_t sn)
{
    g_role[sn - SOCK_HTTP_BASE] = SOCK_ROLE_HTTP;
    if (g_stream_sn == (int8_t)sn) {
        g_stream_sn = -1;
        printf("Stream closed on socket %u\n", sn);
    }
}

/* --------------------------------- Routing --------------------------------- */
static int32_t http_route(uint8_t sn, const char *path, size_t path_len)
{
    if ((path_len == 1 && path[0] == '/') ||
        (path_len == 11 && memcmp(path, "/index.html", 11) == 0)) {
        return send_simple_response(sn, "200 OK", "text/html; charset=utf-8",
                                    HTTP_INDEX_PAGE,
                                    (uint32_t)(sizeof(HTTP_INDEX_PAGE) - 1));
    }

    /*
     * The logo is the one asset the page pulls in. Unlike the status responses
     * it never changes, so let the browser keep it rather than refetch 35 KB
     * on every reload.
     */
    if (path_len == 9 && memcmp(path, "/logo.png", 9) == 0) {
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
        return begin_stream(sn);
    }

    if (path_len == 10 && memcmp(path, "/api/start", 10) == 0) {
        g_streaming   = true;
        g_frame_count = 0;
        g_drop_count  = 0;
        metrics_reset();
        printf("Streaming STARTED\n");
        return send_status_json(sn);
    }

    if (path_len == 9 && memcmp(path, "/api/stop", 9) == 0) {
        g_streaming = false;
        printf("Streaming STOPPED (frames=%lu, dropped=%lu)\n",
               (unsigned long)g_frame_count, (unsigned long)g_drop_count);
        return send_status_json(sn);
    }

    if (path_len == 11 && memcmp(path, "/api/status", 11) == 0) {
        return send_status_json(sn);
    }

    if (path_len == 10 && memcmp(path, "/api/reset", 10) == 0) {
        arducam_recover();
        g_fail_streak = 0;
        metrics_reset();
        return send_status_json(sn);
    }

    /* /api/res?v=<WxH> */
    if (path_len > 11 && memcmp(path, "/api/res?v=", 11) == 0) {
        res_t res;
        if (string_to_res(path + 11, path_len - 11, &res)) {
            apply_resolution(res);
        }
        return send_status_json(sn);
    }

    /*
     * /api/clk?div=<n>&pll=<n>
     *
     * Sweep the sensor clock dividers without reflashing. Frame rate is set
     * almost entirely by how fast the sensor emits a frame, so this is the
     * knob worth searching. Once a good pair is found for each resolution,
     * fold the values into set_framesize().
     */
    if (path_len > 9 && memcmp(path, "/api/clk?", 9) == 0) {
        const char *q   = path + 9;
        size_t      qlen = path_len - 9;
        uint32_t    div, pll;

        if (parse_query_u32(q, qlen, "div", &div) &&
            parse_query_u32(q, qlen, "pll", &pll) &&
            div <= 0xFF && pll <= 0xFF) {
            arducam_set_clock_div((uint8_t)div, (uint8_t)pll);
            metrics_reset();
        }
        return send_status_json(sn);
    }

    {
        static const char nf[] = "Not Found";
        return send_simple_response(sn, "404 Not Found", "text/plain",
                                    nf, (uint32_t)(sizeof(nf) - 1));
    }
}

/* ------------------------------ Socket machine ----------------------------- */
static int32_t http_server_run(uint8_t sn)
{
    uint8_t  sr = getSn_SR(sn);
    uint16_t received;
    int32_t  ret;

    switch (sr) {
    case SOCK_ESTABLISHED:
        if (getSn_IR(sn) & Sn_IR_CON) {
            setSn_IR(sn, Sn_IR_CON);
        }

        if (g_role[sn - SOCK_HTTP_BASE] == SOCK_ROLE_STREAM) {
            if (push_stream_frame(sn) < 0) {
                release_stream_socket(sn);
                disconnect(sn);
            }
            return 1;
        }

        getsockopt(sn, SO_RECVBUF, &received);
        if (received == 0) {
            return 1;                    /* nothing to parse yet */
        }
        if (received > HTTP_REQ_BUF_SIZE - 1) {
            received = HTTP_REQ_BUF_SIZE - 1;
        }

        ret = recv(sn, g_req_buf, received);
        if (ret <= 0) {
            return ret;
        }
        g_req_buf[ret] = '\0';

        /* Only GET is served; the request line is all this example needs. */
        if (memcmp(g_req_buf, "GET ", 4) == 0) {
            const char *path = (const char *)g_req_buf + 4;
            const char *end  = strpbrk(path, " \r\n");
            size_t      len  = end ? (size_t)(end - path) : strlen(path);

            if (http_route(sn, path, len) < 0) {
                disconnect(sn);
                return 1;
            }
        } else {
            static const char na[] = "Method Not Allowed";
            send_simple_response(sn, "405 Method Not Allowed", "text/plain",
                                 na, (uint32_t)(sizeof(na) - 1));
        }

        /* Every response except the stream is Connection: close. */
        if (g_role[sn - SOCK_HTTP_BASE] != SOCK_ROLE_STREAM) {
            disconnect(sn);
        }
        break;

    case SOCK_CLOSE_WAIT:
        release_stream_socket(sn);
        disconnect(sn);
        break;

    case SOCK_CLOSED:
        release_stream_socket(sn);
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
