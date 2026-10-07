/**
 * @file    detector.c
 * @brief   See detector.h.
 */

#include <string.h>

#include "detector.h"

void detector_init(detector_t *d, uint32_t sample_rate, int block)
{
    memset(d, 0, sizeof(*d));

    const float bps = (float)sample_rate / (float)block;   /* blocks / second */

    /*
     * 20, revised after hearing an actual buzzer rather than a phone.
     *
     *   quiet room, this site     up to 12.4
     *   shouting                  up to 15.4
     *   a phone playing 2 kHz     184 .. 782
     *   the real buzzer           35 .. 51   <- the one that matters
     *
     * A buzzer is not a sine. It has harmonics, the diaphragm rings and the
     * room adds reverberation, so its energy covers fifteen bins where the
     * phone covered three. That drops the ratio by more than an order of
     * magnitude and 25 turned out to sit inside the gap rather than below it -
     * the first real buzzer did not raise an alarm.
     *
     * 20 clears shouting by 1.3x and sits well under the buzzer. The margin is
     * thinner than the bench suggested, which is what the dwell is for.
     *
     * The field value belongs to the field: it should be set from the buzzer in
     * that basement measured against that basement, not from a bench.
     */
    d->cfg.enter_ratio = 20.0f;
    d->cfg.hold_ratio  = 12.0f;

    /*
     * Three seconds of window, 0.6 of a second of it tonal.
     *
     * One second was the first guess and it was wrong, in a way only the real
     * buzzer could show. Within a single 250 ms slice the ratio ran from 31
     * down to 2.3 - below the room's own floor - which means the buzzer is not
     * holding a note, it is pulsing fast. About a third of blocks cross the
     * threshold while it sounds.
     *
     * At that duty a three-second window reaches sixty hits, and the
     * requirement was sixty-two. It missed by two. Lowering the threshold would
     * not have helped: the blocks it misses are the gaps between pulses, which
     * are genuinely silent and sit below the room noise.
     *
     * 0.6 s of hits reaches the mark in under two seconds at the measured duty.
     * Nothing is given away by it - neither the room nor shouting crosses the
     * threshold even once, so both still accumulate exactly zero.
     */
    d->cfg.window_blocks = (uint32_t)(bps * 3.0f);
    if (d->cfg.window_blocks > DET_WINDOW_MAX) d->cfg.window_blocks = DET_WINDOW_MAX;
    /*
     * 0.2 s of tonal blocks inside the 3 s window, down from 0.6 s.
     *
     * Measured against the real buzzer, in the room, with the finished
     * firmware: the peak ratio reached 37 while the window never collected more
     * than 14 hits of the 37 it needed. The buzzer is only tonal for a fraction
     * of the time it is sounding - it pulses, and between pulses the block is
     * genuinely silence, not a quiet tone. Asking for 60 % of three seconds asks
     * for something the sound never contains.
     *
     * Lowering the ratio instead would not have worked. The room in that
     * recording reached 17.9 on its own against a threshold of 20, so there is
     * no room underneath to give away. What there is room in is the count.
     */
    d->cfg.enter_hits = (uint32_t)(bps * 0.2f);

    /*
     * Five seconds of complete silence to clear.
     *
     * This has to be longer than the longest gap between beeps or the alarm
     * drops out mid-event and re-fires, sending an alert per beep. Five covers
     * anything that still reads as one buzzer. The product should use nearer
     * thirty; five is here because waiting half a minute to see the state
     * machine work makes it tedious to test.
     */
    d->cfg.clear_blocks = (uint32_t)(bps * 5.0f);

    d->cfg.bin_tolerance = 2;

    /*
     * The loud path. 2.2x the room, for a full second's worth of blocks inside
     * the same three-second window.
     *
     * A second is the whole defence against claps and doors. Those are one or two
     * blocks; 62 of them inside three seconds is a sound that is still going. It
     * is deliberately a longer dwell than the tonal path's 0.2 s, because a level
     * threshold is a blunter instrument and has to buy its confidence with time
     * instead.
     */
    d->cfg.loud_k    = 2.2f;
    d->cfg.loud_hits = (uint32_t)(bps * 1.0f);

    /*
     * A starting guess for the room, from the bench: quiet reads 0.004 to 0.008.
     * It is only a start - thirty seconds of silence replaces it with whatever
     * the basement actually sounds like, which is the point of learning it rather
     * than defining it.
     */
    d->baseline = 0.005f;

    d->state         = DET_IDLE;
    d->candidate_bin = -1;
    d->locked_bin    = -1;
}

/* Push one block into the ring and keep the running count correct. */
static void window_push(detector_t *d, uint8_t hit)
{
    uint8_t *slot = &d->win[d->win_pos];

    if (*slot) d->win_hits--;
    *slot = hit;
    if (hit) d->win_hits++;

    d->win_pos++;
    if (d->win_pos >= d->cfg.window_blocks) d->win_pos = 0;
}

static void window_clear(detector_t *d)
{
    memset(d->win, 0, sizeof(d->win));
    d->win_pos  = 0;
    d->win_hits = 0;

    memset(d->lwin, 0, sizeof(d->lwin));
    d->lwin_pos  = 0;
    d->lwin_hits = 0;
}

/* Same ring, the loud path's copy. */
static void lwindow_push(detector_t *d, uint8_t hit)
{
    uint8_t *slot = &d->lwin[d->lwin_pos];

    if (*slot) d->lwin_hits--;
    *slot = hit;
    if (hit) d->lwin_hits++;

    d->lwin_pos++;
    if (d->lwin_pos >= d->cfg.window_blocks) d->lwin_pos = 0;
}

void detector_learn(detector_t *d, uint32_t blocks)
{
    d->learn_left = blocks;
    d->learn_sum  = 0.0;
    d->learn_n    = 0;
}

det_event_t detector_update(detector_t *d, float tone_ratio, int peak_bin,
                            float peak_hz, float rms)
{
    d->blocks_in_state++;

    /* Learning: every block into a plain mean, nothing judged. */
    if (d->learn_left) {
        d->learn_sum += rms;
        d->learn_n++;
        if (--d->learn_left == 0) {
            float b = (float)(d->learn_sum / (double)d->learn_n);
            d->baseline = (b > 0.002f) ? b : 0.002f;
        }
        return DET_EV_NONE;
    }

    /*
     * The loud path, first, because the baseline has to be updated from this
     * block before the block is judged against it.
     *
     * The floor on the baseline is not cosmetic: a microphone in a genuinely
     * silent room reads near zero, and a threshold of 2.2 times near zero fires
     * on the next moth. 0.002 is below anything measured in a quiet room and
     * above the part's own noise.
     */
    float loud_thr = d->baseline * d->cfg.loud_k;
    if (loud_thr < d->cfg.loud_floor) loud_thr = d->cfg.loud_floor;
    const uint8_t lhit = (rms >= loud_thr) ? 1 : 0;

    /* Below the floor nothing counts on the tonal path either. */
    const bool audible = (rms >= d->cfg.loud_floor);

    if (!lhit && d->state == DET_IDLE) {
        /* About a thirty-second time constant at 62 blocks a second. */
        d->baseline += (rms - d->baseline) * 0.0005f;
        if (d->baseline < 0.002f) d->baseline = 0.002f;
    }

    const float threshold = (d->state == DET_ALARM) ? d->cfg.hold_ratio
                                                    : d->cfg.enter_ratio;
    const int   anchor    = (d->state == DET_ALARM) ? d->locked_bin
                                                    : d->candidate_bin;
    uint8_t hit = 0;

    if (d->cfg.tone_on && audible && tone_ratio >= threshold) {
        if (anchor < 0) {
            /* Nothing being tracked: this block starts it. */
            if (d->state == DET_IDLE) d->candidate_bin = peak_bin;
            hit = 1;
        } else {
            int delta = peak_bin - anchor;
            if (delta < 0) delta = -delta;

            if (delta <= d->cfg.bin_tolerance) {
                hit = 1;
            } else if (d->state == DET_IDLE && d->win_hits == 0) {
                /*
                 * A different frequency, and nothing left of the old one. Move
                 * rather than stay anchored to something that has gone.
                 *
                 * The window must be empty for this. Letting the anchor follow
                 * the loudest block would let a melody hand its count from note
                 * to note, which is exactly the case this is meant to reject.
                 */
                d->candidate_bin = peak_bin;
                hit = 1;
            }
        }
    }

    window_push(d, hit);
    lwindow_push(d, lhit);

    /* Either path keeps the alarm alive. Standing down needs both to be quiet -
     * a sounder that is broadband while it rings and tonal as it dies would
     * otherwise clear in the middle of itself. */
    d->quiet_run = (hit || lhit) ? 0 : d->quiet_run + 1;

    if (d->state == DET_IDLE) {
        bool tonal = (d->win_hits  >= d->cfg.enter_hits);
        bool loud  = (d->lwin_hits >= d->cfg.loud_hits);

        if (tonal || loud) {
            d->state           = DET_ALARM;
            d->blocks_in_state = 0;
            d->by_loud         = !tonal;
            d->locked_bin      = tonal ? d->candidate_bin : peak_bin;
            d->locked_hz       = peak_hz;
            d->peak_ratio      = tone_ratio;
            d->peak_rms        = rms;
            d->quiet_run       = 0;
            window_clear(d);
            return DET_EV_ALARM;
        }
        return DET_EV_NONE;
    }

    /* DET_ALARM */
    if (tone_ratio > d->peak_ratio) d->peak_ratio = tone_ratio;
    if (rms > d->peak_rms) d->peak_rms = rms;

    if (d->quiet_run >= d->cfg.clear_blocks) {
        d->state           = DET_IDLE;
        d->blocks_in_state = 0;
        d->candidate_bin   = -1;
        d->locked_bin      = -1;
        d->quiet_run       = 0;
        window_clear(d);
        return DET_EV_CLEAR;
    }

    return DET_EV_NONE;
}

float detector_state_seconds(const detector_t *d, uint32_t sample_rate,
                             int block)
{
    return (float)d->blocks_in_state * (float)block / (float)sample_rate;
}
