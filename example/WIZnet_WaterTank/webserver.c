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
#include "settings.h"
#include "arducam_mega.h"

extern uint8_t image_buff[];        /* the camera driver owns this */

/* In main.c, beside the detector it writes to. */
void mic_set_thresholds(float loud_k, float enter_ratio);

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
 * always has room for it soon after the previous one left; the W6300 gives each
 * socket a 4 KB transmit buffer, so half of it leaves room for the previous
 * chunk to still be draining. */
#define TX_CHUNK    2048

/*
 * Frame cost, in two halves, because they have different cures.
 *
 *   capture   the sensor's own frame wait plus the JPEG over SPI. Bounded by
 *             the camera; a lower resolution is the only lever.
 *   send      the same bytes onto the wire. Bounded by the link and by the
 *             viewer, which on a phone over a forwarded port is the slow end.
 *
 * Printed every two seconds and only while something is watching, so an idle
 * box stays quiet.
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

static volatile float s_tone;
static volatile int   s_hz;

/*
 * What the microphone is hearing right now, for the panel's meters.
 *
 * Written by core 0 once per block and read by core 1 once a second, so a torn
 * float would cost one wrong digit for one second on a display. That is a long
 * way from worth a lock.
 */
static volatile float s_rms, s_base, s_loud_k, s_enter;

/*
 * Peak held between reads, not the last block.
 *
 * A block is 16 ms and the panel asks once every few hundred milliseconds, so
 * reporting whatever the most recent block happened to be means a shout lands
 * between two reads and never appears. Somebody adjusting a threshold by making
 * a noise at the microphone would see nothing move and conclude the microphone
 * is broken.
 *
 * Core 0 raises these and the server clears them on read: whatever the loudest
 * and most tonal moment since the last look was, that is what gets drawn.
 */
static volatile float    s_peak_rms, s_peak_tone;
static volatile int      s_peak_hz;
static volatile uint32_t s_blocks;      /* ever, so the panel can see it move */

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
    send_str(sn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Tank</title><style>"
        "body{margin:0;background:#111;color:#ddd;"
        "font:15px -apple-system,system-ui,sans-serif}"
        "header{padding:12px 14px;background:#1b1b1b;font-weight:600}"
        "nav{display:flex;background:#1b1b1b;border-bottom:1px solid #333}"
        /*
         * Every property a button can inherit or be given by the browser is set
         * here, rather than only the ones that looked wrong.
         *
         * The third tab rendered with a box around it and its label cut in half
         * while the first two were fine, and chasing that one difference cost
         * two attempts. A control that is styled by subtraction - turn off the
         * border, turn off the background - keeps whatever was not named, and
         * what was not named here was its height, its box model, its radius and
         * its platform appearance. Naming them all is both shorter to reason
         * about and the end of that class of bug.
         */
        "nav button{appearance:none;-webkit-appearance:none;"
        "flex:1 1 0;min-width:0;box-sizing:border-box;"
        "margin:0;padding:0 8px;height:46px;line-height:44px;"
        "background:transparent;border:0;border-radius:0;"
        "border-bottom:2px solid transparent;"
        "color:#888;font:inherit;white-space:nowrap;overflow:hidden;"
        "text-overflow:ellipsis;cursor:pointer}"
        "nav button.a{color:#fff;border-bottom-color:#2ecc71}"
        "nav button:focus{outline:none}"
        "nav button:focus-visible{outline:2px solid #2ecc71;outline-offset:-4px}"
        ".s{display:flex;align-items:center;gap:10px;padding:10px 14px;"
        "border-bottom:1px solid #222}"
        ".s span{flex:0 0 92px;color:#999;font-size:13px}"
        ".bar{flex:1;height:14px;border-radius:7px;background:#2a2a2a;"
        "transition:background .25s}"
        ".on-run{background:#2ecc71}"
        ".on-buz{background:#ff3b30;animation:p 1s infinite}"
        "@keyframes p{50%{opacity:.45}}"
        ".v{color:#777;font-size:12px;flex:0 0 auto;"
        "font-variant-numeric:tabular-nums}"
        "#box{background:#000;min-height:180px;display:flex;"
        "align-items:center;justify-content:center;text-align:center;"
        "color:#666;padding:28px 18px;font-size:14px;line-height:1.6}"
        "#box img{width:100%;height:auto;display:block}"
        "p.n{padding:10px 14px;color:#777;font-size:12px;margin:0}"
        ".f{padding:10px 14px;border-bottom:1px solid #222}"
        ".f label{display:block;color:#999;font-size:12px;margin-bottom:5px}"
        ".f input{width:100%;box-sizing:border-box;padding:9px 10px;"
        "background:#1d1d1d;border:1px solid #333;border-radius:6px;"
        "color:#eee;font:14px ui-monospace,monospace}"
        ".row{display:flex;gap:10px}.row>div{flex:1}"
        ".rs{display:flex;gap:8px}"
        ".rs button{flex:1;padding:11px;background:#1d1d1d;"
        "border:1px solid #333;border-radius:6px;color:#aaa;font:inherit}"
        ".rs button.a{background:#16281c;border-color:#2ecc71;color:#fff}"
        ".rs small{color:#777;font-size:11px}"
        ".cur{margin-top:8px;padding:8px 10px;background:#15201a;"
        "border-left:2px solid #2ecc71;border-radius:4px;color:#9bbfa8;"
        "font:12px ui-monospace,monospace;line-height:1.7}"
        ".act{display:flex;gap:10px;padding:14px}"
        ".act button{flex:1;padding:13px;border:none;border-radius:8px;"
        "font:600 15px inherit}"
        "#save{background:#2ecc71;color:#06220f}"
        "#ref{background:#2a2a2a;color:#ccc}"
        "#msg{padding:0 14px 16px;font-size:13px;color:#888;line-height:1.5}"
        ".h{color:#666;font-size:11px;margin-top:5px;line-height:1.5}"
        ".hide{display:none}"
        /* A meter is a filled bar with a tick where the threshold sits, so the
           question "is this sound close to firing" is answered by looking
           rather than by comparing two numbers. */
        ".mt{position:relative;height:18px;border-radius:4px;background:#1d1d1d;"
        "border:1px solid #333;overflow:hidden;margin-top:6px}"
        ".mt i{position:absolute;left:0;top:0;bottom:0;background:#2ecc71;"
        "transition:width .3s}"
        ".mt u{position:absolute;top:-2px;bottom:-2px;width:2px;"
        "background:#ff3b30;text-decoration:none}"
        ".mt.over i{background:#ff3b30}"
        ".rd{display:flex;justify-content:space-between;align-items:baseline;"
        "font:12px ui-monospace,monospace;color:#888;margin-top:5px}"
        ".rd b{color:#eee;font-size:15px;font-weight:600}"
        "input[type=range]{width:100%;margin:10px 0 0;accent-color:#2ecc71}"
        ".sv{color:#2ecc71;font:600 13px ui-monospace,monospace}"
        "</style></head><body>"
        "<header>저수조 모니터</header>"
        "<nav><button id=\"tl\" class=\"a\">LIVE</button>"
        "<button id=\"tn\">NETWORK</button>"
        "<button id=\"tm\">MIC / CAM</button></nav>"

        "<div id=\"live\">"
        "<div class=\"s\"><span>RUN</span><div id=\"r\" class=\"bar\"></div>"
        "<div class=\"v\" id=\"rv\">-</div></div>"
        "<div class=\"s\"><span>BUZZER</span><div id=\"b\" class=\"bar\"></div>"
        "<div class=\"v\" id=\"bv\">-</div></div>"
        "<div id=\"box\"></div>"
        "<p class=\"n\" id=\"note\"></p>"
        "</div>"

        "<div id=\"net\" class=\"hide\">"
        "<div class=\"f\"><label>주소 방식</label>"
        "<div class=\"rs\">"
        "<button id=\"m1\">DHCP <small>자동</small></button>"
        "<button id=\"m0\">STATIC <small>고정</small></button></div>"
        "<div class=\"h\">아래 네 칸은 지금 동작 중인 값입니다. "
        "DHCP는 공유기에서 주소를 받고, STATIC은 아래 값을 그대로 씁니다. "
        "바꾸면 SAVE 후 재시작해야 적용됩니다.</div></div>"
        "<div class=\"f\"><label>내부 IP</label><input id=\"ip\"></div>"
        "<div class=\"f row\">"
        "<div><label>서브넷</label><input id=\"sn\"></div>"
        "<div><label>게이트웨이</label><input id=\"gw\"></div></div>"
        "<div class=\"f row\">"
        "<div><label>DNS</label><input id=\"dns\"></div>"
        "<div><label>내부 포트</label><input id=\"port\"></div></div>"
        "<div class=\"f row\">"
        "<div><label>외부 IP</label><input id=\"ph\"></div>"
        "<div><label>외부 포트</label><input id=\"pp\"></div></div>"
        "<div class=\"f\"><label>Discord Webhook URL</label>"
        "<input id=\"wh\" autocomplete=\"off\">"
        "<div class=\"h\" id=\"whs\"></div></div>"
        "<div class=\"act\"><button id=\"ref\">REFRESH</button>"
        "<button id=\"save\">SAVE</button></div>"
        "<div id=\"msg\"></div>"
        "</div>"

        /* ------------------------------------------------- mic / camera ---
           Two meters above two sliders, and the meters update once a second
           whether or not anything is being adjusted.
           A threshold is not a number somebody can pick from a manual - it is
           the gap between what this room sounds like and what its buzzer sounds
           like, and nobody knows that gap until they stand in the room and look
           at it. So the panel shows the live value, marks where the threshold
           sits, and lets the slider move the mark. */
        "<div id=\"mic\" class=\"hide\">"

        "<div class=\"f\"><label>음량 — 평소 대비 몇 배</label>"
        "<div class=\"mt\" id=\"lm\"><i id=\"lf\"></i><u id=\"lt\"></u></div>"
        "<div class=\"rd\"><span>지금 <b id=\"lv\">-</b> 배</span>"
        "<span id=\"ldb\">-</span></div>"
        "<input type=\"range\" id=\"ls\" min=\"1.2\" max=\"6\" step=\"0.1\">"
        "<div class=\"h\">경보 기준 <span class=\"sv\" id=\"lsv\">-</span> 배 "
        "&nbsp;·&nbsp; 빨간 선이 기준입니다. 부저를 울렸을 때 막대가 선을 "
        "넘고, 조용할 때 넘지 않는 자리로 맞추세요.</div></div>"

        "<div class=\"f\"><label>음조성 — 한 주파수가 얼마나 도드라지나</label>"
        "<div class=\"mt\" id=\"tm\"><i id=\"tf\"></i><u id=\"tt\"></u></div>"
        "<div class=\"rd\"><span>지금 <b id=\"tv\">-</b></span>"
        "<span id=\"thz\">-</span></div>"
        "<input type=\"range\" id=\"ts\" min=\"5\" max=\"60\" step=\"1\">"
        "<div class=\"h\">경보 기준 <span class=\"sv\" id=\"tsv\">-</span> "
        "&nbsp;·&nbsp; 삐- 하는 맑은 소리에 반응합니다. 넓게 퍼지는 소리에는 "
        "오르지 않으니, 그런 부저라면 위의 음량 쪽으로 맞추세요.</div></div>"

        "<p class=\"n\">마이크 입력 <b id=\"blk\">-</b> "
        "<small>(정상이면 62 blk/s 근처입니다. 0이면 마이크를 못 읽는 중입니다.)"
        "</small></p>"
        "<p class=\"n\">두 기준 중 <b>하나만 넘어도 경보</b>입니다. "
        "슬라이더는 움직이는 즉시 반영되고, 재시작 후에도 유지하려면 "
        "아래 SAVE를 누르세요.</p>"

        "<div class=\"f\"><label>카메라 해상도</label>"
        "<div class=\"rs\">"
        "<button id=\"r1\">QVGA <small>320&times;240</small></button>"
        "<button id=\"r2\">VGA <small>640&times;480</small></button>"
        "<button id=\"r3\">HD <small>1280&times;720</small></button></div>"
        "<div class=\"h\">누르면 바로 적용됩니다. 재시작 후에도 그 해상도로 "
        "올라오게 하려면 SAVE 하세요. 해상도가 높을수록 한 장을 찍어 보내는 "
        "시간이 길어져 영상이 느려집니다.</div></div>"

        "<div class=\"act\"><button id=\"ref2\">REFRESH</button>"
        "<button id=\"save2\">SAVE</button></div>"
        "<div id=\"msg2\"></div>"
        "</div>"

        "<script>");

    /* The token and the stand-down time are the two things the page cannot
     * work out for itself. */
    {
        static char head[320];
        snprintf(head, sizeof(head),
            "var T='%s';"
            "var NOTE='부저음이 %d초간 들리지 않으면 경보가 해제됩니다. "
            "영상은 항상 표시됩니다.';",
            STREAM_TOKEN, (int)CLEAR_SECONDS);
        send_str(sn, head);
    }

    send_str(sn,
        "var live=1,tabn=0,RES=1,armed=0;"
        "function q(i){return document.getElementById(i)}"

        /*
         * The video starts with the page and is never taken down.
         *
         * It used to be swapped in and out by the alarm, which meant the one
         * moment someone most wanted to look - walking up to the box to find out
         * why it had been quiet - was a moment with nothing to see. A control
         * panel that only works during an emergency cannot be checked.
         *
         * Reloading the <img> is also how the stream recovers: if the connection
         * dies the browser fires onerror, and a fresh src with a new cache-buster
         * asks for another one.
         */
        "function startStream(){q('box').innerHTML="
        "'<img id=\"v\" src=\"/stream?t='+T+'&c='+Date.now()+'\">';"
        "q('v').onerror=function(){setTimeout(startStream,2000)};}"

        /* The lit button is the stored value - there is no hidden field and no
         * third state. Whatever the board reported is what shows, so the current
         * mode can be read off the panel without pressing anything. */
        /*
         * Unlike the resolution buttons, these do not act at once. The address
         * is what every listening socket was opened on and what the phone is
         * connected through, so changing it means a restart - and a restart that
         * happens the moment somebody taps a button, before they have been told
         * what the new address will be, is a box that disappears mid-sentence.
         * SAVE is where that happens, with a message first.
         */
        "var MODE=0;"
        "function setMode(v){MODE=v;"
        "q('m1').className=(v?'a':'');q('m0').className=(v?'':'a');}"
        "q('m1').onclick=function(){setMode(1)};"
        "q('m0').onclick=function(){setMode(0)};"

        "function setRes(v){RES=v;"
        "q('r1').className=(v==1?'a':'');q('r2').className=(v==2?'a':'');"
        "q('r3').className=(v==3?'a':'');}"

        /* Pressing a button changes the sensor there and then, and the video is
           reloaded because the connection it was on was dropped to let the
           sensor be reconfigured. SAVE is what makes the choice survive a power
           cut; it is not what makes it happen. */
        "function pickRes(v){setRes(v);q('msg').textContent='해상도 변경 중...';"
        "fetch('/api/res?v='+v+'&t='+T,{cache:'no-store'})"
        ".then(function(r){return r.text()}).then(function(t){"
        "q('msg').textContent=t+' 적용됨';startStream();})"
        ".catch(function(){q('msg').textContent='해상도 변경 실패';});}"
        "q('r1').onclick=function(){pickRes(1)};"
        "q('r2').onclick=function(){pickRes(2)};"
        "q('r3').onclick=function(){pickRes(3)};"
        /* Three tabs now. `live` still means "the video is on screen", which
           is what decides whether the stream element exists; `tabn` is which
           panel is showing. The microphone tab keeps polling even though the
           video is hidden, because its meters are the whole point of it. */
        "function tab(n){tabn=n;live=(n==0);"
        "q('tl').className=n==0?'a':'';q('tn').className=n==1?'a':'';"
        "q('tm').className=n==2?'a':'';"
        "q('live').className=n==0?'':'hide';"
        "q('net').className=n==1?'':'hide';"
        "q('mic').className=n==2?'':'hide';"
        "if(n==1)loadCfg();}"
        "q('tl').onclick=function(){tab(0)};"
        "q('tn').onclick=function(){tab(1)};"
        "q('tm').onclick=function(){tab(2)};"

        /* Polling stops while the config tab is open. Nothing on that tab shows
         * live state, and a request every second would compete for the same
         * four sockets as the save. */
        /* The network tab is the only one that stops polling: nothing on it
           moves, and a request a second would compete with the save for the
           same six sockets. */
        "function poll(){if(tabn==1)return;"
        "fetch('/api/status?t='+T,{cache:'no-store'}).then(function(r){"
        "return r.json()}).then(function(s){"
        "q('r').className='bar on-run';q('rv').textContent=s.up+'s';"
        "q('b').className=s.buzzer?'bar on-buz':'bar';"
        "q('bv').textContent=s.buzzer?(s.hz+'Hz'):('tone '+s.tone);"
        "q('note').textContent=s.buzzer?NOTE:'';"
        "meters(s);"
        "}).catch(function(){q('r').className='bar';"
        "q('rv').textContent='응답 없음';});}"

        /* Both meters are drawn the same way: the bar is the live value as a
           share of the scale, and the tick is the threshold on that same scale.
           Keeping the scale a little above the threshold means the tick never
           sits at the very edge, where it would be impossible to judge. */
        "function meter(bar,fill,tick,val,thr,scale){"
        "var w=Math.max(0,Math.min(100,val/scale*100));"
        "q(fill).style.width=w+'%';"
        "q(tick).style.left=Math.min(100,thr/scale*100)+'%';"
        "q(bar).className=val>=thr?'mt over':'mt';}"

        "function meters(s){"
        "if(!armed){q('ls').value=s.loud;q('ts').value=s.enter;armed=1;}"
        "var lthr=parseFloat(q('ls').value),tthr=parseFloat(q('ts').value);"
        "q('lsv').textContent=lthr.toFixed(1);"
        "q('tsv').textContent=tthr.toFixed(0);"
        "q('lv').textContent=s.x.toFixed(1);"
        "q('tv').textContent=s.tone.toFixed(1);"
        "q('ldb').textContent=s.db.toFixed(0)+' dB  (평소 '+s.basedb.toFixed(0)+' dB)';"
        "q('thz').textContent=s.hz+' Hz';"
        "q('blk').textContent=s.blk+' blk/s';"
        "q('blk').style.color=s.blk>40?'#2ecc71':'#ff3b30';"
        "meter('lm','lf','lt',s.x,lthr,Math.max(lthr*1.6,4));"
        "meter('tm','tf','tt',s.tone,tthr,Math.max(tthr*1.6,20));}"

        /* Sent on release rather than on every pixel of the drag: each one is a
           request, and the board has six sockets. */
        "function pushMic(){q('msg2').textContent='적용 중...';"
        "fetch('/api/mic?loud='+q('ls').value+'&enter='+q('ts').value+'&t='+T)"
        ".then(function(r){return r.text()}).then(function(t){"
        "q('msg2').textContent=t+' — 재시작 후에도 쓰려면 SAVE 하세요.';})"
        ".catch(function(){q('msg2').textContent='적용 실패';});}"
        "q('ls').onchange=pushMic;q('ts').onchange=pushMic;"

        "function loadCfg(){"
        "fetch('/api/config?t='+T,{cache:'no-store'}).then(function(r){"
        "return r.json()}).then(function(c){"
        "q('ip').value=c.ip;q('sn').value=c.sn;q('gw').value=c.gw;"
        "q('dns').value=c.dns;q('port').value=c.port;"
        "q('ph').value=c.ph;q('pp').value=c.pp;setRes(c.res);"
        "setMode(c.dhcp);"
        "q('whs').textContent=c.whset?"
        "'현재 저장되어 있습니다. 비워두면 그대로 유지됩니다.':"
        "'설정되어 있지 않습니다. 알림을 받으려면 URL을 붙여넣으세요.';"
        "q('wh').value='';"
        "q('msg').textContent='';});}"

        "q('ref').onclick=function(){q('msg').textContent='불러오는 중...';"
        "loadCfg();};"

        /* Both tabs save the whole settings block, because it is one record in
           flash and a partial write is not a thing. Which button was pressed
           only decides which line the answer appears on. */
        "function doSave(where){"
        "var p=['ip','sn','gw','dns','port','ph','pp','wh'].map(function(k){"
        "return k+'='+encodeURIComponent(q(k).value)}).join('&')"
        "+'&res='+RES+'&dhcp='+MODE"
        "+'&loud='+q('ls').value+'&enter='+q('ts').value;"
        "q(where).textContent='저장 중...';"
        "fetch('/api/save?t='+T,{method:'POST',body:p}).then(function(r){"
        "return r.text()}).then(function(t){q(where).textContent=t;"
        /* The board restarts after a save because the address it listens on is
         * one of the things being changed. Saying so beats a page that simply
         * stops answering. */
        "}).catch(function(){q(where).textContent="
        "'전송 실패 - 이미 재시작했을 수 있습니다.';});}"
        "q('save').onclick=function(){doSave('msg')};"
        "q('save2').onclick=function(){doSave('msg2')};"
        "q('ref2').onclick=function(){armed=0;q('msg2').textContent="
        "'보드에 저장된 값을 다시 읽었습니다.';};"

        /* Three times a second while the meters are on screen, once a second
           otherwise. Adjusting a threshold means making a noise and watching
           the bar answer, and a bar that answers a second later cannot be aimed
           with. */
        "function beat(){poll();"
        "setTimeout(beat,tabn==2?300:1000);}"
        "startStream();loadCfg();beat();"
        "</script></body></html>");
}

/* -------------------------------------------------------------------- api */

static void send_status(uint8_t sn)
{
    static char body[384];
    static char hdr[128];

    /*
     * Level as dBFS, because that is the unit anybody adjusting a microphone
     * already thinks in - a bare 0.0043 means nothing to read off a screen.
     * Full scale is 0 dB, so a quiet room lands near -47 and the buzzer near
     * -38. The ratio against the learned background is sent as well, since that
     * is the number the threshold is actually compared with.
     */
    float rms  = s_peak_rms;
    float tone = s_peak_tone;
    int   hz   = s_peak_hz;
    float base = s_base > 0.0f ? s_base : 0.0001f;
    float db   = (rms > 0.0000001f) ? 20.0f * log10f(rms) : -99.0f;
    float bdb  = 20.0f * log10f(base);

    /* Cleared on read, so the next window starts from nothing. */
    s_peak_rms  = 0.0f;
    s_peak_tone = 0.0f;

    /*
     * How many blocks the detector has seen. A panel that shows a level of zero
     * cannot say whether the room is silent or the microphone is not being read
     * at all - two very different faults that look identical. This number moving
     * is the difference.
     */
    static uint32_t last_blocks;
    static uint32_t last_ms;
    uint32_t span = now_ms() - last_ms;
    uint32_t bps  = span ? (s_blocks - last_blocks) * 1000u / span : 0;
    last_blocks = s_blocks;
    last_ms     = now_ms();

    int n = snprintf(body, sizeof(body),
        "{\"run\":1,\"buzzer\":%d,\"stream\":1,\"hz\":%d,"
        "\"tone\":%.1f,\"up\":%lu,"
        "\"db\":%.1f,\"basedb\":%.1f,\"x\":%.2f,"
        "\"loud\":%.2f,\"enter\":%.0f,\"blk\":%lu}",
        s_alarm ? 1 : 0, hz, (double)tone,
        (unsigned long)(now_ms() / 1000u),
        (double)db, (double)bdb, (double)(rms / base),
        (double)s_loud_k, (double)s_enter,
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
        "\"dhcp\":%u,\"whset\":%u}",
        live.ip[0],  live.ip[1],  live.ip[2],  live.ip[3],
        live.sn[0],  live.sn[1],  live.sn[2],  live.sn[3],
        live.gw[0],  live.gw[1],  live.gw[2],  live.gw[3],
        live.dns[0], live.dns[1], live.dns[2], live.dns[3],
        g_set.http_port, g_set.pub_host, g_set.pub_port, g_set.res,
        g_set.use_dhcp, g_set.webhook[0] ? 1u : 0u);

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

    if (form_get(body, "loud", v, sizeof(v))) {
        float f = (float)atof(v);
        if (f >= 1.2f && f <= 8.0f) s.loud_k = f;
    }
    if (form_get(body, "enter", v, sizeof(v))) {
        float f = (float)atof(v);
        if (f >= 5.0f && f <= 80.0f) s.enter_ratio = f;
    }

    if (form_get(body, "wh", v, sizeof(v))) {
        strncpy(s.webhook, v, SETTINGS_WEBHOOK_MAX - 1);
        s.webhook[SETTINGS_WEBHOOK_MAX - 1] = '\0';
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

        uint64_t t0 = time_us_64();
        uint32_t len = capture();
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
    int32_t  n    = send(sn, (uint8_t *)c->tx_p, want);

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
         * Applied immediately and not stored. Somebody is watching a meter
         * while they drag a slider, and a value that only took effect after a
         * restart would make that impossible. SAVE is what makes it survive a
         * power cut.
         */
        const char *l = strstr(path, "loud=");
        const char *t = strstr(path, "enter=");

        mic_set_thresholds(l ? (float)atof(l + 5) : 0.0f,
                           t ? (float)atof(t + 6) : 0.0f);

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

void webserver_set_detail(int hz, float tone, float rms, float baseline,
                          float loud_k, float enter_ratio)
{
    s_hz     = hz;
    s_tone   = tone;
    s_rms    = rms;
    s_base   = baseline;
    s_loud_k = loud_k;
    s_enter  = enter_ratio;

    s_blocks++;
    if (rms  > s_peak_rms)  s_peak_rms  = rms;
    if (tone > s_peak_tone) { s_peak_tone = tone; s_peak_hz = hz; }
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
