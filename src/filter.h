#ifndef BIZAR_FILTER_H
#define BIZAR_FILTER_H

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

#endif
