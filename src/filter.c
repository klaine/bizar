#include "filter.h"

#include <math.h>

/* A plain clamp, unlike sf_clampf: sf_filter_process rejects non-finite
 * input and state on entry, so this per-sample path skips the check. */
static float filter_clampf(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

#ifndef SF_FILTER_SELFOSC_DAMP
#define SF_FILTER_SELFOSC_DAMP 0.3f
#endif
#ifndef SF_FILTER_SELFOSC_LEVEL
#define SF_FILTER_SELFOSC_LEVEL 0.108f
#endif

#ifndef SF_FILTER_SAT
#define SF_FILTER_SAT 0.3f
#endif

static float steiner_diode_clip(float x) {
    /* Odd soft saturation on the SVF bandpass state: limits a loud resonant
     * peak without injecting a DC bias that 1/g would amplify into a large
     * offset at low cutoffs. It is applied every sample, so it must stay
     * linear for small signals: the old x / (1 + 0.26|x|) shaved a few percent
     * off even a quiet state on every pass and held the resonant peak 12-15 dB
     * below MiniFreak V's at high Resonance. */
    return x / sqrtf(1.0f + SF_FILTER_SAT * x * x);
}

float sf_filter_passband(sf_filter_state_t *state, float pb_res) {
    /* Resonance usually holds still for many samples, so the two powf calls
     * only run when it moves. */
    if (state->passband == 0.0f || state->passband_res != pb_res) {
        state->passband_res = pb_res;
        state->passband = powf(10.0f, -0.41f * powf(pb_res, 1.3f));
    }
    return state->passband;
}

float sf_filter_process(sf_filter_state_t *state, int mode, float g,
                        float resonance, float input) {
    if (!isfinite(input) || !isfinite(state->low) || !isfinite(state->band) ||
        !isfinite(g) || !isfinite(resonance)) {
        state->low = 0.0f;
        state->band = 0.0f;
        return 0.0f;
    }
    float res = filter_clampf(resonance, 0.0f, 1.0f);
    /* MiniFreak V's passband sinks as Resonance rises (-3.5 dB at 0.5, -6 at
     * 0.8, -7.5 at 0.95, measured on LP, BP and HP alike) while the resonant
     * peak keeps its height. The input is scaled by that loss and the
     * damping narrowed by the same factor, so the peak - and with it the
     * level the soft clip below sees - stays where it was. */
    float pb_res = res < 0.95f ? res : 0.95f;
    float passband = sf_filter_passband(state, pb_res);
    float damping = (2.0f - 2.04f * res) * passband;
    if (res > 0.95f) {
        /* Above 0.95 the MiniFreak's filter goes into self-oscillation: the
         * damping reaches zero at 0.967 and turns negative, and a small kick
         * lets it start from silence. */
        damping = res < 0.967f ? 0.062f * passband * (0.967f - res) / 0.017f
                                : -SF_FILTER_SELFOSC_DAMP * (res - 0.967f) / 0.033f;
        if (damping < 0.0f) {
            if (fabsf(state->low) + fabsf(state->band) < 1e-5f) input += 1e-3f;
            /* Amplitude-dependent damping (Van der Pol): the ring grows until
             * the two integrator states reach the measured level, then holds. */
            float amp2 = (state->low * state->low + state->band * state->band) * 0.25f;
            damping *= 1.0f - amp2 / (SF_FILTER_SELFOSC_LEVEL * SF_FILTER_SELFOSC_LEVEL);
        }
    }
    float a1 = 1.0f / (1.0f + g * (g + damping));
    float a2 = g * a1;
    float a3 = g * a2;
    float driven_in = input * (1.0f + 0.32f * res) * passband;
    /* The soft clip bleeds energy in proportion to level, which would pin a
     * self-oscillating ring far below the target at low cutoffs; the
     * amplitude-dependent damping bounds it instead. */
    float ic1eq = damping < 0.0f ? state->low : steiner_diode_clip(state->low);
    float ic2eq = state->band;
    float v3 = driven_in - ic2eq;
    float v1 = a1 * ic1eq + a2 * v3;
    float v2 = ic2eq + a2 * ic1eq + a3 * v3;
    state->low = filter_clampf(2.0f * v1 - ic1eq, -6.0f, 6.0f);
    state->band = filter_clampf(2.0f * v2 - ic2eq, -6.0f, 6.0f);
    float high = driven_in - damping * v1 - v2;
    if (mode == FILTER_BP) return v1;
    if (mode == FILTER_HP) return high;
    if (mode == FILTER_NOTCH) return v2 + high;
    return v2;
}
