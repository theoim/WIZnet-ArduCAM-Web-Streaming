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

#endif /* CAM_PIPELINE_H */
