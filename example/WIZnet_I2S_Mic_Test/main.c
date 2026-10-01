/*
 * INMP441 capture plus a Goertzel scan - standalone.
 *
 * Two jobs.
 *
 * The first was capture: does the microphone work, is the rate right at the
 * 200 MHz the exhibition build runs at, is the left channel the live one. That
 * is settled - 85 s at 16000.0 Hz average, zero overruns, right channel silent.
 *
 * The second is the one that decides whether the water-tank product is
 * possible at all. A level threshold fires on anything loud, and a basement is
 * full of loud: a pump starting, a door, rain. The question is whether a buzzer
 * can be told apart from all of it by its shape rather than its size.
 *
 * So this scans 1.5-4.5 kHz and prints two numbers side by side:
 *
 *   rms         what a level threshold would see
 *   tone        peak bin over the mean of the rest
 *
 * Play a 3 kHz sine from a phone: rms climbs and tone goes to the hundreds.
 * Clap, talk, drop something: rms climbs just as hard and tone stays in the low
 * single digits. If those two columns separate, the product works. If they do
 * not, nothing built on top of this would.
 *
 * Wiring:
 *
 *   INMP441        W6300-EVB-Pico2        header pin
 *   VDD      ->    3V3 (OUT)                   36
 *   GND      ->    GND                         38
 *   SCK      ->    GP2      BCLK                4
 *   WS       ->    GP3      LRCL                5
 *   SD       ->    GP28     DATA               34
 *   L/R      ->    GND      (left channel)
 *
 * Those three are what the exhibition build leaves free. GP4 is the camera's
 * VSYNC, GP5..GP14 are its parallel data, PCLK and HSYNC, GP0/GP1 are its SCCB
 * bus, and GP15..GP22 belong to the W6300. GP26..GP29 are declared in the
 * camera header as an SPI port but nothing ever initialises them, so GP28 is
 * free; GP27 is avoided because the sensor init writes to it once.
 *
 * Output goes to USB serial at any baud rate.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"

#include "i2s_rx.pio.h"
#include "goertzel.h"
#include "detector.h"

/* ------------------------------------------------------------------ config */

#define PIN_I2S_BCLK    2            /* WS is forced to this + 1 */
#define PIN_I2S_DATA    28           /* not GP4 - that is the camera's VSYNC */

#define SAMPLE_RATE     16000        /* 200 MHz divides into this exactly */
#define SYS_CLOCK_KHZ   200000       /* same as the exhibition build */

#define FRAMES_PER_BUF  256          /* 16 ms, and one Goertzel block */
#define WORDS_PER_BUF   (FRAMES_PER_BUF * 2)   /* left and right interleaved */

#define SCAN_LO_HZ      1500.0f      /* buzzers live between these two */
#define SCAN_HI_HZ      4500.0f

/* Provisional. The real one comes from measuring the buzzer in the basement
 * against that basement's own noise, not from a round number chosen here. */
#define TONE_THRESHOLD  20.0f

/*
 * Two rates, because the console has two jobs and they want opposite things.
 *
 * Watching an empty room, one line a second is already more than there is to
 * say - four a second scrolls the interesting part off the screen before it can
 * be read. But when something is happening, 250 ms is the resolution that shows
 * the dwell filling and the ratio moving.
 *
 * The LED carries the real-time part, so the console is free to be slow when
 * nothing is going on.
 */
#define PRINT_IDLE_MS    1000
#define PRINT_ACTIVE_MS   250

/* ------------------------------------------------------------------- state */

static int32_t  s_buf[2][WORDS_PER_BUF];
static uint     s_dma_chan[2];
static volatile uint32_t s_ready_mask;
static volatile uint32_t s_frames_total;
static volatile uint32_t s_overrun;

static PIO  s_pio;
static uint s_sm;

static goertzel_bank_t s_bank;
static detector_t      s_det;

/* --------------------------------------------------------------------- dma */

static void dma_handler(void)
{
    for (int i = 0; i < 2; i++) {
        if (dma_hw->ints0 & (1u << s_dma_chan[i])) {
            dma_hw->ints0 = 1u << s_dma_chan[i];

            if (s_ready_mask & (1u << i)) s_overrun++;
            s_ready_mask |= (1u << i);
            s_frames_total += FRAMES_PER_BUF;

            dma_channel_set_write_addr(s_dma_chan[i], s_buf[i], false);
        }
    }
}

static void dma_setup(void)
{
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

    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_start(s_dma_chan[0]);
}

/* ------------------------------------------------------------------ helper */

/* Philips I2S presents the MSB one BCLK after the WS edge, so the 32-bit window
 * the state machine captures sits one bit late: bit 31 is the tail of the
 * previous slot, the sample's sign is at bit 30, bits 6..0 are zero padding.
 * Shifting left by one restores the sign before the arithmetic shift extends
 * it. Reading it as `word >> 8` costs the sign, and the symptom is specific:
 * the signal never goes negative and DC parks near half of full scale. */
static inline int32_t to_sample(int32_t word)
{
    return ((int32_t)((uint32_t)word << 1)) >> 8;
}

/* --------------------------------------------------------------------- led */
/*
 * GP25 is the on-board LED, and nothing else on this board wants it - the
 * camera, the W6300 and the microphone between them leave it alone, so the
 * same code works once this moves into the exhibition build.
 *
 * Brightness is distance from an alarm, not loudness. A faint glow tracks the
 * tone ratio, half brightness means the threshold has just been crossed, and
 * the ramp from there to full is the dwell filling up. Solid full is the alarm.
 *
 * Loudness was the other option and it is the wrong one: the whole result this
 * is built on is that loudness does not tell a buzzer from a shout. An
 * indicator driven by level would light up for the thing being rejected.
 */
#define PIN_LED         25
#define LED_WRAP        4095

static void led_init(void)
{
    gpio_set_function(PIN_LED, GPIO_FUNC_PWM);

    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv(&c, 64.0f);       /* ~760 Hz: no visible flicker */
    pwm_config_set_wrap(&c, LED_WRAP);
    pwm_init(pwm_gpio_to_slice_num(PIN_LED), &c, true);
}

/* @p level is 0..1. Squared on the way out because perceived brightness is
 * roughly logarithmic in duty - linear duty spends most of its range in a
 * region the eye reads as "on". */
static void led_set(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    pwm_set_gpio_level(PIN_LED, (uint16_t)(level * level * (float)LED_WRAP));
}

/* -------------------------------------------------------------------- main */

int main(void)
{
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);
    stdio_init_all();

    sleep_ms(2000);

    printf("\nINMP441 capture + Goertzel scan\n");
    printf("-------------------------------\n");
    printf("sys clock      : %u Hz\n", clock_get_hz(clk_sys));
    printf("pins           : BCLK=GP%d  WS=GP%d  SD=GP%d\n",
           PIN_I2S_BCLK, PIN_I2S_BCLK + 1, PIN_I2S_DATA);
    printf("target rate    : %d Hz  (BCLK %d Hz)\n",
           SAMPLE_RATE, SAMPLE_RATE * 64);

    uint offset;
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(
            &i2s_rx_program, &s_pio, &s_sm, &offset, PIN_I2S_BCLK, 3, true)) {
        printf("FAIL: no PIO state machine for GP%d..GP%d\n",
               PIN_I2S_BCLK, PIN_I2S_BCLK + 2);
        while (true) tight_loop_contents();
    }

    float div = i2s_rx_program_init(s_pio, s_sm, offset,
                                    PIN_I2S_BCLK, PIN_I2S_DATA, SAMPLE_RATE);
    float div_actual = (float)((int)(div * 256.0f + 0.5f)) / 256.0f;
    float rate_actual = (float)clock_get_hz(clk_sys) / (div_actual * 128.0f);

    printf("pio            : block %d  sm %u  offset %u\n",
           pio_get_index(s_pio), s_sm, offset);
    printf("clkdiv         : %.6f -> %.6f, rate %.2f Hz\n",
           div, div_actual, rate_actual);

    goertzel_bank_init(&s_bank, SAMPLE_RATE, FRAMES_PER_BUF,
                       SCAN_LO_HZ, SCAN_HI_HZ);

    printf("scan           : %.0f..%.0f Hz, %d bins of %.1f Hz\n",
           s_bank.freq[0], s_bank.freq[s_bank.n_bins - 1],
           s_bank.n_bins, s_bank.bin_hz);
    detector_init(&s_det, SAMPLE_RATE, FRAMES_PER_BUF);
    led_init();

    /*
     * Printed from the struct rather than from the constants, so a figure that
     * never reached the detector shows up here as a zero instead of looking
     * right in the banner and behaving wrongly in the loop.
     */
    printf("detector       : enter %.0f, need %lu hits in %lu blocks (%.1f s of %.1f s)\n",
           (double)s_det.cfg.enter_ratio,
           (unsigned long)s_det.cfg.enter_hits,
           (unsigned long)s_det.cfg.window_blocks,
           (double)s_det.cfg.enter_hits * FRAMES_PER_BUF / (double)SAMPLE_RATE,
           (double)s_det.cfg.window_blocks * FRAMES_PER_BUF / (double)SAMPLE_RATE);
    printf("                 hold %.0f, clear after %.1f s of silence\n",
           (double)s_det.cfg.hold_ratio,
           (double)s_det.cfg.clear_blocks * FRAMES_PER_BUF / (double)SAMPLE_RATE);
    printf("GP25 LED       : brightness = how close to an alarm, solid = alarm\n");

    if (s_det.cfg.enter_hits == 0 || s_det.cfg.enter_ratio <= 0.0f) {
        printf("FAIL: detector not configured - every block would alarm\n");
        while (true) tight_loop_contents();
    }

    printf("\n");
    printf("Play a steady tone and watch one column lift and `tone` climb.\n");
    printf("Clap or talk: `rms` climbs just as much, `tone` does not.\n\n");

    dma_setup();
    pio_sm_set_enabled(s_pio, s_sm, true);

    sleep_ms(200);                      /* the part settles after its clock */
    s_ready_mask = 0;
    s_frames_total = 0;
    s_overrun = 0;

    absolute_time_t run_start  = get_absolute_time();
    absolute_time_t next_print = make_timeout_time_ms(PRINT_IDLE_MS);
    absolute_time_t next_stats = make_timeout_time_ms(5000);

    char  spectrum[GOERTZEL_MAX_BINS + 1];
    float rms = 0.0f;
    float hold_tone = 0.0f;             /* max over the print interval */
    /*
     * The minimum matters as much as the maximum, and that only became obvious
     * when a real buzzer failed to raise an alarm while the printed figure sat
     * well above the threshold. One line is the best of fifteen blocks; the
     * detector counts all fifteen, so a peak of 40 over a floor of 12 passes
     * far fewer of them than the line suggests.
     */
    float hold_tone_min = 0.0f;
    float hold_rms  = 0.0f;
    int   hold_hz   = 0;
    float hold_mag  = 0.0f;
    bool  have_block = false;

    while (true) {
        for (int i = 0; i < 2; i++) {
            if (!(s_ready_mask & (1u << i))) continue;

            const int32_t *raw = s_buf[i];

            /* Unpack in place: the Goertzel bank wants real samples, and doing
             * it here keeps that module free of this part's bit layout. */
            static int32_t mono[FRAMES_PER_BUF * 2];
            double sum = 0.0, sumsq = 0.0;

            for (int f = 0; f < FRAMES_PER_BUF; f++) {
                int32_t s = to_sample(raw[f * 2]);
                mono[f * 2] = s;
                sum   += (double)s;
                sumsq += (double)s * (double)s;
            }

            double mean = sum / FRAMES_PER_BUF;
            double var  = sumsq / FRAMES_PER_BUF - mean * mean;
            rms = (float)(sqrt(var > 0.0 ? var : 0.0) / 8388608.0);

            goertzel_bank_run(&s_bank, mono, 2);

            /* Every block, not the held maximum: the dwell counts blocks that
             * each passed, and feeding it one summary per 250 ms would make a
             * two-second dwell mean something else entirely. */
            det_event_t ev = detector_update(&s_det, s_bank.tone_ratio,
                                             s_bank.peak_bin, s_bank.peak_hz);

            if (ev == DET_EV_ALARM) {
                printf("\n*** ALARM   %.0f Hz, tone %.1f ***\n\n",
                       (double)s_det.locked_hz, (double)s_bank.tone_ratio);
            } else if (ev == DET_EV_CLEAR) {
                printf("\n*** CLEAR   tone gone for %.1f s, peak was %.1f ***\n\n",
                       (double)s_det.cfg.clear_blocks * FRAMES_PER_BUF /
                       (double)SAMPLE_RATE, (double)s_det.peak_ratio);
            }

            if (s_det.state == DET_ALARM) {
                led_set(1.0f);
            } else if (s_bank.tone_ratio >= s_det.cfg.enter_ratio) {
                /* Threshold met: the second half of the range is the window
                 * filling toward the hit count. */
                float p = (float)s_det.win_hits / (float)s_det.cfg.enter_hits;
                led_set(0.5f + 0.5f * (p > 1.0f ? 1.0f : p));
            } else {
                led_set(0.5f * s_bank.tone_ratio / s_det.cfg.enter_ratio);
            }

            /*
             * Render on the first block of every interval as well as on a new
             * tonal peak. Rendering only on a new peak means a silent or
             * untonal interval prints an empty bar, which reads as "the scan is
             * broken" when it actually means "nothing stood out" - two very
             * different things to be looking at while chasing a wiring fault.
             */
            if (!have_block || s_bank.tone_ratio > hold_tone) {
                hold_tone = s_bank.tone_ratio;
                hold_hz   = (int)s_bank.peak_hz;
                hold_mag  = s_bank.peak_mag;
                goertzel_bank_render(&s_bank, spectrum);
            }
            if (rms > hold_rms) hold_rms = rms;
            if (!have_block || s_bank.tone_ratio < hold_tone_min)
                hold_tone_min = s_bank.tone_ratio;
            have_block = true;

            s_ready_mask &= ~(1u << i);
        }

        if (have_block && time_reached(next_print)) {
            printf("[%s] %5d Hz  tone %6.1f (min %5.1f)  hits %3lu/%lu | "
                   "rms %.4f | %s\n",
                   spectrum, hold_hz, (double)hold_tone, (double)hold_tone_min,
                   (unsigned long)s_det.win_hits,
                   (unsigned long)s_det.cfg.enter_hits,
                   (double)hold_rms,
                   s_det.state == DET_ALARM ? "ALARM" : "watch");

            float hold_next = hold_tone;
            hold_tone = 0.0f;
            hold_tone_min = 0.0f;
            hold_rms  = 0.0f;
            have_block = false;

            /* Anything at or above the hold threshold is worth watching
             * closely, whether or not it ever becomes an alarm - a near miss is
             * exactly what wants looking at while the threshold is still being
             * chosen. */
            bool active = (s_det.state == DET_ALARM) ||
                          (hold_next >= s_det.cfg.hold_ratio);
            next_print = make_timeout_time_ms(active ? PRINT_ACTIVE_MS
                                                     : PRINT_IDLE_MS);
        }

        if (time_reached(next_stats)) {
            double elapsed_us =
                (double)absolute_time_diff_us(run_start, get_absolute_time());

            /*
             * The raw words are here because the frame counter does not prove
             * what it looks like it proves. BCLK, WS and the DMA all keep
             * running at exactly the right rate with the data line
             * disconnected - the state machine simply shifts in zeros. A rate
             * of 16000 Hz with silent audio is the signature of a dead SD line,
             * not of a quiet room, and the only way to tell them apart from the
             * console is to look at what actually arrived.
             */
            printf("    -- %.1f Hz avg, %lu frames, %lu overruns | "
                   "bins %d | raw %08lx %08lx %08lx %08lx\n",
                   (double)s_frames_total * 1e6 / elapsed_us,
                   (unsigned long)s_frames_total,
                   (unsigned long)s_overrun,
                   s_bank.n_bins,
                   (unsigned long)s_buf[0][0], (unsigned long)s_buf[0][2],
                   (unsigned long)s_buf[0][4], (unsigned long)s_buf[0][6]);

            next_stats = make_timeout_time_ms(5000);
        }
    }
}
