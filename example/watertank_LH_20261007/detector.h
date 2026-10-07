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
     * The second path: how many times the room's own level a block has to reach
     * to count as loud.
     *
     * Measured against the real sounder, with three different ways of asking
     * whether anything stood out in the spectrum:
     *
     *            quiet room        the sounder
     *   tone     4 .. 12           3.4 .. 5.5      (lower than the room)
     *   peak/median 5 .. 15        3.6 .. 6.0      (lower than the room)
     *   best 5 bins/median 3 .. 9  2.3 .. 4.4      (lower than the room)
     *   rms      0.004 .. 0.008    0.012 .. 0.016  (two to four times)
     *
     * Every shape measure says the sounder is LESS distinctive than silence,
     * and they are right: it is broadband, shaped like the room, only louder.
     * There is no peak to find because there is no peak.
     *
     * So loudness, which was rejected early on and for a good reason - a clap
     * measured sixteen times the room and had to be ignored. What makes it
     * usable is the dwell: a clap is one or two blocks and a sounder is hundreds.
     * The threshold does not separate them; the duration does.
     *
     * 2.2 sits above the room's own peaks (0.008 against a 0.005 baseline is
     * 1.6) and below the sounder's floor (0.012 is 2.4).
     */
    float    loud_k;

    /* Loud blocks inside the window that raise the alarm on that path. */
    uint32_t loud_hits;

    /*
     * An absolute level, as rms (1.0 = full scale), below which nothing counts
     * on either path - not as loud, not as tonal, not as keeping an alarm up.
     * Zero means none, which is how the detector ran before it existed.
     *
     * The site's room reached a tone ratio of 17.9 against an entry of 20, and
     * its own noise crossed 2.2x the baseline often enough to hold an alarm up.
     * The buzzer is far louder than either. A floor just under it says what no
     * ratio can: nothing quieter than this is the buzzer.
     */
    float    loud_floor;

    /*
     * Whether the tonal path may raise or hold an alarm. Off by default.
     *
     * On site it raised one on a quiet 2 kHz whine at 0.6x the room - tone 27,
     * well over the entry of 20, from something that was not the buzzer at all
     * - and the same whine then held the alarm up for as long as it went on.
     * The site's buzzer is told apart by being loud, so the loud path decides.
     */
    bool     tone_on;

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

    /* The loud path's own window, same length, counted separately. */
    uint8_t      lwin[DET_WINDOW_MAX];
    uint32_t     lwin_pos;
    uint32_t     lwin_hits;

    /*
     * What the room sounds like with nothing happening, as a slow average.
     *
     * Slow on purpose - about thirty seconds - and frozen whenever a block is
     * already over the threshold or an alarm is up. A baseline that followed the
     * sound it is measuring would climb to meet a buzzer and stand the alarm
     * down while the buzzer was still going.
     */
    float        baseline;

    /*
     * The learning period at boot: blocks still to run, and their sum.
     *
     * The slow average only moves on blocks under the threshold, and the
     * threshold is built from the average - so a basement louder than the
     * starting guess makes every block "loud", nothing is ever learned, and the
     * alarm comes up a second after boot. For these few seconds the average
     * takes every block and nothing is judged.
     */
    uint32_t     learn_left;
    double       learn_sum;
    uint32_t     learn_n;

    /* True when the alarm came from the loud path rather than the tonal one.
     * Worth reporting: the two describe different sounds and the one that fired
     * says which. */
    bool         by_loud;

    uint32_t     quiet_run;         /* consecutive blocks with neither hit */

    int          candidate_bin;     /* bin the window is accumulating on */
    int          locked_bin;        /* bin that raised the alarm */
    float        locked_hz;
    float        peak_ratio;        /* highest ratio seen this alarm */
    float        peak_rms;          /* loudest block seen this alarm */
} detector_t;

/** Defaults from the bench figures above. */
void detector_init(detector_t *d, uint32_t sample_rate, int block);

/**
 * Feed one analysed block. Returns the transition, if any.
 *
 * @p rms is the block's level, 1.0 being full scale. It drives the loud path and
 * the baseline; pass it even when only tonal detection is wanted, because the
 * baseline has to keep learning the room either way.
 */
det_event_t detector_update(detector_t *d, float tone_ratio, int peak_bin,
                            float peak_hz, float rms);

/** Learn the room from the next @p blocks blocks, judging nothing meanwhile. */
void detector_learn(detector_t *d, uint32_t blocks);

/** Seconds spent in the current state. */
float detector_state_seconds(const detector_t *d, uint32_t sample_rate,
                             int block);

#endif /* __DETECTOR_H__ */
