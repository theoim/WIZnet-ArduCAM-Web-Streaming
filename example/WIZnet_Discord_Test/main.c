/*
 * Discord webhook from a W6300-EVB-Pico2 - standalone.
 *
 * One question: can this board post a line of text to a Discord channel over
 * TLS? Everything else in the water-tank product sits on top of that answer, so
 * it gets settled on its own, with no camera and no microphone in the way.
 *
 * What it does: brings up the network, posts three messages a few seconds
 * apart, then stops. Three rather than one because the interesting failures are
 * not in the first attempt - a handshake that works once and then runs the
 * board out of sockets or heap would pass a single-shot test and fail in the
 * field.
 *
 * Before flashing, put a webhook URL in DISCORD_WEBHOOK_URL below. In Discord:
 * channel settings, Integrations, Webhooks, New Webhook, Copy Webhook URL.
 *
 * That URL is a credential. Anyone who has it can post to the channel, so it
 * does not belong in a commit.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "wizchip_conf.h"
#include "w6300.h"
#include "socket.h"
#include "wizchip_spi.h"
#include "dns.h"

#include "wiz_claw_net.h"
#include "discord.h"
#include "test_image.h"

/* --------------------------------------------------------------- settings */

/*
 * A webhook URL is a password for a Discord channel: anyone who has it can post
 * there, and deleting it from a file later does not remove it from the history
 * of the repository. It lives in secrets.h, which is not committed - copy the
 * water tank example's secrets.h.example, or paste a URL here while testing and
 * take it out before committing.
 */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

#ifndef DISCORD_WEBHOOK_URL
#define DISCORD_WEBHOOK_URL   "REPLACE_WITH_YOUR_WEBHOOK_URL"
#endif

#define SYS_CLOCK_KHZ       200000      /* same as the exhibition build */

/*
 * Static rather than DHCP, deliberately, and only for this test. One fewer
 * thing to be wrong while finding out whether TLS works at all - a board that
 * never got a lease and a board that cannot handshake look identical from the
 * console until the addresses are printed.
 *
 * The product should use DHCP: a basement router hands out what it hands out.
 */
static wiz_NetInfo g_net_info = {
    .mac  = { 0x00, 0x08, 0xDC, 0x12, 0x34, 0x58 },
    .ip   = { 192, 168, 11, 7 },
    .sn   = { 255, 255, 255, 0 },
    .gw   = { 192, 168, 11, 1 },
    .dns  = { 8, 8, 8, 8 },
    .dhcp = NETINFO_STATIC,
};

static struct repeating_timer g_1s_timer;

static bool timer_1s_cb(struct repeating_timer *t)
{
    (void)t;
    wiz_claw_net_1s_tick();     /* DNS retransmission timer */
    return true;
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);
    stdio_init_all();
    sleep_ms(2000);

    printf("\nDiscord webhook test\n");
    printf("--------------------\n");
    printf("sys clock : %u Hz\n", clock_get_hz(clk_sys));

    if (strstr(DISCORD_WEBHOOK_URL, "REPLACE")) {
        printf("\nFAIL: set DISCORD_WEBHOOK_URL in main.c first.\n");
        printf("Discord -> channel settings -> Integrations -> Webhooks\n");
        while (true) tight_loop_contents();
    }

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    /*
     * Socket budget. The W6300 has eight, and they are a fixed resource rather
     * than a pool that grows:
     *
     *   5   DNS        wiz_claw_net
     *   0   HTTPS      opened and closed per post by wiz_claw_http
     *
     * The product will also want sockets for its own web server, which is why
     * this is written down rather than left to whatever each module grabs.
     */
    wiz_claw_net_config_t net_cfg = {
        .dns_server_ip = { 8, 8, 8, 8 },
        .dns_socket    = 5,
        .dhcp_socket   = 0,
    };
    wiz_claw_net_init(&net_cfg);

    /* DNS retransmits on a one-second tick; without this a lookup that drops a
     * packet never retries and the post fails with a resolve error. */
    add_repeating_timer_ms(-1000, timer_1s_cb, NULL, &g_1s_timer);

    wizchip_setnetinfo(&g_net_info);
    wiz_claw_net_print_info(&g_net_info);

    printf("\nPosting three messages.\n\n");

    static const char *messages[] = {
        "[test 1/3] W6300-EVB-Pico2 reporting in. TLS handshake worked.",
        "[test 2/3] 두 번째 메시지입니다. 한글과 \"따옴표\", 줄바꿈도\n확인합니다.",
        "[test 3/3] Third and last. If all three arrived, the transport is good.",
    };

    int sent = 0;

    for (int i = 0; i < 3; i++) {
        printf("posting %d/3 ...\n", i + 1);

        absolute_time_t t0 = get_absolute_time();
        bool ok = discord_post_text(DISCORD_WEBHOOK_URL, messages[i]);
        int64_t ms = absolute_time_diff_us(t0, get_absolute_time()) / 1000;

        printf("  %s  status %d, %lld ms\n",
               ok ? "OK  " : "FAIL", discord_last_status(), ms);

        if (ok) sent++;

        /* Discord allows roughly five posts per two seconds per webhook. Three
         * seconds keeps this well clear of that while still being quick enough
         * that a socket or heap leak would show up as the third attempt being
         * slower than the first. */
        sleep_ms(3000);
    }

    printf("\n%d of 3 delivered.\n", sent);

    /*
     * The fourth message carries a picture, which is the shape the product
     * actually sends: an alarm is a line of text and a frame, and they go in
     * one request so the owner gets both at once rather than six seconds apart.
     *
     * The image is compiled in rather than captured, so a failure here belongs
     * to the upload and not to the camera.
     */
    if (sent == 3) {
        printf("\nposting 4/4 - text and a %u byte JPEG in one request ...\n",
               test_jpeg_len);

        sleep_ms(3000);

        absolute_time_t t0 = get_absolute_time();
        bool ok = discord_post_photo(DISCORD_WEBHOOK_URL,
                                     "[test 4/4] 🔴 수위 경보 예시입니다. "
                                     "텍스트와 사진이 한 번의 요청으로 갑니다.",
                                     test_jpeg, test_jpeg_len,
                                     "snapshot.jpg");
        int64_t ms = absolute_time_diff_us(t0, get_absolute_time()) / 1000;

        printf("  %s  status %d, %lld ms\n",
               ok ? "OK  " : "FAIL", discord_last_status(), ms);

        if (ok) {
            printf("\nTransport is complete: text, and text with an image.\n");
            printf("Check Discord - the grid and border should be intact.\n");
            printf("Corruption there would mean a length or boundary fault.\n");
        }
    }

    if (sent != 3) {
        printf("\nWhat the numbers mean:\n");
        printf("  status  -1   never got a reply - DNS, routing or handshake\n");
        printf("  status 401   webhook URL wrong\n");
        printf("  status 404   webhook deleted, or the URL has a typo\n");
        printf("  status 429   rate limited, posting too fast\n");
    }

    while (true) tight_loop_contents();
}
