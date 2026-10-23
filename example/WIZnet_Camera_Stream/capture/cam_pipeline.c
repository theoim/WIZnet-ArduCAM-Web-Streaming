/**
 * @file    cam_pipeline.c
 * @brief   Core 1 captures, core 0 sends: two frame buffers between them.
 *
 * The reason, measured 2026-10-22 on the LH module over HTTPS: the loop was
 * capture (29 ms at VGA) then send (8 ms with TLS) then wait for the next
 * VSYNC. 37 ms is longer than the sensor's frame interval, so every other frame
 * start was missed and the rate halved to 15.0 fps - at 1280x720 as well.
 * Overlapping the two makes the period max(readout, send).
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"

#include "stream_config.h"
#include "cam_pipeline.h"
#include "cam_state.h"
#include "dvp_capture.h"

typedef enum { BUF_FREE = 0, BUF_FILLING, BUF_READY, BUF_SENDING } buf_state_t;

static uint8_t g_buf[2][CAM_FRAME_BUF_SIZE] __attribute__((aligned(4)));
static volatile buf_state_t g_state[2];
static volatile uint32_t    g_len[2];
static volatile uint32_t    g_seq_of[2];

static critical_section_t g_cs;
static volatile uint32_t  g_seq;              /* last frame number completed */
static volatile uint32_t  g_taken_seq;        /* last frame number core 0 took */
static volatile uint32_t  g_captured, g_overwritten;
static volatile uint32_t  g_take_hit, g_take_miss, g_leaks;

static volatile bool g_started;
static volatile bool g_pause_req;
static volatile bool g_paused;

/* Pick a buffer for the next capture: a free one, else the READY one that
 * nobody took (latest frame wins). Never the one being sent. */
static int claim_for_capture(void)
{
    int idx = -1;
    critical_section_enter_blocking(&g_cs);
    for (int i = 0; i < 2 && idx < 0; i++) {
        if (g_state[i] == BUF_FREE) idx = i;
    }
    if (idx < 0) {
        int oldest = -1;
        for (int i = 0; i < 2; i++) {
            if (g_state[i] == BUF_READY &&
                (oldest < 0 || g_seq_of[i] < g_seq_of[oldest])) {
                oldest = i;
            }
        }
        if (oldest >= 0) {
            idx = oldest;
            g_overwritten++;
        }
    }
    if (idx >= 0) g_state[idx] = BUF_FILLING;
    critical_section_exit(&g_cs);
    return idx;
}

static void core1_capture_loop(void)
{
    while (true) {
        if (g_pause_req) {
            g_paused = true;
            while (g_pause_req) tight_loop_contents();
            g_paused = false;
            continue;
        }
        if (!cam_state_streaming()) {
            sleep_ms(2);
            continue;
        }

        int idx = claim_for_capture();
        if (idx < 0) {                 /* both busy: core 0 is mid-send of one */
            tight_loop_contents();
            continue;
        }

        int rc = cam_capture_frame_into(g_buf[idx], CAM_FRAME_BUF_SIZE);
        uint32_t len = camera.frame_length;
        bool ok = (rc == 0 && len >= 2 && g_buf[idx][0] == 0xFF && g_buf[idx][1] == 0xD8);

        critical_section_enter_blocking(&g_cs);
        if (ok) {
            g_len[idx]    = len;
            g_seq_of[idx] = ++g_seq;
            g_state[idx]  = BUF_READY;
            g_captured++;
        } else {
            g_state[idx] = BUF_FREE;
        }
        critical_section_exit(&g_cs);

        if (ok) {
            cam_state_note_capture_ok();
        } else if (rc != 0) {
            /* Recovery resets the sensor over SCCB - core 1 owns it right now. */
            if (cam_state_note_capture_fail()) {
                printf("No frames for %u attempts - sensor recovered (core 1)\n",
                       FAIL_STREAK_RECOVER);
            }
        } else {
            cam_state_note_dropped();          /* captured, but no SOI */
        }
    }
}

void cam_pipeline_start(void)
{
    critical_section_init(&g_cs);
    for (int i = 0; i < 2; i++) g_state[i] = BUF_FREE;
    multicore_launch_core1(core1_capture_loop);
    g_started = true;
    printf("[pipe] core 1 capturing into 2 x %u KB, core 0 serving the network\n",
           (unsigned)(CAM_FRAME_BUF_SIZE / 1024));
}

bool cam_pipeline_take(cam_frame_t *f)
{
    int best = -1;
    int leaked = -1;
    critical_section_enter_blocking(&g_cs);
    /*
     * Core 0 sends one frame at a time and only asks for the next one after
     * releasing the last, so at this point no buffer can legitimately be
     * SENDING. One that is was never released - and while it stays that way
     * core 1 is left one buffer, recaptures into it the moment it finishes,
     * and core 0 almost never sees it READY: the picture freezes while
     * cap_frames keeps climbing. Seen once on 2026-10-22 (OV2640, 24 min,
     * cause not found by reading the code). Reclaim it and count it.
     */
    for (int i = 0; i < 2; i++) {
        if (g_state[i] == BUF_SENDING) {
            g_state[i] = BUF_FREE;
            g_leaks++;
            leaked = i;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (g_state[i] == BUF_READY && g_seq_of[i] > g_taken_seq &&
            (best < 0 || g_seq_of[i] > g_seq_of[best])) {
            best = i;
        }
    }
    if (best >= 0) {
        g_state[best] = BUF_SENDING;
        g_taken_seq   = g_seq_of[best];
        f->data = g_buf[best];
        f->len  = g_len[best];
        f->seq  = g_seq_of[best];
        f->idx  = best;
        g_take_hit++;
    } else {
        g_take_miss++;
    }
    critical_section_exit(&g_cs);
    if (leaked >= 0) {
        printf("[pipe] buffer %d was still SENDING when the next frame was asked"
               " for - reclaimed (leaks=%lu)\n", leaked, (unsigned long)g_leaks);
    }
    return best >= 0;
}

void cam_pipeline_debug(char *out, unsigned cap)
{
    static const char *nm[] = {"FREE", "FILLING", "READY", "SENDING"};
    snprintf(out, cap, "buf0=%s(seq %lu) buf1=%s(seq %lu) last_seq=%lu taken=%lu"
             " hit=%lu miss=%lu leaks=%lu paused=%d",
             nm[g_state[0]], (unsigned long)g_seq_of[0],
             nm[g_state[1]], (unsigned long)g_seq_of[1],
             (unsigned long)g_seq, (unsigned long)g_taken_seq,
             (unsigned long)g_take_hit, (unsigned long)g_take_miss,
             (unsigned long)g_leaks, g_paused ? 1 : 0);
}

void cam_pipeline_release(const cam_frame_t *f)
{
    critical_section_enter_blocking(&g_cs);
    if (g_state[f->idx] == BUF_SENDING) g_state[f->idx] = BUF_FREE;
    critical_section_exit(&g_cs);
}

void cam_pipeline_pause(void)
{
    if (!g_started || get_core_num() == 1) return;
    g_pause_req = true;
    /* A capture in progress finishes first: at most one frame, or the 2 s VSYNC
     * timeout if the sensor is dead. */
    absolute_time_t until = make_timeout_time_ms(2500);
    while (!g_paused && absolute_time_diff_us(get_absolute_time(), until) > 0) {
        tight_loop_contents();
    }
    if (!g_paused) printf("[pipe] core 1 did not pause within 2.5 s\n");
}

void cam_pipeline_resume(void)
{
    if (!g_started || get_core_num() == 1) return;
    g_pause_req = false;
}

uint32_t cam_pipeline_captured(void)    { return g_captured; }
uint32_t cam_pipeline_overwritten(void) { return g_overwritten; }
uint32_t cam_pipeline_take_hits(void)   { return g_take_hit; }
uint32_t cam_pipeline_take_misses(void) { return g_take_miss; }
uint32_t cam_pipeline_leaks(void)       { return g_leaks; }
