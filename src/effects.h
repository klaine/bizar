#ifndef BIZAR_EFFECTS_H
#define BIZAR_EFFECTS_H

#define SF_FX_SLOTS 3
/* Delay memory is shared by every effect. 131072 samples is 2.97 s at 44.1
 * kHz, enough for a 1/1 delay at 80 BPM (the old 16384-sample buffer only held
 * 371 ms, so tempo-synced quarter notes were impossible). The incremental
 * clear below is sized so this larger buffer is ready shortly after startup. */
#define SF_DELAY_SIZE 131072
#define SF_FX_CLEAR_CHUNK 2048

/* Reverb network: four input-diffusion all-pass filters feeding a four-line
 * feedback delay network (FDN), laid out at fixed offsets inside the slot's
 * delay buffer (see the regions in effects.c). The right channel runs 23
 * samples longer per line, which is what decorrelates the two sides. */
#define SF_REV_COMBS 4
#define SF_REV_APS 4
#define SF_FX_COMP_BANDS 3
/* The phaser is six identical first-order all-pass stages, which is what gives
 * the MiniFreak's three notches at 0.27x, 1x and 3.7x the corner frequency. */
#define SF_PHASER_STAGES 6
/* The longest free-running delay, in ms. The Time control spans 1..this for
 * every effect; only Delay reads it as milliseconds. */
#define SF_FX_TIME_MAX 1000.0f

/* Direct-form-I biquad coefficients, normalised so a0 == 1. */
typedef struct {
    float b0, b1, b2, a1, a2;
} sf_biquad_t;

enum {
    FX_OFF = 0,
    FX_DELAY = 1,
    FX_REVERB = 2,
    FX_CHORUS = 3,
    FX_DRIVE = 4,
    FX_CRUSH = 5,
    FX_PHASER = 6,
    FX_EQ = 7,
    FX_FLANGER = 8,
    FX_SUPER_UNISON = 9,
    FX_PEAK_EQ = 10,
    FX_MULTICOMP = 11,
    FX_TYPE_COUNT = 12
};

typedef struct {
    float mix_smooth[SF_FX_SLOTS];
    float eq_smooth_db[SF_FX_SLOTS][3];
    float eq_low_state[SF_FX_SLOTS][2];
    float peak_frequency_smooth[SF_FX_SLOTS];
    float peak_gain_smooth[SF_FX_SLOTS];
    float peak_width_smooth[SF_FX_SLOTS];
    float peak_b0[SF_FX_SLOTS];
    float peak_b1[SF_FX_SLOTS];
    float peak_b2[SF_FX_SLOTS];
    float peak_a1[SF_FX_SLOTS];
    float peak_a2[SF_FX_SLOTS];
    float peak_x1[SF_FX_SLOTS][2];
    float peak_x2[SF_FX_SLOTS][2];
    float peak_y1[SF_FX_SLOTS][2];
    float peak_y2[SF_FX_SLOTS][2];
    float comp_env[SF_FX_SLOTS][2][SF_FX_COMP_BANDS];
    unsigned pos[SF_FX_SLOTS];
    unsigned clear_pos[SF_FX_SLOTS];
    float delay_l[SF_FX_SLOTS][SF_DELAY_SIZE];
    float delay_r[SF_FX_SLOTS][SF_DELAY_SIZE];
    float phase[SF_FX_SLOTS];
    float phaser_l[SF_FX_SLOTS][SF_PHASER_STAGES];
    float phaser_r[SF_FX_SLOTS][SF_PHASER_STAGES];
    float phaser_fb[SF_FX_SLOTS][2];
    /* SnH phaser: the held sweep position and the step it belongs to. */
    float phaser_hold[SF_FX_SLOTS];
    int phaser_step[SF_FX_SLOTS];
    unsigned phaser_rng[SF_FX_SLOTS];
    float crush_l[SF_FX_SLOTS];
    float crush_r[SF_FX_SLOTS];
    /* Fractional sample-and-hold clock: the MiniFreak's downsampling ratios
     * are not whole numbers (2.45x at the bottom of the knob). */
    float crush_phase[SF_FX_SLOTS];
    /* 3-band EQ (low shelf, bell, high shelf) and the Distortion tone filter.
     * Coefficients are rebuilt only when their controls move; the *_key
     * fields hold the control values the current coefficients were made for. */
    sf_biquad_t eq_bq[SF_FX_SLOTS][3];
    float eq_z[SF_FX_SLOTS][3][2][2];
    float eq_key_db[SF_FX_SLOTS][3];
    int eq_key_sub[SF_FX_SLOTS];
    sf_biquad_t drive_bq[SF_FX_SLOTS];
    float drive_z[SF_FX_SLOTS][2][2];
    float drive_key[SF_FX_SLOTS];
    /* Per-sample gains that only move with a knob, kept with the control
     * value (key) they were computed for; a key outside the control's range
     * marks them unset. */
    float drive_gain_key[SF_FX_SLOTS];
    float drive_gain[SF_FX_SLOTS];
    float mix_key[SF_FX_SLOTS];
    float mix_wet[SF_FX_SLOTS];
    float mix_dry[SF_FX_SLOTS];
    float unison_tone_key[SF_FX_SLOTS];
    float unison_tone_alpha[SF_FX_SLOTS];
    /* Multi Comp gains are recomputed every few samples and held between. */
    float comp_gain[SF_FX_SLOTS][2][SF_FX_COMP_BANDS];
    sf_biquad_t comp_split[SF_FX_SLOTS][2];
    float comp_z[SF_FX_SLOTS][2][2][2];
    unsigned comp_split_ready[SF_FX_SLOTS];
    unsigned comp_tick[SF_FX_SLOTS];
    /* Reverb. rev_ready is 0 until the comb/all-pass region has been cleared
     * for this slot, and is re-armed whenever the slot runs a different effect
     * type so a delay tail is never read back as reverb. */
    unsigned rev_comb_pos[SF_FX_SLOTS][SF_REV_COMBS][2];
    float rev_comb_low[SF_FX_SLOTS][SF_REV_COMBS][2];
    unsigned rev_ap_pos[SF_FX_SLOTS][SF_REV_APS][2];
    unsigned rev_pre_pos[SF_FX_SLOTS];
    float rev_mod_phase[SF_FX_SLOTS];
    /* Precomputed sine/cosine offsets let the reverb modulation use one
     * sin/cos pair per sample instead of one per FDN line. */
    float rev_mod_sin[SF_FX_SLOTS][SF_REV_COMBS][2];
    float rev_mod_cos[SF_FX_SLOTS][SF_REV_COMBS][2];
    unsigned rev_ready[SF_FX_SLOTS];
    unsigned rev_clear[SF_FX_SLOTS];
    /* Per-line loop gain and damping, derived from a decay time; rebuilt only
     * when Time, Intensity or the preset change. */
    float rev_gain[SF_FX_SLOTS][SF_REV_COMBS];
    float rev_damp[SF_FX_SLOTS][SF_REV_COMBS];
    float rev_level[SF_FX_SLOTS];
    float rev_key_time[SF_FX_SLOTS];
    float rev_key_fb[SF_FX_SLOTS];
    int rev_key_sub[SF_FX_SLOTS];
    /* State for the filtered/tape delay feedback path. Kept separate from the
     * EQ and drive filters because a slot can switch between those effects. */
    float delay_low_l[SF_FX_SLOTS];
    float delay_low_r[SF_FX_SLOTS];
    /* Delay loop filters: the Filtered band-pass (high-pass, low-pass) and
     * the Ping-Pong/Mono low cut. */
    sf_biquad_t delay_bq[SF_FX_SLOTS][3];
    float delay_z[SF_FX_SLOTS][2][3][2];
    unsigned delay_bq_ready[SF_FX_SLOTS];
} sf_effect_state_t;

void sf_effect_state_init(sf_effect_state_t *state,
                          const float mix[SF_FX_SLOTS],
                          const float eq_db[SF_FX_SLOTS][3],
                          const float peak_frequency[SF_FX_SLOTS],
                          const float peak_gain[SF_FX_SLOTS],
                          const float peak_width[SF_FX_SLOTS]);
void sf_effect_state_smooth(sf_effect_state_t *state,
                            const float mix[SF_FX_SLOTS],
                            const float eq_db[SF_FX_SLOTS][3],
                            const float peak_frequency[SF_FX_SLOTS],
                            const float peak_gain[SF_FX_SLOTS],
                            const float peak_width[SF_FX_SLOTS], float amount);
void sf_effect_clear_buffers(sf_effect_state_t *state);
/* Clears every buffer and filter memory after a non-finite sample so a NaN in
 * a feedback path cannot latch. */
void sf_effect_reset_nonfinite(sf_effect_state_t *state);
/* A modulation route's offset (in knob units) applied to an effect's stored
 * Time (ms or knob-position ms) and mix. Intensity is a plain knob position. */
float sf_effect_mod_time(int type, int subtype, float time_ms, float offset);
float sf_effect_mod_mix(int type, float mix, float offset);
/* The same for the 3 Bands and Peak EQs, whose three knobs are band gains
 * or frequency, gain and width: route = Time, Intensity, Amount offsets. */
void sf_effect_mod_eq(int type, const float route[3], float eq_db[3],
                      float *peak_hz, float *peak_gain, float *peak_width);
void sf_effect_process(sf_effect_state_t *state, int slot, int type, int subtype,
                       float mix, float time_ms, float feedback, float tempo,
                       float input_l, float input_r,
                       float *output_l, float *output_r);

#endif
