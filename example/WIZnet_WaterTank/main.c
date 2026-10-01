/*
 * Basement water tank monitor.
 *
 * A tank has a buzzer that sounds when the water rises. Nobody can be there to
 * hear it. This board listens for that buzzer, and when it hears it, sends a
 * message and a picture to Discord and opens a live view the owner can reach
 * from a phone.
 *
 * ---------------------------------------------------------------------------
 *
 * The one thing worth knowing before reading the rest: it does not listen for
 * loudness. Measured on the bench,
 *
 *   a clap           was sixteen times louder than the room
 *   a 2 kHz tone     was not louder than the room at all
 *
 * so a level threshold fires on the thing to ignore and misses the thing to
 * catch. What separates them is shape: a buzzer puts nearly all its energy at
 * one frequency and a room spreads it everywhere. Sorted that way the same two
 * sounds are seventy times apart. See goertzel.h and detector.h.
 *
 * ---------------------------------------------------------------------------
 *
 *   WATCHING   microphone only. No camera, no open ports.
 *      |
 *      |  a tone, held
 *      v
 *   ALARM      capture a frame, post it with the text and a link,
 *              open the live view
 *      |
 *      |  silence for CLEAR_SECONDS, then the view idles out
 *      v
 *   WATCHING   stand-down message, close the port, camera off
 *
 * Commissioning: fill in config.h. Webhook, token, public URL, addresses.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#include "wizchip_conf.h"
#include "w6300.h"
#include "socket.h"
#include "wizchip_spi.h"
#include "dns.h"

#include "config.h"
#include "wiz_claw_net.h"
#include "discord.h"
#include "webserver.h"
#include "settings.h"
#include "goertzel.h"
#include "detector.h"
#include "arducam_mega.h"
#include "i2s_rx.pio.h"

#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/irq.h"

extern uint8_t image_buff[];

/*
 * settings.h names the two modes without including the camera header, so the
 * stored byte and the driver enum are two separate facts. Check them against
 * each other here: if the driver ever reorders res_t, this fails at build time
 * instead of shipping a board that quietly comes up in the wrong mode.
 */
_Static_assert(SETTINGS_RES_QVGA == RES_320X240,  "stored QVGA value drifted");
_Static_assert(SETTINGS_RES_VGA  == RES_640X480,  "stored VGA value drifted");
_Static_assert(SETTINGS_RES_HD   == RES_1280X720, "stored HD value drifted");

/* ------------------------------------------------------------------ audio */

#define SAMPLE_RATE     16000

/*
 * 256 frames is one analysis block - it sets the Goertzel bin width and the
 * detector's idea of time, so it is not free to change.
 *
 * What is free to change is how many of them one DMA buffer holds, and that is
 * the whole slack the audio path has. The number that matters is not the slack
 * across the pair of buffers - it is the span of ONE buffer, because a capture
 * that runs longer than a single buffer takes overwrites it whatever the other
 * one is doing.
 *
 * Measured with an HD stream running at four blocks a buffer: 24 to 36 blocks a
 * second collected out of 62, with the overrun counter climbing by eleven every
 * second. One HD frame is a sensor wait plus 40 KB over SPI plus 40 KB on the
 * wire, about 100 ms, against a 64 ms buffer - so every frame cost a buffer, and
 * five frames a second cost most of the audio. With the stream closed the same
 * build collected 60 to 68.
 *
 * Eight blocks is 128 ms a buffer, which a 100 ms capture fits inside. The cost
 * is 32 KB of RAM and up to 128 ms before a block is analysed; a buzzer that has
 * been sounding for two seconds does not care.
 */
#define FRAMES_PER_BUF  256
#define BLOCKS_PER_BUF  8
#define WORDS_PER_BLOCK (FRAMES_PER_BUF * 2)
#define WORDS_PER_BUF   (WORDS_PER_BLOCK * BLOCKS_PER_BUF)

static int32_t  s_buf[2][WORDS_PER_BUF];
static uint     s_dma_chan[2];
static volatile uint32_t s_ready_mask;
static volatile uint32_t s_overrun;
static PIO      s_pio;
static uint     s_sm;

static goertzel_bank_t s_bank;
static goertzel_bank_t s_bank_low;      /* log only - see config.h */
static detector_t      s_det;

static void dma_handler(void)
{
    for (int i = 0; i < 2; i++) {
        if (dma_hw->ints0 & (1u << s_dma_chan[i])) {
            dma_hw->ints0 = 1u << s_dma_chan[i];
            if (s_ready_mask & (1u << i)) s_overrun++;
            s_ready_mask |= (1u << i);
            dma_channel_set_write_addr(s_dma_chan[i], s_buf[i], false);
        }
    }
}

/* Philips I2S puts the MSB one clock after the word-select edge, so the window
 * the state machine captures is a bit late and the sign lands at bit 30. Read
 * as `word >> 8` the signal never goes negative and the DC offset parks at half
 * scale, which is a distinctive enough symptom to name here. */
static inline int32_t to_sample(int32_t w)
{
    return ((int32_t)((uint32_t)w << 1)) >> 8;
}

static void audio_init(void)
{
    uint offset;

    /* Nothing names a PIO block or DMA channel: the camera takes pio0 sm0
     * outright and the W6300's QSPI is a PIO program too. */
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(
            &i2s_rx_program, &s_pio, &s_sm, &offset, PIN_I2S_BCLK, 3, true)) {
        printf("FATAL: no PIO state machine for the microphone\n");
        while (true) tight_loop_contents();
    }

    i2s_rx_program_init(s_pio, s_sm, offset, PIN_I2S_BCLK, PIN_I2S_DATA,
                        SAMPLE_RATE);

    s_dma_chan[0] = dma_claim_unused_channel(true);
    s_dma_chan[1] = dma_claim_unused_channel(true);

    for (int i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(s_dma_chan[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, false));
        channel_config_set_chain_to(&c, s_dma_chan[i ^ 1]);
        dma_channel_configure(s_dma_chan[i], &c, s_buf[i],
                              &s_pio->rxf[s_sm], WORDS_PER_BUF, false);
        dma_channel_set_irq0_enabled(s_dma_chan[i], true);
    }

    irq_add_shared_handler(DMA_IRQ_0, dma_handler,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_start(s_dma_chan[0]);
    pio_sm_set_enabled(s_pio, s_sm, true);

    goertzel_bank_init(&s_bank, SAMPLE_RATE, FRAMES_PER_BUF,
                       SCAN_LO_HZ, SCAN_HI_HZ);
    goertzel_bank_init(&s_bank_low, SAMPLE_RATE, FRAMES_PER_BUF,
                       SCAN_LOW_LO_HZ, SCAN_LOW_HI_HZ);
    detector_init(&s_det, SAMPLE_RATE, FRAMES_PER_BUF);

    /* The bench default is five seconds, short enough to test by hand. A buzzer
     * that beeps would clear between two beeps of one event at that setting and
     * send an alert for each. */
    s_det.cfg.clear_blocks =
        (uint32_t)(CLEAR_SECONDS * SAMPLE_RATE / FRAMES_PER_BUF);
}

/* ------------------------------------------------------- audio accounting */

/*
 * Overruns already accounted for.
 *
 * Two different things lose audio and they want reporting differently:
 *
 *   A Discord message costs six to seven seconds. DNS, a TLS handshake and a
 *   45 KB upload, all on this loop, and nothing is read while they run. It is a
 *   known, bounded, deliberate cost - during an alarm the alarm is already
 *   raised, and at boot nothing is happening yet.
 *
 *   A frame that will not drain, or anything else that blocks for hundreds of
 *   milliseconds unexpectedly, is a fault.
 *
 * Without this they are the same number, and the first one buries the second.
 * Each post says what it cost and clears the slate, so the ten-second line only
 * ever reports loss nobody asked for.
 */
static uint32_t s_loss_seen;

static void loss_mark(const char *why)
{
    uint32_t lost = s_overrun - s_loss_seen;

    if (lost) {
        printf("[audio] %lu buffers lost during %s (%lu ms of sound)\n",
               (unsigned long)lost, why,
               (unsigned long)(lost * BLOCKS_PER_BUF * FRAMES_PER_BUF *
                               1000u / SAMPLE_RATE));
    }
    s_loss_seen = s_overrun;
}

/* -------------------------------------------------------------------- led */

static void led_init(void)
{
    gpio_set_function(PIN_LED_BUZZER, GPIO_FUNC_PWM);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv(&c, 64.0f);       /* ~760 Hz: no visible flicker */
    pwm_config_set_wrap(&c, 4095);
    pwm_init(pwm_gpio_to_slice_num(PIN_LED_BUZZER), &c, true);

    gpio_init(PIN_LED_RUN);
    gpio_set_dir(PIN_LED_RUN, GPIO_OUT);
    gpio_put(PIN_LED_RUN, 0);
}

/*
 * RUN is a heartbeat rather than a steady lamp. A lamp that is simply on says
 * the board has power; one that blinks says the loop is still turning, which is
 * the question someone standing at the box actually has.
 */
static void led_run_tick(void)
{
    static uint32_t next_ms;
    static bool     on;

    uint32_t t = to_ms_since_boot(get_absolute_time());
    if (t < next_ms) return;

    on = !on;
    gpio_put(PIN_LED_RUN, on);
    next_ms = t + 500;
}

/*
 * Buzzer indicator. Brightness is distance from an alarm, not loudness.
 *
 * Driving it from level would light it for exactly the sounds the detector
 * exists to reject - a clap here measured sixteen times louder than the room
 * while the buzzer was barely louder than the room at all. Below the threshold
 * it glows in proportion to how close the sound is, so a near miss is visible
 * to someone standing at the box, which is what matters while the threshold is
 * still being settled on site.
 *
 * Squared on the way out because perceived brightness is roughly logarithmic
 * in duty.
 */
static void led_buzzer(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    pwm_set_gpio_level(PIN_LED_BUZZER, (uint16_t)(level * level * 4095.0f));
}

/* ---------------------------------------------------------------- network */

/* Addresses come from flash; only the MAC is fixed in the build, because it
 * identifies the board rather than the site. */
static wiz_NetInfo g_net = { .mac = NET_MAC, .dhcp = NETINFO_STATIC };

static struct repeating_timer g_1s;

static bool tick_1s(struct repeating_timer *t)
{
    (void)t;
    wiz_claw_net_1s_tick();
    return true;
}

/* ------------------------------------------------------------------ alerts */

static void alert_alarm(float hz, float ratio)
{
    static char msg[512];

    /*
     * Capture before posting. The frame wanted is the one from the moment the
     * buzzer was heard, and a capture after a seven-second upload would show
     * the room seven seconds later.
     */
    uint32_t len = 0;
    if (arducam_mega.get_frame() == 0) {        /* 0 is success, not a length */
        len = arducam_mega.frame.frame_length;
    }

    snprintf(msg, sizeof(msg),
        "🔴 **[수위 경보]** 부저음이 감지되었습니다.\n"
        "감지 방식 %s · %.0f Hz · 음조성 %.0f · 음량 %.1f배\n"
        "라이브: http://%s:%u/?t=" STREAM_TOKEN,
        s_det.by_loud ? "음량" : "음조성",
        (double)hz, (double)ratio,
        (double)(s_det.baseline > 0.0f ? s_det.peak_rms / s_det.baseline : 0.0f),
        g_set.pub_host, g_set.pub_port);

    bool ok;
    if (len >= 2 && image_buff[0] == 0xFF && image_buff[1] == 0xD8) {
        printf("[alarm] captured %lu bytes\n", (unsigned long)len);
        ok = discord_post_photo(g_set.webhook, msg, image_buff, (size_t)len,
                                "tank.jpg");
    } else {
        /* Still send the text. A failed capture at the moment of an alarm is
         * the worst possible time to say nothing at all. */
        printf("[alarm] no usable frame (%lu bytes), sending text only\n",
               (unsigned long)len);
        ok = discord_post_text(g_set.webhook, msg);
    }

    printf("[alarm] alert %s (status %d)\n",
           ok ? "sent" : "FAILED", discord_last_status());
    loss_mark("the alarm message");
}

static void alert_clear(float peak_ratio)
{
    static char msg[256];

    snprintf(msg, sizeof(msg),
        "🟢 **[상황 종료]** 부저음이 %d초간 들리지 않아 감시 모드로 돌아갑니다.\n"
        "경보 중 최고 음조성 %.0f",
        (int)CLEAR_SECONDS, (double)peak_ratio);

    discord_post_text(g_set.webhook, msg);
    loss_mark("the stand-down message");
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    set_sys_clock_khz(PLL_SYS_KHZ, true);
    stdio_init_all();
    sleep_ms(2000);

    printf("\nWater tank monitor\n------------------\n");

    /*
     * Repeated rather than printed once.
     *
     * A terminal attaches to USB CDC some seconds after the board boots, and
     * anything written before that is gone. A single message followed by a halt
     * is therefore invisible to whoever connects afterwards - the board looks
     * dead rather than misconfigured, which is a worse place to start from.
     *
     * The LED blinks for the same reason: it says something is running without
     * needing a console at all.
     */
    /*
     * Only the token stops the board. A missing webhook costs the owner a
     * message; a missing token would publish the camera to anyone who found the
     * port, and that is not a thing to boot into. The public URL only decides
     * whether the link in the message is tappable.
     */
    const bool need_token = (strstr(STREAM_TOKEN, "CHANGEME") != NULL);
    const bool need_url   = (strstr(PUBLIC_URL_BASE, "CHANGEME") != NULL);

    if (need_token) {
        bool on = false;
        while (true) {
            printf("\nsecrets.h is missing or not filled in:\n");
            if (need_token)   printf("  STREAM_TOKEN     still says CHANGEME\n");
            if (need_url)     printf("  PUBLIC_URL_BASE  still says CHANGEME\n");
            printf("Copy secrets.h.example to secrets.h, fill it in and "
                   "rebuild.\nThe firmware stays here until the token is set, "
                   "because an unset token\nwould publish the camera to anyone "
                   "who found the port.\n");

            on = !on;
            led_buzzer(on ? 1.0f : 0.0f);
            sleep_ms(2000);
        }
    }

    led_init();
    led_buzzer(0.0f);

    /* Before the network, the camera or the alerts: all three read values that
     * may have been changed from the panel and stored in flash. */
    settings_load();
    printf("addr %u.%u.%u.%u:%u  public %s:%u\n",
           g_set.ip[0], g_set.ip[1], g_set.ip[2], g_set.ip[3], g_set.http_port,
           g_set.pub_host, g_set.pub_port);

    printf("camera ...\n");
    arducam_mega.init();        /* leaves the sensor at JPEG, 320x240 */

    /*
     * init() leaves the sensor at 320x240, so the stored mode has to be written
     * over it - and the timing here is not decoration.
     *
     * The exhibition build learned this the hard way: a set_pixel_format()
     * followed immediately by set_frame_size() has its resolution write silently
     * dropped, and the console then reports HD while the sensor keeps sending
     * 320x240 frames, because that line prints our own variable and nobody asked
     * the sensor. Which is exactly what this firmware did until now - the HD
     * button changed a number and nothing else.
     *
     * So: a settle before, the format written explicitly, a settle, the size,
     * the return value checked, a settle. The panel can change this later
     * without a restart; see /api/res in webserver.c.
     */
    sleep_ms(200);
    arducam_mega.set_pixel_format(PIXFORMAT_JPEG);
    sleep_ms(200);

    if (arducam_mega.set_frame_size((res_t)g_set.res) != 0) {
        printf("WARNING: set_frame_size(%u) failed - the sensor is probably "
               "still at 320x240\n", g_set.res);
    }
    sleep_ms(200);

    printf("camera at %s\n", settings_res_name(g_set.res));

    printf("network ...\n");
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    wiz_claw_net_config_t ncfg = {
        .dns_socket = SOCK_DNS, .dhcp_socket = SOCK_DHCP,
    };

    /* The resolver gets the stored server too. Leaving it at the compile-time
     * default would mean a box whose DNS was changed from the panel still asked
     * the old one - and a webhook that stops resolving looks exactly like a
     * webhook that stopped working. */
    memcpy(ncfg.dns_server_ip, g_set.dns, 4);
    wiz_claw_net_init(&ncfg);
    add_repeating_timer_ms(-1000, tick_1s, NULL, &g_1s);

    memcpy(g_net.ip,  g_set.ip,  4);
    memcpy(g_net.sn,  g_set.sn,  4);
    memcpy(g_net.gw,  g_set.gw,  4);
    memcpy(g_net.dns, g_set.dns, 4);

    /*
     * The stored address goes in first even when DHCP is asked for. The MAC has
     * to be in the chip before DHCP can send anything, and it is the fallback: a
     * site with no DHCP server still has to come up reachable at an address
     * somebody can be told.
     */
    wizchip_setnetinfo(&g_net);

    bool dhcp_ok = false;

    if (g_set.use_dhcp) {
        wiz_NetInfo d = g_net;

        printf("DHCP, up to 60 s ...\n");
        if (wiz_claw_net_dhcp_run(&d) == WIZ_CLAW_OK) {
            memcpy(g_net.ip,  d.ip,  4);
            memcpy(g_net.sn,  d.sn,  4);
            memcpy(g_net.gw,  d.gw,  4);
            memcpy(g_net.dns, d.dns, 4);
            dhcp_ok = true;
        } else {
            printf("DHCP did not answer - using the stored address\n");
        }

        /*
         * The socket goes back to the web server, and the chip is left static
         * holding the leased address.
         *
         * Nothing renews the lease: renewal needs DHCP_run() called forever and
         * this loop has a microphone to read. The address is held, not owned - if
         * the router gives it to something else after the lease expires, the box
         * needs a power cycle. A DHCP reservation on the router removes the
         * question, and is worth asking the site for.
         */
        close(SOCK_DHCP);
        wizchip_setnetinfo(&g_net);

        /* The resolver was given the stored DNS before DHCP ran. If the router
         * named a different one, the old answer would outlive this boot. */
        if (dhcp_ok) {
            memcpy(ncfg.dns_server_ip, g_net.dns, 4);
            wiz_claw_net_init(&ncfg);
        }
    }

    wiz_claw_net_print_info(&g_net);
    printf("address from %s\n",
           g_set.use_dhcp ? (dhcp_ok ? "DHCP" : "DHCP failed, stored fallback")
                          : "stored settings");

    webserver_init();

    printf("microphone ...\n");
    audio_init();

    absolute_time_t next_log = make_timeout_time_ms(1000);
    absolute_time_t next_health = make_timeout_time_ms(10000);
    uint32_t blocks_done = 0;

    /*
     * Four figures a second, because one of them on its own misleads.
     *
     * `win_tone` is the best block of the second and `win_min` the worst. A
     * buzzer that reads 37 at its peak and 2 in the gaps between pulses passes
     * far fewer blocks than the peak suggests, and the detector counts blocks.
     * That gap is why the hit count was raised to the alarm so slowly, and it is
     * invisible if only the maximum is printed.
     *
     * `win_rms` is what a loudness threshold would have seen. It is here to be
     * read against the tone, not to decide anything: the room and a 2 kHz tone
     * measure the same loudness, which is the whole reason this detector sorts
     * by shape instead. If the buzzer turns out to be reliably louder than the
     * room as well, it can become a second condition - but that needs the
     * number first.
     */
    float    win_tone = 0.0f;
    float    win_rms  = 0.0f;
    bool     win_any  = false;
    char     spectrum[GOERTZEL_MAX_BINS + 1];

    /* The low band keeps its own best block, because the loudest moment down
     * there is not necessarily the loudest moment up here - and if they differ,
     * that difference is the answer. */
    float    win_low_tone = 0.0f;
    int      win_low_hz   = 0;
    char     spectrum_low[GOERTZEL_MAX_BINS + 1];

    /* The two new ways of measuring "stands out", kept per band. See goertzel.h. */
    float    win_med = 0.0f, win_band = 0.0f;
    int      win_band_hz = 0;
    float    win_low_med = 0.0f, win_low_band = 0.0f;
    int      win_low_band_hz = 0;

    spectrum[0]     = '\0';
    spectrum_low[0] = '\0';

    printf("\nwatching.\n\n");
    /*
     * The stored webhook, not the compile-time one: a box whose webhook was
     * changed from the panel would otherwise announce its boot to the old
     * channel and every alarm afterwards to the new one.
     *
     * The address is in the text because with DHCP that message is the only way
     * anybody learns it without a USB cable - which is what makes DHCP safe to
     * default to.
     */
    {
        static char boot[256];

        snprintf(boot, sizeof(boot),
            "🟢 **[시스템 가동]** 저수조 부저 감시를 시작합니다.\n"
            "내부 주소 %u.%u.%u.%u:%u (%s) · 카메라 %s\n"
            "외부 접속 http://%s:%u/?t=" STREAM_TOKEN,
            g_net.ip[0], g_net.ip[1], g_net.ip[2], g_net.ip[3], g_set.http_port,
            g_set.use_dhcp ? (dhcp_ok ? "DHCP" : "DHCP 실패, 고정 주소")
                           : "고정",
            settings_res_name(g_set.res),
            g_set.pub_host, g_set.pub_port);

        discord_post_text(g_set.webhook, boot);
        loss_mark("the startup message");
    }


    while (true) {

        /* ---- audio: every block, because the dwell counts blocks ---- */
        for (int i = 0; i < 2; i++) {
            if (!(s_ready_mask & (1u << i))) continue;

            /*
             * Every block in the buffer, in order. The dwell counts blocks, so
             * handing the detector one summary per buffer would quietly turn a
             * three-second window into a twelve-second one.
             */
            for (int blk = 0; blk < BLOCKS_PER_BUF; blk++) {
                const int32_t *src = &s_buf[i][blk * WORDS_PER_BLOCK];

                static int32_t mono[WORDS_PER_BLOCK];
                double sum = 0.0, sumsq = 0.0;

                for (int f = 0; f < FRAMES_PER_BUF; f++) {
                    int32_t v = to_sample(src[f * 2]);
                    mono[f * 2] = v;
                    sum   += (double)v;
                    sumsq += (double)v * (double)v;
                }

                /* Variance, not mean square: the microphone carries a DC offset and
                 * including it would report the offset as signal level. */
                double mean = sum / FRAMES_PER_BUF;
                double var  = sumsq / FRAMES_PER_BUF - mean * mean;
                float  rms  = (float)(sqrt(var > 0.0 ? var : 0.0) / 8388608.0);

                goertzel_bank_run(&s_bank, mono, 2);
                goertzel_bank_run(&s_bank_low, mono, 2);

                det_event_t ev = detector_update(&s_det, s_bank.tone_ratio,
                                                 s_bank.peak_bin, s_bank.peak_hz,
                                                 rms);

                if (ev == DET_EV_ALARM) {
                    printf("\n*** ALARM by %s - %.0f Hz, tone %.1f, "
                           "rms %.4f vs base %.4f (%.1fx) ***\n",
                           s_det.by_loud ? "LEVEL" : "TONE",
                           (double)s_det.locked_hz, (double)s_bank.tone_ratio,
                           (double)rms, (double)s_det.baseline,
                           (double)(rms / (s_det.baseline > 0.0f
                                           ? s_det.baseline : 1.0f)));

                    /* The camera is enabled before the alert goes out, so the link
                     * in the message already works by the time the phone buzzes. */
                    webserver_set_alarm(true);
                    led_buzzer(1.0f);
                    alert_alarm(s_det.locked_hz, s_bank.tone_ratio);

                } else if (ev == DET_EV_CLEAR) {
                    printf("\n*** CLEAR, peak was %.1f ***\n",
                           (double)s_det.peak_ratio);

                    /*
                     * One event ends the alarm, the lamp and the video together.
                     *
                     * CLEAR already is the thirty seconds of silence, so a separate
                     * linger timer on top of it would only be a second thirty
                     * seconds - and two timers that are supposed to mean the same
                     * thing eventually disagree.
                     */
                    webserver_set_alarm(false);
                    led_buzzer(0.0f);
                    alert_clear(s_det.peak_ratio);
                }

                /* The panel carries the ratio while watching, not only during an
                 * alarm: the figure someone needs when deciding where to mount the
                 * box is how close the quiet room already sits to the threshold. */
                webserver_set_detail((int)(s_det.state == DET_ALARM
                                           ? s_det.locked_hz : s_bank.peak_hz),
                                     s_bank.tone_ratio);

                if (s_det.state != DET_ALARM) {
                    float p = s_bank.tone_ratio / s_det.cfg.enter_ratio;
                    led_buzzer(0.5f * (p > 1.0f ? 1.0f : p));
                }

                blocks_done++;

                /*
                 * The bar is drawn from the block that stood out most, so the column
                 * that lifts is the bin the detector is actually counting. A buzzer
                 * puts one column up and keeps it there; a voice or a ringtone moves
                 * it around, and that is readable at a glance in a way two numbers
                 * are not.
                 */
                if (!win_any || s_bank.tone_ratio > win_tone) {
                    win_tone = s_bank.tone_ratio;
                    goertzel_bank_render(&s_bank, spectrum);
                }
                if (s_bank.med_ratio > win_med) win_med = s_bank.med_ratio;
                if (s_bank.band_ratio > win_band) {
                    win_band    = s_bank.band_ratio;
                    win_band_hz = (int)s_bank.band_hz;
                }
                if (s_bank_low.med_ratio > win_low_med)
                    win_low_med = s_bank_low.med_ratio;
                if (s_bank_low.band_ratio > win_low_band) {
                    win_low_band    = s_bank_low.band_ratio;
                    win_low_band_hz = (int)s_bank_low.band_hz;
                }
                if (!win_any || s_bank_low.tone_ratio > win_low_tone) {
                    win_low_tone = s_bank_low.tone_ratio;
                    win_low_hz   = (int)s_bank_low.peak_hz;
                    goertzel_bank_render(&s_bank_low, spectrum_low);
                }
                if (rms > win_rms) win_rms = rms;
                win_any = true;
            }

            s_ready_mask &= ~(1u << i);
        }

        webserver_poll();
        led_run_tick();

        /*
         * One line a second, and two of the four figures are about whether the
         * detector is being fed rather than what it heard.
         *
         * The window counts blocks, and blocks only arrive if the loop comes
         * back for them. At 16 kHz with 256-sample buffers that is 62.5 a
         * second. Much below it means the loop is spending its time elsewhere
         * and the window fills in slow motion - which from the outside looks
         * exactly like a buzzer that is not loud enough.
         *
         * `ovr` is the same fact from the other side: a buffer that filled
         * before the previous one had been read.
         */
        if (time_reached(next_log)) {
            /*
             * Two bars, low band first, separated by a bar so the boundary is
             * visible. Only the right-hand one feeds the detector; the left-hand
             * one is there to answer where the buzzer's energy really is.
             */
            /*
             * t is what the detector reads; m and b are candidates for what it
             * should read. b carries its own frequency because the best run of
             * bins is not always under the loudest single one - and when those
             * two disagree, the disagreement is the finding.
             */
#if MIC_DEBUG_LOG
            /*
             * m and b are gone. They were added to test whether a median-based
             * background would find the sounder that the mean missed, and the
             * answer was no - both read LOWER on the sounder than on silence,
             * because it has no peak to find. The columns that replaced them are
             * the two the level path is decided on: how far above the learned
             * baseline this second got, and how much of the window is loud.
             */
            printf("[%s|%s] hi %4d t%5.1f | lo %4d t%5.1f | "
                   "T%2lu/%lu L%3lu/%lu | rms%.4f base%.4f %4.1fx | "
                   "blk%2lu ovr%lu %s\n",
                   spectrum_low, spectrum,
                   (int)s_bank.peak_hz, (double)win_tone,
                   win_low_hz, (double)win_low_tone,
                   (unsigned long)s_det.win_hits,
                   (unsigned long)s_det.cfg.enter_hits,
                   (unsigned long)s_det.lwin_hits,
                   (unsigned long)s_det.cfg.loud_hits,
                   (double)win_rms, (double)s_det.baseline,
                   (double)(win_rms / (s_det.baseline > 0.0f
                                       ? s_det.baseline : 1.0f)),
                   (unsigned long)blocks_done,
                   (unsigned long)s_overrun,
                   s_det.state == DET_ALARM ? "ALARM" : "watch");
#else
            (void)spectrum_low; (void)spectrum;
            (void)win_low_hz;   (void)win_band_hz; (void)win_low_band_hz;
            (void)win_med;      (void)win_band;
            (void)win_low_med;  (void)win_low_band;
#endif

            /*
             * One line, only when audio was actually lost, at most every ten
             * seconds.
             *
             * This is the number that said the board was "dying": the loop was
             * sitting inside one HD frame's send while DMA buffers filled behind
             * it and were overwritten. It stays in the console after the
             * per-block line was turned off, because it is the one audio figure
             * that reports a fault rather than a reading - silence here means
             * the microphone is being read on time.
             */
            if (time_reached(next_health)) {
                loss_mark("the last 10 s");
                next_health = make_timeout_time_ms(10000);
            }

            blocks_done  = 0;
            win_tone     = 0.0f;
            win_rms      = 0.0f;
            win_low_tone = 0.0f;
            win_med      = 0.0f;
            win_band     = 0.0f;
            win_low_med  = 0.0f;
            win_low_band = 0.0f;
            win_any      = false;
            next_log     = make_timeout_time_ms(1000);
        }
    }
}
