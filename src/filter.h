#ifndef BIZAR_FILTER_H
#define BIZAR_FILTER_H

#include <math.h>

enum {
    FILTER_LP = 0,
    FILTER_BP = 1,
    FILTER_HP = 2,
    FILTER_NOTCH = 3
};

typedef struct {
    float low;
    float band;
    /* Passband gain cached for passband_res; 0 (never a valid gain) marks
     * it unset so a zeroed state recomputes. */
    float passband_res;
    float passband;
} sf_filter_state_t;

/* 10^(-0.41 pb_res^1.3), the passband loss at a Resonance already capped at
 * 0.95, memoised in the state. */
float sf_filter_passband(sf_filter_state_t *state, float pb_res);
float sf_filter_process(sf_filter_state_t *state, int mode, float g,
                        float resonance, float input);

/* sf_filter_process's coefficients for a fixed g and Resonance, for a caller
 * that keeps them while its knobs hold still. They exist only below
 * self-oscillation (Resonance up to 0.95), where the damping does not depend
 * on the state. */
typedef struct {
    float drive, passband, damping, a1, a2, a3;
} sf_filter_coefs_t;

/* Fills *coefs and returns 1, or returns 0 when g or Resonance is not
 * finite or Resonance is above 0.95: sf_filter_process must run instead. */
int sf_filter_coefs(sf_filter_state_t *state, float g, float resonance,
                    sf_filter_coefs_t *coefs);
/* Moves coefficients from sf_filter_coefs to a new g at the same Resonance
 * (a swept cutoff), or returns 0 when g is not finite. */
static inline int sf_filter_coefs_g(sf_filter_coefs_t *coefs, float g) {
    if (!isfinite(g)) return 0;
    float a1 = 1.0f / (1.0f + g * (g + coefs->damping));
    float a2 = g * a1;
    coefs->a1 = a1;
    coefs->a2 = a2;
    coefs->a3 = g * a2;
    return 1;
}
/* sf_filter_process on coefficients from sf_filter_coefs: the same output. */
float sf_filter_process_coefs(sf_filter_state_t *state, int mode,
                              const sf_filter_coefs_t *coefs, float input);

#endif
