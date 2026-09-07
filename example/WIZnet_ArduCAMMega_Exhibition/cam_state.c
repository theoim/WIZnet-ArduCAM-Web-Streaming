/**
 * @file    cam_state.c
 * @brief   Capture state, the metrics window, and the status document.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"

#include "cam_state.h"
#include "cam_controls.h"
#include "net_load.h"
#include "exhibition_config.h"
#include "arducam_mega.h"

/* -------------------------------- Variables -------------------------------- */
static volatile bool  g_streaming   = false;
static volatile res_t g_resolution  = RES_320X240;

static uint32_t g_frame_count = 0;
static uint32_t g_drop_count  = 0;
static uint32_t g_fail_streak = 0;

/*
 * Reported values, averaged over a one-second window.
 *
 * The frame period is kept in three parts rather than one, because "22 fps" on
 * its own does not say what to change:
 *
 *   vsync_ms  waiting for the sensor to start the next frame
 *   read_ms   pulling pixels out of the sensor (PIO + DMA)
 *   send_ms   putting the JPEG on the wire
 *
 *   1000 / fps  ~=  vsync_ms + read_ms + send_ms
 *
 * Under load only send_ms should move. That is what makes the load demo
 * readable: the bar that grows names the stack as the cost, not the sensor.
 */
static uint32_t g_fps_x10   = 0;
static uint32_t g_vsync_ms  = 0;
static uint32_t g_read_ms   = 0;
static uint32_t g_send_ms   = 0;
static uint32_t g_drain_ms  = 0;
static uint32_t g_frame_kb  = 0;
static uint32_t g_load_kbps = 0;

static uint32_t        g_win_frames   = 0;
static uint64_t        g_win_vsync_us = 0;
static uint64_t        g_win_read_us  = 0;
static uint64_t        g_win_send_us  = 0;
static uint64_t        g_win_drain_us = 0;
static uint64_t        g_win_bytes    = 0;
static absolute_time_t g_win_start;

/* ---------------------------------- Setup ---------------------------------- */
void cam_state_init(void)
{
    g_win_start = get_absolute_time();
}

bool cam_state_streaming(void)
{
    return g_streaming;
}

void cam_state_set_streaming(bool on)
{
    g_streaming = on;
    if (on) {
        g_frame_count = 0;
        g_drop_count  = 0;
        cam_state_metrics_reset();
    }
}

res_t cam_state_resolution(void)
{
    return g_resolution;
}

/* ------------------------------- Resolution -------------------------------- */
const char *cam_state_res_string(res_t res)
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

/** Match a token that is not NUL-terminated - it is a slice of a URL. */
bool cam_state_res_parse(const char *s, size_t len, res_t *out)
{
    static const struct { const char *name; res_t res; } table[] = {
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

void cam_state_apply_resolution(res_t res)
{
    if (res == g_resolution) {
        return;
    }
    g_resolution = res;
    arducam_mega.set_frame_size(res);
    sleep_ms(200);                      /* let the sensor settle */

    /*
     * set_frame_size() re-applies the clock dividers and puts the sensor back
     * to its own defaults for everything else, so the panel would be showing
     * values the hardware no longer has. Push the table back.
     */
    cam_controls_apply_all();

    cam_state_metrics_reset();          /* the old window described the old mode */
    printf("Resolution changed to %s\n", cam_state_res_string(res));
}

void cam_state_recover(void)
{
    arducam_recover();

    /*
     * Re-apply only the controls someone actually set. On the lwIP image this
     * runs while the MCU *is* the TCP stack, so every extra I2C transaction
     * here is time nothing is being acknowledged - and this path runs precisely
     * when things are already going badly.
     */
    cam_controls_apply_all();

    g_fail_streak = 0;
    cam_state_metrics_reset();
}

/* --------------------------------- Counters -------------------------------- */
void cam_state_note_dropped(void)
{
    g_drop_count++;
}

bool cam_state_note_capture_fail(void)
{
    g_drop_count++;

    /*
     * A run of failures means the sensor stopped emitting frames rather than
     * that one capture went bad - typically a clock setting it could not take.
     * Reset it instead of spinning on a dead sensor.
     */
    if (++g_fail_streak >= FAIL_STREAK_RECOVER) {
        cam_state_recover();
        return true;
    }
    return false;
}

void cam_state_note_capture_ok(void)
{
    g_fail_streak = 0;
}

void cam_state_note_drain(uint64_t drain_us)
{
    g_win_drain_us += drain_us;
}

void cam_state_note_frame(uint64_t send_us, uint32_t bytes)
{
    g_frame_count++;
    g_win_frames++;
    g_win_vsync_us += arducam_vsync_wait_us;
    g_win_read_us  += arducam_readout_us;
    g_win_send_us  += send_us;
    g_win_bytes    += bytes;
}

/* --------------------------------- Metrics --------------------------------- */
void cam_state_metrics_reset(void)
{
    g_win_frames   = 0;
    g_win_vsync_us = 0;
    g_win_read_us  = 0;
    g_win_send_us  = 0;
    g_win_drain_us = 0;
    g_win_bytes    = 0;
    g_win_start    = get_absolute_time();
    (void)net_load_take_bytes();        /* the load window restarts with it */
}

void cam_state_metrics_update(void)
{
    int64_t  elapsed_us = absolute_time_diff_us(g_win_start, get_absolute_time());
    uint32_t load_bytes;

    if (elapsed_us < 1000000) {
        return;
    }

    if (g_win_frames) {
        g_fps_x10  = (uint32_t)(((uint64_t)g_win_frames * 10000000ULL) /
                                (uint64_t)elapsed_us);
        g_vsync_ms = (uint32_t)(g_win_vsync_us / g_win_frames / 1000ULL);
        g_read_ms  = (uint32_t)(g_win_read_us  / g_win_frames / 1000ULL);
        g_send_ms  = (uint32_t)(g_win_send_us  / g_win_frames / 1000ULL);
        g_drain_ms = (uint32_t)(g_win_drain_us / g_win_frames / 1000ULL);
        g_frame_kb = (uint32_t)(g_win_bytes    / g_win_frames / 1024ULL);
    } else {
        g_fps_x10 = 0;
    }

    /*
     * Load is reported as what was actually served, not as what was asked for.
     * A browser that cannot keep the requests coming drags this number down
     * with it, which is the truthful reading - the device is not under load it
     * is not being given.
     */
    load_bytes  = net_load_take_bytes();
    g_load_kbps = (uint32_t)(((uint64_t)load_bytes * 8000ULL) /
                             (uint64_t)elapsed_us);   /* kbit/s */

    g_win_frames   = 0;
    g_win_vsync_us = 0;
    g_win_read_us  = 0;
    g_win_send_us  = 0;
    g_win_drain_us = 0;
    g_win_bytes    = 0;
    g_win_start    = get_absolute_time();
}

/* -------------------------------- Reporting -------------------------------- */
int cam_state_status_json(char *buf, size_t cap)
{
    int    n;
    size_t used;

    n = snprintf(buf, cap,
                 "{\"streaming\":%s,\"res\":\"%s\","
                 "\"frames\":%lu,\"dropped\":%lu,"
                 "\"fps\":%lu.%lu,\"vsync_ms\":%lu,\"read_ms\":%lu,"
                 "\"send_ms\":%lu,\"drain_ms\":%lu,\"kb\":%lu,"
                 "\"clk_div\":%u,\"pll_div\":%u,"
                 "\"load_kbps\":%lu,"
                 "\"stack\":\"" STACK_NAME "\",\"ctrl\":{",
                 g_streaming ? "true" : "false",
                 cam_state_res_string(g_resolution),
                 (unsigned long)g_frame_count,
                 (unsigned long)g_drop_count,
                 (unsigned long)(g_fps_x10 / 10),
                 (unsigned long)(g_fps_x10 % 10),
                 (unsigned long)g_vsync_ms,
                 (unsigned long)g_read_ms,
                 (unsigned long)g_send_ms,
                 (unsigned long)g_drain_ms,
                 (unsigned long)g_frame_kb,
                 (unsigned)arducam_clk_div,
                 (unsigned)arducam_pll_div,
                 (unsigned long)g_load_kbps);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    used = (size_t)n;

    n = cam_controls_json_values(buf + used, cap - used);
    if (n < 0) {
        return -1;
    }
    used += (size_t)n;

    n = snprintf(buf + used, cap - used, "}}");
    if (n < 0 || (size_t)n >= cap - used) {
        return -1;
    }
    return (int)(used + (size_t)n);
}
