/**
 * @file    cam_state.c
 * @brief   Capture state, the metrics window, and the status document.
 */

#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/regs/addressmap.h"

#include "cam_state.h"
#include "net_load.h"
#include "stream_config.h"
#include "cam_driver.h"
#include "dvp_capture.h"
/* Always: the control slot and the written values live there, and work
 * without the capture loop too (STREAM_PIPELINE 0 writes directly). */
#include "cam_pipeline.h"
#if STREAM_PIPELINE
extern uint32_t g_push_calls;           /* server_toe.c */
#endif
#if defined(STREAM_HTTPS)
#include "tls_server.h"
#endif
#include "net_server.h"
#include "settings.h"

/* ------------------------------ SRAM reporting ----------------------------- */
/*
 * How much of the chip's RAM this build is actually using, reported live so it
 * can be read while the stream runs rather than inferred from a map file.
 *
 * The number that matters at an exhibition is the one taken under load: a map
 * file shows what was reserved at link time, and the interesting question is
 * what a 1080p stream costs on top of that.
 *
 * used = statics + heap actually handed out.
 *
 *   statics   everything the linker placed, up to __bss_end__. On this build
 *             that is dominated by one object: image_buff, the 200 KB JPEG
 *             capture buffer in ov2640.c. It does not grow with the
 *             selected resolution - a 1080p frame lands in the same array a
 *             QVGA frame does - so the resolution buttons move the network
 *             numbers, not this one.
 *   heap      mallinfo().uordblks, the bytes currently allocated. The lwIP
 *             image lives here; the TOE image barely touches it.
 *
 * The stack is NOT counted. Its 8 KB sits above __StackLimit and how much of it
 * is actually touched is not something malloc can see, so the figure reported
 * here is a floor, not a ceiling.
 */
extern char __StackTop;
extern char __StackLimit;
extern char __bss_end__;

uint32_t cam_state_sram_total(void)
{
    return (uint32_t)(&__StackTop - (char *)SRAM_BASE);
}

uint32_t cam_state_sram_used(void)
{
    struct mallinfo mi = mallinfo();
    uint32_t statics = (uint32_t)(&__bss_end__ - (char *)SRAM_BASE);

    return statics + (uint32_t)mi.uordblks;
}

/* -------------------------------- Variables -------------------------------- */
static volatile bool  g_streaming   = false;
/* HD, not QVGA. The board is powered on before the doors open and whatever is
 * set here is what a visitor walks up to, so it has to be a mode worth looking
 * at - and one of the two the page offers, or the resolution bar would start
 * with neither button lit. main.c configures the sensor from this value at
 * boot, so changing it here is enough. */
/*
 * SVGA at boot, where the exhibition build started at 720p.
 *
 * The OV2640 has no 1280x720 - its modes step 640x480, 800x600, 1024x768 - and
 * 800x600 is the nearest one that still looks like a video feed on a projector.
 */
/* The driver's measured default unless the build overrides it
 * (-DSTREAM_DEFAULT_RES=RES_640X480 for the VGA work). */
#ifdef STREAM_DEFAULT_RES
static volatile res_t g_resolution  = STREAM_DEFAULT_RES;
#else
static volatile res_t g_resolution  = (res_t)0;   /* from cam_caps in cam_state_init() */
#endif

static uint32_t g_frame_count = 0;
static uint32_t g_drop_count  = 0;
static uint32_t g_too_big     = 0;      /* frames larger than the buffer */
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

/*
 * When the last frame went out, for /api/status.
 *
 * The window above is only advanced by the streaming path, so once nobody
 * holds /stream open it freezes at its last value and the status endpoint kept
 * reporting it - 17 fps from a camera that had captured nothing for minutes.
 * Measured on 2026-10-22: two readings at different resolutions came back
 * identical down to the frame count. age_ms makes that visible, and fps is
 * reported as 0 once the last frame is older than CAM_STATUS_STALE_MS.
 */
#ifndef CAM_STATUS_STALE_MS
#define CAM_STATUS_STALE_MS  2000u
#endif
static uint64_t g_last_frame_us = 0;     /* 0 = no frame since boot */

/*
 * Stream reopens since boot, by the reason the page gave.
 *
 * The device validates a frame by its SOI and EOI markers only, so a JPEG
 * damaged in the middle counts as delivered here and fails in the browser,
 * which then reopens the stream (r=err). The page also reopens when the
 * picture stops updating (r=idle) - which a background tab once faked 214
 * times in one run. A clean run is dropped 0 AND r_err 0, and r_idle tells
 * whether the viewer, rather than the camera, was the problem.
 */
static uint32_t g_reopens = 0, g_r_err = 0, g_r_idle = 0, g_r_stall = 0;
static uint32_t g_opens = 0;

static bool why_is(const char *w, size_t n, const char *lit)
{
    size_t k = strlen(lit);
    return n == k && memcmp(w, lit, k) == 0;
}

void cam_state_note_stream_open(const char *why, size_t why_len)
{
    g_opens++;
    if (why_is(why, why_len, "open")) {
        return;                         /* first view on a page load */
    }
    g_reopens++;
    if (why_is(why, why_len, "err"))   g_r_err++;
    if (why_is(why, why_len, "idle"))  g_r_idle++;
    if (why_is(why, why_len, "stall")) g_r_stall++;
}

/* ---------------------------------- Setup ---------------------------------- */
void cam_state_init(void)
{
    if ((int)g_resolution == 0) {
        g_resolution = cam_caps.default_res;
    }
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

void cam_state_set_boot_resolution(res_t res)
{
    g_resolution = res;
}

res_t cam_state_resolution(void)
{
    /*
     * The default comes from the driver (cam_caps.default_res), which a static
     * initialiser cannot read. main() asks for the resolution before
     * cam_state_init() runs, so the first caller fills it in - otherwise boot
     * tried to set size 0 and logged a refusal for a sensor that was fine.
     */
    if ((int)g_resolution == 0) {
        g_resolution = cam_caps.default_res;
    }
    return g_resolution;
}

/* ------------------------------- Resolution -------------------------------- */
const char *cam_state_res_string(res_t res)
{
    switch (res) {
    case RES_160X120:   return "160x120";
    case RES_176X144:   return "176x144";
    case RES_320X240:   return "320x240";
    case RES_352X288:   return "352x288";
    case RES_640X480:   return "640x480";
    case RES_800X600:   return "800x600";
    case RES_1024X768:  return "1024x768";
    case RES_1280X720:  return "1280x720";
    case RES_1280X1024: return "1280x1024";
    case RES_1600X1200: return "1600x1200";
    case RES_1920X1080: return "1920x1080";
    default:            return "unknown";
    }
}

/* Smallest first: the status lists the sizes the sensor supports in this order. */
static const struct { const char *name; res_t res; } k_res_table[] = {
    { "160x120",   RES_160X120   },
    { "176x144",   RES_176X144   },
    { "320x240",   RES_320X240   },
    { "352x288",   RES_352X288   },
    { "640x480",   RES_640X480   },
    { "800x600",   RES_800X600   },
    { "1024x768",  RES_1024X768  },
    { "1280x720",  RES_1280X720  },
    { "1280x1024", RES_1280X1024 },
    { "1600x1200", RES_1600X1200 },
    { "1920x1080", RES_1920X1080 },
};
#define RES_TABLE_COUNT (sizeof(k_res_table) / sizeof(k_res_table[0]))

/** Match a token that is not NUL-terminated - it is a slice of a URL. */
bool cam_state_res_parse(const char *s, size_t len, res_t *out)
{
    size_t i;

    for (i = 0; i < RES_TABLE_COUNT; i++) {
        size_t n = strlen(k_res_table[i].name);
        if (n == len && memcmp(s, k_res_table[i].name, n) == 0) {
            *out = k_res_table[i].res;
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
#if STREAM_PIPELINE
    cam_pipeline_pause();               /* core 1 off the sensor first */
#endif
    int set_rc = cam_sensor_supports(res) ? cam_set_framesize(res) : -1;
#if STREAM_PIPELINE
    cam_pipeline_resume();
#endif
    if (set_rc != 0) {
        printf("Resolution %s refused by %s\n", cam_state_res_string(res), cam_caps.name);
        return;
    }
    g_resolution = res;
    sleep_ms(200);                      /* let the sensor settle */

    /*
     * A frame-size change may put the sensor back to its own defaults for
     * everything else (the OV2640 tables do). Whether the Arducam controller
     * keeps its control registers across one is not known, so the written
     * values stop being claimed rather than shown as if they still held.
     */
    cam_pipeline_controls_forget("frame size changed");

    cam_state_metrics_reset();          /* the old window described the old mode */
    printf("Resolution changed to %s\n", cam_state_res_string(res));
}

void cam_state_recover(void)
{
#if STREAM_PIPELINE
    cam_pipeline_pause();               /* no-op when core 1 calls this */
#endif
    cam_recover();
#if STREAM_PIPELINE
    cam_pipeline_resume();
#endif
    cam_pipeline_controls_forget("sensor reset");

    /*
     * Written controls are not pushed back after a reset: this path runs when
     * the sensor has already stopped producing frames, and writing values that
     * may be what broke it would only add to that. They are forgotten above
     * instead, so nothing claims they still hold.
     */

    g_fail_streak = 0;
    cam_state_metrics_reset();
}

/* --------------------------------- Counters -------------------------------- */
void cam_state_note_dropped(void)
{
    g_drop_count++;
}

/* A frame that did not fit the buffer: dropped, but the sensor is working, so
 * it neither counts towards nor breaks a failure streak. */
void cam_state_note_too_big(void)
{
    g_drop_count++;
    g_too_big++;
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
    g_last_frame_us = time_us_64();
    g_win_vsync_us += cam_vsync_wait_us;
    g_win_read_us  += cam_readout_us;
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
    char   tls_frag[560] = "";
#if defined(STREAM_HTTPS)
    tls_status_json(tls_frag, sizeof(tls_frag));
#endif
#if STREAM_PIPELINE
    {
        size_t k = strlen(tls_frag);
        snprintf(tls_frag + k, sizeof(tls_frag) - k,
                 "\"pipe\":1,\"cap_frames\":%lu,\"cap_over\":%lu,"
                 "\"take_hit\":%lu,\"take_miss\":%lu,\"pipe_leak\":%lu,\"push_calls\":%lu,",
                 (unsigned long)cam_pipeline_captured(),
                 (unsigned long)cam_pipeline_overwritten(),
                 (unsigned long)cam_pipeline_take_hits(),
                 (unsigned long)cam_pipeline_take_misses(),
                 (unsigned long)cam_pipeline_leaks(),
                 (unsigned long)g_push_calls);
    }
#endif
    {
        size_t k = strlen(tls_frag);
        net_server_status_json(tls_frag + k, sizeof(tls_frag) - k);
    }
    long   age_ms = g_last_frame_us
                  ? (long)((time_us_64() - g_last_frame_us) / 1000ULL) : -1L;
    bool   stale  = (age_ms < 0) || ((unsigned long)age_ms > CAM_STATUS_STALE_MS);
    uint32_t fps_x10 = stale ? 0u : g_fps_x10;

    n = snprintf(buf, cap,
                 "{\"streaming\":%s,\"res\":\"%s\","
                 "\"frames\":%lu,\"dropped\":%lu,\"too_big\":%lu,"
                 "\"fps\":%lu.%lu,\"vsync_ms\":%lu,\"read_ms\":%lu,"
                 "\"send_ms\":%lu,\"drain_ms\":%lu,\"kb\":%lu,"
                 "\"clk_div\":%u,\"pll_div\":%u,"
                 "\"load_kbps\":%lu,"
                 "\"sram_used\":%lu,\"sram_total\":%lu,"
                 "\"age_ms\":%ld,\"stale\":%s,"
                 "\"opens\":%lu,\"reopens\":%lu,\"r_err\":%lu,\"r_idle\":%lu,\"r_stall\":%lu,"
                 "%s\"stack\":\"" STACK_NAME "\",\"ctrl\":{",
                 g_streaming ? "true" : "false",
                 cam_state_res_string(g_resolution),
                 (unsigned long)g_frame_count,
                 (unsigned long)g_drop_count,
                 (unsigned long)g_too_big,
                 (unsigned long)(fps_x10 / 10),
                 (unsigned long)(fps_x10 % 10),
                 (unsigned long)g_vsync_ms,
                 (unsigned long)g_read_ms,
                 (unsigned long)g_send_ms,
                 (unsigned long)g_drain_ms,
                 (unsigned long)g_frame_kb,
                 (unsigned)cam_clk_div,
                 (unsigned)cam_pll_div,
                 (unsigned long)g_load_kbps,
                 (unsigned long)cam_state_sram_used(),
                 (unsigned long)cam_state_sram_total(),
                 age_ms,
                 stale ? "true" : "false",
                 (unsigned long)g_opens, (unsigned long)g_reopens,
                 (unsigned long)g_r_err, (unsigned long)g_r_idle,
                 (unsigned long)g_r_stall,
                 tls_frag);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    used = (size_t)n;

    /*
     * ctrl: the last value written to each control since the sensor was last
     * reset or resized. Not read back from the sensor; a control nobody set
     * since then is absent rather than shown as a default.
     */
    {
        unsigned cnt = 0;
        const cam_ctrl_desc_t *t = cam_sensor_controls(&cnt);
        bool first = true;
        for (unsigned i = 0; t && i < cnt; i++) {
            int16_t v;
            if (!cam_pipeline_control_written(i, &v)) {
                continue;
            }
            n = snprintf(buf + used, cap - used, "%s\"%s\":%d", first ? "" : ",",
                         t[i].name, v);
            if (n < 0 || (size_t)n >= cap - used) {
                return -1;
            }
            used += (size_t)n;
            first = false;
        }
    }
    n = snprintf(buf + used, cap - used, "},\"ctrl_applied\":%lu,\"ctrl_timeouts\":%lu",
                 (unsigned long)cam_pipeline_cmd_applied(),
                 (unsigned long)cam_pipeline_cmd_timeouts());
    if (n < 0 || (size_t)n >= cap - used) {
        return -1;
    }
    used += (size_t)n;

    /* build: which image is running - the boot log is usually gone by the time
     * USB serial is attached. settings: what is saved in flash. */
    n = snprintf(buf + used, cap - used, ",\"build\":\"%s\",", CAM_BUILD_GIT);
    if (n < 0 || (size_t)n >= cap - used) {
        return -1;
    }
    used += (size_t)n;
    n = settings_status_json(buf + used, cap - used);
    if (n < 0) {
        return -1;
    }
    used += (size_t)n;

    /* res_list: the sizes this sensor accepts, so the page offers only those. */
    {
        bool first = true;
        n = snprintf(buf + used, cap - used, ",\"res_list\":[");
        if (n < 0 || (size_t)n >= cap - used) {
            return -1;
        }
        used += (size_t)n;
        for (size_t i = 0; i < RES_TABLE_COUNT; i++) {
            if (!cam_sensor_supports(k_res_table[i].res)) {
                continue;
            }
            n = snprintf(buf + used, cap - used, "%s\"%s\"", first ? "" : ",",
                         k_res_table[i].name);
            if (n < 0 || (size_t)n >= cap - used) {
                return -1;
            }
            used += (size_t)n;
            first = false;
        }
    }

    n = snprintf(buf + used, cap - used, "]}");
    if (n < 0 || (size_t)n >= cap - used) {
        return -1;
    }
    return (int)(used + (size_t)n);
}
