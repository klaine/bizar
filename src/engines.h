#ifndef BIZAR_ENGINES_H
#define BIZAR_ENGINES_H

#include <stdint.h>

#include "filter.h"

#define SF_WT_FAMILY_COUNT 32
#define SF_LEGACY_WT_FAMILY_COUNT 4

/* Per-voice delay memory for the Karplus-Strong string and the Comb Filter.
 * 4096 samples at 44.1 kHz is 93 ms, so the lowest tunable pitch is ~11 Hz:
 * the Comb can tune down to 0.092x the note (Wave 0), which needs it. Each engine position owns its own
 * buffer, so Osc 1 = Karplus and Osc 2 = Comb cannot stomp on one another. */
#define SF_VOICE_DELAY 4096
#define SF_PHASER_MAX_STAGES 12

/* Oscillator 1 modes. ORDER IS A PRESET FORMAT: `state`, the factory patches
 * and every saved Schwung patch store the mode as this integer, so new modes
 * are appended. The wavetable-based Additive mode was removed in state version
 * 18 (the MiniFreak has no such engine) and Osc 2's Cross FM in 19;
 * apply_state_json shifts older indices down. Never reorder the block above `OSC_VANALOG` otherwise. */
enum {
    OSC_VA = 0,
    OSC_FM = 1,
    OSC_FOLD = 2,
    OSC_NOISE = 3,
    OSC_WAVETABLE = 4,
    OSC_SUPERWAVE = 5,
    OSC_HARMO = 6,
    OSC_FORMANT = 7,
    /* --- appended; do not reorder anything above this line --- */
    OSC_VANALOG = 8,
    OSC_WAVESHAPER = 9,
    OSC_CHORD = 10,
    OSC_KARPLUS = 11,
    OSC_SAWX = 12,
    OSC_HARM = 13,
    OSC_BASS = 14,
    OSC_MODAL = 15,
    OSC_SPEECH = 16,
    OSC_SAMPLE = 17,
    OSC_PARTICLES = 18,
    OSC_CLOUD = 19,
    OSC_HITGRAINS = 20,
    OSC_FROZEN = 21,
    OSC_SKAN = 22,
    OSC_LICK = 23,
    OSC_RASTER = 24,
    OSC_AUDIOIN = 25,
    SF_OSC1_MODE_COUNT = 26
};

/* Oscillator 2 modes: generators, plus the signal processors that act on
 * Oscillator 1. Same preset-format rule as Osc 1: append only. */
enum {
    ENGINE_VA = 0,
    ENGINE_RING = 1,
    ENGINE_FOLD = 2,
    ENGINE_NOISE = 3,
    ENGINE_WAVETABLE = 4,
    ENGINE_SUPERWAVE = 5,
    ENGINE_HARMO = 6,
    ENGINE_CHORD = 7,
    ENGINE_MULTIFILTER = 8,
    ENGINE_CRUSHER = 9,
    /* --- appended; do not reorder anything above this line --- */
    ENGINE_COMB = 10,
    ENGINE_SURGEON = 11,
    ENGINE_PHASERFILTER = 12,
    ENGINE_DESTROY = 13,
    ENGINE_VANALOG = 14,
    ENGINE_WAVESHAPER = 15,
    ENGINE_KARPLUS = 16,
    ENGINE_SAWX = 17,
    ENGINE_HARM = 18,
    ENGINE_BASS = 19,
    ENGINE_MODAL = 20,
    ENGINE_SPEECH = 21,
    ENGINE_SAMPLE = 22,
    ENGINE_PARTICLES = 23,
    ENGINE_CLOUD = 24,
    ENGINE_HITGRAINS = 25,
    ENGINE_FROZEN = 26,
    ENGINE_SKAN = 27,
    ENGINE_LICK = 28,
    ENGINE_RASTER = 29,
    ENGINE_AUDIOIN = 30,
    /* Appended for the MiniFreak bank: Osc 2 has a Formant generator distinct
     * from Speech. Existing state blobs never used this index, so this is safe. */
    ENGINE_FORMANT = 31,
    /* The MiniFreak's own Osc 2 FM pair: its Two Op FM generator (Osc 1's
     * engine, on Osc 2) and its FM / RM processor, which phase- and
     * ring-modulates an Osc 2 sine..square by Osc 1's audio.  */
    ENGINE_TWO_OP_FM = 32,
    ENGINE_FM_RM = 33,
    SF_OSC2_MODE_COUNT = 34
};

#define SF_SUPERWAVE_COPIES 4
#define SF_CHORD_PHASES 5

typedef struct {
    int type;
    float wave;
    float timbre;
    float shape;
    int table;
} sf_engine_params_t;

/* Allocation-free state for the adapted Plaits V.Analog variable-saw and
 * hard-sync pulse oscillators. One pair per voice and oscillator position. */
typedef struct {
    float phase;
    float next_sample;
    float previous_pw;
    int high;
} sf_plaits_variable_saw_state_t;

typedef struct {
    float slave_phase;
    float next_sample;
    float previous_pw;
    int high;
} sf_plaits_sync_state_t;

typedef struct {
    sf_plaits_variable_saw_state_t saw;
    sf_plaits_sync_state_t sync;
    int initialized;
} sf_vanalog_state_t;

/* Plaits ModalVoice: 12 modes (MiniFreak's count, measured) rendered as
 * three banks of four resonators, followed by the excitation filter. The mode
 * amplitudes are fixed by the engine's 0.015 strike position and live in
 * plaits_modal_luts.h. */
#define SF_MODAL_MODES 12
#define SF_MODAL_BANKS 3
typedef struct {
    float mode_state_1[SF_MODAL_BANKS][4];
    float mode_state_2[SF_MODAL_BANKS][4];
    float excitation_1;
    float excitation_2;
    float harmonics_lp;
    int was_gated;
} sf_modal_state_t;

/* Plaits NaiveSpeechSynth: a polyBLEP impulse train through one colouration
 * filter and five formant band-passes. */
typedef struct {
    float pulse_phase;
    float pulse_next;
    float pulse_lp;
    float pulse_hp;
    int click_left;
    int was_gated;
    unsigned age;
    float formant_1[5];
    float formant_2[5];
    float coloration_1;
    float coloration_2;
} sf_speech_state_t;

/* Harmo's partial weights, kept for as long as the knobs and the partial
 * count that made them hold still: rebuilding them every sample was most of
 * the engine's cost. count 0 marks it empty. */
#define SF_HARMO_PARTIALS 40
typedef struct {
    float wave;
    float timbre;
    int count;
    float weights[SF_HARMO_PARTIALS];
} sf_harmo_cache_t;

/* Karplus Strong's and the Comb Filter's coefficients, kept per oscillator
 * position in two levels: the knob level while Wave, Timbre and Shape hold
 * still, and the pitch level (built from it) while the increment does too.
 * Working them out every sample was most of both engines' cost. valid 0
 * marks a cache empty. */
typedef struct {
    int valid;
    float wave, timbre, shape, increment;
    /* Knob level. */
    float bow, bow_q, bow_x, lp_mult, loss_table, fraction, alpha_bow;
    /* Pitch level. */
    float f0, inv_f0, hp, loop_alpha, tuned_delay, loop_gain, alpha_strike;
    unsigned tap;
} sf_karplus_cache_t;

typedef struct {
    int valid;
    float wave, timbre, shape, increment;
    /* Knob level. */
    float ratio, in_gain, feedback, alpha, lag;
    /* Pitch level. */
    float delay;
} sf_comb_cache_t;

/* The Noise engine's coefficients, kept while Wave, Timbre and Shape hold
 * still: the dust filters and level, the metal bank's increments and gains
 * and the tone level took 5 exp2s, 2 expf, a powf, 3 square roots and
 * several divisions every sample. valid 0 marks it empty. */
typedef struct {
    int valid;
    float wave, timbre, shape;
    int dust_on, metal_on;
    float dust_fade, al, ah, hp0, mix, density, gain;
    float metal_mix, metal_level, metal_inc[6], metal_g[6];
    float balance, tone_gain, tone_level;
} sf_noise_cache_t;

/* Two Op FM's ratio, index and feedback, kept while Wave, Timbre and Shape
 * hold still. */
typedef struct {
    int valid;
    float wave, timbre, shape;
    float ratio, amount, feedback, fb2;
} sf_fm_cache_t;

/* The coloured noise's (Audio In placeholder's) knob terms, and the metallic
 * oscillator's increment and phase ratio for the increment they were made
 * at. */
typedef struct {
    int valid;
    float type, rate, balance, increment;
    unsigned period;
    float type_c, balance_c, spread, tone_mix, norm;
    float metallic_increment, metallic_ratio;
} sf_colored_noise_cache_t;

/* FM / RM's phase-modulation depth, ring amount and wave segment, kept
 * while Wave, Timbre and Shape hold still. */
typedef struct {
    int valid;
    float wave, timbre, shape;
    int segment;
    float blend, pm, ring;
} sf_fmrm_cache_t;

/* The Wavefolder's gain, offset and the sine of the folded offset, kept
 * while its amount and bias hold still. */
typedef struct {
    int valid;
    float amount, bias;
    float gain, dc, ref_sin;
} sf_fold_cache_t;
/* BasicWaves' knob-derived values, kept while Wave, Timbre and Shape hold
 * still. valid 0 marks it empty. */
typedef struct {
    int valid;
    float wave, timbre, shape;
    int pulse;          /* Wave <= 0.5: pulse..saw, else saw..double saw */
    float offset, mix, osc_gain, sine_gain;
} sf_basic_cache_t;

/* V.Analog's coefficients in two levels, as Karplus': the knob level while
 * Wave, Timbre and Shape hold still, the pitch level while the increment
 * does too. */
typedef struct {
    int valid;
    float wave, timbre, shape, increment;
    /* Knob level. The saw slopes are for a pulse width the pitch leaves
     * unclamped. */
    float saw_ratio, saw_pulse_width, triangle_amount, notch_amount, saw_gain;
    float saw_slope_up, saw_slope_down;
    float square_pulse_width, square_gain, square_ratio, normalization;
    /* Pitch level: both oscillators' clamped frequencies and pulse widths. */
    float saw_frequency, saw_pw, slope_up, slope_down;
    float master_frequency, slave_frequency, square_pw;
} sf_vanalog_cache_t;

/* Waveshaper's coefficients, in the same two levels. */
typedef struct {
    int valid;
    float wave, timbre, shape, increment;
    /* Knob level. */
    float harmonics, pw, slope, shape_amount, wavefolder_gain;
    float slope_up, slope_down;
    /* Pitch level. */
    float f0, tick_pw, tick_slope_up, tick_slope_down, wf_gain, shape_fractional;
    int shape_integral;
} sf_waveshaper_cache_t;

/* SuperWave's and SawX's wave choice, gains and detune ratios (knob level)
 * and their copies' increments (pitch level; increment -1 when the copies
 * are off). */
typedef struct {
    int valid;
    float wave, timbre, shape, increment;
    int selected;
    float copy_level, norm, level_gain;
    float ratio[SF_SUPERWAVE_COPIES];
    float copy_increment[SF_SUPERWAVE_COPIES];
} sf_superwave_cache_t;

typedef struct {
    int valid;
    float wave, timbre, shape, increment;
    int copies;
    float chorus, noise_pm, mod_gain, norm_root;
    float ratio[SF_SUPERWAVE_COPIES];
    float copy_increment[SF_SUPERWAVE_COPIES];
} sf_sawx_cache_t;
/* Chords' per-sample constants, two levels like Karplus: the knob level
 * (chord voicing, registration gains, wavetable blend) while Wave, Timbre and
 * Shape hold still, and each chord voice's clamped increments while the
 * increment does too. valid 0 marks it empty. */
typedef struct {
    int valid;
    float wave, timbre, shape, increment;
    /* Knob level. */
    float ratios[SF_CHORD_PHASES];
    float amplitudes[SF_CHORD_PHASES];
    float gain_8, gain_4, gain_2, gain_1;
    float wavetable;
    int table;
    float table_mix;
    unsigned gain_mask;               /* saws with any gain */
    /* Pitch level: the 8', 4', 2' and 1' increments of each voice. */
    float inc[SF_CHORD_PHASES][4];
    unsigned mask[SF_CHORD_PHASES];   /* saws with gain, under Nyquist */
} sf_chord_cache_t;

/* A wavetable read's knob- and pitch-dependent parts: the family and frame
 * pair with their crossfades, and the mip level. */
typedef struct {
    int family_a, family_b, frame_a, frame_b;
    float family_mix, frame_mix;
} sf_wt_pos_t;

/* Wavetable (and the granular placeholders, which stand in with it). Knob
 * level: the table read position, the width warp's slopes and the tone
 * filter's coefficient; pitch level: the mip level. */
typedef struct {
    int valid;
    int kind;              /* 1 Wavetable, 2 granular placeholder */
    int table;
    float wave, timbre, shape, increment;
    sf_wt_pos_t pos;
    float duty, rise, fall;
    int filter;            /* 0 off, 1 low-pass, 2 high-pass */
    float alpha;
    int level;
} sf_wavetable_cache_t;

/* Harm's knob-level constants (two powf and four exp2 per sample). */
typedef struct {
    int valid;
    float wave, timbre, shape;
    float keep, rect_mix, noise_pow;
    float ratio[4];
} sf_harm_cache_t;

/* Bass: the two wavefolders' gains, offsets and the sine of their offset,
 * which depend on Timbre and Shape alone. */
typedef struct {
    int valid;
    float timbre, shape;
    float gain1, dc1, sref1, gain2, dc2, sref2;
} sf_bass_cache_t;

/* Speech's formant band-pass coefficients, kept while the phoneme and
 * register positions and the attack click hold still (the positions, not
 * the knobs, so a word that has reached its last phoneme hits the cache
 * too), the sustained phoneme position while Wave and Shape hold still, and
 * the word's last level. valid 0 marks it empty. */
typedef struct {
    int valid;
    float p, r;
    int click;
    float g[5], h[5], amp[5];
    /* The phoneme table interpolated across the register, for one pair of
     * phonemes: a word walks across a pair for a quarter of its length. */
    int corners_valid, corner_pi;
    float corner_r;
    float fr0[5], fr1[5], ar0[5], ar1[5];
    int position_valid;
    float wave, shape, position;
    /* Word mode: Timbre's stretch, and the age from which the word's level
     * has reached its last point (it only grows with age). */
    int stretch_valid;
    float stretch_timbre, stretch;
    int word_done;
    unsigned done_age;
    int level_valid;
    float db, level;
} sf_speech_cache_t;

/* Formant's (Plaits grainlet pair) coefficients in two levels: the knob
 * level while Wave, Timbre and Shape hold still, and the pitch level while
 * the increment does too. The carrier's values at the grain's end and start
 * are filled in at the first carrier reset that needs them. */
typedef struct {
    int valid;
    float wave, timbre, shape, increment;
    /* Knob level. */
    float f1, f2, bleed;
    /* Pitch level. */
    float f0, carrier_shape, dc_f;
    int carrier_integral;
    float carrier_m, carrier_bp, carrier_half_over_bp, carrier_one_minus_bp;
    int ends_valid;
    float carrier_end, after;
} sf_formant_cache_t;

/* Modal's resonator coefficients in two levels: the knob level while
 * Timbre, Shape and the smoothed structure hold still, and the pitch level
 * while the increment does too. */
typedef struct {
    int valid;
    float timbre, shape, harmonics_lp, increment;
    /* Knob level. */
    float brightness, damping, cutoff_ratio, stretch_base;
    float stretch[SF_MODAL_MODES], q[SF_MODAL_MODES];
    /* Pitch level. */
    float cutoff, exc_g, exc_h;
    float g[SF_MODAL_MODES], rg[SF_MODAL_MODES], h[SF_MODAL_MODES],
          gain[SF_MODAL_MODES];
} sf_modal_cache_t;

/* The Multi Filter's, Surgeon's and Phaser's coefficients, which only move
 * with Wave, Timbre and Shape (none of them tracks the pitch). valid 0 marks
 * a cache empty. The Multi Filter's are in two levels, Wave's and Timbre and
 * Shape's, so a modulated Wave keeps the second. */
typedef struct {
    int valid;
    float wave, timbre, shape;
    /* Wave level. */
    float cutoff, g, alpha;
    /* Timbre and Shape level. */
    int mode, filter_mode, stages;
    float stage_res, makeup;
    /* Both: sf_filter_coefs, when coefs_ok. */
    int coefs_ok;
    sf_filter_coefs_t coefs;
} sf_multifilter_cache_t;

typedef struct {
    int valid;
    float wave, timbre, shape;
    float g, resonance;
    int filter_mode, coefs_ok;
    sf_filter_coefs_t coefs;
} sf_surgeon_cache_t;

typedef struct {
    int valid;
    float wave, timbre, shape;
    int stages;
    float a, feedback_prev, now, denominator, out_gain;
} sf_phaser_cache_t;

typedef struct {
    float phase;
    float increment;
    float sub_phase;
    uint32_t *rng;
    float *noise_state;
    unsigned *noise_count;
    float *stack_phase;
    float *chord_phase;
    sf_vanalog_state_t *vanalog_state;
    sf_filter_state_t *filter_state;
    sf_filter_state_t *multifilter_states;
    float *multifilter_lp6;
    /* Appended for the delay-based and multi-stage engines. A NULL pointer
     * disables the engine that needs it, so a caller that only wires up the
     * original field set still renders everything that does not use one. */
    float *delay_buf;
    unsigned *delay_pos;
    float *delay_low;
    unsigned *excitation_left;
    int gate;
    /* One-pole DC blocker memory for the Formant / Speech grainlets. */
    float *formant_dc;
    /* Modal resonator state; NULL disables the Modal engine. */
    sf_modal_state_t *modal_state;
    /* Naive speech synthesiser state; NULL disables the Speech engine. */
    sf_speech_state_t *speech_state;
    float *apf;
    float *apf_fb;
    float *sh_hold;
    unsigned *sh_count;
    /* Harmo's weight cache; NULL rebuilds the weights every sample. */
    sf_harmo_cache_t *harmo_cache;
    /* Karplus and Comb coefficient caches; NULL works them out every sample. */
    sf_karplus_cache_t *karplus_cache;
    sf_comb_cache_t *comb_cache;
    /* Noise, Two Op FM and Wavefolder caches; NULL works them out every
     * sample. */
    sf_noise_cache_t *noise_cache;
    sf_fm_cache_t *fm_cache;
    sf_fold_cache_t *fold_cache;
    sf_colored_noise_cache_t *colored_cache;
    sf_fmrm_cache_t *fmrm_cache;
    /* BasicWaves, V.Analog, Waveshaper, SuperWave and SawX caches; NULL
     * works their coefficients out every sample. */
    sf_basic_cache_t *basic_cache;
    sf_vanalog_cache_t *vanalog_cache;
    sf_waveshaper_cache_t *waveshaper_cache;
    sf_superwave_cache_t *superwave_cache;
    sf_sawx_cache_t *sawx_cache;
    /* Chords, Wavetable (and the granular placeholders), Harm and Bass
     * caches; NULL works them out every sample. */
    sf_chord_cache_t *chord_cache;
    sf_wavetable_cache_t *wavetable_cache;
    sf_harm_cache_t *harm_cache;
    sf_bass_cache_t *bass_cache;
    /* Speech, Formant and Modal coefficient caches; NULL works them out
     * every sample. */
    sf_speech_cache_t *speech_cache;
    sf_formant_cache_t *formant_cache;
    sf_modal_cache_t *modal_cache;
    /* Processor coefficient caches; NULL works them out every sample. */
    sf_multifilter_cache_t *multifilter_cache;
    sf_surgeon_cache_t *surgeon_cache;
    sf_phaser_cache_t *phaser_cache;
} sf_engine_voice_t;

float sf_engine1_sample(const sf_engine_params_t *params,
                        const sf_engine_voice_t *carrier,
                        const sf_engine_voice_t *modulator);

/* raw2_out (optional) receives Osc 2's output before its level knob: the
 * generator's own sample, or a processor's wet signal. */
float sf_engine2_mix(const sf_engine_params_t *params,
                     const sf_engine_voice_t *oscillator2,
                     const sf_engine_voice_t *oscillator1,
                     float oscillator1_sample, float oscillator1_level,
                     float oscillator2_level,
                     float *raw2_out);

/* Whether an Osc 1 or Osc 2 mode reads and writes its position's delay line
 * (delay_buf), which the voice then has to clear before the engine runs. */
int sf_engine1_uses_delay(int type);
int sf_engine2_uses_delay(int type);

/* Builds the engines' lookup tables. Call once before rendering so the
 * build never lands on the audio thread. */
void sf_engines_init(void);

#endif
