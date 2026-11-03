#include "engines.h"
#include "dsp_common.h"
#include "wavetable_bank.h"
#include "plaits_waveshaping_luts.h"
#include "plaits_modal_luts.h"

#include <math.h>

static float engine_exp2f_small(float x) {
    /* Accurate 3rd-order approximation of 2^x for |x| <= 0.5 (detune/vibrato). */
    return 1.0f + x * (0.69314718f + x * (0.24022651f + x * 0.05550411f));
}

static float engine_exp2f(float x) {
    x = sf_clampf(x, -24.0f, 24.0f);
    int ipart = (int)floorf(x);
    float fpart = x - (float)ipart;
    float scale = ldexpf(1.0f, ipart);
    return scale * (1.0f + fpart * (0.69314718f + fpart * (0.24022651f
                 + fpart * (0.05550411f + fpart * 0.00961813f))));
}

/* Keeps clang from vectorizing a short loop where measured slower (a vector
 * divide across five lanes). */
#if defined(__clang__)
#define SF_SCALAR_LOOP _Pragma("clang loop vectorize(disable) interleave(disable)")
#else
#define SF_SCALAR_LOOP
#endif

/* engine_exp2f with the power of two built from its exponent bits: for
 * ipart in -24..24, ldexpf(1, ipart) is that same exact power of two. */
static float engine_exp2f_bits(float x) {
    x = sf_clampf(x, -24.0f, 24.0f);
    int ipart = (int)floorf(x);
    float fpart = x - (float)ipart;
    union { uint32_t u; float f; } scale = { (uint32_t)(ipart + 127) << 23 };
    return scale.f * (1.0f + fpart * (0.69314718f + fpart * (0.24022651f
                 + fpart * (0.05550411f + fpart * 0.00961813f))));
}

/* 2^-n for 0 <= n <= 126, built from its exponent bits. Multiplying by it is
 * exact scaling, so it rounds just as dividing by 2^n does. */
static float engine_pow2_neg(int n) {
    union { uint32_t u; float f; } v = {(uint32_t)(127 - n) << 23};
    return v.f;
}

static float engine_fast_svf_g(float cutoff_hz) {
    float w = SF_PI * sf_clampf(cutoff_hz, 20.0f, 18000.0f) / SF_SAMPLE_RATE;
    float w2 = w * w;
    return w * (1.0f + w2 * (0.33333333f + w2 * 0.13333333f));
}

/* sin(2 pi phase) for phase in [0, 1), without a libm call. Folded onto a
 * quarter cycle, the 9th-order Taylor series is within 4e-6 of sinf: below a
 * 16-bit output's LSB even at full scale. */
static float engine_sin_cycle(float phase) {
    float x = phase < 0.5f ? phase : phase - 1.0f;
    if (x > 0.25f) x = 0.5f - x;
    else if (x < -0.25f) x = -0.5f - x;
    float t = SF_TWO_PI * x;
    float t2 = t * t;
    return t * (1.0f + t2 * (-1.6666667e-1f + t2 * (8.3333333e-3f
             + t2 * (-1.9841270e-4f + t2 * 2.7557319e-6f))));
}

static float poly_blep(float phase, float increment) {
    if (increment <= 0.0f) return 0.0f;
    /* One division at an edge and none between them: clang would otherwise
     * work out both edges' divisions on every call and pick one. */
    int rising = phase < increment;
    if (!rising && !(phase > 1.0f - increment)) return 0.0f;
    float t = (rising ? phase : phase - 1.0f) / increment;
    return rising ? t + t - t * t - 1.0f : t * t + t + t + 1.0f;
}

static float triangle_sample(float phase) {
    return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
}

static float basic_wave_sample(float phase, float increment, float morph,
                               float symmetry) {
    float saw = 2.0f * phase - 1.0f - poly_blep(phase, increment);
    float duty = 0.1f + 0.8f * symmetry;
    float edge = phase - duty;
    if (edge < 0.0f) edge += 1.0f;
    float pulse = (phase < duty ? 1.0f : -1.0f)
                + poly_blep(phase, increment) - poly_blep(edge, increment);
    if (morph < 0.5f) return pulse + (saw - pulse) * (morph * 2.0f);

    float octave_phase = phase * 2.0f;
    if (octave_phase >= 1.0f) octave_phase -= 1.0f;
    float octave_increment = sf_clampf(increment * 2.0f, 0.00001f, 0.49f);
    float octave_saw = 2.0f * octave_phase - 1.0f
                     - poly_blep(octave_phase, octave_increment);
    float offset = (symmetry - 0.5f) * 0.45f;
    float shifted_phase = phase + offset;
    if (shifted_phase < 0.0f) shifted_phase += 1.0f;
    else if (shifted_phase >= 1.0f) shifted_phase -= 1.0f;
    float shifted_octave_phase = shifted_phase * 2.0f;
    if (shifted_octave_phase >= 1.0f) shifted_octave_phase -= 1.0f;
    float shifted_octave_saw = 2.0f * shifted_octave_phase - 1.0f
                             - poly_blep(shifted_octave_phase, octave_increment);
    octave_saw = 0.5f * (octave_saw + shifted_octave_saw);
    return saw + (octave_saw - saw) * ((morph - 0.5f) * 2.0f);
}

static float standard_wave_sample(float phase, float increment, float wave) {
    float position = sf_clampf(wave, 0.0f, 1.0f) * 3.0f;
    int wave_a = (int)position;
    int wave_b = wave_a < 3 ? wave_a + 1 : wave_a;
    float blend = position - (float)wave_a;
    float a;
    float b;
    if (wave_a == 0) a = sinf(SF_TWO_PI * phase);
    else if (wave_a == 1) a = triangle_sample(phase);
    else if (wave_a == 2) a = basic_wave_sample(phase, increment, 0.5f, 0.5f);
    else a = basic_wave_sample(phase, increment, 0.0f, 0.5f);
    if (wave_b == 0) b = sinf(SF_TWO_PI * phase);
    else if (wave_b == 1) b = triangle_sample(phase);
    else if (wave_b == 2) b = basic_wave_sample(phase, increment, 0.5f, 0.5f);
    else b = basic_wave_sample(phase, increment, 0.0f, 0.5f);
    return a + (b - a) * blend;
}

/* MiniFreak Basic Waves, measured on MiniFreak V as a built-from-saws model.
 * Two unit saws, one offset by D = 0.5 - 0.45 * Timbre of a cycle, give either
 * a pulse (their difference) or a double saw (their sum; an octave-up saw at
 * D = 0.5). Wave 0..0.5 crossfades that pulse into a plain saw, and Wave
 * 0.5..1 crossfades the saw into the double saw, each linearly. Shape scales
 * the oscillator down by 0.52 * Shape and brings in a sine an octave below. */
#define SF_BASIC_GAIN 0.44f

static float basic_saw(float phase, float increment) {
    return 2.0f * phase - 1.0f - poly_blep(phase, increment);
}

static float minifreak_basic_wave_sample(const sf_engine_params_t *params,
                                         const sf_engine_voice_t *voice) {
    /* The knob-derived values only move with the knobs (sf_basic_cache_t). */
    sf_basic_cache_t local, *c = voice->basic_cache;
    if (!c) {
        local = (sf_basic_cache_t){0};
        c = &local;
    }
    if (!c->valid || c->wave != params->wave || c->timbre != params->timbre
        || c->shape != params->shape) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        float wave = sf_clampf(params->wave, 0.0f, 1.0f);
        float timbre = sf_clampf(params->timbre, 0.0f, 1.0f);
        float shape = sf_clampf(params->shape, 0.0f, 1.0f);
        c->offset = 0.5f - 0.45f * timbre;
        c->pulse = wave <= 0.5f;
        c->mix = c->pulse ? 1.0f - 2.0f * wave : 2.0f * wave - 1.0f;
        c->osc_gain = 1.0f - 0.52f * shape;
        c->sine_gain = shape;
        c->valid = 1;
    }
    float shifted = voice->phase + c->offset;
    if (shifted >= 1.0f) shifted -= 1.0f;
    float saw = basic_saw(voice->phase, voice->increment);
    float other = basic_saw(shifted, voice->increment);
    float osc;
    if (c->pulse) {
        float pulse_mix = c->mix;
        osc = saw + (saw - other - saw) * pulse_mix;
    } else {
        float double_mix = c->mix;
        osc = saw + other * double_mix;
    }
    float osc_gain = c->osc_gain;
    float shape = c->sine_gain;
    /* At Shape 0 the sine adds a zero, which leaves osc_gain * osc as it is
     * (osc is never -0), so it is not worked out. */
    if (shape == 0.0f) return SF_BASIC_GAIN * (osc_gain * osc);
    return SF_BASIC_GAIN * (osc_gain * osc
                            + shape * engine_sin_cycle(voice->sub_phase));
}

static float wavetable_frame_sample(int family, int frame, int level, float phase) {
    float sample_pos = phase * (float)SF_WT_SIZE;
    unsigned index = (unsigned)sample_pos;
    float fraction = sample_pos - (float)index;
    /* The Wavetable width warp can round a phase just below 1 up to 1.0,
     * which is sample 0 again, not one past the end of the frame. */
    index &= SF_WT_SIZE - 1u;
    unsigned next = (index + 1u) & (SF_WT_SIZE - 1u);
    const int16_t *table = sf_wavetable_bank[family][frame][level];
    float a = (float)table[index] * (1.0f / 32768.0f);
    float b = (float)table[next] * (1.0f / 32768.0f);
    return a + (b - a) * fraction;
}

static void wavetable_position(float family_position, float position, sf_wt_pos_t *p) {
    family_position = sf_clampf(family_position, 0.0f, (float)(SF_WT_FAMILIES - 1));
    p->family_a = (int)family_position;
    p->family_b = p->family_a + 1 < SF_WT_FAMILIES ? p->family_a + 1 : p->family_a;
    p->family_mix = family_position - (float)p->family_a;
    float frame_position = sf_clampf(position, 0.0f, 1.0f) * (float)(SF_WT_FRAMES - 1);
    p->frame_a = (int)frame_position;
    p->frame_b = p->frame_a + 1 < SF_WT_FRAMES ? p->frame_a + 1 : p->frame_a;
    p->frame_mix = frame_position - (float)p->frame_a;
}

static int wavetable_level(float increment) {
    float harmonic_budget = 0.45f / increment;
    int level = 0;
    while (level + 1 < SF_WT_LEVELS
           && (float)sf_wt_max_harmonics[level] > harmonic_budget) ++level;
    return level;
}

/* wavetable_sample with the position and level worked out. A whole frame
 * position gives frame_b no weight, so its read is skipped like family_b's
 * (a0 is never -0, so adding the zero changes nothing). */
static float wavetable_sample_at(const sf_wt_pos_t *p, int level, float phase) {
    float a0 = wavetable_frame_sample(p->family_a, p->frame_a, level, phase);
    float sample0 = a0;
    if (p->frame_mix != 0.0f) {
        float b0 = wavetable_frame_sample(p->family_a, p->frame_b, level, phase);
        sample0 = a0 + (b0 - a0) * p->frame_mix;
    }
    if (p->family_mix == 0.0f) return sample0;
    float a1 = wavetable_frame_sample(p->family_b, p->frame_a, level, phase);
    float sample1 = a1;
    if (p->frame_mix != 0.0f) {
        float b1 = wavetable_frame_sample(p->family_b, p->frame_b, level, phase);
        sample1 = a1 + (b1 - a1) * p->frame_mix;
    }
    return sample0 + (sample1 - sample0) * p->family_mix;
}

static float wavetable_sample(float family_position, float position,
                              float phase, float increment) {
    float harmonic_budget = 0.45f / increment;
    int level = 0;
    while (level + 1 < SF_WT_LEVELS
           && (float)sf_wt_max_harmonics[level] > harmonic_budget) ++level;

    family_position = sf_clampf(family_position, 0.0f, (float)(SF_WT_FAMILIES - 1));
    int family_a = (int)family_position;
    int family_b = family_a + 1 < SF_WT_FAMILIES ? family_a + 1 : family_a;
    float family_mix = family_position - (float)family_a;
    float frame_position = sf_clampf(position, 0.0f, 1.0f) * (float)(SF_WT_FRAMES - 1);
    int frame_a = (int)frame_position;
    int frame_b = frame_a + 1 < SF_WT_FRAMES ? frame_a + 1 : frame_a;
    float frame_mix = frame_position - (float)frame_a;
    float a0 = wavetable_frame_sample(family_a, frame_a, level, phase);
    float b0 = wavetable_frame_sample(family_a, frame_b, level, phase);
    float sample0 = a0 + (b0 - a0) * frame_mix;
    /* A whole family position (always so for a fixed table) gives family_b
     * no weight; skip its two reads. */
    if (family_mix == 0.0f) return sample0;
    float a1 = wavetable_frame_sample(family_b, frame_a, level, phase);
    float b1 = wavetable_frame_sample(family_b, frame_b, level, phase);
    float sample1 = a1 + (b1 - a1) * frame_mix;
    return sample0 + (sample1 - sample0) * family_mix;
}

/* West-Coast sinusoidal + reflection wavefolder with pre-folder DC bias
 * for even-harmonic asymmetry (Braids FOLD / Noise Engineering style). It is
 * split in two: what Amount and Bias alone decide (fold_prepare), which the
 * engines cache, and the per-sample fold (fold_apply). ref_sin is the sine
 * of the folded offset, which takes the offset back out of the output. */
static float fold_ref_sin(float dc) {
    float ref = dc;
    for (int i = 0; i < 4; ++i) {
        if (ref > 1.0f) ref = 2.0f - ref;
        else if (ref < -1.0f) ref = -2.0f - ref;
        else break;
    }
    /* sin(0) is 0 with its sign, which an unbiased fold (Destroy) always has. */
    return ref == 0.0f ? ref : sinf(ref * (0.5f * SF_PI));
}

static void fold_prepare(float amount, float bias, float *gain, float *dc, float *ref_sin) {
    *gain = 1.0f + amount * 6.5f;
    *dc = bias * (0.2f + amount * 0.65f);
    *ref_sin = fold_ref_sin(*dc);
}

/* Kept out of line: inlined into Bass, Apple clang pairs its sinf with
 * Bass's cosf into one sincos call, which rounds differently. */
__attribute__((noinline))
static float fold_apply(float sample, float gain, float dc, float ref_sin) {
    float x = sample * gain + dc;
    /* Four reflections fold |x| <= 9, but Osc 2's drive reaches about 19,
     * which used to clip flat at +-1 instead of folding. The fold repeats
     * every 4, so drop whole periods first. */
    if (x > 9.0f || x < -9.0f) x = fmodf(x - 1.0f, 4.0f) + 1.0f;
    for (int i = 0; i < 4; ++i) {
        if (x > 1.0f) x = 2.0f - x;
        else if (x < -1.0f) x = -2.0f - x;
        else break;
    }
    float sin_fold = sinf(x * (0.5f * SF_PI)) - ref_sin * 0.5f;
    float blended = x + (sin_fold - x) * 0.65f;
    return sf_clampf(blended, -1.0f, 1.0f);
}

static float fold_sample_biased(float sample, float amount, float bias) {
    float gain, dc, ref_sin;
    fold_prepare(amount, bias, &gain, &dc, &ref_sin);
    return fold_apply(sample, gain, dc, ref_sin);
}

/* fold_sample_biased with its knob-only terms kept in a sf_fold_cache_t. */
static float fold_sample_cached(sf_fold_cache_t *c, float sample, float amount,
                                float bias) {
    if (!c) return fold_sample_biased(sample, amount, bias);
    if (!c->valid || c->amount != amount || c->bias != bias) {
        c->amount = amount;
        c->bias = bias;
        fold_prepare(amount, bias, &c->gain, &c->dc, &c->ref_sin);
        c->valid = 1;
    }
    return fold_apply(sample, c->gain, c->dc, c->ref_sin);
}

static float warp_phase(float phase, float amount) {
    /* No warp adds exactly zero to a phase in [0, 1). */
    if (amount == 0.0f) return phase;
    float warped = phase + sinf(SF_TWO_PI * phase) * amount * 0.12f;
    if (warped < 0.0f) warped += 1.0f;
    else if (warped >= 1.0f) warped -= 1.0f;
    return warped;
}

static float noise_sample(uint32_t *rng) {
    return ((float)(sf_xorshift32(rng) >> 8) * (1.0f / 8388607.5f)) - 1.0f;
}

static float colored_noise_sample(const sf_engine_voice_t *voice, float type,
                                  float rate, float balance) {
    /* The knob terms, and the metallic oscillator's from the increment, are
     * worked out when those change (see sf_colored_noise_cache_t). */
    sf_colored_noise_cache_t local, *c = voice->colored_cache;
    if (!c) {
        local = (sf_colored_noise_cache_t){0};
        c = &local;
    }
    int knobs = !c->valid || c->type != type || c->rate != rate || c->balance != balance;
    if (knobs) {
        c->type = type;
        c->rate = rate;
        c->balance = balance;
        c->type_c = sf_clampf(type, 0.0f, 1.0f);
        float r = sf_clampf(rate, 0.0f, 1.0f);
        c->balance_c = sf_clampf(balance, 0.0f, 1.0f);
        /* Rate reduces the noise update rate, creating the MiniFreak's particle
         * character; Type selects particle -> white -> metallic noise. */
        c->period = 1u + (unsigned)(r * r * 63.0f);
        c->spread = engine_exp2f((r - 0.5f) * 6.0f);
        c->tone_mix = c->balance_c * 0.65f;
        c->norm = sqrtf(1.0f + c->tone_mix * c->tone_mix);
    }
    if (knobs || c->increment != voice->increment) {
        c->increment = voice->increment;
        c->metallic_increment = sf_clampf(voice->increment * c->spread, 0.00001f, 0.49f);
        c->metallic_ratio = c->metallic_increment / fmaxf(voice->increment, 0.00001f);
    }
    c->valid = 1;
    type = c->type_c;
    balance = c->balance_c;

    if (!voice->noise_state || !voice->rng) return 0.0f;
    if (!voice->noise_count || *voice->noise_count == 0u) {
        *voice->noise_state = noise_sample(voice->rng);
        if (voice->noise_count) *voice->noise_count = c->period;
    }
    if (voice->noise_count) --(*voice->noise_count);
    float particle = *voice->noise_state;
    float white = noise_sample(voice->rng);
    float noise;
    if (type < 0.5f) {
        float blend = type * 2.0f;
        noise = particle + (white - particle) * blend;
    } else {
        /* The metallic oscillator is only worked out where it is heard. It
         * keeps its own phase where it can: the voice phase times the ratio
         * jumped at every wrap of the voice phase unless the ratio was whole. */
        float metallic_phase;
        if (voice->stack_phase) {
            metallic_phase = voice->stack_phase[0];
            float next = metallic_phase + c->metallic_increment;
            voice->stack_phase[0] = next - floorf(next);
        } else {
            metallic_phase = voice->phase * c->metallic_ratio;
            metallic_phase -= floorf(metallic_phase);
        }
        float metallic = basic_wave_sample(metallic_phase, c->metallic_increment, 0.0f, 0.5f);
        float blend = (type - 0.5f) * 2.0f;
        noise = white + (metallic - white) * blend;
    }

    /* Balance adds a tonal waveform while walking Sine -> Triangle -> Square.
     * At zero the output is pure noise; the tonal component grows with travel. */
    float tone;
    if (balance < 1.0f / 3.0f)
        tone = sinf(SF_TWO_PI * voice->phase);
    else if (balance < 2.0f / 3.0f)
        tone = triangle_sample(voice->phase);
    else
        tone = basic_wave_sample(voice->phase, voice->increment, 0.0f, 0.5f);
    return (noise + tone * c->tone_mix) / c->norm;
}

/* MiniFreak Noise engine, measured on MiniFreak V (Osc 1 and Osc 2 alike;
 * nothing tracks the keyboard except the tone).
 *
 * Wave below about 0.45 is dust: random +/-1 clicks whose density and colour
 * come from the tables below (fitted per Wave x Timbre point to MiniFreak V's
 * octave spectrum, crest factor and level). At Wave 0 it is a few clicks a
 * second, and Timbre packs them densely; further up Wave the clicks thicken
 * and darken, and Timbre opens a one-pole low-pass (with a high-pass shelf on
 * top at high Timbre). Above that Wave crossfades into metallic noise: the
 * TR-808's six square oscillators (205/304/370/523/540/800 Hz) scaled by 1.49
 * and then by 1 + 4 x Timbre, through a 2 kHz high-pass; towards Wave 1 only
 * the 554 Hz one (and a little of the lowest) is left.
 *
 * Shape is the noise / tone balance: the noise falls as 1 - min(Shape, 0.7),
 * the tone (at the note's pitch) rises as (Shape / 0.7)^0.75 and morphs sine
 * -> triangle at 0.5 -> square at 1, at a constant RMS. */
#define SF_NOISE_W_POINTS 9
#define SF_NOISE_T_POINTS 6
/* Low- and high-pass corners are log2(Hz). */
static const float g_noise_w[SF_NOISE_W_POINTS] = {0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.375f, 0.5f};
static const float g_noise_t[SF_NOISE_T_POINTS] = {0.0f, 0.125f, 0.25f, 0.5f, 0.75f, 1.0f};
static const float g_noise_lp_hz_log2[SF_NOISE_W_POINTS][SF_NOISE_T_POINTS] = {
    {12.573f, 12.383f, 12.573f, 12.383f, 12.383f, 11.050f},
    {11.811f, 11.621f, 11.621f, 11.621f, 11.526f, 14.288f},
    {10.669f, 10.288f, 10.478f, 10.669f, 11.430f, 12.764f},
    {10.097f, 9.716f, 9.716f, 10.097f, 11.240f, 12.383f},
    {9.145f, 8.765f, 8.765f, 9.907f, 11.240f, 12.002f},
    {8.195f, 7.622f, 8.000f, 9.526f, 11.050f, 11.811f},
    {7.435f, 6.476f, 7.238f, 9.526f, 11.050f, 12.002f},
    {7.044f, 5.907f, 6.858f, 9.335f, 11.050f, 12.193f},
    {9.145f, 8.384f, 8.195f, 9.907f, 11.050f, 11.621f},
};
static const float g_noise_hp_hz_log2[SF_NOISE_W_POINTS][SF_NOISE_T_POINTS] = {
    {5.907f, 5.907f, 5.907f, 8.830f, 10.403f, 11.752f},
    {5.907f, 5.907f, 5.907f, 5.907f, 5.907f, 11.527f},
    {5.907f, 5.907f, 5.907f, 5.907f, 5.907f, 11.077f},
    {5.907f, 5.907f, 5.907f, 5.907f, 5.907f, 9.504f},
    {5.907f, 5.907f, 5.907f, 5.907f, 5.907f, 9.055f},
    {5.907f, 5.907f, 5.907f, 5.907f, 5.907f, 8.155f},
    {5.907f, 5.907f, 5.907f, 5.907f, 5.907f, 8.155f},
    {5.907f, 5.907f, 5.907f, 5.907f, 5.907f, 8.155f},
    {5.907f, 5.907f, 5.907f, 5.907f, 5.907f, 8.379f},
};
static const float g_noise_hp_mix[SF_NOISE_W_POINTS][SF_NOISE_T_POINTS] = {
    {0.000f, 0.000f, 0.000f, 0.125f, 2.000f, 4.000f},
    {0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 2.000f},
    {0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 2.000f},
    {0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 1.000f},
    {0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 1.000f},
    {0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 1.000f},
    {0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.500f},
    {0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.250f},
    {0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.500f},
};
static const float g_noise_log2_density[SF_NOISE_W_POINTS][SF_NOISE_T_POINTS] = {
    {-12.83f, -12.08f, -10.57f, -7.17f, -5.66f, -4.15f},
    {-12.83f, -12.08f, -9.44f, -5.66f, -3.77f, -3.40f},
    {-11.70f, -11.70f, -7.93f, -3.40f, -1.89f, -3.02f},
    {-10.19f, -10.57f, -6.79f, -1.13f, -0.75f, -2.64f},
    {-9.44f, -8.30f, -4.91f, 0.00f, -0.38f, -2.26f},
    {-7.55f, -7.93f, 0.00f, -0.38f, 0.00f, -1.51f},
    {-6.42f, -5.28f, 0.00f, 0.00f, 0.00f, -1.13f},
    {-4.91f, 0.00f, -1.51f, 0.00f, 0.00f, -1.13f},
    {-1.51f, -1.51f, -0.75f, -0.38f, -0.38f, -1.89f},
};
static const float g_noise_level_db[SF_NOISE_W_POINTS][SF_NOISE_T_POINTS] = {
    {-56.8f, -55.2f, -48.4f, -40.4f, -34.9f, -30.8f},
    {-52.2f, -48.8f, -43.5f, -35.1f, -29.8f, -27.0f},
    {-47.1f, -45.3f, -38.9f, -30.7f, -26.1f, -24.6f},
    {-41.6f, -40.3f, -34.4f, -26.8f, -23.7f, -23.0f},
    {-38.0f, -36.1f, -31.2f, -23.8f, -22.2f, -22.0f},
    {-33.7f, -32.4f, -27.2f, -21.8f, -21.2f, -21.2f},
    {-30.9f, -28.5f, -23.8f, -20.6f, -20.4f, -20.5f},
    {-28.8f, -26.3f, -22.8f, -20.7f, -20.6f, -20.9f},
    {-27.4f, -27.3f, -25.3f, -24.1f, -23.8f, -24.0f},
};

/* MiniFreak V levels (dBFS) to engine units: an engine sample of RMS 1 plays
 * at this many dBFS through a neutral voice. */
#define SF_NOISE_ENGINE_DB 14.9f
#define SF_NOISE_TONE_DB (-23.7f)
static const float g_noise_metal_hz[6] = {306.0f, 457.0f, 554.0f, 780.0f, 807.0f, 1195.0f};
/* Metal bank level and the dust's fade under it, at Wave 0.5, 0.625, 0.75,
 * 0.875 and 1 (dB). */
static const float g_noise_metal_db[5] = {-30.0f, -28.9f, -20.1f, -17.6f, -24.6f};
static const float g_noise_dust_fade_db[5] = {0.0f, -10.0f, -17.0f, -25.0f, -47.0f};

static float noise_wave_table(const float table[5], float wave) {
    float x = sf_clampf((wave - 0.5f) * 8.0f, 0.0f, 4.0f);
    int i = x >= 4.0f ? 3 : (int)x;
    return table[i] + (table[i + 1] - table[i]) * (x - (float)i);
}

static float noise_table(const float table[SF_NOISE_W_POINTS][SF_NOISE_T_POINTS],
                         int wi, float wf, int ti, float tf) {
    float a = table[wi][ti] + (table[wi][ti + 1] - table[wi][ti]) * tf;
    float b = table[wi + 1][ti] + (table[wi + 1][ti + 1] - table[wi + 1][ti]) * tf;
    return a + (b - a) * wf;
}

/* The dust's filter coefficients, density and level for a Wave (<= 0.5) and
 * Timbre. */
static void mf_noise_dust_coefficients(sf_noise_cache_t *c, float wave, float timbre) {
    int wi = 0;
    while (wi < SF_NOISE_W_POINTS - 2 && wave > g_noise_w[wi + 1]) ++wi;
    float wf = sf_clampf((wave - g_noise_w[wi]) / (g_noise_w[wi + 1] - g_noise_w[wi]), 0.0f, 1.0f);
    int ti = 0;
    while (ti < SF_NOISE_T_POINTS - 2 && timbre > g_noise_t[ti + 1]) ++ti;
    float tf = sf_clampf((timbre - g_noise_t[ti]) / (g_noise_t[ti + 1] - g_noise_t[ti]), 0.0f, 1.0f);
    float lp_hz = engine_exp2f(noise_table(g_noise_lp_hz_log2, wi, wf, ti, tf));
    float hp_hz = engine_exp2f(noise_table(g_noise_hp_hz_log2, wi, wf, ti, tf));
    float mix = noise_table(g_noise_hp_mix, wi, wf, ti, tf);
    float density = engine_exp2f(noise_table(g_noise_log2_density, wi, wf, ti, tf));
    float level_db = noise_table(g_noise_level_db, wi, wf, ti, tf);

    float al = expf(-SF_TWO_PI * lp_hz / SF_SAMPLE_RATE);
    float ah = expf(-SF_TWO_PI * hp_hz / SF_SAMPLE_RATE);
    /* Power of the click train through LP + mix x HP, per unit density, so
     * the tables can hold the measured level directly. */
    float hp0 = 0.5f * (1.0f + ah);
    float p_lp = (1.0f - al) / (1.0f + al);
    float p_hp = hp0;
    float p_x = (1.0f - al) * hp0 * (1.0f - (1.0f - ah) * al / (1.0f - al * ah));
    float power = density * (p_lp + 2.0f * mix * p_x + mix * mix * p_hp);
    c->al = al;
    c->ah = ah;
    c->hp0 = hp0;
    c->mix = mix;
    c->density = density;
    c->gain = engine_exp2f((level_db + SF_NOISE_ENGINE_DB) * 0.16609640f)
            / sqrtf(power > 1e-12f ? power : 1e-12f);
}

static float mf_noise_dust(const sf_engine_voice_t *voice, const sf_noise_cache_t *c) {
    float al = c->al;
    float ah = c->ah;
    float hp0 = c->hp0;
    float click = 0.0f;
    uint32_t r = sf_xorshift32(voice->rng);
    if ((float)(r >> 8) * (1.0f / 16777216.0f) < c->density)
        click = (r & 1u) ? 1.0f : -1.0f;
    float *lp = &voice->filter_state->low;
    float *hp = &voice->filter_state->band;
    *lp = (1.0f - al) * click + al * *lp;
    float hp_out = hp0 * (click - *voice->noise_state) + ah * *hp;
    *voice->noise_state = click;
    *hp = hp_out;
    return c->gain * (*lp + c->mix * hp_out);
}

/* The metal bank's six increments and gains and its level for a Wave and
 * Timbre. */
static void mf_noise_metal_coefficients(sf_noise_cache_t *c, float wave, float timbre) {
    float scale = (1.0f + 4.0f * timbre) / SF_SAMPLE_RATE;
    float thin = sf_clampf((1.0f - wave) / 0.375f, 0.0f, 1.0f);
    float others = thin * thin * sqrtf(thin);
    for (int i = 0; i < 6; ++i) {
        c->metal_inc[i] = sf_clampf(g_noise_metal_hz[i] * scale, 0.0f, 0.45f);
        c->metal_g[i] = i == 2 ? 1.0f : (i == 0 ? fmaxf(others, 0.3f) : others);
    }
    c->metal_level = engine_exp2f((noise_wave_table(g_noise_metal_db, wave)
                                   + SF_NOISE_ENGINE_DB) * 0.16609640f);
}

static float mf_noise_metal(const sf_engine_voice_t *voice, const sf_noise_cache_t *c) {
    float sum = 0.0f;
    for (int i = 0; i < 6; ++i) {
        float *phase = i < 5 ? &voice->chord_phase[i] : &voice->stack_phase[0];
        float increment = c->metal_inc[i];
        float p = *phase + increment;
        p -= floorf(p);
        *phase = p;
        float edge = p - 0.5f;
        if (edge < 0.0f) edge += 1.0f;
        /* Away from both edges the two corrections are 0 and (1 + 0) - 0
         * is 1, so they (and their divisions) are only worked out next to
         * an edge. */
        float square = p < 0.5f ? 1.0f : -1.0f;
        if (p < increment || p > 1.0f - increment
            || edge < increment || edge > 1.0f - increment)
            square = square + poly_blep(p, increment) - poly_blep(edge, increment);
        float g = c->metal_g[i];
        sum += g * square;
    }
    /* 2 kHz one-pole high-pass (stack_phase[1..2] hold its state). */
    const float a = 0.75207f;  /* exp(-2 pi 2000 / 44100) */
    float out = 0.5f * (1.0f + a) * (sum - voice->stack_phase[1]) + a * voice->stack_phase[2];
    voice->stack_phase[1] = sum;
    voice->stack_phase[2] = out;
    return out * c->metal_level;
}

/* Everything the Noise engine works out from the knobs alone (see
 * sf_noise_cache_t). */
static void minifreak_noise_coefficients(sf_noise_cache_t *c,
                                         const sf_engine_params_t *params) {
    c->wave = params->wave;
    c->timbre = params->timbre;
    c->shape = params->shape;
    float wave = sf_clampf(params->wave, 0.0f, 1.0f);
    float timbre = sf_clampf(params->timbre, 0.0f, 1.0f);
    float shape = sf_clampf(params->shape, 0.0f, 1.0f);
    /* The dust keeps running under the metal bank, fading out towards
     * Wave 1 (g_noise_dust_fade_db). */
    float dust_db = noise_wave_table(g_noise_dust_fade_db, wave);
    c->dust_on = dust_db > -60.0f;
    if (c->dust_on) {
        mf_noise_dust_coefficients(c, wave < 0.5f ? wave : 0.5f, timbre);
        c->dust_fade = engine_exp2f(dust_db * 0.16609640f);
    }
    c->metal_on = wave > 0.375f;
    if (c->metal_on) {
        float x = sf_clampf((wave - 0.375f) / 0.125f, 0.0f, 1.0f);
        c->metal_mix = sqrtf(x);
        mf_noise_metal_coefficients(c, wave, timbre);
    }
    c->balance = shape < 0.7f ? shape : 0.7f;
    c->tone_gain = powf(c->balance / 0.7f, 0.75f);
    c->tone_level = engine_exp2f((SF_NOISE_TONE_DB + SF_NOISE_ENGINE_DB) * 0.16609640f);
    c->valid = 1;
}

static float minifreak_noise_sample(const sf_engine_voice_t *voice,
                                    const sf_engine_params_t *params) {
    if (!voice->rng || !voice->noise_state || !voice->filter_state
        || !voice->chord_phase || !voice->stack_phase)
        return colored_noise_sample(voice, sf_clampf(params->wave, 0.0f, 1.0f),
                                    sf_clampf(params->timbre, 0.0f, 1.0f),
                                    sf_clampf(params->shape, 0.0f, 1.0f));
    /* The coefficients only move with the knobs, so they are worked out when
     * those change. */
    sf_noise_cache_t local, *c = voice->noise_cache;
    if (!c) {
        local = (sf_noise_cache_t){0};
        c = &local;
    }
    if (!c->valid || c->wave != params->wave || c->timbre != params->timbre
        || c->shape != params->shape)
        minifreak_noise_coefficients(c, params);

    float noise = 0.0f;
    if (c->dust_on)
        noise = mf_noise_dust(voice, c) * c->dust_fade;
    if (c->metal_on)
        noise += mf_noise_metal(voice, c) * c->metal_mix;

    float out = noise * (1.0f - c->balance);
    if (c->tone_gain > 0.0f) {
        float shape = sf_clampf(params->shape, 0.0f, 1.0f);
        float phase = voice->phase;
        float tri = triangle_sample(phase) * 1.73205081f;
        float tone;
        /* Only the waves the blend uses are worked out. */
        if (shape < 0.5f) {
            float sine = sinf(SF_TWO_PI * phase) * 1.41421356f;
            tone = sine + (tri - sine) * (shape * 2.0f);
        } else if (shape < 0.75f) {
            tone = tri;
        } else {
            float edge = phase - 0.5f;
            if (edge < 0.0f) edge += 1.0f;
            float square = (phase < 0.5f ? 1.0f : -1.0f) + poly_blep(phase, voice->increment)
                         - poly_blep(edge, voice->increment);
            tone = tri + (square - tri) * ((shape - 0.75f) * 4.0f);
        }
        out += tone * c->tone_gain * c->tone_level;
    }
    return out;
}

/* SuperWave's Wave knob toggles through four base waveforms, evenly spaced
 * across 0..1: saw (0), square, triangle, sine (1). Saw at 0 is the classic
 * JP-8000 supersaw. */
static int superwave_wave_select(float wave) {
    /* MiniFreak V switches at 0.25, 0.5 and 0.75 (saw, square, triangle,
     * sine), measured. */
    int selected = (int)(sf_clampf(wave, 0.0f, 1.0f) * 4.0f);
    if (selected < 0) selected = 0;
    if (selected > 3) selected = 3;
    return selected;
}

/* basic_wave_sample(phase, increment, 0.0f, 0.5f), the pulse: its saw gets no
 * weight there, pulse + (saw - pulse) * 0 is the pulse itself (the pulse is
 * never -0), so it is not worked out. */
static float basic_square(float phase, float increment) {
    float duty = 0.1f + 0.8f * 0.5f;
    float edge = phase - duty;
    if (edge < 0.0f) edge += 1.0f;
    return (phase < duty ? 1.0f : -1.0f)
         + poly_blep(phase, increment) - poly_blep(edge, increment);
}

static float superwave_wave_sample(float phase, float increment, int selected) {
    switch (selected) {
        default:
        /* basic_wave_sample(phase, increment, 0.5f, 0.5f) is its saw: the
         * octave saws are weighted (0.5 - 0.5) * 2 = 0, and the saw is never
         * -0, so saw + x * 0 is the saw. */
        case 0: return basic_saw(phase, increment);
        case 1: return basic_square(phase, increment);
        case 2: return triangle_sample(phase);
        case 3: return sinf(SF_TWO_PI * phase);
    }
}

/* MiniFreak V levels, measured on a held C3: the base wave sits about 9 dB
 * under Bizar's full-scale wave, and Shape (the copies' level) adds
 * power, not amplitude: +3 dB at 0.5 and +4.7 dB at 1, detuned or not. The
 * copies start a fixed spread of phases apart so they add in power from the
 * first cycle. */
#define SF_SUPERWAVE_GAIN 0.36f
#define SF_SUPERWAVE_SHAPE_GAIN 0.7f

static float superwave_sample(const sf_engine_params_t *params,
                              const sf_engine_voice_t *voice) {
    /* Incommensurate detune spacing inspired by Braids SAWx / JP-8000 prevents
     * periodic phase-cancellation swooshing across the stacked oscillators. */
    static const float detune_spread[SF_SUPERWAVE_COPIES] = {
        -1.03f, -0.37f, 0.41f, 1.09f
    };
    static const float phase_offset[SF_SUPERWAVE_COPIES] = {
        0.21f, 0.47f, 0.73f, 0.89f
    };
    /* The wave choice, detune ratios and gains only move with the knobs, the
     * copies' increments with the pitch (sf_superwave_cache_t). */
    sf_superwave_cache_t local, *c = voice->superwave_cache;
    if (!c) {
        local = (sf_superwave_cache_t){0};
        c = &local;
    }
    int knobs = !c->valid || c->wave != params->wave || c->timbre != params->timbre
             || c->shape != params->shape;
    if (knobs) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        c->selected = superwave_wave_select(params->wave);
        float copy_level = sf_clampf(params->shape, 0.0f, 1.0f);
        c->copy_level = copy_level;
        float t = sf_clampf(params->timbre, 0.0f, 1.0f);
        float detune_cents = t * (0.32f + 0.68f * t) * 54.0f;
        for (int copy = 0; copy < SF_SUPERWAVE_COPIES; ++copy) {
            float cents = detune_spread[copy] * detune_cents;
            c->ratio[copy] = engine_exp2f_small(cents * (1.0f / 1200.0f));
        }
        c->norm = sqrtf(1.0f + copy_level * copy_level * (float)SF_SUPERWAVE_COPIES);
        c->level_gain = 1.0f + SF_SUPERWAVE_SHAPE_GAIN * copy_level;
    }
    float base = superwave_wave_sample(voice->phase, voice->increment, c->selected);
    float copy_level = c->copy_level;
    if (copy_level <= 0.0001f || !voice->stack_phase) {
        c->valid = 1;
        c->increment = -1.0f;   /* the copies' increments were not worked out */
        return base * SF_SUPERWAVE_GAIN;
    }
    if (knobs || c->increment != voice->increment) {
        c->increment = voice->increment;
        for (int copy = 0; copy < SF_SUPERWAVE_COPIES; ++copy)
            c->copy_increment[copy] = sf_clampf(voice->increment * c->ratio[copy],
                                                0.00001f, 0.49f);
    }
    c->valid = 1;

    float sum = base;
    for (int copy = 0; copy < SF_SUPERWAVE_COPIES; ++copy) {
        float increment = c->copy_increment[copy];
        float phase = voice->stack_phase[copy];
        float read = phase + phase_offset[copy];
        if (read >= 1.0f) read -= 1.0f;
        sum += superwave_wave_sample(read, increment, c->selected) * copy_level;
        phase += increment;
        if (phase >= 1.0f) phase -= 1.0f;
        voice->stack_phase[copy] = phase;
    }
    return sum / c->norm * c->level_gain * SF_SUPERWAVE_GAIN;
}

/* Output trims that level each engine against MiniFreak V at identical
 * Volume, measured over a Wave/Timbre/Shape sweep of a held C3. */
#define SF_TRIM_WAVETABLE 2.4f   /* +7.5 dB */
#define SF_TRIM_KARPLUS 2.1f     /* +6.4 dB */
#define SF_TRIM_MODAL 1.6f       /* +4.1 dB */
#define SF_TRIM_SPEECH 0.52f     /* -5.7 dB */

/* Knob remaps that make a MiniFreak Wave/Timbre/Shape value sound like the same
 * value on MiniFreak V. Fitted by sweeping both synths over a held C3 and
 * minimising the band-spectrum distance, then checked on held-out settings.
 * route[m] is the engine knob (0 wave, 1 timbre, 2 shape) that MiniFreak knob m
 * drives; each curve is piecewise linear over evenly spaced knots. */
typedef struct {
    unsigned char route[3];
    float wave[3];
    float timbre[5];
    float shape[5];
} sf_remap_t;

/* MiniFreak Timbre drives the pulse Shape knob, as the preset importer notes.
 * A pure knob swap with straight curves fits as well as any fitted curve. */
static const sf_remap_t g_remap_vanalog = {
    {1, 2, 0}, {0.0f, 0.5f, 1.0f},
    {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}, {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}};
static float remap_curve(const float *knots, int count, float x) {
    x = sf_clampf(x, 0.0f, 1.0f) * (float)(count - 1);
    int i = (int)x;
    if (i >= count - 1) return knots[count - 1];
    return knots[i] + (knots[i + 1] - knots[i]) * (x - (float)i);
}

static const sf_engine_params_t *remap_params(const sf_engine_params_t *in,
                                              const sf_remap_t *r,
                                              sf_engine_params_t *out) {
    float knob[3];
    knob[r->route[0]] = remap_curve(r->wave, 3, in->wave);
    knob[r->route[1]] = remap_curve(r->timbre, 5, in->timbre);
    knob[r->route[2]] = remap_curve(r->shape, 5, in->shape);
    *out = *in;
    out->wave = knob[0];
    out->timbre = knob[1];
    out->shape = knob[2];
    return out;
}

/* --------------------------------------------------------------- Harmo --- */

/* MiniFreak Harmo, measured on MiniFreak V: only the odd partials sound (the
 * even ones sit 50 dB down). Wave slides a peak up the series, centred on
 * partial 1 + 14.5 * Wave, with roughly -1.3 dB * distance^2 of skirt either
 * side. Timbre lifts the floor under that peak from -50 dB at 0 to -9 dB by
 * 0.5, which turns the single bump into a flat odd-harmonic spread. Above
 * partial 15 the hardware carries a thin tail of odd partials (about -25 dB at
 * 17, falling 0.35 dB per partial) that also opens up with Timbre; it supplies
 * the upper-band energy of the real engine. Shape is a detuned chorus copy.
 * Output power is constant across the knobs. SF_HARMO_PARTIALS is in
 * engines.h, for the weight cache. */
#define SF_HARMO_PEAK_SPAN 14.5f
#define SF_HARMO_SKIRT_DB 1.3f
#define SF_HARMO_FLOOR_LOW_DB (-50.0f)
#define SF_HARMO_FLOOR_HIGH_DB (-9.0f)
#define SF_HARMO_TAIL_LOW_DB (-45.0f)
#define SF_HARMO_TAIL_HIGH_DB (-25.0f)
#define SF_HARMO_TAIL_SLOPE_DB 0.35f
#define SF_HARMO_GAIN 0.715f
#define SF_HARMO_DB_STEPS 4
#define SF_HARMO_DB_RANGE 120

/* Amplitude for -index / SF_HARMO_DB_STEPS dB, so the per-sample weight build
 * costs a table lookup instead of a powf per partial. */
static float g_harmo_amp[SF_HARMO_DB_RANGE * SF_HARMO_DB_STEPS + 1];
static volatile int g_harmo_amp_ready;

static void harmo_build_table(void) {
    for (int i = 0; i <= SF_HARMO_DB_RANGE * SF_HARMO_DB_STEPS; ++i)
        g_harmo_amp[i] = powf(10.0f, -(float)i / (float)SF_HARMO_DB_STEPS * 0.05f);
    g_harmo_amp_ready = 1;
}

static float harmo_amp(float db) {
    float index = -db * (float)SF_HARMO_DB_STEPS;
    if (index < 0.0f) index = 0.0f;
    if (index > (float)(SF_HARMO_DB_RANGE * SF_HARMO_DB_STEPS))
        index = (float)(SF_HARMO_DB_RANGE * SF_HARMO_DB_STEPS);
    return g_harmo_amp[(int)index];
}

static void harmo_weights(const sf_engine_params_t *params, float *weights, int count) {
    if (!g_harmo_amp_ready) harmo_build_table();
    float centre = 1.0f + SF_HARMO_PEAK_SPAN * sf_clampf(params->wave, 0.0f, 1.0f);
    float lift = sf_clampf(params->timbre * 2.0f, 0.0f, 1.0f);
    float floor_db = SF_HARMO_FLOOR_LOW_DB
        + (SF_HARMO_FLOOR_HIGH_DB - SF_HARMO_FLOOR_LOW_DB) * lift;
    float tail_db = SF_HARMO_TAIL_LOW_DB
        + (SF_HARMO_TAIL_HIGH_DB - SF_HARMO_TAIL_LOW_DB) * lift;
    float power = 0.0f;
    for (int i = 0; i < count; ++i) {
        float harmonic = (float)(2 * i + 1);
        float distance = harmonic - centre;
        float db = -SF_HARMO_SKIRT_DB * distance * distance;
        float floor_here = floor_db;
        if (harmonic > 15.5f) {
            float tail = tail_db - SF_HARMO_TAIL_SLOPE_DB * (harmonic - 17.0f);
            floor_here = tail < floor_db ? tail : floor_db;
        }
        if (db < floor_here) db = floor_here;
        weights[i] = harmo_amp(db);
        power += weights[i] * weights[i];
    }
    float scale = SF_HARMO_GAIN / sqrtf(power);
    for (int i = 0; i < count; ++i) weights[i] *= scale;
}

/* Odd partials by rotation: z_(i+1) = z_i * e^(j 2 theta), starting at theta. */
static float harmo_additive(float phase, const float *weights, int count) {
    float theta = SF_TWO_PI * phase;
    float s = sinf(theta), c = cosf(theta);
    float c2 = 1.0f - 2.0f * s * s, s2 = 2.0f * s * c;
    float sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        sum += weights[i] * s;
        float next_s = s * c2 + c * s2;
        c = c * c2 - s * s2;
        s = next_s;
    }
    return sum;
}

static float harmo_sample(const sf_engine_params_t *params,
                          const sf_engine_voice_t *voice) {
    /* Keep every partial under Nyquist (increment * harmonic < 0.49). */
    int count = (int)((0.49f / voice->increment + 1.0f) * 0.5f);
    if (count > SF_HARMO_PARTIALS) count = SF_HARMO_PARTIALS;
    if (count < 1) count = 1;
    float local[SF_HARMO_PARTIALS];
    const float *weights = local;
    sf_harmo_cache_t *cache = voice->harmo_cache;
    if (!cache) {
        harmo_weights(params, local, count);
    } else {
        /* The weights depend on Wave, Timbre and the count alone, which hold
         * still unless a knob is moving or being modulated. */
        if (cache->count != count || cache->wave != params->wave
            || cache->timbre != params->timbre) {
            harmo_weights(params, cache->weights, count);
            cache->wave = params->wave;
            cache->timbre = params->timbre;
            cache->count = count;
        }
        weights = cache->weights;
    }
    float raw = harmo_additive(voice->phase, weights, count);

    /* Chorus thickens the sound with a gently detuned copy. */
    float chorus = sf_clampf(params->shape, 0.0f, 1.0f);
    if (chorus > 0.001f && voice->stack_phase) {
        float increment = sf_clampf(voice->increment * (1.0f + 0.006f * chorus),
                                        0.00001f, 0.49f);
        float phase = voice->stack_phase[0];
        float wet = harmo_additive(phase, weights, count);
        float wet_level = 0.35f * chorus;
        raw = raw * (1.0f - wet_level) + wet * wet_level;
        phase += increment;
        if (phase >= 1.0f) phase -= 1.0f;
        voice->stack_phase[0] = phase;
    }
    return raw;
}

/* ------------------------------------------------------------- Formant --- */

/* Adapted from Emilie Gillet's Plaits GrainEngine and GrainletOscillator
 * (plaits/dsp/engine/grain_engine.cc, plaits/dsp/oscillator/grainlet_oscillator.h; MIT).
 * Wave = Interval (ratio between the two formants + carrier bleed),
 * Timbre = Formant (base formant frequency), Shape = Shape (grainlet width/shape). */
static float plaits_grainlet_carrier(float phase, float shape) {
    shape = sf_clampf(shape, 0.0f, 1.0f) * 3.0f;
    int shape_integral = (int)shape;
    float shape_fractional = shape - (float)shape_integral;
    float t = 1.0f - shape_fractional;

    if (shape_integral == 0) {
        phase = phase * (1.0f + t * t * t * 15.0f);
        if (phase >= 1.0f) phase = 1.0f;
        phase += 0.75f;
    } else if (shape_integral == 1) {
        float breakpoint = 0.001f + 0.499f * t * t * t;
        if (phase < breakpoint) phase *= (0.5f / breakpoint);
        else phase = 0.5f + (phase - breakpoint) * 0.5f / (1.0f - breakpoint);
        phase += 0.75f;
    } else {
        t = 1.0f - t;
        phase = 0.25f + phase * (0.5f + t * t * t * 14.5f);
        if (phase >= 0.75f) phase = 0.75f;
    }
    return (sinf(SF_TWO_PI * phase) + 1.0f) * 0.25f;
}

static float plaits_grainlet_value(float carrier_phase, float formant_phase,
                                   float shape, float bleed) {
    float carrier = plaits_grainlet_carrier(carrier_phase, shape);
    float formant = sinf(SF_TWO_PI * formant_phase);
    return carrier * (formant + bleed) / (1.0f + bleed);
}

static float plaits_grainlet_tick(float carrier_phase, float f0, float f1,
                                  float shape, float bleed,
                                  float *formant_phase, float *next_state) {
    float this_sample = *next_state;
    float next_sample = 0.0f;
    float c_next = carrier_phase + f0;
    float fp = *formant_phase;

    if (c_next >= 1.0f) {
        float c_wrap = c_next - 1.0f;
        float reset_time = c_wrap / f0;
        float before = plaits_grainlet_value(
            1.0f, fp + (1.0f - reset_time) * f1, shape, bleed);
        float after = plaits_grainlet_value(0.0f, 0.0f, shape, bleed);
        float discontinuity = after - before;
        this_sample += discontinuity * (0.5f * reset_time * reset_time);
        float u = 1.0f - reset_time;
        next_sample -= discontinuity * (0.5f * u * u);
        fp = reset_time * f1;
        c_next = c_wrap;
    } else {
        fp += f1;
        if (fp >= 1.0f) fp -= (float)(int)fp;
    }

    next_sample += plaits_grainlet_value(c_next, fp, shape, bleed);
    *formant_phase = fp;
    *next_state = next_sample;
    return this_sample;
}

/* plaits_grainlet_carrier with its shape-dependent terms worked out ahead
 * (formant_carrier_prepare): the same expressions, so the same values. */
static void formant_carrier_prepare(sf_formant_cache_t *c, float shape) {
    shape = sf_clampf(shape, 0.0f, 1.0f) * 3.0f;
    int shape_integral = (int)shape;
    float shape_fractional = shape - (float)shape_integral;
    float t = 1.0f - shape_fractional;
    c->carrier_integral = shape_integral;
    if (shape_integral == 0) {
        c->carrier_m = 1.0f + t * t * t * 15.0f;
    } else if (shape_integral == 1) {
        float breakpoint = 0.001f + 0.499f * t * t * t;
        c->carrier_bp = breakpoint;
        c->carrier_half_over_bp = 0.5f / breakpoint;
        c->carrier_one_minus_bp = 1.0f - breakpoint;
    } else {
        t = 1.0f - t;
        c->carrier_m = 0.5f + t * t * t * 14.5f;
    }
}

static float formant_carrier(const sf_formant_cache_t *c, float phase) {
    if (c->carrier_integral == 0) {
        phase = phase * c->carrier_m;
        if (phase >= 1.0f) phase = 1.0f;
        phase += 0.75f;
    } else if (c->carrier_integral == 1) {
        if (phase < c->carrier_bp) phase *= c->carrier_half_over_bp;
        else phase = 0.5f + (phase - c->carrier_bp) * 0.5f / c->carrier_one_minus_bp;
        phase += 0.75f;
    } else {
        phase = 0.25f + phase * c->carrier_m;
        if (phase >= 0.75f) phase = 0.75f;
    }
    return (sinf(SF_TWO_PI * phase) + 1.0f) * 0.25f;
}

static float formant_grain(float carrier, float formant_phase, float bleed) {
    float formant = sinf(SF_TWO_PI * formant_phase);
    return carrier * (formant + bleed) / (1.0f + bleed);
}

/* Both grainlets of plaits_grainlet_tick in one pass. They share the
 * carrier phase, so the carrier (a sinf) is worked out once, and its values
 * at a reset are kept in the cache. */
static float formant_pair_tick(sf_formant_cache_t *c, float carrier_phase,
                               float *stack) {
    const float f0 = c->f0;
    float this1 = stack[2];
    float this2 = stack[3];
    float next1 = 0.0f;
    float next2 = 0.0f;
    float c_next = carrier_phase + f0;
    float fp1 = stack[0];
    float fp2 = stack[1];

    if (c_next >= 1.0f) {
        if (!c->ends_valid) {
            c->carrier_end = plaits_grainlet_carrier(1.0f, c->carrier_shape);
            c->after = plaits_grainlet_value(0.0f, 0.0f, c->carrier_shape, c->bleed);
            c->ends_valid = 1;
        }
        float c_wrap = c_next - 1.0f;
        float reset_time = c_wrap / f0;
        float u = 1.0f - reset_time;
        float before1 = formant_grain(c->carrier_end,
                                      fp1 + (1.0f - reset_time) * c->f1, c->bleed);
        float discontinuity1 = c->after - before1;
        this1 += discontinuity1 * (0.5f * reset_time * reset_time);
        next1 -= discontinuity1 * (0.5f * u * u);
        fp1 = reset_time * c->f1;
        float before2 = formant_grain(c->carrier_end,
                                      fp2 + (1.0f - reset_time) * c->f2, c->bleed);
        float discontinuity2 = c->after - before2;
        this2 += discontinuity2 * (0.5f * reset_time * reset_time);
        next2 -= discontinuity2 * (0.5f * u * u);
        fp2 = reset_time * c->f2;
        c_next = c_wrap;
    } else {
        fp1 += c->f1;
        if (fp1 >= 1.0f) fp1 -= (float)(int)fp1;
        fp2 += c->f2;
        if (fp2 >= 1.0f) fp2 -= (float)(int)fp2;
    }

    float carrier = formant_carrier(c, c_next);
    next1 += formant_grain(carrier, fp1, c->bleed);
    next2 += formant_grain(carrier, fp2, c->bleed);
    stack[0] = fp1;
    stack[1] = fp2;
    stack[2] = next1;
    stack[3] = next2;
    return this1 + this2;
}

static float formant_sample_cached(const sf_engine_params_t *params,
                                   const sf_engine_voice_t *voice,
                                   sf_formant_cache_t *c) {
    int knobs = !c->valid || c->wave != params->wave || c->timbre != params->timbre
             || c->shape != params->shape;
    if (knobs) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        float harmonics = sf_clampf(params->wave, 0.0f, 1.0f);
        float timbre = sf_clampf(params->timbre, 0.0f, 1.0f);
        float f1_hz = 440.0f * engine_exp2f((24.0f + 84.0f * timbre - 69.0f) / 12.0f);
        float f1 = sf_clampf(f1_hz / SF_SAMPLE_RATE, 0.0001f, 0.245f);
        float ratio = engine_exp2f((-24.0f + 48.0f * harmonics) / 12.0f);
        c->f1 = f1;
        c->f2 = sf_clampf(f1 * ratio, 0.0001f, 0.245f);
        float carrier_bleed = harmonics < 0.5f ? 1.0f - 2.0f * harmonics : 0.0f;
        c->bleed = carrier_bleed * (2.0f - carrier_bleed);
    }
    if (knobs || c->increment != voice->increment) {
        c->increment = voice->increment;
        float f0 = sf_clampf(voice->increment, 0.00001f, 0.125f);
        float morph = sf_clampf(params->shape, 0.0f, 1.0f);
        c->f0 = f0;
        c->carrier_shape = 0.33f + (morph - 0.33f) * fmaxf(1.0f - f0 * 24.0f, 0.0f);
        formant_carrier_prepare(c, c->carrier_shape);
        c->dc_f = 1.0f - expf(-SF_TWO_PI * 0.3f * f0);
        c->ends_valid = 0;
    }
    c->valid = 1;

    float sum = formant_pair_tick(c, voice->phase, voice->stack_phase);
    if (voice->formant_dc) {
        float hp = sum - *voice->formant_dc;
        *voice->formant_dc += c->dc_f * hp;
        sum = hp;
    }
    return sum * 1.35f;
}

static float formant_sample(const sf_engine_params_t *params,
                            const sf_engine_voice_t *voice) {
    if (voice->formant_cache && voice->stack_phase)
        return formant_sample_cached(params, voice, voice->formant_cache);
    float f0 = sf_clampf(voice->increment, 0.00001f, 0.125f);
    float harmonics = sf_clampf(params->wave, 0.0f, 1.0f);
    float timbre = sf_clampf(params->timbre, 0.0f, 1.0f);
    float morph = sf_clampf(params->shape, 0.0f, 1.0f);

    float f1_hz = 440.0f * engine_exp2f((24.0f + 84.0f * timbre - 69.0f) / 12.0f);
    float f1 = sf_clampf(f1_hz / SF_SAMPLE_RATE, 0.0001f, 0.245f);
    float ratio = engine_exp2f((-24.0f + 48.0f * harmonics) / 12.0f);
    float f2 = sf_clampf(f1 * ratio, 0.0001f, 0.245f);

    float carrier_bleed = harmonics < 0.5f ? 1.0f - 2.0f * harmonics : 0.0f;
    float carrier_bleed_fixed = carrier_bleed * (2.0f - carrier_bleed);
    float carrier_shape = 0.33f + (morph - 0.33f) * fmaxf(1.0f - f0 * 24.0f, 0.0f);

    float sum;
    if (voice->stack_phase) {
        float g1 = plaits_grainlet_tick(voice->phase, f0, f1, carrier_shape,
                                        carrier_bleed_fixed,
                                        &voice->stack_phase[0],
                                        &voice->stack_phase[2]);
        float g2 = plaits_grainlet_tick(voice->phase, f0, f2, carrier_shape,
                                        carrier_bleed_fixed,
                                        &voice->stack_phase[1],
                                        &voice->stack_phase[3]);
        sum = g1 + g2;
    } else {
        float fp1 = voice->phase * (f1 / f0);
        fp1 -= (float)(int)fp1;
        float fp2 = voice->phase * (f2 / f0);
        fp2 -= (float)(int)fp2;
        sum = plaits_grainlet_value(voice->phase, fp1, carrier_shape,
                                    carrier_bleed_fixed)
            + plaits_grainlet_value(voice->phase, fp2, carrier_shape,
                                    carrier_bleed_fixed);
    }
    /* GrainEngine places a one-pole DC blocker at 0.3 * f0 after the grainlet
     * pair; the grainlets carry a substantial offset otherwise. */
    if (voice->formant_dc) {
        float dc_f = 1.0f - expf(-SF_TWO_PI * 0.3f * f0);
        float hp = sum - *voice->formant_dc;
        *voice->formant_dc += dc_f * hp;
        sum = hp;
    }
    return sum * 1.35f;
}

/* --------------------------------------------------------------- Speech -- */

/* Adapted from Emilie Gillet's Plaits NaiveSpeechSynth: a polyBLEP impulse
 * train, one band-pass colouration filter, then five formant band-passes. The
 * phoneme table is the five-vowel bank from naive_speech_synth.cc; Plaits'
 * SAM/LPC word banks are not carried over. */
static float plaits_this_blep(float t);
static float plaits_next_blep(float t);

static const uint8_t speech_phonemes[5][5][5][2] = {
    {
        { { 74, 255 }, { 83, 114 }, { 97, 90 }, { 98, 90 }, { 100, 25 } },
        { { 75, 255 }, { 84, 128 }, { 100, 114 }, { 101, 101 }, { 103, 20 } },
        { { 76, 255 }, { 85, 128 }, { 100, 18 }, { 102, 16 }, { 104, 3 } },
        { { 79, 255 }, { 85, 161 }, { 101, 25 }, { 104, 4 }, { 110, 0 } },
        { { 79, 255 }, { 85, 128 }, { 101, 6 }, { 106, 25 }, { 110, 0 } }
    },
    {
        { { 67, 255 }, { 91, 64 }, { 98, 90 }, { 101, 64 }, { 102, 32 } },
        { { 67, 255 }, { 92, 51 }, { 99, 64 }, { 103, 51 }, { 105, 25 } },
        { { 69, 255 }, { 93, 51 }, { 100, 32 }, { 102, 25 }, { 103, 25 } },
        { { 67, 255 }, { 91, 16 }, { 100, 8 }, { 103, 4 }, { 110, 0 } },
        { { 65, 255 }, { 95, 25 }, { 101, 45 }, { 105, 2 }, { 110, 0 } }
    },
    {
        { { 59, 255 }, { 92, 8 }, { 99, 40 }, { 102, 20 }, { 104, 10 } },
        { { 61, 255 }, { 94, 45 }, { 101, 32 }, { 103, 25 }, { 105, 8 } },
        { { 60, 255 }, { 93, 16 }, { 101, 16 }, { 104, 4 }, { 105, 4 } },
        { { 65, 255 }, { 92, 25 }, { 100, 8 }, { 105, 4 }, { 110, 0 } },
        { { 60, 255 }, { 96, 64 }, { 101, 12 }, { 106, 12 }, { 110, 1 } }
    },
    {
        { { 67, 255 }, { 78, 72 }, { 98, 22 }, { 99, 25 }, { 101, 2 } },
        { { 67, 255 }, { 79, 80 }, { 99, 64 }, { 101, 64 }, { 102, 12 } },
        { { 68, 255 }, { 79, 80 }, { 100, 12 }, { 102, 20 }, { 103, 5 } },
        { { 69, 255 }, { 79, 90 }, { 101, 40 }, { 104, 10 }, { 110, 0 } },
        { { 69, 255 }, { 79, 72 }, { 101, 20 }, { 106, 20 }, { 110, 0 } }
    },
    {
        { { 65, 255 }, { 74, 25 }, { 98, 6 }, { 100, 10 }, { 101, 4 } },
        { { 65, 255 }, { 74, 25 }, { 100, 36 }, { 101, 51 }, { 103, 12 } },
        { { 66, 255 }, { 75, 25 }, { 100, 18 }, { 102, 8 }, { 104, 5 } },
        { { 63, 255 }, { 77, 64 }, { 99, 8 }, { 104, 2 }, { 110, 0 } },
        { { 63, 255 }, { 77, 40 }, { 100, 4 }, { 106, 2 }, { 110, 0 } }
    }
};

/* stmlib::Svf with the FREQUENCY_DIRTY tangent approximation. */
static float speech_svf_bp(float *state_1, float *state_2, float in,
                           float frequency, float q) {
    const float a = 3.736e-01f * SF_PI * SF_PI * SF_PI;
    float g = frequency * (SF_PI + a * frequency * frequency);
    float r = 1.0f / q;
    float h = 1.0f / (1.0f + r * g + g * g);
    float hp = (in - r * *state_1 - g * *state_1 - *state_2) * h;
    float bp = g * hp + *state_1;
    *state_1 = g * hp + bp;
    float lp = g * bp + *state_2;
    *state_2 = g * bp + lp;
    return bp;
}

/* speech_svf_bp in two halves, the coefficients and the filter, so the
 * coefficients can be kept: the same expressions, so the same values. */
static void speech_svf_coefficients(float frequency, float q, float *g_out,
                                    float *h_out) {
    const float a = 3.736e-01f * SF_PI * SF_PI * SF_PI;
    float g = frequency * (SF_PI + a * frequency * frequency);
    float r = 1.0f / q;
    *g_out = g;
    *h_out = 1.0f / (1.0f + r * g + g * g);
}

static float speech_svf_bp_run(float *state_1, float *state_2, float in,
                               float g, float r, float h) {
    float hp = (in - r * *state_1 - g * *state_1 - *state_2) * h;
    float bp = g * hp + *state_1;
    *state_1 = g * hp + bp;
    float lp = g * bp + *state_2;
    *state_2 = g * bp + lp;
    return bp;
}

/* MiniFreak Speech has two regimes, measured on MiniFreak V with each note
 * retriggered after the knobs were set. From Wave 0.375 up it plays a spoken
 * word once and then falls silent; below that it is a sustained voice. In
 * word mode Timbre stretches the word (2.75x longer at 0, 0.42x at 1, with a
 * plateau across 0.4..0.6) and lifts its formants, and Shape picks one of
 * about ten different words. The word bank itself is not carried over, so every
 * Shape plays the same generic word, whose loudness contour is tabulated at
 * Timbre 0.5, relative to its peak. It is a level envelope only, not audio. */
#define SF_SPEECH_WORD_POINTS 9
static const float speech_word_time[SF_SPEECH_WORD_POINTS] = {
    0.0f, 0.1f, 0.15f, 0.2f, 0.45f, 0.5f, 0.55f, 0.6f, 0.8f
};
static const float speech_word_db[SF_SPEECH_WORD_POINTS] = {
    -27.0f, -18.0f, -5.0f, 0.0f, -4.0f, -14.0f, -21.0f, -29.0f, -33.0f
};
#define SF_SPEECH_STRETCH_POINTS 21
static const float speech_word_stretch[SF_SPEECH_STRETCH_POINTS] = {
    2.75f, 2.42f, 2.17f, 1.92f, 1.67f, 1.5f, 1.33f, 1.17f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 0.92f, 0.75f, 0.67f, 0.58f, 0.58f, 0.5f, 0.42f, 0.42f
};
#ifndef SF_SPEECH_REG_BASE
#define SF_SPEECH_REG_BASE 1.5f
#endif
#ifndef SF_SPEECH_REG_SLOPE
#define SF_SPEECH_REG_SLOPE 0.0f
#endif
#ifndef SF_SPEECH_BREATH
#define SF_SPEECH_BREATH 4.0f
#endif
#ifndef SF_SPEECH_LOW
#define SF_SPEECH_LOW 0.6f
#endif

static int speech_is_word(float wave) {
    return wave >= 0.375f;
}

/* speech_word_level_db in two halves: Timbre's stretch, then the level. The
 * same expressions, so the same values. *done is set once the level has
 * reached the contour's last point. */
static float speech_word_stretch_for(float timbre) {
    float position = sf_clampf(timbre, 0.0f, 1.0f) * (float)(SF_SPEECH_STRETCH_POINTS - 1);
    int i = (int)position;
    if (i > SF_SPEECH_STRETCH_POINTS - 2) i = SF_SPEECH_STRETCH_POINTS - 2;
    return speech_word_stretch[i]
        + (speech_word_stretch[i + 1] - speech_word_stretch[i]) * (position - (float)i);
}

static float speech_word_level_db_stretched(float seconds, float stretch, int *done) {
    float x = seconds / stretch;
    if (x >= speech_word_time[SF_SPEECH_WORD_POINTS - 1]) {
        *done = 1;
        return speech_word_db[SF_SPEECH_WORD_POINTS - 1];
    }
    int k = 1;
    while (x > speech_word_time[k]) ++k;
    float span = speech_word_time[k] - speech_word_time[k - 1];
    float f = (x - speech_word_time[k - 1]) / span;
    return speech_word_db[k - 1] + (speech_word_db[k] - speech_word_db[k - 1]) * f;
}

static float speech_sample(const sf_engine_params_t *params,
                           const sf_engine_voice_t *voice) {
    sf_speech_state_t *state = voice->speech_state;
    if (!state) return 0.0f;

    int trigger = voice->gate && !state->was_gated;
    state->was_gated = voice->gate;
    if (trigger) {
        state->click_left = (int)(0.05f * SF_SAMPLE_RATE);
        state->age = 0;
    } else if (state->age < 0xffffffffu) {
        ++state->age;
    }
    int word = speech_is_word(params->wave);
    float seconds = (float)state->age * (1.0f / SF_SAMPLE_RATE);
    if (state->click_left > 0) --state->click_left;

    float frequency = sf_clampf(voice->increment, 0.000001f, 0.25f);
    if (state->click_left > 0) frequency *= 0.5f;

    /* Oscillator::Render<OSCILLATOR_SHAPE_IMPULSE_TRAIN>. */
    float this_sample = state->pulse_next;
    state->pulse_next = 0.0f;
    state->pulse_phase += frequency;
    if (state->pulse_phase >= 1.0f) {
        state->pulse_phase -= 1.0f;
        float t = state->pulse_phase / frequency;
        this_sample -= plaits_this_blep(t);
        state->pulse_next -= plaits_next_blep(t);
    }
    state->pulse_next += state->pulse_phase;
    state->pulse_lp += 0.25f * ((state->pulse_hp - this_sample) - state->pulse_lp);
    float excitation = 4.0f * state->pulse_lp;
    state->pulse_hp = this_sample;

    float flat_pulse = excitation;
    /* 800 Hz band-pass colouration of the glottal pulse. */
    excitation = speech_svf_bp(&state->coloration_1, &state->coloration_2,
                               excitation, 800.0f / SF_SAMPLE_RATE, 0.5f);
    excitation *= 4.0f;

    /* Type (Wave) chooses the library offset, Word (Shape) the phoneme and
     * Timbre the vocal register, which is how the MiniFreak labels them. */
    sf_speech_cache_t local, *c = voice->speech_cache;
    if (!c) {
        local = (sf_speech_cache_t){0};
        c = &local;
    }
    float p;
    float r;
    if (word) {
        /* Word mode: the vowel walks through the library over the word and
         * Timbre sets its length instead of the vocal register. The walk
         * (0.85 s at stretch 1) and the level contour below both run on the
         * stretched time, so a longer word sounds its vowels for longer. */
        if (!c->stretch_valid || c->stretch_timbre != params->timbre) {
            c->stretch = speech_word_stretch_for(params->timbre);
            c->stretch_timbre = params->timbre;
            c->stretch_valid = 1;
            c->word_done = 0;
        }
        float stretch_seconds = sf_clampf(seconds / c->stretch, 0.0f, 4.0f);
        p = stretch_seconds * 4.0f / 0.85f;
        if (p > 3.999f) p = 3.999f;
        r = SF_SPEECH_REG_BASE + SF_SPEECH_REG_SLOPE * sf_clampf(params->timbre, 0.0f, 1.0f);
    } else {
        if (!c->position_valid || c->wave != params->wave || c->shape != params->shape) {
            c->position = fmodf(params->wave * 4.0f + params->shape * 4.0f, 5.0f);
            c->wave = params->wave;
            c->shape = params->shape;
            c->position_valid = 1;
        }
        /* The library is a ring: from 4 to 5 the last phoneme blends back
         * into the first (see pn below), rather than holding still. */
        p = c->position;
        if (!(p < 5.0f)) p = 0.0f;
        r = sf_clampf(params->timbre, 0.0f, 1.0f) * 3.999f;
    }
    if (p < 0.0f) p = 0.0f;
    int click = state->click_left > 0;

    /* The five band-passes' coefficients move only with the phoneme and
     * register positions and the attack click. */
    if (!c->valid || c->p != p || c->r != r || c->click != click) {
        c->p = p;
        c->r = r;
        c->click = click;
        c->valid = 1;
        int pi = (int)p;
        int pn = pi < 4 ? pi + 1 : 0;
        float pf = p - (float)pi;
        if (!c->corners_valid || c->corner_pi != pi || c->corner_r != r) {
            c->corners_valid = 1;
            c->corner_pi = pi;
            c->corner_r = r;
            int ri = (int)r;
            float rf = r - (float)ri;
            for (int i = 0; i < 5; ++i) {
                float f00 = (float)speech_phonemes[pi][ri][i][0];
                float f01 = (float)speech_phonemes[pi][ri + 1][i][0];
                float f10 = (float)speech_phonemes[pn][ri][i][0];
                float f11 = (float)speech_phonemes[pn][ri + 1][i][0];
                float a00 = (float)speech_phonemes[pi][ri][i][1];
                float a01 = (float)speech_phonemes[pi][ri + 1][i][1];
                float a10 = (float)speech_phonemes[pn][ri][i][1];
                float a11 = (float)speech_phonemes[pn][ri + 1][i][1];
                c->fr0[i] = f00 + (f01 - f00) * rf;
                c->fr1[i] = f10 + (f11 - f10) * rf;
                c->ar0[i] = a00 + (a01 - a00) * rf;
                c->ar1[i] = a10 + (a11 - a10) * rf;
            }
        }
        SF_SCALAR_LOOP for (int i = 0; i < 5; ++i) {
            float fr0 = c->fr0[i];
            float fr1 = c->fr1[i];
            float f = fr0 + (fr1 - fr0) * pf;
            float ar0 = c->ar0[i];
            float ar1 = c->ar1[i];
            c->amp[i] = (ar0 + (ar1 - ar0) * pf) * (1.0f / 256.0f);

            if (f >= 160.0f) f = 160.0f;
            float formant = (55.0f / SF_SAMPLE_RATE)
                          * engine_exp2f_bits((f - 33.0f) * (1.0f / 12.0f));
            if (click && i == 0) formant *= 0.5f;
            speech_svf_coefficients(formant, 20.0f, &c->g[i], &c->h[i]);
        }
    }

    /* The band-passes are independent: four run together, then the fifth;
     * only the sum keeps its order. */
    float band[5];
    {
        float s1v[4], s2v[4], gv[4], hv[4];
        for (int i = 0; i < 4; ++i) {
            s1v[i] = state->formant_1[i];
            s2v[i] = state->formant_2[i];
            gv[i] = c->g[i];
            hv[i] = c->h[i];
        }
        for (int i = 0; i < 4; ++i)
            band[i] = speech_svf_bp_run(&s1v[i], &s2v[i], excitation, gv[i],
                                        1.0f / 20.0f, hv[i]);
        for (int i = 0; i < 4; ++i) {
            state->formant_1[i] = s1v[i];
            state->formant_2[i] = s2v[i];
        }
        band[4] = speech_svf_bp_run(&state->formant_1[4], &state->formant_2[4],
                                    excitation, c->g[4], 1.0f / 20.0f, c->h[4]);
    }
    float out = 0.0f;
    for (int i = 0; i < 5; ++i) out += band[i] * c->amp[i];
    /* The first sustained voice (Wave 0.025..0.2) is a plain formant buzz; the
     * word and the second sustained voice carry a flat, full-range spectrum. */
    if (word || params->wave >= 0.2f)
        out += SF_SPEECH_BREATH * flat_pulse
             + SF_SPEECH_LOW * sinf(SF_TWO_PI * state->pulse_phase);
    if (word) {
        /* The level only falls to its last point as the age grows, so from
         * the age it got there (for this stretch) it stays there. */
        float db;
        if (c->word_done && state->age >= c->done_age) {
            db = speech_word_db[SF_SPEECH_WORD_POINTS - 1];
        } else {
            int done = 0;
            db = speech_word_level_db_stretched(seconds, c->stretch, &done);
            if (done) {
                c->word_done = 1;
                c->done_age = state->age;
            }
        }
        if (!c->level_valid || c->db != db) {
            c->level = engine_exp2f_bits(db * 0.16609640f);
            c->db = db;
            c->level_valid = 1;
        }
        out *= c->level;
    }
    return out;
}

/* ------------------------------------------------------------- V.Analog -- */

/* Adapted from Emilie Gillet's Plaits VirtualAnalogEngine VA_VARIANT 2 and
 * VariableShapeOscillator / VariableSawOscillator, with BLEP helpers from
 * stmlib (MIT); see THIRD_PARTY_NOTICES.md. */
static float plaits_this_blep(float t) {
    return 0.5f * t * t;
}

static float plaits_next_blep(float t) {
    float u = 1.0f - t;
    return -0.5f * u * u;
}

static float plaits_next_integrated_blep(float t) {
    float half = 0.5f * t;
    float square = half * half;
    return 0.1875f - half + 1.5f * square - square * square;
}

static float plaits_this_integrated_blep(float t) {
    return plaits_next_integrated_blep(1.0f - t);
}

static float plaits_vanalog_detuning(float position) {
    static const float intervals[5] = {0.0f, 7.01f, 12.01f, 19.01f, 24.01f};
    float detune = sf_clampf(2.05f * position - 1.025f, -1.0f, 1.0f);
    float sign = detune < 0.0f ? -1.0f : 1.0f;
    float scan = fabsf(detune) * 3.9999f;
    int interval = (int)scan;
    if (interval > 3) interval = 3;
    float fraction = scan - (float)interval;
    fraction = fraction * fraction * (3.0f - 2.0f * fraction);
    fraction = fraction * fraction * (3.0f - 2.0f * fraction);
    return sign * (intervals[interval]
        + (intervals[interval + 1] - intervals[interval]) * fraction);
}

/* The variable saw's per-pitch set-up: frequency and pulse width clamped as
 * the oscillator needs them. */
static float plaits_variable_saw_frequency(float frequency) {
    if (frequency >= 0.25f) frequency = 0.25f;
    if (frequency < 0.00001f) frequency = 0.00001f;
    return frequency;
}

static float plaits_variable_saw_pw(float frequency, float pulse_width) {
    if (frequency >= 0.25f) return 0.5f;
    return sf_clampf(pulse_width, frequency * 2.0f, 1.0f - frequency * 2.0f);
}

/* frequency and pulse_width come from the two functions above, the slopes
 * are 1 / pulse_width and 1 / (1 - pulse_width), and the amounts are the
 * clamped waveshape and 1 less it. */
static float plaits_variable_saw_tick(sf_plaits_variable_saw_state_t *state,
                                      float frequency, float pulse_width,
                                      float triangle_amount, float notch_amount,
                                      float slope_up, float slope_down) {
    const float notch_depth = 0.2f;
    float phase = state->phase + frequency;
    float sample = state->next_sample;
    float next_sample = 0.0f;

    if (!state->high && phase >= pulse_width) {
        float triangle_step = (slope_up + slope_down) * frequency * triangle_amount;
        float notch = (notch_depth + 1.0f - pulse_width) * notch_amount;
        float t = (phase - pulse_width)
                / (state->previous_pw - pulse_width + frequency);
        sample += notch * plaits_this_blep(t);
        next_sample += notch * plaits_next_blep(t);
        sample -= triangle_step * plaits_this_integrated_blep(t);
        next_sample -= triangle_step * plaits_next_integrated_blep(t);
        state->high = 1;
    } else if (phase >= 1.0f) {
        phase -= 1.0f;
        float triangle_step = (slope_up + slope_down) * frequency * triangle_amount;
        float notch = (notch_depth + 1.0f) * notch_amount;
        float t = phase / frequency;
        sample -= notch * plaits_this_blep(t);
        next_sample -= notch * plaits_next_blep(t);
        sample += triangle_step * plaits_this_integrated_blep(t);
        next_sample += triangle_step * plaits_next_integrated_blep(t);
        state->high = 0;
    }

    float triangle = phase < pulse_width
        ? phase * slope_up : 1.0f - (phase - pulse_width) * slope_down;
    float notch_saw = phase < pulse_width ? phase : 1.0f + notch_depth;
    next_sample += notch_saw * notch_amount + triangle * triangle_amount;
    state->phase = phase;
    state->next_sample = next_sample;
    state->previous_pw = pulse_width;
    return (2.0f * sample - 1.0f) / (1.0f + notch_depth);
}

/* The square's frequencies take plaits_variable_saw_frequency's clamp and
 * its pulse width plaits_variable_saw_pw's, against the slave frequency. */
static float plaits_variable_shape_square_tick(sf_plaits_sync_state_t *state,
                                               float master_phase,
                                               float master_frequency,
                                               float slave_frequency,
                                               float pulse_width) {
    float phase = state->slave_phase;
    float sample = state->next_sample;
    float next_sample = 0.0f;
    float master_next = master_phase + master_frequency;
    int reset = master_next >= 1.0f;
    int transition_during_reset = 0;
    float reset_time = 0.0f;

    if (reset) {
        master_next -= 1.0f;
        reset_time = master_next / master_frequency;
        float at_reset = phase + (1.0f - reset_time) * slave_frequency;
        if (at_reset >= 1.0f) {
            at_reset -= 1.0f;
            transition_during_reset = 1;
        }
        if (!state->high && at_reset >= pulse_width) transition_during_reset = 1;
        float reset_value = at_reset < pulse_width ? 0.0f : 1.0f;
        sample -= reset_value * plaits_this_blep(reset_time);
        next_sample -= reset_value * plaits_next_blep(reset_time);
    }

    phase += slave_frequency;
    while (transition_during_reset || !reset) {
        if (!state->high) {
            if (phase < pulse_width) break;
            float t = (phase - pulse_width)
                    / (state->previous_pw - pulse_width + slave_frequency);
            sample += plaits_this_blep(t);
            next_sample += plaits_next_blep(t);
            state->high = 1;
        }
        if (state->high) {
            if (phase < 1.0f) break;
            phase -= 1.0f;
            float t = phase / slave_frequency;
            sample -= plaits_this_blep(t);
            next_sample -= plaits_next_blep(t);
            state->high = 0;
        }
    }
    if (reset) {
        phase = reset_time * slave_frequency;
        state->high = 0;
    }
    next_sample += phase < pulse_width ? 0.0f : 1.0f;
    state->slave_phase = phase;
    state->next_sample = next_sample;
    state->previous_pw = pulse_width;
    return 2.0f * sample - 1.0f;
}

static float vanalog_sample(const sf_engine_params_t *params,
                            const sf_engine_voice_t *voice) {
    sf_vanalog_state_t *state = voice->vanalog_state;
    if (!state) {
        sf_engine_params_t remapped;
        const sf_engine_params_t *p = remap_params(params, &g_remap_vanalog, &remapped);
        return standard_wave_sample(voice->phase, voice->increment,
                                    sf_clampf(p->wave, 0.0f, 1.0f));
    }
    /* The coefficients only move with the knobs and the pitch, so they are
     * worked out when those change (see sf_vanalog_cache_t). */
    sf_vanalog_cache_t local, *c = voice->vanalog_cache;
    if (!c) {
        local = (sf_vanalog_cache_t){0};
        c = &local;
    }
    int knobs = !c->valid || c->wave != params->wave || c->timbre != params->timbre
             || c->shape != params->shape;
    if (knobs) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        sf_engine_params_t remapped;
        const sf_engine_params_t *p = remap_params(params, &g_remap_vanalog, &remapped);
        /* Plaits VA_VARIANT 2: Shape drives a variable pulse/hard-sync
         * oscillator, Wave controls the variable saw, and Timbre selects the
         * saw/pulse interval. */
        float wave = sf_clampf(p->wave, 0.0f, 1.0f);
        float timbre = sf_clampf(p->timbre, 0.0f, 1.0f);
        float shape = sf_clampf(p->shape, 0.0f, 1.0f);
        float detune = plaits_vanalog_detuning(timbre);
        c->saw_ratio = engine_exp2f(detune / 12.0f);

        /* Plaits VA_VARIANT 2 morph mapping: triangle/notch-saw crossover. */
        float saw_morph = wave;
        float saw_pulse_width = saw_morph < 0.5f
            ? saw_morph + 0.5f : 1.0f - (saw_morph - 0.5f) * 2.0f;
        saw_pulse_width = sf_clampf(saw_pulse_width * 1.1f, 0.005f, 1.0f);
        float saw_shape = sf_clampf(10.0f - 21.0f * saw_morph, 0.0f, 1.0f);
        float saw_gain = sf_clampf(8.0f * (1.0f - saw_morph), 0.02f, 1.0f);
        c->saw_pulse_width = saw_pulse_width;
        c->triangle_amount = sf_clampf(saw_shape, 0.0f, 1.0f);
        c->notch_amount = 1.0f - c->triangle_amount;
        c->saw_gain = saw_gain;
        /* The slopes for a pulse width the pitch does not clamp. */
        c->saw_slope_up = 1.0f / saw_pulse_width;
        c->saw_slope_down = 1.0f / (1.0f - saw_pulse_width);

        float square_pulse_width = sf_clampf(1.3f * shape - 0.15f, 0.005f, 0.5f);
        float square_gain = sf_clampf(shape * 8.0f, 0.0f, 1.0f);
        /* Shape's upper half runs the pulse into hard sync. In the reference
         * the square plateau extends slightly past the midpoint (~0.535)
         * before hard sync begins, and the sync interval then climbs at ~255
         * semitones per unit of Shape (reaching ~2x at 0.58, ~3x at 0.62, and
         * ~6x at 0.66). Gentle gain compensation tames the resonant formant
         * peak so it sits in the same balance against the fundamental as the
         * reference. */
        float square_sync_semitones = shape <= 0.545f
            ? 0.0f : (shape - 0.545f) * 280.0f;
        square_gain /= (1.0f + 0.025f * square_sync_semitones);
        c->square_pulse_width = square_pulse_width;
        c->square_gain = square_gain;
        c->square_ratio = engine_exp2f(square_sync_semitones / 12.0f);
        c->normalization = 1.0f / fmaxf(square_gain, saw_gain);
    }
    if (knobs || c->increment != voice->increment) {
        c->increment = voice->increment;
        float saw_increment = sf_clampf(voice->increment * c->saw_ratio, 0.00001f, 0.25f);
        float square_increment = sf_clampf(voice->increment * c->square_ratio,
                                           0.00001f, 0.25f);
        /* Both increments are already within plaits_variable_saw_frequency's
         * clamp. */
        float frequency = saw_increment;
        float pw = plaits_variable_saw_pw(frequency, c->saw_pulse_width);
        c->saw_frequency = frequency;
        c->saw_pw = pw;
        if (pw == c->saw_pulse_width) {
            c->slope_up = c->saw_slope_up;
            c->slope_down = c->saw_slope_down;
        } else {
            c->slope_up = 1.0f / pw;
            c->slope_down = 1.0f / (1.0f - pw);
        }
        c->master_frequency = plaits_variable_saw_frequency(voice->increment);
        c->slave_frequency = square_increment;
        c->square_pw = plaits_variable_saw_pw(c->slave_frequency, c->square_pulse_width);
    }
    c->valid = 1;

    if (!state->initialized) {
        state->saw.phase = voice->stack_phase ? voice->stack_phase[0] : voice->phase;
        state->saw.previous_pw = 0.5f;
        state->sync.slave_phase = voice->stack_phase ? voice->stack_phase[1] : voice->phase;
        state->sync.previous_pw = 0.5f;
        state->initialized = 1;
    }
    float saw = plaits_variable_saw_tick(&state->saw, c->saw_frequency, c->saw_pw,
                                         c->triangle_amount, c->notch_amount,
                                         c->slope_up, c->slope_down);
    float pulse = plaits_variable_shape_square_tick(&state->sync, voice->phase,
                                                   c->master_frequency,
                                                   c->slave_frequency,
                                                   c->square_pw);
    if (voice->stack_phase) {
        voice->stack_phase[0] = state->saw.phase;
        voice->stack_phase[1] = state->sync.slave_phase;
    }

    float saw_gain = c->saw_gain;
    float square_gain = c->square_gain;
    float normalization = c->normalization;
    return saw * saw_gain * 0.66f * normalization
         + pulse * square_gain * 0.40f * normalization;
}

/* ----------------------------------------------------------- Waveshaper -- */

/* Adapted from Emilie Gillet's Plaits WaveshapingEngine and Oscillator slope
 * shape (plaits/dsp/engine/waveshaping_engine.cc, plaits/dsp/oscillator/
 * oscillator.h; MIT). Wave = waveshaper curve, Timbre = wavefolder amount,
 * Shape = slope asymmetry (pulse width plus the fold bias it induces). */
static float plaits_tame(float f0, float harmonics, float order) {
    f0 *= harmonics;
    float max_f = 0.5f / order;
    /* At or below max_f the amount is at least 1 and clamps to 1: skip the
     * division. */
    if (f0 <= max_f) return 1.0f;
    float max_amount = 1.0f - (f0 - max_f) / (0.5f - max_f);
    max_amount = sf_clampf(max_amount, 0.0f, 1.0f);
    return max_amount * max_amount * max_amount;
}

/* Bandlimited variable-slope oscillator (Oscillator::Render<SLOPE>). Returns
 * the current sample. The rising/falling segment is derived from the phase, so
 * the oscillator needs no separate high/low latch. */
/* The slope oscillator's pulse width for a frequency already clamped to
 * 0.000001..0.25 (as the oscillator clamps it). */
static float plaits_slope_pw(float frequency, float pw) {
    return sf_clampf(pw, fabsf(frequency) * 2.0f, 1.0f - 2.0f * fabsf(frequency));
}

/* frequency clamped to 0.000001..0.25, pw from plaits_slope_pw, and the
 * slopes 1 / pw and 1 / (1 - pw). */
static inline float plaits_slope_tick(float *phase, float *next_sample,
                               float frequency, float pw,
                               float slope_up, float slope_down) {
    float this_sample = *next_sample;
    float next = 0.0f;
    int rising = *phase < pw;
    *phase += frequency;
    if (rising ^ (*phase < pw)) {
        float t = (*phase - pw) / frequency;
        t = sf_clampf(t, 0.0f, 1.0f);
        float discontinuity = (slope_up + slope_down) * frequency;
        this_sample -= plaits_this_integrated_blep(t) * discontinuity;
        next -= plaits_next_integrated_blep(t) * discontinuity;
        rising = *phase < pw;
    }
    if (*phase >= 1.0f) {
        *phase -= 1.0f;
        float t = sf_clampf(*phase / frequency, 0.0f, 1.0f);
        float discontinuity = (slope_up + slope_down) * frequency;
        this_sample += plaits_this_integrated_blep(t) * discontinuity;
        next += plaits_next_integrated_blep(t) * discontinuity;
        rising = 1;
    }
    next += rising ? (*phase * slope_up)
                   : (1.0f - (*phase - pw) * slope_down);
    *next_sample = next;
    return 2.0f * this_sample - 1.0f;
}

static float plaits_interpolate_hermite(const float *table, float index,
                                        float size) {
    index *= size;
    int integral = (int)index;
    float fractional = index - (float)integral;
    float xm1 = table[integral];
    float x0  = table[integral + 1];
    float x1  = table[integral + 2];
    float x2  = table[integral + 3];
    float c = (x1 - xm1) * 0.5f;
    float v = x0 - x1;
    float w = c + v;
    float a = w + v + (x2 - x0) * 0.5f;
    float b_neg = w + a;
    return (((a * fractional) - b_neg) * fractional + c) * fractional + x0;
}

static float waveshaper_sample(const sf_engine_params_t *params,
                               const sf_engine_voice_t *voice) {
    sf_vanalog_state_t *state = voice->vanalog_state;
    if (!state) return 0.0f;
    /* The coefficients only move with the knobs and the pitch, so they are
     * worked out when those change (see sf_waveshaper_cache_t). */
    sf_waveshaper_cache_t local, *c = voice->waveshaper_cache;
    if (!c) {
        local = (sf_waveshaper_cache_t){0};
        c = &local;
    }
    int knobs = !c->valid || c->wave != params->wave || c->timbre != params->timbre
             || c->shape != params->shape;
    if (knobs) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        float morph = sf_clampf(params->shape, 0.0f, 1.0f);
        float harmonics = sf_clampf(params->wave, 0.0f, 1.0f);
        float timbre = sf_clampf(params->timbre, 0.0f, 1.0f);
        /* Start from a bandlimited variable-slope signal; Shape sets its
         * pulse width (the "Asym" knob). */
        float pw = morph * 0.45f + 0.5f;
        c->harmonics = harmonics;
        c->pw = pw;
        c->slope = 3.0f + fabsf(morph - 0.5f) * 5.0f;
        c->shape_amount = fabsf(harmonics - 0.5f) * 2.0f;
        c->wavefolder_gain = timbre;
        /* The slopes for a pulse width the pitch does not clamp. */
        c->slope_up = 1.0f / pw;
        c->slope_down = 1.0f / (1.0f - pw);
    }
    if (knobs || c->increment != voice->increment) {
        c->increment = voice->increment;
        float f0 = sf_clampf(voice->increment, 0.000001f, 0.25f);
        float slope = c->slope;
        float shape_amount = c->shape_amount;
        float shape_amount_attenuation = plaits_tame(f0, slope, 16.0f);
        float wavefolder_gain_attenuation = plaits_tame(
            f0, slope * (3.0f + shape_amount * shape_amount_attenuation * 5.0f),
            12.0f);
        float harmonics = c->harmonics;
        float wavefolder_gain = c->wavefolder_gain;
        float shape_mod = 0.5f + (harmonics - 0.5f) * shape_amount_attenuation;
        c->wf_gain = 0.03f + 0.46f * wavefolder_gain
                   * wavefolder_gain_attenuation;
        float shape = shape_mod * 3.9999f;
        int shape_integral = (int)shape;
        if (shape_integral > 3) shape_integral = 3;
        c->shape_integral = shape_integral;
        c->shape_fractional = shape - (float)shape_integral;
        /* f0 is already in the slope oscillator's range. */
        float pw = plaits_slope_pw(f0, c->pw);
        c->f0 = f0;
        c->tick_pw = pw;
        if (pw == c->pw) {
            c->tick_slope_up = c->slope_up;
            c->tick_slope_down = c->slope_down;
        } else {
            c->tick_slope_up = 1.0f / pw;
            c->tick_slope_down = 1.0f / (1.0f - pw);
        }
    }
    c->valid = 1;

    float f0 = c->f0;
    float slope_out = plaits_slope_tick(&state->saw.phase, &state->saw.next_sample,
                                        f0, c->tick_pw,
                                        c->tick_slope_up, c->tick_slope_down);
    /* The triangle operator only feeds the unused auxiliary output, but it is
     * kept in step with the master phase so the engine stays a faithful port.
     * Its pulse width 0.5 is never clamped (f0 <= 0.25), so its slopes are
     * exactly 2. */
    plaits_slope_tick(&state->sync.slave_phase, &state->sync.next_sample,
                      f0, 0.5f, 2.0f, 2.0f);

    int shape_integral = c->shape_integral;
    float shape_fractional = c->shape_fractional;
    const int16_t *shape_1 = plaits_lut_ws_table[shape_integral];
    const int16_t *shape_2 = plaits_lut_ws_table[shape_integral + 1];

    float ws_index = 127.0f * slope_out + 128.0f;
    int ws_integral = (int)ws_index;
    float ws_fractional = ws_index - (float)ws_integral;
    ws_integral &= 255;

    float x0 = (float)shape_1[ws_integral] * (1.0f / 32768.0f);
    float x1 = (float)shape_1[ws_integral + 1] * (1.0f / 32768.0f);
    float x = x0 + (x1 - x0) * ws_fractional;
    float y0 = (float)shape_2[ws_integral] * (1.0f / 32768.0f);
    float y1 = (float)shape_2[ws_integral + 1] * (1.0f / 32768.0f);
    float y = y0 + (y1 - y0) * ws_fractional;

    float mix = x + (y - x) * shape_fractional;
    float index = sf_clampf(mix * c->wf_gain + 0.5f, 0.001f, 0.999f);
    return plaits_interpolate_hermite(plaits_lut_fold + 1, index, 512.0f) * 1.35f;
}

/* ------------------------------------------------------- Karplus-Strong -- */

/* Adapted from Emilie Gillet's Plaits StringVoice / String (stmlib Hermite
 * delay-line interpolation, brightness-shaped strike/dust exciter, and
 * damping loop filter). */
static float plaits_delay_read_hermite(const float *buf, unsigned size,
                                       unsigned write_pos, float delay) {
    float read_f = (float)write_pos - delay;
    while (read_f < 0.0f) read_f += (float)size;
    unsigned integral = (unsigned)read_f;
    float fractional = read_f - (float)integral;
    unsigned mask = size - 1u;
    float xm1 = buf[(integral - 1u) & mask];
    float x0  = buf[integral & mask];
    float x1  = buf[(integral + 1u) & mask];
    float x2  = buf[(integral + 2u) & mask];
    float c = (x1 - xm1) * 0.5f;
    float v = x0 - x1;
    float w = c + v;
    float a = w + v + (x2 - x0) * 0.5f;
    float b_neg = w + a;
    return (((a * fractional) - b_neg) * fractional + c) * fractional + x0;
}

/* MiniFreak V's Karplus Strong, measured on held notes: Bow (Wave) is a
 * continuous, pitched excitation under the pluck, Position (Timbre) colours the
 * strike, and Decay (Shape) sets a loss per period of the string, so higher
 * notes die faster in time. Bow 0 is a bare pluck that decays to silence; a
 * bowed string settles on a plateau that rises with Bow and with Decay and
 * saturates about 8 dB over the pluck's peak. The strike is the same burst
 * on every note, and the string loses its partials almost evenly, with only
 * a gentle extra loss up top that grows as Decay comes down. */
#define SF_KP_BOW_GAIN 0.0009f
#define SF_KP_BOW_DECAY_DB 10.0f
#define SF_KP_BOW_LP 800.0f
#define SF_KP_BOW_LP_OCT 4.0f
#define SF_KP_HP_HARM 1.0f
#define SF_KP_BOW_NOISE 20.0f
#define SF_KP_CLIP 1.0f
#define SF_KP_OUT 3.2f
#define SF_KP_STRIKE 0.55f
#define SF_KP_MAX_DB_S 115.0f
#define SF_KP_LP_C0 42.0f
#define SF_KP_LP_C1 72.0f
#define SF_KP_EXC_HARM 3.0f
#define SF_KP_POS0 0.25f
#define SF_KP_POS_OCT 2.0f
/* Fundamental loss per period in dB at Decay 0, 0.1, ... 1. */
static const float g_karplus_loss_db[11] = {
    0.83f, 0.71f, 0.57f, 0.44f, 0.32f, 0.22f, 0.138f, 0.075f, 0.034f, 0.0099f, 0.003f
};

/* The strike burst: white noise hashed from the sample index, so every note
 * gets the same burst and the strike-position comb can read it back. */
static float karplus_burst(unsigned n) {
    uint32_t x = (uint32_t)n * 0x9e3779b9u + 0x7f4a7c15u;
    x ^= x >> 16; x *= 0x85ebca6bu; x ^= x >> 13; x *= 0xc2b2ae35u; x ^= x >> 16;
    return (float)(x >> 8) * (1.0f / 8388607.5f) - 1.0f;
}

/* The excitation lowpass's coefficient for a cutoff. */
static float karplus_exc_alpha(float exc_cutoff) {
    exc_cutoff = sf_clampf(exc_cutoff, 80.0f, 0.45f * SF_SAMPLE_RATE);
    return 2.0f * SF_PI * exc_cutoff / (SF_SAMPLE_RATE + 2.0f * SF_PI * exc_cutoff);
}

static float karplus_sample(const sf_engine_params_t *params,
                            const sf_engine_voice_t *voice) {
    if (!voice->delay_buf || !voice->delay_pos || !voice->delay_low
        || !voice->excitation_left) return 0.0f;
    unsigned size = (unsigned)SF_VOICE_DELAY;
    /* The coefficients only move with the knobs and the pitch, so they are
     * worked out when those change (see sf_karplus_cache_t). */
    sf_karplus_cache_t local, *c = voice->karplus_cache;
    if (!c) {
        local = (sf_karplus_cache_t){0};
        c = &local;
    }
    int knobs = !c->valid || c->wave != params->wave || c->timbre != params->timbre
             || c->shape != params->shape;
    if (knobs) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        float bow = sf_clampf(params->wave, 0.0f, 1.0f);
        float position = sf_clampf(params->timbre, 0.0f, 1.0f);
        float decay = sf_clampf(params->shape, 0.0f, 1.0f);
        /* Loop lowpass, high above the note: the extra top-end loss. */
        c->lp_mult = engine_exp2f((SF_KP_LP_C0 + SF_KP_LP_C1 * decay) / 12.0f);
        /* Loop gain: the measured loss per period, less what the lowpass
         * already takes from the fundamental (at the pitch level). */
        float di = decay * 10.0f;
        int i = (int)di;
        if (i > 9) i = 9;
        c->loss_table = g_karplus_loss_db[i]
                      + (g_karplus_loss_db[i + 1] - g_karplus_loss_db[i]) * (di - (float)i);
        c->fraction = SF_KP_POS0 * engine_exp2f(-(position - 0.25f) * SF_KP_POS_OCT);
        c->alpha_bow = karplus_exc_alpha(SF_KP_BOW_LP * engine_exp2f(bow * SF_KP_BOW_LP_OCT));
        c->bow = bow;
        c->bow_q = sqrtf(sqrtf(bow));   /* times bow: bow^1.25 */
        c->bow_x = engine_exp2f(decay * SF_KP_BOW_DECAY_DB * 0.16609640f);
    }
    if (knobs || c->increment != voice->increment) {
        c->increment = voice->increment;
        float f0 = sf_clampf(voice->increment, 20.0f / SF_SAMPLE_RATE, 0.25f);
        float base_hz = f0 * SF_SAMPLE_RATE;
        float delay_f = sf_clampf(1.0f / f0, 4.0f, (float)size - 4.0f);
        float loop_cutoff = sf_clampf(base_hz * c->lp_mult, 40.0f, 0.49f * SF_SAMPLE_RATE);
        float loop_alpha = sf_clampf(
            2.0f * SF_PI * loop_cutoff / (SF_SAMPLE_RATE + 2.0f * SF_PI * loop_cutoff),
            0.001f, 0.995f);
        float phase_delay = (1.0f - loop_alpha) / fmaxf(loop_alpha, 0.05f);
        if (phase_delay > 2.0f) phase_delay = 2.0f;
        float loss_db = fminf(c->loss_table, SF_KP_MAX_DB_S / base_hz);
        float ratio = base_hz / loop_cutoff;
        float lp_gain = 1.0f / sqrtf(1.0f + ratio * ratio);
        unsigned tap = (unsigned)(delay_f * c->fraction + 0.5f);
        c->f0 = f0;
        c->inv_f0 = 1.0f / f0;
        c->hp = f0 * SF_KP_HP_HARM * 2.0f * SF_PI;
        c->loop_alpha = loop_alpha;
        c->tuned_delay = sf_clampf(delay_f - phase_delay, 4.0f, (float)size - 4.0f);
        c->loop_gain = sf_clampf(engine_exp2f(-loss_db * 0.16609640f) / lp_gain, 0.0f, 0.99999f);
        c->tap = tap < 1u ? 1u : tap;
        c->alpha_strike = karplus_exc_alpha(base_hz * SF_KP_EXC_HARM * SF_KP_POS0 * 1.41421356f
                                            / c->fraction);
    }
    c->valid = 1;

    unsigned pos = *voice->delay_pos & (size - 1u);
    float delayed = plaits_delay_read_hermite(voice->delay_buf, size, pos, c->tuned_delay);

    /* Excitation: the strike burst, then the bow's noise, through one
     * lowpass (the strike's while it lasts, then the bow's) and a highpass
     * under the note that keeps the bow from rumbling at DC. */
    float raw = 0.0f;
    float exc_alpha = c->alpha_bow;
    if (*voice->excitation_left > 0) {
        /* Strike: a comb at the strike position, which slides its first peak
         * from the 2nd partial at Position 0 toward the 11th at 1; the strike
         * lowpass opens with it so the strike keeps its level. */
        unsigned n = *voice->excitation_left;
        raw = (karplus_burst(n) - karplus_burst(n + c->tap)) * SF_KP_STRIKE;
        --(*voice->excitation_left);
        exc_alpha = c->alpha_strike;
    }
    if (voice->rng && voice->gate && c->bow > 0.0f) {
        /* Bow: a pulse once a period (flat harmonics of the same level at
         * any pitch) roughened with noise, so the string sustains a pitched
         * tone rather than a filtered hiss. */
        float pulse = (voice->phase < c->f0 ? c->inv_f0 : 0.0f) - 1.0f;
        raw += (pulse + SF_KP_BOW_NOISE * noise_sample(voice->rng))
             * SF_KP_BOW_GAIN * c->bow * c->bow_q * c->bow_x;
    }
    float excitation = 0.0f;
    if (voice->noise_state) {
        *voice->noise_state += exc_alpha * (raw - *voice->noise_state);
        excitation = *voice->noise_state;
        if (voice->formant_dc) {
            *voice->formant_dc += c->hp * (excitation - *voice->formant_dc);
            excitation -= *voice->formant_dc;
        }
    }

    *voice->delay_low += c->loop_alpha * (delayed - *voice->delay_low);
    float s = sf_clampf(excitation + *voice->delay_low * c->loop_gain, -4.0f, 4.0f);
    voice->delay_buf[pos] = s;
    *voice->delay_pos = (pos + 1u) & (size - 1u);
    /* Soft saturation caps a hard-bowed, long-decay string. */
    float out = s * SF_KP_OUT;
    return out / (1.0f + fabsf(out) * (1.0f / SF_KP_CLIP));
}

/* ---------------------------------------------------------------- Modal -- */

/* stmlib::OnePole::tan<FREQUENCY_FAST>, the approximation Plaits uses for the
 * ResonatorSvf coefficients. */
static float modal_tan_fast(float f) {
    const float a = 3.260e-01f * SF_PI * SF_PI * SF_PI;
    const float b = 1.823e-01f * SF_PI * SF_PI * SF_PI * SF_PI * SF_PI;
    float f2 = f * f;
    return f * (SF_PI + f2 * (a + b * f2));
}

static float modal_semitones_to_ratio(float semitones) {
    return engine_exp2f(semitones * (1.0f / 12.0f));
}

static float modal_lut_stiffness(float structure) {
    float index = sf_clampf(structure, 0.0f, 1.0f) * 64.0f;
    int i = (int)index;
    if (i > 63) i = 63;
    float fraction = index - (float)i;
    return plaits_lut_stiffness[i]
         + (plaits_lut_stiffness[i + 1] - plaits_lut_stiffness[i]) * fraction;
}

/* modal_sample with its coefficients kept in a sf_modal_cache_t: the same
 * expressions in the same order, so the same output. */
static float modal_sample_cached(const sf_engine_params_t *params,
                                 const sf_engine_voice_t *voice,
                                 sf_modal_cache_t *c) {
    sf_modal_state_t *state = voice->modal_state;
    const float accent = 0.8f;
    float structure = sf_clampf(params->wave, 0.0f, 1.0f);
    int trigger = voice->gate && !state->was_gated;
    state->was_gated = voice->gate;
    if (trigger) state->harmonics_lp = structure;
    state->harmonics_lp += 0.01f * (structure - state->harmonics_lp);

    /* Knob level: everything but the pitch. The structure smoother settles
     * on a fixed value, so this holds still a few ms after Wave does. */
    int knobs = !c->valid || c->timbre != params->timbre || c->shape != params->shape
             || c->harmonics_lp != state->harmonics_lp;
    if (knobs) {
        c->timbre = params->timbre;
        c->shape = params->shape;
        c->harmonics_lp = state->harmonics_lp;
        float brightness = sf_clampf(params->timbre, 0.0f, 1.0f);
        float damping = sf_clampf(params->shape, 0.0f, 1.0f);
        brightness += 0.25f * accent * (1.0f - brightness);
        damping += 0.25f * accent * (1.0f - damping);
        c->brightness = brightness;
        c->damping = damping;
        const float range = 60.0f;
        c->cutoff_ratio = modal_semitones_to_ratio(
            (brightness * (2.0f - brightness) - 0.5f) * range);
        const float stiffness0 = modal_lut_stiffness(state->harmonics_lp);
        float stiffness = stiffness0;
        float stretch_base = 1.0f;
        for (int i = 0; i < 2; ++i) {
            stretch_base += stiffness;
            stiffness *= stiffness < 0.0f ? 0.93f : 0.98f;
        }
        c->stretch_base = stretch_base;
        stiffness = stiffness0;
        float stretch_factor = 1.0f;
        float q_sqrt = modal_semitones_to_ratio(damping * 79.7f);
        float q = 500.0f * (SF_SAMPLE_RATE / 48000.0f) * q_sqrt * q_sqrt;
        float mode_brightness = brightness * (1.0f - state->harmonics_lp * 0.3f);
        mode_brightness *= 1.0f - damping * 0.3f;
        float q_loss = mode_brightness * (2.0f - mode_brightness) * 0.85f + 0.15f;
        for (int i = 0; i < SF_MODAL_MODES; ++i) {
            c->stretch[i] = stretch_factor;
            c->q[i] = q;
            stretch_factor += stiffness;
            stiffness *= stiffness < 0.0f ? 0.93f : 0.98f;
            q *= q_loss;
        }
    }
    if (knobs || c->increment != voice->increment) {
        c->increment = voice->increment;
        const float f = 2.0f * voice->increment;
        float cutoff = f * c->cutoff_ratio;
        if (cutoff > 0.499f) cutoff = 0.499f;
        c->cutoff = cutoff;
        {
            const float exc_q = 1.5f;
            float g = modal_tan_fast(cutoff);
            float r = 1.0f / exc_q;
            c->exc_g = g;
            c->exc_h = 1.0f / (1.0f + r * g + g * g);
        }
        float f0 = voice->increment;
        f0 /= c->stretch_base;
        float harmonic = f0;
        float frequencies[SF_MODAL_MODES];
        for (int i = 0; i < SF_MODAL_MODES; ++i) {
            float mode_frequency = harmonic * c->stretch[i];
            if (mode_frequency >= 0.499f) mode_frequency = 0.499f;
            frequencies[i] = mode_frequency;
            harmonic += f0;
        }
        /* No carried dependency here, so this loop vectorizes. */
        for (int i = 0; i < SF_MODAL_MODES; ++i) {
            float mode_frequency = frequencies[i];
            float mode_attenuation = 1.0f - mode_frequency * 2.0f;
            float mode_q = 1.0f + mode_frequency * c->q[i];
            float g = modal_tan_fast(mode_frequency);
            float r = 1.0f / mode_q;
            c->g[i] = g;
            c->rg[i] = r + g;
            c->h[i] = 1.0f / (1.0f + r * g + g * g);
            c->gain[i] = plaits_modal_amplitudes[i] * mode_attenuation;
        }
    }
    c->valid = 1;

    float temp = 0.0f;
    if (trigger) {
        float attenuation = 1.0f - c->damping * 0.5f;
        float amplitude = (0.12f + 0.08f * accent) * attenuation;
        temp = amplitude * modal_semitones_to_ratio(c->cutoff * c->cutoff * 24.0f)
             / c->cutoff;
    }
    {
        const float exc_q = 1.5f;
        float g = c->exc_g;
        float r = 1.0f / exc_q;
        float h = c->exc_h;
        float hp = (temp - r * state->excitation_1 - g * state->excitation_1
                    - state->excitation_2) * h;
        float bp = g * hp + state->excitation_1;
        state->excitation_1 = g * hp + bp;
        float lp = g * bp + state->excitation_2;
        state->excitation_2 = g * bp + lp;
        temp = lp;
    }

    /* The resonators are independent, so they run four at a time; only the
     * sum has to keep its order. */
    float band[SF_MODAL_MODES];
    for (int bank = 0; bank < SF_MODAL_BANKS; ++bank) {
        /* Local copies: the compiler cannot tell the state from the cache
         * otherwise, and would not run the four lanes together. */
        float s1v[4], s2v[4], gv[4], rgv[4], hv[4];
        for (int slot = 0; slot < 4; ++slot) {
            s1v[slot] = state->mode_state_1[bank][slot];
            s2v[slot] = state->mode_state_2[bank][slot];
            gv[slot] = c->g[bank * 4 + slot];
            rgv[slot] = c->rg[bank * 4 + slot];
            hv[slot] = c->h[bank * 4 + slot];
        }
        for (int slot = 0; slot < 4; ++slot) {
            float s1 = s1v[slot];
            float s2 = s2v[slot];
            float g = gv[slot];
            float hp = (temp - rgv[slot] * s1 - s2) * hv[slot];
            float bp = g * hp + s1;
            s1 = g * hp + bp;
            float lp = g * bp + s2;
            s2 = g * bp + lp;
            s1v[slot] = s1;
            s2v[slot] = s2;
            band[bank * 4 + slot] = bp;
        }
        for (int slot = 0; slot < 4; ++slot) {
            state->mode_state_1[bank][slot] = s1v[slot];
            state->mode_state_2[bank][slot] = s2v[slot];
        }
    }
    float out = 0.0f;
    for (int i = 0; i < SF_MODAL_MODES; ++i) out += c->gain[i] * band[i];
    return out;
}

static float modal_sample(const sf_engine_params_t *params,
                          const sf_engine_voice_t *voice) {
    if (voice->modal_state && voice->modal_cache)
        return modal_sample_cached(params, voice, voice->modal_cache);
    /* Plaits ModalVoice, struck: mallet click -> excitation LPF -> a bank of
     * inharmonic modes. Wave is the structure/inharmonicity, Timbre the
     * exciter brightness and Shape the damping that sets how long the modes
     * ring.
     *
     * Measured on MiniFreak V: its Modal is this voice with a fixed accent of
     * about 0.8 (velocity does not change it) and 12 modes. The accent lifts
     * damping to 0.2 + 0.8 Shape, so the first mode loses 44 dB/s at Shape
     * 0.3 and the rate halves every 0.094 of Shape; it lifts brightness to
     * 0.2 + 0.8 Timbre, so Timbre 0 still rings three modes. Mode 3 stays on
     * the third harmonic at every Wave. */
    sf_modal_state_t *state = voice->modal_state;
    if (!state) return 0.0f;

    const float accent = 0.8f;
    float structure = sf_clampf(params->wave, 0.0f, 1.0f);
    float brightness = sf_clampf(params->timbre, 0.0f, 1.0f);
    float damping = sf_clampf(params->shape, 0.0f, 1.0f);
    brightness += 0.25f * accent * (1.0f - brightness);
    damping += 0.25f * accent * (1.0f - damping);

    /* Struck mode: the module's amp envelope shapes the hit, so the resonator
     * only needs a click on the rising edge of the gate. */
    int trigger = voice->gate && !state->was_gated;
    state->was_gated = voice->gate;

    /* ModalEngine smooths the structure knob into the resonator. A strike
     * starts from the knob's value: the voice clears this state on a new
     * note, and striking modes that are still gliding up from Wave 0 gives
     * them harmonic, not inharmonic, amplitudes. */
    if (trigger) state->harmonics_lp = structure;
    state->harmonics_lp += 0.01f * (structure - state->harmonics_lp);

    const float range = 60.0f;
    const float f = 2.0f * voice->increment;
    float cutoff = f * modal_semitones_to_ratio(
        (brightness * (2.0f - brightness) - 0.5f) * range);
    if (cutoff > 0.499f) cutoff = 0.499f;
    const float exc_q = 1.5f;

    float temp = 0.0f;
    if (trigger) {
        float attenuation = 1.0f - damping * 0.5f;
        float amplitude = (0.12f + 0.08f * accent) * attenuation;
        temp = amplitude * modal_semitones_to_ratio(cutoff * cutoff * 24.0f)
             / cutoff;
    }

    /* Excitation filter: ResonatorSvf<1> low-pass at the click's cutoff. */
    {
        float g = modal_tan_fast(cutoff);
        float r = 1.0f / exc_q;
        float h = 1.0f / (1.0f + r * g + g * g);
        float hp = (temp - r * state->excitation_1 - g * state->excitation_1
                    - state->excitation_2) * h;
        float bp = g * hp + state->excitation_1;
        state->excitation_1 = g * hp + bp;
        float lp = g * bp + state->excitation_2;
        state->excitation_2 = g * bp + lp;
        temp = lp;
    }

    const float stiffness0 = modal_lut_stiffness(state->harmonics_lp);
    float stiffness = stiffness0;
    float f0 = voice->increment;
    /* NthHarmonicCompensation(3, stiffness), on its own copy of the
     * stiffness so the mode loop below starts from the table value. */
    float stretch_base = 1.0f;
    for (int i = 0; i < 2; ++i) {
        stretch_base += stiffness;
        stiffness *= stiffness < 0.0f ? 0.93f : 0.98f;
    }
    f0 /= stretch_base;
    stiffness = stiffness0;

    float harmonic = f0;
    float stretch_factor = 1.0f;
    /* Plaits runs at 48 kHz; scaling q keeps its decay times at 44.1 kHz. */
    float q_sqrt = modal_semitones_to_ratio(damping * 79.7f);
    float q = 500.0f * (SF_SAMPLE_RATE / 48000.0f) * q_sqrt * q_sqrt;
    float mode_brightness = brightness * (1.0f - state->harmonics_lp * 0.3f);
    mode_brightness *= 1.0f - damping * 0.3f;
    float q_loss = mode_brightness * (2.0f - mode_brightness) * 0.85f + 0.15f;

    float out = 0.0f;
    for (int i = 0; i < SF_MODAL_MODES; ++i) {
        float mode_frequency = harmonic * stretch_factor;
        if (mode_frequency >= 0.499f) mode_frequency = 0.499f;
        float mode_attenuation = 1.0f - mode_frequency * 2.0f;
        float mode_q = 1.0f + mode_frequency * q;

        int bank = i >> 2;
        int slot = i & 3;
        float s1 = state->mode_state_1[bank][slot];
        float s2 = state->mode_state_2[bank][slot];
        float g = modal_tan_fast(mode_frequency);
        float r = 1.0f / mode_q;
        float h = 1.0f / (1.0f + r * g + g * g);
        float hp = (temp - (r + g) * s1 - s2) * h;
        float bp = g * hp + s1;
        s1 = g * hp + bp;
        float lp = g * bp + s2;
        s2 = g * bp + lp;
        state->mode_state_1[bank][slot] = s1;
        state->mode_state_2[bank][slot] = s2;
        out += plaits_modal_amplitudes[i] * mode_attenuation * bp;

        stretch_factor += stiffness;
        stiffness *= stiffness < 0.0f ? 0.93f : 0.98f;
        harmonic += f0;
        q *= q_loss;
    }
    return out;
}

/* ------------------------------------------------- Noise Engineering x3 -- */

/* NE SawX, measured on MiniFreak V: a falling saw multiplied by
 * 1 + 7 Wave + 0.5 Timbre and wrapped back into range, so Wave 1 is a saw at
 * eight times the note restarted every cycle. Timbre is not a gain on the
 * MiniFreak (its best-fitting gain stays near 1 + 7 Wave) and adds little
 * noise; a small gain and a little noise stand in for it until it is
 * measured properly. */
#define SF_SAWX_GAIN 0.58f
#ifndef SF_SAWX_TIMBRE_GAIN
#define SF_SAWX_TIMBRE_GAIN 0.5f
#endif

static float sawx_wrapped(float phase, float gain) {
    /* A modulus of the scaled phase: each cycle restarts at the top, and the
     * last ramp is cut short when the gain is not whole (measured; wrapping
     * a centred ramp instead lost 10-20 dB of the low harmonics). */
    float x = phase * gain;
    return 1.0f - 2.0f * (x - floorf(x));
}

static float sawx_sample(const sf_engine_params_t *params,
                         const sf_engine_voice_t *voice) {
    /* Wave = Saw Mod, Timbre = Noise, Shape = Chorus. */
    static const float spread[SF_SUPERWAVE_COPIES] = {-1.03f, -0.37f, 0.41f, 1.09f};
    /* The gains and detune ratios only move with the knobs, the copies'
     * increments with the pitch (sf_sawx_cache_t). */
    sf_sawx_cache_t local, *c = voice->sawx_cache;
    if (!c) {
        local = (sf_sawx_cache_t){0};
        c = &local;
    }
    int knobs = !c->valid || c->wave != params->wave || c->timbre != params->timbre
             || c->shape != params->shape;
    if (knobs) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        float chorus = sf_clampf(params->shape, 0.0f, 1.0f);
        float noise_pm = sf_clampf(params->timbre, 0.0f, 1.0f);
        c->chorus = chorus;
        c->noise_pm = noise_pm;
        c->mod_gain = 1.0f + 7.0f * sf_clampf(params->wave, 0.0f, 1.0f)
                    + SF_SAWX_TIMBRE_GAIN * noise_pm;
        float detune_cents = 10.0f + chorus * 26.0f;
        float norm = 1.0f;
        for (int copy = 0; copy < SF_SUPERWAVE_COPIES; ++copy) {
            c->ratio[copy] = engine_exp2f_small(spread[copy] * detune_cents * (1.0f / 1200.0f));
            norm += chorus * chorus;
        }
        c->norm_root = sqrtf(norm);
        c->copies = chorus > 0.001f;
    }
    float mod_gain = c->mod_gain;
    float sum = sawx_wrapped(voice->phase, mod_gain);

    if (voice->stack_phase && c->copies) {
        if (knobs || c->increment != voice->increment) {
            c->increment = voice->increment;
            for (int copy = 0; copy < SF_SUPERWAVE_COPIES; ++copy)
                c->copy_increment[copy] = sf_clampf(voice->increment * c->ratio[copy],
                                                    0.00001f, 0.49f);
        }
        float chorus = c->chorus;
        for (int copy = 0; copy < SF_SUPERWAVE_COPIES; ++copy) {
            float increment = c->copy_increment[copy];
            float phase = voice->stack_phase[copy];
            sum += sawx_wrapped(phase, mod_gain) * chorus;
            phase += increment;
            if (phase >= 1.0f) phase -= 1.0f;
            voice->stack_phase[copy] = phase;
        }
        /* The copies drift apart, so they add in power: MiniFreak V's level
         * does not move with Chorus. Without copies the norm is 1, and
         * dividing by sqrtf(1) leaves the sum as it is. */
        sum /= c->norm_root;
    } else {
        c->increment = -1.0f;   /* the copies' increments were not worked out */
    }
    c->valid = 1;
    float noise_pm = c->noise_pm;
    if (noise_pm > 0.001f) sum += noise_sample(voice->rng) * noise_pm * 0.01f;
    return sum * SF_SAWX_GAIN;
}

/* NE Harm, measured on MiniFreak V: four sine partials with amplitudes
 * 0/-11.3/-13.6/-17.5 dB and fixed start phases. Below Wave 0.5 partial k
 * sits at 1 + (k-1)(1 - v^3) times the note (v = 1 - 2 Wave: unison at 0,
 * harmonics at 0.5); above it at k + u^3 (2^k - k) (u = 2 Wave - 1: octaves
 * at 1). Timbre crossfades each partial towards a louder |sin| without its
 * DC: the sine keeps 1 - Timbre^2.3 and |sin| comes in at 2 Timbre^3, so
 * the fundamental is gone at 1 and the even harmonics stay loud. */
static const float g_harm_amp[4] = {1.0f, 0.272f, 0.209f, 0.133f};
static const float g_harm_phase[4] = {0.0f, -0.075f, -0.085f, -0.17f};
#define SF_HARM_GAIN 0.75f
#define SF_HARM_DRIVE 4.5f
#define SF_HARM_RECT 2.0f
#define SF_HARM_RECT_DC 0.63662f
#define SF_HARM_NOISE 0.8f

static float harm_ratio(int k, float wave) {
    /* Partial 1 plays on the voice's own phase, so its ratio is 1 at every
     * Wave. The spread below gave it 1 + u^3 above Wave 0.5, which only the
     * Nyquist check read: it muted the whole engine an octave early. */
    if (k == 1) return 1.0f;
    if (wave < 0.5f) {
        float v = 1.0f - 2.0f * wave;
        return 1.0f + (float)(k - 1) * (1.0f - v * v * v);
    }
    float u = 2.0f * wave - 1.0f;
    return (float)k + u * u * u * (engine_exp2f((float)k) - (float)k);
}

static float ne_harm_sample(const sf_engine_params_t *params,
                            const sf_engine_voice_t *voice) {
    /* Wave = partial spread, Timbre = rectification, Shape = drive into a
     * tanh clip with pink noise ahead of it (MiniFreak V: equal noise energy
     * per octave, -47 dB re the fundamental's octave at Shape 0.1, -22 at 1).
     * Partial 1 runs on the voice phase, partials 2..4 on stack_phase[0..2],
     * so moving Wave never makes them jump; the pink filter uses
     * chord_phase[0..2]. */
    float wave = sf_clampf(params->wave, 0.0f, 1.0f);
    float rect = sf_clampf(params->timbre, 0.0f, 1.0f);
    float shape = sf_clampf(params->shape, 0.0f, 1.0f);
    sf_harm_cache_t local, *c = voice->harm_cache;
    if (!c) {
        local = (sf_harm_cache_t){0};
        c = &local;
    }
    if (!c->valid || c->wave != params->wave || c->timbre != params->timbre
        || c->shape != params->shape) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        c->keep = 1.0f - powf(rect, 2.3f);
        c->rect_mix = SF_HARM_RECT * rect * rect * rect;
        for (int k = 1; k <= 4; ++k) c->ratio[k - 1] = harm_ratio(k, wave);
        c->noise_pow = powf(shape, 1.7f);
        c->valid = 1;
    }
    float keep = c->keep;
    float rect_mix = c->rect_mix;
    float sum = 0.0f;
    for (int k = 1; k <= 4; ++k) {
        float ratio = c->ratio[k - 1];
        float increment = voice->increment * ratio;
        if (increment >= 0.49f) break;
        float phase;
        if (k == 1) phase = voice->phase;
        else if (voice->stack_phase) {
            phase = voice->stack_phase[k - 2];
            float next = phase + increment;
            voice->stack_phase[k - 2] = next - floorf(next);
        } else phase = voice->phase * ratio;
        float s = sinf(SF_TWO_PI * (phase + g_harm_phase[k - 1]));
        s = s * keep + (fabsf(s) - SF_HARM_RECT_DC) * rect_mix;
        sum += s * g_harm_amp[k - 1];
    }
    float drive = 1.0f + SF_HARM_DRIVE * shape;
    float x = sum * drive;
    if (shape > 0.001f && voice->chord_phase) {
        /* Paul Kellet's economy pink filter. */
        float *b = voice->chord_phase;
        float white = noise_sample(voice->rng);
        b[0] = 0.99765f * b[0] + white * 0.0990f;
        b[1] = 0.96300f * b[1] + white * 0.2965f;
        b[2] = 0.57000f * b[2] + white * 1.0526f;
        float pink = b[0] + b[1] + b[2] + white * 0.1848f;
        x += pink * SF_HARM_NOISE * c->noise_pow;
    }
    return tanhf(x) * SF_HARM_GAIN;
}

/* Shape's noise, fitted to MiniFreak V: two one-pole low-passes at about
 * 740 Hz (its noise centres near 1 kHz) and the gain that keeps the noise
 * share of the output where MiniFreak V has it (-21 dB at 0.25, -10 at 1). */
#define SF_BASS_NOISE_LP 0.1f
#define SF_BASS_NOISE_GAIN 3.2f

static float ne_bass_sample(const sf_engine_params_t *params,
                            const sf_engine_voice_t *voice) {
    /* NE Bass: quadrature oscillators with saturation, a two-stage asymmetric
     * wavefolder, and noise phase-modulation applied between the stages.
     * Wave = Saturate, Timbre = Fold, Shape = Noise. */
    float sat = sf_clampf(params->wave, 0.0f, 1.0f);
    float fold = sf_clampf(params->timbre, 0.0f, 1.0f);
    float noise_level = sf_clampf(params->shape, 0.0f, 1.0f);
    /* The folds' offsets and their sines depend on Timbre and Shape alone. */
    sf_bass_cache_t local, *k = voice->bass_cache;
    if (!k) {
        local = (sf_bass_cache_t){0};
        k = &local;
    }
    if (!k->valid || k->timbre != params->timbre || k->shape != params->shape) {
        k->timbre = params->timbre;
        k->shape = params->shape;
        fold_prepare(fold * 0.6f, 0.0f, &k->gain1, &k->dc1, &k->sref1);
        fold_prepare(fold * 0.9f, (noise_level - 0.5f) * 0.9f, &k->gain2, &k->dc2, &k->sref2);
        k->valid = 1;
    }

    float theta = SF_TWO_PI * voice->phase;
    float s = sinf(theta);
    float c = cosf(theta);
    float drive = 1.0f + sat * 6.0f;
    float satur_cos = c * drive / (1.0f + fabsf(c) * drive * 0.55f);

    float fold1 = fold_apply(satur_cos, k->gain1, k->dc1, k->sref1);
    /* The noise is dark on MiniFreak V, so it is low-passed first
     * (filter_state is free in this engine). */
    float noise = noise_sample(voice->rng);
    if (voice->filter_state) {
        sf_filter_state_t *lp = voice->filter_state;
        lp->low += SF_BASS_NOISE_LP * (noise - lp->low);
        lp->band += SF_BASS_NOISE_LP * (lp->low - lp->band);
        noise = lp->band * SF_BASS_NOISE_GAIN;
    }
    float noise_pm = noise * noise_level * 0.5f;
    float mixed = fold1 * (0.55f + 0.45f * s) + noise_pm;
    return fold_apply(mixed, k->gain2, k->dc2, k->sref2);
}

/* -------------------------------------------------- sample/granular stubs */

static float granular_placeholder_sample(const sf_engine_params_t *params,
                                         const sf_engine_voice_t *voice) {
    /* PLACEHOLDER for Sample, Particles, Cloud Grains, Hit Grains, Frozen,
     * Skan, Lick and Raster. Those need a sample library and a grain scheduler
     * this module deliberately does not carry yet, but a preset naming one must
     * still load and make a sound, so the wavetable oscillator stands in with
     * the same three controls and a grain-retrigger window on top. */
    float tone;
    sf_wavetable_cache_t *c = voice->wavetable_cache;
    if (c) {
        int knobs = !c->valid || c->kind != 2 || c->wave != params->wave
                 || c->timbre != params->timbre;
        if (knobs) {
            c->kind = 2;
            c->wave = params->wave;
            c->timbre = params->timbre;
            wavetable_position(params->wave * (SF_WT_FAMILIES - 1), params->timbre, &c->pos);
        }
        if (knobs || c->increment != voice->increment) {
            c->increment = voice->increment;
            c->level = wavetable_level(voice->increment);
        }
        c->valid = 1;
        tone = wavetable_sample_at(&c->pos, c->level, warp_phase(voice->phase, params->shape));
    } else {
        tone = wavetable_sample(params->wave * (SF_WT_FAMILIES - 1), params->timbre,
                                warp_phase(voice->phase, params->shape),
                                voice->increment);
    }
    float density = 1.0f + params->timbre * 7.0f;
    float window = sinf(SF_PI * voice->phase * density);
    return tone * (0.35f + 0.65f * fabsf(window));
}

static float audioin_placeholder_sample(const sf_engine_params_t *params,
                                        const sf_engine_voice_t *voice) {
    /* PLACEHOLDER for Audio In. The module declares no `audio_in` capability
     * and the Move host hands it no capture buffer, so there is nothing to
     * process; noise with the same three controls stands in so a preset naming
     * Audio In is audible rather than silent. */
    return colored_noise_sample(voice, params->wave, params->timbre, params->shape);
}

/* ---------------------------------------------------------------- Chords -- */

/* Adapted from Emilie Gillet's Plaits ChordEngine / ChordBank
 * (plaits/dsp/engine/chord_engine.cc, plaits/dsp/chords/chord_bank.cc; MIT).
 * Wave = Interval, Timbre = Inv/Trsp, Shape = Waveform. The chord table and
 * inversion walk are the upstream ones, so the knob positions land on the
 * hardware's 11 chord types. */
static const float sf_chord_semitones[11][4] = {
    {0.00f, 0.01f, 11.99f, 12.00f}, /* OCT  */
    {0.00f, 7.00f,  7.01f, 12.00f}, /* 5    */
    {0.00f, 5.00f,  7.00f, 12.00f}, /* sus4 */
    {0.00f, 3.00f,  7.00f, 12.00f}, /* m    */
    {0.00f, 3.00f,  7.00f, 10.00f}, /* m7   */
    {0.00f, 3.00f, 10.00f, 14.00f}, /* m9   */
    {0.00f, 3.00f, 10.00f, 17.00f}, /* m11  */
    {0.00f, 2.00f,  9.00f, 16.00f}, /* 69   */
    {0.00f, 4.00f, 11.00f, 14.00f}, /* M9   */
    {0.00f, 4.00f,  7.00f, 11.00f}, /* M7   */
    {0.00f, 4.00f,  7.00f, 12.00f}  /* M    */
};

/* Registrations: Saw 8', Square 8', Saw 4', Square 4', Saw 2', Square 2'. */
static const float sf_chord_registrations[8][6] = {
    {0.00f, 1.00f, 0.00f, 0.00f, 0.00f, 0.00f}, /* Square */
    {1.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f}, /* Saw */
    {0.50f, 0.00f, 0.50f, 0.00f, 0.00f, 0.00f}, /* Saw + saw */
    {0.33f, 0.00f, 0.33f, 0.00f, 0.33f, 0.00f}, /* Full saw */
    {0.33f, 0.00f, 0.00f, 0.33f, 0.00f, 0.33f}, /* Full saw + square */
    {0.50f, 0.00f, 0.00f, 0.00f, 0.00f, 0.50f}, /* Saw + high square */
    {0.00f, 0.50f, 0.00f, 0.00f, 0.00f, 0.50f}, /* Square + high square */
    {0.00f, 0.10f, 0.10f, 0.00f, 0.20f, 0.60f}  /* Saw+square + high */
};

#if SF_CHORD_PHASES < 5
#error "The Chords engine needs five phase accumulators"
#endif

static float sf_chord_saw(float phase, float increment) {
    return 2.0f * phase - 1.0f - poly_blep(phase, increment);
}

static float sf_chord_frac(float x) {
    return x - (float)(int)x;
}

/* ChordBank::ComputeChordInversion, with five voices for four chord notes. */
static void sf_chord_inversion(int chord, float inversion,
                               float *ratios, float *amplitudes) {
    inversion *= 20.0f; /* kChordNumNotes * kChordNumVoices */
    int integral = (int)inversion;
    if (integral < 0) integral = 0;
    float fractional = inversion - (float)integral;
    int rotations = integral / 4;
    int rotated_note = integral % 4;
    for (int v = 0; v < 5; ++v) {
        ratios[v] = 1.0f;
        amplitudes[v] = 0.0f;
    }
    for (int i = 0; i < 4; ++i) {
        float base = engine_exp2f(sf_chord_semitones[chord][i] * (1.0f / 12.0f));
        float transposition = 0.25f
            * (float)(1 << ((3 + integral - i) / 4));
        int target = (i - rotations + 5) % 5;
        int previous = (target - 1 + 5) % 5;
        if (i == rotated_note) {
            ratios[target] = base * transposition;
            ratios[previous] = ratios[target] * 2.0f;
            amplitudes[previous] = 0.25f * fractional;
            amplitudes[target] = 0.25f * (1.0f - fractional);
        } else if (i < rotated_note) {
            ratios[previous] = base * transposition;
            amplitudes[previous] = 0.25f;
        } else {
            ratios[target] = base * transposition;
            amplitudes[target] = 0.25f;
        }
    }
}

static void sf_chord_registration(float registration, float *amplitudes) {
    registration = sf_clampf(registration, 0.0f, 1.0f) * 6.999f;
    int integral = (int)registration;
    if (integral > 6) integral = 6;
    float fractional = registration - (float)integral;
    for (int i = 0; i < 6; ++i) {
        float a = sf_chord_registrations[integral][i];
        float b = sf_chord_registrations[integral + 1][i];
        amplitudes[i] = a + (b - a) * fractional;
    }
}

/* The four band-limited saws StringSynthOscillator derives from one phase. */
static float sf_chord_divide_down(float phase, float increment,
                                  const float *registration) {
    float gain_8 = registration[0] + 2.0f * registration[1];
    float gain_4 = registration[2] - registration[1] + 2.0f * registration[3];
    float gain_2 = registration[4] - registration[3] + 2.0f * registration[5];
    float gain_1 = -registration[5];
    float increment_4 = increment * 2.0f;
    float increment_2 = increment * 4.0f;
    float increment_1 = increment * 8.0f;
    float sum = sf_chord_saw(phase, increment) * gain_8;
    if (increment_4 < 0.49f)
        sum += sf_chord_saw(sf_chord_frac(phase * 2.0f), increment_4) * gain_4;
    if (increment_2 < 0.49f)
        sum += sf_chord_saw(sf_chord_frac(phase * 4.0f), increment_2) * gain_2;
    if (increment_1 < 0.49f)
        sum += sf_chord_saw(sf_chord_frac(phase * 8.0f), increment_1) * gain_1;
    return sum;
}

#define SF_CHORD_WT_COUNT 18
#define SF_CHORD_WT_HARMONICS 32
/* Harmonic amplitudes (dB) of one chord voice at Shape 0.575, 0.6 .. 1,
 * unpicked from MiniFreak V's OCT chord by taking each octave voice's power
 * off the even harmonics. That assumes equal voices, which the hardware's
 * wavetable chords are not quite, so the tables are approximate: across the
 * factory presets that use them the brightness lands within about 0.2 octave
 * of MiniFreak V, where the old bright stand-in was 2-3 octaves over. */
static const signed char g_chord_wt_db[SF_CHORD_WT_COUNT][SF_CHORD_WT_HARMONICS] = {
    {-2, 0, -7, -49, -32, -13, -44, -64, -43, -21, -49, -84, -46, -53, -55, -97, -58, -67, -71, -102, -78, -87, -98, -110, -100, -108, -99, -112, -99, -109, -101, -110},
    {-7, 0, -31, -58, -38, -47, -62, -79, -66, -70, -67, -102, -66, -79, -73, -106, -79, -87, -80, -109, -100, -98, -99, -99, -97, -109, -100, -110, -103, -111, -102, -103},
    {-13, 0, -10, -29, -48, -29, -46, -11, -53, -68, -54, -64, -55, -76, -66, -77, -69, -79, -78, -100, -90, -104, -98, -106, -96, -107, -97, -112, -97, -109, -95, -107},
    {-15, 0, -4, -20, -44, -21, -45, -38, -51, -60, -48, -20, -52, -66, -62, -77, -66, -82, -71, -87, -78, -101, -96, -106, -97, -107, -94, -106, -96, -106, -100, -108},
    {-12, 0, -1, -17, -44, -18, -46, -34, -46, -58, -48, -11, -48, -64, -59, -18, -55, -80, -61, -81, -70, -95, -82, -104, -94, -104, -92, -106, -92, -104, -96, -107},
    {-21, 0, -11, -23, -55, -12, -50, -6, -56, -49, -51, -50, -56, -63, -57, -15, -57, -59, -74, -76, -65, -92, -92, -102, -90, -103, -94, -104, -93, -104, -98, -107},
    {-22, -15, 0, -10, -62, -16, -60, -27, -55, -63, -61, -4, -53, -76, -53, -6, -60, -72, -64, -85, -62, -82, -62, -54, -75, -97, -85, -102, -84, -101, -86, -68},
    {-31, -55, 0, -20, -21, -18, -44, -53, -35, -20, -49, -31, -35, -60, -40, -11, -52, -31, -61, -69, -65, -76, -61, -57, -80, -81, -91, -100, -89, -94, -84, -65},
    {-23, -17, -4, -6, -4, -6, -44, -32, -19, 0, -46, -27, -25, -60, -29, -8, -51, -39, -48, -59, -56, -67, -69, -86, -74, -72, -89, -100, -86, -87, -85, -72},
    {-44, 0, -34, -15, -11, -12, -56, -39, -43, -3, -43, -39, -25, -34, -46, -30, -49, -40, -47, -53, -52, -66, -85, -73, -85, -63, -86, -94, -89, -96, -88, -91},
    {-13, -2, -19, 0, -6, -19, -15, -32, -14, -1, -25, -9, -19, -18, -27, -48, -46, -35, -39, -52, -54, -51, -62, -59, -78, -64, -89, -83, -91, -84, -94, -101},
    {-50, 0, -51, -24, -29, -28, -29, -40, -26, -9, -36, -13, -34, -25, -42, -37, -74, -20, -44, -54, -69, -58, -92, -62, -95, -73, -95, -85, -97, -98, -99, -98},
    {-67, 0, -51, -24, -30, -30, -32, -45, -24, -11, -43, -18, -43, -28, -48, -34, -71, -18, -46, -56, -74, -66, -87, -67, -94, -83, -96, -88, -100, -104, -98, -96},
    {-59, 0, -51, -16, -27, -36, -39, -53, -24, -20, -61, -24, -58, -24, -59, -31, -61, -16, -50, -60, -78, -81, -80, -74, -97, -98, -95, -95, -101, -107, -100, -92},
    {-59, 0, -23, -16, -26, -14, -37, -57, -24, -19, -63, -30, -59, -23, -53, -22, -52, -16, -49, -65, -74, -82, -79, -72, -91, -96, -96, -93, -93, -100, -95, -83},
    {-54, -11, -24, -32, -38, 0, -45, -19, -35, -31, -63, -26, -65, -30, -51, -38, -55, -33, -58, -67, -82, -80, -85, -78, -93, -101, -101, -101, -98, -95, -97, -89},
    {-18, -22, 0, -14, -44, -7, -30, -13, -25, -35, -42, -26, -42, -38, -34, -42, -47, -49, -60, -61, -58, -77, -68, -80, -87, -101, -93, -106, -91, -92, -101, -91},
    {-11, -8, 0, -12, -38, -18, -28, -31, -18, -35, -47, -37, -42, -33, -33, -26, -49, -35, -53, -57, -59, -71, -70, -82, -90, -101, -94, -104, -96, -92, -98, -108},
};
/* MiniFreak V level of the OCT chord at the same points (dBFS). */
static const float g_chord_wt_level_db[SF_CHORD_WT_COUNT] = {
    -21.5f, -23.0f, -27.5f, -27.2f, -27.8f, -28.7f, -30.1f, -30.4f, -28.4f, -29.8f, -26.9f, -26.1f, -24.8f, -23.0f, -27.8f, -26.3f, -23.8f, -25.9f
};

/* MiniFreak V level of the divide-down chord at Shape 0.55 (dBFS), the
 * reference the wavetable levels are taken against, and the gain that puts
 * a unit-RMS table at that level here. */
#define SF_CHORD_WT_REF_DB (-22.8f)
#define SF_CHORD_WT_GAIN 1.0f
#define SF_CHORD_WT_SIZE 256
static float g_chord_wt[SF_CHORD_WT_COUNT][SF_CHORD_WT_SIZE + 1];
static int g_chord_wt_ready;

static void chord_wavetable_init(void) {
    for (int t = 0; t < SF_CHORD_WT_COUNT; ++t) {
        float sum2 = 0.0f;
        for (int i = 0; i < SF_CHORD_WT_SIZE; ++i) {
            float x = 0.0f;
            for (int h = 0; h < SF_CHORD_WT_HARMONICS; ++h) {
                if (g_chord_wt_db[t][h] <= -120) continue;
                x += engine_exp2f((float)g_chord_wt_db[t][h] * 0.16609640f)
                   * sinf(SF_TWO_PI * (float)((h + 1) * i) / (float)SF_CHORD_WT_SIZE);
            }
            g_chord_wt[t][i] = x;
            sum2 += x * x;
        }
        float gain = engine_exp2f((g_chord_wt_level_db[t] - SF_CHORD_WT_REF_DB) * 0.16609640f)
                   / sqrtf(sum2 / (float)SF_CHORD_WT_SIZE + 1e-12f);
        for (int i = 0; i < SF_CHORD_WT_SIZE; ++i) g_chord_wt[t][i] *= gain;
        g_chord_wt[t][SF_CHORD_WT_SIZE] = g_chord_wt[t][0];
    }
    g_chord_wt_ready = 1;
}

static void chord_wavetable_position(float position, int *table, float *mix) {
    float p = sf_clampf(position, 0.0f, 1.0f) * (float)(SF_CHORD_WT_COUNT - 1);
    int t = p >= (float)(SF_CHORD_WT_COUNT - 1) ? SF_CHORD_WT_COUNT - 2 : (int)p;
    *table = t;
    *mix = p - (float)t;
}

static float chord_wavetable_read(int t, float tf, float phase) {
    float x = phase * (float)SF_CHORD_WT_SIZE;
    int i = (int)x;
    if (i >= SF_CHORD_WT_SIZE) i = SF_CHORD_WT_SIZE - 1;
    float f = x - (float)i;
    float a = g_chord_wt[t][i] + (g_chord_wt[t][i + 1] - g_chord_wt[t][i]) * f;
    float b = g_chord_wt[t + 1][i] + (g_chord_wt[t + 1][i + 1] - g_chord_wt[t + 1][i]) * f;
    return SF_CHORD_WT_GAIN * (a + (b - a) * tf);
}

static float chord_wavetable_sample(float position, float phase) {
    int t;
    float tf;
    chord_wavetable_position(position, &t, &tf);
    return chord_wavetable_read(t, tf, phase);
}

/* sf_chord_divide_down with the gains and the four increments worked out,
 * skipping the saws that are over Nyquist or have no gain: adding nothing
 * leaves the sum exactly as it was. */
static float sf_chord_divide_down_cached(float phase, const float *inc, unsigned mask,
                                         const sf_chord_cache_t *c) {
    float sum = 0.0f;
    if (mask & 1u) sum = sf_chord_saw(phase, inc[0]) * c->gain_8;
    if (mask & 2u)
        sum += sf_chord_saw(sf_chord_frac(phase * 2.0f), inc[1]) * c->gain_4;
    if (mask & 4u)
        sum += sf_chord_saw(sf_chord_frac(phase * 4.0f), inc[2]) * c->gain_2;
    if (mask & 8u)
        sum += sf_chord_saw(sf_chord_frac(phase * 8.0f), inc[3]) * c->gain_1;
    return sum;
}

static float chord_sample_cached(const sf_engine_params_t *params,
                                 const sf_engine_voice_t *voice, sf_chord_cache_t *c) {
    int knobs = !c->valid || c->wave != params->wave || c->timbre != params->timbre
             || c->shape != params->shape;
    if (knobs) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        int chord = (int)(sf_clampf(params->wave, 0.0f, 1.0f) * 10.99f);
        if (chord < 0) chord = 0;
        if (chord > 10) chord = 10;
        float inversion = sf_clampf(params->timbre, 0.0f, 1.0f);
        float morph = sf_clampf(params->shape, 0.0f, 1.0f);
        sf_chord_inversion(chord, inversion, c->ratios, c->amplitudes);
        float registration[6];
        sf_chord_registration(fmaxf(1.0f - morph * 2.15f, 0.0f), registration);
        c->gain_8 = registration[0] + 2.0f * registration[1];
        c->gain_4 = registration[2] - registration[1] + 2.0f * registration[3];
        c->gain_2 = registration[4] - registration[3] + 2.0f * registration[5];
        c->gain_1 = -registration[5];
        c->wavetable = sf_clampf((morph - 0.55f) / 0.025f, 0.0f, 1.0f);
        float table_pos = (morph - 0.575f) / 0.425f;
        if (c->wavetable > 0.0f && !g_chord_wt_ready) chord_wavetable_init();
        chord_wavetable_position(table_pos, &c->table, &c->table_mix);
        unsigned mask = 0u;
        if (c->gain_8 != 0.0f) mask |= 1u;
        if (c->gain_4 != 0.0f) mask |= 2u;
        if (c->gain_2 != 0.0f) mask |= 4u;
        if (c->gain_1 != 0.0f) mask |= 8u;
        c->gain_mask = mask;
    }
    if (knobs || c->increment != voice->increment) {
        c->increment = voice->increment;
        for (int v = 0; v < 5; ++v) {
            float increment = sf_clampf(voice->increment * c->ratios[v],
                                        0.00001f, 0.49f);
            float *inc = c->inc[v];
            inc[0] = increment;
            inc[1] = increment * 2.0f;
            inc[2] = increment * 4.0f;
            inc[3] = increment * 8.0f;
            unsigned mask = c->gain_mask;
            if (!(inc[3] < 0.49f)) {
                mask &= ~8u;
                if (!(inc[2] < 0.49f)) mask &= ~4u;
                if (!(inc[1] < 0.49f)) mask &= ~2u;
            }
            c->mask[v] = mask;
        }
    }
    c->valid = 1;

    float wavetable = c->wavetable;
    float total = 0.0f;
    for (int v = 0; v < 5; ++v) {
        float increment = c->inc[v][0];
        float phase = voice->chord_phase ? voice->chord_phase[v] : voice->phase;
        /* A silent voice adds exactly nothing; its phase still runs. */
        if (c->amplitudes[v] != 0.0f) {
            if (wavetable <= 0.0f) {
                total += sf_chord_divide_down_cached(phase, c->inc[v], c->mask[v], c) * c->amplitudes[v];
            } else if (wavetable >= 1.0f) {
                total += chord_wavetable_read(c->table, c->table_mix, phase) * c->amplitudes[v];
            } else {
                float divide_down = sf_chord_divide_down_cached(phase, c->inc[v], c->mask[v], c);
                float wave = chord_wavetable_read(c->table, c->table_mix, phase);
                total += (divide_down * (1.0f - wavetable) + wave * wavetable)
                       * c->amplitudes[v];
            }
        }
        if (voice->chord_phase) {
            phase += increment;
            if (phase >= 1.0f) phase -= 1.0f;
            voice->chord_phase[v] = phase;
        }
    }
    return total;
}

static float chord_sample(const sf_engine_params_t *params,
                          const sf_engine_voice_t *voice) {
    if (voice->chord_cache) return chord_sample_cached(params, voice, voice->chord_cache);
    int chord = (int)(sf_clampf(params->wave, 0.0f, 1.0f) * 10.99f);
    if (chord < 0) chord = 0;
    if (chord > 10) chord = 10;
    float inversion = sf_clampf(params->timbre, 0.0f, 1.0f);
    float morph = sf_clampf(params->shape, 0.0f, 1.0f);

    float ratios[5];
    float amplitudes[5];
    sf_chord_inversion(chord, inversion, ratios, amplitudes);

    float registration[6];
    sf_chord_registration(fmaxf(1.0f - morph * 2.15f, 0.0f), registration);
    /* Above about 0.55 the hardware switches to a wavetable of soft, few-
     * harmonic waves (g_chord_wt_db, measured); Shape then scans it. */
    float wavetable = sf_clampf((morph - 0.55f) / 0.025f, 0.0f, 1.0f);
    float table_pos = (morph - 0.575f) / 0.425f;
    if (wavetable > 0.0f && !g_chord_wt_ready) chord_wavetable_init();

    float total = 0.0f;
    for (int v = 0; v < 5; ++v) {
        float increment = sf_clampf(voice->increment * ratios[v],
                                        0.00001f, 0.49f);
        float phase = voice->chord_phase ? voice->chord_phase[v] : voice->phase;
        float divide_down = sf_chord_divide_down(phase, increment, registration);
        float wave = wavetable > 0.0f ? chord_wavetable_sample(table_pos, phase) : 0.0f;
        total += (divide_down * (1.0f - wavetable) + wave * wavetable)
               * amplitudes[v];
        if (voice->chord_phase) {
            phase += increment;
            if (phase >= 1.0f) phase -= 1.0f;
            voice->chord_phase[v] = phase;
        }
    }
    return total;
}

/* ------------------------------------------------- Osc 2 delay processors -- */

/*
 * MiniFreak Comb Filter, measured from MiniFreak V through its Audio In engine.
 *
 * It is a feedback comb tuned to the note: Wave sets the comb's pitch as a
 * ratio of Osc 2's frequency (x0.092 at 0, unison at 0.65, x7.7 at 1), Timbre
 * trades input gain for feedback (up to +20 dB at the teeth; the feedback
 * itself, fitted to how long a plucked comb rings on MiniFreak V, climbs to
 * almost 1 at full Timbre, where the ring decays under 1 dB/s), and Shape is a
 * one-pole low-pass in the loop (about 5 kHz down to 490 Hz). A hard-knee
 * limiter in the loop caps the ringing at about 0.6 of full oscillator level, which
 * is why a high-Timbre comb keeps its level as its input fades: a tenth of
 * the input costs only a few dB.
 */
static const float g_comb_ratio[21] = {
    0.092f, 0.0986f, 0.1074f, 0.1195f, 0.1356f, 0.1575f, 0.1871f, 0.2269f,
    0.2809f, 0.3534f, 0.4519f, 0.5832f, 0.7627f, 1.0033f, 1.3272f, 1.7743f,
    2.3741f, 3.1215f, 4.322f, 5.6186f, 7.6618f
};
#define SF_COMB_TIMBRE_POINTS 12
static const float g_comb_timbre_x[SF_COMB_TIMBRE_POINTS] = {
    0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 0.95f, 1.0f
};
static const float g_comb_input_gain[SF_COMB_TIMBRE_POINTS] = {
    1.0f, 0.905f, 0.812f, 0.678f, 0.544f, 0.435f, 0.355f, 0.299f, 0.262f,
    0.252f, 0.266f, 0.296f
};
static const float g_comb_feedback[SF_COMB_TIMBRE_POINTS] = {
    0.0f, 0.367f, 0.60f, 0.747f, 0.841f, 0.90f, 0.935f, 0.951f, 0.970f,
    0.990f, 0.9966f, 0.9998f
};
/* Loop low-pass cutoff (Hz) at Shape 0, 0.25, 0.5, 0.75 and 1. */
static const float g_comb_damp_hz[5] = {5000.0f, 3500.0f, 2700.0f, 1590.0f, 490.0f};
/* Limiter knee, in units where a full-scale oscillator peaks at 1. Fitted on
 * Flute (Comb at Timbre 0.99): within 1 dB of MiniFreak V from full input
 * down to 3% of it. */
#define SF_COMB_LIMIT 0.6f

static float comb_process(const sf_engine_voice_t *voice,
                          const sf_engine_params_t *params, float input) {
    if (!voice->delay_buf || !voice->delay_pos || !voice->delay_low) return input;
    unsigned size = (unsigned)SF_VOICE_DELAY;
    /* The coefficients only move with the knobs and the pitch, so they are
     * worked out when those change (see sf_comb_cache_t). */
    sf_comb_cache_t local, *c = voice->comb_cache;
    if (!c) {
        local = (sf_comb_cache_t){0};
        c = &local;
    }
    int knobs = !c->valid || c->wave != params->wave || c->timbre != params->timbre
             || c->shape != params->shape;
    if (knobs) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        float w = sf_clampf(params->wave, 0.0f, 1.0f) * 20.0f;
        int wi = (int)w;
        if (wi > 19) wi = 19;
        c->ratio = g_comb_ratio[wi]
                 * engine_exp2f((w - (float)wi) * log2f(g_comb_ratio[wi + 1] / g_comb_ratio[wi]));

        float t = sf_clampf(params->timbre, 0.0f, 1.0f);
        int ti = 0;
        while (ti < SF_COMB_TIMBRE_POINTS - 2 && t > g_comb_timbre_x[ti + 1]) ++ti;
        float tf = (t - g_comb_timbre_x[ti]) / (g_comb_timbre_x[ti + 1] - g_comb_timbre_x[ti]);
        c->in_gain = g_comb_input_gain[ti] + (g_comb_input_gain[ti + 1] - g_comb_input_gain[ti]) * tf;
        c->feedback = g_comb_feedback[ti] + (g_comb_feedback[ti + 1] - g_comb_feedback[ti]) * tf;

        float sh = sf_clampf(params->shape, 0.0f, 1.0f) * 4.0f;
        int si = (int)sh;
        if (si > 3) si = 3;
        float damp_hz = g_comb_damp_hz[si]
                      * engine_exp2f((sh - (float)si) * log2f(g_comb_damp_hz[si + 1] / g_comb_damp_hz[si]));
        c->alpha = 1.0f - expf(-SF_TWO_PI * damp_hz / SF_SAMPLE_RATE);
        /* The loop low-pass delays low frequencies by (1 - alpha) / alpha
         * samples; take that off the line so the teeth land on the comb
         * pitch. */
        c->lag = (1.0f - c->alpha) / c->alpha;
    }
    if (knobs || c->increment != voice->increment) {
        c->increment = voice->increment;
        float hz = voice->increment * SF_SAMPLE_RATE * c->ratio;
        float delay_f = SF_SAMPLE_RATE / (hz > 1.0f ? hz : 1.0f) - c->lag;
        c->delay = sf_clampf(delay_f, 2.0f, (float)size - 2.0f);
    }
    c->valid = 1;

    unsigned pos = *voice->delay_pos % size;
    float read_f = (float)pos - c->delay;
    if (read_f < 0.0f) read_f += (float)size;
    unsigned i0 = (unsigned)read_f;
    unsigned i1 = (i0 + 1u) % size;
    float frac = read_f - (float)i0;
    float a = voice->delay_buf[i0 % size];
    float delayed = a + (voice->delay_buf[i1] - a) * frac;

    float low = *voice->delay_low;
    low += c->alpha * (c->in_gain * input + c->feedback * delayed - low);
    *voice->delay_low = low;
    /* Hard-knee limiter, x / (1 + |x / L|^8)^(1/8). Below about L / 8 the sum
     * rounds to exactly 1, and the three roots would return 1. */
    float u = low * (1.0f / SF_COMB_LIMIT);
    float u2 = u * u;
    float u4 = u2 * u2;
    float knee = 1.0f + u4 * u4;
    float out = knee == 1.0f ? low : low / sqrtf(sqrtf(sqrtf(knee)));
    voice->delay_buf[pos] = out;
    *voice->delay_pos = (pos + 1u) % size;
    return out;
}

/* Multi Filter corner (notch) frequency at Wave 0, 0.1 .. 1, measured on
 * MiniFreak V: an octave per 0.1 from C1 up to 0.4, bending lower above. */
static const float g_multifilter_hz[11] = {
    16.35f, 65.4f, 130.8f, 261.6f, 523.0f, 981.0f, 1799.0f, 3303.0f, 6050.0f,
    10923.0f, 17921.0f
};

/* log2 of each step's ratio, worked out once at run time (by the same log2f
 * the per-sample code used, so the same values: a volatile read keeps the
 * compiler from folding them with its own log2). */
static float g_multifilter_log2_step[10];
static volatile int g_multifilter_steps_ready;

static void multifilter_build_steps(void) {
    for (int i = 0; i < 10; ++i) {
        volatile float ratio = g_multifilter_hz[i + 1] / g_multifilter_hz[i];
        g_multifilter_log2_step[i] = log2f(ratio);
    }
    g_multifilter_steps_ready = 1;
}

static float multifilter_process(const sf_engine_voice_t *voice,
                                 const sf_engine_params_t *params, float input) {
    /* MiniFreak Multi Filter's 14 mode/slope combinations, selected by Shape
     * (measured on MiniFreak V): LP36/24/12/6, HP6/12/24/36, BP12/24/36 and
     * Notch12/24/36, so the slope is steepest at both ends of the low and
     * high-pass runs. */
    if (!voice->multifilter_states || !voice->multifilter_lp6) return input;
    /* The coefficients only move with the knobs, so they are worked out when
     * those change (see sf_multifilter_cache_t). */
    sf_multifilter_cache_t local, *c = voice->multifilter_cache;
    if (!c) {
        local = (sf_multifilter_cache_t){0};
        c = &local;
    }
    int wave_moved = !c->valid || c->wave != params->wave;
    int knobs_moved = !c->valid || c->timbre != params->timbre || c->shape != params->shape;
    if (wave_moved) {
        c->wave = params->wave;
        float wpos = sf_clampf(params->wave, 0.0f, 1.0f) * 10.0f;
        int wi = wpos >= 10.0f ? 9 : (int)wpos;
        if (!g_multifilter_steps_ready) multifilter_build_steps();
        c->cutoff = g_multifilter_hz[wi]
                  * engine_exp2f((wpos - (float)wi) * g_multifilter_log2_step[wi]);
        c->g = engine_fast_svf_g(c->cutoff);
    }
    if (knobs_moved) {
        c->timbre = params->timbre;
        c->shape = params->shape;
        int mode = (int)(sf_clampf(params->shape, 0.0f, 1.0f) * 13.0f + 0.5f);
        float resonance = sf_clampf(params->timbre, 0.0f, 1.0f) * 0.95f;
        int filter_mode;
        int stages;
        if (mode <= 2) {
            filter_mode = FILTER_LP;
            stages = 3 - mode;
        } else if (mode <= 7) {
            filter_mode = FILTER_HP;
            stages = mode - 4;
        } else if (mode <= 10) {
            filter_mode = FILTER_BP;
            stages = mode - 7;
        } else {
            filter_mode = FILTER_NOTCH;
            stages = mode - 10;
        }
        if (stages < 1) stages = 1;
        if (stages > 3) stages = 3;
        /* sf_filter_process carries the main VCF's passband loss with Resonance;
         * the Multi Filter's passband stays at 0 dB, so each stage undoes it. In
         * the cascaded modes every stage resonates, at 0.8 of the knob: MiniFreak
         * V's resonant 36 dB low-pass peaks at +3 / +16 / +21 dB for Timbre 0.5 /
         * 0.75 / 1, against +3 / +9 / +14 for the 12 dB one. */
        float stage_res = stages > 1 ? 0.8f * resonance : resonance;
        c->mode = mode;
        c->filter_mode = filter_mode;
        c->stages = stages;
        c->stage_res = stage_res;
        if (mode != 3 && mode != 4) {
            float pb = sf_filter_passband(&voice->multifilter_states[0], stage_res);
            c->makeup = 1.0f / ((1.0f + 0.32f * stage_res) * pb);
        }
    }
    if (wave_moved || knobs_moved) {
        if (c->mode == 3 || c->mode == 4)
            c->alpha = 1.0f - expf(-SF_TWO_PI * c->cutoff / SF_SAMPLE_RATE);
        else if (knobs_moved)
            c->coefs_ok = sf_filter_coefs(&voice->multifilter_states[0], c->g, c->stage_res,
                                          &c->coefs);
        else if (c->coefs_ok)
            c->coefs_ok = sf_filter_coefs_g(&c->coefs, c->g);
    }
    c->valid = 1;

    if (c->mode == 3 || c->mode == 4) {
        *voice->multifilter_lp6 += c->alpha * (input - *voice->multifilter_lp6);
        return c->mode == 3 ? *voice->multifilter_lp6
                            : input - *voice->multifilter_lp6;
    }
    float out = input;
    if (c->coefs_ok) {
        for (int stage = 0; stage < c->stages; ++stage)
            out = sf_filter_process_coefs(&voice->multifilter_states[stage], c->filter_mode,
                                          &c->coefs, out) * c->makeup;
    } else {
        for (int stage = 0; stage < c->stages; ++stage)
            out = sf_filter_process(&voice->multifilter_states[stage], c->filter_mode,
                                    c->g, c->stage_res, out) * c->makeup;
    }
    return out;
}

static float surgeon_process(const sf_engine_voice_t *voice,
                             const sf_engine_voice_t *other,
                             const sf_engine_params_t *params, float input) {
    /* MiniFreak Surgeon is a variable-width parametric filter: Wave = centre
     * frequency, Timbre = Spread (broad to narrow), Shape = LP/BP/HP/Notch. */
    sf_filter_state_t *state = voice->filter_state ? voice->filter_state
                                                    : other->filter_state;
    if (!state) return input;
    sf_surgeon_cache_t local, *c = voice->surgeon_cache;
    if (!c) {
        local = (sf_surgeon_cache_t){0};
        c = &local;
    }
    int wave_moved = !c->valid || c->wave != params->wave;
    int knobs_moved = !c->valid || c->timbre != params->timbre || c->shape != params->shape;
    if (wave_moved) {
        c->wave = params->wave;
        float cutoff = 20.0f * engine_exp2f(sf_clampf(params->wave, 0.0f, 1.0f) * 9.813781f);
        c->g = engine_fast_svf_g(cutoff);
    }
    if (knobs_moved) {
        c->timbre = params->timbre;
        c->shape = params->shape;
        float spread = sf_clampf(params->timbre, 0.0f, 1.0f);
        c->resonance = 0.08f + spread * spread * 0.84f;
        int mode = (int)(sf_clampf(params->shape, 0.0f, 1.0f) * 3.0f + 0.5f);
        c->filter_mode = mode == 0 ? FILTER_LP
                       : mode == 1 ? FILTER_BP
                       : mode == 2 ? FILTER_HP : FILTER_NOTCH;
        c->coefs_ok = sf_filter_coefs(state, c->g, c->resonance, &c->coefs);
    } else if (wave_moved && c->coefs_ok) {
        c->coefs_ok = sf_filter_coefs_g(&c->coefs, c->g);
    }
    c->valid = 1;
    return c->coefs_ok ? sf_filter_process_coefs(state, c->filter_mode, &c->coefs, input)
                       : sf_filter_process(state, c->filter_mode, c->g, c->resonance, input);
}

#define SF_PHASER_MIX 0.5f
/* Feedback k = 0.972 Timbre, a loop delay of 0.45 sample and an output
 * gain of (1 - k)^0.25, fitted to MiniFreak V's transfer function (a
 * 32.7 Hz saw through the Phaser at Volume 1 over Volume 0): within 1-2 dB
 * at every Timbre. Near full Timbre the hardware's response sinks 9 dB while
 * one sharp peak rises 22 dB. */
#define SF_PHASER_FB 0.972f
#define SF_PHASER_FB_PREV 0.45f

static float phaser_process(const sf_engine_voice_t *voice,
                            const sf_engine_params_t *params, float input) {
    /* MiniFreak Phaser Filter: Wave = cutoff, Timbre = feedback, Shape selects
     * 2..12 poles. Osc 2 Volume provides the dry/wet mix outside this function. */
    if (!voice->apf) return input;
    sf_phaser_cache_t local, *c = voice->phaser_cache;
    if (!c) {
        local = (sf_phaser_cache_t){0};
        c = &local;
    }
    int wave_moved = !c->valid || c->wave != params->wave;
    int knobs_moved = !c->valid || c->timbre != params->timbre || c->shape != params->shape;
    if (wave_moved) {
        c->wave = params->wave;
        float freq = 20.0f * engine_exp2f(
            sf_clampf(params->wave, 0.0f, 1.0f) * 9.813781f);
        float g = engine_fast_svf_g(freq);
        /* First-order all-pass (z^-1 - a) / (1 - a z^-1): its phase passes -90
         * degrees at freq when a = (1 - g) / (1 + g). */
        c->a = sf_clampf((1.0f - g) / (1.0f + g), -0.999f, 0.999f);
    }
    if (knobs_moved) {
        c->timbre = params->timbre;
        c->shape = params->shape;
        float timbre = sf_clampf(params->timbre, 0.0f, 1.0f);
        float feedback = SF_PHASER_FB * timbre;
        int pairs = 1 + (int)(sf_clampf(params->shape, 0.0f, 1.0f) * 5.0f + 0.5f);
        c->stages = pairs * 2;
        c->now = (1.0f - SF_PHASER_FB_PREV) * feedback;
        c->feedback_prev = SF_PHASER_FB_PREV * feedback;
        c->out_gain = sqrtf(sqrtf(1.0f - feedback));
    }
    float a = c->a;
    int stages = c->stages;

    /* The chain's output is fed back into its input with a fraction of a
     * sample of delay: 45 % last sample's output, 55 % this one's, which is
     * solved in closed form since each stage is -a x + s, so the chain is
     * G x + T with G = a^stages. G only moves with the knobs, so it is kept
     * in the cache's denominator. */
    float offset = 0.0f;
    if (wave_moved || knobs_moved) {
        float gain = 1.0f;
        for (int stage = 0; stage < stages; ++stage) {
            offset = -a * offset + voice->apf[stage];
            gain *= -a;
        }
        c->denominator = 1.0f - c->now * gain;
        c->valid = 1;
    } else {
        for (int stage = 0; stage < stages; ++stage)
            offset = -a * offset + voice->apf[stage];
    }
    float previous = voice->apf_fb ? *voice->apf_fb : 0.0f;
    float x = (input + c->feedback_prev * previous + c->now * offset) / c->denominator;
    float y = x;
    for (int stage = 0; stage < stages; ++stage) {
        float out = -a * y + voice->apf[stage];
        voice->apf[stage] = y + a * out;
        y = out;
    }
    if (voice->apf_fb) *voice->apf_fb = sf_clampf(y, -4.0f, 4.0f);
    /* A phaser: the all-pass chain summed with its own input, which is what
     * cuts the notches (one per two stages, measured on MiniFreak V). */
    return SF_PHASER_MIX * (x + y) * c->out_gain;
}

static float destroy_process(const sf_engine_voice_t *voice,
                             const sf_engine_params_t *params, float input) {
    /* Wavefolder -> decimator -> bit crusher, in the manual's knob order:
     * Wave = Fold, Timbre = Decimate, Shape = BitCrush. Only the samples the
     * decimator holds are folded. */
    if (!voice->sh_hold || !voice->sh_count) return input;
    if (*voice->sh_count == 0u) {
        float fold_amount = sf_clampf(params->wave, 0.0f, 1.0f);
        float decimate = sf_clampf(params->timbre, 0.0f, 1.0f);
        *voice->sh_hold = fold_sample_biased(input, fold_amount, 0.0f);
        *voice->sh_count = 1u + (unsigned)(decimate * decimate * 63.0f);
    }
    --(*voice->sh_count);
    float out = *voice->sh_hold;
    int bits = 16 - (int)(sf_clampf(params->shape, 0.0f, 1.0f) * 12.0f);
    if (bits < 4) bits = 4;
    float levels = (float)(1u << (bits - 1));
    return roundf(out * levels) * engine_pow2_neg(bits - 1);
}

/* Position / width / tone voice shared by both oscillators' Wavetable types.
 * `width` is the duty of the first half-cycle (0.5 leaves the table untouched);
 * `tone` is a centred one-pole low-pass / high-pass (0.5 is unfiltered). */
static float wavetable_voice_sample_cached(int table, float position, float width, float tone,
                                           const sf_engine_voice_t *voice,
                                           sf_wavetable_cache_t *c) {
    int knobs = !c->valid || c->kind != 1 || c->table != table || c->wave != position
             || c->timbre != width || c->shape != tone;
    if (knobs) {
        c->kind = 1;
        c->table = table;
        c->wave = position;
        c->timbre = width;
        c->shape = tone;
        if (table < 0) table = 0;
        if (table >= SF_WT_FAMILY_COUNT) table = SF_WT_FAMILY_COUNT - 1;
        position = sf_clampf(position, 0.0f, 1.0f);
        width = sf_clampf(width, 0.0f, 1.0f);
        float duty = 0.5f + (width - 0.5f) * 0.9f;
        c->duty = duty;
        c->rise = 0.5f / duty;
        c->fall = 0.5f / (1.0f - duty);
        wavetable_position((float)table, position, &c->pos);
        tone = sf_clampf(tone, 0.0f, 1.0f);
        c->filter = 0;
        if (!(fabsf(tone - 0.5f) < 0.002f)) {
            float cutoff = tone < 0.5f
                ? 20.0f * engine_exp2f(tone * 2.0f * 9.813781f)
                : 20.0f * engine_exp2f((tone - 0.5f) * 2.0f * 9.813781f);
            float g = engine_fast_svf_g(cutoff);
            c->alpha = g / (1.0f + g);
            c->filter = tone < 0.5f ? 1 : 2;
        }
    }
    if (knobs || c->increment != voice->increment) {
        c->increment = voice->increment;
        c->level = wavetable_level(voice->increment);
    }
    c->valid = 1;
    float duty = c->duty;
    float phase = voice->phase;
    if (phase < duty) phase = phase * c->rise;
    else phase = 0.5f + (phase - duty) * c->fall;
    float sample = wavetable_sample_at(&c->pos, c->level, phase);

    if (!voice->filter_state || c->filter == 0) return sample;
    voice->filter_state->low += c->alpha * (sample - voice->filter_state->low);
    return c->filter == 1 ? voice->filter_state->low
                          : sample - voice->filter_state->low;
}

static float wavetable_voice_sample(int table, float position, float width, float tone,
                                    const sf_engine_voice_t *voice) {
    if (voice->wavetable_cache)
        return wavetable_voice_sample_cached(table, position, width, tone, voice,
                                             voice->wavetable_cache);
    if (table < 0) table = 0;
    if (table >= SF_WT_FAMILY_COUNT) table = SF_WT_FAMILY_COUNT - 1;
    position = sf_clampf(position, 0.0f, 1.0f);
    width = sf_clampf(width, 0.0f, 1.0f);
    float duty = 0.5f + (width - 0.5f) * 0.9f;
    float phase = voice->phase;
    if (phase < duty) phase = phase * (0.5f / duty);
    else phase = 0.5f + (phase - duty) * (0.5f / (1.0f - duty));
    float sample = wavetable_sample((float)table, position, phase, voice->increment);

    tone = sf_clampf(tone, 0.0f, 1.0f);
    if (!voice->filter_state || fabsf(tone - 0.5f) < 0.002f) return sample;
    float cutoff = tone < 0.5f
        ? 20.0f * engine_exp2f(tone * 2.0f * 9.813781f)
        : 20.0f * engine_exp2f((tone - 0.5f) * 2.0f * 9.813781f);
    float g = engine_fast_svf_g(cutoff);
    float alpha = g / (1.0f + g);
    voice->filter_state->low += alpha * (sample - voice->filter_state->low);
    return tone < 0.5f ? voice->filter_state->low
                       : sample - voice->filter_state->low;
}

/* Both oscillators read Wavetable the same way, as the knobs are labelled:
 * Wave = position, Timbre = width (squared, 0 leaves the table untouched),
 * Shape = tone. A MiniFreak bank's knob values are converted to these by
 * scripts/generate_minifreak_bank.py rather than remapped here. */
static float minifreak_wavetable_sample(const sf_engine_params_t *params,
                                        const sf_engine_voice_t *voice) {
    return wavetable_voice_sample(params->table, params->wave,
                                  0.5f - 0.5f * params->timbre * params->timbre,
                                  params->shape, voice);
}

/* ------------------------------------------------------------- Two Op FM -- */

/* Adapted from Emilie Gillet's Plaits FMEngine and lut_fm_frequency_quantizer
 * (plaits/dsp/engine/fm_engine.cc, plaits/resources.cc; MIT). */
static const float plaits_lut_fm_frequency_quantizer[129] = {
    -12.0f, -12.0f, -12.0f, -11.84f, -11.84f, -11.84f, -11.11f, -10.38f,
    -9.65f, -8.92f, -8.19f, -7.46f, -6.73f, -6.0f, -6.0f, -6.0f,
    -5.5455116f, -5.0910232f, -4.6365348f, -4.1820464f, -4.1820464f, -4.1820464f,
    -3.6592906f, -3.1365348f, -2.6137790f, -2.0910232f, -1.5682674f, -1.0455116f,
    -0.5227558f, 0.0f, 0.0f, 0.0f,
    0.16f, 0.16f, 0.16f, 0.89f, 1.62f, 2.35f, 3.08f, 3.81f,
    4.54f, 5.27f, 6.0f, 6.0f, 6.0f, 6.4544884f, 6.9089768f, 7.3634652f,
    7.8179536f, 7.8179536f, 7.8179536f, 8.2855299f, 8.7531063f, 9.2206827f,
    9.6882591f, 9.6882591f, 9.6882591f, 10.2661943f, 10.8441295f, 11.4220648f,
    12.0f, 12.0f, 12.0f, 12.16f, 12.16f, 12.16f, 12.6297750f, 13.0995500f,
    13.5693250f, 14.0391000f, 14.0391000f, 14.0391000f, 14.9076199f, 15.7761397f,
    16.6446596f, 17.5131794f, 17.5131794f, 17.5131794f, 18.0f, 18.0f,
    18.0f, 18.5097750f, 19.0195500f, 19.0195500f, 19.0195500f, 19.8179536f,
    19.8179536f, 19.8179536f, 20.6638643f, 21.5097750f, 21.5097750f, 21.5097750f,
    22.1323313f, 22.7548875f, 23.3774438f, 24.0f, 24.0f, 24.0f,
    24.5097750f, 25.0195500f, 25.0195500f, 25.0195500f, 25.4740384f, 25.9285268f,
    26.3830152f, 26.8375036f, 26.8375036f, 26.8375036f, 27.3503204f, 27.8631371f,
    27.8631371f, 27.8631371f, 28.3973529f, 28.9315686f, 29.4657843f, 30.0f,
    30.0f, 30.0f, 30.75f, 31.5f, 32.25f, 33.0f, 33.75f, 34.5f,
    35.25f, 36.0f, 36.0f, 36.0f, 36.0f
};

static float two_op_fm_sample(const sf_engine_params_t *params,
                              const sf_engine_voice_t *carrier,
                              const sf_engine_voice_t *modulator) {
    /* Plaits FMEngine, measured on MiniFreak V. Wave is the 129-entry ratio
     * quantizer (-12..+36 st: 0.242 -> 1:1, 0.488 -> 2:1, 0.650 -> 3:1,
     * 1.000 -> 8:1), Timbre the index (2 x Timbre^2 cycles of phase
     * modulation, 4 pi rad at full, with no high-note taming), and Shape a
     * bipolar feedback around 0.5: below it the previous output bends the
     * modulator's frequency by 0.5 fb^2, above it phase-modulates the
     * modulator by 0.25 fb^2 cycles. The modulator keeps its own phase
     * (stack_phase[0]); stack_phase[1] holds the previous output. */
    (void)modulator;
    /* The ratio, index and feedback only move with the knobs. */
    sf_fm_cache_t local, *c = carrier->fm_cache;
    if (!c) {
        local = (sf_fm_cache_t){0};
        c = &local;
    }
    if (!c->valid || c->wave != params->wave || c->timbre != params->timbre
        || c->shape != params->shape) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        float rpos = sf_clampf(params->wave, 0.0f, 1.0f) * 128.0f;
        int ra = (int)rpos;
        if (ra > 127) ra = 127;
        float rblend = rpos - (float)ra;
        float ratio_semi = plaits_lut_fm_frequency_quantizer[ra]
            + (plaits_lut_fm_frequency_quantizer[ra + 1]
               - plaits_lut_fm_frequency_quantizer[ra]) * rblend;
        c->ratio = engine_exp2f(ratio_semi / 12.0f);
        float t = sf_clampf(params->timbre, 0.0f, 1.0f);
        c->amount = 2.0f * t * t;
        c->feedback = 2.0f * sf_clampf(params->shape, 0.0f, 1.0f) - 1.0f;
        c->fb2 = c->feedback * c->feedback;
        c->valid = 1;
    }
    float ratio = c->ratio;
    float amount = c->amount;
    float feedback = c->feedback;
    float fb2 = c->fb2;

    float *state = carrier->stack_phase;
    if (!state) {
        float mod = sinf(SF_TWO_PI * carrier->sub_phase * 2.0f * ratio);
        return sinf(SF_TWO_PI * (carrier->phase + amount * mod));
    }
    float previous = state[1];
    float mod_increment = carrier->increment * ratio;
    if (feedback < 0.0f) mod_increment *= 1.0f + previous * 0.5f * fb2;
    mod_increment = sf_clampf(mod_increment, -0.5f, 0.5f);
    float mod_phase = state[0] + mod_increment;
    mod_phase -= floorf(mod_phase);
    state[0] = mod_phase;
    float mod_pm = feedback > 0.0f ? 0.25f * fb2 * previous : 0.0f;
    float mod = sinf(SF_TWO_PI * (mod_phase + mod_pm));
    float out = sinf(SF_TWO_PI * (carrier->phase + amount * mod));
    state[1] = out;
    return out;
}

/* MiniFreak Osc 2 FM / RM processor, measured on MiniFreak V. Osc 2 is a
 * sine -> triangle -> saw -> square crossfade on Wave (1/3 steps, all four
 * with their fundamentals in phase), added at its Volume while Osc 1 passes
 * through at its own. Osc 1's audio before its Volume is the modulator:
 * Timbre phase-modulates Osc 2 by 52.5 rad per unit of a full-level sine
 * (pitch independent), and Shape crossfades Osc 2 into Osc 2 times Osc 1
 * squared up by a comparator: +1.42 while Osc 1 is below zero, -0.90 above
 * (the polarity follows a Basic Waves pulse's duty cycle into the carrier). */
#define SF_FMRM_PM_INDEX 8.356f   /* 52.5 rad / 2 pi, in cycles */
#define SF_FMRM_RING_DC 0.26f
#define SF_FMRM_RING_SWING 1.16f
#define SF_FMRM_COMPARATOR_GAIN 16.0f

static float fm_rm_wave_sample(float phase, float increment, int segment, float blend) {
    float tri_phase = phase + 0.25f;
    if (tri_phase >= 1.0f) tri_phase -= 1.0f;
    float saw_phase = phase + 0.5f;
    if (saw_phase >= 1.0f) saw_phase -= 1.0f;
    float a;
    float b;
    if (segment == 0) {
        a = sinf(SF_TWO_PI * phase);
        b = triangle_sample(tri_phase);
    } else if (segment == 1) {
        a = triangle_sample(tri_phase);
        b = basic_saw(saw_phase, increment);
    } else {
        a = basic_saw(saw_phase, increment);
        float edge = phase + 0.5f;
        if (edge >= 1.0f) edge -= 1.0f;
        b = (phase < 0.5f ? 1.0f : -1.0f)
          + poly_blep(phase, increment) - poly_blep(edge, increment);
    }
    return a + (b - a) * blend;
}

static float fm_rm_sample(const sf_engine_params_t *params,
                          const sf_engine_voice_t *oscillator2, float modulator) {
    /* The depth, ring amount and wave segment only move with the knobs. */
    sf_fmrm_cache_t local, *c = oscillator2->fmrm_cache;
    if (!c) {
        local = (sf_fmrm_cache_t){0};
        c = &local;
    }
    if (!c->valid || c->wave != params->wave || c->timbre != params->timbre
        || c->shape != params->shape) {
        c->wave = params->wave;
        c->timbre = params->timbre;
        c->shape = params->shape;
        float timbre = sf_clampf(params->timbre, 0.0f, 1.0f);
        float shape = sf_clampf(params->shape, 0.0f, 1.0f);
        c->pm = SF_FMRM_PM_INDEX * timbre;
        /* Ring amount against Shape, fitted to the sideband levels. */
        c->ring = (0.74f + 0.26f * shape) * shape;
        float position = sf_clampf(params->wave, 0.0f, 1.0f) * 3.0f;
        c->segment = position >= 3.0f ? 2 : (int)position;
        c->blend = position - (float)c->segment;
        c->valid = 1;
    }
    float phase = oscillator2->phase + c->pm * modulator;
    phase -= floorf(phase);
    float osc = fm_rm_wave_sample(phase, oscillator2->increment, c->segment, c->blend);
    float ring = c->ring;
    float square = sf_clampf(-modulator * SF_FMRM_COMPARATOR_GAIN, -1.0f, 1.0f);
    float gain = 1.0f - ring + ring * (SF_FMRM_RING_DC + SF_FMRM_RING_SWING * square);
    return osc * gain;
}

/* ------------------------------------------------------------- dispatch -- */

float sf_engine1_sample(const sf_engine_params_t *params,
                        const sf_engine_voice_t *carrier,
                        const sf_engine_voice_t *modulator) {
    switch (params->type) {
    case OSC_FM:
        return two_op_fm_sample(params, carrier, modulator);
    case OSC_FOLD: {
        float source = standard_wave_sample(carrier->phase, carrier->increment,
                                            params->wave * 0.65f);
        return fold_sample_cached(carrier->fold_cache, source, params->timbre,
                                  (params->shape - 0.5f) * 1.4f);
    }
    case OSC_NOISE:
        return minifreak_noise_sample(carrier, params);
    case OSC_WAVETABLE:
        return SF_TRIM_WAVETABLE * minifreak_wavetable_sample(params, carrier);
    case OSC_SUPERWAVE: return superwave_sample(params, carrier);
    case OSC_HARMO:     return harmo_sample(params, carrier);
    case OSC_FORMANT:   return formant_sample(params, carrier);
    case OSC_VANALOG:   return vanalog_sample(params, carrier);
    case OSC_WAVESHAPER:return waveshaper_sample(params, carrier);
    case OSC_CHORD:     return chord_sample(params, carrier);
    case OSC_KARPLUS:   return SF_TRIM_KARPLUS * karplus_sample(params, carrier);
    case OSC_SAWX:      return sawx_sample(params, carrier);
    case OSC_HARM:      return ne_harm_sample(params, carrier);
    case OSC_BASS:      return ne_bass_sample(params, carrier);
    case OSC_MODAL:     return SF_TRIM_MODAL * modal_sample(params, carrier);
    case OSC_SPEECH:    return SF_TRIM_SPEECH * speech_sample(params, carrier);
    case OSC_SAMPLE:
    case OSC_PARTICLES:
    case OSC_CLOUD:
    case OSC_HITGRAINS:
    case OSC_FROZEN:
    case OSC_SKAN:
    case OSC_LICK:
    case OSC_RASTER:
        return granular_placeholder_sample(params, carrier);
    case OSC_AUDIOIN:
        return audioin_placeholder_sample(params, carrier);
    default:
        /* BasicWaves sits about 6 dB below the other generators at the same
         * Volume setting on the MiniFreak; the reference Default preset is
         * 1.0x at 0.5 gain and 2.0x without it. */
        return minifreak_basic_wave_sample(params, carrier);
    }
}

float sf_engine2_mix(const sf_engine_params_t *params,
                     const sf_engine_voice_t *oscillator2,
                     const sf_engine_voice_t *oscillator1,
                     float oscillator1_sample, float oscillator1_level,
                     float oscillator2_level, float *raw2_out) {
    float input = oscillator1_sample * oscillator1_level;
#define RAW2_OUT() do { if (raw2_out) *raw2_out = raw2; } while (0)
    float raw2 = 0.0f;

    switch (params->type) {
    /* ---- generators: added to whatever Osc 1 produced ---- */
    case ENGINE_WAVETABLE:
        /* Same controls as Oscillator 1: Wave = position, Timbre = width
         * (squared, 0 leaves the table untouched), Shape = tone; the bank comes from
         * the separate osc2_table selector. */
        raw2 = SF_TRIM_WAVETABLE * wavetable_voice_sample(
            params->table, params->wave, 0.5f - 0.5f * params->timbre * params->timbre,
            params->shape, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_SUPERWAVE:
        raw2 = superwave_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_HARMO:
        raw2 = harmo_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_VANALOG:
        raw2 = vanalog_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_WAVESHAPER:
        raw2 = waveshaper_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_KARPLUS:
        raw2 = SF_TRIM_KARPLUS * karplus_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_SAWX:
        raw2 = sawx_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_HARM:
        raw2 = ne_harm_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_BASS:
        raw2 = ne_bass_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_MODAL:
        raw2 = SF_TRIM_MODAL * modal_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_SPEECH:
        raw2 = SF_TRIM_SPEECH * speech_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_FORMANT:
        raw2 = formant_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_SAMPLE:
    case ENGINE_PARTICLES:
    case ENGINE_CLOUD:
    case ENGINE_HITGRAINS:
    case ENGINE_FROZEN:
    case ENGINE_SKAN:
    case ENGINE_LICK:
    case ENGINE_RASTER:
        raw2 = granular_placeholder_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_AUDIOIN:
        raw2 = audioin_placeholder_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_CHORD:
        raw2 = chord_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_TWO_OP_FM:
        raw2 = two_op_fm_sample(params, oscillator2, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    case ENGINE_FM_RM:
        raw2 = fm_rm_sample(params, oscillator2, oscillator1_sample);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;

    /* ---- processors: act on Osc 1, blended by oscillator2_level ---- */
    case ENGINE_MULTIFILTER: {
        float wet = multifilter_process(oscillator2, params, input);
        if (raw2_out) *raw2_out = wet;
        return input * (1.0f - oscillator2_level) + wet * oscillator2_level;
    }
    case ENGINE_SURGEON: {
        float wet = surgeon_process(oscillator2, oscillator1, params, input);
        if (raw2_out) *raw2_out = wet;
        return input * (1.0f - oscillator2_level) + wet * oscillator2_level;
    }
    case ENGINE_COMB:
        raw2 = comb_process(oscillator2, params, input);
        RAW2_OUT();
        return input * (1.0f - oscillator2_level) + raw2 * oscillator2_level;
    case ENGINE_PHASERFILTER:
        raw2 = phaser_process(oscillator2, params, input);
        RAW2_OUT();
        return input * (1.0f - oscillator2_level) + raw2 * oscillator2_level;
    case ENGINE_DESTROY:
        raw2 = destroy_process(oscillator2, params, input);
        RAW2_OUT();
        return input * (1.0f - oscillator2_level) + raw2 * oscillator2_level;
    case ENGINE_CRUSHER: {
        int bits = 16 - (int)(params->wave * 12.0f);
        if (bits < 4) bits = 4;
        if (bits > 16) bits = 16;   /* a Wave below 0 would shift past the word */
        float levels = (float)(1u << (bits - 1));
        float drive = 1.0f + params->timbre * 8.0f;
        float crushed = roundf((input * drive + (params->shape - 0.5f) * 0.1f) * levels)
                      * engine_pow2_neg(bits - 1);
        if (raw2_out) *raw2_out = crushed;
        return input * (1.0f - oscillator2_level) + crushed * oscillator2_level;
    }
    case ENGINE_RING:
        raw2 = basic_wave_sample(oscillator2->phase, oscillator2->increment,
                                 params->wave, params->shape);
        RAW2_OUT();
        return input + input * raw2 * oscillator2_level * params->timbre * 2.0f;
    case ENGINE_FOLD: {
        float fold_input = input * (0.5f + params->wave * 2.0f);
        raw2 = fold_sample_cached(oscillator2->fold_cache, fold_input, params->timbre,
                                  (params->shape - 0.5f) * 1.4f);
        RAW2_OUT();
        return input * (1.0f - oscillator2_level) + raw2 * oscillator2_level;
    }
    case ENGINE_NOISE:
        raw2 = minifreak_noise_sample(oscillator2, params);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    default:
        raw2 = minifreak_basic_wave_sample(params, oscillator2);
        RAW2_OUT();
        return input + raw2 * oscillator2_level;
    }
#undef RAW2_OUT
}

int sf_engine1_uses_delay(int type) {
    return type == OSC_KARPLUS;
}

int sf_engine2_uses_delay(int type) {
    return type == ENGINE_KARPLUS || type == ENGINE_COMB;
}

void sf_engines_init(void) {
    if (!g_harmo_amp_ready) harmo_build_table();
    if (!g_chord_wt_ready) chord_wavetable_init();
    if (!g_multifilter_steps_ready) multifilter_build_steps();
}
