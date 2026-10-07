/**
 * @file    xcore.h
 * @brief   The handful of values that cross between the two cores.
 *
 * Built only when USE_CORE1 is set. The single-core image compiles without any
 * of this and stays the regression baseline: if the split ever makes detection
 * behave differently, the two images are there to compare.
 *
 * ---------------------------------------------------------------------------
 *
 * Why the split exists, in one measurement:
 *
 *   [audio] 50 buffers lost during the startup message (6400 ms of sound)
 *
 * A Discord alert is a DNS query, a TLS handshake and a 45 KB upload, and on one
 * core all of it happens instead of reading the microphone. Six to eight seconds
 * of deafness, every time the box has something to say - including the moment it
 * has just heard the buzzer, which is exactly when a second beep would arrive.
 * The live view stops for the same seconds.
 *
 * Neither is a bug to fix in the network code. They are what one core doing two
 * jobs looks like, and the part has two.
 *
 *   core 0   the microphone. I2S DMA, Goertzel, the detector, the buzzer lamp.
 *            Never blocks, never allocates, never touches flash or the network.
 *
 *   core 1   everything that waits. Web server, camera, TLS, settings. It may
 *            block for seconds; nothing it does can stop core 0 reading a block.
 *
 * ---------------------------------------------------------------------------
 *
 * The rules that keep this safe, each one earned from something that can go
 * wrong rather than from a style guide:
 *
 *   The W6300 is core 1's, exclusively. Two cores driving one QSPI peripheral
 *   would interleave register writes inside a single chip transaction. The
 *   network is brought up before core 1 starts, and core 0 never speaks to it
 *   again.
 *
 *   The camera is core 1's, for the same reason, and because the only two
 *   callers - the live stream and the alarm photo - are both on core 1 anyway.
 *
 *   Flash writes stop core 0. Erasing a sector makes the whole XIP window
 *   unreadable, and core 0 executes from it. settings_save() therefore locks
 *   core 0 out for the duration, which needs multicore_lockout_victim_init() on
 *   core 0 before core 1 ever saves.
 *
 *   The one-second network tick moved to core 1. It was a repeating timer, and
 *   a timer callback runs on whichever core created the pool - core 0 - while
 *   the DNS state it advances belongs to core 1.
 *
 *   core 0 does not allocate. pico_multicore turns on a malloc mutex, so it
 *   would be safe rather than corrupt, but safe here means core 0 waiting on a
 *   lock held by an mbedTLS allocation - which is the stall this file exists to
 *   remove.
 */

#ifndef __XCORE_H__
#define __XCORE_H__

#include <stdbool.h>
#include <stdint.h>

/*
 * Core 0 announces, core 1 acts.
 *
 * One writer and one reader per field, and the flag is written last, so no lock
 * is needed: core 1 never sees a flag set over half-written numbers. The detail
 * fields are read while watching too, where a torn float would cost a wrong
 * digit on a status panel and nothing else.
 */
typedef struct {
    volatile bool  boot_pending;    /* say hello, once */
    volatile bool  alarm_pending;   /* a buzzer was heard */
    volatile bool  clear_pending;   /* it has been quiet long enough */

    /* Core 1 counts its own laps. Core 0 prints the figure, which is the only
     * way to tell a quiet console from a core that has stopped. */
    volatile uint32_t laps;

    volatile float hz;              /* what fired, for the message */
    volatile float tone;
    volatile float rms;
    volatile float baseline;
    volatile bool  by_loud;         /* which of the two paths found it */
    volatile float peak_ratio;      /* highest seen during the alarm */
    volatile float peak_rms;
} xcore_t;

extern xcore_t g_xc;

/** Core 1's loop. Never returns. */
void core1_main(void);

#endif /* __XCORE_H__ */
