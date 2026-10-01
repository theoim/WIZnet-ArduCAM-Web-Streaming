/**
 * @file    goertzel.c
 * @brief   See goertzel.h.
 */

#include <math.h>
#include <string.h>

#include "goertzel.h"

#define SAMPLE_FULL_SCALE   8388608.0f      /* 2^23 - the part is 24-bit */
#define MAX_BLOCK           512

static float s_window[MAX_BLOCK];
static int   s_window_n;

void goertzel_bank_init(goertzel_bank_t *b, uint32_t sample_rate,
                        int block, float f_lo, float f_hi)
{
    memset(b, 0, sizeof(*b));

    if (block > MAX_BLOCK) block = MAX_BLOCK;
    b->block  = block;
    b->bin_hz = (float)sample_rate / (float)block;

    /*
     * Bins are placed on exact Goertzel indices. Any other spacing leaves the
     * filter straddling two integers of k, which costs selectivity for nothing.
     */
    int k_lo = (int)(f_lo / b->bin_hz + 0.5f);
    int k_hi = (int)(f_hi / b->bin_hz + 0.5f);
    if (k_lo < 1) k_lo = 1;
    if (k_hi > block / 2 - 1) k_hi = block / 2 - 1;
    if (k_hi - k_lo + 1 > GOERTZEL_MAX_BINS) k_hi = k_lo + GOERTZEL_MAX_BINS - 1;

    b->n_bins = 0;
    for (int k = k_lo; k <= k_hi; k++) {
        float w = 2.0f * (float)M_PI * (float)k / (float)block;
        b->freq[b->n_bins]  = (float)k * b->bin_hz;
        b->coeff[b->n_bins] = 2.0f * cosf(w);
        b->n_bins++;
    }

    if (s_window_n != block) {
        for (int i = 0; i < block; i++) {
            s_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i /
                                              (float)(block - 1)));
        }
        s_window_n = block;
    }
}

void goertzel_bank_run(goertzel_bank_t *b, const int32_t *samples, int stride)
{
    const int n = b->block;

    /*
     * Window and normalise once, not once per bin. With 49 bins the difference
     * is 49 passes over the block against one.
     *
     * The mean is removed as part of the same pass: the part has a DC offset
     * that drifts by a few tenths of a percent of full scale, and a windowed DC
     * term is a low-frequency component that leaks like any other.
     */
    static float x[MAX_BLOCK];
    float mean = 0.0f;

    for (int i = 0; i < n; i++) {
        x[i] = (float)samples[i * stride] / SAMPLE_FULL_SCALE;
        mean += x[i];
    }
    mean /= (float)n;

    for (int i = 0; i < n; i++) {
        x[i] = (x[i] - mean) * s_window[i];
    }

    /*
     * Hann halves the coherent gain, so a full-scale sine would otherwise read
     * 0.5. Folding 2/n * 2 into one constant puts the magnitude back into the
     * same units as the input: 1.0 means a full-scale tone.
     */
    const float scale = 4.0f / (float)n;

    int   peak_bin = 0;
    float peak_mag = 0.0f;

    for (int bin = 0; bin < b->n_bins; bin++) {
        const float c = b->coeff[bin];
        float s1 = 0.0f, s2 = 0.0f;

        for (int i = 0; i < n; i++) {
            float s = x[i] + c * s1 - s2;
            s2 = s1;
            s1 = s;
        }

        float power = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (power < 0.0f) power = 0.0f;

        float mag = sqrtf(power) * scale;
        b->mag[bin] = mag;

        if (mag > peak_mag) {
            peak_mag = mag;
            peak_bin = bin;
        }
    }

    b->peak_bin = peak_bin;
    b->peak_mag = peak_mag;
    b->peak_hz  = b->freq[peak_bin];

    /*
     * The peak and its two neighbours are excluded because the Hann main lobe
     * is about two bins wide - a single tone genuinely occupies three. Counting
     * them as background would make every tone look less tonal than it is.
     */
    float sum = 0.0f;
    int   cnt = 0;
    for (int bin = 0; bin < b->n_bins; bin++) {
        if (bin >= peak_bin - 1 && bin <= peak_bin + 1) continue;
        sum += b->mag[bin];
        cnt++;
    }

    b->mean_other = cnt ? sum / (float)cnt : 0.0f;
    b->tone_ratio = (b->mean_other > 1e-9f) ? peak_mag / b->mean_other : 0.0f;
}

void goertzel_bank_render(const goertzel_bank_t *b, char *out)
{
    /* Logarithmic, because the interesting range spans three decades: a room
     * floor around 1e-4 of full scale and a buzzer close to 1e-1. */
    static const char levels[] = " .:-=+*#%@";
    const int n_levels = (int)sizeof(levels) - 2;

    for (int i = 0; i < b->n_bins; i++) {
        float m = b->mag[i];
        int   l = 0;

        if (m > 1e-6f) {
            float db = 20.0f * log10f(m);       /* -120 .. 0 dBFS */
            l = (int)((db + 80.0f) / 80.0f * (float)n_levels);
            if (l < 0) l = 0;
            if (l > n_levels) l = n_levels;
        }
        out[i] = levels[l];
    }
    out[b->n_bins] = '\0';
}
