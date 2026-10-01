/**
 * @file    goertzel.h
 * @brief   A bank of Goertzel filters, for telling a buzzer from a basement.
 *
 * The problem this solves: a level threshold fires on anything loud. A pump
 * starting, a door, rain, someone talking. In an unattended basement that is a
 * stream of false alarms, which is worse than no alarm at all because the
 * owner stops reading them.
 *
 * A buzzer is not loud, it is *narrow*. Nearly all of its energy sits in one
 * tone, usually somewhere between 2 and 4 kHz. Speech and impacts spread their
 * energy across the whole band. That difference is what gets measured here.
 *
 * Goertzel computes the energy at one frequency for the cost of one multiply
 * and two adds per sample - no FFT, no buffers beyond the block already in
 * hand. A bank of them is just that repeated, so scanning a whole range costs
 * nothing worth counting on an M33.
 *
 * The bank scans rather than targets a single frequency on purpose: the buzzer
 * in the field has not been heard yet. Watch which bin lifts, then the product
 * can run that one bin alone.
 */

#ifndef __GOERTZEL_H__
#define __GOERTZEL_H__

#include <stdint.h>

#define GOERTZEL_MAX_BINS   64

typedef struct {
    int      n_bins;
    int      block;                         /* samples per analysis block     */
    float    bin_hz;                        /* width of one bin               */
    float    freq[GOERTZEL_MAX_BINS];       /* centre frequency of each bin   */
    float    coeff[GOERTZEL_MAX_BINS];      /* 2*cos(2*pi*k/N), precomputed   */
    float    mag[GOERTZEL_MAX_BINS];        /* magnitude of the last block    */

    /* Results for the last block. */
    int      peak_bin;
    float    peak_mag;                      /* 1.0 is a full-scale sine       */
    float    peak_hz;

    /*
     * Peak divided by the mean of the bins that are not the peak or its
     * immediate neighbours.
     *
     * This is the number that separates a buzzer from a room. A pure tone puts
     * everything in one place and lands in the tens or hundreds; speech, a
     * clap, a pump spread out and sit in the low single digits. It is also
     * level-independent, so a quiet buzzer close to the microphone and a loud
     * one across the room read the same - which a level threshold cannot do.
     */
    float    tone_ratio;
    float    mean_other;
} goertzel_bank_t;

/**
 * Lay out contiguous bins covering [f_lo, f_hi].
 *
 * Bin width is sample_rate / block, so at 16 kHz with a 256-sample block each
 * bin is 62.5 Hz and 1.5-4.5 kHz needs 49 of them. Contiguous matters: bins
 * spaced further apart than they are wide would let a buzzer fall in a gap.
 */
void goertzel_bank_init(goertzel_bank_t *b, uint32_t sample_rate,
                        int block, float f_lo, float f_hi);

/**
 * Run one block. @p samples are 24-bit signed, @p stride is the step between
 * consecutive samples of the channel of interest (2 for an interleaved stereo
 * buffer).
 *
 * A Hann window is applied first. Without it the low-frequency energy in
 * speech leaks upward and shows as tonality where there is none.
 */
void goertzel_bank_run(goertzel_bank_t *b, const int32_t *samples, int stride);

/**
 * Render the bank as one line of characters, one per bin, scaled in dB.
 * @p out must hold n_bins + 1 bytes.
 */
void goertzel_bank_render(const goertzel_bank_t *b, char *out);

#endif /* __GOERTZEL_H__ */
