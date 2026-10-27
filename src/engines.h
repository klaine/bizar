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

/* Builds the engines' lookup tables. Call once before rendering so the
 * build never lands on the audio thread. */
void sf_engines_init(void);

#endif
