#ifndef BIZAR_VOICE_H
#define BIZAR_VOICE_H

#include "engines.h"
#include "filter.h"
#include "modulation.h"

#define SF_MAX_VOICES 6
/* Velo > Time, measured on MiniFreak V: Decay and Release stretch by
 * exp(K * amount * velocity), so amount 1 at full velocity is about 30x. */
#define SF_VEL_TIME_K 3.45f

enum {
    VOICE_POLY = 0,
    VOICE_MONO = 1,
    VOICE_UNISON = 2
};

enum {
    ENV_OFF = 0,
    ENV_ATTACK = 1,
    ENV_DECAY = 2,
    ENV_SUSTAIN = 3,
    ENV_RELEASE = 4
};

/* Envelope stage shapes from the MiniFreak curve menus: Default and Quick
 * for the attack, Default and Percussive for decay and release. */
enum {
    ENV_CURVE_DEFAULT = 0,
    ENV_CURVE_QUICK = 1
};

/* One stage's shape, as the fraction of the way to the stage target reached
 * at linear stage position t (0..1). A single exponential when `a` is 0, the
 * Percussive fast-plus-slow pair otherwise; `norm` stretches it so t = 1
 * lands exactly on the target. Built once per block (or at note-off), so the
 * per-sample cost is one or two expf calls. */
typedef struct {
    float k;      /* single exponential: e^-kt */
    float a;      /* Percussive: weight of the fast component */
    float kf, ks; /* Percussive: fast and slow rates, per stage length */
    float norm;
} sf_env_shape_t;

/* LFO retrigger modes, in the MiniFreak's order. The hardware's LFO, CycEnv
 * and Seq Start sources are imported as Mono Kbd. */
enum {
    LFO_RETRIG_FREE = 0,
    LFO_RETRIG_POLY = 1,
    LFO_RETRIG_MONO = 2,
    LFO_RETRIG_LEGATO = 3,
    LFO_RETRIG_ONE = 4,
    LFO_RETRIG_COUNT = 5
};

/* Cycling envelope stages: MiniFreak's Rise / Hold / Fall. */
enum {
    CYCLE_IDLE = 0,
    CYCLE_RISE = 1,
    CYCLE_HOLD = 2,
    CYCLE_FALL = 3,
    CYCLE_RELEASE = 4   /* Env mode only */
};

/* Cycling envelope modes, as on the MiniFreak. Env is a one-shot Rise/Fall
 * with Hold as its sustain level. Run free-runs and never restarts; Loop
 * restarts with each note. Both keep cycling after the note is released. */
enum {
    CYCLE_ENV = 0,
    CYCLE_RUN = 1,
    CYCLE_LOOP = 2
};

/* Env-mode Cycling Envelope stage shapes and steps, built once per block. */
typedef struct {
    sf_env_shape_t rise, fall, release;
    float rise_step;
    float fall_step;
    float release_samples;
} sf_cycle_env_t;

typedef struct {
    int mode;
    float pressure;
    float spread_cents;
    int cycle_mode;
    /* 0 = oscillators retrigger with the note, 1 = free-running. */
    int osc_phase;
    /* LFO_RETRIG_*. Poly Kbd and One restart this voice's LFO; the shared
     * (Free, Mono Kbd, Legato Kbd) phases are restarted by the caller. */
    int lfo1_retrig;
    int lfo2_retrig;
    /* 0 = retrigger the envelope on every note, 1 = let a legato
     * note carry the envelope on (MiniFreak's Env Continue). */
    int env_retrig;
    unsigned *age_counter;
    /* Voices stacked in Unison mode, 2..6. */
    int unison_voices;
    /* Nonzero: the seed of this note's Sample & Hold draws, shared by a
     * Unison stack so it plays one random value, as the MiniFreak's single
     * stacked voice does. 0: the voice draws its own. */
    uint32_t lfo_seed;
    /* The key that played this voice, which its note-off releases. A
     * keyboard-chord voice plays another note than its key; -1 means the
     * key is the note itself. */
    int key;
} sf_voice_start_params_t;

typedef struct {
    int active;
    /* Steal fade: a start that would cut an audible voice is deferred until
     * a ~3 ms fade-out ends, then run from these saved arguments. */
    int steal_pending;
    int steal_note;
    int steal_velocity;
    int steal_channel;
    int steal_index;
    sf_voice_start_params_t steal_params;
    int note;
    /* See sf_voice_start_params_t.key. */
    int key;
    int channel;
    int env_stage;
    unsigned age;
    float velocity;
    float pressure;
    float env;
    float release_step;
    /* Envelope value when the note was released, so the release curve
     * can shape a fall from wherever the envelope actually was. */
    float release_from;
    /* Release stage shape, fixed when the note ends. */
    sf_env_shape_t release_shape;
    /* Last shaped (audible) envelope output, so a release starts from the
     * value actually heard rather than the raw linear phase. */
    float last_env_out;
    /* Release-length factor from the modulation grid, kept up to date by the
     * render loop so a note released mid-block sees the current value. */
    float release_scale;
    float base_note_hz;
    float current_hz;
    float target_hz;
    float phase1;
    float phase2;
    float lfo1_phase;
    float lfo2_phase;
    /* One mode: set once the LFO has run its single cycle. */
    int lfo1_done;
    int lfo2_done;
    /* Sample & Hold / Slew S&H LFO state, re-sampled on each phase wrap. */
    sf_lfo_snh_t lfo1_snh;
    sf_lfo_snh_t lfo2_snh;
    int cycle_stage;
    float cycle_value;
    /* Last Cycling Envelope output, and (Env mode) the level its release
     * started from. */
    float cycle_out;
    float cycle_from;
    float cycle_hold_left;
    /* Run/Loop bend denominators for the Rise [0] and Fall [1] curves, cached
     * against the curve they were computed for. */
    float cycle_bend_k[2];
    float cycle_bend_den[2];
    float sub_phase1;
    float sub_phase2;
    float noise_state1;
    float noise_state2;
    unsigned noise_count1;
    unsigned noise_count2;
    float super_phase1[SF_SUPERWAVE_COPIES];
    float super_phase2[SF_SUPERWAVE_COPIES];
    sf_vanalog_state_t vanalog1;
    sf_vanalog_state_t vanalog2;
    float chord_phase2[SF_CHORD_PHASES];
    float chord_phase1[SF_CHORD_PHASES];
    sf_filter_state_t filter;
    sf_filter_state_t osc1_filter;
    sf_filter_state_t osc2_filter;
    sf_filter_state_t multifilter[3];
    float multifilter_lp6;
    /* Delay lines for Karplus-Strong and the Comb Filter, one per engine
     * position (see SF_VOICE_DELAY). */
    float delay1[SF_VOICE_DELAY];
    float delay2[SF_VOICE_DELAY];
    unsigned delay_pos1;
    unsigned delay_pos2;
    unsigned karplus_excitation_left1;
    unsigned karplus_excitation_left2;
    float delay_low1;
    float delay_low2;
    /* DC blocker memory for the Formant / Speech engines, one per position. */
    float formant_dc1;
    float formant_dc2;
    /* Plaits Modal resonators, one per oscillator position. */
    sf_modal_state_t modal1;
    sf_modal_state_t modal2;
    /* Plaits NaiveSpeechSynth state, one per oscillator position. */
    sf_speech_state_t speech1;
    sf_speech_state_t speech2;
    /* Up to twelve all-pass stages for the Phaser Filter, plus its feedback memory,
     * which is an Osc 2 processor only. */
    float apf[SF_PHASER_MAX_STAGES];
    float apf_fb;
    /* Sample-and-hold state for the Destroy processor, also Osc 2 only. */
    float sh_hold;
    unsigned sh_count;
    float unison_cents;
    float unison_ratio;
    float unison_gain;
    /* Glide: the target the current slide is heading for, and the per-sample
     * pitch ratio it moves by (MiniFreak V glides in a straight line in
     * semitones). */
    float glide_target_hz;
    float glide_ratio;
    /* DC blocker on the oscillator mix. */
    float dc_in;
    float dc_out;
    /* Filter FM: the source's own DC blocker, and last sample's filter input
     * and source for the 2x oversampled filter's mid-point. */
    float fm_dc_in;
    float fm_dc_out;
    float fm_prev_signal;
    float fm_prev_src;
    float random_bipolar;
    uint32_t rng;
    /* Sample & Hold draws (see sf_voice_start_params_t.lfo_seed). */
    uint32_t lfo_rng;
} sf_voice_t;


sf_voice_t *sf_voice_allocate(sf_voice_t voices[SF_MAX_VOICES]);
/* The MiniFreak's order of the unison slot `index` in a stack of `voices`
 * (+1, -1, +2, -2, ... steps). A keyboard chord deals the stack out in this
 * order: rank r plays chord note r mod notes (measured). */
int sf_unison_rank(int index, int voices);
void sf_voice_start(sf_voice_t *voice, int index, int note, int velocity,
                    int channel, int legato,
                    const sf_voice_start_params_t *params);
void sf_voice_release_note(sf_voice_t voices[SF_MAX_VOICES], int note,
                           int channel, float release_ms, float vel_time,
                           int release_curve);
void sf_voice_release_channel(sf_voice_t voices[SF_MAX_VOICES], int channel,
                             int immediate, float release_ms, float vel_time,
                             int release_curve);
/* Builds a stage shape for a stage of nominal length `ms` (the Default
 * curve's time to the target) and returns the length the stage actually
 * runs for: a Percussive stage runs until its slow tail reaches -60 dB. */
float sf_env_shape_setup(sf_env_shape_t *shape, int stage, int curve, float ms);
/* A stage time moved by a modulation amount the way the MiniFreak moves it:
 * along its knob (see voice.c). `stage` is ENV_ATTACK, ENV_DECAY or
 * ENV_RELEASE. */
float sf_env_time_mod(int stage, float ms, float amount);
float sf_voice_envelope_sample(sf_voice_t *voice, float attack_step,
                               float decay_step, float sustain,
                               const sf_env_shape_t *attack_shape,
                               const sf_env_shape_t *decay_shape);
float sf_voice_cycle_sample(sf_voice_t *voice, int mode, int gated,
                            float rise_step, float fall_step,
                            float hold_value,
                            float rise_curve, float fall_curve,
                            const sf_cycle_env_t *env);
/* The last value sf_voice_cycle_sample produced, for the per-block routing
 * passes that read the Cycling Envelope once per block. */
float sf_voice_cycle_output(const sf_voice_t *voice);
/* The release (ms to -60 dB) the ADSR gives at the knob position whose
 * decay is `decay_ms`: Env mode's Fall knob sets both. */
float sf_env_release_for_decay(float decay_ms);

#endif
