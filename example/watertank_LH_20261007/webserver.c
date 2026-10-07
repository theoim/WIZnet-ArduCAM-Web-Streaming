/**
 * @file    webserver.c
 * @brief   See webserver.h.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "socket.h"
#include "wizchip_conf.h"

#include <math.h>

#include "webserver.h"
#include "config.h"
#include "web_page.h"
#include "settings.h"
#include "arducam_mega.h"

extern uint8_t image_buff[];        /* the camera driver owns this */

/* --------------------------------------------------------------- per socket */

typedef enum {
    CONN_IDLE = 0,      /* listening */
    CONN_STREAM         /* pumping MJPEG parts until the client goes away */
} conn_state_t;

typedef struct {
    conn_state_t state;
    uint32_t     last_frame_ms;     /* streaming only, for the frame pacing */
    bool         kpalv_set;         /* set once the connection exists */

    /*
     * The frame currently going out, a piece at a time.
     *
     * A 45 KB HD frame cannot be handed to the chip in one go, and waiting for
     * it to drain is what was killing the microphone: the loop sat inside one
     * send for hundreds of milliseconds - a whole second, in one measured case -
     * while the DMA buffers behind it filled and were overwritten. The overrun
     * counter climbed by three a second and never came back down, and from the
     * outside the board looked like it was dying.
     *
     * Holding a pointer and a length instead means the loop pushes what fits,
     * returns, reads the microphone, and comes back. Nothing waits.
     */
    const uint8_t *tx_p;
    uint32_t       tx_left;
    uint32_t       tx_start_ms;     /* to give up on a client that stopped */
} conn_t;

/* How much is offered to the chip in one pass. Small enough that the chip
 * always has room for it soon after the previous one left. */
#define TX_CHUNK    2048

/*
 * Frame cost, in two halves, because they have different cures.
 *
 *   capture   the sensor's own frame wait plus the JPEG over SPI. Bounded by
 *             the camera; a lower resolution is the only lever.
 *   send      the same bytes onto the wire. Bounded by the link and by the
 *             viewer, which on a phone over a forwarded port is the slow end.
 *
 * Printed every two seconds and only while somebody is watching, so an idle box
 * stays quiet. Without it "the video feels slow" has no number attached, and
 * the answer last time was a setting rather than a fault.
 */
static uint32_t s_fr_count, s_fr_bytes;
static uint64_t s_fr_cap_us, s_fr_send_us;
static uint32_t s_fr_report_ms;
static uint64_t s_fr_start_us;
static uint64_t s_fr_cap_this;

/* A frame that has not finished inside this is a client that is not reading.
 * Dropping it costs one frame; waiting for it costs the microphone. */
#define TX_TIMEOUT_MS   3000

static conn_t   s_conn[SOCK_HTTP_COUNT];

/*
 * Written by the detector and read by the server. On the dual-core image those
 * are different cores, so it is volatile: without that the compiler is entitled
 * to hold it in a register across the polling loop and never notice it changed.
 * One writer, one reader, one bool - no lock is needed beyond that.
 */

static void listen_again(uint8_t sn, conn_t *c);

/*
 * Close every stream except the one on @p keep_sn. Pass 0xFF to close them all.
 *
 * One viewer at a time, enforced rather than hoped for. Two things make streams
 * pile up: the page reconnects when a frame fails, and a client that walks out
 * of range holds its socket until keepalive notices ten seconds later - so a
 * flaky phone leaves a trail of sockets that are all still capturing.
 *
 * At QVGA that wasted effort. At HD it broke the box: each frame is a sensor
 * wait plus 40 KB over SPI plus 40 KB on the wire, call it 100 ms, and three of
 * those in one pass of the loop is a third of a second in which no audio block
 * is read and no other request is answered. From a phone that looks like a save
 * that times out and a REFRESH that never lands - which is exactly what it did.
 */
static void drop_streams(uint8_t keep_sn)
{
    for (int i = 0; i < SOCK_HTTP_COUNT; i++) {
        uint8_t sn = (uint8_t)(SOCK_HTTP_BASE + i);

        if (sn == keep_sn) continue;
        if (s_conn[i].state != CONN_STREAM) continue;

        printf("[web] closing older stream on socket %u\n", sn);
        listen_again(sn, &s_conn[i]);
    }
}
static volatile bool s_alarm;
static uint32_t s_last_request_ms;
static uint32_t s_reboot_at_ms;     /* non-zero once a save has been accepted */

/*
 * Peak held between reads, not the last block.
 *
 * A block is 16 ms and the panel asks a few times a second, so reporting the
 * most recent block means a shout lands between two reads and never appears -
 * and somebody making a noise at the microphone to set a threshold concludes
 * the microphone is broken. Core 0 raises these; the server clears them on read.
 */
static volatile float    s_peak_rms, s_base;
static volatile uint32_t s_blocks;

/* In main.c, beside the detector it writes to. */
void mic_set_floor_db(int db);

#if USE_CORE1
#include "xcore.h"
#define STAGE(s)  do { g_xc.stage = (s); \
                       g_xc.stage_at = to_ms_since_boot(get_absolute_time()); \
                  } while (0)
#else
#define STAGE(s)  do { } while (0)
#endif

static volatile float s_tone;
static volatile int   s_hz;

#define REQ_BUF_SIZE    1600

/* ------------------------------------------------------------------ helpers */

static uint32_t now_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

static void send_all(uint8_t sn, const void *data, uint16_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t sent = 0;
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + 3000;

    while (sent < len) {
        int32_t n = send(sn, (uint8_t *)p + sent, (uint16_t)(len - sent));

        /*
         * Zero is SOCK_BUSY, not failure. ioLibrary returns it when the chip's
         * transmit buffer is full and means "ask again"; only a negative value
         * is an error. Treating zero as failure closes the socket mid-frame,
         * which looks like a stream that dies whenever the picture gets large.
         */
        if (n < 0) return;
        if (n == 0) {
            if (getSn_SR(sn) != SOCK_ESTABLISHED) return;

            /*
             * A client that stopped reading must not be able to stop the board.
             * Without a deadline this waits on a phone that walked out of range,
             * and for as long as it waits the microphone goes unread - which is
             * the one thing the firmware exists to keep doing.
             */
            if (to_ms_since_boot(get_absolute_time()) > deadline) {
                printf("[web] send timed out on socket %u\n", sn);
                return;
            }
            sleep_ms(1);
            continue;
        }
        sent += (uint16_t)n;
    }
}

/*
 * An image, which can be longer than send_all's uint16_t length.
 *
 * A QVGA frame is a few kilobytes and fits either way. A 720p frame is tens of
 * kilobytes and still would - until one bright, detailed scene produces a JPEG
 * past 64 KB, at which point the cast silently sends part of a picture and the
 * stream looks like a camera fault. Chunking removes the ceiling instead of
 * raising it.
 */
static void send_blob(uint8_t sn, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len) {
        uint16_t chunk = (len > 32768u) ? 32768u : (uint16_t)len;

        send_all(sn, p, chunk);
        p   += chunk;
        len -= chunk;
    }
}

static void send_str(uint8_t sn, const char *s)
{
    send_all(sn, s, (uint16_t)strlen(s));
}

/*
 * 404 for everything that is not an exact match, including a wrong or missing
 * token. No body, no hint, no difference between "no such path" and "not
 * allowed" - a scanner learns nothing it did not already know.
 *
 * This matters more than it did when the port only opened during an alarm. The
 * box is a control panel, so it answers all day, and the token is the whole of
 * what stands between the camera and whoever finds the port.
 */
static void send_404(uint8_t sn)
{
    send_str(sn,
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n");
}

/*
 * True when the request came from this box's own subnet.
 *
 * Such a request does not need the token, and that is not a hole in the gate -
 * it is the only way through it the first time. A box that has just been
 * unpacked has no webhook yet, which means nobody has been sent the link, which
 * means nobody has the token: the panel that exists to set the webhook would be
 * unreachable precisely when it is needed. Printing the token on a console only
 * moves the problem to whoever does not have a USB cable.
 *
 * What makes this safe enough is where the line is drawn. A request arriving
 * through the port forward carries the public address of whoever sent it, which
 * cannot match a private subnet, so the gate stays shut on the side that faces
 * the internet. Open on the inside means anyone already on the building's LAN
 * can watch the tank - which is a different and much smaller statement than
 * anyone on the internet, and it is the same group of people who could walk
 * downstairs and look at it.
 *
 * Anyone who needs the token can read it off the page source once they are in.
 */
static bool peer_is_local(uint8_t sn)
{
    uint8_t     peer[4];
    wiz_NetInfo me;

    getSn_DIPR(sn, peer);
    wizchip_getnetinfo(&me);

    for (int i = 0; i < 4; i++) {
        if (((peer[i] ^ me.ip[i]) & me.sn[i]) != 0) return false;
    }
    return true;
}

static bool token_ok(const char *path)
{
    const char *q = strstr(path, "t=");
    if (!q) return false;
    q += 2;

    size_t n = strlen(STREAM_TOKEN);
    if (strncmp(q, STREAM_TOKEN, n) != 0) return false;

    char after = q[n];
    return (after == '\0' || after == '&' || after == ' ');
}

/*
 * Capture one frame and report its size.
 *
 * get_frame() returns a status, not a length - zero means it worked - and the
 * size lands in arducam_mega.frame.frame_length. Reading the return value as a
 * length is a quiet failure: every capture looks empty, so the stream serves
 * nothing and the alert goes out as text with no picture, while the console
 * still says the camera initialised fine.
 */
static uint32_t capture(void)
{
    if (arducam_mega.get_frame() != 0) return 0;

    uint32_t len = arducam_mega.frame.frame_length;
    if (len < 2) return 0;
    if (image_buff[0] != 0xFF || image_buff[1] != 0xD8) return 0;

    return len;
}

/* -------------------------------------------------------------- resolution */

/*
 * Change the sensor's mode while everything keeps running.
 *
 * A resolution does not need the reboot that an address does: nothing else in
 * the firmware caches the frame size, the capture buffer is one fixed 200 KB
 * array whatever the mode, and the page reloads the video afterwards. Making
 * someone save and wait through a restart to find out whether HD is worth the
 * bandwidth is the difference between a setting that gets tuned and one that
 * gets left alone.
 *
 * The timing is copied from the exhibition build, where it was paid for: with
 * no settle around it the resolution write is silently dropped and the sensor
 * carries on at 320x240 while every status line claims otherwise. The stream is
 * dropped first for the same reason - reconfiguring the sensor underneath a
 * capture that is already in progress is how a camera wedges.
 */
static bool apply_res(int r)
{
    if (!settings_res_valid((uint8_t)r)) return false;

    drop_streams(0xFF);

    sleep_ms(200);
    if (arducam_mega.set_frame_size((res_t)r) != 0) {
        printf("[cam] set_frame_size(%d) FAILED\n", r);
        return false;
    }
    sleep_ms(200);

    /*
     * Live only. Flash is written by SAVE, so a resolution tried out here is
     * forgotten on the next power cut - which is the right way round: it costs
     * a flash erase to find out a mode looks wrong.
     */
    g_set.res = (uint8_t)r;
    printf("[cam] resolution now %s\n", settings_res_name(g_set.res));
    return true;
}

/* ------------------------------------------------------------------- page */

/*
 * Emitted in pieces rather than built in one buffer.
 *
 * The page outgrew a single snprintf when it gained a second tab, and a buffer
 * sized for "the largest page so far" is one that silently truncates the next
 * time a field is added. Streaming it out has no such edge.
 */
static void send_page(uint8_t sn)
{
    send_str(sn, PAGE_HEAD);


    /* The token and the stand-down time are the two things the page cannot
     * work out for itself. */
    {
        static char head[320];
        snprintf(head, sizeof(head),
            "var T='%s';"
            "var NOTE='부저음이 %.1f초간 들리지 않으면 경보가 해제됩니다. "
            "영상은 항상 표시됩니다.';",
            STREAM_TOKEN, (double)CLEAR_SECONDS);
        send_str(sn, head);
    }


    send_str(sn, PAGE_SCRIPT);
}

/* -------------------------------------------------------------------- api */

static void send_status(uint8_t sn)
{
    static char body[384];
    static char hdr[128];

    /*
     * Level as dBFS, because that is the unit anybody adjusting a microphone
     * already thinks in - a bare 0.0043 means nothing read off a screen. Full
     * scale is 0 dB, so a quiet room lands near -47 and a buzzer near -38.
     */
    float rms = s_peak_rms;
    float db  = (rms > 0.0000001f) ? 20.0f * log10f(rms) : -99.0f;
    float bdb = (s_base > 0.0f) ? 20.0f * log10f(s_base) : -99.0f;

    s_peak_rms = 0.0f;              /* next window starts from nothing */

    /*
     * Blocks a second. A panel showing a level of zero cannot say whether the
     * room is silent or the microphone is not being read at all - two very
     * different faults that look identical. This number moving is the
     * difference.
     */
    static uint32_t last_blocks, last_ms;
    uint32_t span = now_ms() - last_ms;
    uint32_t bps  = span ? (s_blocks - last_blocks) * 1000u / span : 0;
    last_blocks = s_blocks;
    last_ms     = now_ms();

    int n = snprintf(body, sizeof(body),
        "{\"run\":1,\"buzzer\":%d,\"stream\":1,\"hz\":%d,"
        "\"tone\":%.1f,\"up\":%lu,"
        "\"db\":%.1f,\"basedb\":%.1f,\"fl\":%d,\"blk\":%lu}",
        s_alarm ? 1 : 0, s_hz, (double)s_tone,
        (unsigned long)(now_ms() / 1000u),
        (double)db, (double)bdb, (int)g_set.det_floor_db,
        (unsigned long)bps);

    int h = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nCache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", n);

    send_all(sn, hdr, (uint16_t)h);
    send_all(sn, body, (uint16_t)n);
}

static void send_config(uint8_t sn)
{
    static char body[640];
    static char hdr[128];

    /*
     * The four address boxes are filled from the chip, not from what was stored.
     *
     * There were two sets of numbers on the panel and it was a mistake. The
     * stored set is a fallback that only matters on a day the router does not
     * answer; the live set is what the box is reachable at right now, and it is
     * what somebody looking at the screen means by "the address". Showing both
     * only asked the reader to work out which was which.
     *
     * It also makes the obvious action work. Switch to STATIC and save, and the
     * box keeps the address it already has - which is what pressing STATIC
     * plainly means. Before, it saved the old fallback and the board came back
     * somewhere else.
     */
    wiz_NetInfo live;
    wizchip_getnetinfo(&live);

    /*
     * The webhook is never sent out, not even masked. It is a password for a
     * Discord channel, and a panel that is reachable from the LAN without a
     * token is not a place to print one. All the page is told is whether there
     * is one, which is the only thing it needs in order to say so.
     */
    int n = snprintf(body, sizeof(body),
        "{\"ip\":\"%u.%u.%u.%u\",\"sn\":\"%u.%u.%u.%u\","
        "\"gw\":\"%u.%u.%u.%u\",\"dns\":\"%u.%u.%u.%u\","
        "\"port\":%u,\"ph\":\"%s\",\"pp\":%u,\"res\":%u,"
        "\"dhcp\":%u,\"whset\":%u,\"fl\":%d}",
        live.ip[0],  live.ip[1],  live.ip[2],  live.ip[3],
        live.sn[0],  live.sn[1],  live.sn[2],  live.sn[3],
        live.gw[0],  live.gw[1],  live.gw[2],  live.gw[3],
        live.dns[0], live.dns[1], live.dns[2], live.dns[3],
        g_set.http_port, g_set.pub_host, g_set.pub_port, g_set.res,
        g_set.use_dhcp, g_set.webhook[0] ? 1u : 0u, g_set.det_floor_db);

    int h = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nCache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", n);

    send_all(sn, hdr, (uint16_t)h);
    send_all(sn, body, (uint16_t)n);
}

/* ----------------------------------------------------------- form decoding */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Pull one field out of an application/x-www-form-urlencoded body.
 *
 * Written here rather than pulled in because the whole form is eight short
 * fields. The decoding that matters is percent-escapes: a webhook URL survives
 * a round trip untouched, but encodeURIComponent still escapes the slashes and
 * colons, so a reader that does not undo them stores a URL that will not
 * resolve.
 */
static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;

    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t o = 0;

            while (*v && *v != '&' && o + 1 < cap) {
                if (*v == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    out[o++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 3;
                } else if (*v == '+') {
                    out[o++] = ' ';
                    v++;
                } else {
                    out[o++] = *v++;
                }
            }
            out[o] = '\0';
            return o > 0;
        }

        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

static void handle_save(uint8_t sn, const char *body)
{
    settings_t s = g_set;       /* start from what is live, not from zero */
    char v[SETTINGS_WEBHOOK_MAX];

    if (!body) {
        send_str(sn, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; "
                     "charset=utf-8\r\nConnection: close\r\n\r\n본문 없음");
        return;
    }

    /*
     * Every address is validated before any of them is kept. A box that
     * accepted three good fields and one bad one would come back up on a
     * half-applied configuration and be unreachable by either the old address
     * or the new.
     */
    struct { const char *k; uint8_t *dst; } ips[] = {
        { "ip", s.ip }, { "sn", s.sn }, { "gw", s.gw }, { "dns", s.dns },
    };

    for (unsigned i = 0; i < 4; i++) {
        if (!form_get(body, ips[i].k, v, sizeof(v))) continue;
        if (!settings_parse_ip(v, ips[i].dst)) {
            send_str(sn, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; "
                         "charset=utf-8\r\nConnection: close\r\n\r\n"
                         "주소 형식이 올바르지 않습니다. 저장하지 않았습니다.");
            return;
        }
    }

    if (form_get(body, "port", v, sizeof(v))) {
        int p = atoi(v);
        if (p > 0 && p < 65536) s.http_port = (uint16_t)p;
    }
    if (form_get(body, "pp", v, sizeof(v))) {
        int p = atoi(v);
        if (p > 0 && p < 65536) s.pub_port = (uint16_t)p;
    }
    if (form_get(body, "ph", v, sizeof(v))) {
        strncpy(s.pub_host, v, SETTINGS_HOST_MAX - 1);
        s.pub_host[SETTINGS_HOST_MAX - 1] = '\0';
    }
    /*
     * Only the two values the page offers are accepted. Anything else would be
     * handed straight to set_frame_size() at the next boot, and a sensor put
     * into a mode this firmware never streams comes back as a box with no
     * picture and nothing on the console to say why.
     */
    if (form_get(body, "res", v, sizeof(v))) {
        uint8_t r = (uint8_t)atoi(v);
        if (settings_res_valid(r)) s.res = r;
    }

    if (form_get(body, "dhcp", v, sizeof(v))) {
        s.use_dhcp = (atoi(v) != 0) ? 1 : 0;
    }

    if (form_get(body, "wh", v, sizeof(v))) {
        strncpy(s.webhook, v, SETTINGS_WEBHOOK_MAX - 1);
        s.webhook[SETTINGS_WEBHOOK_MAX - 1] = '\0';
    }

    /*
     * The alarm floor in dBFS. The page always sends the field, so an empty one
     * is a deliberate "none" rather than a missing key. A positive number is
     * read as the same level without its sign - "35" typed on a phone keypad
     * that has no minus means -35.
     */
    if (form_get(body, "db", v, sizeof(v))) {
        long f = strtol(v, NULL, 10);
        if (f > 0) f = -f;
        if (f < -90) f = -90;
        s.det_floor_db = (int8_t)f;
    } else {
        s.det_floor_db = 0;
    }

    bool ok = settings_save(&s);

    send_str(sn, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; "
                 "charset=utf-8\r\nConnection: close\r\n\r\n");
    if (!ok) {
        send_str(sn, "플래시 쓰기에 실패했습니다. 설정은 그대로입니다.");
    } else if (s.use_dhcp) {
        /* Nobody can be told an address that has not been handed out yet, so
         * say where it will appear instead of pretending to know it. */
        send_str(sn, "저장했습니다. 3초 뒤 재시작하며 공유기에서 주소를 "
                     "받습니다. 새 주소는 Discord 가동 메시지에 표시됩니다.");
    } else {
        send_str(sn, "저장했습니다. 3초 뒤 재시작합니다. "
                     "주소를 바꿨다면 새 주소로 접속하세요.");
    }

    /*
     * Restart rather than re-apply in place. The listening sockets, the chip's
     * address and the alert's link all read these values at startup, and
     * changing them underneath a running server means closing the connection
     * that is still sending this reply. A reboot is one well-understood path
     * instead of several fragile ones.
     *
     * Three seconds so the reply actually leaves the chip first.
     */
    if (ok) s_reboot_at_ms = now_ms() + 3000;
}

/* ---------------------------------------------------------------- streaming */

static void send_snapshot(uint8_t sn)
{
    static char hdr[160];

    uint32_t len = capture();
    if (len == 0) {
        send_str(sn, "HTTP/1.1 503 Service Unavailable\r\n"
                     "Content-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }

    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
        "Content-Length: %lu\r\nCache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", (unsigned long)len);

    send_all(sn, hdr, (uint16_t)n);
    send_blob(sn, image_buff, len);
}

/*
 * One pass of the stream: either start a frame or push a piece of one.
 *
 * Returns false when the connection should be handed back to the listener pool.
 * Never waits for the network, and never captures while a frame is still going
 * out - which is also what keeps image_buff safe to point into.
 */
static bool stream_pump(uint8_t sn, conn_t *c)
{
    static char part[160];

    if (c->tx_left == 0) {
        if (now_ms() - c->last_frame_ms < STREAM_MIN_INTERVAL_MS) return true;
        c->last_frame_ms = now_ms();

        STAGE(ST_CAPTURE);
        uint64_t t0 = time_us_64();
        uint32_t len = capture();
        STAGE(ST_POLL);
        s_fr_cap_this = time_us_64() - t0;
        s_fr_start_us = t0;
        if (len == 0) return true;  /* a dropped frame is not a dead client */

        int n = snprintf(part, sizeof(part),
            "\r\n--" MJPEG_BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
            "Content-Length: %lu\r\n\r\n", (unsigned long)len);

        /* The part header is 60-odd bytes and always fits, so the blocking
         * helper is honest here in a way it is not for the picture. */
        send_all(sn, part, (uint16_t)n);

        c->tx_p        = image_buff;
        c->tx_left     = len;
        c->tx_start_ms = now_ms();
        return true;
    }

    uint16_t want = (c->tx_left > TX_CHUNK) ? TX_CHUNK : (uint16_t)c->tx_left;

    STAGE(ST_SEND);
    int32_t  n    = send(sn, (uint8_t *)c->tx_p, want);
    STAGE(ST_POLL);

    if (n < 0) return false;        /* the socket is gone */

    if (n == 0) {                   /* SOCK_BUSY - no room yet, come back */
        if (now_ms() - c->tx_start_ms > TX_TIMEOUT_MS) {
            printf("[web] stream client stopped reading, dropping it\n");
            return false;
        }
        return true;
    }

    c->tx_p    += n;
    c->tx_left -= (uint32_t)n;

    if (c->tx_left == 0) {
        uint64_t frame_us = time_us_64() - s_fr_start_us;

        s_fr_count++;
        s_fr_cap_us  += s_fr_cap_this;
        s_fr_send_us += (frame_us > s_fr_cap_this) ? frame_us - s_fr_cap_this : 0;
        s_fr_bytes   += (uint32_t)(c->tx_p - image_buff);

        if (now_ms() - s_fr_report_ms >= 2000) {
            uint32_t span = now_ms() - s_fr_report_ms;

            printf("[cam] %.1f fps   capture %lu ms   send %lu ms   "
                   "frame %lu KB\n",
                   (double)s_fr_count * 1000.0 / (double)span,
                   (unsigned long)(s_fr_cap_us  / 1000u / s_fr_count),
                   (unsigned long)(s_fr_send_us / 1000u / s_fr_count),
                   (unsigned long)(s_fr_bytes / s_fr_count / 1024u));

            s_fr_count = 0; s_fr_bytes = 0;
            s_fr_cap_us = 0; s_fr_send_us = 0;
            s_fr_report_ms = now_ms();
        }
    }

    return (getSn_SR(sn) == SOCK_ESTABLISHED);
}

/* ------------------------------------------------------------------ routing */

static void handle_request(uint8_t sn, conn_t *c, char *req)
{
    s_last_request_ms = now_ms();

    char *sp = strchr(req, ' ');
    if (!sp) { send_404(sn); return; }

    char *path = sp + 1;
    char *end  = strchr(path, ' ');
    if (end) *end = '\0';

    if (!token_ok(path) && !peer_is_local(sn)) {
        /* Usually the browser asking for /favicon.ico, which has no token and
         * never will. Printing the path separates that from a real probe. */
        printf("[web] 404 %.48s\n", path);
        send_404(sn);
        return;
    }

    if (strncmp(path, "/api/status", 11) == 0) { send_status(sn); return; }
    if (strncmp(path, "/api/config", 11) == 0) { send_config(sn); return; }

    if (strncmp(path, "/api/res", 8) == 0) {
        const char *v = strstr(path, "v=");
        bool ok = v ? apply_res(atoi(v + 2)) : false;

        send_str(sn, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; "
                     "charset=utf-8\r\nConnection: close\r\n\r\n");
        send_str(sn, ok ? settings_res_name(g_set.res)
                        : "해상도 변경 실패");
        return;
    }

    if (strncmp(path, "/api/mic", 8) == 0) {
        /*
         * Applied at once and not stored. Somebody is watching a meter while
         * they drag a slider; a value that only took effect after a restart
         * would make that impossible. SAVE is what makes it survive a power cut.
         */
        const char *f = strstr(path, "fl=");

        if (f) mic_set_floor_db(atoi(f + 3));

        send_str(sn, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; "
                     "charset=utf-8\r\nConnection: close\r\n\r\n적용됨");
        return;
    }

    if (strncmp(path, "/api/save", 9) == 0) {
        /* The reply has to reach the phone before the reboot does, and a 40 KB
         * HD frame queued ahead of it is the one thing that can stop that. */
        drop_streams(sn);
        /* The body sits after the blank line. end was turned into a NUL to
         * isolate the path, so the search starts from what follows it. */
        char *blank = end ? strstr(end + 1, "\r\n\r\n") : NULL;
        handle_save(sn, blank ? blank + 4 : NULL);
        return;
    }

    if (strncmp(path, "/stream", 7) == 0) {
        drop_streams(sn);
        send_str(sn,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary="
            MJPEG_BOUNDARY "\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
        c->state         = CONN_STREAM;
        c->last_frame_ms = 0;
        c->tx_left       = 0;
        return;
    }

    if (strncmp(path, "/snap", 5) == 0) { send_snapshot(sn); return; }

    send_page(sn);
}

/* ------------------------------------------------------------------- socket */

static void listen_again(uint8_t sn, conn_t *c)
{
    c->state     = CONN_IDLE;
    c->kpalv_set = false;
    c->tx_p      = NULL;
    c->tx_left   = 0;

    if (getSn_SR(sn) != SOCK_CLOSED) {
        disconnect(sn);
        close(sn);
    }
    /*
     * SF_IO_NONBLOCK, which changes what send() does when the chip is full: it
     * returns SOCK_BUSY at once instead of spinning until there is room. Every
     * wait in this file is then ours, with a deadline on it, rather than
     * ioLibrary's with none.
     */
    if (socket(sn, Sn_MR_TCP, g_set.http_port, SF_IO_NONBLOCK) == sn) {
        listen(sn);
    }
}

static void service(uint8_t sn, conn_t *c)
{
    static uint8_t req[REQ_BUF_SIZE];

    switch (getSn_SR(sn)) {

    case SOCK_ESTABLISHED:
        if (getSn_IR(sn) & Sn_IR_CON) setSn_IR(sn, Sn_IR_CON);

        /*
         * Keepalive, set here rather than at listen() because the register is
         * per-connection. Without it a client that vanishes without closing - a
         * phone leaving Wi-Fi, a NAT entry on the forwarded port that expired -
         * keeps its listener until something else disturbs it, and the next
         * request finds nothing free to answer with.
         */
        if (!c->kpalv_set) {
            setSn_KPALVTR(sn, KEEPALIVE_UNITS);
            c->kpalv_set = true;
        }

        if (c->state == CONN_STREAM) {
            if (!stream_pump(sn, c)) listen_again(sn, c);
            else s_last_request_ms = now_ms();
            return;
        }

        if (getSn_RX_RSR(sn) > 0) {
            int32_t n = recv(sn, req, REQ_BUF_SIZE - 1);
            if (n > 0) {
                req[n] = '\0';
                handle_request(sn, c, (char *)req);
                if (c->state != CONN_STREAM) listen_again(sn, c);
            }
        }
        break;

    case SOCK_CLOSE_WAIT:
        disconnect(sn);
        /* fall through */
    case SOCK_CLOSED:
        listen_again(sn, c);
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------- api */

void webserver_init(void)
{
    memset(s_conn, 0, sizeof(s_conn));
    s_alarm = false;
    s_reboot_at_ms = 0;

    for (int i = 0; i < SOCK_HTTP_COUNT; i++) {
        listen_again((uint8_t)(SOCK_HTTP_BASE + i), &s_conn[i]);
    }

    s_last_request_ms = now_ms();
    printf("[web] listening on port %u, sockets %d..%d\n",
           g_set.http_port, SOCK_HTTP_BASE,
           SOCK_HTTP_BASE + SOCK_HTTP_COUNT - 1);
}

void webserver_set_alarm(bool in_alarm) { s_alarm = in_alarm; }
bool webserver_streaming(void)          { return s_alarm; }

void webserver_set_detail(int hz, float tone, float rms, float baseline)
{
    s_base = baseline;
    s_blocks++;
    if (rms > s_peak_rms) s_peak_rms = rms;

    s_hz   = hz;
    s_tone = tone;
}

void webserver_poll(void)
{
    for (int i = 0; i < SOCK_HTTP_COUNT; i++) {
        service((uint8_t)(SOCK_HTTP_BASE + i), &s_conn[i]);
    }

    if (s_reboot_at_ms && now_ms() >= s_reboot_at_ms) {
        printf("[web] restarting to apply saved settings\n");
        watchdog_reboot(0, 0, 0);
        while (true) tight_loop_contents();
    }
}

uint32_t webserver_idle_seconds(void)
{
    return (now_ms() - s_last_request_ms) / 1000u;
}
