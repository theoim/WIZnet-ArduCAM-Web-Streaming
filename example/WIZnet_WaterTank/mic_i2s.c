/**
 * @file    mic_i2s.c
 * @brief   INMP441 capture. See mic_i2s.h for why this is switchable.
 *
 * Verified standalone in example/WIZnet_I2S_Mic_Test before being brought here:
 * 85 s at 16000.0 Hz average, zero overruns, right channel silent, which is
 * what L/R tied to ground should give.
 */

#include <stdio.h>

#include "mic_i2s.h"
#include "exhibition_config.h"

#if EXHIBITION_MIC

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/irq.h"

#include "i2s_rx.pio.h"

#define PIN_I2S_BCLK    2       /* WS is this + 1 */
#define PIN_I2S_DATA    28      /* not GP4 - that is the camera's VSYNC */

#define SAMPLE_RATE     16000   /* 200 MHz divides into this exactly:
                                 * clkdiv lands on 97 + 168/256 with no error */

#define FRAMES_PER_BUF  256     /* 16 ms */
#define WORDS_PER_BUF   (FRAMES_PER_BUF * 2)

static int32_t  s_buf[2][WORDS_PER_BUF];
static uint     s_dma_chan[2];
static PIO      s_pio;
static uint     s_sm;
static bool     s_running;

static volatile uint32_t s_ready_mask;
static volatile uint32_t s_frames;
static volatile uint32_t s_overruns;

static volatile int32_t  s_peak;
static volatile int32_t  s_rms;

static void mic_dma_handler(void)
{
    for (int i = 0; i < 2; i++) {
        if (!(dma_hw->ints0 & (1u << s_dma_chan[i]))) continue;

        dma_hw->ints0 = 1u << s_dma_chan[i];

        if (s_ready_mask & (1u << i)) s_overruns++;
        s_ready_mask |= (1u << i);
        s_frames += FRAMES_PER_BUF;

        /* The chain re-triggers this channel once its partner finishes; all it
         * needs back is where to write. */
        dma_channel_set_write_addr(s_dma_chan[i], s_buf[i], false);
    }
}

/*
 * Philips I2S presents the MSB one BCLK after the WS edge, so the 32-bit window
 * the state machine captures sits one bit late: bit 31 is the tail of the
 * previous slot, the sample's sign bit is at bit 30, and bits 6..0 are the
 * part's own zero padding. Shifting left by one restores the sign before the
 * arithmetic shift extends it.
 *
 * Reading this as `word >> 8` instead costs the sign, and the symptom is
 * specific enough to name: the signal never goes negative and the DC offset
 * parks near half of full scale.
 */
static inline int32_t to_sample(int32_t word)
{
    return ((int32_t)((uint32_t)word << 1)) >> 8;
}

bool mic_i2s_init(void)
{
    uint offset;

    /* Nothing here names a PIO block or a DMA channel. The camera takes pio0
     * sm0 explicitly and the W6300's QSPI is a PIO program too, so asking for
     * whatever is free is the only arrangement that survives either of them
     * changing. */
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(
            &i2s_rx_program, &s_pio, &s_sm, &offset, PIN_I2S_BCLK, 3, true)) {
        printf("MIC: no PIO state machine free for GP%d..GP%d\n",
               PIN_I2S_BCLK, PIN_I2S_BCLK + 2);
        return false;
    }

    float div = i2s_rx_program_init(s_pio, s_sm, offset,
                                    PIN_I2S_BCLK, PIN_I2S_DATA, SAMPLE_RATE);

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

    /* Shared rather than exclusive: the camera polls its own channel today, but
     * a future change that wants an interrupt should not have to find this. */
    irq_add_shared_handler(DMA_IRQ_0, mic_dma_handler,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_start(s_dma_chan[0]);
    pio_sm_set_enabled(s_pio, s_sm, true);

    s_running = true;

    printf("MIC: INMP441 on BCLK=GP%d WS=GP%d SD=GP%d, "
           "pio%d sm%u, dma %u/%u, %d Hz (clkdiv %.4f)\n",
           PIN_I2S_BCLK, PIN_I2S_BCLK + 1, PIN_I2S_DATA,
           pio_get_index(s_pio), s_sm, s_dma_chan[0], s_dma_chan[1],
           SAMPLE_RATE, (double)div);

    return true;
}

void mic_i2s_poll(void)
{
    if (!s_running) return;

    for (int i = 0; i < 2; i++) {
        if (!(s_ready_mask & (1u << i))) continue;

        const int32_t *b = s_buf[i];
        int64_t sum = 0, sumsq = 0;
        int32_t peak = 0;

        /* Left channel only. The right slot is silent with L/R grounded, and
         * walking it would double the work for nothing. */
        for (int f = 0; f < FRAMES_PER_BUF; f++) {
            int32_t s = to_sample(b[f * 2]);
            sum   += s;
            sumsq += (int64_t)s * (int64_t)s;
            int32_t a = s < 0 ? -s : s;
            if (a > peak) peak = a;
        }

        /* Mean square minus square of the mean: the part has a DC offset that
         * drifts, so a fixed subtraction would not hold. */
        int64_t mean  = sum / FRAMES_PER_BUF;
        int64_t msq   = sumsq / FRAMES_PER_BUF;
        int64_t var   = msq - mean * mean;
        if (var < 0) var = 0;

        int32_t r = 0;
        while ((int64_t)(r + 1) * (r + 1) <= var && r < (1 << 23)) {
            r = (r == 0) ? 1 : r * 2;           /* coarse, then refine */
        }
        if (r > 1) {
            int32_t lo = r / 2, hi = r;
            while (lo < hi) {                   /* integer sqrt, no libm */
                int32_t mid = lo + (hi - lo + 1) / 2;
                if ((int64_t)mid * mid <= var) lo = mid; else hi = mid - 1;
            }
            r = lo;
        }

        s_peak = peak;
        s_rms  = r;

        s_ready_mask &= ~(1u << i);
    }
}

bool mic_i2s_present(void) { return s_running; }

void mic_i2s_level(int32_t *peak, int32_t *rms)
{
    if (peak) *peak = s_peak;
    if (rms)  *rms  = s_rms;
}

uint32_t mic_i2s_frames(void)   { return s_frames; }
uint32_t mic_i2s_overruns(void) { return s_overruns; }

#else  /* EXHIBITION_MIC == 0 */

bool     mic_i2s_init(void)      { return false; }
void     mic_i2s_poll(void)      { }
bool     mic_i2s_present(void)   { return false; }
void     mic_i2s_level(int32_t *peak, int32_t *rms)
{
    if (peak) *peak = 0;
    if (rms)  *rms  = 0;
}
uint32_t mic_i2s_frames(void)    { return 0; }
uint32_t mic_i2s_overruns(void)  { return 0; }

#endif /* EXHIBITION_MIC */
