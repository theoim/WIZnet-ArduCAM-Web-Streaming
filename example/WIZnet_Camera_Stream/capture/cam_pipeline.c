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
#include "pico/flash.h"
#include "pico/critical_section.h"

#include "stream_config.h"
#include "cam_pipeline.h"
#include "cam_state.h"
#include "cam_driver.h"
#include "dvp_capture.h"

typedef enum { BUF_FREE = 0, BUF_FILLING, BUF_READY, BUF_SENDING } buf_state_t;

static uint8_t g_buf[2][CAM_FRAME_BUF_SIZE] __attribute__((aligned(4)));
static volatile buf_state_t g_state[2];
static volatile uint32_t    g_len[2];
static volatile uint32_t    g_seq_of[2];
static volatile uint64_t    g_time_of[2];       /* capture completed, us since boot */
static volatile uint8_t     g_readers[2];       /* snapshots sending from it        */

static critical_section_t g_cs;
static volatile uint32_t  g_seq;              /* last frame number completed */
static volatile uint32_t  g_taken_seq;        /* last frame number core 0 took */
static volatile uint32_t  g_captured, g_overwritten;
static volatile uint32_t  g_take_hit, g_take_miss, g_leaks;

static volatile bool g_started;
static volatile bool g_pause_req;
static volatile bool g_paused;
static volatile bool g_oneshot_req;            /* one capture while not streaming */

/* ---- control command slot (cam_pipeline.h) ---- */
typedef enum { CMD_EMPTY = 0, CMD_POSTED, CMD_RUNNING, CMD_DONE } cmd_state_t;

static cam_ctrl_batch_t     g_cmd_batch;
static cam_ctrl_result_t    g_cmd_result;
static volatile cmd_state_t g_cmd_state;
static volatile bool        g_cmd_abandoned;   /* core 0 stopped waiting      */
static cam_ctrl_written_t   g_written;
static volatile uint32_t    g_cmd_applied, g_cmd_timeouts;

/* Pick a buffer for the next capture: a free one, else the READY one that
 * nobody took (latest frame wins). Never the one being sent, by the stream or
 * by a snapshot. */
static int claim_for_capture(void)
{
    int idx = -1;
    critical_section_enter_blocking(&g_cs);
    for (int i = 0; i < 2 && idx < 0; i++) {
        if (g_state[i] == BUF_FREE && g_readers[i] == 0) idx = i;
    }
    if (idx < 0) {
        int oldest = -1;
        for (int i = 0; i < 2; i++) {
            if (g_state[i] == BUF_READY && g_readers[i] == 0 &&
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

/* Core 1, between frames: write a posted control batch, record the result. */
static void core1_service_command(void)
{
    if (g_cmd_state != CMD_POSTED) {
        return;                                  /* cheap check, no lock */
    }
    critical_section_enter_blocking(&g_cs);
    bool mine = (g_cmd_state == CMD_POSTED);     /* core 0 may have withdrawn it */
    if (mine) g_cmd_state = CMD_RUNNING;
    critical_section_exit(&g_cs);
    if (!mine) {
        return;
    }

    cam_ctrl_result_t res;
    cam_ctrl_run(&g_cmd_batch, cam_sensor_set_control, &res);

    critical_section_enter_blocking(&g_cs);
    cam_ctrl_written_record(&g_written, &g_cmd_batch, &res);
    g_cmd_result = res;
    g_cmd_applied++;
    if (g_cmd_abandoned) {
        g_cmd_abandoned = false;                 /* nobody is waiting: free it */
        g_cmd_state = CMD_EMPTY;
    } else {
        g_cmd_state = CMD_DONE;
    }
    critical_section_exit(&g_cs);

    if (res.failed_at >= 0) {
        printf("[ctrl] write %d of %u failed (rc %d), %u written before it\n",
               res.failed_at + 1, (unsigned)g_cmd_batch.n, res.err, (unsigned)res.n_ok);
    }
}

static void core1_capture_loop(void)
{
    /* Lets core 0 park this core in RAM while it erases or programs the
     * settings sectors (flash_safe_execute, multicore lockout). */
    flash_safe_execute_core_init();
    while (true) {
        if (g_pause_req) {
            g_paused = true;
            while (g_pause_req) tight_loop_contents();
            g_paused = false;
            continue;
        }
        core1_service_command();
        bool oneshot = false;
        if (!cam_state_streaming()) {
            if (!g_oneshot_req) {
                sleep_ms(2);
                continue;
            }
            oneshot = true;            /* a snapshot asked; streaming stays off */
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
            g_len[idx]     = len;
            g_seq_of[idx]  = ++g_seq;
            g_time_of[idx] = time_us_64();
            g_state[idx]   = BUF_READY;
            g_captured++;
            if (oneshot) g_oneshot_req = false;   /* a failure retries until withdrawn */
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

/* ------------------------------- Snapshot --------------------------------- */
/* Newest READY buffer captured after `after_us` with a sequence above
 * `min_seq`, or -1. Caller holds g_cs. */
static int newest_ready(uint32_t min_seq, uint64_t after_us)
{
    int best = -1;
    for (int i = 0; i < 2; i++) {
        if (g_state[i] == BUF_READY && g_seq_of[i] > min_seq &&
            g_time_of[i] >= after_us &&
            (best < 0 || g_seq_of[i] > g_seq_of[best])) {
            best = i;
        }
    }
    return best;
}

bool cam_pipeline_snapshot_hold(cam_frame_t *f, uint32_t timeout_ms, bool *fresh)
{
    if (!g_started || get_core_num() == 1) {
        return false;
    }
    uint64_t now = time_us_64();
    uint64_t recent = now > CAM_SNAPSHOT_MAX_AGE_MS * 1000ull
                    ? now - CAM_SNAPSHOT_MAX_AGE_MS * 1000ull : 0;
    uint32_t seq0;
    int      idx;

    critical_section_enter_blocking(&g_cs);
    seq0 = g_seq;
    /* Streaming: the newest frame will do if it is recent. Stopped: a READY
     * buffer is whatever was left when the stream stopped, so always capture. */
    idx = cam_state_streaming() ? newest_ready(0, recent) : -1;
    if (idx < 0 && !cam_state_streaming()) {
        g_oneshot_req = true;
    }
    if (idx >= 0) g_readers[idx]++;
    critical_section_exit(&g_cs);

    absolute_time_t until = make_timeout_time_ms(timeout_ms);
    while (idx < 0) {
        if (absolute_time_diff_us(get_absolute_time(), until) <= 0) {
            g_oneshot_req = false;       /* withdrawn; a capture in flight lands as READY */
            return false;
        }
        tight_loop_contents();
        critical_section_enter_blocking(&g_cs);
        idx = newest_ready(seq0, now);
        if (idx >= 0) g_readers[idx]++;
        critical_section_exit(&g_cs);
    }

    f->data = g_buf[idx];
    f->len  = g_len[idx];
    f->seq  = g_seq_of[idx];
    f->idx  = idx;
    if (fresh) *fresh = (f->seq > seq0);
    return true;
}

void cam_pipeline_snapshot_release(const cam_frame_t *f)
{
    critical_section_enter_blocking(&g_cs);
    if (g_readers[f->idx]) g_readers[f->idx]--;
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

/* ---------------------------- Control commands ---------------------------- */
const char *cam_cmd_rc_name(cam_cmd_rc_t rc)
{
    switch (rc) {
    case CAM_CMD_OK:              return "ok";
    case CAM_CMD_FAILED:          return "write failed";
    case CAM_CMD_BUSY:            return "previous command still running";
    case CAM_CMD_TIMEOUT_NOT_RUN: return "not started in time, withdrawn";
    case CAM_CMD_TIMEOUT_RUNNING: return "started, not finished in time";
    }
    return "?";
}

cam_cmd_rc_t cam_pipeline_set_controls(const cam_ctrl_batch_t *b, cam_ctrl_result_t *res,
                                       uint32_t timeout_ms)
{
    /* No capture loop to hand it to: the caller owns the sensor. */
    if (!g_started || get_core_num() == 1) {
        cam_ctrl_run(b, cam_sensor_set_control, res);
        cam_ctrl_written_record(&g_written, b, res);
        g_cmd_applied++;
        return res->failed_at < 0 ? CAM_CMD_OK : CAM_CMD_FAILED;
    }

    critical_section_enter_blocking(&g_cs);
    if (g_cmd_state != CMD_EMPTY) {
        critical_section_exit(&g_cs);
        return CAM_CMD_BUSY;
    }
    g_cmd_batch     = *b;
    g_cmd_abandoned = false;
    g_cmd_state     = CMD_POSTED;
    critical_section_exit(&g_cs);

    absolute_time_t until = make_timeout_time_ms(timeout_ms);
    while (g_cmd_state != CMD_DONE &&
           absolute_time_diff_us(get_absolute_time(), until) > 0) {
        tight_loop_contents();
    }

    cam_cmd_rc_t rc;
    critical_section_enter_blocking(&g_cs);
    if (g_cmd_state == CMD_DONE) {
        *res = g_cmd_result;
        g_cmd_state = CMD_EMPTY;
        rc = res->failed_at < 0 ? CAM_CMD_OK : CAM_CMD_FAILED;
    } else if (g_cmd_state == CMD_POSTED) {
        g_cmd_state = CMD_EMPTY;                 /* withdrawn: nothing written */
        g_cmd_timeouts++;
        rc = CAM_CMD_TIMEOUT_NOT_RUN;
    } else {
        g_cmd_abandoned = true;                  /* core 1 frees the slot later */
        g_cmd_timeouts++;
        rc = CAM_CMD_TIMEOUT_RUNNING;
    }
    critical_section_exit(&g_cs);
    if (rc != CAM_CMD_OK && rc != CAM_CMD_FAILED) {
        printf("[ctrl] %u control(s): %s after %lu ms\n", (unsigned)b->n,
               cam_cmd_rc_name(rc), (unsigned long)timeout_ms);
    }
    return rc;
}

bool cam_pipeline_control_written(unsigned idx, int16_t *value)
{
    if (!g_started) {
        return cam_ctrl_written_get(&g_written, idx, value);
    }
    critical_section_enter_blocking(&g_cs);
    bool ok = cam_ctrl_written_get(&g_written, idx, value);
    critical_section_exit(&g_cs);
    return ok;
}

void cam_pipeline_controls_forget(const char *why)
{
    uint32_t had;
    if (!g_started) {
        had = g_written.mask;
        cam_ctrl_written_clear(&g_written);
    } else {
        critical_section_enter_blocking(&g_cs);
        had = g_written.mask;
        cam_ctrl_written_clear(&g_written);
        critical_section_exit(&g_cs);
    }
    if (had) {
        printf("[ctrl] %s: written control values forgotten (sensor may not"
               " have kept them)\n", why);
    }
}

uint32_t cam_pipeline_cmd_applied(void)  { return g_cmd_applied; }
uint32_t cam_pipeline_cmd_timeouts(void) { return g_cmd_timeouts; }

uint32_t cam_pipeline_captured(void)    { return g_captured; }
uint32_t cam_pipeline_overwritten(void) { return g_overwritten; }
uint32_t cam_pipeline_take_hits(void)   { return g_take_hit; }
uint32_t cam_pipeline_take_misses(void) { return g_take_miss; }
uint32_t cam_pipeline_leaks(void)       { return g_leaks; }
