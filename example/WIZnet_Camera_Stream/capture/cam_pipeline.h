/**
 * @file    cam_pipeline.h
 * @brief   Core 1 captures, core 0 sends: two frame buffers between them.
 *
 * Ownership is split by peripheral so the two cores never touch the same one:
 *   core 1  camera - VSYNC wait, PIO + DMA capture, JPEG end walk, and SCCB
 *           while it runs (failure recovery)
 *   core 0  network - W6300, TLS, HTTP, and SCCB only between
 *           cam_pipeline_pause() and cam_pipeline_resume()
 *
 * Latest frame wins: when core 0 is still sending one buffer and core 1
 * finishes the other, the next capture overwrites that unsent frame rather than
 * waiting. Core 0 therefore always sends the newest picture, and the count of
 * overwritten frames says how far the network is behind the sensor.
 */

#ifndef CAM_PIPELINE_H
#define CAM_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "cam_ctrl.h"

/* JPEG up to 1280x720 is 35-37 KB and 800x600 24-28 KB (measured); 96 KB
 * leaves room for busy scenes. 1600x1200 and up (80-100 KB) do not fit and are
 * rejected as truncated - use the sequential build (STREAM_PIPELINE 0) or raise
 * this if those sizes matter. */
#ifndef CAM_FRAME_BUF_SIZE
#define CAM_FRAME_BUF_SIZE   (96 * 1024)
#endif

typedef struct {
    const uint8_t *data;
    uint32_t       len;
    uint32_t       seq;
    int            idx;
} cam_frame_t;

/* Launch the capture loop on core 1. Call once, after the sensor is set up. */
void cam_pipeline_start(void);

/* Core 0: the newest captured frame not yet taken, marked as being sent.
 * false = nothing new; do something else and ask again. */
bool cam_pipeline_take(cam_frame_t *f);
void cam_pipeline_release(const cam_frame_t *f);

/* Core 0: stop core 1 between frames before touching the sensor over SCCB
 * (resolution, clock, reset). Returns once core 1 is idle, or after a timeout.
 * No-ops when called on core 1 itself or before the pipeline started. */
void cam_pipeline_pause(void);
void cam_pipeline_resume(void);

uint32_t cam_pipeline_captured(void);     /* frames core 1 completed        */
uint32_t cam_pipeline_overwritten(void);  /* completed but never sent        */
uint32_t cam_pipeline_take_hits(void);    /* core 0 asked and got a frame    */
uint32_t cam_pipeline_take_misses(void);  /* core 0 asked, nothing new       */
uint32_t cam_pipeline_leaks(void);        /* SENDING buffers reclaimed       */

/* One line describing both buffers and the counters, for the stall log. */
void cam_pipeline_debug(char *out, unsigned cap);

/* ------------------------------- Snapshot ---------------------------------
 *
 * GET /snapshot.jpg sends one frame straight out of a capture buffer - no copy.
 * The buffer is held with a reader count, so core 1 captures into the other
 * one until the snapshot is sent. The streaming flag is not touched.
 *
 *   streaming  the newest frame if it is under CAM_SNAPSHOT_MAX_AGE_MS old,
 *              else wait for the next one
 *   stopped    core 1 captures one frame for it; the stream stays stopped
 *
 * Core 0 waits at most timeout_ms, then gives up (the server answers 504).
 * false also before cam_pipeline_start() and on core 1.
 */
#ifndef CAM_SNAPSHOT_TIMEOUT_MS
#define CAM_SNAPSHOT_TIMEOUT_MS  2500u
#endif
#ifndef CAM_SNAPSHOT_MAX_AGE_MS
#define CAM_SNAPSHOT_MAX_AGE_MS  500u
#endif

/* *fresh (optional): true when the frame was captured after the call. */
bool cam_pipeline_snapshot_hold(cam_frame_t *f, uint32_t timeout_ms, bool *fresh);
void cam_pipeline_snapshot_release(const cam_frame_t *f);

/* ---------------------------- Control commands ----------------------------
 *
 * Sensor controls go through one fixed slot rather than through
 * cam_pipeline_pause(). Pausing waits up to 2.5 s and stops capture; a control
 * write is a few SCCB transactions that core 1 can do between two frames.
 *
 *   core 0  validates the whole batch (cam_ctrl_batch_add), posts it, waits
 *   core 1  picks it up at the top of its loop - between frames, also while
 *           the stream is stopped - writes it, records what was written
 *
 * One command at a time. A second one while the first is still in the slot is
 * refused (BUSY), not queued. Before cam_pipeline_start(), or with
 * STREAM_PIPELINE 0, the batch is written directly on the calling core.
 */

/* How long core 0 waits. One frame is at most ~60 ms (OV2640 800x600); a
 * capture stuck on a dead sensor can hold core 1 for its 2 s VSYNC timeout. */
#ifndef CAM_CMD_TIMEOUT_MS
#define CAM_CMD_TIMEOUT_MS  1000u
#endif

typedef enum {
    CAM_CMD_OK = 0,             /* every item written                          */
    CAM_CMD_FAILED,             /* a write failed: res says which; the items
                                   before it were written, the rest not tried  */
    CAM_CMD_BUSY,               /* the previous command is still in the slot   */
    CAM_CMD_TIMEOUT_NOT_RUN,    /* core 1 never started it: withdrawn, nothing
                                   was written                                 */
    CAM_CMD_TIMEOUT_RUNNING,    /* core 1 started but did not finish in time:
                                   outcome unknown until it does; what it
                                   writes is still recorded                    */
} cam_cmd_rc_t;

const char *cam_cmd_rc_name(cam_cmd_rc_t rc);

/* Core 0. @p res is filled for OK and FAILED only. */
cam_cmd_rc_t cam_pipeline_set_controls(const cam_ctrl_batch_t *b, cam_ctrl_result_t *res,
                                       uint32_t timeout_ms);

/* Last acknowledged write of control @p idx since the sensor was last reset or
 * resized. Not a readback. false = no write since then. */
bool cam_pipeline_control_written(unsigned idx, int16_t *value);

/* The sensor was reset or resized: whether it kept the written values is not
 * known, so stop claiming them. Either core. */
void cam_pipeline_controls_forget(const char *why);

uint32_t cam_pipeline_cmd_applied(void);   /* commands core 1 finished       */
uint32_t cam_pipeline_cmd_timeouts(void);  /* core 0 gave up waiting         */

#endif /* CAM_PIPELINE_H */
