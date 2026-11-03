#include "effects.h"
#include "dsp_common.h"

#include <math.h>
#include <string.h>

/* Four-lane vectors (GCC/Clang vector extensions). Lane arithmetic is plain
 * IEEE single precision, the same as the scalar code it replaces. */
typedef float sf_v4f __attribute__((vector_size(16)));
typedef unsigned sf_v4u __attribute__((vector_size(16)));
typedef int sf_v4i __attribute__((vector_size(16)));
typedef float sf_v2f __attribute__((vector_size(8)));
typedef int sf_v2i __attribute__((vector_size(8)));

static inline sf_v4f v4f_load(const float *p) { sf_v4f v; memcpy(&v, p, sizeof(v)); return v; }
static inline sf_v4u v4u_load(const unsigned *p) { sf_v4u v; memcpy(&v, p, sizeof(v)); return v; }
static inline void v4f_store(float *p, sf_v4f v) { memcpy(p, &v, sizeof(v)); }
static inline void v4u_store(unsigned *p, sf_v4u v) { memcpy(p, &v, sizeof(v)); }
static inline int v4i_all(sf_v4i m) { return (m[0] & m[1] & m[2] & m[3]) != 0; }
static inline sf_v4f v4f_select(sf_v4i m, sf_v4f a, sf_v4f b) {
    return (sf_v4f)((m & (sf_v4i)a) | (~m & (sf_v4i)b));
}
static inline sf_v2f v2f_select(sf_v2i m, sf_v2f a, sf_v2f b) {
    return (sf_v2f)((m & (sf_v2i)a) | (~m & (sf_v2i)b));
}
static inline sf_v2f v2f_clamp(sf_v2f x, float lo, float hi) {
    sf_v2f vlo = {lo, lo}, vhi = {hi, hi}, vinf = {INFINITY, INFINITY};
    sf_v2f above = v2f_select((sf_v2i)(x == vinf), vlo, vhi);
    sf_v2f rest = v2f_select((sf_v2i)(x >= vlo), x, vlo);
    return v2f_select((sf_v2i)(x > vhi), above, rest);
}
/* sf_clampf on every lane, NaN and +inf included. */
static inline sf_v4f v4f_clamp(sf_v4f x, float lo, float hi) {
    sf_v4f vlo = {lo, lo, lo, lo}, vhi = {hi, hi, hi, hi};
    sf_v4f vinf = {INFINITY, INFINITY, INFINITY, INFINITY};
    sf_v4f above = v4f_select((sf_v4i)(x == vinf), vlo, vhi);
    sf_v4f rest = v4f_select((sf_v4i)(x >= vlo), x, vlo);
    return v4f_select((sf_v4i)(x > vhi), above, rest);
}

/* ---- Knob curves ------------------------------------------------------------
 *
 * Every effect except Delay reads Time and Intensity as the MiniFreak's 0..1
 * knob positions: Time is (time_ms - 1) / (SF_FX_TIME_MAX - 1) and Intensity
 * is feedback / 0.95. The curves below turn those positions into physical
 * values and were measured from MiniFreak V renders, one table per effect.
 * Interpolation is linear in the knob and, for the `log` tables, geometric in
 * the value, which is how rates and decay times move on the hardware. */
static float table_lerp(const float *xs, const float *ys, int n, float x) {
    if (x <= xs[0]) return ys[0];
    for (int i = 1; i < n; ++i) {
        if (x <= xs[i]) {
            float f = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
            return ys[i - 1] + f * (ys[i] - ys[i - 1]);
        }
    }
    return ys[n - 1];
}

static float table_log(const float *xs, const float *ys, int n, float x) {
    if (x <= xs[0]) return ys[0];
    for (int i = 1; i < n; ++i) {
        if (x <= xs[i]) {
            float f = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
            return ys[i - 1] * powf(ys[i] / ys[i - 1], f);
        }
    }
    return ys[n - 1];
}

/* table_lerp read backwards: the knob position whose value is y. ys rises. */
static float table_unlerp(const float *xs, const float *ys, int n, float y) {
    if (y <= ys[0]) return xs[0];
    for (int i = 1; i < n; ++i) {
        if (y <= ys[i]) {
            float f = (y - ys[i - 1]) / (ys[i] - ys[i - 1]);
            return xs[i - 1] + f * (xs[i] - xs[i - 1]);
        }
    }
    return xs[n - 1];
}

/* sin(2 pi phase) for any phase, without a libm call: folded onto a quarter
 * cycle, the 9th-order series is within 4e-6 of sinf (engines.c uses the
 * same). The modulated delays and the reverb call this every sample. */
static inline __attribute__((always_inline)) float fx_sin_cycle(float phase) {
    float x = phase - floorf(phase);
    x = x >= 0.5f ? x - 1.0f : x;
    /* Past a quarter cycle either way, reflect about +/-0.25: 0.5 - x or
     * -0.5 - x. Written without branches so the copies' LFOs vectorise. */
    x = fabsf(x) > 0.25f ? copysignf(0.5f, x) - x : x;
    float t = SF_TWO_PI * x;
    float t2 = t * t;
    return t * (1.0f + t2 * (-1.6666667e-1f + t2 * (8.3333333e-3f
             + t2 * (-1.9841270e-4f + t2 * 2.7557319e-6f))));
}

/* tan(x) for 0 <= x <= 0.6 (the phaser's corner, up to 8 kHz): the [5/4] Pade
 * form is within 1e-7 of tanf there, with one divide. */
static float fx_tan_small(float x) {
    float x2 = x * x;
    return x * (945.0f - x2 * (105.0f - x2)) / (945.0f - x2 * (420.0f - 15.0f * x2));
}

/* An effect rate from its Time knob, cached per slot: the knob curves cost a
 * powf, and Time only moves with a knob or a route. */
enum { RATE_CHORUS, RATE_FLANGER, RATE_PHASER };
static float knob_rate(sf_effect_state_t *state, int slot, int table, float time_knob);

/* ---- Modulation routes ------------------------------------------------------
 *
 * Measured on MiniFreak V, a route into an effect's Time, Intensity or Amount
 * adds to that knob's 0..1 position and stops at its ends, like every other
 * knob destination. Most effects already store the knob position. The
 * free-running Delay stores milliseconds and Delay/Reverb store a mix that the
 * importer converted from the Send-routing Amount, so those go through their
 * knob law and back. */
static const float g_delay_ms_x[14] = {0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.3f, 0.4f,
                                       0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 0.95f, 1.0f};
static const float g_delay_ms[14] = {2.0f, 10.45f, 20.27f, 31.7f, 45.0f, 78.3f, 123.3f,
                                     184.1f, 266.0f, 376.9f, 526.1f, 728.1f, 854.0f, 1000.0f};
/* Send-routing wet level against Amount. */
static const float g_delay_send_wet[5] = {0.0f, 0.0776f, 0.221f, 0.49f, 1.0f};
static const float g_reverb_send_wet[5] = {0.0f, 0.202f, 0.285f, 0.519f, 1.0f};
static const float g_knob5[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};

float sf_effect_mod_time(int type, int subtype, float time_ms, float offset) {
    if (offset == 0.0f) return time_ms;
    if (type == FX_DELAY && subtype < 7) {
        float knob = table_unlerp(g_delay_ms_x, g_delay_ms, 14, time_ms);
        return table_lerp(g_delay_ms_x, g_delay_ms, 14, sf_clampf(knob + offset, 0.0f, 1.0f));
    }
    float knob = (time_ms - 1.0f) / (SF_FX_TIME_MAX - 1.0f);
    return 1.0f + sf_clampf(knob + offset, 0.0f, 1.0f) * (SF_FX_TIME_MAX - 1.0f);
}

/* Peak EQ frequency (Hz) against Time, geometric between points. */
static const float g_peak_x[6] = {0.0f, 0.125f, 0.375f, 0.625f, 0.875f, 1.0f};
static const float g_peak_hz[6] = {32.0f, 54.0f, 205.0f, 1098.0f, 6255.0f, 15019.0f};

/* table_log read backwards. ys rises. */
static float table_unlog(const float *xs, const float *ys, int n, float y) {
    if (y <= ys[0]) return xs[0];
    for (int i = 1; i < n; ++i) {
        if (y <= ys[i]) {
            float f = logf(y / ys[i - 1]) / logf(ys[i] / ys[i - 1]);
            return xs[i - 1] + f * (xs[i] - xs[i - 1]);
        }
    }
    return xs[n - 1];
}

void sf_effect_mod_eq(int type, const float route[3], float eq_db[3],
                      float *peak_hz, float *peak_gain, float *peak_width) {
    if (type == FX_EQ) {
        /* Time, Intensity and Amount are the low, mid and high gains, each
         * -15..+6 dB across the knob. */
        for (int band = 0; band < 3; ++band)
            eq_db[band] = sf_clampf(eq_db[band] + route[band] * 21.0f, -15.0f, 6.0f);
    } else if (type == FX_PEAK_EQ) {
        if (route[0] != 0.0f) {
            float knob = table_unlog(g_peak_x, g_peak_hz, 6, *peak_hz);
            *peak_hz = table_log(g_peak_x, g_peak_hz, 6,
                                 sf_clampf(knob + route[0], 0.0f, 1.0f));
        }
        *peak_gain = sf_clampf(*peak_gain + route[1] * 21.0f, -15.0f, 6.0f);
        if (route[2] != 0.0f) {
            /* Width is 1.1 * 118^(knob - 0.5). */
            float knob = 0.5f + logf(*peak_width / 1.1f) / logf(118.0f);
            knob = sf_clampf(knob + route[2], 0.0f, 1.0f);
            *peak_width = sf_clampf(1.1f * powf(118.0f, knob - 0.5f), 0.1f, 10.0f);
        }
    }
}

float sf_effect_mod_mix(int type, float mix, float offset) {
    if (offset == 0.0f) return mix;
    /* The EQs' Amount knob is a band gain or width (sf_effect_mod_eq). */
    if (type == FX_EQ || type == FX_PEAK_EQ) return mix;
    if ((type == FX_DELAY || type == FX_REVERB) && mix <= 0.5f) {
        /* mix = sqrt(wet) / 2 (see mix_law), wet from the Send Amount table. */
        const float *wet_table = type == FX_DELAY ? g_delay_send_wet : g_reverb_send_wet;
        float amount = table_unlerp(g_knob5, wet_table, 5, 4.0f * mix * mix);
        float wet = table_lerp(g_knob5, wet_table, 5, sf_clampf(amount + offset, 0.0f, 1.0f));
        return 0.5f * sqrtf(wet);
    }
    return sf_clampf(mix + offset, 0.0f, 1.0f);
}
/* LFO rates in Hz against Time. */
static const float g_chorus_rate[5] = {0.09f, 0.91f, 1.95f, 3.29f, 5.0f};
static const float g_flanger_rate[5] = {0.10f, 0.59f, 1.83f, 4.45f, 10.0f};
static const float g_phaser_rate[5] = {0.10f, 0.185f, 0.567f, 2.29f, 10.0f};
/* Phaser feedback against Intensity. */
static const float g_phaser_fb_x[6] = {0.0f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f};
static const float g_phaser_fb[6] = {0.0f, 0.20f, 0.35f, 0.50f, 0.75f, 0.96f};
/* Reverb decay (T60 of the low end, seconds) against Time. Intensity is a
 * damping filter in the loop: it takes a fixed amount off 4 kHz on every
 * 35 ms pass, whatever the decay, so long tails darken more than short ones.
 * Only the top few percent of Intensity also shorten the low end. */
static const float g_reverb_t60[5] = {0.91f, 1.92f, 3.48f, 6.89f, 17.8f};
static const float g_reverb_int_x[6] = {0.0f, 0.5f, 0.75f, 0.9f, 0.97f, 1.0f};
static const float g_reverb_int_t60[6] = {1.0f, 1.0f, 1.0f, 0.93f, 0.74f, 0.17f};
static const float g_reverb_int_hf_db[6] = {0.0f, 0.23f, 0.68f, 1.3f, 3.27f, 3.5f};
#define SF_REVERB_PASS_SAMPLES 1520.0f
/* Distortion drive (small-signal gain, dB) against Time. */
static const float g_drive_x[7] = {0.0f, 0.25f, 0.4f, 0.5f, 0.6f, 0.75f, 1.0f};
#ifndef SF_DRIVE_MAX_DB
#define SF_DRIVE_MAX_DB 20.0f
#endif
static const float g_drive_db[7] = {0.2f, 0.7f, 2.2f, 4.5f, 7.7f, 13.7f, SF_DRIVE_MAX_DB};
/* Bit Crusher downsampling ratio (log2) against Time. */
static const float g_crush_x[11] = {0.0f, 0.1f, 0.2f, 0.25f, 0.3f, 0.4f, 0.5f, 0.6f, 0.75f, 0.9f, 1.0f};
static const float g_crush_log2[11] = {0.0f, 1.29f, 2.14f, 2.49f, 2.82f, 3.41f, 3.95f, 4.45f, 5.18f, 5.87f, 6.32f};

/* The Distortion's clip ceiling and the Bit Crusher's bits-per-Intensity
 * slope set how hard a typical voice level drives them; both were matched
 * against MiniFreak V renders of the same patch. */
#ifndef SF_DRIVE_CEILING
#define SF_DRIVE_CEILING 0.15f
#endif
#ifndef SF_CRUSH_MIN_BITS
#define SF_CRUSH_MIN_BITS 6.0f
#endif
#ifndef SF_CRUSH_BITS_SLOPE
#define SF_CRUSH_BITS_SLOPE 40.0f
#endif
/* The Delay's repeats come back 0.9 dB below the input. */
#define SF_DELAY_WET 0.9f
/* Reverb and Multi Comp level calibration, see their sections. */
#ifndef SF_REVERB_WET
#define SF_REVERB_WET 1.9f
#endif
#ifndef SF_REVERB_T60_TILT
#define SF_REVERB_T60_TILT 1.5f
#endif
#ifndef SF_COMP_SPLIT_LOW
#define SF_COMP_SPLIT_LOW 190.0f
#endif
#ifndef SF_COMP_SPLIT_HIGH
#define SF_COMP_SPLIT_HIGH 2500.0f
#endif
#ifndef SF_COMP_OFFSET_LOW
#define SF_COMP_OFFSET_LOW 0.0f
#endif
#ifndef SF_COMP_OFFSET_HIGH
#define SF_COMP_OFFSET_HIGH -8.0f
#endif
#ifndef SF_COMP_LEVEL_OFFSET_DB
#define SF_COMP_LEVEL_OFFSET_DB 0.0f
#endif

/* Delay subdivisions used by the Sync presets, in the MiniFreak's order: each
 * straight value is preceded by its triplet and followed by the dotted value
 * of the next shorter note, and the Time knob steps through them in
 * sixteenths (measured on MiniFreak V). The value is in beats, so a quarter
 * note is 1.0 and the DSP only needs the current tempo. Keep this list in step
 * with the Delay division names in web_ui.html. */
typedef struct {
    const char *name;
    float beats;
} sf_delay_div_t;

static const sf_delay_div_t g_delay_divs[] = {
    {"1/32T",  1.0f / 12.0f},
    {"1/32",   0.125f},
    {"1/16T",  1.0f / 6.0f},
    {"1/32D",  0.1875f},
    {"1/16",   0.25f},
    {"1/8T",   1.0f / 3.0f},
    {"1/16D",  0.375f},
    {"1/8",    0.5f},
    {"1/4T",   2.0f / 3.0f},
    {"1/8D",   0.75f},
    {"1/4",    1.0f},
    {"1/2T",   4.0f / 3.0f},
    {"1/4D",   1.5f},
    {"1/2",    2.0f},
    {"1/1T",   8.0f / 3.0f},
    {"1/2D",   3.0f},
    {"1/1",    4.0f}
};
#define SF_DELAY_DIV_COUNT ((int)(sizeof(g_delay_divs) / sizeof(g_delay_divs[0])))

/* Read a fractional delay from the ring buffer. `delay_samples` is measured
 * from the current write cursor. Linear interpolation is smooth enough for the
 * chorus/flanger/unison uses; Tape delay asks for the four-point Hermite form
 * so time modulation does not add quantisation noise. */
/* Callers keep 1 <= delay_samples <= SF_DELAY_SIZE - 4 (every caller's delay
 * is clamped in ms or samples before it gets here).
 *
 * The delay's whole samples come off the cursor in integer arithmetic and only
 * its fraction stays in float. Reading at (float)pos - delay lost precision
 * once pos passed 65536 (a 1/128-sample grid, so taps jittered with the
 * cursor), and a position just below 0 wrapped up to exactly SF_DELAY_SIZE,
 * one past the end of the ring, reading whatever followed it. */
static inline __attribute__((always_inline)) float effect_read_delay(
        const float *buf, unsigned pos, float delay_samples, int hermite) {
    unsigned whole = (unsigned)delay_samples;
    float part = delay_samples - (float)whole;   /* exact */
    /* pos - delay lies frac of the way from i0 to i0 + 1. */
    unsigned back = whole + (part > 0.0f ? 1u : 0u);
    float frac = part > 0.0f ? 1.0f - part : 0.0f;
    unsigned i0 = (pos - back) & (SF_DELAY_SIZE - 1u);
    unsigned i1 = (i0 + 1u) & (SF_DELAY_SIZE - 1u);
    if (!hermite) return buf[i0] * (1.0f - frac) + buf[i1] * frac;
    unsigned im1 = (i0 + (unsigned)SF_DELAY_SIZE - 1u) & (SF_DELAY_SIZE - 1u);
    unsigned i2 = (i1 + 1u) & (SF_DELAY_SIZE - 1u);
    float xm1 = buf[im1];
    float x0 = buf[i0];
    float x1 = buf[i1];
    float x2 = buf[i2];
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * frac + c2) * frac + c1) * frac + x0;
}

/* A triangle wavefolder with a fold period of four, so an input of 0 passes
 * through and +/-2 folds back to 0. */
static float effect_fold(float x) {
    /* x - 4 floor(x / 4) is fmodf plus the wrap of a negative remainder,
     * with the same rounding: 4 floor(x / 4) is exact, so the difference is
     * rounded once, as fmodf's exact remainder + 4 was. */
    x += 1.0f;
    x = x - 4.0f * floorf(x * 0.25f);
    return x < 2.0f ? x - 1.0f : 3.0f - x;
}

/* The six MiniFreak Distortion presets. They deliberately react to gain in
 * different ways instead of all sharing one tanh curve. Each maps a signal
 * already scaled to the clip ceiling (1.0 = ceiling). */
/* Always inlined: as a call (twice a sample) it costs more than its switch. */
__attribute__((always_inline)) static inline float effect_distort(float x, int preset) {
    switch (preset) {
    case 0: /* Classic: tanh; at full drive a sine becomes a square */
        return tanhf(x);
    case 1: /* Soft Clip: a slower knee than Classic */
        return x / (1.0f + fabsf(x));
    case 2: /* Germanium: asymmetric, a little even-harmonic grit */
        return x >= 0.0f ? tanhf(x * 1.18f)
                         : tanhf(x * 0.82f) + 0.04f * x * x;
    case 3: /* Dual Fold */
        return effect_fold(x * 0.75f);
    case 4: /* Climb: cubic soft knee with a slower approach to the ceiling */
        return x / cbrtf(1.0f + fabsf(x) * fabsf(x) * fabsf(x));
    default: /* Tape: tanh plus a touch of linear signal for low-level warmth */
        return tanhf(x * 0.9f) + 0.08f * x;
    }
}

/* ---- Biquads ---------------------------------------------------------------- */

static float biquad_run(const sf_biquad_t *c, float z[2], float x) {
    /* Transposed direct form II. */
    float y = c->b0 * x + z[0];
    z[0] = c->b1 * x - c->a1 * y + z[1];
    z[1] = c->b2 * x - c->a2 * y;
    return y;
}

static void biquad_set(sf_biquad_t *c, float b0, float b1, float b2,
                       float a0, float a1, float a2) {
    c->b0 = b0 / a0;
    c->b1 = b1 / a0;
    c->b2 = b2 / a0;
    c->a1 = a1 / a0;
    c->a2 = a2 / a0;
}

/* RBJ cookbook shapes. kind 0 low shelf, 1 bell, 2 high shelf. */
static void biquad_eq(sf_biquad_t *c, int kind, float freq, float gain_db, float q) {
    float a = powf(10.0f, gain_db / 40.0f);
    float w = SF_TWO_PI * freq / SF_SAMPLE_RATE;
    float cw = cosf(w);
    float alpha = sinf(w) / (2.0f * q);
    if (kind == 1) {
        biquad_set(c, 1.0f + alpha * a, -2.0f * cw, 1.0f - alpha * a,
                   1.0f + alpha / a, -2.0f * cw, 1.0f - alpha / a);
        return;
    }
    float sq = 2.0f * sqrtf(a) * alpha;
    if (kind == 0) {
        biquad_set(c, a * ((a + 1.0f) - (a - 1.0f) * cw + sq),
                   2.0f * a * ((a - 1.0f) - (a + 1.0f) * cw),
                   a * ((a + 1.0f) - (a - 1.0f) * cw - sq),
                   (a + 1.0f) + (a - 1.0f) * cw + sq,
                   -2.0f * ((a - 1.0f) + (a + 1.0f) * cw),
                   (a + 1.0f) + (a - 1.0f) * cw - sq);
    } else {
        biquad_set(c, a * ((a + 1.0f) + (a - 1.0f) * cw + sq),
                   -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cw),
                   a * ((a + 1.0f) + (a - 1.0f) * cw - sq),
                   (a + 1.0f) - (a - 1.0f) * cw + sq,
                   2.0f * ((a - 1.0f) - (a + 1.0f) * cw),
                   (a + 1.0f) - (a - 1.0f) * cw - sq);
    }
}

/* Butterworth low (highpass == 0) or high pass. */
static void biquad_pass(sf_biquad_t *c, int highpass, float freq) {
    float w = SF_TWO_PI * sf_clampf(freq, 10.0f, 20000.0f) / SF_SAMPLE_RATE;
    float cw = cosf(w);
    float alpha = sinf(w) / (2.0f * 0.7071f);
    if (highpass)
        biquad_set(c, (1.0f + cw) * 0.5f, -(1.0f + cw), (1.0f + cw) * 0.5f,
                   1.0f + alpha, -2.0f * cw, 1.0f - alpha);
    else
        biquad_set(c, (1.0f - cw) * 0.5f, 1.0f - cw, (1.0f - cw) * 0.5f,
                   1.0f + alpha, -2.0f * cw, 1.0f - alpha);
}

/* ---- Reverb network layout -------------------------------------------------
 *
 * The slot's existing delay buffer holds the whole reverb. Each stage owns a
 * reserved span wide enough for the right channel's extra 23 samples, and the
 * spans are contiguous so the region clear can walk one cursor:
 *
 *   comb 0  0..1579      (L 1557, R 1580)
 *   comb 1  1580..3219   (L 1617, R 1640)
 *   comb 2  3220..4733   (L 1491, R 1514)
 *   comb 3  4734..6178   (L 1422, R 1445)
 *   ap 0    6179..6426   (L 225,  R 248)
 *   ap 1    6427..7005   (L 556,  R 579)
 *   ap 2    7006..7469   (L 441,  R 464)
 *   ap 3    7470..7833   (L 341,  R 364)
 *   pre     7834..11929  (pre-delay ring, both channels)
 *
 * 11930 of the 131072 samples are used, so the same buffer is still shared with
 * every other effect type (the larger size is for tempo-synced Delay). */
static const unsigned rev_comb_len[SF_REV_COMBS] = {1557u, 1617u, 1491u, 1422u};
/* Per tap (lane 2 * line + channel): the line's base, and the all-pass
 * lengths with the right channel's spread. */
static const unsigned rev_comb_lane_base[2 * SF_REV_COMBS] = {
    0u, 0u, 1580u, 1580u, 3220u, 3220u, 4734u, 4734u};
static const unsigned rev_ap_lane_len[2 * SF_REV_APS] = {
    225u, 225u + 23u, 556u, 556u + 23u, 441u, 441u + 23u, 341u, 341u + 23u};
static const unsigned rev_ap_base[SF_REV_APS] = {6179u, 6427u, 7006u, 7470u};
#define SF_REV_STEREO_SPREAD 23u
#define SF_REV_PRE_BASE 7834u
#define SF_REV_PRE_SIZE 4096u
#define SF_REV_REGION_END (SF_REV_PRE_BASE + SF_REV_PRE_SIZE)
/* Samples of the region cleared per sample while the reverb is coming up.
 * 11930 over 256 is 47 samples (about 1.1 ms at 44.1 kHz), and the slot stays
 * dry until it finishes so nothing stale is ever audible. */
#define SF_REV_CLEAR_CHUNK 256u
/* How far past a line's cursor its Hermite tap reaches, rounded up: the tap
 * sits at idx + 1.5 + mod_depth + mod with |mod| <= mod_depth <= 2, and the
 * interpolator reads two samples beyond it. */
#define SF_REV_TAP_REACH 8u

/* Reverb presets: Default, Long, Hall, Echoes, Room, Dark Room. The decay
 * multipliers are the MiniFreak's, measured against Default at the same Time
 * and Intensity. wet_db is fitted per preset at noon Time against MiniFreak V
 * renders of a short burst (wet energy per band and window, not a deconvolved
 * IR, which overstated the MiniFreak's wet by 5-10 dB); reverb_update then
 * tilts it with the decay. The wet arrives after the preset's pre-delay and
 * builds up over the first pass through the 35 ms lines; the early (diffused
 * input) share only softens that step. */
typedef struct {
    float t60_scale;
    float wet_db;
    float line_scale;
    float diffusion;
    float mod_depth;
    float hf_scale;
    float early_gain;
    float predelay_ms;
} sf_reverb_preset_t;

static const sf_reverb_preset_t g_reverb_presets[6] = {
    {1.00f,  -5.7f, 1.00f, 0.55f, 1.2f, 1.00f, 0.05f, 30.0f}, /* Default */
    {1.70f,  -9.3f, 1.00f, 0.60f, 1.5f, 1.00f, 0.20f, 90.0f}, /* Long */
    {1.20f, -11.5f, 1.00f, 0.68f, 1.9f, 1.00f, 0.05f, 30.0f}, /* Hall */
    {0.18f,   6.3f, 0.82f, 0.25f, 0.4f, 1.00f, 0.45f, 50.0f}, /* Echoes */
    {0.27f,  -8.1f, 0.62f, 0.48f, 0.8f, 1.00f, 0.20f, 10.0f}, /* Room */
    {0.28f,   2.1f, 0.62f, 0.42f, 0.6f, 1.50f, 0.20f, 10.0f}  /* Dark Room */
};

static unsigned reverb_line_len(int comb, float line_scale, int ch) {
    unsigned len = (unsigned)((float)rev_comb_len[comb] * line_scale);
    if (len < 8u) len = 8u;
    return len + (ch ? SF_REV_STEREO_SPREAD : 0u);
}

/* Loop gain and damping per FDN line. Each pass through a line of n samples
 * must lose 60 dB * n / (T60 * fs) at DC; the one-pole damping filter then
 * takes the Intensity's per-pass loss off 4 kHz, scaled to the line length. */
static void reverb_update(sf_effect_state_t *state, int slot, float time_knob,
                          float intensity, int preset) {
    const sf_reverb_preset_t *p = &g_reverb_presets[preset];
    float t60 = table_log(g_knob5, g_reverb_t60, 5, time_knob)
              * table_lerp(g_reverb_int_x, g_reverb_int_t60, 6, intensity)
              * p->t60_scale;
    float hf_db = table_lerp(g_reverb_int_x, g_reverb_int_hf_db, 6, intensity) * p->hf_scale;
    if (t60 < 0.05f) t60 = 0.05f;
    float cw = cosf(SF_TWO_PI * 4000.0f / SF_SAMPLE_RATE);
    for (int c = 0; c < SF_REV_COMBS; ++c) {
        float n = (float)reverb_line_len(c, p->line_scale, 0);
        float g = powf(10.0f, -3.0f * n / (t60 * SF_SAMPLE_RATE));
        float target = powf(10.0f, -hf_db * (n / SF_REVERB_PASS_SAMPLES) * 0.05f);
        float b = 0.0f;
        if (target < 0.9999f) {
            /* |(1 - b) / (1 - b e^-jw)| = target, solved for b. */
            float t2 = target * target;
            float k = 1.0f - t2 * cw;
            float d = 1.0f - t2;
            float disc = k * k - d * d;
            b = (k - sqrtf(disc > 0.0f ? disc : 0.0f)) / d;
            b = sf_clampf(b, 0.0f, 0.98f);
        }
        for (int ch = 0; ch < 2; ++ch) {
            state->rev_damp_lane[slot][2 * c + ch] = b;
            state->rev_damp1_lane[slot][2 * c + ch] = 1.0f - b;
            state->rev_gain_lane[slot][2 * c + ch] = g;
            unsigned len = reverb_line_len(c, p->line_scale, ch);
            state->rev_len[slot][c][ch] = len;
            /* A shorter preset's lines wrap their cursors here, once,
             * instead of checking every sample. */
            if (state->rev_comb_pos[slot][c][ch] >= len)
                state->rev_comb_pos[slot][c][ch] %= len;
        }
    }
    {
        unsigned pre = (unsigned)(p->predelay_ms * 0.001f * SF_SAMPLE_RATE);
        if (pre > SF_REV_PRE_SIZE - 1u) pre = SF_REV_PRE_SIZE - 1u;
        state->rev_pre_len[slot] = pre;
    }
    /* The MiniFreak's wet grows only about 1.5 dB per doubling of the decay,
     * half what a fixed-gain network gives, so the level leans against the
     * Time knob's T60 (3.48 s at noon, where wet_db was fitted). */
    float base_t60 = table_log(g_knob5, g_reverb_t60, 5, time_knob);
    state->rev_level[slot] = SF_REVERB_WET
        * powf(10.0f, (p->wet_db - SF_REVERB_T60_TILT * log2f(base_t60 / 3.48f)) * 0.05f);
    state->rev_key_time[slot] = time_knob;
    state->rev_key_fb[slot] = intensity;
    state->rev_key_sub[slot] = preset;
}

/* The tap modulation for the slot's current rev_mod_phase (see rev_mod_u). */
static void reverb_mod_prepare(sf_effect_state_t *state, int slot) {
    /* fx_sin_cycle of the phase and of a quarter cycle on, side by side.
     * floorf is trunc minus one where trunc overshoots, exact for any
     * |phase| < 2^31. */
    float phase = state->rev_mod_phase[slot];
    sf_v2f v = {phase, phase + 0.25f};
    sf_v2f tr = __builtin_convertvector(__builtin_convertvector(v, sf_v2i), sf_v2f);
    const sf_v2f one = {1.0f, 1.0f}, zero = {0.0f, 0.0f};
    const sf_v2f half = {0.5f, 0.5f}, quarter = {0.25f, 0.25f};
    sf_v2f x = v - (tr - v2f_select((sf_v2i)(tr > v), one, zero));
    x = v2f_select((sf_v2i)(x >= half), x - 1.0f, x);
    x = v2f_select((sf_v2i)(x > quarter), 0.5f - x,
                   v2f_select((sf_v2i)(x < -quarter), -0.5f - x, x));
    sf_v2f t = SF_TWO_PI * x;
    sf_v2f t2 = t * t;
    sf_v2f sc = t * (1.0f + t2 * (-1.6666667e-1f + t2 * (8.3333333e-3f
               + t2 * (-1.9841270e-4f + t2 * 2.7557319e-6f))));
    float mod_sin = sc[0];
    float mod_cos = sc[1];
    for (int h = 0; h < 2; ++h) {
        sf_v4f ms = v4f_load(&state->rev_mod_sin[slot][2 * h][0]);
        sf_v4f mc = v4f_load(&state->rev_mod_cos[slot][2 * h][0]);
        v4f_store(&state->rev_mod_u[slot][4 * h], mod_sin * ms + mod_cos * mc);
    }
}

/* 3-band EQ layouts (Default, Wide, Mid 1K): shelf and bell frequency and Q.
 * Default is the MiniFreak's, fitted to within 0.12 dB. */
static const float g_eq_layout[3][3][2] = {
    {{125.0f, 0.433f}, {643.0f, 0.379f}, {4100.0f, 0.437f}},
    {{80.0f, 0.433f}, {643.0f, 0.30f}, {6500.0f, 0.437f}},
    {{160.0f, 0.433f}, {1000.0f, 0.70f}, {3200.0f, 0.437f}}
};

static void update_peak_coefficients(sf_effect_state_t *state, int slot) {
    float frequency = sf_clampf(state->peak_frequency_smooth[slot], 30.0f, 15000.0f);
    float gain_db = sf_clampf(state->peak_gain_smooth[slot], -15.0f, 6.0f);
    float q = sf_clampf(state->peak_width_smooth[slot], 0.1f, 10.0f);
    float omega = SF_TWO_PI * frequency / SF_SAMPLE_RATE;
    float sine = sinf(omega);
    float cosine = cosf(omega);
    float amplitude = powf(10.0f, gain_db / 40.0f);
    float alpha = sine / (2.0f * q);
    float a0 = 1.0f + alpha / amplitude;
    state->peak_b0[slot] = (1.0f + alpha * amplitude) / a0;
    state->peak_b1[slot] = (-2.0f * cosine) / a0;
    state->peak_b2[slot] = (1.0f - alpha * amplitude) / a0;
    state->peak_a1[slot] = (-2.0f * cosine) / a0;
    state->peak_a2[slot] = (1.0f - alpha / amplitude) / a0;
}

static void reset_filters(sf_effect_state_t *state, int slot) {
    state->crush_l[slot] = 0.0f;
    state->crush_r[slot] = 0.0f;
    state->crush_phase[slot] = 1.0f;
    state->delay_low_l[slot] = 0.0f;
    state->delay_low_r[slot] = 0.0f;
    memset(state->phaser_l[slot], 0, sizeof(state->phaser_l[slot]));
    memset(state->phaser_r[slot], 0, sizeof(state->phaser_r[slot]));
    memset(state->phaser_fb[slot], 0, sizeof(state->phaser_fb[slot]));
    memset(state->eq_z[slot], 0, sizeof(state->eq_z[slot]));
    memset(state->drive_z[slot], 0, sizeof(state->drive_z[slot]));
    memset(state->comp_z[slot], 0, sizeof(state->comp_z[slot]));
    memset(state->delay_z[slot], 0, sizeof(state->delay_z[slot]));
    memset(state->rev_comb_low[slot], 0, sizeof(state->rev_comb_low[slot]));
    for (int channel = 0; channel < 2; ++channel) {
        state->eq_low_state[slot][channel] = 0.0f;
        state->peak_x1[slot][channel] = 0.0f;
        state->peak_x2[slot][channel] = 0.0f;
        state->peak_y1[slot][channel] = 0.0f;
        state->peak_y2[slot][channel] = 0.0f;
        for (int band = 0; band < SF_FX_COMP_BANDS; ++band) {
            state->comp_env[slot][channel][band] = 0.0f;
            state->comp_gain[slot][channel][band] = 1.0f;
        }
    }
    state->comp_tick[slot] = 0u;
}

void sf_effect_state_init(sf_effect_state_t *state,
                          const float mix[SF_FX_SLOTS],
                          const float eq_db[SF_FX_SLOTS][3],
                          const float peak_frequency[SF_FX_SLOTS],
                          const float peak_gain[SF_FX_SLOTS],
                          const float peak_width[SF_FX_SLOTS]) {
    for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
        state->mix_smooth[slot] = mix[slot];
        state->pos[slot] = 0;
        state->clear_pos[slot] = 0;
        state->last_type[slot] = FX_OFF;
        state->phase[slot] = 0.0f;
        state->rev_mod_phase[slot] = 0.0f;
        state->peak_frequency_smooth[slot] = peak_frequency[slot];
        state->peak_gain_smooth[slot] = peak_gain[slot];
        state->peak_width_smooth[slot] = peak_width[slot];
        state->rev_ready[slot] = 0u;
        state->rev_clear[slot] = 0u;
        state->rev_key_time[slot] = -1.0f;
        state->rev_key_fb[slot] = -1.0f;
        state->rev_key_sub[slot] = -1;
        state->eq_key_sub[slot] = -1;
        state->drive_key[slot] = -1.0f;
        state->drive_gain_key[slot] = -1.0f;
        state->mix_key[slot] = -1.0f;
        state->unison_tone_key[slot] = 2.0f; /* the tone itself spans -1..1 */
        state->blk[slot].type = -1; /* no block values yet */
        state->comp_split_ready[slot] = 0u;
        state->delay_bq_ready[slot] = 0u;
        state->rate_key_table[slot] = -1;
        state->crush_key_time[slot] = -1.0f;
        state->crush_key_int[slot] = -1.0f;
        state->phaser_hold[slot] = 0.5f;
        state->phaser_step[slot] = -1;
        state->phaser_rng[slot] = 0x9E3779B9u * (unsigned)(slot + 1);
        state->phaser_key_sync[slot] = -1;
        state->phaser_key_rate[slot] = -1.0f;
        state->phaser_key_int[slot] = -1.0f;
        state->phaser_key_space[slot] = -1;
        state->phaser_key_sweep[slot] = -1.0f;
        state->comp_key_time[slot] = -1.0f;
        reset_filters(state, slot);
        for (int comb = 0; comb < SF_REV_COMBS; ++comb) {
            state->rev_comb_pos[slot][comb][0] = 0u;
            state->rev_comb_pos[slot][comb][1] = 0u;
            for (int ch = 0; ch < 2; ++ch) {
                float angle = SF_TWO_PI * ((float)comb * 0.19f
                                         + (float)ch * 0.31f);
                state->rev_mod_sin[slot][comb][ch] = sinf(angle);
                state->rev_mod_cos[slot][comb][ch] = cosf(angle);
            }
        }
        for (int ap = 0; ap < SF_REV_APS; ++ap) {
            state->rev_ap_pos[slot][ap][0] = 0u;
            state->rev_ap_pos[slot][ap][1] = 0u;
        }
        state->rev_pre_pos[slot] = 0u;
        reverb_mod_prepare(state, slot);
        for (int band = 0; band < 3; ++band) {
            float db = eq_db ? eq_db[slot][band] : 0.0f;
            state->eq_smooth_db[slot][band] = db;
            state->eq_key_db[slot][band] = 1000.0f;
        }
        update_peak_coefficients(state, slot);
        state->peak_key[slot][0] = state->peak_frequency_smooth[slot];
        state->peak_key[slot][1] = state->peak_gain_smooth[slot];
        state->peak_key[slot][2] = state->peak_width_smooth[slot];
    }
}

void sf_effect_smooth_mix(sf_effect_state_t *state, const float mix[SF_FX_SLOTS],
                         float amount) {
    for (int slot = 0; slot < SF_FX_SLOTS; ++slot)
        state->mix_smooth[slot] += amount * (mix[slot] - state->mix_smooth[slot]);
}

void sf_effect_smooth_eq(sf_effect_state_t *state,
                         const float eq_db[SF_FX_SLOTS][3],
                         const float peak_frequency[SF_FX_SLOTS],
                         const float peak_gain[SF_FX_SLOTS],
                         const float peak_width[SF_FX_SLOTS], float amount) {
    for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
        for (int band = 0; band < 3; ++band) {
            state->eq_smooth_db[slot][band] += amount
                * (eq_db[slot][band] - state->eq_smooth_db[slot][band]);
        }
        state->peak_frequency_smooth[slot] += amount
            * (peak_frequency[slot] - state->peak_frequency_smooth[slot]);
        state->peak_gain_smooth[slot] += amount
            * (peak_gain[slot] - state->peak_gain_smooth[slot]);
        state->peak_width_smooth[slot] += amount
            * (peak_width[slot] - state->peak_width_smooth[slot]);
    }
}

/* The effects that read back through the slot's delay ring: they stay dry
 * until it is clear. The rest never read it (Reverb clears its own region). */
static int reads_delay_ring(int type) {
    return type == FX_DELAY || type == FX_CHORUS || type == FX_FLANGER
        || type == FX_SUPER_UNISON;
}

void sf_effect_clear_buffers(sf_effect_state_t *state, const int type[SF_FX_SLOTS]) {
    for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
        unsigned begin = state->clear_pos[slot];
        /* A slot that waits on the clear gets the bigger chunk: a Delay is
         * back 64 ms after a preset change instead of 190 ms. */
        unsigned chunk = type && reads_delay_ring(type[slot]) ? SF_FX_CLEAR_CHUNK_WAITING
                                                              : SF_FX_CLEAR_CHUNK;
        unsigned end = begin + chunk;
        if (end > (unsigned)SF_DELAY_SIZE) end = (unsigned)SF_DELAY_SIZE;
        for (unsigned i = begin; i < end; ++i) {
            state->delay_l[slot][i] = 0.0f;
            state->delay_r[slot][i] = 0.0f;
        }
        state->clear_pos[slot] = end;
    }
}

void sf_effect_reset_nonfinite(sf_effect_state_t *state) {
    /* The delay memory is wiped by the incremental clear, as after a preset
     * load, rather than by a 3 MB memset on the audio thread. */
    for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
        state->clear_pos[slot] = 0;
        reset_filters(state, slot);
        state->rev_ready[slot] = 0u;
        state->rev_clear[slot] = 0u;
    }
}

/* Dry and wet gains for an Amount position. The MiniFreak does not use one
 * mix law for every effect:
 *   Delay, Reverb  wet rises (squared) to full by Amount 0.5 with the dry
 *                  kept, then the dry falls away (squared). This is the
 *                  Delay's Insert law; the importer converts Send settings
 *                  and the Reverb's own laws onto it.
 *   Phaser         the all-pass sum tops out at an even blend, so the notches
 *                  only reach full depth at Amount 1.
 *   Chorus, Flanger, Distortion, Bit Crusher
 *                  equal-power crossfade.
 *   Super Unison   linear crossfade (not yet measured). */
static void mix_law(sf_effect_state_t *state, int slot, int type, float mix,
                    float *dry, float *wet) {
    mix = sf_clampf(mix, 0.0f, 1.0f);
    switch (type) {
    case FX_DELAY:
    case FX_REVERB: {
        float w = mix < 0.5f ? 2.0f * mix : 1.0f;
        float d = mix > 0.5f ? 2.0f * (1.0f - mix) : 1.0f;
        *wet = w * w;
        *dry = d * d;
        break;
    }
    case FX_PHASER:
        *wet = 0.5f * mix;
        *dry = 1.0f - 0.5f * mix;
        break;
    case FX_CHORUS:
    case FX_FLANGER:
    case FX_DRIVE:
    case FX_CRUSH:
        /* The same curve for all four, so one cached pair serves the slot. */
        if (mix != state->mix_key[slot]) {
            state->mix_key[slot] = mix;
            state->mix_wet[slot] = sinf(0.5f * SF_PI * mix);
            state->mix_dry[slot] = cosf(0.5f * SF_PI * mix);
        }
        *wet = state->mix_wet[slot];
        *dry = state->mix_dry[slot];
        break;
    default:
        *wet = mix;
        *dry = 1.0f - mix;
        break;
    }
}

static float knob_rate(sf_effect_state_t *state, int slot, int table, float time_knob) {
    if (state->rate_key_table[slot] != table || state->rate_key_time[slot] != time_knob) {
        state->rate_key_table[slot] = table;
        state->rate_key_time[slot] = time_knob;
        state->rate[slot] = table == RATE_CHORUS
            ? table_lerp(g_knob5, g_chorus_rate, 5, time_knob)
            : table_log(g_knob5, table == RATE_FLANGER ? g_flanger_rate : g_phaser_rate,
                        5, time_knob);
    }
    return state->rate[slot];
}

static float lfo_triangle(float phase) {
    return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
}

/* Multi Comp gain computer: every band's gain from its detector envelope,
 * the preset, Amount and Intensity (the tilt). */
static void comp_update_gains(sf_effect_state_t *state, int slot, int subtype,
                              float amount, float intensity) {
    static const float curve_x[8] = {-100.0f, -81.0f, -71.0f, -61.0f, -51.0f, -45.0f, -39.0f, -34.0f};
    static const float curve_db[8] = {0.0f, 0.8f, 2.0f, 4.7f, 10.8f, 13.5f, 10.7f, 8.4f};
    int preset = subtype >= 0 && subtype < 5 ? subtype : 0;
    float band_scale[SF_FX_COMP_BANDS] = {1.0f, 0.87f, 1.0f};
    /* Each band's detector is calibrated separately: on a full voice
     * the MiniFreak lifts the low band (fundamental included) about
     * 5 dB, the mids barely and the top end about 10 dB. */
    const float band_offset[SF_FX_COMP_BANDS] = {
        SF_COMP_LEVEL_OFFSET_DB + SF_COMP_OFFSET_LOW, SF_COMP_LEVEL_OFFSET_DB,
        SF_COMP_LEVEL_OFFSET_DB + SF_COMP_OFFSET_HIGH};
    float upward_scale = 1.0f;
    float overall = 1.0f;
    switch (preset) {
    case 1: /* Bass Ctrl */
        band_scale[0] *= 1.25f;
        band_scale[2] *= 0.90f;
        break;
    case 2: /* High Ctrl */
        band_scale[0] *= 0.90f;
        band_scale[2] *= 1.25f;
        break;
    case 3: /* All Up */
        upward_scale = 1.4f;
        break;
    case 4: /* Tighter */
        overall = 0.6f;
        break;
    default: /* OPP */
        break;
    }
    float tilt[SF_FX_COMP_BANDS] = {12.1f * (0.5f - intensity), 0.0f,
                                    12.8f * (intensity - 0.5f)};
    for (int channel = 0; channel < 2; ++channel) {
        for (int band = 0; band < SF_FX_COMP_BANDS; ++band) {
            float envelope = state->comp_env[slot][channel][band];
            float level = 10.0f * log10f(envelope + 1e-12f) + band_offset[band];
            float lift = level < -34.0f
                ? table_lerp(curve_x, curve_db, 8, level)
                : 8.4f - 0.5f * (level + 34.0f);
            if (level < -45.0f) lift *= upward_scale;
            float gain_db = amount * (lift * band_scale[band] * overall + tilt[band]);
            state->comp_gain[slot][channel][band] = powf(10.0f, gain_db * 0.05f);
        }
    }
}

/* The phaser's all-pass coefficient for a sweep position (0..1, linear in
 * octaves from lo). */
static float phaser_coefficient(float lo, float octaves, float sweep) {
    float corner = lo * exp2f(octaves * sweep);
    float t = fx_tan_small(SF_PI * corner / SF_SAMPLE_RATE);
    return (t - 1.0f) / (t + 1.0f);
}
/* sf_clampf's lower half, for a value whose upper bound cannot be reached. */
static inline float fx_at_least(float x, float lo) {
    return x >= lo ? x : lo;
}

/* Chorus voice layouts: Default, Lush, Single. */
static const float g_chorus_base_ms[3][3] = {{13.0f, 18.0f, 0.0f},
                                             {11.0f, 15.0f, 19.0f},
                                             {15.0f, 0.0f, 0.0f}};
static const float g_chorus_depth_ms[3][3] = {{11.5f, 7.0f, 0.0f},
                                              {11.0f, 9.0f, 7.0f},
                                              {9.0f, 0.0f, 0.0f}};
/* Each voice's LFO offset, (float)v / (float)voices for the layout. */
static const float g_chorus_offset[3][3] = {{0.0f, 1.0f / 2.0f, 0.0f},
                                            {0.0f, 1.0f / 3.0f, 2.0f / 3.0f},
                                            {0.0f, 0.0f, 0.0f}};
/* Super Unison copy offsets, (float)copy / 6.0f, padded to eight lanes. */
static const float g_unison_offset[8] = {0.0f / 6.0f, 1.0f / 6.0f, 2.0f / 6.0f,
                                         3.0f / 6.0f, 4.0f / 6.0f, 5.0f / 6.0f, 0.0f, 0.0f};

/* Recomputes the slot's block values (sf_fx_block_t) for new controls. Same
 * expressions as the per-sample code had, so the results are bit-identical. */
static void fx_block_update(sf_effect_state_t *state, int slot, int type, int subtype,
                            float mix, float time_ms, float feedback, float tempo) {
    sf_fx_block_t *b = &state->blk[slot];
    b->type = type;
    b->subtype = subtype;
    /* The MiniFreak's knob positions; see "Knob curves" above. */
    float time_knob = sf_clampf((time_ms - 1.0f) / (SF_FX_TIME_MAX - 1.0f), 0.0f, 1.0f);
    float intensity = sf_clampf(feedback / 0.95f, 0.0f, 1.0f);
    b->time_knob = time_knob;
    b->intensity = intensity;
    float dry_gain = 1.0f, wet_gain = 0.0f;
    mix_law(state, slot, type, mix, &dry_gain, &wet_gain);
    b->dry = dry_gain;
    b->wet = wet_gain;

    if (type == FX_DELAY) {
        int mode = subtype % 7;
        int sync = subtype >= 7;
        float delay_l_samples;
        if (sync) {
            int division = (int)(time_knob * (float)(SF_DELAY_DIV_COUNT - 1) + 0.5f);
            if (division < 0) division = 0;
            if (division >= SF_DELAY_DIV_COUNT) division = SF_DELAY_DIV_COUNT - 1;
            float bpm = sf_clampf(tempo, 20.0f, 300.0f);
            delay_l_samples = (60.0f / bpm) * g_delay_divs[division].beats
                            * SF_SAMPLE_RATE;
        } else {
            delay_l_samples = time_ms * 0.001f * SF_SAMPLE_RATE;
        }
        /* Stereo and Filtered answer on the right 5 ms late. */
        float delay_r_samples = delay_l_samples;
        if (mode == 1 || mode == 4) delay_r_samples += 0.005f * SF_SAMPLE_RATE;
        b->delay_samples[0] = sf_clampf(delay_l_samples, 1.0f, (float)(SF_DELAY_SIZE - 4));
        b->delay_samples[1] = sf_clampf(delay_r_samples, 1.0f, (float)(SF_DELAY_SIZE - 4));
        static const float mode_gain[7] = {1.0f, 0.9f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        b->delay_gain = SF_DELAY_WET * mode_gain[mode];
    } else if (type == FX_CHORUS) {
        float rate = knob_rate(state, slot, RATE_CHORUS, time_knob);
        int layout = subtype == 1 ? 1 : (subtype == 4 ? 2 : 0);
        b->voices = layout == 1 ? 3 : (layout == 2 ? 1 : 2);
        b->voice_gain = layout == 1 ? 0.33f : (layout == 2 ? 0.6f : 0.4f);
        float depth_scale = intensity;
        if (subtype == 1) rate *= 0.80f;
        else if (subtype == 2) { rate *= 0.65f; depth_scale *= 1.15f; }
        else if (subtype == 3) { rate *= 0.50f; depth_scale *= 0.80f; }
        for (int v = 0; v < 3; ++v) {
            b->base_ms[v] = g_chorus_base_ms[layout][v];
            b->depth_ms[v] = g_chorus_depth_ms[layout][v] * depth_scale;
        }
        b->phase_inc = rate / SF_SAMPLE_RATE;
    } else if (type == FX_FLANGER) {
        int silly = subtype == 2 || subtype == 3;
        int sync = subtype == 1 || subtype == 3;
        float rate = sync
            ? (sf_clampf(tempo, 20.0f, 300.0f) / 60.0f) / 2.0f
            : knob_rate(state, slot, RATE_FLANGER, time_knob);
        float depth_ms = 0.53f;
        if (silly) { rate *= 1.75f; depth_ms *= 1.5f; }
        b->depth_ms[0] = depth_ms;
        b->fb = 0.9f * intensity;
        b->phase_inc = rate / SF_SAMPLE_RATE;
    } else if (type == FX_SUPER_UNISON) {
        int preset = subtype >= 0 && subtype < 8 ? subtype : 0;
        /* Classic, Ravey, Soli, Slow, Slow Trig, Wide Trig, Mono Trig, Wavy. */
        static const float rate_scale_of[8] = {1.0f, 1.8f, 0.8f, 0.35f, 0.35f, 1.2f, 1.0f, 0.9f};
        static const float width_style_of[8] = {1.0f, 1.3f, 0.9f, 0.8f, 0.8f, 1.7f, 0.05f, 1.0f};
        static const float spread_ms_of[8] = {1.8f, 2.2f, 1.5f, 2.0f, 2.0f, 2.5f, 1.8f, 2.0f};
        float rate_scale = rate_scale_of[preset];
        float width_style = width_style_of[preset];
        float spread_ms = spread_ms_of[preset];
        float detune = time_knob;
        float rate = (0.06f + detune * 0.65f) * rate_scale;
        float depth_ms = (0.25f + detune * 9.0f) * width_style;
        for (int copy = 0; copy < 6; ++copy) {
            b->base_ms[copy] = 4.0f + (float)copy * spread_ms;
            b->depth_ms[copy] = depth_ms;
        }
        b->phase_inc = rate / SF_SAMPLE_RATE;
        /* Param 2 sweeps the LPF/HPF color around a neutral center. */
        float signed_tone = (intensity - 0.5f) * 2.0f;
        b->tone_mode = 0;
        if (fabsf(signed_tone) > 0.015f) {
            if (signed_tone != state->unison_tone_key[slot]) {
                state->unison_tone_key[slot] = signed_tone;
                float cutoff = 20.0f * powf(900.0f, fabsf(signed_tone));
                float g = tanf(SF_PI * sf_clampf(cutoff, 20.0f, 18000.0f)
                               / SF_SAMPLE_RATE);
                state->unison_tone_alpha[slot] = g / (1.0f + g);
            }
            b->tone_alpha = state->unison_tone_alpha[slot];
            b->tone_mode = signed_tone < 0.0f ? 1 : 2;
        }
    }
}



/* Shared tail of every run function: advance the ring and mix. */
#define FX_FINISH(dry_gain, wet_gain)                                       \
    do {                                                                    \
        state->pos[slot] = (pos + 1u) & (SF_DELAY_SIZE - 1u);               \
        sf_fx_frame_t fx_out_;                                              \
        fx_out_.l = input_l * (dry_gain) + wet_l * (wet_gain);              \
        fx_out_.r = input_r * (dry_gain) + wet_r * (wet_gain);              \
        return fx_out_;                                                     \
    } while (0)
/* The same for an effect whose ring share prepare zeroes per block. */
#define FX_MIX(dry_gain, wet_gain)                                          \
    do {                                                                    \
        sf_fx_frame_t fx_out_;                                              \
        fx_out_.l = input_l * (dry_gain) + wet_l * (wet_gain);              \
        fx_out_.r = input_r * (dry_gain) + wet_r * (wet_gain);              \
        return fx_out_;                                                     \
    } while (0)

static sf_fx_frame_t fx_run_delay(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    unsigned pos = state->pos[slot];
    if (state->clear_pos[slot] < (unsigned)SF_DELAY_SIZE) {
        /* Waiting on the incremental clear: dry. */
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
        state->pos[slot] = (pos + 1u) & (SF_DELAY_SIZE - 1u);
        sf_fx_frame_t dry = {input_l, input_r};
        return dry;
    }
    const sf_fx_block_t *blk = &state->blk[slot];
    int subtype = blk->subtype;
    float feedback = blk->feedback;
    const float dry_gain = blk->dry, wet_gain = blk->wet;
    float wet_l = input_l;
    float wet_r = input_r;
    /*
     * Seven delay characters, each available in a free-running and a
     * tempo-synced form:
     *   0 Digital, 1 Stereo, 2 Ping-Pong, 3 Mono,
     *   4 Filtered, 5 Filtered Ping-Pong, 6 Tape
     * The Sync variants are 7..13 and use the Time knob as a subdivision
     * selector instead of milliseconds.
     */
    int mode = subtype % 7;
    /* Line lengths: blk->delay_samples (fx_block_update). Stereo and
     * Filtered answer on the right 5 ms late. */
    if (mode == 6) {
        wet_l = effect_read_delay(state->delay_l[slot], pos, blk->delay_samples[0], 1);
        wet_r = effect_read_delay(state->delay_r[slot], pos, blk->delay_samples[1], 1);
    } else {
        wet_l = effect_read_delay(state->delay_l[slot], pos, blk->delay_samples[0], 0);
        wet_r = effect_read_delay(state->delay_r[slot], pos, blk->delay_samples[1], 0);
    }

    /*
     * Line inputs. Measured on MiniFreak V:
     *   Digital        two independent lines.
     *   Stereo         the right line repeats only once (its feedback is
     *                  ~-28 dB per pass) and answers 5 ms late.
     *   Ping-Pong      the input enters the left line only; the right
     *                  line re-echoes the left at unity and feeds back
     *                  into the left, so one Intensity step spans a
     *                  left-right round trip. Low end cut below 250 Hz.
     *   Mono           one summed line, same low cut.
     *   Filtered (PP)  as Stereo (Ping-Pong) with a band-pass of about
     *                  350 Hz..1.8 kHz inside the loop, so every repeat
     *                  narrows.
     *   Tape           (Bizar's own) dark, saturating repeats.
     */
    int ping_pong = mode == 2 || mode == 5;
    int band_pass = mode == 4 || mode == 5;
    float mono_in = 0.5f * (input_l + input_r);
    float line_l, line_r;
    if (ping_pong) {
        line_l = mono_in + feedback * wet_r;
        line_r = wet_l;
    } else if (mode == 3) {
        line_l = mono_in + feedback * wet_l;
        line_r = line_l;
    } else if (mode == 1 || mode == 4) {
        line_l = input_l + feedback * wet_l;
        line_r = input_r + 0.04f * feedback * wet_r;
    } else {
        float feedback_l = wet_l;
        float feedback_r = wet_r;
        if (mode == 6) {
            /* Tape repeats lose high end on every pass and saturate. */
            state->delay_low_l[slot] += 0.20f * (feedback_l - state->delay_low_l[slot]);
            state->delay_low_r[slot] += 0.20f * (feedback_r - state->delay_low_r[slot]);
            feedback_l = tanhf(state->delay_low_l[slot] * 1.3f);
            feedback_r = tanhf(state->delay_low_r[slot] * 1.3f);
        }
        line_l = input_l + feedback * feedback_l;
        line_r = input_r + feedback * feedback_r;
    }
    if (!state->delay_bq_ready[slot]) {
        biquad_pass(&state->delay_bq[slot][0], 1, 350.0f);
        biquad_pass(&state->delay_bq[slot][1], 0, 1800.0f);
        biquad_pass(&state->delay_bq[slot][2], 1, 250.0f);
        state->delay_bq_ready[slot] = 1u;
    }
    if (band_pass) {
        for (int f = 0; f < 2; ++f) {
            line_l = biquad_run(&state->delay_bq[slot][f], state->delay_z[slot][0][f], line_l);
            line_r = biquad_run(&state->delay_bq[slot][f], state->delay_z[slot][1][f], line_r);
        }
    } else if (ping_pong || mode == 3) {
        /* 12 dB/oct low cut at 250 Hz on what enters the left line. */
        line_l = biquad_run(&state->delay_bq[slot][2], state->delay_z[slot][0][2], line_l);
        if (mode == 3) line_r = line_l;
    }
    state->delay_l[slot][pos] = sf_clampf(line_l, -4.0f, 4.0f);
    state->delay_r[slot][pos] = sf_clampf(line_r, -4.0f, 4.0f);
    if (mode == 3) wet_r = wet_l;
    wet_l *= blk->delay_gain;
    wet_r *= blk->delay_gain;
    FX_FINISH(dry_gain, wet_gain);
}

static sf_fx_frame_t fx_run_reverb(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    unsigned pos = state->pos[slot];
    /* Per sample, as before: a non-finite reset mid-block rewinds clear_pos. */
    if (state->clear_pos[slot] < SF_REV_REGION_END)
        state->clear_pos[slot] = SF_REV_REGION_END;
    const sf_fx_block_t *blk = &state->blk[slot];
    int subtype = blk->subtype;
    float time_knob = blk->time_knob;
    float intensity = blk->intensity;
    const float dry_gain = blk->dry, wet_gain = blk->wet;
    float wet_l = input_l;
    float wet_r = input_r;
    /*
     * Four input-diffusion all-pass filters feed a four-line feedback delay
     * network with a normalised Hadamard matrix, per-line damping, and
     * gently modulated fractional taps. Time sets the decay (T60), and
     * Intensity damps it, the top end most; both follow the MiniFreak's
     * measured curves. Loop gains are derived from the target T60, so the
     * decay no longer depends on the line lengths a preset picks.
     */
    if (!state->rev_ready[slot]) {
        unsigned begin = state->rev_clear[slot];
        unsigned end = begin + SF_REV_CLEAR_CHUNK;
        if (end > SF_REV_REGION_END) end = SF_REV_REGION_END;
        for (unsigned i = begin; i < end; ++i) {
            state->delay_l[slot][i] = 0.0f;
            state->delay_r[slot][i] = 0.0f;
        }
        state->rev_clear[slot] = end;
        if (end >= SF_REV_REGION_END) {
            state->rev_ready[slot] = 1u;
            for (int c = 0; c < SF_REV_COMBS; ++c) {
                state->rev_comb_pos[slot][c][0] = 0u;
                state->rev_comb_pos[slot][c][1] = 0u;
                state->rev_comb_low[slot][c][0] = 0.0f;
                state->rev_comb_low[slot][c][1] = 0.0f;
            }
            for (int a = 0; a < SF_REV_APS; ++a) {
                state->rev_ap_pos[slot][a][0] = 0u;
                state->rev_ap_pos[slot][a][1] = 0u;
            }
            state->rev_pre_pos[slot] = 0u;
        }
        sf_fx_frame_t dry = {input_l, input_r};
        return dry;
    }

    int preset = subtype >= 0 && subtype < 6 ? subtype : 0;
    const sf_reverb_preset_t *p = &g_reverb_presets[preset];
    if (preset != state->rev_key_sub[slot]
        || fabsf(time_knob - state->rev_key_time[slot]) > 1e-4f
        || fabsf(intensity - state->rev_key_fb[slot]) > 1e-4f)
        reverb_update(state, slot, time_knob, intensity, preset);

    /* Pre-delay: the MiniFreak's wet arrives 10 to 90 ms after the dry,
     * depending on the preset. */
    float pre_l = input_l;
    float pre_r = input_r;
    {
        unsigned pre = state->rev_pre_len[slot];
        unsigned w = state->rev_pre_pos[slot];
        state->delay_l[slot][SF_REV_PRE_BASE + w] = input_l;
        state->delay_r[slot][SF_REV_PRE_BASE + w] = input_r;
        unsigned r = (w + SF_REV_PRE_SIZE - pre) % SF_REV_PRE_SIZE;
        pre_l = state->delay_l[slot][SF_REV_PRE_BASE + r];
        pre_r = state->delay_r[slot][SF_REV_PRE_BASE + r];
        state->rev_pre_pos[slot] = (w + 1u) % SF_REV_PRE_SIZE;
    }

    /* Input diffusion. The all-pass coefficient also decides how discrete
     * or smeared Echoes sounds. */
    float diff_l, diff_r;
    {
        /* Left and right side by side. */
        sf_v2f x = {pre_l * 0.30f, pre_r * 0.30f};
        const float d = p->diffusion;
        float *const bl = state->delay_l[slot];
        float *const br = state->delay_r[slot];
        for (int a = 0; a < SF_REV_APS; ++a) {
            unsigned base = rev_ap_base[a];
            unsigned il = state->rev_ap_pos[slot][a][0];
            unsigned ir = state->rev_ap_pos[slot][a][1];
            sf_v2f stored = {bl[base + il], br[base + ir]};
            sf_v2f out = -d * x + stored;
            sf_v2f w = v2f_clamp(x + d * stored, -4.0f, 4.0f);
            bl[base + il] = w[0];
            br[base + ir] = w[1];
            x = out;
        }
        diff_l = x[0];
        diff_r = x[1];
        /* Advance the eight all-pass cursors (lane 2 * stage + channel). */
        for (int h = 0; h < 2; ++h) {
            sf_v4u step = v4u_load(&state->rev_ap_pos[slot][2 * h][0]) + 1u;
            sf_v4u wrap = (sf_v4u)(step < v4u_load(&rev_ap_lane_len[4 * h]));
            v4u_store(&state->rev_ap_pos[slot][2 * h][0], step & wrap);
        }
    }

    /* Line outputs, lane 2 * line + channel: lines 0-1, then 2-3. */
    sf_v4f lines[2];
    {
        /* The eight taps (lane 2 * line + channel) run as two four-lane
         * vectors, each lane doing the scalar arithmetic unchanged. */
        const float md = p->mod_depth;
        float *const lane_buf[2] = {state->delay_l[slot], state->delay_r[slot]};
        sf_v4u idx[2] = {v4u_load(&state->rev_comb_pos[slot][0][0]),
                         v4u_load(&state->rev_comb_pos[slot][2][0])};
        sf_v4u len[2] = {v4u_load(&state->rev_len[slot][0][0]),
                         v4u_load(&state->rev_len[slot][2][0])};
        sf_v4f frac[2];
        unsigned i0[8];
        for (int h = 0; h < 2; ++h) {
            sf_v4f mod = md * v4f_load(&state->rev_mod_u[slot][4 * h]);
            /* idx is the oldest sample (about to be overwritten); the
             * slots just after it are the next oldest. The tap swings
             * inside that region. Reading behind idx would land on
             * samples written a moment ago and short-circuit the line.
             * |mod| <= mod_depth, so read >= idx + 1.5. */
            sf_v4f read = __builtin_convertvector(idx[h], sf_v4f) + md + 1.5f + mod;
            sf_v4u i = __builtin_convertvector(read, sf_v4u);
            frac[h] = read - __builtin_convertvector(i, sf_v4f);
            v4u_store(&i0[4 * h], i);
        }
        /* Four-point Hermite: linear interpolation would take ~1 dB
         * off 6 kHz on every pass and the top of a long tail would
         * die several times faster than the measured damping. */
        sf_v4f x[4][2];
        if (v4i_all((sf_v4i)(idx[0] + SF_REV_TAP_REACH <= len[0])
                    & (sf_v4i)(idx[1] + SF_REV_TAP_REACH <= len[1]))) {
            /* Every tap and its neighbours (idx + 1.5 + 2 mod_depth + 2
             * at most) lie inside its line: four contiguous samples. */
            for (int h = 0; h < 2; ++h) {
                sf_v4f w[4];
                for (int k = 0; k < 4; ++k) {
                    int lane = 4 * h + k;
                    w[k] = v4f_load(lane_buf[lane & 1] + rev_comb_lane_base[lane] + i0[lane] - 1u);
                }
                sf_v4f t0 = __builtin_shufflevector(w[0], w[1], 0, 4, 1, 5);
                sf_v4f t1 = __builtin_shufflevector(w[2], w[3], 0, 4, 1, 5);
                sf_v4f t2 = __builtin_shufflevector(w[0], w[1], 2, 6, 3, 7);
                sf_v4f t3 = __builtin_shufflevector(w[2], w[3], 2, 6, 3, 7);
                x[0][h] = __builtin_shufflevector(t0, t1, 0, 1, 4, 5);
                x[1][h] = __builtin_shufflevector(t0, t1, 2, 3, 6, 7);
                x[2][h] = __builtin_shufflevector(t2, t3, 0, 1, 4, 5);
                x[3][h] = __builtin_shufflevector(t2, t3, 2, 3, 6, 7);
            }
        } else {
            /* A tap near the end of its line: its neighbours wrap. The
             * read position needs no wrap of its own, since frac is the
             * same either way. */
            float g[4][8];
            for (int lane = 0; lane < 8; ++lane) {
                unsigned n = state->rev_len[slot][lane >> 1][lane & 1];
                const float *buf = lane_buf[lane & 1] + rev_comb_lane_base[lane];
                for (int k = 0; k < 4; ++k) {
                    unsigned at = i0[lane] - 1u + (unsigned)k;
                    if (at >= n) at -= n;
                    g[k][lane] = buf[at];
                }
            }
            for (int k = 0; k < 4; ++k) {
                x[k][0] = v4f_load(&g[k][0]);
                x[k][1] = v4f_load(&g[k][4]);
            }
        }
        for (int h = 0; h < 2; ++h) {
            sf_v4f xm1 = x[0][h], x0 = x[1][h], x1 = x[2][h], x2 = x[3][h];
            sf_v4f f = frac[h];
            sf_v4f c1 = 0.5f * (x1 - xm1);
            sf_v4f c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            sf_v4f c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            sf_v4f out = ((c3 * f + c2) * f + c1) * f + x0;
            sf_v4f low = v4f_load(&state->rev_comb_low[slot][2 * h][0]);
            low = out * v4f_load(&state->rev_damp1_lane[slot][4 * h])
                + low * v4f_load(&state->rev_damp_lane[slot][4 * h]);
            v4f_store(&state->rev_comb_low[slot][2 * h][0], low);
            lines[h] = low;
        }
    }

    /* Normalised 4x4 Hadamard feedback matrix, both channels at once:
     * the sums keep the scalar order ((o0 +- o1) +- o2) +- o3, and
     * adding a negated lane is exactly a subtraction. */
    sf_v4f o0 = __builtin_shufflevector(lines[0], lines[0], 0, 1, 0, 1);
    sf_v4f o1 = __builtin_shufflevector(lines[0], -lines[0], 2, 3, 6, 7);
    sf_v4f o2 = __builtin_shufflevector(lines[1], lines[1], 0, 1, 0, 1);
    sf_v4f o3 = __builtin_shufflevector(lines[1], -lines[1], 2, 3, 6, 7);
    sf_v4f o3n = __builtin_shufflevector(lines[1], -lines[1], 6, 7, 2, 3);
    sf_v4f s01 = o0 + o1;                /* o0 + o1, o0 - o1 */
    sf_v4f sum01 = s01 + o2 + o3;        /* rows 0 and 1, L R L R */
    sf_v4f sum23 = s01 - o2 + o3n;       /* rows 2 and 3 */
    sf_v4f had[2] = {sum01 * 0.5f, sum23 * 0.5f};
    {
        float *const lane_buf[2] = {state->delay_l[slot], state->delay_r[slot]};
        /* Lines 0 and 2 take the input as is, 1 and 3 crossed and
         * inverted. */
        sf_v4f feed = {diff_l * 0.5f, diff_r * 0.5f, -diff_r * 0.5f, -diff_l * 0.5f};
        for (int h = 0; h < 2; ++h) {
            sf_v4f next = feed + v4f_load(&state->rev_gain_lane[slot][4 * h]) * had[h];
            next = v4f_clamp(next, -4.0f, 4.0f);
            sf_v4u idx = v4u_load(&state->rev_comb_pos[slot][2 * h][0]);
            for (int k = 0; k < 4; ++k) {
                int lane = 4 * h + k;
                lane_buf[lane & 1][rev_comb_lane_base[lane] + idx[k]] = next[k];
            }
            sf_v4u step = idx + 1u;
            sf_v4u wrap = (sf_v4u)(step < v4u_load(&state->rev_len[slot][2 * h][0]));
            v4u_store(&state->rev_comb_pos[slot][2 * h][0], step & wrap);
        }
    }

    float sum_l = sum01[0] * 0.25f;
    float sum_r = sum01[1] * 0.25f;
    if (p->early_gain > 0.0f) {
        sum_l += diff_l * p->early_gain;
        sum_r += diff_r * p->early_gain;
    }
    wet_l = sum_l * state->rev_level[slot];
    wet_r = sum_r * state->rev_level[slot];
    state->rev_mod_phase[slot] += 0.12f / SF_SAMPLE_RATE;
    if (state->rev_mod_phase[slot] >= 1.0f) state->rev_mod_phase[slot] -= 1.0f;
    reverb_mod_prepare(state, slot);
    FX_FINISH(dry_gain, wet_gain);
}

static sf_fx_frame_t fx_run_chorus(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    unsigned pos = state->pos[slot];
    if (state->clear_pos[slot] < (unsigned)SF_DELAY_SIZE) {
        /* Waiting on the incremental clear: dry. */
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
        state->pos[slot] = (pos + 1u) & (SF_DELAY_SIZE - 1u);
        sf_fx_frame_t dry = {input_l, input_r};
        return dry;
    }
    const sf_fx_block_t *blk = &state->blk[slot];
    int subtype = blk->subtype;
    const float dry_gain = blk->dry, wet_gain = blk->wet;
    float wet_l = input_l;
    float wet_r = input_r;
    /*
     * 0 Default, 1 Lush, 2 Dark, 3 Shaded, 4 Single. Default is the
     * MiniFreak's pair of voices sitting at 13 and 18 ms and swinging in
     * opposite directions; Intensity widens the swing (the lower voice
     * more), Time sets the rate. Lush adds a third voice and Single keeps
     * one.
     */
    /* Voice layout, rate and swing: fx_block_update. */
    int layout = subtype == 1 ? 1 : (subtype == 4 ? 2 : 0);
    int voices = blk->voices;
    float phase = state->phase[slot];
    wet_l = 0.0f;
    wet_r = 0.0f;
    for (int v = 0; v < voices; ++v) {
        float offset = g_chorus_offset[layout][v];
        float lfo_l = fx_sin_cycle(phase + offset);
        float lfo_r = fx_sin_cycle(phase + offset + 0.25f);
        float d = blk->depth_ms[v];
        float base = blk->base_ms[v];
        /* base + |d| is at most 26.3 ms, so the 40 ms ceiling of the
         * old clamp never acts. */
        float delay_ms_l = fx_at_least(base + d * lfo_l, 0.5f);
        float delay_ms_r = fx_at_least(base + d * lfo_r, 0.5f);
        wet_l += effect_read_delay(state->delay_l[slot], pos,
                                   delay_ms_l * 0.001f * SF_SAMPLE_RATE, 0);
        wet_r += effect_read_delay(state->delay_r[slot], pos,
                                   delay_ms_r * 0.001f * SF_SAMPLE_RATE, 0);
    }
    wet_l *= blk->voice_gain;
    wet_r *= blk->voice_gain;
    if (subtype == 2 || subtype == 3) {
        const float alpha = subtype == 2 ? 0.18f : 0.28f;
        state->eq_low_state[slot][0] += alpha
            * (wet_l - state->eq_low_state[slot][0]);
        state->eq_low_state[slot][1] += alpha
            * (wet_r - state->eq_low_state[slot][1]);
        wet_l = state->eq_low_state[slot][0];
        wet_r = state->eq_low_state[slot][1];
    }
    state->delay_l[slot][pos] = input_l;
    state->delay_r[slot][pos] = input_r;
    phase += blk->phase_inc;
    if (phase >= 1.0f) phase -= 1.0f;
    state->phase[slot] = phase;
    FX_FINISH(dry_gain, wet_gain);
}

static sf_fx_frame_t fx_run_flanger(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    unsigned pos = state->pos[slot];
    if (state->clear_pos[slot] < (unsigned)SF_DELAY_SIZE) {
        /* Waiting on the incremental clear: dry. */
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
        state->pos[slot] = (pos + 1u) & (SF_DELAY_SIZE - 1u);
        sf_fx_frame_t dry = {input_l, input_r};
        return dry;
    }
    const sf_fx_block_t *blk = &state->blk[slot];
    const float dry_gain = blk->dry, wet_gain = blk->wet;
    float wet_l = input_l;
    float wet_r = input_r;
    /* 0 Default, 1 Default Sync, 2 Silly, 3 Silly Sync. A short line
     * swept between 0.22 and 1.28 ms; Intensity is the feedback. */
    float depth_ms = blk->depth_ms[0];
    float fb = blk->fb;
    float lfo = fx_sin_cycle(state->phase[slot]);
    /* 0.75 + 0.795 at most: the 10 ms ceiling never acts. */
    float delay_ms_l = fx_at_least(0.75f + depth_ms * lfo, 0.05f);
    float delay_ms_r = fx_at_least(0.75f - depth_ms * lfo, 0.05f);
    wet_l = effect_read_delay(state->delay_l[slot], pos,
                              delay_ms_l * 0.001f * SF_SAMPLE_RATE, 0);
    wet_r = effect_read_delay(state->delay_r[slot], pos,
                              delay_ms_r * 0.001f * SF_SAMPLE_RATE, 0);
    state->delay_l[slot][pos] = sf_clampf(input_l + wet_l * fb, -4.0f, 4.0f);
    state->delay_r[slot][pos] = sf_clampf(input_r + wet_r * fb, -4.0f, 4.0f);
    state->phase[slot] += blk->phase_inc;
    if (state->phase[slot] >= 1.0f) state->phase[slot] -= 1.0f;
    FX_FINISH(dry_gain, wet_gain);
}

static sf_fx_frame_t fx_run_super_unison(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    unsigned pos = state->pos[slot];
    if (state->clear_pos[slot] < (unsigned)SF_DELAY_SIZE) {
        /* Waiting on the incremental clear: dry. */
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
        state->pos[slot] = (pos + 1u) & (SF_DELAY_SIZE - 1u);
        sf_fx_frame_t dry = {input_l, input_r};
        return dry;
    }
    const sf_fx_block_t *blk = &state->blk[slot];
    int subtype = blk->subtype;
    const float dry_gain = blk->dry, wet_gain = blk->wet;
    float wet_l = input_l;
    float wet_r = input_r;
    /*
     * Six detuned copies with independent LFO phases. The preset profiles
     * follow the MiniFreak names: Classic, Ravey, Soli, Slow, Slow Trig,
     * Wide Trig, Mono Trig, Wavy. The Trig presets would ideally reset the
     * copy phases on note-on; that hook lives in the voice layer, so this
     * engine keeps them free-running for now.
     */
    int preset = subtype >= 0 && subtype < 8 ? subtype : 0;
    /* Copy delays, swing and rate: fx_block_update. */
    float copy_gain = 0.55f;
    float depth_ms = blk->depth_ms[0];
    float phase = state->phase[slot];
    /* Every copy's tap first (vectorisable), then the reads. The base
     * plus the swing is at most 16.5 + 15.7 ms, so the 40 ms ceiling of
     * the old clamp never acts. */
    /* Taps 0..5 are the left copies, 6..11 the right. */
    float tap[12];
    float lfo[8];
    for (int k = 0; k < 8; ++k) lfo[k] = fx_sin_cycle(phase + g_unison_offset[k]);
    for (int copy = 0; copy < 6; ++copy) {
        float modulation = lfo[copy];
        float base_ms = blk->base_ms[copy];
        float delay_ms_l = fx_at_least(base_ms + depth_ms * modulation, 1.0f);
        float delay_ms_r = fx_at_least(base_ms - depth_ms * modulation, 1.0f);
        tap[copy] = delay_ms_l * 0.001f * SF_SAMPLE_RATE;
        tap[copy + 6] = delay_ms_r * 0.001f * SF_SAMPLE_RATE;
    }
    /* effect_read_delay's position and fraction, for all twelve. */
    unsigned i0[12];
    float frac[12];
    float pos_f = (float)pos;
    for (int k = 0; k < 12; ++k) {
        float read = pos_f - tap[k];
        read = read < 0.0f ? read + (float)SF_DELAY_SIZE : read;
        i0[k] = (unsigned)read;
        frac[k] = read - truncf(read);
    }
    const float *buf_l = state->delay_l[slot];
    const float *buf_r = state->delay_r[slot];
    wet_l = input_l;
    wet_r = input_r;
    for (int copy = 0; copy < 6; ++copy) {
        unsigned a = i0[copy], b = i0[copy + 6];
        float fa = frac[copy], fb = frac[copy + 6];
        float read_l = buf_l[a] * (1.0f - fa) + buf_l[(a + 1u) & (SF_DELAY_SIZE - 1u)] * fa;
        float read_r = buf_r[b] * (1.0f - fb) + buf_r[(b + 1u) & (SF_DELAY_SIZE - 1u)] * fb;
        wet_l += read_l * (copy_gain / 6.0f);
        wet_r += read_r * (copy_gain / 6.0f);
    }
    if (preset == 6) {
        float mono = (wet_l + wet_r) * 0.5f;
        wet_l = mono;
        wet_r = mono;
    }
    state->delay_l[slot][pos] = input_l;
    state->delay_r[slot][pos] = input_r;
    /* Param 2 sweeps the LPF/HPF color around a neutral center (the
     * filter's mode and coefficient: fx_block_update). */
    if (blk->tone_mode) {
        float alpha = blk->tone_alpha;
        float low_l = state->eq_low_state[slot][0] += alpha
            * (wet_l - state->eq_low_state[slot][0]);
        float low_r = state->eq_low_state[slot][1] += alpha
            * (wet_r - state->eq_low_state[slot][1]);
        if (blk->tone_mode == 1) { wet_l = low_l; wet_r = low_r; }
        else { wet_l -= low_l; wet_r -= low_r; }
    }
    phase += blk->phase_inc;
    if (phase >= 1.0f) phase -= 1.0f;
    state->phase[slot] = phase;
    FX_FINISH(dry_gain, wet_gain);
}

static sf_fx_frame_t fx_run_drive(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    const sf_fx_block_t *blk = &state->blk[slot];
    int subtype = blk->subtype;
    float time_knob = blk->time_knob;
    float intensity = blk->intensity;
    const float dry_gain = blk->dry, wet_gain = blk->wet;
    float wet_l = input_l;
    float wet_r = input_r;
    /* Time is drive: the measured small-signal gain runs from unity to
     * SF_DRIVE_MAX_DB (+20 dB), into a clipper whose ceiling is fixed, so a full knob turns
     * a sine into a square. Intensity is a centred tone control: a
     * 12 dB/oct low-pass closing below the middle, a high-pass opening
     * above it. */
    if (time_knob != state->drive_gain_key[slot]) {
        state->drive_gain_key[slot] = time_knob;
        state->drive_gain[slot] = powf(10.0f, table_lerp(g_drive_x, g_drive_db, 7, time_knob) * 0.05f);
    }
    float drive = state->drive_gain[slot];
    int distortion = subtype >= 0 && subtype < 6 ? subtype : 0;
    const float inv_ceiling = 1.0f / SF_DRIVE_CEILING;
    wet_l = SF_DRIVE_CEILING * effect_distort(input_l * drive * inv_ceiling, distortion);
    wet_r = SF_DRIVE_CEILING * effect_distort(input_r * drive * inv_ceiling, distortion);
    if (fabsf(intensity - state->drive_key[slot]) > 1e-4f) {
        /* Low-pass, bypass, high-pass: the stored state of one is a
         * thump in another, so start clean when the tone filter changes. */
        float key = state->drive_key[slot];
        int was = key < 0.49f ? 0 : (key > 0.51f ? 2 : 1);
        int now = intensity < 0.49f ? 0 : (intensity > 0.51f ? 2 : 1);
        if (was != now)
            memset(state->drive_z[slot], 0, sizeof(state->drive_z[slot]));
        if (intensity < 0.49f)
            biquad_pass(&state->drive_bq[slot], 0, 43.0f * expf(12.54f * intensity));
        else if (intensity > 0.51f)
            biquad_pass(&state->drive_bq[slot], 1,
                        28.0f * powf(27.0f, 4.0f * (intensity - 0.5f)));
        state->drive_key[slot] = intensity;
    }
    if (intensity < 0.49f || intensity > 0.51f) {
        wet_l = biquad_run(&state->drive_bq[slot], state->drive_z[slot][0], wet_l);
        wet_r = biquad_run(&state->drive_bq[slot], state->drive_z[slot][1], wet_r);
    }
    FX_MIX(dry_gain, wet_gain);
}

static sf_fx_frame_t fx_run_crush(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    const sf_fx_block_t *blk = &state->blk[slot];
    float time_knob = blk->time_knob;
    float intensity = blk->intensity;
    const float dry_gain = blk->dry, wet_gain = blk->wet;
    float wet_l = input_l;
    float wet_r = input_r;
    /* Time lowers the sample rate (a fractional hold clock, from 44.1 kHz
     * down to 551 Hz); Intensity removes bits, quickly: past about a
     * quarter of the knob a voice is down to one bit, a square. The
     * quantiser is mid-rise so one bit still has two levels. Digital
     * silence stays silent: mid-rise has no zero level, and it used to turn
     * an idle synth into a constant half-step offset (-39 dBFS by default). */
    if (state->crush_key_time[slot] != time_knob) {
        state->crush_key_time[slot] = time_knob;
        state->crush_step[slot] = 1.0f / exp2f(table_lerp(g_crush_x, g_crush_log2, 11, time_knob));
    }
    if (state->crush_key_int[slot] != intensity) {
        state->crush_key_int[slot] = intensity;
        float bits = sf_clampf(16.0f - SF_CRUSH_BITS_SLOPE * intensity, SF_CRUSH_MIN_BITS, 16.0f);
        state->crush_levels[slot] = exp2f(bits - 1.0f);
    }
    float levels = state->crush_levels[slot];
    state->crush_phase[slot] += state->crush_step[slot];
    if (state->crush_phase[slot] >= 1.0f) {
        state->crush_phase[slot] -= 1.0f;
        if (state->crush_phase[slot] >= 1.0f) state->crush_phase[slot] = 0.0f;
        state->crush_l[slot] = input_l == 0.0f ? 0.0f : (floorf(input_l * levels) + 0.5f) / levels;
        state->crush_r[slot] = input_r == 0.0f ? 0.0f : (floorf(input_r * levels) + 0.5f) / levels;
    }
    wet_l = state->crush_l[slot];
    wet_r = state->crush_r[slot];
    FX_MIX(dry_gain, wet_gain);
}

static sf_fx_frame_t fx_run_phaser(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    const sf_fx_block_t *blk = &state->blk[slot];
    int subtype = blk->subtype;
    float tempo = blk->tempo;
    float time_knob = blk->time_knob;
    float intensity = blk->intensity;
    const float dry_gain = blk->dry, wet_gain = blk->wet;
    float wet_l = input_l;
    float wet_r = input_r;
    /* 0 Default, 1 Default Sync, 2 Space, 3 Space Sync, 4 SnH, 5 SnH Sync.
     * Six identical first-order all-passes with output feedback. The
     * corner sweeps 275..1480 Hz (Space: 630..8000 Hz) on a triangle that
     * is linear in octaves; SnH jumps to a new random corner eight times
     * per cycle. */
    int sync = subtype == 1 || subtype == 3 || subtype == 5;
    int space = subtype == 2 || subtype == 3;
    int sample_hold = subtype == 4 || subtype == 5;
    /* The phase step only moves with the tempo (Sync) or the Time knob. */
    float rate_key = sync ? tempo : time_knob;
    if (sync != state->phaser_key_sync[slot] || !(rate_key == state->phaser_key_rate[slot])) {
        float rate = sync
            ? (sf_clampf(tempo, 20.0f, 300.0f) / 60.0f) / 4.0f
            : knob_rate(state, slot, RATE_PHASER, time_knob);
        state->phaser_key_sync[slot] = sync;
        state->phaser_key_rate[slot] = rate_key;
        state->phaser_inc[slot] = rate / SF_SAMPLE_RATE;
    }
    float inc = state->phaser_inc[slot];
    float lo = space ? 630.0f : 275.0f;
    /* log2(hi / lo): 1480 / 275 and 8000 / 630. */
    float octaves = space ? 3.6665927f : 2.4280887f;
    float coefficient;
    if (sample_hold) {
        int step = (int)(state->phase[slot] * 8.0f);
        if (step != state->phaser_step[slot]) {
            state->phaser_rng[slot] = state->phaser_rng[slot] * 1664525u + 1013904223u;
            state->phaser_hold[slot] = (float)(state->phaser_rng[slot] >> 8) / 16777216.0f;
            state->phaser_step[slot] = step;
        }
        float sweep = state->phaser_hold[slot];
        /* The corner only moves with a new step (or a Space switch). */
        if (sweep != state->phaser_key_sweep[slot] || space != state->phaser_key_space[slot]) {
            state->phaser_key_sweep[slot] = sweep;
            state->phaser_key_space[slot] = space;
            state->phaser_coef[slot] = phaser_coefficient(lo, octaves, sweep);
        }
        coefficient = state->phaser_coef[slot];
    } else {
        coefficient = phaser_coefficient(lo, octaves, 0.5f + 0.5f * lfo_triangle(state->phase[slot]));
    }
    if (intensity != state->phaser_key_int[slot]) {
        state->phaser_key_int[slot] = intensity;
        state->phaser_fb_amount[slot] = table_lerp(g_phaser_fb_x, g_phaser_fb, 6, intensity);
    }
    float fb = state->phaser_fb_amount[slot];
    float x_l = input_l + fb * state->phaser_fb[slot][0];
    float x_r = input_r + fb * state->phaser_fb[slot][1];
    for (int stage = 0; stage < SF_PHASER_STAGES; ++stage) {
        float y_l = coefficient * x_l + state->phaser_l[slot][stage];
        float y_r = coefficient * x_r + state->phaser_r[slot][stage];
        state->phaser_l[slot][stage] = x_l - coefficient * y_l;
        state->phaser_r[slot][stage] = x_r - coefficient * y_r;
        x_l = y_l;
        x_r = y_r;
    }
    state->phaser_fb[slot][0] = sf_clampf(x_l, -4.0f, 4.0f);
    state->phaser_fb[slot][1] = sf_clampf(x_r, -4.0f, 4.0f);
    wet_l = x_l;
    wet_r = x_r;
    state->phase[slot] += inc;
    if (state->phase[slot] >= 1.0f) state->phase[slot] -= 1.0f;
    FX_MIX(dry_gain, wet_gain);
}

static sf_fx_frame_t fx_run_eq(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    float dry_gain, wet_gain;
    float wet_l = input_l;
    float wet_r = input_r;
    /* 0 Default, 1 Wide, 2 Mid 1K: low shelf, bell and high shelf, each
     * -15..+6 dB. Coefficients follow the smoothed band gains. */
    for (int band = 0; band < 3; ++band) {
        /* A local copy: the stores to eq_z would otherwise force the
         * coefficients to be reloaded for the right channel. */
        const sf_biquad_t c = state->eq_bq[slot][band];
        wet_l = biquad_run(&c, state->eq_z[slot][band][0], wet_l);
        wet_r = biquad_run(&c, state->eq_z[slot][band][1], wet_r);
    }
    dry_gain = 0.0f;
    wet_gain = 1.0f;
    FX_MIX(dry_gain, wet_gain);
}

static sf_fx_frame_t fx_run_peak_eq(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    float dry_gain, wet_gain;
    float wet_l = input_l;
    float wet_r = input_r;
    const float b0 = state->peak_b0[slot], b1 = state->peak_b1[slot];
    const float b2 = state->peak_b2[slot], a1 = state->peak_a1[slot];
    const float a2 = state->peak_a2[slot];
    float input[2] = {input_l, input_r};
    float result[2];
    for (int channel = 0; channel < 2; ++channel) {
        float x = input[channel];
        float y = b0 * x
                + b1 * state->peak_x1[slot][channel]
                + b2 * state->peak_x2[slot][channel]
                - a1 * state->peak_y1[slot][channel]
                - a2 * state->peak_y2[slot][channel];
        state->peak_x2[slot][channel] = state->peak_x1[slot][channel];
        state->peak_x1[slot][channel] = x;
        state->peak_y2[slot][channel] = state->peak_y1[slot][channel];
        state->peak_y1[slot][channel] = y;
        result[channel] = y;
    }
    wet_l = result[0];
    wet_r = result[1];
    dry_gain = 0.0f;
    wet_gain = 1.0f;
    FX_MIX(dry_gain, wet_gain);
}

static sf_fx_frame_t fx_run_multicomp(sf_effect_state_t *state, int slot, float input_l, float input_r) {
    const sf_fx_block_t *blk = &state->blk[slot];
    int subtype = blk->subtype;
    float mix = blk->mix;
    float time_knob = blk->time_knob;
    float intensity = blk->intensity;
    float dry_gain, wet_gain;
    float wet_l = input_l;
    float wet_r = input_r;
    /*
     * Three bands (12 dB/oct splits at SF_COMP_SPLIT_LOW/HIGH) with the MiniFreak's
     * static curve: quiet material is lifted up to +13 dB at about -45
     * dBFS per band, louder material is compressed 2:1 back down, and the
     * lift fades out towards silence so the noise floor is not pumped up.
     * Amount scales the whole curve in dB. Intensity tilts the low and
     * high bands against each other by up to +/-6 dB, Time sets the
     * detector speed. Presets: OPP, Bass Ctrl, High Ctrl, All Up, Tighter.
     */
    if (!state->comp_split_ready[slot]) {
        biquad_pass(&state->comp_split[slot][0], 0, SF_COMP_SPLIT_LOW);
        biquad_pass(&state->comp_split[slot][1], 1, SF_COMP_SPLIT_HIGH);
        state->comp_split_ready[slot] = 1u;
    }
    float amount = sf_clampf(mix, 0.0f, 1.0f);
    if (time_knob != state->comp_key_time[slot]) {
        float attack_ms = 2.0f + time_knob * 20.0f;
        float release_ms = 40.0f + time_knob * 300.0f;
        state->comp_key_time[slot] = time_knob;
        state->comp_attack[slot] = 1.0f / (1.0f + attack_ms * 0.001f * SF_SAMPLE_RATE);
        state->comp_release[slot] = 1.0f / (1.0f + release_ms * 0.001f * SF_SAMPLE_RATE);
    }
    float attack_alpha = state->comp_attack[slot];
    float release_alpha = state->comp_release[slot];
    float input[2] = {input_l, input_r};
    float bands[2][SF_FX_COMP_BANDS];
    for (int channel = 0; channel < 2; ++channel) {
        float low = biquad_run(&state->comp_split[slot][0], state->comp_z[slot][channel][0],
                               input[channel]);
        float high = biquad_run(&state->comp_split[slot][1], state->comp_z[slot][channel][1],
                                input[channel]);
        /* Mid is the remainder, so the three bands always sum back to
         * the input when no gain is applied. */
        bands[channel][0] = low;
        bands[channel][1] = input[channel] - low - high;
        bands[channel][2] = high;
        for (int band = 0; band < SF_FX_COMP_BANDS; ++band) {
            float power = bands[channel][band] * bands[channel][band];
            float *envelope = &state->comp_env[slot][channel][band];
            float coefficient = power > *envelope ? attack_alpha : release_alpha;
            *envelope += coefficient * (power - *envelope);
        }
    }
    /* The gains follow the detectors every eighth sample, from the
     * envelopes just updated. */
    if ((state->comp_tick[slot]++ & 7u) == 0u)
        comp_update_gains(state, slot, subtype, amount, intensity);
    float result[2];
    for (int channel = 0; channel < 2; ++channel) {
        float sum = 0.0f;
        for (int band = 0; band < SF_FX_COMP_BANDS; ++band)
            sum += bands[channel][band] * state->comp_gain[slot][channel][band];
        result[channel] = sum;
    }
    wet_l = result[0];
    wet_r = result[1];
    dry_gain = 0.0f;
    wet_gain = 1.0f;
    FX_MIX(dry_gain, wet_gain);
}

/* The 3-band EQ's coefficients follow the smoothed band gains, which move
 * once per block. */
static void eq_prepare(sf_effect_state_t *state, int slot, int subtype) {
    int layout = subtype >= 0 && subtype < 3 ? subtype : 0;
    for (int band = 0; band < 3; ++band) {
        float db = state->eq_smooth_db[slot][band];
        if (layout != state->eq_key_sub[slot]
            || fabsf(db - state->eq_key_db[slot][band]) > 0.01f) {
            biquad_eq(&state->eq_bq[slot][band], band,
                      g_eq_layout[layout][band][0], db, g_eq_layout[layout][band][1]);
            state->eq_key_db[slot][band] = db;
        }
    }
    state->eq_key_sub[slot] = layout;
}

/* The Peak EQ's coefficients, rebuilt only when the smoothed controls moved
 * and only for a slot that runs it. */
static void peak_prepare(sf_effect_state_t *state, int slot) {
    if (state->peak_frequency_smooth[slot] != state->peak_key[slot][0]
        || state->peak_gain_smooth[slot] != state->peak_key[slot][1]
        || state->peak_width_smooth[slot] != state->peak_key[slot][2]) {
        update_peak_coefficients(state, slot);
        state->peak_key[slot][0] = state->peak_frequency_smooth[slot];
        state->peak_key[slot][1] = state->peak_gain_smooth[slot];
        state->peak_key[slot][2] = state->peak_width_smooth[slot];
    }
}

/* What `frames` samples of an effect that never reads the ring leave in it:
 * zeros from pos on (so a later Delay starts clean), and pos moved on. */
static void ring_zero_block(sf_effect_state_t *state, int slot, int frames) {
    unsigned pos = state->pos[slot];
    unsigned n = frames > 0 ? (unsigned)frames : 0u;
    unsigned first = (unsigned)SF_DELAY_SIZE - pos;
    if (first > n) first = n;
    memset(&state->delay_l[slot][pos], 0, first * sizeof(float));
    memset(&state->delay_r[slot][pos], 0, first * sizeof(float));
    memset(&state->delay_l[slot][0], 0, (n - first) * sizeof(float));
    memset(&state->delay_r[slot][0], 0, (n - first) * sizeof(float));
    state->pos[slot] = (pos + n) & (SF_DELAY_SIZE - 1u);
}

sf_effect_fn sf_effect_prepare(sf_effect_state_t *state, int slot, int type, int subtype,
                               float mix, float time_ms, float feedback, float tempo,
                               int frames) {
    sf_fx_block_t *blk = &state->blk[slot];
    blk->subtype = subtype;
    blk->mix = mix;
    blk->time_ms = time_ms;
    blk->feedback = feedback;
    blk->tempo = tempo;
    /* The knob positions, mix law, delay lengths and LFO rates. */
    fx_block_update(state, slot, type, subtype, mix, time_ms, feedback, tempo);

    /* Delay storage is cleared incrementally after instance creation, every
     * preset load and whenever a slot leaves Reverb, whose lines fill the
     * start of the ring and would otherwise come back as a Delay's or a
     * Chorus's echoes. Only the effects that read the ring wait for it (each
     * checks per sample, since a non-finite reset can restart the clear
     * mid-block). The Reverb clears its own region, which the sweep then
     * skips so it never wipes a live room. */
    if (type != state->last_type[slot]) {
        if (state->last_type[slot] == FX_REVERB) state->clear_pos[slot] = 0;
        state->last_type[slot] = type;
    }
    if (type == FX_REVERB && state->clear_pos[slot] < SF_REV_REGION_END)
        state->clear_pos[slot] = SF_REV_REGION_END;

    /* Running anything other than the reverb re-arms its one-shot region clear,
     * so switching into it never reads a delay tail back as a room. That
     * includes a Delay or Chorus still waiting on the clear: skipping those
     * let a quick Reverb -> Delay -> Reverb resume the old room's lines. A
     * non-finite reset mid-block disarms it by itself. */
    if (type != FX_REVERB) {
        state->rev_ready[slot] = 0u;
        state->rev_clear[slot] = 0u;
    }

    switch (type) {
    case FX_DELAY: return fx_run_delay;
    case FX_REVERB: return fx_run_reverb;
    case FX_CHORUS: return fx_run_chorus;
    case FX_FLANGER: return fx_run_flanger;
    case FX_SUPER_UNISON: return fx_run_super_unison;
    case FX_DRIVE: ring_zero_block(state, slot, frames); return fx_run_drive;
    case FX_CRUSH: ring_zero_block(state, slot, frames); return fx_run_crush;
    case FX_PHASER: ring_zero_block(state, slot, frames); return fx_run_phaser;
    case FX_EQ:
        eq_prepare(state, slot, subtype);
        ring_zero_block(state, slot, frames);
        return fx_run_eq;
    case FX_PEAK_EQ:
        peak_prepare(state, slot);
        ring_zero_block(state, slot, frames);
        return fx_run_peak_eq;
    case FX_MULTICOMP: ring_zero_block(state, slot, frames); return fx_run_multicomp;
    default: break;
    }
    /* Off: the slot passes its input through, and its share of the ring is
     * zeroed as it goes so a later Delay starts clean. That is the same as
     * `frames` samples of the old per-sample path, done here at once. */
    ring_zero_block(state, slot, frames);
    return 0;
}

void sf_effect_process(sf_effect_state_t *state, int slot, int type, int subtype,
                       float mix, float time_ms, float feedback, float tempo,
                       float input_l, float input_r,
                       float *output_l, float *output_r) {
    sf_effect_fn fn = sf_effect_prepare(state, slot, type, subtype, mix, time_ms,
                                        feedback, tempo, 1);
    if (fn) {
        sf_fx_frame_t o = fn(state, slot, input_l, input_r);
        *output_l = o.l;
        *output_r = o.r;
    } else {
        *output_l = input_l * 1.0f + input_l * 0.0f;
        *output_r = input_r * 1.0f + input_r * 0.0f;
    }
}
