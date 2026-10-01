/**
 * @file    detector.h
 * @brief   Turns a stream of Goertzel blocks into two states and the moments
 *          between them.
 *
 * Measured on the bench:
 *
 *   room, nothing happening      tone up to 11.8   rms 0.004 .. 0.006
 *   shouting into the microphone tone up to 15.4   rms up to 0.273
 *   a clap                       tone 4.8          rms 0.067
 *   a steady 2 kHz sine          tone 184 .. 782   rms 0.004 .. 0.008
 *
 * The last two rows are the whole argument. The clap is sixteen times louder
 * than the room and the sine is not louder than the room at all - a level
 * threshold would fire on the thing to ignore and miss the thing to catch.
 * Sorted by tone instead, they are seventy times apart.
 *
 * The threshold goes between 15.4 and 184, with room on both sides.
 *
 * No calibration step is needed. tone_ratio is a peak divided by its own
 * background, so it does not move when the room gets louder or the buzzer gets
 * further away - which is the reason a level-based design has to measure a
 * baseline at boot and this one does not.
 *
 * ---------------------------------------------------------------------------
 *
 * Entry is N-of-M rather than N consecutive, and that is not a detail.
 *
 * The first version required the threshold to be met on every block of a
 * two-second run. That works for a buzzer that holds a continuous note and
 * fails completely for one that beeps - every silent gap resets the count, so
 * two seconds never accumulate and the alarm never fires. Which kind is in the
 * basement is not known, so the detector cannot assume either.
 *
 * Counting hits inside a sliding window handles both: a continuous tone fills
 * it, and a tone that beeps fills it more slowly but still fills it. A single
 * spike still cannot, because one block is far short of the count.
 */

#ifndef __DETECTOR_H__
#define __DETECTOR_H__

#include <stdbool.h>
#include <stdint.h>

#define DET_WINDOW_MAX  256

typedef enum {
    DET_IDLE = 0,       /* watching */
    DET_ALARM           /* a tone has been present long enough to believe */
} det_state_t;

typedef enum {
    DET_EV_NONE = 0,
    DET_EV_ALARM,       /* IDLE -> ALARM, this block */
    DET_EV_CLEAR        /* ALARM -> IDLE, this block */
} det_event_t;

typedef struct {
    /* A block counts as a hit when tone_ratio reaches this. */
    float    enter_ratio;

    /*
     * While in alarm, this lower figure keeps it there. Hysteresis, because a
     * buzzer fading in and out around one threshold would otherwise alternate
     * between states and send an alert on every swing.
     */
    float    hold_ratio;

    /* Sliding window length, and how many hits inside it raise the alarm. */
    uint32_t window_blocks;
    uint32_t enter_hits;

    /* Consecutive blocks with no hit at all before the alarm clears. This has
     * to exceed the longest silent gap the buzzer leaves between beeps. */
    uint32_t clear_blocks;

    /*
     * How far the peak bin may wander and still count as the same tone.
     *
     * Not a strong filter by itself - a sustained shout holds its pitch inside
     * a couple of bins. It earns its place against melody: a ringtone measured
     * on the bench crossed the threshold repeatedly at 2125, 3125, 2375 and
     * 3375 Hz, and refusing to pool hits from different parts of the band is
     * what stopped those from adding up.
     */
    int      bin_tolerance;
} det_config_t;

typedef struct {
    det_config_t cfg;

    det_state_t  state;
    uint32_t     blocks_in_state;

    /* Sliding window of hits, as a ring. */
    uint8_t      win[DET_WINDOW_MAX];
    uint32_t     win_pos;
    uint32_t     win_hits;

    uint32_t     quiet_run;         /* consecutive blocks with no hit */

    int          candidate_bin;     /* bin the window is accumulating on */
    int          locked_bin;        /* bin that raised the alarm */
    float        locked_hz;
    float        peak_ratio;        /* highest ratio seen this alarm */
} detector_t;

/** Defaults from the bench figures above. */
void detector_init(detector_t *d, uint32_t sample_rate, int block);

/** Feed one analysed block. Returns the transition, if any. */
det_event_t detector_update(detector_t *d, float tone_ratio, int peak_bin,
                            float peak_hz);

/** Seconds spent in the current state. */
float detector_state_seconds(const detector_t *d, uint32_t sample_rate,
                             int block);

#endif /* __DETECTOR_H__ */
