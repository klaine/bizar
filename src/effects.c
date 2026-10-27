#include "effects.h"
#include "dsp_common.h"

#include <math.h>
#include <string.h>

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
static float effect_read_delay(const float *buf, unsigned pos, float delay_samples,
                               int hermite) {
    if (!(delay_samples >= 1.0f)) delay_samples = 1.0f;
    if (delay_samples > (float)(SF_DELAY_SIZE - 4))
        delay_samples = (float)(SF_DELAY_SIZE - 4);
    float read = (float)pos - delay_samples;
    if (read < 0.0f) read += (float)SF_DELAY_SIZE;
    unsigned i0 = (unsigned)read;
    float frac = read - (float)i0;
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
    x = fmodf(x + 1.0f, 4.0f);
    if (x < 0.0f) x += 4.0f;
    return x < 2.0f ? x - 1.0f : 3.0f - x;
}

/* The six MiniFreak Distortion presets. They deliberately react to gain in
 * different ways instead of all sharing one tanh curve. Each maps a signal
 * already scaled to the clip ceiling (1.0 = ceiling). */
static float effect_distort(float x, int preset) {
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
static const unsigned rev_comb_base[SF_REV_COMBS] = {0u, 1580u, 3220u, 4734u};
static const unsigned rev_ap_len[SF_REV_APS] = {225u, 556u, 441u, 341u};
static const unsigned rev_ap_base[SF_REV_APS] = {6179u, 6427u, 7006u, 7470u};
#define SF_REV_STEREO_SPREAD 23u
#define SF_REV_PRE_BASE 7834u
#define SF_REV_PRE_SIZE 4096u
#define SF_REV_REGION_END (SF_REV_PRE_BASE + SF_REV_PRE_SIZE)
/* Samples of the region cleared per sample while the reverb is coming up.
 * 11930 over 256 is 47 samples (about 1.1 ms at 44.1 kHz), and the slot stays
 * dry until it finishes so nothing stale is ever audible. */
#define SF_REV_CLEAR_CHUNK 256u

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
        state->rev_gain[slot][c] = g;
        state->rev_damp[slot][c] = b;
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
        state->comp_split_ready[slot] = 0u;
        state->delay_bq_ready[slot] = 0u;
        state->phaser_hold[slot] = 0.5f;
        state->phaser_step[slot] = -1;
        state->phaser_rng[slot] = 0x9E3779B9u * (unsigned)(slot + 1);
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
        for (int band = 0; band < 3; ++band) {
            float db = eq_db ? eq_db[slot][band] : 0.0f;
            state->eq_smooth_db[slot][band] = db;
            state->eq_key_db[slot][band] = 1000.0f;
        }
        update_peak_coefficients(state, slot);
    }
}

void sf_effect_state_smooth(sf_effect_state_t *state,
                            const float mix[SF_FX_SLOTS],
                            const float eq_db[SF_FX_SLOTS][3],
                            const float peak_frequency[SF_FX_SLOTS],
                            const float peak_gain[SF_FX_SLOTS],
                            const float peak_width[SF_FX_SLOTS], float amount) {
    for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
        state->mix_smooth[slot] += amount * (mix[slot] - state->mix_smooth[slot]);
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
        update_peak_coefficients(state, slot);
    }
}

void sf_effect_clear_buffers(sf_effect_state_t *state) {
    for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
        unsigned begin = state->clear_pos[slot];
        unsigned end = begin + SF_FX_CLEAR_CHUNK;
        if (end > (unsigned)SF_DELAY_SIZE) end = (unsigned)SF_DELAY_SIZE;
        for (unsigned i = begin; i < end; ++i) {
            state->delay_l[slot][i] = 0.0f;
            state->delay_r[slot][i] = 0.0f;
        }
        state->clear_pos[slot] = end;
    }
}

void sf_effect_reset_nonfinite(sf_effect_state_t *state) {
    memset(state->delay_l, 0, sizeof(state->delay_l));
    memset(state->delay_r, 0, sizeof(state->delay_r));
    for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
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

static float lfo_triangle(float phase) {
    return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
}

void sf_effect_process(sf_effect_state_t *state, int slot, int type, int subtype,
                       float mix, float time_ms, float feedback, float tempo,
                       float input_l, float input_r,
                       float *output_l, float *output_r) {
    unsigned pos = state->pos[slot];
    float dry_gain = 1.0f;
    float wet_gain = 0.0f;
    float wet_l = input_l;
    float wet_r = input_r;
    /* The MiniFreak's knob positions; see "Knob curves" above. */
    float time_knob = sf_clampf((time_ms - 1.0f) / (SF_FX_TIME_MAX - 1.0f), 0.0f, 1.0f);
    float intensity = sf_clampf(feedback / 0.95f, 0.0f, 1.0f);

    /* Delay storage is cleared incrementally after instance creation. */
    if (state->clear_pos[slot] < (unsigned)SF_DELAY_SIZE) {
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
        state->pos[slot] = (pos + 1u) & (SF_DELAY_SIZE - 1u);
        *output_l = input_l;
        *output_r = input_r;
        return;
    }

    /* Running anything other than the reverb re-arms its one-shot region clear,
     * so switching into it never reads a delay tail back as a room. Rewinding
     * every non-reverb sample is harmless; the important case is a slot that
     * was switched away from Reverb while its clear was still in progress. */
    if (type != FX_REVERB) {
        state->rev_ready[slot] = 0u;
        state->rev_clear[slot] = 0u;
    }

    mix_law(state, slot, type, mix, &dry_gain, &wet_gain);

    if (type == FX_DELAY) {
        /*
         * Seven delay characters, each available in a free-running and a
         * tempo-synced form:
         *   0 Digital, 1 Stereo, 2 Ping-Pong, 3 Mono,
         *   4 Filtered, 5 Filtered Ping-Pong, 6 Tape
         * The Sync variants are 7..13 and use the Time knob as a subdivision
         * selector instead of milliseconds.
         */
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
        delay_l_samples = sf_clampf(delay_l_samples, 1.0f,
                                        (float)(SF_DELAY_SIZE - 4));
        delay_r_samples = sf_clampf(delay_r_samples, 1.0f,
                                        (float)(SF_DELAY_SIZE - 4));

        int hermite = mode == 6;
        wet_l = effect_read_delay(state->delay_l[slot], pos, delay_l_samples, hermite);
        wet_r = effect_read_delay(state->delay_r[slot], pos, delay_r_samples, hermite);

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
        static const float mode_gain[7] = {1.0f, 0.9f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        wet_l *= SF_DELAY_WET * mode_gain[mode];
        wet_r *= SF_DELAY_WET * mode_gain[mode];
    } else if (type == FX_REVERB) {
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
            *output_l = input_l;
            *output_r = input_r;
            return;
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
            unsigned pre = (unsigned)(p->predelay_ms * 0.001f * SF_SAMPLE_RATE);
            if (pre > SF_REV_PRE_SIZE - 1u) pre = SF_REV_PRE_SIZE - 1u;
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
        float diff_l = pre_l * 0.30f;
        float diff_r = pre_r * 0.30f;
        for (int a = 0; a < SF_REV_APS; ++a) {
            unsigned base = rev_ap_base[a];
            for (int ch = 0; ch < 2; ++ch) {
                unsigned len = rev_ap_len[a] + (ch ? SF_REV_STEREO_SPREAD : 0u);
                float *buf = ch ? state->delay_r[slot] : state->delay_l[slot];
                unsigned idx = state->rev_ap_pos[slot][a][ch];
                float x = ch ? diff_r : diff_l;
                float stored = buf[base + idx];
                float out = -p->diffusion * x + stored;
                buf[base + idx] = sf_clampf(x + p->diffusion * stored, -4.0f, 4.0f);
                state->rev_ap_pos[slot][a][ch] = idx + 1u < len ? idx + 1u : 0u;
                if (ch) diff_r = out; else diff_l = out;
            }
        }

        float mod_sin = sinf(SF_TWO_PI * state->rev_mod_phase[slot]);
        float mod_cos = cosf(SF_TWO_PI * state->rev_mod_phase[slot]);
        float line_out_l[SF_REV_COMBS];
        float line_out_r[SF_REV_COMBS];
        for (int c = 0; c < SF_REV_COMBS; ++c) {
            unsigned base = rev_comb_base[c];
            float damp = state->rev_damp[slot][c];
            for (int ch = 0; ch < 2; ++ch) {
                unsigned len = reverb_line_len(c, p->line_scale, ch);
                float *buf = ch ? state->delay_r[slot] : state->delay_l[slot];
                unsigned idx = state->rev_comb_pos[slot][c][ch];
                if (idx >= len) idx %= len;
                state->rev_comb_pos[slot][c][ch] = idx;
                float mod = p->mod_depth
                    * (mod_sin * state->rev_mod_sin[slot][c][ch]
                       + mod_cos * state->rev_mod_cos[slot][c][ch]);
                /* idx is the oldest sample (about to be overwritten); the
                 * slots just after it are the next oldest. The tap swings
                 * inside that region. Reading behind idx would land on
                 * samples written a moment ago and short-circuit the line. */
                float read = (float)idx + p->mod_depth + 1.5f + mod;
                if (read < 0.0f) read += (float)len;
                if (read >= (float)len) read -= (float)len;
                unsigned i0 = (unsigned)read;
                float frac = read - (float)i0;
                /* Four-point Hermite: linear interpolation would take ~1 dB
                 * off 6 kHz on every pass and the top of a long tail would
                 * die several times faster than the measured damping. */
                /* i0 < len (the tap swing is a few samples on a line of
                 * hundreds), so the neighbours wrap by one subtraction
                 * rather than a division each. */
                unsigned im1 = i0 ? i0 - 1u : len - 1u;
                unsigned i1 = i0 + 1u < len ? i0 + 1u : i0 + 1u - len;
                unsigned i2 = i0 + 2u < len ? i0 + 2u : i0 + 2u - len;
                float xm1 = buf[base + im1];
                float x0 = buf[base + i0];
                float x1 = buf[base + i1];
                float x2 = buf[base + i2];
                float c1 = 0.5f * (x1 - xm1);
                float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
                float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                float out = ((c3 * frac + c2) * frac + c1) * frac + x0;
                float *low = &state->rev_comb_low[slot][c][ch];
                *low = out * (1.0f - damp) + *low * damp;
                if (ch) line_out_r[c] = *low; else line_out_l[c] = *low;
            }
        }

        /* Normalised 4x4 Hadamard feedback matrix. */
        float h_l[SF_REV_COMBS] = {
            (line_out_l[0] + line_out_l[1] + line_out_l[2] + line_out_l[3]) * 0.5f,
            (line_out_l[0] - line_out_l[1] + line_out_l[2] - line_out_l[3]) * 0.5f,
            (line_out_l[0] + line_out_l[1] - line_out_l[2] - line_out_l[3]) * 0.5f,
            (line_out_l[0] - line_out_l[1] - line_out_l[2] + line_out_l[3]) * 0.5f
        };
        float h_r[SF_REV_COMBS] = {
            (line_out_r[0] + line_out_r[1] + line_out_r[2] + line_out_r[3]) * 0.5f,
            (line_out_r[0] - line_out_r[1] + line_out_r[2] - line_out_r[3]) * 0.5f,
            (line_out_r[0] + line_out_r[1] - line_out_r[2] - line_out_r[3]) * 0.5f,
            (line_out_r[0] - line_out_r[1] - line_out_r[2] + line_out_r[3]) * 0.5f
        };
        for (int c = 0; c < SF_REV_COMBS; ++c) {
            unsigned base = rev_comb_base[c];
            float g = state->rev_gain[slot][c];
            float feed_l = ((c & 1) ? -diff_r : diff_l) * 0.5f;
            float feed_r = ((c & 1) ? -diff_l : diff_r) * 0.5f;
            float next_l = feed_l + g * h_l[c];
            float next_r = feed_r + g * h_r[c];
            for (int ch = 0; ch < 2; ++ch) {
                unsigned len = reverb_line_len(c, p->line_scale, ch);
                float *buf = ch ? state->delay_r[slot] : state->delay_l[slot];
                unsigned idx = state->rev_comb_pos[slot][c][ch];
                if (idx >= len) idx %= len;
                buf[base + idx] = sf_clampf(ch ? next_r : next_l, -4.0f, 4.0f);
                state->rev_comb_pos[slot][c][ch] = idx + 1u < len ? idx + 1u : 0u;
            }
        }

        float sum_l = (line_out_l[0] + line_out_l[1]
                     + line_out_l[2] + line_out_l[3]) * 0.25f;
        float sum_r = (line_out_r[0] + line_out_r[1]
                     + line_out_r[2] + line_out_r[3]) * 0.25f;
        if (p->early_gain > 0.0f) {
            sum_l += diff_l * p->early_gain;
            sum_r += diff_r * p->early_gain;
        }
        wet_l = sum_l * state->rev_level[slot];
        wet_r = sum_r * state->rev_level[slot];
        state->rev_mod_phase[slot] += 0.12f / SF_SAMPLE_RATE;
        if (state->rev_mod_phase[slot] >= 1.0f) state->rev_mod_phase[slot] -= 1.0f;
    } else if (type == FX_CHORUS) {
        /*
         * 0 Default, 1 Lush, 2 Dark, 3 Shaded, 4 Single. Default is the
         * MiniFreak's pair of voices sitting at 13 and 18 ms and swinging in
         * opposite directions; Intensity widens the swing (the lower voice
         * more), Time sets the rate. Lush adds a third voice and Single keeps
         * one.
         */
        float rate = table_lerp(g_knob5, g_chorus_rate, 5, time_knob);
        static const float base_ms[3][3] = {{13.0f, 18.0f, 0.0f},
                                            {11.0f, 15.0f, 19.0f},
                                            {15.0f, 0.0f, 0.0f}};
        static const float depth_ms[3][3] = {{11.5f, 7.0f, 0.0f},
                                             {11.0f, 9.0f, 7.0f},
                                             {9.0f, 0.0f, 0.0f}};
        int layout = subtype == 1 ? 1 : (subtype == 4 ? 2 : 0);
        int voices = layout == 1 ? 3 : (layout == 2 ? 1 : 2);
        float voice_gain = layout == 1 ? 0.33f : (layout == 2 ? 0.6f : 0.4f);
        float depth_scale = intensity;
        if (subtype == 1) rate *= 0.80f;
        else if (subtype == 2) { rate *= 0.65f; depth_scale *= 1.15f; }
        else if (subtype == 3) { rate *= 0.50f; depth_scale *= 0.80f; }
        float phase = state->phase[slot];
        wet_l = 0.0f;
        wet_r = 0.0f;
        for (int v = 0; v < voices; ++v) {
            float offset = (float)v / (float)voices;
            float lfo_l = sinf(SF_TWO_PI * (phase + offset));
            float lfo_r = sinf(SF_TWO_PI * (phase + offset + 0.25f));
            float d = depth_ms[layout][v] * depth_scale;
            float delay_ms_l = sf_clampf(base_ms[layout][v] + d * lfo_l, 0.5f, 40.0f);
            float delay_ms_r = sf_clampf(base_ms[layout][v] + d * lfo_r, 0.5f, 40.0f);
            wet_l += effect_read_delay(state->delay_l[slot], pos,
                                       delay_ms_l * 0.001f * SF_SAMPLE_RATE, 0);
            wet_r += effect_read_delay(state->delay_r[slot], pos,
                                       delay_ms_r * 0.001f * SF_SAMPLE_RATE, 0);
        }
        wet_l *= voice_gain;
        wet_r *= voice_gain;
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
        phase += rate / SF_SAMPLE_RATE;
        if (phase >= 1.0f) phase -= 1.0f;
        state->phase[slot] = phase;
    } else if (type == FX_FLANGER) {
        /* 0 Default, 1 Default Sync, 2 Silly, 3 Silly Sync. A short line
         * swept between 0.22 and 1.28 ms; Intensity is the feedback. */
        int silly = subtype == 2 || subtype == 3;
        int sync = subtype == 1 || subtype == 3;
        float rate = sync
            ? (sf_clampf(tempo, 20.0f, 300.0f) / 60.0f) / 2.0f
            : table_log(g_knob5, g_flanger_rate, 5, time_knob);
        float depth_ms = 0.53f;
        if (silly) { rate *= 1.75f; depth_ms *= 1.5f; }
        float fb = 0.9f * intensity;
        float lfo = sinf(SF_TWO_PI * state->phase[slot]);
        float delay_ms_l = sf_clampf(0.75f + depth_ms * lfo, 0.05f, 10.0f);
        float delay_ms_r = sf_clampf(0.75f - depth_ms * lfo, 0.05f, 10.0f);
        wet_l = effect_read_delay(state->delay_l[slot], pos,
                                  delay_ms_l * 0.001f * SF_SAMPLE_RATE, 0);
        wet_r = effect_read_delay(state->delay_r[slot], pos,
                                  delay_ms_r * 0.001f * SF_SAMPLE_RATE, 0);
        state->delay_l[slot][pos] = sf_clampf(input_l + wet_l * fb, -4.0f, 4.0f);
        state->delay_r[slot][pos] = sf_clampf(input_r + wet_r * fb, -4.0f, 4.0f);
        state->phase[slot] += rate / SF_SAMPLE_RATE;
        if (state->phase[slot] >= 1.0f) state->phase[slot] -= 1.0f;
    } else if (type == FX_SUPER_UNISON) {
        /*
         * Six detuned copies with independent LFO phases. The preset profiles
         * follow the MiniFreak names: Classic, Ravey, Soli, Slow, Slow Trig,
         * Wide Trig, Mono Trig, Wavy. The Trig presets would ideally reset the
         * copy phases on note-on; that hook lives in the voice layer, so this
         * engine keeps them free-running for now.
         */
        int preset = subtype >= 0 && subtype < 8 ? subtype : 0;
        float rate_scale = 1.0f;
        float width_style = 1.0f;
        float spread_ms = 1.8f;
        float copy_gain = 0.55f;
        switch (preset) {
        case 1: /* Ravey */
            rate_scale = 1.8f;
            width_style = 1.3f;
            spread_ms = 2.2f;
            break;
        case 2: /* Soli */
            rate_scale = 0.8f;
            width_style = 0.9f;
            spread_ms = 1.5f;
            break;
        case 3: /* Slow */
        case 4: /* Slow Trig */
            rate_scale = 0.35f;
            width_style = 0.8f;
            spread_ms = 2.0f;
            break;
        case 5: /* Wide Trig */
            rate_scale = 1.2f;
            width_style = 1.7f;
            spread_ms = 2.5f;
            break;
        case 6: /* Mono Trig */
            rate_scale = 1.0f;
            width_style = 0.05f;
            spread_ms = 1.8f;
            break;
        case 7: /* Wavy */
            rate_scale = 0.9f;
            width_style = 1.0f;
            spread_ms = 2.0f;
            break;
        default: /* Classic */
            break;
        }
        float detune = time_knob;
        float rate = (0.06f + detune * 0.65f) * rate_scale;
        float depth_ms = (0.25f + detune * 9.0f) * width_style;
        float phase = state->phase[slot];
        wet_l = input_l;
        wet_r = input_r;
        for (int copy = 0; copy < 6; ++copy) {
            float offset = (float)copy / 6.0f;
            float modulation = sinf(SF_TWO_PI * (phase + offset));
            float base_ms = 4.0f + (float)copy * spread_ms;
            float delay_ms_l = sf_clampf(base_ms + depth_ms * modulation, 1.0f, 40.0f);
            float delay_ms_r = sf_clampf(base_ms - depth_ms * modulation, 1.0f, 40.0f);
            wet_l += effect_read_delay(state->delay_l[slot], pos,
                                       delay_ms_l * 0.001f * SF_SAMPLE_RATE, 0)
                   * (copy_gain / 6.0f);
            wet_r += effect_read_delay(state->delay_r[slot], pos,
                                       delay_ms_r * 0.001f * SF_SAMPLE_RATE, 0)
                   * (copy_gain / 6.0f);
        }
        if (preset == 6) {
            float mono = (wet_l + wet_r) * 0.5f;
            wet_l = mono;
            wet_r = mono;
        }
        state->delay_l[slot][pos] = input_l;
        state->delay_r[slot][pos] = input_r;
        /* Param 2 sweeps the LPF/HPF color around a neutral center. */
        float signed_tone = (intensity - 0.5f) * 2.0f;
        if (fabsf(signed_tone) > 0.015f) {
            if (signed_tone != state->unison_tone_key[slot]) {
                state->unison_tone_key[slot] = signed_tone;
                float cutoff = 20.0f * powf(900.0f, fabsf(signed_tone));
                float g = tanf(SF_PI * sf_clampf(cutoff, 20.0f, 18000.0f)
                               / SF_SAMPLE_RATE);
                state->unison_tone_alpha[slot] = g / (1.0f + g);
            }
            float alpha = state->unison_tone_alpha[slot];
            float low_l = state->eq_low_state[slot][0] += alpha
                * (wet_l - state->eq_low_state[slot][0]);
            float low_r = state->eq_low_state[slot][1] += alpha
                * (wet_r - state->eq_low_state[slot][1]);
            if (signed_tone < 0.0f) { wet_l = low_l; wet_r = low_r; }
            else { wet_l -= low_l; wet_r -= low_r; }
        }
        phase += rate / SF_SAMPLE_RATE;
        if (phase >= 1.0f) phase -= 1.0f;
        state->phase[slot] = phase;
    } else if (type == FX_DRIVE) {
        /* Time is drive: the measured small-signal gain runs from unity to
         * +32 dB, into a clipper whose ceiling is fixed, so a full knob turns
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
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
    } else if (type == FX_CRUSH) {
        /* Time lowers the sample rate (a fractional hold clock, from 44.1 kHz
         * down to 551 Hz); Intensity removes bits, quickly: past about a
         * quarter of the knob a voice is down to one bit, a square. The
         * quantiser is mid-rise so one bit still has two levels. */
        float ratio = exp2f(table_lerp(g_crush_x, g_crush_log2, 11, time_knob));
        float bits = sf_clampf(16.0f - SF_CRUSH_BITS_SLOPE * intensity, SF_CRUSH_MIN_BITS, 16.0f);
        float levels = exp2f(bits - 1.0f);
        state->crush_phase[slot] += 1.0f / ratio;
        if (state->crush_phase[slot] >= 1.0f) {
            state->crush_phase[slot] -= 1.0f;
            if (state->crush_phase[slot] >= 1.0f) state->crush_phase[slot] = 0.0f;
            state->crush_l[slot] = (floorf(input_l * levels) + 0.5f) / levels;
            state->crush_r[slot] = (floorf(input_r * levels) + 0.5f) / levels;
        }
        wet_l = state->crush_l[slot];
        wet_r = state->crush_r[slot];
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
    } else if (type == FX_PHASER) {
        /* 0 Default, 1 Default Sync, 2 Space, 3 Space Sync, 4 SnH, 5 SnH Sync.
         * Six identical first-order all-passes with output feedback. The
         * corner sweeps 275..1480 Hz (Space: 630..8000 Hz) on a triangle that
         * is linear in octaves; SnH jumps to a new random corner eight times
         * per cycle. */
        int sync = subtype == 1 || subtype == 3 || subtype == 5;
        int space = subtype == 2 || subtype == 3;
        int sample_hold = subtype == 4 || subtype == 5;
        float rate = sync
            ? (sf_clampf(tempo, 20.0f, 300.0f) / 60.0f) / 4.0f
            : table_log(g_knob5, g_phaser_rate, 5, time_knob);
        float lo = space ? 630.0f : 275.0f;
        float hi = space ? 8000.0f : 1480.0f;
        float sweep;
        if (sample_hold) {
            int step = (int)(state->phase[slot] * 8.0f);
            if (step != state->phaser_step[slot]) {
                state->phaser_rng[slot] = state->phaser_rng[slot] * 1664525u + 1013904223u;
                state->phaser_hold[slot] = (float)(state->phaser_rng[slot] >> 8) / 16777216.0f;
                state->phaser_step[slot] = step;
            }
            sweep = state->phaser_hold[slot];
        } else {
            sweep = 0.5f + 0.5f * lfo_triangle(state->phase[slot]);
        }
        float corner = lo * powf(hi / lo, sweep);
        float t = tanf(SF_PI * corner / SF_SAMPLE_RATE);
        float coefficient = (t - 1.0f) / (t + 1.0f);
        float fb = table_lerp(g_phaser_fb_x, g_phaser_fb, 6, intensity);
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
        state->phase[slot] += rate / SF_SAMPLE_RATE;
        if (state->phase[slot] >= 1.0f) state->phase[slot] -= 1.0f;
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
    } else if (type == FX_EQ) {
        /* 0 Default, 1 Wide, 2 Mid 1K: low shelf, bell and high shelf, each
         * -15..+6 dB. Coefficients follow the smoothed band gains. */
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
        for (int band = 0; band < 3; ++band) {
            wet_l = biquad_run(&state->eq_bq[slot][band], state->eq_z[slot][band][0], wet_l);
            wet_r = biquad_run(&state->eq_bq[slot][band], state->eq_z[slot][band][1], wet_r);
        }
        dry_gain = 0.0f;
        wet_gain = 1.0f;
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
    } else if (type == FX_PEAK_EQ) {
        float input[2] = {input_l, input_r};
        float result[2];
        for (int channel = 0; channel < 2; ++channel) {
            float x = input[channel];
            float y = state->peak_b0[slot] * x
                    + state->peak_b1[slot] * state->peak_x1[slot][channel]
                    + state->peak_b2[slot] * state->peak_x2[slot][channel]
                    - state->peak_a1[slot] * state->peak_y1[slot][channel]
                    - state->peak_a2[slot] * state->peak_y2[slot][channel];
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
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
    } else if (type == FX_MULTICOMP) {
        /*
         * Three bands (12 dB/oct splits at SF_COMP_SPLIT_LOW/HIGH) with the MiniFreak's
         * static curve: quiet material is lifted up to +13 dB at about -45
         * dBFS per band, louder material is compressed 2:1 back down, and the
         * lift fades out towards silence so the noise floor is not pumped up.
         * Amount scales the whole curve in dB. Intensity tilts the low and
         * high bands against each other by up to +/-6 dB, Time sets the
         * detector speed. Presets: OPP, Bass Ctrl, High Ctrl, All Up, Tighter.
         */
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
        if (!state->comp_split_ready[slot]) {
            biquad_pass(&state->comp_split[slot][0], 0, SF_COMP_SPLIT_LOW);
            biquad_pass(&state->comp_split[slot][1], 1, SF_COMP_SPLIT_HIGH);
            state->comp_split_ready[slot] = 1u;
        }
        float amount = sf_clampf(mix, 0.0f, 1.0f);
        float tilt[SF_FX_COMP_BANDS] = {12.1f * (0.5f - intensity), 0.0f,
                                        12.8f * (intensity - 0.5f)};
        float attack_ms = 2.0f + time_knob * 20.0f;
        float release_ms = 40.0f + time_knob * 300.0f;
        float attack_alpha = 1.0f / (1.0f + attack_ms * 0.001f * SF_SAMPLE_RATE);
        float release_alpha = 1.0f / (1.0f + release_ms * 0.001f * SF_SAMPLE_RATE);
        int update = (state->comp_tick[slot]++ & 7u) == 0u;
        float input[2] = {input_l, input_r};
        float result[2];
        for (int channel = 0; channel < 2; ++channel) {
            float low = biquad_run(&state->comp_split[slot][0], state->comp_z[slot][channel][0],
                                   input[channel]);
            float high = biquad_run(&state->comp_split[slot][1], state->comp_z[slot][channel][1],
                                    input[channel]);
            /* Mid is the remainder, so the three bands always sum back to
             * the input when no gain is applied. */
            float bands[SF_FX_COMP_BANDS] = {low, input[channel] - low - high, high};
            float sum = 0.0f;
            for (int band = 0; band < SF_FX_COMP_BANDS; ++band) {
                float power = bands[band] * bands[band];
                float *envelope = &state->comp_env[slot][channel][band];
                float coefficient = power > *envelope ? attack_alpha : release_alpha;
                *envelope += coefficient * (power - *envelope);
                if (update) {
                    float level = 10.0f * log10f(*envelope + 1e-12f) + band_offset[band];
                    float lift = level < -34.0f
                        ? table_lerp(curve_x, curve_db, 8, level)
                        : 8.4f - 0.5f * (level + 34.0f);
                    if (level < -45.0f) lift *= upward_scale;
                    float gain_db = amount * (lift * band_scale[band] * overall + tilt[band]);
                    state->comp_gain[slot][channel][band] = powf(10.0f, gain_db * 0.05f);
                }
                sum += bands[band] * state->comp_gain[slot][channel][band];
            }
            result[channel] = sum;
        }
        wet_l = result[0];
        wet_r = result[1];
        dry_gain = 0.0f;
        wet_gain = 1.0f;
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
    } else {
        state->delay_l[slot][pos] = 0.0f;
        state->delay_r[slot][pos] = 0.0f;
        dry_gain = 1.0f;
        wet_gain = 0.0f;
    }

    state->pos[slot] = (pos + 1u) & (SF_DELAY_SIZE - 1u);
    *output_l = input_l * dry_gain + wet_l * wet_gain;
    *output_r = input_r * dry_gain + wet_r * wet_gain;
}
