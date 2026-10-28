/*
 * Bizar: original two-engine hybrid sound generator for Ableton Move.
 *
 * This is an API-v2 Schwung module. All mutable synthesis state is per
 * instance. A fixed BSS-backed instance pool avoids heap allocation in every
 * plugin entry point, including create/destroy. DSP is intentionally compact
 * and original; it does not reproduce Arturia firmware or proprietary modes.
 */

#include "plugin_api_v2_compat.h"
#include "dsp_common.h"
#include "effects.h"
#include "engines.h"
#include "filter.h"
#include "factory_bank.h"
#include "minifreak_bank.h"
#include "sequencer.h"
#include "move_info.h"
#include "modulation.h"
#include "voice.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SF_PROFILE
#include <time.h>
#endif

#define SF_INSTANCE_POOL 8
/* Keyboard Chord notes added to each key: the rest of the six voices. */
#define SF_CHORD_MAX_NOTES (SF_MAX_VOICES - 1)

/* Bizar's own 128-preset factory bank (scripts/generate_factory_bank.py),
 * then the optional MiniFreak bank import (scripts/generate_minifreak_bank.py).
 * SF_MF_PRESET_COUNT is 0 unless a .mnfx export was present at build time. */
#define SF_FACTORY_PRESET_COUNT (SF_ORIGINAL_PRESET_COUNT + SF_MF_PRESET_COUNT)

/*
 * Cutoff is a NORMALISED 0..1 log control, not a frequency in Hz.
 *
 * Like NuSaw and the other Schwung sound generators, the knob domain is the
 * plain 0..1 range the host already speaks, and the exponential fit to real
 * Hz lives here in the DSP (see cutoff_hz_from_param). One unit of knob
 * travel is therefore a fixed fraction of the range, so a slow turn is fine
 * at the bottom and a fast sweep still reaches the top without needing a
 * special "wide" acceleration the host does not actually implement.
 * The read-only `cutoff_hz` next to it reports the real frequency, and the
 * Remote UI shows Hz as well.
 *
 * 0 = 20 Hz, 1 = 18 kHz, 0.898 = 9 kHz (the default).
 */
#define SF_CUTOFF_MIN_HZ 20.0f
/* Octaves the cutoff swings at Filter FM = 1 for a full-scale (+/-1) source.
 * The knob is squared (fm * |fm|), so half travel is a quarter of this. */
#define SF_FILTER_FM_OCTAVES 6.0f
#define SF_CUTOFF_MAX_HZ 18000.0f
#define SF_CUTOFF_PARAM_MAX 1.0f
#define SF_CUTOFF_DEFAULT 0.898f
/* MiniFreak V's longest glide is about 11.2 s. */
#define SF_GLIDE_MAX_MS 12000.0f
/* A full +1 route into Pitch, Pitch 1 or Pitch 2 moves five octaves, linear
 * down to the smallest vibrato depths (measured on MiniFreak V). */
#define SF_PITCH_MOD_SEMITONES 60.0f
#define SF_DEFAULT_TEMPO 120.0f

/*
 * Control rate (the Mod Rate setting). The modulation grid, LFOs, glide, pitch
 * and the filter/VCA controls run once every g_control_divs[control_rate]
 * samples per voice and ramp linearly to each new value over that many
 * samples (5.5 kHz at 1/8, far above the 100 Hz LFO ceiling). The
 * oscillators, filter, amp envelope and Cycling Envelope always run every
 * sample. Plaits does the same in blocks of 24. Measured with six voices,
 * 1/2 saves about a fifth of the render and 1/8 about a third, all within
 * 1 dB of Full.
 */
static const int g_control_divs[] = {1, 2, 4, 8};
static const char *const g_control_rates[] = {"Full", "1/2", "1/4", "1/8"};
#define SF_CONTROL_RATE_COUNT 4
#define SF_CONTROL_RATE_DEFAULT 1
enum {
    CTL_INC1, CTL_INC2,
    CTL_WAVE1, CTL_TIMBRE1, CTL_SHAPE1, CTL_WAVE2, CTL_TIMBRE2, CTL_SHAPE2,
    CTL_LEVEL1, CTL_LEVEL2, CTL_RES, CTL_FILTER_G, CTL_AMP, CTL_FM, CTL_FILTER_W,
    CTL_COUNT
};

typedef struct {
    /* 0 until the first tick, and the voice age that tick saw: a new note
     * (or a steal) starts from its own values instead of ramping from the
     * previous note's. */
    int valid;
    unsigned age;
    /* Samples left before the next tick. */
    int countdown;
    unsigned lfo1_wraps;
    unsigned lfo2_wraps;
    /* The ramped values and their per-sample steps. */
    float value[CTL_COUNT];
    float step[CTL_COUNT];
    /* Engine wiring, rebuilt each tick; the per-sample fields (phase,
     * increment, sub phase, gate) are filled in every sample. */
    sf_engine_voice_t osc1;
    sf_engine_voice_t osc2;
} sf_voice_ctl_t;

/* A one-pass index of a JSON object's top-level keys, so a state load looks
 * each of its few hundred keys up in a hash table instead of rescanning the
 * whole blob with strstr for every one (that was 0.6 ms a load on a desktop,
 * several times that on Move). Lives in the instance: no allocation and no
 * large stack frame on the audio thread. */
#define SF_JSON_KEYS 768
#define SF_JSON_SLOTS 2048
typedef struct {
    const char *text;          /* the string indexed, NULL when none */
    int count;
    uint32_t key_off[SF_JSON_KEYS];
    uint32_t val_off[SF_JSON_KEYS];
    uint16_t key_len[SF_JSON_KEYS];
    uint16_t slot[SF_JSON_SLOTS]; /* entry + 1, 0 empty */
} sf_json_index_t;

typedef struct {
    int pool_index;
    int osc1_mode;
    int osc1_table;
    int osc2_table;
    float osc1_wave;
    float osc1_wave_smooth;
    float osc1_timbre;
    float osc1_timbre_smooth;
    float osc1_shape;
    float osc1_shape_smooth;
    float osc1_tune;
    float osc1_level;
    float osc1_level_smooth;
    /* Pitch-modulation quantizer, per oscillator: Continuous or one of the
     * ten scales the MiniFreak offers under the waveform display. */
    int osc1_quantize;
    int osc2_mode;
    float osc2_wave;
    float osc2_wave_smooth;
    float osc2_timbre;
    float osc2_timbre_smooth;
    float osc2_shape;
    float osc2_shape_smooth;
    float osc2_tune;
    float osc2_level;
    float osc2_level_smooth;
    int osc2_quantize;
    float mix;
    float cutoff;
    float cutoff_smooth;
    float resonance;
    float resonance_smooth;
    int filter_mode;
    int filter_fm_src;
    float filter_env;
    float filter_fm;
    float filter_fm_smooth;
    float attack_ms;
    float decay_ms;
    float sustain;
    float release_ms;
    /* Envelope page, second row: the stage shapes, the retrigger switch and
     * the four velocity depths MiniFreak exposes next to them. */
    int attack_curve;
    int decay_curve;
    int release_curve;
    int env_retrig;
    float vel_vca;
    float vel_vcf;
    float vel_env;
    float vel_time;
    int velo_source;
    int kbd_source;
    float lfo1_rate;
    float lfo1_depth;
    int lfo1_shape;
    sf_seq_t seq;
    sf_shaper_t lfo1_shaper;
    float lfo2_rate;
    float lfo2_depth;
    int lfo2_shape;
    sf_shaper_t lfo2_shaper;
    int lfo1_sync;
    int lfo1_div;
    int lfo1_retrig;
    int lfo2_sync;
    int lfo2_div;
    int lfo2_retrig;
    int osc_phase;
    /* Mod Rate, an index into g_control_divs. A CPU setting rather than part
     * of the sound, so a preset load leaves it alone. */
    int control_rate;
    int cycle_mode;
    float cycle_rise_ms;
    float cycle_fall_ms;
    float cycle_hold_ms;
    float cycle_amount;
    /* Rise and Fall stage shapes, 0..1 with 0.5 linear. The hardware bends
     * them with Shift + the time knob; 0.5 is its default "straight" shape. */
    float cycle_rise_curve;
    float cycle_fall_curve;
    /* The hand-built Cycle Amount control also sweeps the filter directly.
     * Imported MiniFreak patches use only their explicit matrix routes. */
    int cycle_filter_direct;
    int voice_mode;
    float glide_ms;
    int glide_mode;
    float bend_range;
    float spread_cents;
    int unison_voices;
    /* 0: one stack follows the last key; 1: every key gets its own stack. */
    int unison_mode;
    /* Keyboard Chord: each key also plays chord_notes more notes, at
     * chord_offset semitones from it. */
    int chord;
    int chord_notes;
    int chord_offset[SF_CHORD_MAX_NOTES];
    float stereo;
    float output;
    float output_smooth;
    /* Shared phase for free-running (retrig off) LFOs. */
    float lfo1_phase_global;
    float lfo2_phase_global;
    /* The modulation grid: twelve destination columns, each fed by the seven
     * hardware source rows, and four macros of four destination slots. Storing
     * the matrix the way the hardware does means a preset's routing imports
     * without translation and without a route budget. */
    int mod_dest[SF_MOD_COLUMNS];
    float mod_amount[SF_MOD_COLUMNS][MODSRC_COUNT];
    /* The newest voice's source values from the last block, for the Remote
     * UI's live modulation needles (read-only `mod_sources`). */
    float ui_source[MODSRC_COUNT];
    int ui_source_live;
    /* The Move grid shows the matrix as a column list plus ONE eight-knob
     * editor page. The aliases below read and write the selected column, so
     * twelve routes cost two pages instead of twelve while the original
     * modcol keys, state and automation keep working unchanged. */
    int mod_edit_col;
    int macro_dest[SF_MACROS][SF_MACRO_SLOTS];
    float macro_amount[SF_MACROS][SF_MACRO_SLOTS];
    /* Mod wheel, CC 1. The grid's "Wheel" row reads it. */
    float wheel;
    float macros[4];
    int fx_type[SF_FX_SLOTS];
    int fx_subtype[SF_FX_SLOTS];
    float fx_mix[SF_FX_SLOTS];
    float fx_time[SF_FX_SLOTS];
    float fx_feedback[SF_FX_SLOTS];
    float fx_eq_db[SF_FX_SLOTS][3];
    float fx_peak_frequency[SF_FX_SLOTS];
    float fx_peak_gain[SF_FX_SLOTS];
    float fx_peak_width[SF_FX_SLOTS];
    /* Last block's routes into each slot's Time, Intensity and Amount, for the
     * EQs, whose knobs are smoothed before this block's routes resolve. */
    float fx_route[SF_FX_SLOTS][3];
    sf_effect_state_t fx_state;
    float bend_ratio[16];
    float bend_raw[16];
    float channel_pressure[16];
    int last_note;
    int preset_index;
    /* Preset-change declick: the last output sample, and what is left of it
     * fading out under the new preset (see cut_for_preset_change). */
    float last_out[2];
    float declick[2];
    int declick_left;
    int contract_dirty_blocks;
    int contract_was_dirty;
    unsigned age_counter;
    /* Held-note stack for mono/unison last-note priority. */
    uint8_t held_note[16];
    uint8_t held_vel[16];
    uint8_t held_chan[16];
    int held_count;
    int held_mode;
    /* Sustain pedal (CC 64), and the keys released while it was down in Seq
     * or Arp mode, which stay held in the sequencer until it comes up. */
    int sustain_down;
    uint32_t seq_sustained[4];
    sf_voice_t voices[SF_MAX_VOICES];
    sf_voice_ctl_t voice_ctl[SF_MAX_VOICES];
    /* Shared-LFO wraps so far, so a voice that only looks at the LFO once
     * per control tick still draws a new Sample & Hold value for each. */
    unsigned lfo1_wraps;
    unsigned lfo2_wraps;
    sf_json_index_t json_ix;
#ifdef SF_PROFILE
    float cpu_us; /* smoothed render time, profile builds only (cpu_us key) */
#endif
} sf_instance_t;

static sf_instance_t g_instances[SF_INSTANCE_POOL];
/* The host's API, for tempo and transport. NULL in the browser build and the
 * tests, where Move's set info (move_info.h) is the tempo source. */
static const host_api_v1_t *g_host;
/* Static storage: every flag starts at 0 (free). */
static atomic_uint g_instance_claimed[SF_INSTANCE_POOL];

static const char *const g_osc1_modes[SF_OSC1_MODE_COUNT] = {
    "BasicWaves", "Two Op FM", "Wavefolder", "Noise", "Wavetable",
    "SuperWave", "Harmo", "Formant",
    "V.Analog", "Waveshaper", "Chords", "KarplusStr", "SawX", "Harm", "Bass",
    "Modal", "Speech",
    "Sample", "Particles", "Cloud Grains", "Hit Grains", "Frozen", "Skan",
    "Lick", "Raster", "Audio In"
};
static const char *const g_osc2_modes[SF_OSC2_MODE_COUNT] = {
    "BasicWaves", "Ring", "Wavefolder", "Noise", "Wavetable",
    "SuperWave", "Harmo", "Chord", "MultiFilter", "Crusher",
    "Comb Filter", "Surgeon Filter", "Phaser Filter", "Destroy",
    "V.Analog", "Waveshaper", "KarplusStr", "SawX", "Harm", "Bass",
    "Modal", "Speech",
    "Sample", "Particles", "Cloud Grains", "Hit Grains", "Frozen", "Skan",
    "Lick", "Raster", "Audio In", "Formant", "Two Op FM", "FM / RM"
};
static const char *const g_wavetable_names[] = {
    "Harmonic", "Microwave 2", "Bell 03", "VoxSynth",
    "Spectral", "SuperSaw", "Fourier 2", "Noise Wave",
    "Folding", "Drone", "Sitar", "Tidal",
    "Harmonix", "Morphing", "Wavetrip", "Sine to Saw",
    "Saw Bend", "Basic Morph", "Tidy 006", "AM Sine",
    "Hmmm", "Iterative", "Osmaos", "Twist 02",
    "Sine Mutate", "Reso 00", "Sohler 34", "Sohler 35",
    "PD 101", "Sohler 43", "Mellow D", "Tidy 007"
};
static const char *const g_filter_modes[] = {"LP", "BP", "HP"};
static const char *const g_filter_fm_srcs[] = {"OSC 1", "OSC 2", "Mix"};
static const char *const g_voice_modes[] = {"Poly", "Mono", "Unison"};
static const char *const g_glide_modes[] = {"Time", "Rate"};
static const char *const g_unison_modes[] = {"Mono", "Poly"};
static const char *const g_off_on[] = {"Off", "On"};
static const char *const g_mod_quantize_modes[] = {
    "Continuous", "Chromatic", "Octaves", "Fifths", "Minor", "Major",
    "Phrygian Dominant", "Minor 9th", "Major 9th", "Minor Pentatonic",
    "Major Pentatonic"
};
static const char *const g_cycle_modes[] = {"Env", "Run", "Loop"};
static const char *const g_fx_types[FX_TYPE_COUNT] = {"Off", "Delay", "Reverb", "Chorus", "Drive", "Crusher", "Phaser", "3-Band EQ", "Flanger", "Super Unison", "Peak EQ", "Multi Comp"};
/* MiniFreak presets are per effect type, not one shared character list. The
 * old shared list forced Delay presets to be called Lush or Space, which made
 * the manual's Sync delay modes unreachable. These tables are indexed by
 * FX_TYPE_COUNT and each effect's subtype parameter is clamped against its own
 * table. Keep the Delay order in sync with effects.c. */
static const char *const g_fx_presets_off[] = {"Off"};
static const char *const g_fx_presets_delay[] = {
    "Digital", "Stereo", "Ping-Pong", "Mono", "Filtered", "Filtered Ping-Pong",
    "Tape", "Digital Sync", "Stereo Sync", "Ping-Pong Sync", "Mono Sync",
    "Filtered Sync", "Filtered Ping-Pong Sync", "Tape Sync"
};
static const char *const g_fx_presets_reverb[] = {
    "Default", "Long", "Hall", "Echoes", "Room", "Dark Room"
};
static const char *const g_fx_presets_chorus[] = {
    "Default", "Lush", "Dark", "Shaded", "Single"
};
static const char *const g_fx_presets_drive[] = {
    "Classic", "Soft Clip", "Germanium", "Dual Fold", "Climb", "Tape"
};
static const char *const g_fx_presets_crusher[] = {"Default"};
static const char *const g_fx_presets_phaser[] = {
    "Default", "Default Sync", "Space", "Space Sync", "SnH", "SnH Sync"
};
static const char *const g_fx_presets_eq[] = {"Default", "Wide", "Mid 1K"};
static const char *const g_fx_presets_flanger[] = {
    "Default", "Default Sync", "Silly", "Silly Sync"
};
static const char *const g_fx_presets_super_unison[] = {
    "Classic", "Ravey", "Soli", "Slow", "Slow Trig", "Wide Trig",
    "Mono Trig", "Wavy"
};
static const char *const g_fx_presets_peak_eq[] = {"Default"};
static const char *const g_fx_presets_multicomp[] = {
    "OPP", "Bass Ctrl", "High Ctrl", "All Up", "Tighter"
};
static const char *const *const g_fx_presets[FX_TYPE_COUNT] = {
    g_fx_presets_off, g_fx_presets_delay, g_fx_presets_reverb,
    g_fx_presets_chorus, g_fx_presets_drive, g_fx_presets_crusher,
    g_fx_presets_phaser, g_fx_presets_eq, g_fx_presets_flanger,
    g_fx_presets_super_unison, g_fx_presets_peak_eq, g_fx_presets_multicomp
};
static const int g_fx_preset_counts[FX_TYPE_COUNT] = {
    1, 14, 6, 5, 6, 1, 6, 3, 4, 8, 1, 5
};
/* Grid source rows, in the hardware order, with the key suffix each one
 * uses so the parameter and state keys can be generated from it. */
static const char *const g_mod_slot_suffix[MODSRC_COUNT] = {
    "cyc", "env", "lfo1", "lfo2", "vel", "whl", "kbd"
};
static const char *const g_lfo_shapes[] = {
    "Triangle", "Saw", "Square", "Shaper", "Sine", "S&H", "Slew S&H",
    "ExpSaw", "ExpRamp"
};
static const char *const g_lfo_sync_modes[] = {"Off", "On"};
static const char *const g_lfo_retrig_modes[] = {"Free", "Poly Kbd", "Mono Kbd", "Legato Kbd", "One"};
/* The MiniFreak Attack, Decay and Release curve menus, plus its envelope
 * Retrig switch (Gen_RetrigMode: 1 is "Env Continue" in the factory bank). */
static const char *const g_env_attack_curves[] = {"Default", "Quick"};
static const char *const g_env_curves[] = {"Default", "Percussive"};
static const char *const g_env_retrig_modes[] = {"Env Reset", "Env Continue"};
/* What the matrix's Velo + AT row reads (MiniFreak's Velo/AT setting). */
static const char *const g_velo_sources[] = {"Velo + AT", "Velo", "AT"};
/* What the matrix's Keyboard row reads (MiniFreak's Kbd Src; see
 * keyboard_source). */
enum { KBD_SOURCE_LINEAR, KBD_SOURCE_S_CURVE, KBD_SOURCE_RANDOM, KBD_SOURCE_VOICES, KBD_SOURCE_COUNT };
static const char *const g_kbd_sources[] = {"Linear", "S Curve", "Random", "Voices"};
static const char *const g_osc_phase_modes[] = {"Trig", "Free"};

/* Tempo-synced LFO divisions: the MiniFreak's 27, in its order, from eight
 * dotted bars down to a 1/32 triplet (a preset's division is index/26 and
 * reads as floor(value * 27)). `beats` is beats per cycle, so a synced rate is
 * tempo/60 / beats Hz. State saved before version 13 used a shorter list and
 * is remapped on load. */
typedef struct {
    const char *name;
    float beats;
} sf_lfo_div_t;
static const sf_lfo_div_t g_lfo_divs[] = {
    {"8 Bar D", 48.0f}, {"8 Bar", 32.0f}, {"4 Bar D", 24.0f},
    {"8 Bar T", 64.0f / 3.0f}, {"4 Bar", 16.0f}, {"2 Bar D", 12.0f},
    {"4 Bar T", 32.0f / 3.0f}, {"2 Bar", 8.0f}, {"1 Bar D", 6.0f},
    {"2 Bar T", 16.0f / 3.0f}, {"1 Bar", 4.0f}, {"1/2D", 3.0f},
    {"1 Bar T", 8.0f / 3.0f}, {"1/2", 2.0f}, {"1/4D", 1.5f},
    {"1/2T", 4.0f / 3.0f}, {"1/4", 1.0f}, {"1/8D", 0.75f},
    {"1/4T", 2.0f / 3.0f}, {"1/8", 0.5f}, {"1/16D", 0.375f},
    {"1/8T", 1.0f / 3.0f}, {"1/16", 0.25f}, {"1/32D", 0.1875f},
    {"1/16T", 1.0f / 6.0f}, {"1/32", 0.125f}, {"1/32T", 1.0f / 12.0f}
};
/* Where each pre-v13 division index lands in the list above. */
static const int g_lfo_div_v12[20] = {
    4, 7, 10, 13, 16, 18, 19, 21, 22, 24, 25, 8, 11, 14, 17, 20, 23, 26, 1, 0
};
#define SF_LFO_DIV_COUNT ((int)(sizeof(g_lfo_divs) / sizeof(g_lfo_divs[0])))
#define SF_LFO_DIV_DEFAULT 16  /* 1/4 */
/* The MiniFreak's LFO Rate knob tops out at 100 Hz. */
#define SF_LFO_MAX_HZ 100.0f
/* Longest envelope stage: the MiniFreak's slowest decay and release take
 * about 46 s to fall 60 dB, and its slowest attack about 20 s. */
#define SF_ENV_MAX_MS 50000.0f

/* The MiniFreak's LFO Rate knob, measured: Hz at each knob position. A Rate
 * route moves the knob, so it is applied in knob space like the envelope
 * times (see sf_env_time_mod). */
#define SF_LFO_RATE_POINTS 14
static const float g_lfo_rate_x[SF_LFO_RATE_POINTS] = {
    0.0f, 0.025f, 0.05f, 0.1f, 0.15f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
    0.9f, 1.0f
};
static const float g_lfo_rate_hz[SF_LFO_RATE_POINTS] = {
    0.06f, 0.1f, 0.147f, 0.264f, 0.422f, 0.636f, 1.314f, 2.548f, 4.796f,
    8.889f, 16.39f, 29.85f, 55.56f, 100.0f
};

static float lfo_rate_mod(float hz, float amount) {
    if (amount == 0.0f) return hz;
    float x = 0.0f;
    if (hz >= g_lfo_rate_hz[SF_LFO_RATE_POINTS - 1]) x = 1.0f;
    else if (hz > g_lfo_rate_hz[0]) {
        int i = 0;
        while (hz > g_lfo_rate_hz[i + 1]) ++i;
        x = g_lfo_rate_x[i] + (g_lfo_rate_x[i + 1] - g_lfo_rate_x[i])
          * logf(hz / g_lfo_rate_hz[i]) / logf(g_lfo_rate_hz[i + 1] / g_lfo_rate_hz[i]);
    }
    float moved = x + amount;
    if (moved < 0.0f) moved = 0.0f;
    if (moved > 1.0f) moved = 1.0f;
    int j = 0;
    while (j < SF_LFO_RATE_POINTS - 2 && moved > g_lfo_rate_x[j + 1]) ++j;
    float f = (moved - g_lfo_rate_x[j]) / (g_lfo_rate_x[j + 1] - g_lfo_rate_x[j]);
    float target = g_lfo_rate_hz[j] * powf(g_lfo_rate_hz[j + 1] / g_lfo_rate_hz[j], f);
    /* Scale rather than replace, so a rate off the measured curve (or a synced
     * one) keeps its base and only takes the knob's local ratio. */
    float base = x <= 0.0f ? g_lfo_rate_hz[0] : (x >= 1.0f ? g_lfo_rate_hz[SF_LFO_RATE_POINTS - 1] : hz);
    return hz * target / base;
}

/* The matrix's Velo + AT row. MiniFreak V offers velocity, aftertouch or
 * both; with only aftertouch selected a played note leaves the row at 0. */
static float velo_at_source(const sf_instance_t *inst, float velocity, float pressure) {
    if (inst->velo_source == 1) return velocity;
    if (inst->velo_source == 2) return pressure;
    return velocity + pressure;
}

/* The Keyboard source row (MiniFreak's Kbd Src), measured from MiniFreak V.
 * Linear is 0 at middle C and 1/64 per semitone, so the top of the matrix
 * range is five octaves away. S Curve runs 1/30 per semitone to +/-0.8 two
 * octaves out, then on to -1 at note 0 and +1 at note 127. Random is a fresh
 * -1..1 draw on every note. Voices gives the six voices 0, +1, -1, 0, +1/2,
 * -1/2: Poly deals them round-robin note by note, a six-voice Unison stack
 * plays all six and Mono always sits on the first (0). Every mode moves a
 * time destination's knob by amount x value, both ways, with no offset. */
static float keyboard_source(const sf_instance_t *inst, const sf_voice_t *voice) {
    static const float voice_values[6] = {0.0f, 1.0f, -1.0f, 0.0f, 0.5f, -0.5f};
    float semis = (float)voice->note - 60.0f;
    float value;
    if (inst->kbd_source == KBD_SOURCE_S_CURVE) {
        if (semis > 24.0f) value = 0.8f + 0.2f * (semis - 24.0f) / 43.0f;
        else if (semis < -24.0f) value = -0.8f + 0.2f * (semis + 24.0f) / 36.0f;
        else value = semis / 30.0f;
    } else if (inst->kbd_source == KBD_SOURCE_RANDOM) {
        return voice->random_bipolar;
    } else if (inst->kbd_source == KBD_SOURCE_VOICES) {
        unsigned slot = inst->voice_mode == VOICE_POLY ? voice->age + 5u
                      : (unsigned)(voice - inst->voices);
        return voice_values[slot % 6u];
    } else {
        value = semis / 64.0f;
    }
    return value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
}

/* Free, Mono Kbd and Legato Kbd LFOs share one phase across the voices. */
static int lfo_shared(int retrig) {
    return retrig == LFO_RETRIG_FREE || retrig == LFO_RETRIG_MONO
        || retrig == LFO_RETRIG_LEGATO;
}

/* The LFO value for a voice. In One mode the MiniFreak plays Saw and Square
 * unipolar (1 -> 0, and 1 then 0), and a finished LFO holds the value its
 * cycle ended on: 0 for most waves, 1 for ExpRamp. */
static float lfo_output(int shape, int retrig, float phase, int done,
                        const sf_shaper_t *shaper, const sf_lfo_snh_t *snh) {
    if (retrig == LFO_RETRIG_ONE) {
        if (done) phase = 0.99999f;
        if (shape == LFO_SAW) return 1.0f - phase;
        if (shape == LFO_SQUARE) return phase < 0.5f ? 1.0f : 0.0f;
    }
    return sf_mod_lfo_sample(shape, phase, shaper, snh);
}

/* Destination columns, in enum order. Kept short: these are what an
 * enum knob shows, and the Move gives a label band of about five
 * characters. The option lists in the parameter metadata repeat this
 * list and the tests check the two against each other. */
static const char *const g_mod_destinations[DEST_COUNT] = {
    "Off","Pitch","Cut","Shp1","Shp2","Vol2","Res","Out","Tim1","Tim2","Wav1","Wav2","Ptc1","Ptc2","Vol1","VCA","EnvA","Gld","Atk","Dec","Sus","Rel","L1Rt","L2Rt","L1Am","L2Am","CyAm","F1T","F1I","F1A","F2T","F2I","F2A","F3T","F3I","F3A","Mac1","Mac2","Mac3","Mac4","Rise","Fall","Hold",
    "M1:1","M1:2","M1:3","M1:4","M1:5","M1:6","M1:7","M1:8","M1:9","M1:10","M1:11","M1:12",
    "M2:1","M2:2","M2:3","M2:4","M2:5","M2:6","M2:7","M2:8","M2:9","M2:10","M2:11","M2:12",
    "M3:1","M3:2","M3:3","M3:4","M3:5","M3:6","M3:7","M3:8","M3:9","M3:10","M3:11","M3:12",
    "M4:1","M4:2","M4:3","M4:4","M4:5","M4:6","M4:7","M4:8","M4:9","M4:10","M4:11","M4:12",
    "M5:1","M5:2","M5:3","M5:4","M5:5","M5:6","M5:7","M5:8","M5:9","M5:10","M5:11","M5:12",
    "M6:1","M6:2","M6:3","M6:4","M6:5","M6:6","M6:7","M6:8","M6:9","M6:10","M6:11","M6:12",
    "M7:1","M7:2","M7:3","M7:4","M7:5","M7:6","M7:7","M7:8","M7:9","M7:10","M7:11","M7:12",
    "FFM"
};


typedef struct {
    const char *wave_short;
    const char *wave_label;
    const char *wave_tip;
    const char *timbre_short;
    const char *timbre_label;
    const char *timbre_tip;
    const char *shape_short;
    const char *shape_label;
    const char *shape_tip;
    const char *level_short;
    const char *level_label;
    const char *level_tip;
} sf_osc_role_meta_t;

/* --------- shared role groups for engines that share one control set ------- */

static const sf_osc_role_meta_t g_osc_role_sample = {
    /* Sample / Frozen. Stubs: these need a sample library and a grain
     * scheduler, so the wavetable oscillator stands in with the same three
     * controls (see granular_placeholder_sample in engines.c). */
    "SMPL", "Wave: Sample Pos (stub)", "Stub engine: wavetable read position",
    "STRT", "Timbre: Sample Start (stub)", "Stub engine: start offset into the sample",
    "LOOP", "Shape: Loop Mix (stub)", "Stub engine: loop blend and window",
    "VOL", "Volume", "Oscillator output level"
};

static const sf_osc_role_meta_t g_osc_role_grain = {
    /* Particles / Cloud Grains / Hit Grains / Skan / Lick / Raster. Stubs. */
    "GRIN", "Wave: Grain Pos (stub)", "Stub engine: grain read position",
    "DENS", "Timbre: Density (stub)", "Stub engine: grain density and retrigger",
    "SPRD", "Shape: Spread (stub)", "Stub engine: grain spread and window",
    "VOL", "Volume", "Oscillator output level"
};

static const sf_osc_role_meta_t g_osc_role_modal = {
    /* MiniFreak's Modal order: Wave = Inharmonic, Timbre = Timbre,
     * Shape = Decay. Plaits calls the last one damping. */
    "INHR", "Wave: Inharmonic", "Inharmonicity: tube at 0, metallic bell at 1",
    "TMBR", "Timbre: Timbre", "Exciter brightness and upper-mode damping",
    "DEC", "Shape: Decay", "Resonator damping: short hit at 0, long ring at 1",
    "VOL", "Volume", "Oscillator output level"
};

static const sf_osc_role_meta_t g_osc_role_speech = {
    /* Speech: Plaits NaiveSpeechSynth formants. Type 0.4+ plays a spoken
     * word once per note and Timbre stretches it; below 0.4 it is a sustained
     * voice, where Timbre is the vocal register and Word picks the phoneme. */
    "TYPE", "Wave: Type", "Spoken word (0.4+) or sustained voice",
    "TMBR", "Timbre: Timbre", "Word length; vocal register in sustained voices",
    "WORD", "Shape: Word", "Phoneme in sustained voices; none on the word",
    "VOL", "Volume", "Oscillator output level"
};

static const sf_osc_role_meta_t g_osc_role_formant = {
    /* Formant (Osc 2): Plaits GrainEngine formant oscillator. */
    "INTRV", "Wave: Interval", "Ratio between the two formant frequencies",
    "FMNT", "Timbre: Formant", "Base formant frequency",
    "SHAPE", "Shape: Shape", "Grainlet width and formant shape",
    "VOL", "Volume", "Oscillator output level"
};

static const sf_osc_role_meta_t g_osc_role_audioin = {
    /* Audio In. Stub: module.json declares no `audio_in` capability and the
     * host hands the plugin no capture buffer, so noise stands in. */
    "GAIN", "Wave: Input Gain (stub)", "Stub engine: no audio input; noise stand-in",
    "TONE", "Timbre: Tone (stub)", "Stub engine: tone shaping of the input",
    "RESO", "Shape: Resonance (stub)", "Stub engine: resonator amount on the input",
    "VOL", "Volume", "Oscillator output level"
};

/* --------- Oscillator 1 roles, indexed by OSC_* mode --------------------- */

static const sf_osc_role_meta_t *const g_osc1_roles[SF_OSC1_MODE_COUNT] = {
    /* 0: BasicWaves */
    &(const sf_osc_role_meta_t){"MORPH", "Wave 1: Morph", "Morphs pulse -> saw -> double-saw",
     "SYM",   "Timbre 1: Symmetry", "Pulse width and double-saw phase offset",
     "SUB",   "Shape 1: Sub Osc", "Blends -1 octave sine sub-oscillator",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 1: Two Op FM */
    &(const sf_osc_role_meta_t){"RATIO", "Wave 1: FM Ratio", "Modulator:carrier ratio (0.5x to 7.0x)",
     "INDEX", "Timbre 1: FM Index", "FM modulation depth (warm bell to metallic)",
     "FDBK",  "Shape 1: Feedback", "Modulator self-feedback grit",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 2: Wavefolder */
    &(const sf_osc_role_meta_t){"WAVE",  "Wave 1: Source", "Input wave morph before wavefolder",
     "FOLD",  "Timbre 1: Fold Amt", "West-Coast sinusoidal + reflection folding",
     "BIAS",  "Shape 1: Asymmetry", "Pre-folder DC symmetry bias (even harmonics)",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 3: Noise */
    &(const sf_osc_role_meta_t){"TYPE",  "Wave 1: Type", "Particle noise -> white noise -> metallic noise",
      "RATE",  "Timbre 1: Rate", "Reduces the noise sample rate; sets metallic-noise pitch",
      "BAL",   "Shape 1: Balance", "Adds Sine -> Triangle -> Square to the noise",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 4: Wavetable */
    &(const sf_osc_role_meta_t){"POS",   "Wave 1: Position", "Selects the starting frame in the chosen wavetable",
      "WIDTH", "Timbre 1: Width", "Compresses the waveform toward a pulse shape",
      "TONE",  "Shape 1: Tone", "Centered one-pole low-pass / high-pass tone control",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 5: SuperWave */
    &(const sf_osc_role_meta_t){"WAVE",  "Wave 1: Base Wave", "Saw -> Square -> Triangle -> Sine",
     "DETUN", "Timbre 1: Detune", "Incommensurate 5-voice unison detune spread",
     "MIX",   "Shape 1: Stack Mix", "Level of detuned unison copies vs center",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 6: Harmo */
    &(const sf_osc_role_meta_t){"CONT",  "Wave 1: Content", "Selects one of 8 harmonic relationship tables",
     "SCULP", "Timbre 1: Sculpting", "Morphs the harmonics from sine toward a brighter tilt",
     "CHOR",  "Shape 1: Chorus", "Detuned copy that widens and thickens the sound",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 7: Formant */
    &(const sf_osc_role_meta_t){"INTRV", "Wave 1: Interval", "Ratio between the two formant frequencies",
     "FMNT",  "Timbre 1: Formant", "Base formant frequency",
     "SHAPE", "Shape 1: Shape", "Grainlet width and formant shape",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 8: V.Analog */
     &(const sf_osc_role_meta_t){"DETUN", "Wave 1: Detune", "Saw pitch relative to pulse, +/- 2 octaves (0.5 = unison)",
      "PULSE", "Timbre 1: Pulse Shape", "Narrow pulse -> square -> hard-sync harmonics",
      "SAW",   "Shape 1: Saw Shape", "Triangle -> saw with increasing harmonics",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 9: Waveshaper */
    &(const sf_osc_role_meta_t){"WSHP",  "Wave 1: Waveshaper", "Waveshaper curve applied to the slope wave",
     "FOLD",  "Timbre 1: Amount", "Wavefolder gain",
     "ASYM",  "Shape 1: Asym", "Slope/pulse-width asymmetry",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 10: Chords */
    &(const sf_osc_role_meta_t){"INTRV", "Wave 1: Interval", "11 chord types: Oct, 5th, sus4, m, m7, m9, m11, 69, M9, M7, M",
     "INV",   "Timbre 1: Inv/Trsp", "Walks the chord through its inversions",
     "WAVE",  "Shape 1: Waveform", "Drawbar organ/string registration -> wavetable",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 11: KarplusStr */
     &(const sf_osc_role_meta_t){"BOW",   "Wave 1: Bow", "Blends the struck sound toward sustained bow excitation",
      "POS",   "Timbre 1: Position", "Strike position and excitation brightness",
      "DECAY", "Shape 1: Decay", "How long the string resonance rings (high = long)",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 12: SawX (Noise Engineering) */
    &(const sf_osc_role_meta_t){"SAWM",  "Wave 1: Saw Mod", "Gain into the saw modulus stage",
     "NOISE", "Timbre 1: Noise", "Subsampled-noise phase modulation amount",
     "CHOR",  "Shape 1: Chorus", "Chorus/stack level of the super-saw core",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 13: Harm (Noise Engineering) */
    &(const sf_osc_role_meta_t){"SPRD",  "Wave 1: Spread", "Partial spacing: unison -> harmonic series -> octaves",
     "RECT",  "Timbre 1: Rectification", "Per-partial half-fold rectification",
     "CLIP",  "Shape 1: Noise/Clip", "Phase-modulated noise and master clip",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 14: Bass (Noise Engineering) */
    &(const sf_osc_role_meta_t){"SAT",   "Wave 1: Saturate", "Saturation of the quadrature oscillator",
     "FOLD",  "Timbre 1: Fold", "Two-stage asymmetric wavefolder amount",
     "NOISE", "Shape 1: Noise", "Noise phase-modulation between the fold stages",
     "VOL 1", "Volume 1", "Oscillator 1 level into mixer"},
    /* 15: Modal */
    &g_osc_role_modal,
    /* 16: Speech */
    &g_osc_role_speech,
    /* 17: Sample */
    &g_osc_role_sample,
    /* 18: Particles */
    &g_osc_role_grain,
    /* 19: Cloud Grains */
    &g_osc_role_grain,
    /* 20: Hit Grains */
    &g_osc_role_grain,
    /* 21: Frozen */
    &g_osc_role_sample,
    /* 22: Skan */
    &g_osc_role_grain,
    /* 23: Lick */
    &g_osc_role_grain,
    /* 24: Raster */
    &g_osc_role_grain,
    /* 25: Audio In */
    &g_osc_role_audioin
};

/* --------- Oscillator 2 roles, indexed by ENGINE_* mode ------------------- */

static const sf_osc_role_meta_t *const g_osc2_roles[SF_OSC2_MODE_COUNT] = {
    /* 0: BasicWaves */
    &(const sf_osc_role_meta_t){"MORPH", "Wave 2: Morph", "Morphs pulse -> saw -> double-saw",
     "SYM",   "Timbre 2: Symmetry", "Pulse width and double-saw phase offset",
     "SUB",   "Shape 2: Sub Osc", "Blends -1 octave sine sub-oscillator",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 1: Ring */
    &(const sf_osc_role_meta_t){"WAVE",  "Wave 2: Mod Wave", "Ring modulator waveform morph",
     "DEPTH", "Timbre 2: Ring Depth", "Ring modulation depth with Osc 1",
     "SYM",   "Shape 2: Mod Sym", "Ring modulator waveform symmetry",
     "RING",  "Volume 2: Ring Lvl", "Ring-modulated signal level"},
    /* 2: Wavefolder */
    &(const sf_osc_role_meta_t){"DRIVE", "Wave 2: Pre Drive", "Pre-gain into wavefolder on Osc 1",
     "FOLD",  "Timbre 2: Fold Amt", "West-Coast sinusoidal + reflection folding",
     "BIAS",  "Shape 2: Asymmetry", "Pre-folder DC symmetry bias (even harmonics)",
     "MIX",   "Volume 2: Dry/Wet", "Dry/Wet balance for folded Osc 1 signal"},
    /* 3: Noise */
    &(const sf_osc_role_meta_t){"TYPE",  "Wave 2: Type", "Particle noise -> white noise -> metallic noise",
      "RATE",  "Timbre 2: Rate", "Reduces the noise sample rate; sets metallic-noise pitch",
      "BAL",   "Shape 2: Balance", "Adds Sine -> Triangle -> Square to the noise",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 4: Wavetable */
    &(const sf_osc_role_meta_t){"POS",   "Wave 2: Position", "Selects the starting frame in the chosen wavetable",
     "WIDTH", "Timbre 2: Width", "Compresses the waveform toward a pulse shape",
     "TONE",  "Shape 2: Tone", "Centered one-pole low-pass / high-pass tone control",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 5: SuperWave */
    &(const sf_osc_role_meta_t){"WAVE",  "Wave 2: Base Wave", "Saw -> Square -> Triangle -> Sine",
     "DETUN", "Timbre 2: Detune", "Incommensurate 5-voice unison detune spread",
     "MIX",   "Shape 2: Stack Mix", "Level of detuned unison copies vs center",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 6: Harmo */
    &(const sf_osc_role_meta_t){"CONT",  "Wave 2: Content", "Selects one of 8 harmonic relationship tables",
     "SCULP", "Timbre 2: Sculpting", "Morphs the harmonics from sine toward a brighter tilt",
     "CHOR",  "Shape 2: Chorus", "Detuned copy that widens and thickens the sound",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 7: Chord */
    &(const sf_osc_role_meta_t){"INTRV", "Wave 2: Interval", "11 chord types: Oct, 5th, sus4, m, m7, m9, m11, 69, M9, M7, M",
     "INV",   "Timbre 2: Inv/Trsp", "Walks the chord through its inversions",
     "WAVE",  "Shape 2: Waveform", "Drawbar organ/string registration -> wavetable",
     "VOL 2", "Volume 2", "Chord oscillator level"},
    /* 8: MultiFilter */
    &(const sf_osc_role_meta_t){"CUT",   "Wave 2: Cutoff", "Processor cutoff frequency (20 Hz to 18 kHz)",
      "RESO",  "Timbre 2: Resonance", "Resonance for the selected filter mode",
      "TYPE",  "Shape 2: Filter Type", "LP/HP 6, 12, 24, 36 dB; BP and Notch 12, 24, 36 dB",
      "MIX",   "Volume 2: Dry/Wet", "Dry/Wet balance for filtered Osc 1 signal"},
    /* 9: Crusher */
    &(const sf_osc_role_meta_t){"BITS",  "Wave 2: Bit Depth", "Reduces bit resolution from 16-bit down to 4-bit",
     "DRIVE", "Timbre 2: Pre Drive", "Pre-drive saturation into bit quantizer",
     "BIAS",  "Shape 2: Quant Bias", "DC offset bias across quantizer steps",
     "MIX",   "Volume 2: Dry/Wet", "Dry/Wet balance for crushed Osc 1 signal"},
    /* 10: Comb Filter */
    &(const sf_osc_role_meta_t){"FREQ",  "Wave 2: Cutoff", "Delay time: longer delays emphasize lower frequencies",
      "GAIN",  "Timbre 2: Gain", "Amount of the delayed signal mixed with the original",
      "DAMP",  "Shape 2: Damping", "Softens the delayed copy so the comb notches are less dramatic",
      "MIX",   "Volume 2: Dry/Wet", "Dry/Wet balance for the combed signal"},
    /* 11: Surgeon Filter */
    &(const sf_osc_role_meta_t){"CUT",   "Wave 2: Cutoff", "Filter center/cutoff frequency (20 Hz to 18 kHz)",
      "SPRD",  "Timbre 2: Spread", "Broad filter response to a very narrow surgical band",
      "MODE",  "Shape 2: Mode", "Low-Pass, Band-Pass, High-Pass, or Notch",
      "MIX",   "Volume 2: Dry/Wet", "Dry/Wet balance for the selected Surgeon filter mode"},
    /* 12: Phaser Filter */
    &(const sf_osc_role_meta_t){"CUT",   "Wave 2: Cutoff", "Sets the frequency range of the phase notches",
      "FBK",   "Timbre 2: Feedback", "All-pass chain feedback into sharper notches",
      "POLES", "Shape 2: Poles", "Selects 2, 4, 6, 8, 10, or 12 filter poles",
     "MIX",   "Volume 2: Dry/Wet", "Dry/Wet balance for the phase-filtered signal"},
    /* 13: Destroy */
    &(const sf_osc_role_meta_t){"FOLD",  "Wave 2: Fold", "Wavefolder amount before the decimator",
     "DECIM", "Timbre 2: Decimate", "Sample-rate reduction (aliasing) amount",
     "BITS",  "Shape 2: Bit Depth", "Bit resolution of the decimated samples",
     "MIX",   "Volume 2: Dry/Wet", "Dry/Wet balance for the destroyed signal"},
    /* 14: V.Analog */
     &(const sf_osc_role_meta_t){"DETUN", "Wave 2: Detune", "Saw pitch relative to pulse, +/- 2 octaves (0.5 = unison)",
      "PULSE", "Timbre 2: Pulse Shape", "Narrow pulse -> square -> hard-sync harmonics",
      "SAW",   "Shape 2: Saw Shape", "Triangle -> saw with increasing harmonics",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 15: Waveshaper */
    &(const sf_osc_role_meta_t){"WSHP",  "Wave 2: Waveshaper", "Waveshaper curve applied to the slope wave",
     "FOLD",  "Timbre 2: Amount", "Wavefolder gain",
     "ASYM",  "Shape 2: Asym", "Slope/pulse-width asymmetry",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 16: KarplusStr */
     &(const sf_osc_role_meta_t){"BOW",   "Wave 2: Bow", "Blends the struck sound toward sustained bow excitation",
      "POS",   "Timbre 2: Position", "Strike position and excitation brightness",
      "DECAY", "Shape 2: Decay", "How long the string resonance rings (high = long)",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 17: SawX */
    &(const sf_osc_role_meta_t){"SAWM",  "Wave 2: Saw Mod", "Gain into the saw modulus stage",
     "NOISE", "Timbre 2: Noise", "Subsampled-noise phase modulation amount",
     "CHOR",  "Shape 2: Chorus", "Chorus/stack level of the super-saw core",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 18: Harm */
    &(const sf_osc_role_meta_t){"SPRD",  "Wave 2: Spread", "Partial spacing: unison -> harmonic series -> octaves",
     "RECT",  "Timbre 2: Rectification", "Per-partial half-fold rectification",
     "CLIP",  "Shape 2: Noise/Clip", "Phase-modulated noise and master clip",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 19: Bass */
    &(const sf_osc_role_meta_t){"SAT",   "Wave 2: Saturate", "Saturation of the quadrature oscillator",
     "FOLD",  "Timbre 2: Fold", "Two-stage asymmetric wavefolder amount",
     "NOISE", "Shape 2: Noise", "Noise phase-modulation between the fold stages",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 20: Modal */
    &g_osc_role_modal,
    /* 21: Speech */
    &g_osc_role_speech,
    /* 22: Sample */
    &g_osc_role_sample,
    /* 23: Particles */
    &g_osc_role_grain,
    /* 24: Cloud Grains */
    &g_osc_role_grain,
    /* 25: Hit Grains */
    &g_osc_role_grain,
    /* 26: Frozen */
    &g_osc_role_sample,
    /* 27: Skan */
    &g_osc_role_grain,
    /* 28: Lick */
    &g_osc_role_grain,
    /* 29: Raster */
    &g_osc_role_grain,
    /* 30: Audio In */
    &g_osc_role_audioin,
    /* 31: Formant (appended for the MiniFreak bank) */
    &g_osc_role_formant,
    /* 32: Two Op FM (the MiniFreak generator, Osc 1's engine) */
    &(const sf_osc_role_meta_t){"RATIO", "Wave 2: FM Ratio", "Modulator:carrier ratio (0.5x to 8.0x)",
     "INDEX", "Timbre 2: FM Index", "FM modulation depth (warm bell to metallic)",
     "FDBK",  "Shape 2: Feedback", "Modulator self-feedback grit",
     "VOL 2", "Volume 2", "Oscillator 2 level"},
    /* 33: FM / RM */
    &(const sf_osc_role_meta_t){"WAVE",  "Wave 2: Waveform", "Osc 2 sine -> triangle -> saw -> square",
     "FM",    "Timbre 2: FM Amt", "Osc 1 phase-modulates Osc 2",
     "RM",    "Shape 2: RM Amt", "Ring-modulates Osc 2 by Osc 1",
     "VOL 2", "Volume 2", "FM / RM output level (Osc 1 stays dry)"}
};

/* Advance an xorshift state and return a bipolar -1..1 value, for the Sample
 * & Hold LFO shapes. Matches the voice's own generator so the two stay in
 * step. */
static float snh_bipolar_random(uint32_t *state) {
    return (float)(sf_xorshift32(state) >> 8) * (2.0f / 16777215.0f) - 1.0f;
}

static float cutoff_hz_from_param(float value) {
    float t = sf_clampf(value, 0.0f, SF_CUTOFF_PARAM_MAX) / SF_CUTOFF_PARAM_MAX;
    return SF_CUTOFF_MIN_HZ * powf(SF_CUTOFF_MAX_HZ / SF_CUTOFF_MIN_HZ, t);
}

static float cutoff_param_from_hz(float hz) {
    float h = sf_clampf(hz, SF_CUTOFF_MIN_HZ, SF_CUTOFF_MAX_HZ);
    return SF_CUTOFF_PARAM_MAX * logf(h / SF_CUTOFF_MIN_HZ)
         / logf(SF_CUTOFF_MAX_HZ / SF_CUTOFF_MIN_HZ);
}

/*
 * The master soft clipper.
 *
 * A cubic: unity-ish below about 0.5, a smooth ceiling at 1.0, and C1 at 1.5 so
 * there is no slope discontinuity to hear. Replacing the old hard clamp is what
 * lets the voice sum be a plain sum — six voices at full tilt compress into the
 * ceiling instead of clipping flat, and one voice is never scaled down to make
 * room for notes that may or may not arrive.
 */
/* A modulation amount scales a time or a rate: 0 leaves it alone, +1
 * multiplies it by four and -1 divides it by four. `direction` is 1 where a
 * longer value is the positive direction - envelope stages and glide - and -1
 * where the destination is a rate, so that a positive dot also speeds an LFO. */
static float sf_mod_time_factor(float amount, float direction) {
    return powf(2.0f, sf_clampf(amount, -1.0f, 1.0f) * direction * 2.0f);
}

/* Routes into the Cycling Envelope's Rise, Fall and Hold move their knobs,
 * as every time route does on MiniFreak V (probed: -0.5 on a 0.4 Rise empties
 * it). Env mode reads Rise/Fall on the ADSR's attack and decay curves and Hold
 * as a sustain level; Run and Loop read all three as 10 s * x^4. */
static float cycle_knob_time(float ms, float amount, float floor_ms) {
    float x = sqrtf(sqrtf(sf_clampf(ms / 10000.0f, 0.0f, 1.0f)));
    x = sf_clampf(x + amount, 0.0f, 1.0f);
    float moved = 10000.0f * x * x * x * x;
    return moved > floor_ms ? moved : floor_ms;
}

static void cycle_times_mod(int mode, float rise_mod, float fall_mod, float hold_mod,
                            float *rise_ms, float *fall_ms, float *hold) {
    if (mode == CYCLE_ENV) {
        if (rise_mod != 0.0f) *rise_ms = sf_env_time_mod(ENV_ATTACK, *rise_ms, rise_mod);
        if (fall_mod != 0.0f) *fall_ms = sf_env_time_mod(ENV_DECAY, *fall_ms, fall_mod);
        *hold = sf_clampf(*hold + hold_mod, 0.0f, 1.0f);
        return;
    }
    if (rise_mod != 0.0f) *rise_ms = cycle_knob_time(*rise_ms, rise_mod, 1.0f);
    if (fall_mod != 0.0f) *fall_ms = cycle_knob_time(*fall_ms, fall_mod, 1.0f);
    if (hold_mod != 0.0f) *hold = cycle_knob_time(*hold, hold_mod, 0.0f);
}

static float soft_clip(float x) {
    if (x >= 1.5f) return 1.0f;
    if (x <= -1.5f) return -1.0f;
    return x - x * x * x * (4.0f / 27.0f);
}

static unsigned clamp_enum(float value, unsigned count) {
    int n = (int)value;
    if (n < 0) return 0;
    if ((unsigned)n >= count) return count - 1;
    return (unsigned)n;
}

static int fx_preset_count(int type) {
    if (type < 0 || type >= FX_TYPE_COUNT) return 1;
    return g_fx_preset_counts[type];
}

static int fx_subtype_index(int type, int subtype) {
    int count = fx_preset_count(type);
    if (subtype < 0) return 0;
    if (subtype >= count) return count - 1;
    return subtype;
}

static const char *fx_preset_name(int type, int subtype) {
    if (type < 0 || type >= FX_TYPE_COUNT) type = 0;
    return g_fx_presets[type][fx_subtype_index(type, subtype)];
}

static int parse_fx_subtype(const char *value, int type) {
    int count = fx_preset_count(type);
    if (!value) return 0;
    for (int i = 0; i < count; ++i) {
        if (strcmp(value, g_fx_presets[type][i]) == 0) return i;
    }
    char *end = NULL;
    long numeric = strtol(value, &end, 10);
    if (end != value && *end == '\0') {
        if (numeric < 0) return 0;
        if (numeric >= count) return count - 1;
        return (int)numeric;
    }
    return 0;
}

static void format_fx_preset_options(int type, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return;
    int count = fx_preset_count(type);
    size_t used = 0;
    used += (size_t)snprintf(buf + used, buf_len - used, "[");
    for (int i = 0; i < count && used < buf_len; ++i) {
        used += (size_t)snprintf(buf + used, buf_len - used, "%s\"%s\"",
                                 i ? "," : "", g_fx_presets[type][i]);
    }
    if (used < buf_len) snprintf(buf + used, buf_len - used, "]");
}

static int format_chain_params(const sf_instance_t *inst, char *buf, int buf_len) {
    int m1 = (int)clamp_enum((float)(inst ? inst->osc1_mode : 0), SF_OSC1_MODE_COUNT);
    int m2 = (int)clamp_enum((float)(inst ? inst->osc2_mode : 0), SF_OSC2_MODE_COUNT);
    const sf_osc_role_meta_t *r1 = g_osc1_roles[m1];
    const sf_osc_role_meta_t *r2 = g_osc2_roles[m2];
    /* Formatted first so every argument below is a string: the preset count is
     * the only number in this document. */
    char preset_max[16];
    snprintf(preset_max, sizeof(preset_max), "%d", SF_FACTORY_PRESET_COUNT - 1);
    char fx1_presets[512];
    char fx2_presets[512];
    char fx3_presets[512];
    format_fx_preset_options((int)clamp_enum((float)(inst ? inst->fx_type[0] : 0),
                                             FX_TYPE_COUNT),
                             fx1_presets, sizeof(fx1_presets));
    format_fx_preset_options((int)clamp_enum((float)(inst ? inst->fx_type[1] : 0),
                                             FX_TYPE_COUNT),
                             fx2_presets, sizeof(fx2_presets));
    format_fx_preset_options((int)clamp_enum((float)(inst ? inst->fx_type[2] : 0),
                                             FX_TYPE_COUNT),
                             fx3_presets, sizeof(fx3_presets));
    int n = snprintf(buf, (size_t)buf_len,
        "["
        "{\"key\":\"osc1_tune\",\"name\":\"Tune 1\",\"short_name\":\"TUNE\",\"description\":\"Oscillator tuning, -48 to +48 semitones plus fine range\",\"type\":\"float\",\"min\":-48.5,\"max\":48.5,\"step\":0.01,\"unit\":\"st\"},"
        "{\"key\":\"osc1_quantize\",\"name\":\"Mod Quantize 1\",\"short_name\":\"QUANT\",\"description\":\"Quantizes this oscillator's pitch modulation to a scale\",\"type\":\"enum\",\"options\":[\"Continuous\",\"Chromatic\",\"Octaves\",\"Fifths\",\"Minor\",\"Major\",\"Phrygian Dominant\",\"Minor 9th\",\"Major 9th\",\"Minor Pentatonic\",\"Major Pentatonic\"]},"
        "{\"key\":\"osc1_mode\",\"name\":\"Type 1\",\"short_name\":\"TYPE\",\"description\":\"Oscillator 1 synthesis engine family\",\"type\":\"enum\",\"options\":[\"BasicWaves\",\"Two Op FM\",\"Wavefolder\",\"Noise\",\"Wavetable\",\"SuperWave\",\"Harmo\",\"Formant\",\"V.Analog\",\"Waveshaper\",\"Chords\",\"KarplusStr\",\"SawX\",\"Harm\",\"Bass\",\"Modal\",\"Speech\",\"Sample\",\"Particles\",\"Cloud Grains\",\"Hit Grains\",\"Frozen\",\"Skan\",\"Lick\",\"Raster\",\"Audio In\"]},"
        "{\"key\":\"osc1_table\",\"name\":\"Wavetable Preset\",\"type\":\"enum\",\"options\":[\"Harmonic\",\"Microwave 2\",\"Bell 03\",\"VoxSynth\",\"Spectral\",\"SuperSaw\",\"Fourier 2\",\"Noise Wave\",\"Folding\",\"Drone\",\"Sitar\",\"Tidal\",\"Harmonix\",\"Morphing\",\"Wavetrip\",\"Sine to Saw\",\"Saw Bend\",\"Basic Morph\",\"Tidy 006\",\"AM Sine\",\"Hmmm\",\"Iterative\",\"Osmaos\",\"Twist 02\",\"Sine Mutate\",\"Reso 00\",\"Sohler 34\",\"Sohler 35\",\"PD 101\",\"Sohler 43\",\"Mellow D\",\"Tidy 007\"]},"
        "{\"key\":\"osc1_wave\",\"name\":\"Wave 1\",\"label\":\"%s\",\"short_name\":\"%s\",\"description\":\"%s\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"osc1_timbre\",\"name\":\"Timbre 1\",\"label\":\"%s\",\"short_name\":\"%s\",\"description\":\"%s\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"osc1_shape\",\"name\":\"Shape 1\",\"label\":\"%s\",\"short_name\":\"%s\",\"description\":\"%s\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"osc1_level\",\"name\":\"Volume 1\",\"label\":\"%s\",\"short_name\":\"%s\",\"description\":\"%s\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"osc2_tune\",\"name\":\"Tune 2\",\"short_name\":\"TUNE\",\"description\":\"Oscillator tuning, -48 to +48 semitones plus fine range\",\"type\":\"float\",\"min\":-48.5,\"max\":48.5,\"step\":0.01,\"unit\":\"st\"},"
        "{\"key\":\"osc2_quantize\",\"name\":\"Mod Quantize 2\",\"short_name\":\"QUANT\",\"description\":\"Quantizes this oscillator's pitch modulation to a scale\",\"type\":\"enum\",\"options\":[\"Continuous\",\"Chromatic\",\"Octaves\",\"Fifths\",\"Minor\",\"Major\",\"Phrygian Dominant\",\"Minor 9th\",\"Major 9th\",\"Minor Pentatonic\",\"Major Pentatonic\"]},"
        "{\"key\":\"osc2_mode\",\"name\":\"Type 2\",\"short_name\":\"TYPE\",\"description\":\"Oscillator 2 synthesis or Osc 1 audio-processor family\",\"type\":\"enum\",\"options\":[\"BasicWaves\",\"Ring\",\"Wavefolder\",\"Noise\",\"Wavetable\",\"SuperWave\",\"Harmo\",\"Chord\",\"MultiFilter\",\"Crusher\",\"Comb Filter\",\"Surgeon Filter\",\"Phaser Filter\",\"Destroy\",\"V.Analog\",\"Waveshaper\",\"KarplusStr\",\"SawX\",\"Harm\",\"Bass\",\"Modal\",\"Speech\",\"Sample\",\"Particles\",\"Cloud Grains\",\"Hit Grains\",\"Frozen\",\"Skan\",\"Lick\",\"Raster\",\"Audio In\",\"Formant\",\"Two Op FM\",\"FM / RM\"]},"
        "{\"key\":\"osc2_table\",\"name\":\"Wavetable Preset\",\"type\":\"enum\",\"options\":[\"Harmonic\",\"Microwave 2\",\"Bell 03\",\"VoxSynth\",\"Spectral\",\"SuperSaw\",\"Fourier 2\",\"Noise Wave\",\"Folding\",\"Drone\",\"Sitar\",\"Tidal\",\"Harmonix\",\"Morphing\",\"Wavetrip\",\"Sine to Saw\",\"Saw Bend\",\"Basic Morph\",\"Tidy 006\",\"AM Sine\",\"Hmmm\",\"Iterative\",\"Osmaos\",\"Twist 02\",\"Sine Mutate\",\"Reso 00\",\"Sohler 34\",\"Sohler 35\",\"PD 101\",\"Sohler 43\",\"Mellow D\",\"Tidy 007\"]},"
        "{\"key\":\"osc2_wave\",\"name\":\"Wave 2\",\"label\":\"%s\",\"short_name\":\"%s\",\"description\":\"%s\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"osc2_timbre\",\"name\":\"Timbre 2\",\"label\":\"%s\",\"short_name\":\"%s\",\"description\":\"%s\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"osc2_shape\",\"name\":\"Shape 2\",\"label\":\"%s\",\"short_name\":\"%s\",\"description\":\"%s\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"osc2_level\",\"name\":\"Volume 2\",\"label\":\"%s\",\"short_name\":\"%s\",\"description\":\"%s\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"cutoff\",\"name\":\"Cutoff\",\"short_name\":\"CUT\",\"description\":\"SEM-inspired 12 dB/oct multimode filter cutoff. Logarithmic: 0 = 20 Hz, 1 = 18 kHz\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02},"
        "{\"key\":\"cutoff_hz\",\"name\":\"Cutoff Hz\",\"short_name\":\"HZ\",\"description\":\"Read-only cutoff frequency in Hz\",\"type\":\"float\",\"min\":20,\"max\":18000,\"step\":1,\"access\":\"read\",\"unit\":\"Hz\"},"
        "{\"key\":\"resonance\",\"name\":\"Resonance\",\"short_name\":\"RESO\",\"description\":\"Resonance of the SEM-inspired multimode filter (self-oscillates above 0.95)\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"filter_mode\",\"name\":\"Filter Type\",\"short_name\":\"MODE\",\"description\":\"Low-Pass, Band-Pass, or High-Pass response\",\"type\":\"enum\",\"options\":[\"LP\",\"BP\",\"HP\"]},"
        "{\"key\":\"filter_env\",\"name\":\"Env Amount\",\"short_name\":\"ENV\",\"description\":\"Bipolar envelope modulation of filter cutoff\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"filter_fm\",\"name\":\"Filter FM\",\"short_name\":\"FM\",\"description\":\"Bipolar audio-rate modulation of filter cutoff by the Filter FM Source (taken before its level knob); squared response, 0 is off\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"filter_fm_src\",\"name\":\"Filter FM Source\",\"short_name\":\"FMSRC\",\"description\":\"Signal that frequency-modulates the filter cutoff: Oscillator 1, Oscillator 2 or their mix\",\"type\":\"enum\",\"options\":[\"OSC 1\",\"OSC 2\",\"Mix\"]},"
        "{\"key\":\"attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":1,\"max\":50000,\"step\":1,\"knobAcceleration\":\"wide\",\"unit\":\"ms\"},"
        "{\"key\":\"decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":1,\"max\":50000,\"step\":1,\"knobAcceleration\":\"wide\",\"unit\":\"ms\"},"
        "{\"key\":\"sustain\",\"name\":\"Sustain\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"release\",\"name\":\"Release\",\"type\":\"float\",\"min\":1,\"max\":50000,\"step\":1,\"knobAcceleration\":\"wide\",\"unit\":\"ms\"},"
        "{\"key\":\"attack_curve\",\"name\":\"Attack Curve\",\"short_name\":\"A CRV\",\"description\":\"Default: near-straight rise. Quick: rises early\",\"type\":\"enum\",\"options\":[\"Default\",\"Quick\"]},"
        "{\"key\":\"decay_curve\",\"name\":\"Decay Curve\",\"short_name\":\"D CRV\",\"description\":\"Default: exponential. Percussive: fast drop, slow tail\",\"type\":\"enum\",\"options\":[\"Default\",\"Percussive\"]},"
        "{\"key\":\"release_curve\",\"name\":\"Release Curve\",\"short_name\":\"R CRV\",\"description\":\"Default: exponential. Percussive: fast drop, slow tail\",\"type\":\"enum\",\"options\":[\"Default\",\"Percussive\"]},"
        "{\"key\":\"env_retrig\",\"name\":\"Env Retrig\",\"short_name\":\"RTG\",\"description\":\"Reset restarts on every note; Continue carries a legato note on\",\"type\":\"enum\",\"options\":[\"Env Reset\",\"Env Continue\"]},"
        "{\"key\":\"vel_vca\",\"name\":\"Vel to VCA\",\"short_name\":\"V>VCA\",\"description\":\"How much velocity drives the voice level; 0 ignores it\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"vel_vcf\",\"name\":\"Velo > VCF\",\"short_name\":\"V>VCF\",\"description\":\"How much velocity scales the envelope's cutoff sweep\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"vel_env\",\"name\":\"Velo > Env\",\"short_name\":\"V>ENV\",\"description\":\"How much velocity scales the matrix Envelope source\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"vel_time\",\"name\":\"Velo > Time\",\"short_name\":\"V>TIM\",\"description\":\"Higher velocity lengthens Decay and Release (about 30x at 1 and full velocity)\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"velo_source\",\"name\":\"Velo Source\",\"short_name\":\"SRC\",\"description\":\"What the matrix Velo row reads\",\"type\":\"enum\",\"options\":[\"Velo + AT\",\"Velo\",\"AT\"]},"
        "{\"key\":\"kbd_source\",\"name\":\"Kbd Source\",\"short_name\":\"KBD\",\"description\":\"What the matrix Keyboard row reads\",\"type\":\"enum\",\"options\":[\"Linear\",\"S Curve\",\"Random\",\"Voices\"]},"
        "{\"key\":\"lfo1_rate\",\"name\":\"LFO 1 Rate\",\"type\":\"float\",\"min\":0.01,\"max\":100,\"unit\":\"Hz\"},"
        "{\"key\":\"lfo1_depth\",\"name\":\"Direct LFO 1 Pitch (extra)\",\"type\":\"float\",\"min\":0,\"max\":12,\"unit\":\"st\"},"
        "{\"key\":\"lfo1_shape\",\"name\":\"LFO 1 Shape\",\"type\":\"enum\",\"options\":[\"Triangle\",\"Saw\",\"Square\",\"Shaper\",\"Sine\",\"S&H\",\"Slew S&H\",\"ExpSaw\",\"ExpRamp\"]},"
        "{\"key\":\"lfo1_sync\",\"name\":\"LFO 1 Sync\",\"short_name\":\"SYNC\",\"description\":\"Lock LFO 1 to Move's tempo\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
        "{\"key\":\"lfo1_div\",\"name\":\"LFO 1 Div\",\"short_name\":\"DIV\",\"description\":\"Tempo division for LFO 1 when Sync is On\",\"type\":\"enum\",\"options\":[\"8 Bar D\",\"8 Bar\",\"4 Bar D\",\"8 Bar T\",\"4 Bar\",\"2 Bar D\",\"4 Bar T\",\"2 Bar\",\"1 Bar D\",\"2 Bar T\",\"1 Bar\",\"1/2D\",\"1 Bar T\",\"1/2\",\"1/4D\",\"1/2T\",\"1/4\",\"1/8D\",\"1/4T\",\"1/8\",\"1/16D\",\"1/8T\",\"1/16\",\"1/32D\",\"1/16T\",\"1/32\",\"1/32T\"]},"
        "{\"key\":\"lfo1_retrig\",\"name\":\"LFO 1 Retrig\",\"short_name\":\"RTG\",\"description\":\"What restarts the LFO; One plays a single cycle per note\",\"type\":\"enum\",\"options\":[\"Free\",\"Poly Kbd\",\"Mono Kbd\",\"Legato Kbd\",\"One\"]},"
        "{\"key\":\"lfo2_rate\",\"name\":\"LFO 2 Rate\",\"type\":\"float\",\"min\":0.01,\"max\":100,\"unit\":\"Hz\"},"
        "{\"key\":\"lfo2_depth\",\"name\":\"Direct LFO 2 Cutoff (extra)\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo2_shape\",\"name\":\"LFO 2 Shape\",\"type\":\"enum\",\"options\":[\"Triangle\",\"Saw\",\"Square\",\"Shaper\",\"Sine\",\"S&H\",\"Slew S&H\",\"ExpSaw\",\"ExpRamp\"]},"
        "{\"key\":\"lfo2_sync\",\"name\":\"LFO 2 Sync\",\"short_name\":\"SYNC\",\"description\":\"Lock LFO 2 to Move's tempo\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
        "{\"key\":\"lfo2_div\",\"name\":\"LFO 2 Div\",\"short_name\":\"DIV\",\"description\":\"Tempo division for LFO 2 when Sync is On\",\"type\":\"enum\",\"options\":[\"8 Bar D\",\"8 Bar\",\"4 Bar D\",\"8 Bar T\",\"4 Bar\",\"2 Bar D\",\"4 Bar T\",\"2 Bar\",\"1 Bar D\",\"2 Bar T\",\"1 Bar\",\"1/2D\",\"1 Bar T\",\"1/2\",\"1/4D\",\"1/2T\",\"1/4\",\"1/8D\",\"1/4T\",\"1/8\",\"1/16D\",\"1/8T\",\"1/16\",\"1/32D\",\"1/16T\",\"1/32\",\"1/32T\"]},"
        "{\"key\":\"lfo2_retrig\",\"name\":\"LFO 2 Retrig\",\"short_name\":\"RTG\",\"description\":\"What restarts the LFO; One plays a single cycle per note\",\"type\":\"enum\",\"options\":[\"Free\",\"Poly Kbd\",\"Mono Kbd\",\"Legato Kbd\",\"One\"]},"
        "{\"key\":\"osc_phase\",\"name\":\"Osc Freerun\",\"short_name\":\"FREE\",\"description\":\"Free-running oscillator phase, or reset on each note\",\"type\":\"enum\",\"options\":[\"Trig\",\"Free\"]},"
        "{\"key\":\"control_rate\",\"name\":\"Mod Rate\",\"short_name\":\"MODRT\",\"description\":\"How often modulation, LFOs and pitch update: Full every sample, 1/8 every eighth sample for the least CPU\",\"type\":\"enum\",\"options\":[\"Full\",\"1/2\",\"1/4\",\"1/8\"]},"
        "{\"key\":\"cycle_mode\",\"name\":\"Cycle Mode\",\"short_name\":\"MODE\",\"description\":\"Env: one-shot, Hold is sustain. Run: free loop. Loop: restarts per note\",\"type\":\"enum\",\"options\":[\"Env\",\"Run\",\"Loop\"]},"
        "{\"key\":\"cycle_rise\",\"name\":\"Cycle Rise\",\"short_name\":\"RISE\",\"description\":\"Cycling envelope rise time (1 ms to 50 s; Run/Loop knobs reach 10 s)\",\"type\":\"float\",\"min\":1,\"max\":50000,\"step\":1,\"knobAcceleration\":\"wide\",\"unit\":\"ms\"},"
        "{\"key\":\"cycle_fall\",\"name\":\"Cycle Fall\",\"short_name\":\"FALL\",\"description\":\"Cycling envelope fall time (1 ms to 50 s; Run/Loop knobs reach 10 s)\",\"type\":\"float\",\"min\":1,\"max\":50000,\"step\":1,\"knobAcceleration\":\"wide\",\"unit\":\"ms\"},"
        "{\"key\":\"cycle_hold\",\"name\":\"Cycle Hold/Sustain\",\"short_name\":\"HOLD\",\"description\":\"Env mode: sustain level. Run/Loop: time held at the peak before Fall\",\"type\":\"float\",\"min\":0,\"max\":10000,\"step\":1,\"knobAcceleration\":\"wide\",\"unit\":\"ms\"},"
        "{\"key\":\"cycle_amount\",\"name\":\"Cycle Amount\",\"short_name\":\"AMT\",\"description\":\"Cycling Envelope level; also the legacy direct filter sweep\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"cycle_rise_curve\",\"name\":\"Cycle Rise Shape\",\"short_name\":\"R SHP\",\"description\":\"Loops: 0.5 linear, lower starts slow. Env: over 0.5 Quick, else Default\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"cycle_fall_curve\",\"name\":\"Cycle Fall Shape\",\"short_name\":\"F SHP\",\"description\":\"Loops: 0.5 linear, lower drops fast. Env: over 0.5 Percussive, else Default\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"cycle_rate\",\"name\":\"Cycle Rate (legacy)\",\"short_name\":\"RATE\",\"description\":\"Legacy rate: sets Rise and Fall to half its period\",\"type\":\"float\",\"min\":0.01,\"max\":20,\"unit\":\"Hz\"},"
        "{\"key\":\"cycle_depth\",\"name\":\"Cycle Depth (legacy)\",\"short_name\":\"DPTH\",\"description\":\"Legacy alias of Cycle Amount\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"lfo1_point1\",\"name\":\"LFO 1 P1\",\"short_name\":\"P1\",\"description\":\"Custom LFO curve, point 1 of 8\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo1_point2\",\"name\":\"P2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo1_point3\",\"name\":\"P3\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo1_point4\",\"name\":\"P4\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo1_point5\",\"name\":\"P5\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo1_point6\",\"name\":\"P6\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo1_point7\",\"name\":\"P7\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo1_point8\",\"name\":\"P8\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo2_point1\",\"name\":\"LFO 2 P1\",\"short_name\":\"P1\",\"description\":\"Custom LFO curve, point 1 of 8\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo2_point2\",\"name\":\"P2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo2_point3\",\"name\":\"P3\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo2_point4\",\"name\":\"P4\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo2_point5\",\"name\":\"P5\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo2_point6\",\"name\":\"P6\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo2_point7\",\"name\":\"P7\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"lfo2_point8\",\"name\":\"P8\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro1\",\"name\":\"Macro 1\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"macro2\",\"name\":\"Macro 2\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"macro3\",\"name\":\"Macro 3\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"macro4\",\"name\":\"Macro 4\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"mod_wheel\",\"name\":\"Mod Wheel\",\"short_name\":\"WHEEL\",\"description\":\"Mod wheel position (the grid's Wheel source); MIDI CC 1 moves it\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"voice_mode\",\"name\":\"Voice Mode\",\"type\":\"enum\",\"options\":[\"Poly\",\"Mono\",\"Unison\"]},"
        "{\"key\":\"glide\",\"name\":\"Glide\",\"type\":\"float\",\"min\":0,\"max\":12000,\"unit\":\"ms\"},"
        "{\"key\":\"glide_mode\",\"name\":\"Glide Mode\",\"short_name\":\"GMODE\",\"description\":\"Time: fixed glide time; Rate: fixed semitones per second\",\"type\":\"enum\",\"options\":[\"Time\",\"Rate\"]},"
        "{\"key\":\"bend_range\",\"name\":\"Bend Range\",\"short_name\":\"BEND\",\"description\":\"Pitch-bend range in semitones\",\"type\":\"float\",\"min\":0,\"max\":24,\"step\":1,\"unit\":\"st\"},"
        "{\"key\":\"spread\",\"name\":\"Unison Spread\",\"type\":\"float\",\"min\":0,\"max\":7200,\"unit\":\"cent\"},"
        "{\"key\":\"unison_voices\",\"name\":\"Unison Voices\",\"type\":\"int\",\"min\":2,\"max\":6,\"step\":1},"
        "{\"key\":\"unison_mode\",\"name\":\"Unison Mode\",\"short_name\":\"UMODE\",\"description\":\"Mono: one stack follows the last key; Poly: every key gets its own stack of Unison Voices\",\"type\":\"enum\",\"options\":[\"Mono\",\"Poly\"]},"
        "{\"key\":\"chord\",\"name\":\"Chord\",\"short_name\":\"CHORD\",\"description\":\"Keyboard Chord: every key also plays the notes below\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
        "{\"key\":\"chord_notes\",\"name\":\"Chord Notes\",\"short_name\":\"NOTES\",\"description\":\"How many notes the chord adds to each key\",\"type\":\"int\",\"min\":1,\"max\":5,\"step\":1},"
        "{\"key\":\"chord_note1\",\"name\":\"Chord Note 1\",\"short_name\":\"NOTE1\",\"type\":\"int\",\"min\":-48,\"max\":48,\"step\":1,\"unit\":\"st\"},"
        "{\"key\":\"chord_note2\",\"name\":\"Chord Note 2\",\"short_name\":\"NOTE2\",\"type\":\"int\",\"min\":-48,\"max\":48,\"step\":1,\"unit\":\"st\"},"
        "{\"key\":\"chord_note3\",\"name\":\"Chord Note 3\",\"short_name\":\"NOTE3\",\"type\":\"int\",\"min\":-48,\"max\":48,\"step\":1,\"unit\":\"st\"},"
        "{\"key\":\"chord_note4\",\"name\":\"Chord Note 4\",\"short_name\":\"NOTE4\",\"type\":\"int\",\"min\":-48,\"max\":48,\"step\":1,\"unit\":\"st\"},"
        "{\"key\":\"chord_note5\",\"name\":\"Chord Note 5\",\"short_name\":\"NOTE5\",\"type\":\"int\",\"min\":-48,\"max\":48,\"step\":1,\"unit\":\"st\"},"
        "{\"key\":\"stereo\",\"name\":\"Stereo Width\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"output\",\"name\":\"Output\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"fx1_type\",\"name\":\"FX 1 Type\",\"type\":\"enum\",\"options\":[\"Off\",\"Delay\",\"Reverb\",\"Chorus\",\"Drive\",\"Crusher\",\"Phaser\",\"3-Band EQ\",\"Flanger\",\"Super Unison\",\"Peak EQ\",\"Multi Comp\"]},"
        "{\"key\":\"fx1_subtype\",\"name\":\"FX 1 Preset\",\"type\":\"enum\",\"options\":%s},"
        "{\"key\":\"fx1_mix\",\"name\":\"FX 1 Mix\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"fx1_time\",\"name\":\"FX 1 Time\",\"type\":\"float\",\"min\":1,\"max\":1000,\"unit\":\"ms\"},"
        "{\"key\":\"fx1_feedback\",\"name\":\"FX 1 Feedback\",\"type\":\"float\",\"min\":0,\"max\":0.95},"
        "{\"key\":\"fx1_frequency\",\"name\":\"FX 1 Peak Frequency\",\"type\":\"float\",\"min\":30,\"max\":15000,\"step\":1,\"knobAcceleration\":\"wide\",\"unit\":\"Hz\"},"
        "{\"key\":\"fx1_gain\",\"name\":\"FX 1 Peak Gain\",\"type\":\"float\",\"min\":-15,\"max\":6,\"unit\":\"dB\"},"
        "{\"key\":\"fx1_width\",\"name\":\"FX 1 Peak Width\",\"type\":\"float\",\"min\":0.1,\"max\":10,\"unit\":\"Q\"},"
        "{\"key\":\"fx1_low\",\"name\":\"FX 1 Low\",\"type\":\"float\",\"min\":-15,\"max\":6,\"step\":0.1,\"unit\":\"dB\"},"
        "{\"key\":\"fx1_mid\",\"name\":\"FX 1 Mid\",\"type\":\"float\",\"min\":-15,\"max\":6,\"step\":0.1,\"unit\":\"dB\"},"
        "{\"key\":\"fx1_high\",\"name\":\"FX 1 High\",\"type\":\"float\",\"min\":-15,\"max\":6,\"step\":0.1,\"unit\":\"dB\"},"
        "{\"key\":\"fx2_type\",\"name\":\"FX 2 Type\",\"type\":\"enum\",\"options\":[\"Off\",\"Delay\",\"Reverb\",\"Chorus\",\"Drive\",\"Crusher\",\"Phaser\",\"3-Band EQ\",\"Flanger\",\"Super Unison\",\"Peak EQ\",\"Multi Comp\"]},"
        "{\"key\":\"fx2_subtype\",\"name\":\"FX 2 Preset\",\"type\":\"enum\",\"options\":%s},"
        "{\"key\":\"fx2_mix\",\"name\":\"FX 2 Mix\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"fx2_time\",\"name\":\"FX 2 Time\",\"type\":\"float\",\"min\":1,\"max\":1000,\"unit\":\"ms\"},"
        "{\"key\":\"fx2_feedback\",\"name\":\"FX 2 Feedback\",\"type\":\"float\",\"min\":0,\"max\":0.95},"
        "{\"key\":\"fx2_frequency\",\"name\":\"FX 2 Peak Frequency\",\"type\":\"float\",\"min\":30,\"max\":15000,\"step\":1,\"knobAcceleration\":\"wide\",\"unit\":\"Hz\"},"
        "{\"key\":\"fx2_gain\",\"name\":\"FX 2 Peak Gain\",\"type\":\"float\",\"min\":-15,\"max\":6,\"unit\":\"dB\"},"
        "{\"key\":\"fx2_width\",\"name\":\"FX 2 Peak Width\",\"type\":\"float\",\"min\":0.1,\"max\":10,\"unit\":\"Q\"},"
        "{\"key\":\"fx2_low\",\"name\":\"FX 2 Low\",\"type\":\"float\",\"min\":-15,\"max\":6,\"step\":0.1,\"unit\":\"dB\"},"
        "{\"key\":\"fx2_mid\",\"name\":\"FX 2 Mid\",\"type\":\"float\",\"min\":-15,\"max\":6,\"step\":0.1,\"unit\":\"dB\"},"
        "{\"key\":\"fx2_high\",\"name\":\"FX 2 High\",\"type\":\"float\",\"min\":-15,\"max\":6,\"step\":0.1,\"unit\":\"dB\"},"
        "{\"key\":\"fx3_type\",\"name\":\"FX 3 Type\",\"type\":\"enum\",\"options\":[\"Off\",\"Delay\",\"Reverb\",\"Chorus\",\"Drive\",\"Crusher\",\"Phaser\",\"3-Band EQ\",\"Flanger\",\"Super Unison\",\"Peak EQ\",\"Multi Comp\"]},"
        "{\"key\":\"fx3_subtype\",\"name\":\"FX 3 Preset\",\"type\":\"enum\",\"options\":%s},"
        "{\"key\":\"fx3_mix\",\"name\":\"FX 3 Mix\",\"type\":\"float\",\"min\":0,\"max\":1},"
        "{\"key\":\"fx3_time\",\"name\":\"FX 3 Time\",\"type\":\"float\",\"min\":1,\"max\":1000,\"unit\":\"ms\"},"
        "{\"key\":\"fx3_feedback\",\"name\":\"FX 3 Feedback\",\"type\":\"float\",\"min\":0,\"max\":0.95},"
        "{\"key\":\"fx3_frequency\",\"name\":\"FX 3 Peak Frequency\",\"type\":\"float\",\"min\":30,\"max\":15000,\"step\":1,\"knobAcceleration\":\"wide\",\"unit\":\"Hz\"},"
        "{\"key\":\"fx3_gain\",\"name\":\"FX 3 Peak Gain\",\"type\":\"float\",\"min\":-15,\"max\":6,\"unit\":\"dB\"},"
        "{\"key\":\"fx3_width\",\"name\":\"FX 3 Peak Width\",\"type\":\"float\",\"min\":0.1,\"max\":10,\"unit\":\"Q\"},"
        "{\"key\":\"fx3_low\",\"name\":\"FX 3 Low\",\"type\":\"float\",\"min\":-15,\"max\":6,\"step\":0.1,\"unit\":\"dB\"},"
        "{\"key\":\"fx3_mid\",\"name\":\"FX 3 Mid\",\"type\":\"float\",\"min\":-15,\"max\":6,\"step\":0.1,\"unit\":\"dB\"},"
        "{\"key\":\"fx3_high\",\"name\":\"FX 3 High\",\"type\":\"float\",\"min\":-15,\"max\":6,\"step\":0.1,\"unit\":\"dB\"},"
        "{\"key\":\"modcol1_dest\",\"name\":\"Mod 1 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol1_cyc\",\"name\":\"Mod 1 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol1_env\",\"name\":\"Mod 1 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol1_lfo1\",\"name\":\"Mod 1 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol1_lfo2\",\"name\":\"Mod 1 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol1_vel\",\"name\":\"Mod 1 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol1_whl\",\"name\":\"Mod 1 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol1_kbd\",\"name\":\"Mod 1 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol2_dest\",\"name\":\"Mod 2 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol2_cyc\",\"name\":\"Mod 2 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol2_env\",\"name\":\"Mod 2 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol2_lfo1\",\"name\":\"Mod 2 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol2_lfo2\",\"name\":\"Mod 2 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol2_vel\",\"name\":\"Mod 2 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol2_whl\",\"name\":\"Mod 2 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol2_kbd\",\"name\":\"Mod 2 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol3_dest\",\"name\":\"Mod 3 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol3_cyc\",\"name\":\"Mod 3 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol3_env\",\"name\":\"Mod 3 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol3_lfo1\",\"name\":\"Mod 3 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol3_lfo2\",\"name\":\"Mod 3 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol3_vel\",\"name\":\"Mod 3 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol3_whl\",\"name\":\"Mod 3 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol3_kbd\",\"name\":\"Mod 3 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol4_dest\",\"name\":\"Mod 4 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol4_cyc\",\"name\":\"Mod 4 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol4_env\",\"name\":\"Mod 4 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol4_lfo1\",\"name\":\"Mod 4 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol4_lfo2\",\"name\":\"Mod 4 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol4_vel\",\"name\":\"Mod 4 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol4_whl\",\"name\":\"Mod 4 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol4_kbd\",\"name\":\"Mod 4 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol5_dest\",\"name\":\"Mod 5 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol5_cyc\",\"name\":\"Mod 5 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol5_env\",\"name\":\"Mod 5 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol5_lfo1\",\"name\":\"Mod 5 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol5_lfo2\",\"name\":\"Mod 5 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol5_vel\",\"name\":\"Mod 5 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol5_whl\",\"name\":\"Mod 5 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol5_kbd\",\"name\":\"Mod 5 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol6_dest\",\"name\":\"Mod 6 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol6_cyc\",\"name\":\"Mod 6 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol6_env\",\"name\":\"Mod 6 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol6_lfo1\",\"name\":\"Mod 6 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol6_lfo2\",\"name\":\"Mod 6 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol6_vel\",\"name\":\"Mod 6 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol6_whl\",\"name\":\"Mod 6 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol6_kbd\",\"name\":\"Mod 6 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol7_dest\",\"name\":\"Mod 7 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol7_cyc\",\"name\":\"Mod 7 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol7_env\",\"name\":\"Mod 7 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol7_lfo1\",\"name\":\"Mod 7 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol7_lfo2\",\"name\":\"Mod 7 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol7_vel\",\"name\":\"Mod 7 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol7_whl\",\"name\":\"Mod 7 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol7_kbd\",\"name\":\"Mod 7 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol8_dest\",\"name\":\"Mod 8 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol8_cyc\",\"name\":\"Mod 8 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol8_env\",\"name\":\"Mod 8 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol8_lfo1\",\"name\":\"Mod 8 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol8_lfo2\",\"name\":\"Mod 8 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol8_vel\",\"name\":\"Mod 8 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol8_whl\",\"name\":\"Mod 8 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol8_kbd\",\"name\":\"Mod 8 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol9_dest\",\"name\":\"Mod 9 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol9_cyc\",\"name\":\"Mod 9 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol9_env\",\"name\":\"Mod 9 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol9_lfo1\",\"name\":\"Mod 9 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol9_lfo2\",\"name\":\"Mod 9 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol9_vel\",\"name\":\"Mod 9 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol9_whl\",\"name\":\"Mod 9 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol9_kbd\",\"name\":\"Mod 9 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol10_dest\",\"name\":\"Mod 10 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol10_cyc\",\"name\":\"Mod 10 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol10_env\",\"name\":\"Mod 10 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol10_lfo1\",\"name\":\"Mod 10 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol10_lfo2\",\"name\":\"Mod 10 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol10_vel\",\"name\":\"Mod 10 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol10_whl\",\"name\":\"Mod 10 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol10_kbd\",\"name\":\"Mod 10 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol11_dest\",\"name\":\"Mod 11 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol11_cyc\",\"name\":\"Mod 11 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol11_env\",\"name\":\"Mod 11 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol11_lfo1\",\"name\":\"Mod 11 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol11_lfo2\",\"name\":\"Mod 11 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol11_vel\",\"name\":\"Mod 11 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol11_whl\",\"name\":\"Mod 11 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol11_kbd\",\"name\":\"Mod 11 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol12_dest\",\"name\":\"Mod 12 Dest\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"modcol12_cyc\",\"name\":\"Mod 12 Cyc\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol12_env\",\"name\":\"Mod 12 Env\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol12_lfo1\",\"name\":\"Mod 12 LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol12_lfo2\",\"name\":\"Mod 12 LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol12_vel\",\"name\":\"Mod 12 Vel\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol12_whl\",\"name\":\"Mod 12 Whl\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"modcol12_kbd\",\"name\":\"Mod 12 Kbd\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro1_dest1\",\"name\":\"Macro 1 Dest 1\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro1_amt1\",\"name\":\"Macro 1 Amount 1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro1_dest2\",\"name\":\"Macro 1 Dest 2\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro1_amt2\",\"name\":\"Macro 1 Amount 2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro1_dest3\",\"name\":\"Macro 1 Dest 3\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro1_amt3\",\"name\":\"Macro 1 Amount 3\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro1_dest4\",\"name\":\"Macro 1 Dest 4\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro1_amt4\",\"name\":\"Macro 1 Amount 4\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro2_dest1\",\"name\":\"Macro 2 Dest 1\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro2_amt1\",\"name\":\"Macro 2 Amount 1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro2_dest2\",\"name\":\"Macro 2 Dest 2\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro2_amt2\",\"name\":\"Macro 2 Amount 2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro2_dest3\",\"name\":\"Macro 2 Dest 3\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro2_amt3\",\"name\":\"Macro 2 Amount 3\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro2_dest4\",\"name\":\"Macro 2 Dest 4\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro2_amt4\",\"name\":\"Macro 2 Amount 4\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro3_dest1\",\"name\":\"Macro 3 Dest 1\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro3_amt1\",\"name\":\"Macro 3 Amount 1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro3_dest2\",\"name\":\"Macro 3 Dest 2\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro3_amt2\",\"name\":\"Macro 3 Amount 2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro3_dest3\",\"name\":\"Macro 3 Dest 3\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro3_amt3\",\"name\":\"Macro 3 Amount 3\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro3_dest4\",\"name\":\"Macro 3 Dest 4\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro3_amt4\",\"name\":\"Macro 3 Amount 4\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro4_dest1\",\"name\":\"Macro 4 Dest 1\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro4_amt1\",\"name\":\"Macro 4 Amount 1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro4_dest2\",\"name\":\"Macro 4 Dest 2\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro4_amt2\",\"name\":\"Macro 4 Amount 2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro4_dest3\",\"name\":\"Macro 4 Dest 3\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro4_amt3\",\"name\":\"Macro 4 Amount 3\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"macro4_dest4\",\"name\":\"Macro 4 Dest 4\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"macro4_amt4\",\"name\":\"Macro 4 Amount 4\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"preset\",\"name\":\"Factory Patch\",\"type\":\"int\",\"min\":0,\"max\":%s,\"step\":1}"
        "]",
        r1->wave_label, r1->wave_short, r1->wave_tip,
        r1->timbre_label, r1->timbre_short, r1->timbre_tip,
        r1->shape_label, r1->shape_short, r1->shape_tip,
        r1->level_label, r1->level_short, r1->level_tip,
        r2->wave_label, r2->wave_short, r2->wave_tip,
        r2->timbre_label, r2->timbre_short, r2->timbre_tip,
        r2->shape_label, r2->shape_short, r2->shape_tip,
        r2->level_label, r2->level_short, r2->level_tip,
        fx1_presets, fx2_presets, fx3_presets,
        preset_max);
    return n >= 0 && n < buf_len ? n : -1;
}

/* Move reads chain_params into a 32 KiB buffer and a longer document breaks
 * the device's parameter list, so the device copy drops every
 * ,"description":"..." field (about 3.3 KB). No description holds a quote. */
static int strip_descriptions(char *buf) {
    static const char tag[] = ",\"description\":\"";
    char *out = buf;
    const char *in = buf;
    while (*in) {
        if (strncmp(in, tag, sizeof(tag) - 1) == 0) {
            const char *end = strchr(in + sizeof(tag) - 1, '"');
            if (end) {
                in = end + 1;
                continue;
            }
        }
        *out++ = *in++;
    }
    *out = '\0';
    return (int)(out - buf);
}

/* The device copy also leaves out the 96 modcol<N>_* entries (about 10 KB).
 * Move's pages reach the matrix through the mod_edit_* aliases, whose
 * metadata is inline in the hierarchy, and the modcol levels are gated out of
 * its walk; the keys themselves still work in get_param, set_param and state,
 * exactly like the seq_* keys. chain_params_full keeps them for the Remote UI
 * and the reference. Each entry is one flat object ending in "}," . */
static int strip_modcol_entries(char *buf) {
    static const char tag[] = "{\"key\":\"modcol";
    char *out = buf;
    const char *in = buf;
    while (*in) {
        if (strncmp(in, tag, sizeof(tag) - 1) == 0) {
            const char *end = strstr(in, "},");
            if (end) {
                in = end + 2;
                continue;
            }
        }
        *out++ = *in++;
    }
    *out = '\0';
    return (int)(out - buf);
}

static int format_ui_hierarchy(const sf_instance_t *inst, char *buf, int buf_len) {
    int m1 = (int)clamp_enum((float)(inst ? inst->osc1_mode : 0), SF_OSC1_MODE_COUNT);
    int m2 = (int)clamp_enum((float)(inst ? inst->osc2_mode : 0), SF_OSC2_MODE_COUNT);
    const sf_osc_role_meta_t *r1 = g_osc1_roles[m1];
    const sf_osc_role_meta_t *r2 = g_osc2_roles[m2];
    int n = snprintf(buf, (size_t)buf_len,
        "{\"levels\":{"
        /* Main page: the performance knobs. Every factory preset has Macro 1
         * Timbre and Macro 2 Space, and the LFO presets Macro 3 Motion, so row
         * one plays alike across the bank. Row two is cutoff and resonance (the
         * host draws them as a filter curve) and attack and release (drawn as
         * an AR envelope). The engine Types moved to knob 1 of their own pages:
         * a 27- or 36-way selector is no performance knob.
         *
         * The levels below are the jog order: each section is followed by
         * what refines it (LFO 1 by its curve, the macros by their slots). */
        "\"root\":{\"label\":\"Bizar\",\"knobs\":[\"macro1\",\"macro2\",\"macro3\",\"output\",\"cutoff\",\"resonance\",\"attack\",\"release\"],\"params\":["
        "{\"key\":\"macro1\",\"short_name\":\"MAC1\",\"label\":\"Macro 1\"},"
        "{\"key\":\"macro2\",\"short_name\":\"MAC2\",\"label\":\"Macro 2\"},"
        "{\"key\":\"macro3\",\"short_name\":\"MAC3\",\"label\":\"Macro 3\"},"
        "{\"key\":\"output\",\"short_name\":\"VOL\",\"label\":\"Volume\"},"
        "\"cutoff\",\"resonance\",\"attack\",\"release\","
        "{\"level\":\"factory\",\"label\":\"Factory Patches\"},"
        "{\"level\":\"engine1\",\"label\":\"Oscillator 1\"},"
        "{\"level\":\"engine2\",\"label\":\"Oscillator 2\"},"
        "{\"level\":\"filter\",\"label\":\"Filter\"},"
        "{\"level\":\"envelope\",\"label\":\"Envelope\"},"
        "{\"level\":\"env_vel\",\"label\":\"Velocity Targets\"},"
        "{\"level\":\"cycle_env\",\"label\":\"Cycling Envelope\"},"
        "{\"level\":\"lfo1\",\"label\":\"LFO 1\"},"
        "{\"level\":\"lfo1_curve\",\"label\":\"LFO 1 Curve\"},"
        "{\"level\":\"lfo2\",\"label\":\"LFO 2\"},"
        "{\"level\":\"lfo2_curve\",\"label\":\"LFO 2 Curve\"},"
        "{\"level\":\"modmatrix\",\"label\":\"Mod Matrix\"},"
        "{\"level\":\"modmatrix_edit\",\"label\":\"Matrix Editor\"},"
        "{\"level\":\"macros\",\"label\":\"Macros\"},"
        "{\"level\":\"macro1_slots\",\"label\":\"Macro 1 Slots\"},"
        "{\"level\":\"macro2_slots\",\"label\":\"Macro 2 Slots\"},"
        "{\"level\":\"macro3_slots\",\"label\":\"Macro 3 Slots\"},"
        "{\"level\":\"macro4_slots\",\"label\":\"Macro 4 Slots\"},"
        "{\"level\":\"effects\",\"label\":\"Effect 1\"},"
        "{\"level\":\"effect2\",\"label\":\"Effect 2\"},"
        "{\"level\":\"effect3\",\"label\":\"Effect 3\"},"
        "{\"level\":\"voice\",\"label\":\"Voice\"},"
        "{\"level\":\"chord\",\"label\":\"Chord\"},"
        "{\"level\":\"settings\",\"label\":\"Settings\"}]},"
        "\"engine1\":{\"label\":\"Oscillator 1\",\"knobs\":[\"osc1_mode\",\"osc1_wave\",\"osc1_timbre\",\"osc1_shape\",\"osc1_tune\",\"osc1_table\",\"osc1_quantize\",\"osc1_level\"],"
        "\"params\":["
        "{\"key\":\"osc1_mode\",\"short_name\":\"TYPE\",\"label\":\"Type 1\"},"
        "{\"key\":\"osc1_wave\",\"short_name\":\"%s\",\"label\":\"%s\"},"
        "{\"key\":\"osc1_timbre\",\"short_name\":\"%s\",\"label\":\"%s\"},"
        "{\"key\":\"osc1_shape\",\"short_name\":\"%s\",\"label\":\"%s\"},"
        "{\"key\":\"osc1_tune\",\"short_name\":\"TUNE\",\"label\":\"Tune 1\"},"
        "{\"key\":\"osc1_table\",\"short_name\":\"TABLE\",\"label\":\"Wavetable\",\"visible_if\":{\"param\":\"osc1_mode\",\"equals\":\"Wavetable\"}},"
        "{\"key\":\"osc1_quantize\",\"short_name\":\"QUANT\",\"label\":\"Mod Quantize 1\"},"
        "{\"key\":\"osc1_level\",\"short_name\":\"%s\",\"label\":\"%s\"}]},"
        "\"engine2\":{\"label\":\"Oscillator 2\",\"knobs\":[\"osc2_mode\",\"osc2_wave\",\"osc2_timbre\",\"osc2_shape\",\"osc2_tune\",\"osc2_table\",\"osc2_quantize\",\"osc2_level\"],"
        "\"params\":["
        "{\"key\":\"osc2_mode\",\"short_name\":\"TYPE\",\"label\":\"Type 2\"},"
        "{\"key\":\"osc2_wave\",\"short_name\":\"%s\",\"label\":\"%s\"},"
        "{\"key\":\"osc2_timbre\",\"short_name\":\"%s\",\"label\":\"%s\"},"
        "{\"key\":\"osc2_shape\",\"short_name\":\"%s\",\"label\":\"%s\"},"
        "{\"key\":\"osc2_tune\",\"short_name\":\"TUNE\",\"label\":\"Tune 2\"},"
        "{\"key\":\"osc2_table\",\"short_name\":\"TABLE\",\"label\":\"Wavetable\",\"visible_if\":{\"param\":\"osc2_mode\",\"equals\":\"Wavetable\"}},"
        "{\"key\":\"osc2_quantize\",\"short_name\":\"QUANT\",\"label\":\"Mod Quantize 2\"},"
        "{\"key\":\"osc2_level\",\"short_name\":\"%s\",\"label\":\"%s\"}]},"
        "\"filter\":{\"label\":\"Filter\",\"knobs\":[\"filter_mode\",\"cutoff\",\"resonance\",\"filter_env\",\"cutoff_hz\",\"filter_fm\",\"filter_fm_src\"],\"params\":[\"filter_mode\",\"cutoff\",\"resonance\",\"filter_env\",\"cutoff_hz\",\"filter_fm\",\"filter_fm_src\"]},"
        "\"envelope\":{\"label\":\"Envelope\",\"knobs\":[\"attack\",\"decay\",\"sustain\",\"release\",\"attack_curve\",\"decay_curve\",\"release_curve\",\"env_retrig\"],\"params\":[\"attack\",\"decay\",\"sustain\",\"release\",\"attack_curve\",\"decay_curve\",\"release_curve\",\"env_retrig\"]},"
        "\"env_vel\":{\"label\":\"Velocity Targets\",\"knobs\":[\"vel_vca\",\"vel_vcf\",\"vel_env\",\"vel_time\",\"velo_source\",\"kbd_source\"],\"params\":[\"vel_vca\",\"vel_vcf\",\"vel_env\",\"vel_time\",\"velo_source\",\"kbd_source\"]},"
        /* Shape, Rate and Depth side by side in one row draw as an LFO. */
        "\"lfo1\":{\"label\":\"LFO 1\",\"knobs\":[\"lfo1_shape\",\"lfo1_rate\",\"lfo1_depth\",\"lfo1_sync\",\"lfo1_div\",\"lfo1_retrig\"],\"params\":[\"lfo1_shape\",\"lfo1_rate\",\"lfo1_depth\",\"lfo1_sync\",\"lfo1_div\",\"lfo1_retrig\"]},"
        "\"lfo2\":{\"label\":\"LFO 2\",\"knobs\":[\"lfo2_shape\",\"lfo2_rate\",\"lfo2_depth\",\"lfo2_sync\",\"lfo2_div\",\"lfo2_retrig\"],\"params\":[\"lfo2_shape\",\"lfo2_rate\",\"lfo2_depth\",\"lfo2_sync\",\"lfo2_div\",\"lfo2_retrig\"]},"
        /* The stage times and level on row one, how the stages bend on row two. */
        "\"cycle_env\":{\"label\":\"Cycling Envelope\",\"knobs\":[\"cycle_rise\",\"cycle_fall\",\"cycle_hold\",\"cycle_amount\",\"cycle_mode\",\"cycle_rise_curve\",\"cycle_fall_curve\"],\"params\":[\"cycle_rise\",\"cycle_fall\",\"cycle_hold\",\"cycle_amount\",\"cycle_mode\",\"cycle_rise_curve\",\"cycle_fall_curve\"]},"
        "\"modmatrix\":{\"label\":\"Mod Matrix\",\"items_param\":\"mod_matrix_items\",\"select_param\":\"mod_edit_col\",\"navigate_to\":\"modmatrix_edit\",\"params\":[]},"
        "\"modmatrix_edit\":{\"label\":\"Matrix Editor\",\"knobs\":[\"mod_edit_dest\",\"mod_edit_cyc\",\"mod_edit_env\",\"mod_edit_lfo1\",\"mod_edit_lfo2\",\"mod_edit_vel\",\"mod_edit_whl\",\"mod_edit_kbd\"],"
        "\"params\":[{\"key\":\"mod_edit_dest\",\"name\":\"Destination\",\"label\":\"Destination\",\"short_name\":\"DEST\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\",\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\",\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\",\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\",\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\",\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\",\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\",\"M1:3\",\"M1:4\",\"M1:5\",\"M1:6\",\"M1:7\",\"M1:8\",\"M1:9\",\"M1:10\",\"M1:11\",\"M1:12\",\"M2:1\",\"M2:2\",\"M2:3\",\"M2:4\",\"M2:5\",\"M2:6\",\"M2:7\",\"M2:8\",\"M2:9\",\"M2:10\",\"M2:11\",\"M2:12\",\"M3:1\",\"M3:2\",\"M3:3\",\"M3:4\",\"M3:5\",\"M3:6\",\"M3:7\",\"M3:8\",\"M3:9\",\"M3:10\",\"M3:11\",\"M3:12\",\"M4:1\",\"M4:2\",\"M4:3\",\"M4:4\",\"M4:5\",\"M4:6\",\"M4:7\",\"M4:8\",\"M4:9\",\"M4:10\",\"M4:11\",\"M4:12\",\"M5:1\",\"M5:2\",\"M5:3\",\"M5:4\",\"M5:5\",\"M5:6\",\"M5:7\",\"M5:8\",\"M5:9\",\"M5:10\",\"M5:11\",\"M5:12\",\"M6:1\",\"M6:2\",\"M6:3\",\"M6:4\",\"M6:5\",\"M6:6\",\"M6:7\",\"M6:8\",\"M6:9\",\"M6:10\",\"M6:11\",\"M6:12\",\"M7:1\",\"M7:2\",\"M7:3\",\"M7:4\",\"M7:5\",\"M7:6\",\"M7:7\",\"M7:8\",\"M7:9\",\"M7:10\",\"M7:11\",\"M7:12\",\"FFM\"]},"
        "{\"key\":\"mod_edit_cyc\",\"name\":\"Cycling Env\",\"label\":\"Cycling Env\",\"short_name\":\"CYC\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"mod_edit_env\",\"name\":\"Envelope\",\"label\":\"Envelope\",\"short_name\":\"ENV\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"mod_edit_lfo1\",\"name\":\"LFO 1\",\"label\":\"LFO 1\",\"short_name\":\"LFO1\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"mod_edit_lfo2\",\"name\":\"LFO 2\",\"label\":\"LFO 2\",\"short_name\":\"LFO2\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"mod_edit_vel\",\"name\":\"Velo + AT\",\"label\":\"Velo + AT\",\"short_name\":\"VEL\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"mod_edit_whl\",\"name\":\"Wheel\",\"label\":\"Wheel\",\"short_name\":\"WHL\",\"type\":\"float\",\"min\":-1,\"max\":1},"
        "{\"key\":\"mod_edit_kbd\",\"name\":\"Key Track\",\"label\":\"Key Track\",\"short_name\":\"KBD\",\"type\":\"float\",\"min\":-1,\"max\":1}]},"
        "\"lfo1_curve\":{\"label\":\"LFO 1 Curve\",\"knobs\":[\"lfo1_point1\",\"lfo1_point2\",\"lfo1_point3\",\"lfo1_point4\",\"lfo1_point5\",\"lfo1_point6\",\"lfo1_point7\",\"lfo1_point8\"],\"params\":[\"lfo1_point1\",\"lfo1_point2\",\"lfo1_point3\",\"lfo1_point4\",\"lfo1_point5\",\"lfo1_point6\",\"lfo1_point7\",\"lfo1_point8\"]},"
        "\"lfo2_curve\":{\"label\":\"LFO 2 Curve\",\"knobs\":[\"lfo2_point1\",\"lfo2_point2\",\"lfo2_point3\",\"lfo2_point4\",\"lfo2_point5\",\"lfo2_point6\",\"lfo2_point7\",\"lfo2_point8\"],\"params\":[\"lfo2_point1\",\"lfo2_point2\",\"lfo2_point3\",\"lfo2_point4\",\"lfo2_point5\",\"lfo2_point6\",\"lfo2_point7\",\"lfo2_point8\"]},"
        "\"macros\":{\"label\":\"Macros\",\"knobs\":[\"macro1\",\"macro2\",\"macro3\",\"macro4\",\"mod_wheel\"],\"params\":[\"macro1\",\"macro2\",\"macro3\",\"macro4\",\"mod_wheel\"]},"
        "\"modcol1\":{\"label\":\"Mod 1: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol1_dest\",\"modcol1_cyc\",\"modcol1_env\",\"modcol1_lfo1\",\"modcol1_lfo2\",\"modcol1_vel\",\"modcol1_whl\",\"modcol1_kbd\"],\"params\":[{\"key\":\"modcol1_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol1_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol1_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol1_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol1_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol1_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol1_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol1_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol2\":{\"label\":\"Mod 2: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol2_dest\",\"modcol2_cyc\",\"modcol2_env\",\"modcol2_lfo1\",\"modcol2_lfo2\",\"modcol2_vel\",\"modcol2_whl\",\"modcol2_kbd\"],\"params\":[{\"key\":\"modcol2_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol2_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol2_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol2_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol2_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol2_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol2_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol2_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol3\":{\"label\":\"Mod 3: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol3_dest\",\"modcol3_cyc\",\"modcol3_env\",\"modcol3_lfo1\",\"modcol3_lfo2\",\"modcol3_vel\",\"modcol3_whl\",\"modcol3_kbd\"],\"params\":[{\"key\":\"modcol3_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol3_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol3_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol3_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol3_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol3_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol3_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol3_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol4\":{\"label\":\"Mod 4: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol4_dest\",\"modcol4_cyc\",\"modcol4_env\",\"modcol4_lfo1\",\"modcol4_lfo2\",\"modcol4_vel\",\"modcol4_whl\",\"modcol4_kbd\"],\"params\":[{\"key\":\"modcol4_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol4_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol4_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol4_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol4_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol4_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol4_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol4_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol5\":{\"label\":\"Mod 5: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol5_dest\",\"modcol5_cyc\",\"modcol5_env\",\"modcol5_lfo1\",\"modcol5_lfo2\",\"modcol5_vel\",\"modcol5_whl\",\"modcol5_kbd\"],\"params\":[{\"key\":\"modcol5_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol5_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol5_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol5_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol5_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol5_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol5_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol5_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol6\":{\"label\":\"Mod 6: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol6_dest\",\"modcol6_cyc\",\"modcol6_env\",\"modcol6_lfo1\",\"modcol6_lfo2\",\"modcol6_vel\",\"modcol6_whl\",\"modcol6_kbd\"],\"params\":[{\"key\":\"modcol6_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol6_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol6_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol6_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol6_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol6_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol6_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol6_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol7\":{\"label\":\"Mod 7: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol7_dest\",\"modcol7_cyc\",\"modcol7_env\",\"modcol7_lfo1\",\"modcol7_lfo2\",\"modcol7_vel\",\"modcol7_whl\",\"modcol7_kbd\"],\"params\":[{\"key\":\"modcol7_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol7_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol7_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol7_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol7_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol7_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol7_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol7_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol8\":{\"label\":\"Mod 8: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol8_dest\",\"modcol8_cyc\",\"modcol8_env\",\"modcol8_lfo1\",\"modcol8_lfo2\",\"modcol8_vel\",\"modcol8_whl\",\"modcol8_kbd\"],\"params\":[{\"key\":\"modcol8_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol8_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol8_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol8_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol8_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol8_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol8_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol8_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol9\":{\"label\":\"Mod 9: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol9_dest\",\"modcol9_cyc\",\"modcol9_env\",\"modcol9_lfo1\",\"modcol9_lfo2\",\"modcol9_vel\",\"modcol9_whl\",\"modcol9_kbd\"],\"params\":[{\"key\":\"modcol9_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol9_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol9_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol9_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol9_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol9_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol9_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol9_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol10\":{\"label\":\"Mod 10: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol10_dest\",\"modcol10_cyc\",\"modcol10_env\",\"modcol10_lfo1\",\"modcol10_lfo2\",\"modcol10_vel\",\"modcol10_whl\",\"modcol10_kbd\"],\"params\":[{\"key\":\"modcol10_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol10_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol10_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol10_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol10_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol10_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol10_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol10_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol11\":{\"label\":\"Mod 11: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol11_dest\",\"modcol11_cyc\",\"modcol11_env\",\"modcol11_lfo1\",\"modcol11_lfo2\",\"modcol11_vel\",\"modcol11_whl\",\"modcol11_kbd\"],\"params\":[{\"key\":\"modcol11_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol11_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol11_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol11_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol11_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol11_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol11_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol11_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"modcol12\":{\"label\":\"Mod 12: %s\",\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999},\"knobs\":[\"modcol12_dest\",\"modcol12_cyc\",\"modcol12_env\",\"modcol12_lfo1\",\"modcol12_lfo2\",\"modcol12_vel\",\"modcol12_whl\",\"modcol12_kbd\"],\"params\":[{\"key\":\"modcol12_dest\",\"short_name\":\"DEST\",\"label\":\"Destination\"},{\"key\":\"modcol12_cyc\",\"short_name\":\"CYC\",\"label\":\"Cycling Env\"},{\"key\":\"modcol12_env\",\"short_name\":\"ENV\",\"label\":\"Envelope\"},{\"key\":\"modcol12_lfo1\",\"short_name\":\"LFO1\",\"label\":\"LFO 1\"},{\"key\":\"modcol12_lfo2\",\"short_name\":\"LFO2\",\"label\":\"LFO 2\"},{\"key\":\"modcol12_vel\",\"short_name\":\"VEL\",\"label\":\"Velo + AT\"},{\"key\":\"modcol12_whl\",\"short_name\":\"WHL\",\"label\":\"Wheel\"},{\"key\":\"modcol12_kbd\",\"short_name\":\"KBD\",\"label\":\"Key Track\"}]},"
        "\"macro1_slots\":{\"label\":\"Macro 1 Slots\",\"knobs\":[\"macro1_dest1\",\"macro1_amt1\",\"macro1_dest2\",\"macro1_amt2\",\"macro1_dest3\",\"macro1_amt3\",\"macro1_dest4\",\"macro1_amt4\"],\"params\":[{\"key\":\"macro1_dest1\",\"short_name\":\"D1\",\"label\":\"Socket 1\"},{\"key\":\"macro1_amt1\",\"short_name\":\"A1\",\"label\":\"Amount 1\"},{\"key\":\"macro1_dest2\",\"short_name\":\"D2\",\"label\":\"Socket 2\"},{\"key\":\"macro1_amt2\",\"short_name\":\"A2\",\"label\":\"Amount 2\"},{\"key\":\"macro1_dest3\",\"short_name\":\"D3\",\"label\":\"Socket 3\"},{\"key\":\"macro1_amt3\",\"short_name\":\"A3\",\"label\":\"Amount 3\"},{\"key\":\"macro1_dest4\",\"short_name\":\"D4\",\"label\":\"Socket 4\"},{\"key\":\"macro1_amt4\",\"short_name\":\"A4\",\"label\":\"Amount 4\"}]},"
        "\"macro2_slots\":{\"label\":\"Macro 2 Slots\",\"knobs\":[\"macro2_dest1\",\"macro2_amt1\",\"macro2_dest2\",\"macro2_amt2\",\"macro2_dest3\",\"macro2_amt3\",\"macro2_dest4\",\"macro2_amt4\"],\"params\":[{\"key\":\"macro2_dest1\",\"short_name\":\"D1\",\"label\":\"Socket 1\"},{\"key\":\"macro2_amt1\",\"short_name\":\"A1\",\"label\":\"Amount 1\"},{\"key\":\"macro2_dest2\",\"short_name\":\"D2\",\"label\":\"Socket 2\"},{\"key\":\"macro2_amt2\",\"short_name\":\"A2\",\"label\":\"Amount 2\"},{\"key\":\"macro2_dest3\",\"short_name\":\"D3\",\"label\":\"Socket 3\"},{\"key\":\"macro2_amt3\",\"short_name\":\"A3\",\"label\":\"Amount 3\"},{\"key\":\"macro2_dest4\",\"short_name\":\"D4\",\"label\":\"Socket 4\"},{\"key\":\"macro2_amt4\",\"short_name\":\"A4\",\"label\":\"Amount 4\"}]},"
        "\"macro3_slots\":{\"label\":\"Macro 3 Slots\",\"knobs\":[\"macro3_dest1\",\"macro3_amt1\",\"macro3_dest2\",\"macro3_amt2\",\"macro3_dest3\",\"macro3_amt3\",\"macro3_dest4\",\"macro3_amt4\"],\"params\":[{\"key\":\"macro3_dest1\",\"short_name\":\"D1\",\"label\":\"Socket 1\"},{\"key\":\"macro3_amt1\",\"short_name\":\"A1\",\"label\":\"Amount 1\"},{\"key\":\"macro3_dest2\",\"short_name\":\"D2\",\"label\":\"Socket 2\"},{\"key\":\"macro3_amt2\",\"short_name\":\"A2\",\"label\":\"Amount 2\"},{\"key\":\"macro3_dest3\",\"short_name\":\"D3\",\"label\":\"Socket 3\"},{\"key\":\"macro3_amt3\",\"short_name\":\"A3\",\"label\":\"Amount 3\"},{\"key\":\"macro3_dest4\",\"short_name\":\"D4\",\"label\":\"Socket 4\"},{\"key\":\"macro3_amt4\",\"short_name\":\"A4\",\"label\":\"Amount 4\"}]},"
        "\"macro4_slots\":{\"label\":\"Macro 4 Slots\",\"knobs\":[\"macro4_dest1\",\"macro4_amt1\",\"macro4_dest2\",\"macro4_amt2\",\"macro4_dest3\",\"macro4_amt3\",\"macro4_dest4\",\"macro4_amt4\"],\"params\":[{\"key\":\"macro4_dest1\",\"short_name\":\"D1\",\"label\":\"Socket 1\"},{\"key\":\"macro4_amt1\",\"short_name\":\"A1\",\"label\":\"Amount 1\"},{\"key\":\"macro4_dest2\",\"short_name\":\"D2\",\"label\":\"Socket 2\"},{\"key\":\"macro4_amt2\",\"short_name\":\"A2\",\"label\":\"Amount 2\"},{\"key\":\"macro4_dest3\",\"short_name\":\"D3\",\"label\":\"Socket 3\"},{\"key\":\"macro4_amt3\",\"short_name\":\"A3\",\"label\":\"Amount 3\"},{\"key\":\"macro4_dest4\",\"short_name\":\"D4\",\"label\":\"Socket 4\"},{\"key\":\"macro4_amt4\",\"short_name\":\"A4\",\"label\":\"Amount 4\"}]},"
        "\"voice\":{\"label\":\"Voice\",\"knobs\":[\"voice_mode\",\"glide\",\"glide_mode\",\"bend_range\",\"unison_voices\",\"unison_mode\",\"spread\",\"stereo\"],\"params\":[\"voice_mode\",\"glide\",\"glide_mode\",\"bend_range\",\"unison_voices\",\"unison_mode\",\"spread\",\"stereo\"]},"
        "\"chord\":{\"label\":\"Chord\",\"knobs\":[\"chord\",\"chord_notes\",\"chord_note1\",\"chord_note2\",\"chord_note3\",\"chord_note4\",\"chord_note5\"],\"params\":[\"chord\",\"chord_notes\",\"chord_note1\",\"chord_note2\",\"chord_note3\",\"chord_note4\",\"chord_note5\"]},"
        /* Set-and-forget controls, kept off the Voice page so it fits one page. */
        "\"settings\":{\"label\":\"Settings\",\"knobs\":[\"osc_phase\",\"control_rate\"],\"params\":[\"osc_phase\",\"control_rate\"]},"
        /* EQ and Peak EQ controls live on the slot's own page. They used to sit
         * on four global pages (EQ Bands 1-2, EQ Band 3, Peak EQ 1-2, Peak EQ 3)
         * that repeated the same three controls per slot, so the Move walk
         * showed seven effect pages for what is really three slots. The type
         * enum now gates the unused set out of the page, and the Remote UI
         * applies the same rule in updateEffectLabels(). */
        "\"effects\":{\"label\":\"Effect 1\",\"knobs\":[\"fx1_type\",\"fx1_subtype\",\"fx1_mix\",\"fx1_time\",\"fx1_feedback\",\"fx1_low\",\"fx1_mid\",\"fx1_high\",\"fx1_frequency\",\"fx1_gain\",\"fx1_width\"],"
        "\"params\":[\"fx1_type\",\"fx1_subtype\",\"fx1_mix\",\"fx1_time\",\"fx1_feedback\","
        "{\"key\":\"fx1_low\",\"short_name\":\"LOW\",\"label\":\"Low\",\"visible_if\":{\"param\":\"fx1_type\",\"equals\":\"3-Band EQ\"}},"
        "{\"key\":\"fx1_mid\",\"short_name\":\"MID\",\"label\":\"Mid\",\"visible_if\":{\"param\":\"fx1_type\",\"equals\":\"3-Band EQ\"}},"
        "{\"key\":\"fx1_high\",\"short_name\":\"HIGH\",\"label\":\"High\",\"visible_if\":{\"param\":\"fx1_type\",\"equals\":\"3-Band EQ\"}},"
        "{\"key\":\"fx1_frequency\",\"short_name\":\"FREQ\",\"label\":\"Peak Freq\",\"visible_if\":{\"param\":\"fx1_type\",\"equals\":\"Peak EQ\"}},"
        "{\"key\":\"fx1_gain\",\"short_name\":\"GAIN\",\"label\":\"Peak Gain\",\"visible_if\":{\"param\":\"fx1_type\",\"equals\":\"Peak EQ\"}},"
        "{\"key\":\"fx1_width\",\"short_name\":\"WIDTH\",\"label\":\"Peak Width\",\"visible_if\":{\"param\":\"fx1_type\",\"equals\":\"Peak EQ\"}}]},"
        "\"effect2\":{\"label\":\"Effect 2\",\"knobs\":[\"fx2_type\",\"fx2_subtype\",\"fx2_mix\",\"fx2_time\",\"fx2_feedback\",\"fx2_low\",\"fx2_mid\",\"fx2_high\",\"fx2_frequency\",\"fx2_gain\",\"fx2_width\"],"
        "\"params\":[\"fx2_type\",\"fx2_subtype\",\"fx2_mix\",\"fx2_time\",\"fx2_feedback\","
        "{\"key\":\"fx2_low\",\"short_name\":\"LOW\",\"label\":\"Low\",\"visible_if\":{\"param\":\"fx2_type\",\"equals\":\"3-Band EQ\"}},"
        "{\"key\":\"fx2_mid\",\"short_name\":\"MID\",\"label\":\"Mid\",\"visible_if\":{\"param\":\"fx2_type\",\"equals\":\"3-Band EQ\"}},"
        "{\"key\":\"fx2_high\",\"short_name\":\"HIGH\",\"label\":\"High\",\"visible_if\":{\"param\":\"fx2_type\",\"equals\":\"3-Band EQ\"}},"
        "{\"key\":\"fx2_frequency\",\"short_name\":\"FREQ\",\"label\":\"Peak Freq\",\"visible_if\":{\"param\":\"fx2_type\",\"equals\":\"Peak EQ\"}},"
        "{\"key\":\"fx2_gain\",\"short_name\":\"GAIN\",\"label\":\"Peak Gain\",\"visible_if\":{\"param\":\"fx2_type\",\"equals\":\"Peak EQ\"}},"
        "{\"key\":\"fx2_width\",\"short_name\":\"WIDTH\",\"label\":\"Peak Width\",\"visible_if\":{\"param\":\"fx2_type\",\"equals\":\"Peak EQ\"}}]},"
        "\"effect3\":{\"label\":\"Effect 3\",\"knobs\":[\"fx3_type\",\"fx3_subtype\",\"fx3_mix\",\"fx3_time\",\"fx3_feedback\",\"fx3_low\",\"fx3_mid\",\"fx3_high\",\"fx3_frequency\",\"fx3_gain\",\"fx3_width\"],"
        "\"params\":[\"fx3_type\",\"fx3_subtype\",\"fx3_mix\",\"fx3_time\",\"fx3_feedback\","
        "{\"key\":\"fx3_low\",\"short_name\":\"LOW\",\"label\":\"Low\",\"visible_if\":{\"param\":\"fx3_type\",\"equals\":\"3-Band EQ\"}},"
        "{\"key\":\"fx3_mid\",\"short_name\":\"MID\",\"label\":\"Mid\",\"visible_if\":{\"param\":\"fx3_type\",\"equals\":\"3-Band EQ\"}},"
        "{\"key\":\"fx3_high\",\"short_name\":\"HIGH\",\"label\":\"High\",\"visible_if\":{\"param\":\"fx3_type\",\"equals\":\"3-Band EQ\"}},"
        "{\"key\":\"fx3_frequency\",\"short_name\":\"FREQ\",\"label\":\"Peak Freq\",\"visible_if\":{\"param\":\"fx3_type\",\"equals\":\"Peak EQ\"}},"
        "{\"key\":\"fx3_gain\",\"short_name\":\"GAIN\",\"label\":\"Peak Gain\",\"visible_if\":{\"param\":\"fx3_type\",\"equals\":\"Peak EQ\"}},"
        "{\"key\":\"fx3_width\",\"short_name\":\"WIDTH\",\"label\":\"Peak Width\",\"visible_if\":{\"param\":\"fx3_type\",\"equals\":\"Peak EQ\"}}]},"
        "\"factory\":{\"label\":\"Factory Patches\",\"list_param\":\"preset\",\"count_param\":\"preset_count\",\"name_param\":\"preset_name\",\"navigate_to\":\"root\"}"
        "}}",
        r1->wave_short, r1->wave_label,
        r1->timbre_short, r1->timbre_label,
        r1->shape_short, r1->shape_label,
        r1->level_short, r1->level_label,
        r2->wave_short, r2->wave_label,
        r2->timbre_short, r2->timbre_label,
        r2->shape_short, r2->shape_label,
        r2->level_short, r2->level_label,
        g_mod_destinations[clamp_enum((float)inst->mod_dest[0], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[1], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[2], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[3], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[4], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[5], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[6], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[7], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[8], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[9], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[10], DEST_COUNT)],
        g_mod_destinations[clamp_enum((float)inst->mod_dest[11], DEST_COUNT)]);
    return n >= 0 && n < buf_len ? n : -1;
}

static int parse_enum(const char *value, const char *const *labels, int count) {
    if (!value) return 0;
    for (int i = 0; i < count; ++i) {
        if (strcmp(value, labels[i]) == 0) return i;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) return 0;
    if (parsed < 0) return 0;
    if (parsed >= count) return count - 1;
    return (int)parsed;
}

static int parse_wavetable_bank(const char *value) {
    if (value && strcmp(value, "Odd/Evn") == 0) return 1;
    if (value && strcmp(value, "Bell") == 0) return 2;
    if (value && strcmp(value, "Formant") == 0) return 3;
    return parse_enum(value, g_wavetable_names, SF_WT_FAMILY_COUNT);
}

static int parse_osc1_type(const char *value) {
    if (value && strcmp(value, "VA") == 0) return OSC_VA;
    if (value && strcmp(value, "FM") == 0) return OSC_FM;
    if (value && strcmp(value, "Fold") == 0) return OSC_FOLD;
    return parse_enum(value, g_osc1_modes, SF_OSC1_MODE_COUNT);
}

static int parse_osc2_type(const char *value) {
    if (value && strcmp(value, "VA") == 0) return ENGINE_VA;
    if (value && strcmp(value, "FM") == 0) return ENGINE_FM_RM;
    if (value && strcmp(value, "Fold") == 0) return ENGINE_FOLD;
    return parse_enum(value, g_osc2_modes, SF_OSC2_MODE_COUNT);
}

static int parse_fx_type(const char *value) {
    if (value && strcmp(value, "EQ") == 0) return FX_EQ;
    return parse_enum(value, g_fx_types, FX_TYPE_COUNT);
}

static int parse_fx_param_key(const char *key, int *slot, int *field) {
    if (!key || key[0] != 'f' || key[1] != 'x'
        || key[2] < '1' || key[2] > '3' || key[3] != '_') return 0;
    int parsed_slot = key[2] - '1';
    const char *suffix = key + 4;
    int parsed_field;
    if (strcmp(suffix, "type") == 0) parsed_field = 0;
    else if (strcmp(suffix, "mix") == 0) parsed_field = 1;
    else if (strcmp(suffix, "time") == 0) parsed_field = 2;
    else if (strcmp(suffix, "feedback") == 0) parsed_field = 3;
    else if (strcmp(suffix, "low") == 0) parsed_field = 4;
    else if (strcmp(suffix, "mid") == 0) parsed_field = 5;
    else if (strcmp(suffix, "high") == 0) parsed_field = 6;
    else if (strcmp(suffix, "subtype") == 0) parsed_field = 7;
    else if (strcmp(suffix, "frequency") == 0) parsed_field = 8;
    else if (strcmp(suffix, "gain") == 0) parsed_field = 9;
    else if (strcmp(suffix, "width") == 0) parsed_field = 10;
    else return 0;
    if (slot) *slot = parsed_slot;
    if (field) *field = parsed_field;
    return 1;
}

/* Grid keys are generated rather than written out: modcol<1-12>_dest,
 * modcol<1-12>_<slot>, macro<1-4>_dest<1-4> and macro<1-4>_amt<1-4>. */
static int parse_mod_column_key(const char *key, int *column, int *source, int *is_dest) {
    if (!key || strncmp(key, "modcol", 6) != 0) return 0;
    const char *p = key + 6;
    int index = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 2) {
        index = index * 10 + (*p - '0');
        ++p;
        ++digits;
    }
    if (digits == 0 || *p != '_' || index < 1 || index > SF_MOD_COLUMNS) return 0;
    ++p;
    *column = index - 1;
    if (strcmp(p, "dest") == 0) {
        *is_dest = 1;
        *source = 0;
        return 1;
    }
    for (int slot = 0; slot < MODSRC_COUNT; ++slot) {
        if (strcmp(p, g_mod_slot_suffix[slot]) == 0) {
            *is_dest = 0;
            *source = slot;
            return 1;
        }
    }
    return 0;
}

/* The Move matrix editor addresses the selected column through aliases:
 * mod_edit_dest plus the same seven source suffixes the grid uses. The
 * selected column itself is mod_edit_col. */
static int parse_mod_edit_key(const char *key, int *source, int *is_dest) {
    if (!key || strncmp(key, "mod_edit_", 9) != 0) return 0;
    const char *p = key + 9;
    if (strcmp(p, "dest") == 0) {
        *is_dest = 1;
        *source = 0;
        return 1;
    }
    for (int slot = 0; slot < MODSRC_COUNT; ++slot) {
        if (strcmp(p, g_mod_slot_suffix[slot]) == 0) {
            *is_dest = 0;
            *source = slot;
            return 1;
        }
    }
    return 0;
}

static int parse_macro_slot_key(const char *key, int *macro, int *slot, int *is_dest) {
    if (!key || strncmp(key, "macro", 5) != 0) return 0;
    const char *p = key + 5;
    if (*p < '1' || *p > '4' || p[1] != '_') return 0;
    int index = *p - '1';
    const char *field = p + 2;
    int dest_field;
    if (strncmp(field, "dest", 4) == 0) {
        dest_field = 1;
        field += 4;
    } else if (strncmp(field, "amt", 3) == 0) {
        dest_field = 0;
        field += 3;
    } else {
        return 0;
    }
    if (*field < '1' || *field > '4' || field[1] != '\0') return 0;
    *macro = index;
    *slot = *field - '1';
    *is_dest = dest_field;
    return 1;
}

static int parse_lfo_curve_key(const char *key, int *lfo, int *point) {
    if (!key || key[0] != 'l' || key[1] != 'f' || key[2] != 'o'
        || (key[3] != '1' && key[3] != '2') || key[4] != '_'
        || strncmp(key + 5, "point", 5) != 0 || key[10] < '1' || key[10] > '8'
        || key[11] != '\0') return 0;
    if (lfo) *lfo = key[3] - '1';
    if (point) *point = key[10] - '1';
    return 1;
}

/* Extended Shaper keys: lfoN_shp_steps, lfoN_shp_mode (SHAPER_*), and
 * lfoN_shp_{amp,curve,slope,on}K for step K = 1..16. They are state-only (not
 * in the Move parameter grid); the first eight amplitudes are also the
 * lfoN_point1..8 knobs. `field` is 0 for steps, then 1 amp, 2 curve, 3 slope,
 * 4 on, and 5 for mode. */
static const char *const g_shaper_fields[4] = {"amp", "curve", "slope", "on"};

static int parse_shaper_ext_key(const char *key, int *lfo, int *field, int *step) {
    if (!key || strncmp(key, "lfo", 3) != 0 || (key[3] != '1' && key[3] != '2')
        || strncmp(key + 4, "_shp_", 5) != 0) return 0;
    const char *rest = key + 9;
    if (lfo) *lfo = key[3] - '1';
    if (strcmp(rest, "steps") == 0) { if (field) *field = 0; if (step) *step = 0; return 1; }
    if (strcmp(rest, "mode") == 0) { if (field) *field = 5; if (step) *step = 0; return 1; }
    for (int f = 0; f < 4; ++f) {
        size_t n = strlen(g_shaper_fields[f]);
        if (strncmp(rest, g_shaper_fields[f], n) != 0) continue;
        char *end;
        long k = strtol(rest + n, &end, 10);
        if (end == rest + n || *end != '\0' || k < 1 || k > SF_SHAPER_STEPS) return 0;
        if (field) *field = f + 1;
        if (step) *step = (int)k - 1;
        return 1;
    }
    return 0;
}

static sf_shaper_t *shaper_for(sf_instance_t *inst, int lfo) {
    return lfo == 0 ? &inst->lfo1_shaper : &inst->lfo2_shaper;
}

static int parse_macro_key(const char *key, int *macro) {
    if (!key || key[0] != 'm' || key[1] != 'a' || key[2] != 'c'
        || key[3] != 'r' || key[4] != 'o' || key[5] < '1' || key[5] > '4'
        || key[6] != '\0') return 0;
    if (macro) *macro = key[5] - '1';
    return 1;
}

static float octave_ratio_approx(float octaves) {
    float x = sf_clampf(octaves, -10.0f, 10.0f);
    int ipart = (int)floorf(x);
    float f = x - (float)ipart;
    float poly = 1.0f + f * (0.69314718f + f * (0.24022651f
               + f * (0.05550411f + f * 0.00961813f)));
    /* 2^ipart built in the exponent bits: ldexpf can be a libm call. */
    uint32_t bits = (uint32_t)(ipart + 127) << 23;
    float scale;
    memcpy(&scale, &bits, sizeof(scale));
    return poly * scale;
}

static float semitone_ratio_approx(float semitones) {
    return octave_ratio_approx(semitones * (1.0f / 12.0f));
}

/* Mod Quantize: one bit per pitch class. Index 0 is Continuous and never
 * reaches this table, so it stays zero. The order follows the MiniFreak menu. */
static const unsigned g_scale_masks[11] = {
    0u, 0xFFFu, 0x001u, 0x081u, 0x5ADu, 0xAB5u, 0x5B3u,
    0x48Du, 0x895u, 0x4A9u, 0x295u
};

/* Snaps a smooth pitch-modulation offset to the nearest note of a scale.
 * Quantizing the offset (rather than the played note) keeps the keyboard
 * itself continuous and is what the hardware's Mod Quantize does. */
static float quantize_pitch_mod(float semitones, int mode) {
    if (mode <= 0 || mode > 10) return semitones;
    unsigned mask = g_scale_masks[mode];
    float base = floorf(semitones);
    int best = (int)base;
    float best_dist = fabsf(semitones - (float)best);
    /* Every scale holds its root, so the nearest note is at most 6 semitones
     * away: nothing outside base - 6 .. base + 7 can win. */
    for (int offset = -6; offset <= 7; ++offset) {
        int candidate = (int)base + offset;
        int pitch_class = candidate % 12;
        if (pitch_class < 0) pitch_class += 12;
        if (!(mask & (1u << pitch_class))) continue;
        float distance = fabsf(semitones - (float)candidate);
        if (distance < best_dist) {
            best_dist = distance;
            best = candidate;
        }
    }
    return (float)best;
}

/* w = pi * f / fs, the argument of the filter's tan(w) coefficient. */
static float fast_svf_w(float cutoff_hz) {
    return SF_PI * sf_clampf(cutoff_hz, 20.0f, 18000.0f) / SF_SAMPLE_RATE;
}

static float fast_svf_g_from_w(float w) {
    float w2 = w * w;
    return w * (1.0f + w2 * (0.33333333f + w2 * 0.13333333f));
}

static float fast_svf_g(float cutoff_hz) {
    return fast_svf_g_from_w(fast_svf_w(cutoff_hz));
}

#define SF_FM_W_MIN (SF_PI * SF_CUTOFF_MIN_HZ / SF_SAMPLE_RATE)
#define SF_FM_W_MAX (SF_PI * SF_CUTOFF_MAX_HZ / SF_SAMPLE_RATE)

/* One step of the Filter FM filter, which runs at twice the sample rate: the
 * cutoff (w, at the base rate) is scaled in octaves by the source, clamped to
 * the knob's range, then halved for the doubled rate. */
static float fm_filter_step(sf_filter_state_t *state, int mode, float w,
                            float octaves, float resonance, float input) {
    float wm = sf_clampf(w * octave_ratio_approx(octaves), SF_FM_W_MIN, SF_FM_W_MAX);
    return sf_filter_process(state, mode, fast_svf_g_from_w(0.5f * wm), resonance, input);
}

static void chord_reset(sf_instance_t *inst) {
    static const int offsets[SF_CHORD_MAX_NOTES] = {4, 7, 12, 16, 19};
    inst->chord = 0;
    inst->chord_notes = 2;
    for (int i = 0; i < SF_CHORD_MAX_NOTES; ++i) inst->chord_offset[i] = offsets[i];
}

/* The notes one key plays: the key itself, then the Keyboard Chord's notes. */
static int key_notes(const sf_instance_t *inst, int key, int notes[SF_MAX_VOICES]) {
    int count = 1;
    notes[0] = key;
    if (inst->chord)
        for (int i = 0; i < inst->chord_notes && count < SF_MAX_VOICES; ++i) {
            int note = key + inst->chord_offset[i];
            notes[count++] = note < 0 ? 0 : note > 127 ? 127 : note;
        }
    return count;
}

/* Mono, and Unison in its Mono mode, play one stack that follows the last
 * held key. */
static int held_stack_mode(const sf_instance_t *inst) {
    return inst->voice_mode == VOICE_MONO
        || (inst->voice_mode == VOICE_UNISON && inst->unison_mode == 0);
}

static void held_clear(sf_instance_t *inst) {
    inst->held_count = 0;
    inst->held_mode = inst->voice_mode;
}

static void held_sync_mode(sf_instance_t *inst) {
    if (inst->held_mode != inst->voice_mode) held_clear(inst);
}

static void held_remove(sf_instance_t *inst, int note, int channel) {
    int j = 0;
    for (int i = 0; i < inst->held_count; ++i) {
        if (inst->held_note[i] == note && inst->held_chan[i] == channel) continue;
        inst->held_note[j] = inst->held_note[i];
        inst->held_vel[j] = inst->held_vel[i];
        inst->held_chan[j] = inst->held_chan[i];
        ++j;
    }
    inst->held_count = j;
}

static void start_note_voices(sf_instance_t *inst, int note, int velocity, int channel, int force_legato) {
    inst->last_note = note;
    /* Mono Kbd restarts the shared LFO on every note, Legato Kbd only on a
     * note played with nothing else held. */
    int any_gated = 0;
    for (int i = 0; i < SF_MAX_VOICES; ++i)
        if (inst->voices[i].active && inst->voices[i].env_stage != ENV_RELEASE) any_gated = 1;
    if (inst->lfo1_retrig == LFO_RETRIG_MONO
        || (inst->lfo1_retrig == LFO_RETRIG_LEGATO && !any_gated))
        inst->lfo1_phase_global = 0.0f;
    if (inst->lfo2_retrig == LFO_RETRIG_MONO
        || (inst->lfo2_retrig == LFO_RETRIG_LEGATO && !any_gated))
        inst->lfo2_phase_global = 0.0f;
    sf_voice_start_params_t params = {
        inst->voice_mode, inst->channel_pressure[channel], inst->spread_cents,
        inst->cycle_mode, inst->osc_phase,
        inst->lfo1_retrig, inst->lfo2_retrig, inst->env_retrig, &inst->age_counter,
        inst->unison_voices, 0u, note
    };
    int notes[SF_MAX_VOICES];
    int count = key_notes(inst, note, notes);
    if (inst->voice_mode == VOICE_MONO) {
        /* Legato only while another key is still held: a voice left sounding
         * by a different voice mode or patch is not a held note. A chord
         * plays on the first voices, one note each (measured: Mono plays
         * every chord note). */
        int legato = inst->voices[0].active && inst->voices[0].env_stage != ENV_RELEASE
                  && (inst->held_count > 1 || force_legato);
        for (int i = 0; i < count; ++i) {
            int voice_legato = legato && inst->voices[i].active
                            && inst->voices[i].env_stage != ENV_RELEASE;
            sf_voice_start(&inst->voices[i], i, notes[i], velocity, channel, voice_legato, &params);
        }
        return;
    }
    if (inst->voice_mode == VOICE_UNISON) {
        params.lfo_seed = (inst->age_counter + 1u) * 2654435761u | 1u;
        /* Measured on MiniFreak V: a chord takes all six voices whatever the
         * Count, and deals them out in slot order, rank r on chord note
         * r mod notes, each keeping its own unison offset. */
        int layout = count > 1 && inst->unison_mode == 0 ? SF_MAX_VOICES : inst->unison_voices;
        if (layout < count) layout = count;
        if (layout > SF_MAX_VOICES) layout = SF_MAX_VOICES;
        params.unison_voices = layout;
        int stack = layout;
        if (inst->unison_mode != 0) {
            /* Poly: a key played while others are held takes only its share
             * of the voices (measured: two held keys at Count 6 sound three
             * each), unless enough voices are free anyway. */
            int keys = 1, idle = 0;
            for (int i = 0; i < SF_MAX_VOICES; ++i) {
                const sf_voice_t *v = &inst->voices[i];
                if (!v->active || v->env_stage == ENV_RELEASE) { ++idle; continue; }
                int seen = 0;
                for (int j = 0; j < i; ++j) {
                    const sf_voice_t *w = &inst->voices[j];
                    if (w->active && w->env_stage != ENV_RELEASE
                        && w->key == v->key && w->channel == v->channel) seen = 1;
                }
                keys += !seen;
            }
            int share = SF_MAX_VOICES / keys;
            if (share < idle) share = idle;
            if (share < count) share = count;
            if (stack > share) stack = share;
        }
        for (int i = 0; i < stack; ++i) {
            int voice_note = notes[sf_unison_rank(i, layout) % count];
            if (inst->unison_mode != 0) {
                /* Poly: every key takes a stack of its own from the pool. */
                sf_voice_start(sf_voice_allocate(inst->voices), i, voice_note,
                               velocity, channel, 0, &params);
                continue;
            }
            int legato = inst->voices[i].active && inst->voices[i].note == voice_note
                      && inst->voices[i].env_stage != ENV_RELEASE;
            if (force_legato)
                legato = inst->voices[i].active && inst->voices[i].env_stage != ENV_RELEASE;
            sf_voice_start(&inst->voices[i], i, voice_note, velocity, channel, legato, &params);
        }
        return;
    }
    for (int i = 0; i < count; ++i) {
        sf_voice_t *voice = sf_voice_allocate(inst->voices);
        int index = (int)(voice - inst->voices);
        sf_voice_start(voice, index, notes[i], velocity, channel, 0, &params);
    }
}

static void start_note(sf_instance_t *inst, int note, int velocity, int channel) {
    if (held_stack_mode(inst)) {
        held_sync_mode(inst);
        held_remove(inst, note, channel);
        if (inst->held_count >= 16) {
            for (int i = 1; i < 16; ++i) {
                inst->held_note[i - 1] = inst->held_note[i];
                inst->held_vel[i - 1] = inst->held_vel[i];
                inst->held_chan[i - 1] = inst->held_chan[i];
            }
            inst->held_count = 15;
        }
        inst->held_note[inst->held_count] = (uint8_t)note;
        inst->held_vel[inst->held_count] = (uint8_t)velocity;
        inst->held_chan[inst->held_count] = (uint8_t)channel;
        inst->held_count++;
    }
    start_note_voices(inst, note, velocity, channel, 0);
}

/* A note-off. A key (from MIDI) honours the sustain pedal; a note the
 * sequencer ends does not, so a held pedal never stretches its gate. */
static void release_note_ex(sf_instance_t *inst, int note, int channel, int key) {
    if (held_stack_mode(inst)) {
        held_sync_mode(inst);
        held_remove(inst, note, channel);
        if (inst->held_count > 0) {
            int top = inst->held_count - 1;
            const sf_voice_t *v = &inst->voices[0];
            if (v->active && v->key == note && v->channel == channel) {
                start_note_voices(inst, inst->held_note[top], inst->held_vel[top],
                                  inst->held_chan[top], 1);
            }
            return;
        }
    }
    if (key && inst->sustain_down)
        sf_voice_sustain_note(inst->voices, note, channel);
    else
        sf_voice_release_note(inst->voices, note, channel, inst->release_ms,
                              inst->vel_time, inst->release_curve);
}

static void release_note(sf_instance_t *inst, int note, int channel) {
    release_note_ex(inst, note, channel, 0);
}

static void release_channel(sf_instance_t *inst, int channel, int immediate) {
    held_clear(inst);
    sf_voice_release_channel(inst->voices, channel, immediate, inst->release_ms,
                             inst->vel_time, inst->release_curve);
}

/* The sequencer takes over the keyboard: held keys feed it instead of the
 * voices, and it plays the voices through start_note/release_note. Any change
 * of ownership releases what is sounding so no note can be left hanging. */
static void seq_flush(sf_instance_t *inst) {
    sf_seq_event_t events[SF_SEQ_EVENTS];
    int count = sf_seq_panic(&inst->seq, events, SF_SEQ_EVENTS);
    memset(inst->seq_sustained, 0, sizeof(inst->seq_sustained));
    for (int i = 0; i < count; ++i) release_note(inst, events[i].note, 0);
    release_channel(inst, -1, 0);
}

static void sustain_pedal(sf_instance_t *inst, int down) {
    down = down ? 1 : 0;
    if (down == inst->sustain_down) return;
    inst->sustain_down = down;
    if (down) return;
    for (int word = 0; word < 4; ++word) {
        uint32_t bits = inst->seq_sustained[word];
        inst->seq_sustained[word] = 0;
        for (int bit = 0; bits; ++bit, bits >>= 1)
            if (bits & 1u) sf_seq_key_up(&inst->seq, word * 32 + bit);
    }
    sf_voice_release_sustained(inst->voices, inst->release_ms, inst->vel_time,
                               inst->release_curve);
}

static void seq_set_mode(sf_instance_t *inst, int mode) {
    mode = (int)clamp_enum((float)mode, 3);
    if (mode == inst->seq.mode) return;
    seq_flush(inst);
    inst->seq.mode = mode;
}

static void seq_reset_patch(sf_instance_t *inst) {
    if (inst->seq.mode != SEQ_MODE_OFF) seq_flush(inst);
    sf_seq_init(&inst->seq);
}

static int seq_has_content(const sf_instance_t *inst) {
    if (inst->seq.mode != SEQ_MODE_OFF) return 1;
    for (int i = 0; i < SF_SEQ_MAX_STEPS; ++i)
        if (inst->seq.steps[i].count) return 1;
    for (int lane = 0; lane < SF_SEQ_LANES; ++lane)
        if (sf_seq_lane_in_use(&inst->seq, lane)) return 1;
    return 0;
}

/* Step-automation lane keys: seq_lane<1..4>_dest, _smooth and _data. Like the
 * other seq_* keys they live in the state only, not on the parameter grid. */
enum { SEQ_LANE_DEST = 0, SEQ_LANE_SMOOTH = 1, SEQ_LANE_DATA = 2 };
static int parse_seq_lane_key(const char *key, int *lane, int *field) {
    if (strncmp(key, "seq_lane", 8) != 0) return 0;
    if (key[8] < '1' || key[8] > '0' + SF_SEQ_LANES || key[9] != '_') return 0;
    *lane = key[8] - '1';
    const char *name = key + 10;
    if (strcmp(name, "dest") == 0) *field = SEQ_LANE_DEST;
    else if (strcmp(name, "smooth") == 0) *field = SEQ_LANE_SMOOTH;
    else if (strcmp(name, "data") == 0) *field = SEQ_LANE_DATA;
    else return 0;
    return 1;
}

static void mark_contract_dirty(sf_instance_t *inst) {
    if (!inst) return;
    inst->contract_dirty_blocks = 24;
    inst->contract_was_dirty = 1;
}

/* The preset table is Bizar's own bank followed by any imported MiniFreak
 * bank, so one index addresses both without either list growing a hole. */
static const char *factory_preset_name(int index) {
    if (index < 0) return "?";
    if (index < SF_ORIGINAL_PRESET_COUNT) return g_factory_names[index];
#if SF_MF_PRESET_COUNT > 0
    index -= SF_ORIGINAL_PRESET_COUNT;
    if (index < SF_MF_PRESET_COUNT) return sf_mf_preset_names[index];
#endif
    return "?";
}

static const char *factory_preset_state(int index) {
    if (index < 0) return NULL;
    if (index < SF_ORIGINAL_PRESET_COUNT) return g_factory_states[index];
#if SF_MF_PRESET_COUNT > 0
    index -= SF_ORIGINAL_PRESET_COUNT;
    if (index < SF_MF_PRESET_COUNT) return sf_mf_preset_states[index];
#endif
    return NULL;
}

/* The Cycling Envelope's third knob is Sustain in Env mode and Hold time in
 * Run/Loop. Keep its physical 0..1 knob position stable when changing modes. */
static void set_cycle_mode(sf_instance_t *inst, int mode) {
    float knob = inst->cycle_mode == CYCLE_ENV
        ? sf_clampf(inst->cycle_hold_ms, 0.0f, 1.0f)
        : cbrtf(sf_clampf(inst->cycle_hold_ms / 10000.0f, 0.0f, 1.0f));
    inst->cycle_mode = (int)clamp_enum((float)mode, 3);
    inst->cycle_hold_ms = inst->cycle_mode == CYCLE_ENV
        ? knob : 10000.0f * knob * knob * knob;
}

static void set_numeric_param(sf_instance_t *inst, const char *key, float value) {
    if (strcmp(key, "osc1_mode") == 0) {
        int next = (int)clamp_enum(value, SF_OSC1_MODE_COUNT);
        if (next != inst->osc1_mode) { inst->osc1_mode = next; mark_contract_dirty(inst); }
    }
    else if (strcmp(key, "osc1_wave") == 0) inst->osc1_wave = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "osc1_timbre") == 0) inst->osc1_timbre = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "osc1_table") == 0) inst->osc1_table = (int)clamp_enum(value, SF_WT_FAMILY_COUNT);
    else if (strcmp(key, "osc1_position") == 0) inst->osc1_wave = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "osc1_shape") == 0) inst->osc1_shape = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "osc1_tune") == 0) inst->osc1_tune = sf_clampf(value, -48.5f, 48.5f);
    else if (strcmp(key, "osc1_quantize") == 0) inst->osc1_quantize = (int)clamp_enum(value, 11);
    else if (strcmp(key, "osc1_level") == 0) inst->osc1_level = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "osc2_mode") == 0) {
        int next = (int)clamp_enum(value, SF_OSC2_MODE_COUNT);
        if (next != inst->osc2_mode) { inst->osc2_mode = next; mark_contract_dirty(inst); }
    }
    else if (strcmp(key, "osc2_wave") == 0) inst->osc2_wave = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "osc2_timbre") == 0) inst->osc2_timbre = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "osc2_table") == 0) inst->osc2_table = (int)clamp_enum(value, SF_WT_FAMILY_COUNT);
    else if (strcmp(key, "osc2_position") == 0) inst->osc2_wave = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "osc2_shape") == 0) inst->osc2_shape = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "osc2_tune") == 0) inst->osc2_tune = sf_clampf(value, -48.5f, 48.5f);
    else if (strcmp(key, "osc2_quantize") == 0) inst->osc2_quantize = (int)clamp_enum(value, 11);
    else if (strcmp(key, "osc2_level") == 0) inst->osc2_level = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "mix") == 0) inst->mix = sf_clampf(value, 0.0f, 1.0f);
    /* Legacy state and automation stored cutoff in Hz. The new domain is 0..1,
     * so anything above it can only be Hz and is converted; a legacy value that
     * happens to sit inside 0..1 cannot be told apart and is read as the new
     * scale. apply_state_json converts exactly, using the state version. */
    else if (strcmp(key, "cutoff") == 0) {
        inst->cutoff = value > SF_CUTOFF_PARAM_MAX
            ? cutoff_param_from_hz(value)
            : sf_clampf(value, 0.0f, SF_CUTOFF_PARAM_MAX);
    }
    else if (strcmp(key, "resonance") == 0) inst->resonance = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "filter_mode") == 0) inst->filter_mode = (int)clamp_enum(value, 3);
    else if (strcmp(key, "filter_env") == 0) inst->filter_env = sf_clampf(value, -1.0f, 1.0f);
    else if (strcmp(key, "filter_fm_src") == 0) inst->filter_fm_src = (int)clamp_enum(value, 3);
    else if (strcmp(key, "filter_fm") == 0) inst->filter_fm = isfinite(value) ? sf_clampf(value, -1.0f, 1.0f) : 0.0f;
    else if (strcmp(key, "attack") == 0) inst->attack_ms = sf_clampf(value, 1.0f, SF_ENV_MAX_MS);
    else if (strcmp(key, "decay") == 0) inst->decay_ms = sf_clampf(value, 1.0f, SF_ENV_MAX_MS);
    else if (strcmp(key, "sustain") == 0) inst->sustain = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "release") == 0) inst->release_ms = sf_clampf(value, 1.0f, SF_ENV_MAX_MS);
    else if (strcmp(key, "attack_curve") == 0) inst->attack_curve = (int)clamp_enum(value, 2);
    else if (strcmp(key, "decay_curve") == 0) inst->decay_curve = (int)clamp_enum(value, 2);
    else if (strcmp(key, "release_curve") == 0) inst->release_curve = (int)clamp_enum(value, 2);
    else if (strcmp(key, "env_retrig") == 0) inst->env_retrig = (int)clamp_enum(value, 2);
    else if (strcmp(key, "vel_vca") == 0) inst->vel_vca = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "vel_vcf") == 0) inst->vel_vcf = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "vel_env") == 0) inst->vel_env = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "vel_time") == 0) inst->vel_time = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "velo_source") == 0) inst->velo_source = (int)clamp_enum(value, 3);
    else if (strcmp(key, "kbd_source") == 0) inst->kbd_source = (int)clamp_enum(value, KBD_SOURCE_COUNT);
    else if (strcmp(key, "lfo1_rate") == 0) inst->lfo1_rate = sf_clampf(value, 0.01f, SF_LFO_MAX_HZ);
    else if (strcmp(key, "lfo1_depth") == 0) inst->lfo1_depth = sf_clampf(value, 0.0f, 12.0f);
    else if (strcmp(key, "lfo1_shape") == 0) inst->lfo1_shape = (int)clamp_enum(value, LFO_SHAPE_COUNT);
    else if (strcmp(key, "lfo2_rate") == 0) inst->lfo2_rate = sf_clampf(value, 0.01f, SF_LFO_MAX_HZ);
    else if (strcmp(key, "lfo2_depth") == 0) inst->lfo2_depth = sf_clampf(value, -1.0f, 1.0f);
    else if (strcmp(key, "lfo2_shape") == 0) inst->lfo2_shape = (int)clamp_enum(value, LFO_SHAPE_COUNT);
    else if (strcmp(key, "lfo1_sync") == 0) inst->lfo1_sync = (int)clamp_enum(value, 2);
    else if (strcmp(key, "lfo1_div") == 0) inst->lfo1_div = (int)clamp_enum(value, SF_LFO_DIV_COUNT);
    else if (strcmp(key, "lfo1_retrig") == 0) inst->lfo1_retrig = (int)clamp_enum(value, LFO_RETRIG_COUNT);
    else if (strcmp(key, "lfo2_sync") == 0) inst->lfo2_sync = (int)clamp_enum(value, 2);
    else if (strcmp(key, "lfo2_div") == 0) inst->lfo2_div = (int)clamp_enum(value, SF_LFO_DIV_COUNT);
    else if (strcmp(key, "lfo2_retrig") == 0) inst->lfo2_retrig = (int)clamp_enum(value, LFO_RETRIG_COUNT);
    else if (strcmp(key, "osc_phase") == 0) inst->osc_phase = (int)clamp_enum(value, 2);
    else if (strcmp(key, "control_rate") == 0) inst->control_rate = (int)clamp_enum(value, SF_CONTROL_RATE_COUNT);
    else if (strcmp(key, "cycle_mode") == 0) set_cycle_mode(inst, (int)value);
    else if (strcmp(key, "cycle_rise") == 0) inst->cycle_rise_ms = sf_clampf(value, 1.0f, SF_ENV_MAX_MS);
    else if (strcmp(key, "cycle_fall") == 0) inst->cycle_fall_ms = sf_clampf(value, 1.0f, SF_ENV_MAX_MS);
    else if (strcmp(key, "cycle_hold") == 0)
        inst->cycle_hold_ms = inst->cycle_mode == CYCLE_ENV
            ? sf_clampf(value, 0.0f, 1.0f) : sf_clampf(value, 0.0f, 10000.0f);
    else if (strcmp(key, "cycle_amount") == 0) inst->cycle_amount = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "cycle_rise_curve") == 0) inst->cycle_rise_curve = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "cycle_fall_curve") == 0) inst->cycle_fall_curve = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "cycle_filter_direct") == 0) inst->cycle_filter_direct = value > 0.5f;
    /* Legacy keys. Before the Cycling Envelope page existed, `cycle_rate` and
     * `cycle_depth` drove a free-running triangle LFO. They are mapped onto the
     * new envelope so an old patch keeps its movement: the old period becomes
     * the Rise and Fall halves, and the old depth becomes the Amount. */
    else if (strcmp(key, "cycle_rate") == 0) {
        float rate = sf_clampf(value, 0.01f, 20.0f);
        float half_ms = 500.0f / rate;
        inst->cycle_rise_ms = sf_clampf(half_ms, 1.0f, 10000.0f);
        inst->cycle_fall_ms = sf_clampf(half_ms, 1.0f, 10000.0f);
    }
    else if (strcmp(key, "cycle_depth") == 0) inst->cycle_amount = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "voice_mode") == 0) inst->voice_mode = (int)clamp_enum(value, 3);
    else if (strcmp(key, "glide") == 0) inst->glide_ms = sf_clampf(value, 0.0f, SF_GLIDE_MAX_MS);
    else if (strcmp(key, "glide_mode") == 0) inst->glide_mode = (int)clamp_enum(value, 2);
    else if (strcmp(key, "bend_range") == 0) {
        inst->bend_range = sf_clampf(value, 0.0f, 24.0f);
        for (int channel = 0; channel < 16; ++channel)
            inst->bend_ratio[channel] = powf(2.0f, inst->bend_raw[channel]
                * inst->bend_range / 12.0f);
    }
    else if (strcmp(key, "spread") == 0) inst->spread_cents = sf_clampf(value, 0.0f, 7200.0f);
    else if (strcmp(key, "mod_wheel") == 0) inst->wheel = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "unison_voices") == 0) inst->unison_voices = (int)sf_clampf(roundf(value), 2.0f, (float)SF_MAX_VOICES);
    else if (strcmp(key, "unison_mode") == 0) inst->unison_mode = (int)clamp_enum(value, 2);
    else if (strcmp(key, "chord") == 0) inst->chord = (int)clamp_enum(value, 2);
    else if (strcmp(key, "chord_notes") == 0)
        inst->chord_notes = (int)sf_clampf(roundf(value), 1.0f, (float)SF_CHORD_MAX_NOTES);
    else if (strncmp(key, "chord_note", 10) == 0 && key[10] >= '1'
             && key[10] < '1' + SF_CHORD_MAX_NOTES && key[11] == '\0')
        inst->chord_offset[key[10] - '1'] = (int)sf_clampf(roundf(value), -48.0f, 48.0f);
    else if (strcmp(key, "stereo") == 0) inst->stereo = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "output") == 0) inst->output = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "seq_mode") == 0) seq_set_mode(inst, (int)value);
    else if (strcmp(key, "seq_arp_mode") == 0) inst->seq.arp_mode = (int)clamp_enum(value, ARP_MODE_COUNT);
    else if (strcmp(key, "seq_arp_oct") == 0) inst->seq.arp_octaves = (int)clamp_enum(value, 4);
    else if (strcmp(key, "seq_div") == 0) inst->seq.division = (int)clamp_enum(value, SF_SEQ_DIV_COUNT);
    else if (strcmp(key, "seq_gate") == 0) inst->seq.gate = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "seq_swing") == 0) inst->seq.swing = sf_clampf(value, 0.0f, 1.0f);
    else if (strcmp(key, "seq_length") == 0) inst->seq.length = (int)sf_clampf(value, 1.0f, (float)SF_SEQ_MAX_STEPS);
    else {
        int lane, lane_field;
        if (parse_seq_lane_key(key, &lane, &lane_field)) {
            if (lane_field == SEQ_LANE_DEST) inst->seq.lane_dest[lane] = (int)clamp_enum(value, DEST_COUNT);
            else if (lane_field == SEQ_LANE_SMOOTH) inst->seq.lane_smooth[lane] = value >= 0.5f;
            return;
        }
        int lfo, point;
        if (parse_lfo_curve_key(key, &lfo, &point)) {
            float v = sf_clampf(value, -1.0f, 1.0f);
            if (lfo == 0) inst->lfo1_shaper.amp[point] = v;
            else inst->lfo2_shaper.amp[point] = v;
            return;
        }        int shp_lfo, shp_field, shp_step;
        if (parse_shaper_ext_key(key, &shp_lfo, &shp_field, &shp_step)) {
            sf_shaper_t *shaper = shaper_for(inst, shp_lfo);
            switch (shp_field) {
            case 0: shaper->steps = (int)sf_clampf(value, 1.0f, (float)SF_SHAPER_STEPS); break;
            case 1: shaper->amp[shp_step] = sf_clampf(value, -1.0f, 1.0f); break;
            case 2: shaper->curve[shp_step] = sf_clampf(value, 0.0f, 1.0f); break;
            case 3: shaper->slope[shp_step] = sf_clampf(value, 0.0f, 1.0f); break;
            case 5: shaper->mode = value > 0.5f ? SHAPER_MINIFREAK : SHAPER_LEVELS; break;
            /* 0 rest, 1 play, 2 tie (MiniFreak mode; Levels treats 2 as on). */
            default: shaper->on[shp_step] = value >= 1.5f ? 2 : (value > 0.5f ? 1 : 0); break;
            }
            return;
        }
        int macro;
        if (parse_macro_key(key, &macro)) {
            inst->macros[macro] = sf_clampf(value, 0.0f, 1.0f);
            return;
        }
        int slot, field;
        if (parse_fx_param_key(key, &slot, &field)) {
            if (field == 0) {
                int next = (int)clamp_enum(value, FX_TYPE_COUNT);
                if (next != inst->fx_type[slot]) inst->fx_subtype[slot] = 0;
                inst->fx_type[slot] = next;
            }
            else if (field == 1) inst->fx_mix[slot] = sf_clampf(value, 0.0f, 1.0f);
            else if (field == 2) inst->fx_time[slot] = sf_clampf(value, 1.0f, SF_FX_TIME_MAX);
            else if (field == 3) inst->fx_feedback[slot] = sf_clampf(value, 0.0f, 0.95f);
            else if (field >= 4 && field <= 6) inst->fx_eq_db[slot][field - 4] = sf_clampf(value, -15.0f, 6.0f);
            else if (field == 7) {
                int type = (int)clamp_enum((float)inst->fx_type[slot], FX_TYPE_COUNT);
                inst->fx_subtype[slot] = (int)clamp_enum(value,
                    (unsigned)fx_preset_count(type));
            }
            else if (field == 8) inst->fx_peak_frequency[slot] = sf_clampf(value, 30.0f, 15000.0f);
            else if (field == 9) inst->fx_peak_gain[slot] = sf_clampf(value, -15.0f, 6.0f);
            else if (field == 10) inst->fx_peak_width[slot] = sf_clampf(value, 0.1f, 10.0f);
            return;
        }
        int column, source, is_dest, macro_index, macro_slot;
        if (parse_mod_column_key(key, &column, &source, &is_dest)) {
            if (is_dest) inst->mod_dest[column] = (int)clamp_enum(value, DEST_COUNT);
            else inst->mod_amount[column][source] = sf_clampf(value, -1.0f, 1.0f);
            return;
        }
        if (strcmp(key, "mod_edit_col") == 0) {
            inst->mod_edit_col = (int)clamp_enum(value, SF_MOD_COLUMNS);
            return;
        }
        if (parse_mod_edit_key(key, &source, &is_dest)) {
            int edit_col = inst->mod_edit_col;
            if (is_dest) inst->mod_dest[edit_col] = (int)clamp_enum(value, DEST_COUNT);
            else inst->mod_amount[edit_col][source] = sf_clampf(value, -1.0f, 1.0f);
            return;
        }
        if (parse_macro_slot_key(key, &macro_index, &macro_slot, &is_dest)) {
            if (is_dest) inst->macro_dest[macro_index][macro_slot] = (int)clamp_enum(value, DEST_COUNT);
            else inst->macro_amount[macro_index][macro_slot] = sf_clampf(value, -1.0f, 1.0f);
        }
    }
}

/* Copies the quoted string value of `key` (no escapes - the only string the
 * state carries is the sequencer pattern, which is digits and . + , only). */
static uint32_t json_hash(const char *key, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) h = (h ^ (unsigned char)key[i]) * 16777619u;
    return h;
}

/* Index json's top-level "key": value pairs. The first of a repeated key
 * wins, as the scan below finds it first. */
static void json_index_build(sf_json_index_t *ix, const char *json) {
    ix->text = json;
    ix->count = 0;
    memset(ix->slot, 0, sizeof(ix->slot));
    const char *p = json;
    while (*p && *p != '{') ++p;
    if (*p) ++p;
    while (*p && ix->count < SF_JSON_KEYS) {
        while (*p && *p != '"' && *p != '}') ++p;
        if (*p != '"') break;
        const char *key = ++p;
        while (*p && *p != '"') ++p;
        if (!*p) break;
        size_t len = (size_t)(p - key);
        ++p;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (*p != ':') continue;
        ++p;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        const char *val = p;
        if (*p == '"') {
            ++p;
            while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
            if (*p) ++p;
        } else {
            while (*p && *p != ',' && *p != '}') ++p;
        }
        if (len >= 65535) continue;
        uint32_t mask = SF_JSON_SLOTS - 1, h = json_hash(key, len) & mask;
        int dup = 0;
        while (ix->slot[h]) {
            int e = ix->slot[h] - 1;
            if (ix->key_len[e] == len && memcmp(json + ix->key_off[e], key, len) == 0) { dup = 1; break; }
            h = (h + 1) & mask;
        }
        if (dup) continue;
        int e = ix->count++;
        ix->key_off[e] = (uint32_t)(key - json);
        ix->key_len[e] = (uint16_t)len;
        ix->val_off[e] = (uint32_t)(val - json);
        ix->slot[h] = (uint16_t)(e + 1);
    }
}

/* The value of key in json, or NULL: from the instance's index when it holds
 * this very string, otherwise by scanning. */
static const char *json_value(const sf_instance_t *inst, const char *json, const char *key) {
    size_t key_len = strlen(key);
    if (inst && inst->json_ix.text == json) {
        const sf_json_index_t *ix = &inst->json_ix;
        uint32_t mask = SF_JSON_SLOTS - 1, h = json_hash(key, key_len) & mask;
        while (ix->slot[h]) {
            int e = ix->slot[h] - 1;
            if (ix->key_len[e] == key_len && memcmp(json + ix->key_off[e], key, key_len) == 0)
                return json + ix->val_off[e];
            h = (h + 1) & mask;
        }
        return NULL;
    }
    const char *p = json;
    do {
        p = strstr(p, key);
        if (!p) return NULL;
        if (p > json && p[-1] == '"' && p[key_len] == '"') break;
        ++p;
    } while (*p);
    p += key_len + 1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p++ != ':') return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static int json_string(const sf_instance_t *inst, const char *json, const char *key,
                       char *out, size_t cap) {
    const char *p = json_value(inst, json, key);
    if (!p || *p++ != '"') return 0;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
    return 1;
}

static int json_number(const sf_instance_t *inst, const char *json, const char *key, float *out) {
    const char *p = json_value(inst, json, key);
    if (!p) return 0;
    char *end = NULL;
    float value = strtof(p, &end);
    if (end == p || !isfinite(value)) return 0;
    *out = value;
    return 1;
}

static void migrate_legacy_engine_mix(sf_instance_t *inst, const char *json) {
    float legacy_mix;
    float ignored;
    if (!json_number(inst, json, "mix", &legacy_mix)) return;
    if (json_number(inst, json, "osc1_wave", &ignored)
        || json_number(inst, json, "osc1_timbre", &ignored)
        || json_number(inst, json, "osc2_wave", &ignored)
        || json_number(inst, json, "osc2_timbre", &ignored)) return;
    legacy_mix = sf_clampf(legacy_mix, 0.0f, 1.0f);
    inst->osc1_level *= 1.0f - legacy_mix;
    inst->osc2_level *= legacy_mix;
}

/* The grid's keys are generated from the slot tables, so the defaults and the
 * state blob are read with the same generator the parameters use. */
static void read_grid(sf_instance_t *inst, const char *json);
static void reset_patch_parameters(sf_instance_t *inst);
static void migrate_route_grid(sf_instance_t *inst, const char *json);

static void initialize_instance(sf_instance_t *inst, int pool_index,
                                const char *json_defaults) {
    inst->pool_index = pool_index;
    inst->last_out[0] = inst->last_out[1] = 0.0f;
    inst->declick[0] = inst->declick[1] = 0.0f;
    inst->declick_left = 0;
    reset_patch_parameters(inst);
    /* A new instance starts quieter than reset_patch_parameters leaves a
     * preset load (module.json's defaults). */
    inst->osc1_level = 0.4f;
    inst->osc2_level = 0.3f;
    inst->control_rate = SF_CONTROL_RATE_DEFAULT;
    inst->last_note = -1;
    inst->contract_dirty_blocks = 0;
    inst->contract_was_dirty = 0;
    inst->age_counter = 0;
    inst->sustain_down = 0;
    memset(inst->seq_sustained, 0, sizeof(inst->seq_sustained));

    for (int channel = 0; channel < 16; ++channel) {
        inst->bend_ratio[channel] = 1.0f;
        inst->bend_raw[channel] = 0.0f;
        inst->channel_pressure[channel] = 0.0f;
    }
    for (int i = 0; i < SF_MAX_VOICES; ++i) {
        inst->voices[i] = (sf_voice_t){0};
        inst->voices[i].rng = 0x6d2b79f5u ^ (uint32_t)(pool_index * 97 + i * 7919 + 1);
        inst->voice_ctl[i] = (sf_voice_ctl_t){0};
    }
    inst->lfo1_wraps = 0u;
    inst->lfo2_wraps = 0u;

    inst->json_ix.text = NULL;
    if (json_defaults && *json_defaults) {
        json_index_build(&inst->json_ix, json_defaults);
#define READ_DEFAULT(name) do { float v; if (json_number(inst, json_defaults, #name, &v)) set_numeric_param(inst, #name, v); } while (0)
        READ_DEFAULT(osc1_mode); READ_DEFAULT(osc1_table); READ_DEFAULT(osc1_position);
        READ_DEFAULT(osc1_wave); READ_DEFAULT(osc1_timbre); READ_DEFAULT(osc1_shape);
        READ_DEFAULT(osc1_tune); READ_DEFAULT(osc1_quantize); READ_DEFAULT(osc1_level);
        READ_DEFAULT(osc2_mode); READ_DEFAULT(osc2_table); READ_DEFAULT(osc2_position);
        READ_DEFAULT(osc2_wave); READ_DEFAULT(osc2_timbre); READ_DEFAULT(osc2_shape);
        READ_DEFAULT(osc2_tune); READ_DEFAULT(osc2_quantize); READ_DEFAULT(osc2_level);
        READ_DEFAULT(mix); READ_DEFAULT(cutoff); READ_DEFAULT(resonance); READ_DEFAULT(filter_mode);
        READ_DEFAULT(filter_env); READ_DEFAULT(filter_fm); READ_DEFAULT(filter_fm_src); READ_DEFAULT(attack); READ_DEFAULT(decay); READ_DEFAULT(sustain);
        READ_DEFAULT(release); READ_DEFAULT(lfo1_rate); READ_DEFAULT(lfo1_depth); READ_DEFAULT(lfo1_shape);
        READ_DEFAULT(attack_curve); READ_DEFAULT(decay_curve); READ_DEFAULT(release_curve);
        READ_DEFAULT(env_retrig); READ_DEFAULT(vel_vca); READ_DEFAULT(vel_vcf);
        READ_DEFAULT(vel_env); READ_DEFAULT(vel_time); READ_DEFAULT(velo_source);
        READ_DEFAULT(kbd_source);
        READ_DEFAULT(lfo2_rate); READ_DEFAULT(lfo2_depth); READ_DEFAULT(lfo2_shape);
        READ_DEFAULT(lfo1_sync); READ_DEFAULT(lfo1_div); READ_DEFAULT(lfo1_retrig);
        READ_DEFAULT(lfo2_sync); READ_DEFAULT(lfo2_div); READ_DEFAULT(lfo2_retrig);
        READ_DEFAULT(osc_phase); READ_DEFAULT(control_rate);
        READ_DEFAULT(cycle_rate); READ_DEFAULT(cycle_depth);
        READ_DEFAULT(cycle_mode); READ_DEFAULT(cycle_rise); READ_DEFAULT(cycle_fall);
        READ_DEFAULT(cycle_hold); READ_DEFAULT(cycle_amount); READ_DEFAULT(cycle_filter_direct);
        READ_DEFAULT(cycle_rise_curve); READ_DEFAULT(cycle_fall_curve);
        READ_DEFAULT(voice_mode);
        READ_DEFAULT(glide); READ_DEFAULT(glide_mode); READ_DEFAULT(bend_range);
        READ_DEFAULT(spread); READ_DEFAULT(unison_voices); READ_DEFAULT(unison_mode);
        READ_DEFAULT(chord); READ_DEFAULT(chord_notes); READ_DEFAULT(chord_note1); READ_DEFAULT(chord_note2);
        READ_DEFAULT(chord_note3); READ_DEFAULT(chord_note4); READ_DEFAULT(chord_note5); READ_DEFAULT(mod_wheel); READ_DEFAULT(stereo); READ_DEFAULT(output);
        READ_DEFAULT(fx1_type); READ_DEFAULT(fx1_subtype); READ_DEFAULT(fx1_mix); READ_DEFAULT(fx1_time); READ_DEFAULT(fx1_feedback);
        READ_DEFAULT(fx1_frequency); READ_DEFAULT(fx1_gain); READ_DEFAULT(fx1_width);
        READ_DEFAULT(fx1_low); READ_DEFAULT(fx1_mid); READ_DEFAULT(fx1_high);
        READ_DEFAULT(fx2_type); READ_DEFAULT(fx2_subtype); READ_DEFAULT(fx2_mix); READ_DEFAULT(fx2_time); READ_DEFAULT(fx2_feedback);
        READ_DEFAULT(fx2_frequency); READ_DEFAULT(fx2_gain); READ_DEFAULT(fx2_width);
        READ_DEFAULT(fx2_low); READ_DEFAULT(fx2_mid); READ_DEFAULT(fx2_high);
        READ_DEFAULT(fx3_type); READ_DEFAULT(fx3_subtype); READ_DEFAULT(fx3_mix); READ_DEFAULT(fx3_time); READ_DEFAULT(fx3_feedback);
        READ_DEFAULT(fx3_frequency); READ_DEFAULT(fx3_gain); READ_DEFAULT(fx3_width);
        READ_DEFAULT(fx3_low); READ_DEFAULT(fx3_mid); READ_DEFAULT(fx3_high);
        read_grid(inst, json_defaults);
        /* The factory patches predate the grid, so their six-route lists are
         * translated the same way an old state blob is. */
        migrate_route_grid(inst, json_defaults);
        READ_DEFAULT(macro1); READ_DEFAULT(macro2); READ_DEFAULT(macro3); READ_DEFAULT(macro4);
        READ_DEFAULT(lfo1_point1); READ_DEFAULT(lfo1_point2); READ_DEFAULT(lfo1_point3); READ_DEFAULT(lfo1_point4);
        READ_DEFAULT(lfo1_point5); READ_DEFAULT(lfo1_point6); READ_DEFAULT(lfo1_point7); READ_DEFAULT(lfo1_point8);
        READ_DEFAULT(lfo2_point1); READ_DEFAULT(lfo2_point2); READ_DEFAULT(lfo2_point3); READ_DEFAULT(lfo2_point4);
        READ_DEFAULT(lfo2_point5); READ_DEFAULT(lfo2_point6); READ_DEFAULT(lfo2_point7); READ_DEFAULT(lfo2_point8);
#undef READ_DEFAULT
        migrate_legacy_engine_mix(inst, json_defaults);
        inst->json_ix.text = NULL;
    }
    sf_effect_state_init(&inst->fx_state, inst->fx_mix, inst->fx_eq_db,
                         inst->fx_peak_frequency, inst->fx_peak_gain, inst->fx_peak_width);
    for (int i = 0; i < SF_MAX_VOICES; ++i) {
        inst->voices[i].sub_phase1 = 0.0f;
        inst->voices[i].sub_phase2 = 0.0f;
        inst->voices[i].noise_state1 = 0.0f;
        inst->voices[i].noise_state2 = 0.0f;
    }
    inst->osc1_shape_smooth = inst->osc1_shape;
    inst->osc1_level_smooth = inst->osc1_level;
    inst->osc1_wave_smooth = inst->osc1_wave;
    inst->osc1_timbre_smooth = inst->osc1_timbre;
    inst->osc2_shape_smooth = inst->osc2_shape;
    inst->osc2_level_smooth = inst->osc2_level;
    inst->osc2_wave_smooth = inst->osc2_wave;
    inst->osc2_timbre_smooth = inst->osc2_timbre;
    inst->cutoff_smooth = inst->cutoff;
    inst->filter_fm_smooth = inst->filter_fm;
    inst->resonance_smooth = inst->resonance;
    inst->output_smooth = inst->output;
    inst->lfo1_phase_global = 0.0f;
    inst->lfo2_phase_global = 0.0f;
}

static void reset_patch_parameters(sf_instance_t *inst) {
    inst->osc1_mode = OSC_VA;
    inst->osc1_table = 0;
    inst->osc2_table = 0;
    inst->osc1_wave = 0.5f;
    inst->osc1_timbre = 0.35f;
    inst->osc1_shape = 0.5f;
    inst->osc1_tune = 0.0f;
    inst->osc1_quantize = 0;
    inst->osc1_level = 0.8f;
    inst->osc2_mode = ENGINE_VA;
    inst->osc2_wave = 0.5f;
    inst->osc2_timbre = 0.35f;
    inst->osc2_shape = 0.5f;
    inst->osc2_tune = 0.0f;
    inst->osc2_quantize = 0;
    inst->osc2_level = 0.6f;
    inst->mix = 0.5f;
    inst->cutoff = SF_CUTOFF_DEFAULT;
    inst->resonance = 0.12f;
    inst->filter_mode = FILTER_LP;
    inst->filter_env = 0.35f;
    inst->filter_fm = 0.0f;
    inst->filter_fm_src = 1;
    inst->attack_ms = 10.0f;
    inst->decay_ms = 240.0f;
    inst->sustain = 0.72f;
    inst->release_ms = 280.0f;
    /* Straight ramps, continue in legato, and full velocity to the VCA:
     * exactly how the envelope behaved before these parameters existed. */
    inst->attack_curve = ENV_CURVE_DEFAULT;
    inst->decay_curve = ENV_CURVE_DEFAULT;
    inst->release_curve = ENV_CURVE_DEFAULT;
    inst->env_retrig = 1;
    inst->velo_source = 0;
    inst->kbd_source = KBD_SOURCE_LINEAR;
    inst->vel_vca = 1.0f;
    inst->vel_vcf = 0.0f;
    inst->vel_env = 0.0f;
    inst->vel_time = 0.0f;
    inst->lfo1_rate = 4.0f;
    inst->lfo1_depth = 0.0f;
    inst->lfo1_shape = LFO_TRIANGLE;
    sf_shaper_default(&inst->lfo1_shaper);
    inst->lfo2_rate = 0.35f;
    inst->lfo2_depth = 0.0f;
    inst->lfo2_shape = LFO_TRIANGLE;
    sf_shaper_default(&inst->lfo2_shaper);
    seq_reset_patch(inst);
    inst->cycle_mode = CYCLE_RUN;
    inst->cycle_rise_ms = 500.0f;
    inst->cycle_fall_ms = 500.0f;
    inst->cycle_hold_ms = 0.0f;
    /* The MiniFreak's Cycling Envelope reaches the matrix at full level; its
     * amount is only visible as the modulatable "CycEnv AM" destination. The
     * legacy direct-to-filter shortcut is off unless a patch asks for it. */
    inst->cycle_amount = 1.0f;
    inst->cycle_rise_curve = 0.5f;
    inst->cycle_fall_curve = 0.5f;
    inst->cycle_filter_direct = 0;
    inst->lfo1_sync = 0;
    inst->lfo1_div = SF_LFO_DIV_DEFAULT;
    inst->lfo1_retrig = 0;
    inst->lfo2_sync = 0;
    inst->lfo2_div = SF_LFO_DIV_DEFAULT;
    inst->lfo2_retrig = 0;
    /* Free-running oscillator phase, which is what 95% of the MiniFreak's
     * factory bank uses and what stops stacked chords from phase-locking. */
    inst->osc_phase = 1;
    inst->voice_mode = VOICE_POLY; inst->held_count = 0; inst->held_mode = VOICE_POLY;
    inst->glide_ms = 0.0f;
    inst->glide_mode = 0;
    inst->bend_range = 2.0f;
    inst->spread_cents = 24.0f;
    inst->unison_voices = SF_MAX_VOICES;
    inst->unison_mode = 0;
    chord_reset(inst);
    inst->stereo = 0.35f;
    inst->output = 0.7f;
    for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
        inst->fx_type[slot] = FX_OFF;
        inst->fx_subtype[slot] = 0;
        inst->fx_mix[slot] = 0.0f;
        inst->fx_time[slot] = 280.0f;
        inst->fx_feedback[slot] = 0.32f;
        inst->fx_peak_frequency[slot] = 1000.0f;
        inst->fx_peak_gain[slot] = 0.0f;
        inst->fx_peak_width[slot] = 1.0f;
        for (int band = 0; band < 3; ++band) inst->fx_eq_db[slot][band] = 0.0f;
    }
    for (int column = 0; column < SF_MOD_COLUMNS; ++column) {
        inst->mod_dest[column] = DEST_OFF;
        for (int source = 0; source < MODSRC_COUNT; ++source)
            inst->mod_amount[column][source] = 0.0f;
    }
    inst->mod_edit_col = 0;
    for (int macro = 0; macro < SF_MACROS; ++macro)
        for (int slot = 0; slot < SF_MACRO_SLOTS; ++slot) {
            inst->macro_dest[macro][slot] = DEST_OFF;
            inst->macro_amount[macro][slot] = 0.0f;
        }
    inst->wheel = 0.0f;
    for (int macro = 0; macro < 4; ++macro) inst->macros[macro] = 0.0f;
    /* No factory preset: the init patch. load_factory_preset sets the index
     * after this; reporting 0 here named a fresh instance after preset 0
     * while it played the init patch. */
    inst->preset_index = -1;
}

static void *sf_create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    sf_engines_init();
    for (int i = 0; i < SF_INSTANCE_POOL; ++i) {
        unsigned expected = 0;
        if (atomic_compare_exchange_strong_explicit(&g_instance_claimed[i], &expected, 1,
                memory_order_acquire, memory_order_relaxed)) {
            initialize_instance(&g_instances[i], i, json_defaults);
            return &g_instances[i];
        }
    }
    return NULL;
}

static void sf_destroy_instance(void *instance) {
    for (int i = 0; i < SF_INSTANCE_POOL; ++i) {
        if (instance == &g_instances[i]) {
            atomic_store_explicit(&g_instance_claimed[i], 0, memory_order_release);
            return;
        }
    }
}

static void sf_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    sf_instance_t *inst = (sf_instance_t *)instance;
    if (!inst || !msg || len < 2) return;
    unsigned status = msg[0] & 0xf0u;
    if (len < 3 && status != 0xc0u && status != 0xd0u) return;
    int channel = msg[0] & 0x0fu;
    int data1 = msg[1] & 0x7f;
    int data2 = len >= 3 ? (msg[2] & 0x7f) : 0;

    if (status == 0x90u && data2 > 0) {
        if (inst->seq.mode != SEQ_MODE_OFF) {
            inst->seq_sustained[data1 >> 5] &= ~(1u << (data1 & 31));
            sf_seq_key_down(&inst->seq, data1, data2);
        }
        else start_note(inst, data1, data2, channel);
    } else if (status == 0x80u || (status == 0x90u && data2 == 0)) {
        if (inst->seq.mode != SEQ_MODE_OFF) {
            if (inst->sustain_down) inst->seq_sustained[data1 >> 5] |= 1u << (data1 & 31);
            else sf_seq_key_up(&inst->seq, data1);
        }
        else release_note_ex(inst, data1, channel, 1);
    } else if (status == 0xa0u) {
        for (int i = 0; i < SF_MAX_VOICES; ++i) {
            sf_voice_t *voice = &inst->voices[i];
            if (voice->active && voice->key == data1 && voice->channel == channel)
                voice->pressure = (float)data2 / 127.0f;
        }
    } else if (status == 0xd0u) {
        inst->channel_pressure[channel] = (float)data1 / 127.0f;
        for (int i = 0; i < SF_MAX_VOICES; ++i)
            if (inst->voices[i].active && inst->voices[i].channel == channel)
                inst->voices[i].pressure = inst->channel_pressure[channel];
    } else if (status == 0xe0u) {
        int bend = ((data2 << 7) | data1) - 8192;
        inst->bend_raw[channel] = (float)bend / 8192.0f;
        inst->bend_ratio[channel] = powf(2.0f, inst->bend_raw[channel]
            * inst->bend_range / 12.0f);
    } else if (status == 0xb0u && data1 == 1) {
        /* Mod wheel, which is the grid's "Wheel" source row. */
        inst->wheel = (float)data2 / 127.0f;
    } else if (status == 0xb0u && data1 == 64) {
        sustain_pedal(inst, data2 >= 64);
    } else if (status == 0xb0u && data1 == 121) {
        /* Reset All Controllers: wheel, this channel's bend and pressure,
         * and the pedal, which lets go of what it was holding. */
        inst->wheel = 0.0f;
        inst->bend_raw[channel] = 0.0f;
        inst->bend_ratio[channel] = 1.0f;
        inst->channel_pressure[channel] = 0.0f;
        for (int i = 0; i < SF_MAX_VOICES; ++i)
            if (inst->voices[i].active && inst->voices[i].channel == channel)
                inst->voices[i].pressure = 0.0f;
        sustain_pedal(inst, 0);
    } else if (status == 0xb0u && data1 == 123) {
        if (inst->seq.mode != SEQ_MODE_OFF) seq_flush(inst);
        release_channel(inst, channel, 0);
    } else if (status == 0xb0u && data1 == 120) {
        release_channel(inst, channel, 1);
    }
}

static void set_param_value(sf_instance_t *inst, const char *key, const char *value) {
    if (strcmp(key, "seq_data") == 0) {
        sf_seq_set_data(&inst->seq, value);
        return;
    }
    {
        int lane, lane_field;
        if (parse_seq_lane_key(key, &lane, &lane_field) && lane_field == SEQ_LANE_DATA) {
            sf_seq_set_lane_data(&inst->seq, lane, value);
            return;
        }
    }
    if (strcmp(key, "osc1_mode") == 0) {
        int next = parse_osc1_type(value);
        if (next != inst->osc1_mode) { inst->osc1_mode = next; mark_contract_dirty(inst); }
    }
    else if (strcmp(key, "osc1_table") == 0) set_numeric_param(inst, key, (float)parse_wavetable_bank(value));
    else if (strcmp(key, "osc2_mode") == 0) {
        int next = parse_osc2_type(value);
        if (next != inst->osc2_mode) { inst->osc2_mode = next; mark_contract_dirty(inst); }
    }
    else if (strcmp(key, "osc2_table") == 0) set_numeric_param(inst, key, (float)parse_wavetable_bank(value));
    else if (strcmp(key, "lfo1_shape") == 0) inst->lfo1_shape = parse_enum(value, g_lfo_shapes, LFO_SHAPE_COUNT);
    else if (strcmp(key, "lfo2_shape") == 0) inst->lfo2_shape = parse_enum(value, g_lfo_shapes, LFO_SHAPE_COUNT);
    else if (strcmp(key, "filter_mode") == 0) inst->filter_mode = parse_enum(value, g_filter_modes, 3);
    else if (strcmp(key, "filter_fm_src") == 0) inst->filter_fm_src = parse_enum(value, g_filter_fm_srcs, 3);
    else if (strcmp(key, "voice_mode") == 0) inst->voice_mode = parse_enum(value, g_voice_modes, 3);
    else if (strcmp(key, "cycle_mode") == 0) set_cycle_mode(inst, parse_enum(value, g_cycle_modes, 3));
    else if (strcmp(key, "attack_curve") == 0) inst->attack_curve = parse_enum(value, g_env_attack_curves, 2);
    else if (strcmp(key, "decay_curve") == 0) inst->decay_curve = parse_enum(value, g_env_curves, 2);
    else if (strcmp(key, "release_curve") == 0) inst->release_curve = parse_enum(value, g_env_curves, 2);
    else if (strcmp(key, "env_retrig") == 0) inst->env_retrig = parse_enum(value, g_env_retrig_modes, 2);
    else if (strcmp(key, "velo_source") == 0) inst->velo_source = parse_enum(value, g_velo_sources, 3);
    else if (strcmp(key, "kbd_source") == 0) inst->kbd_source = parse_enum(value, g_kbd_sources, KBD_SOURCE_COUNT);
    else if (strcmp(key, "lfo1_sync") == 0) inst->lfo1_sync = parse_enum(value, g_lfo_sync_modes, 2);
    else if (strcmp(key, "lfo2_sync") == 0) inst->lfo2_sync = parse_enum(value, g_lfo_sync_modes, 2);
    else if (strcmp(key, "lfo1_retrig") == 0) inst->lfo1_retrig = parse_enum(value, g_lfo_retrig_modes, LFO_RETRIG_COUNT);
    else if (strcmp(key, "lfo2_retrig") == 0) inst->lfo2_retrig = parse_enum(value, g_lfo_retrig_modes, LFO_RETRIG_COUNT);
    else if (strcmp(key, "osc_phase") == 0) inst->osc_phase = parse_enum(value, g_osc_phase_modes, 2);
    else if (strcmp(key, "control_rate") == 0) inst->control_rate = parse_enum(value, g_control_rates, SF_CONTROL_RATE_COUNT);
    else if (strcmp(key, "osc1_quantize") == 0) inst->osc1_quantize = parse_enum(value, g_mod_quantize_modes, 11);
    else if (strcmp(key, "osc2_quantize") == 0) inst->osc2_quantize = parse_enum(value, g_mod_quantize_modes, 11);
    else if (strcmp(key, "glide_mode") == 0) inst->glide_mode = parse_enum(value, g_glide_modes, 2);
    else if (strcmp(key, "unison_mode") == 0) inst->unison_mode = parse_enum(value, g_unison_modes, 2);
    else if (strcmp(key, "chord") == 0) inst->chord = parse_enum(value, g_off_on, 2);
    else if (strcmp(key, "lfo1_div") == 0) {
        for (int i = 0; i < SF_LFO_DIV_COUNT; ++i)
            if (strcmp(value, g_lfo_divs[i].name) == 0) { inst->lfo1_div = i; return; }
    }
    else if (strcmp(key, "lfo2_div") == 0) {
        for (int i = 0; i < SF_LFO_DIV_COUNT; ++i)
            if (strcmp(value, g_lfo_divs[i].name) == 0) { inst->lfo2_div = i; return; }
    }
    else {
        int slot, field;
        if (parse_fx_param_key(key, &slot, &field)) {
            if (field == 0) {
                int next = parse_fx_type(value);
                if (next != inst->fx_type[slot]) inst->fx_subtype[slot] = 0;
                inst->fx_type[slot] = next;
            }
            else if (field == 7) {
                int type = (int)clamp_enum((float)inst->fx_type[slot], FX_TYPE_COUNT);
                inst->fx_subtype[slot] = parse_fx_subtype(value, type);
            }
            else set_numeric_param(inst, key, strtof(value, NULL));
            return;
        }
        int column, source, is_dest, macro_index, macro_slot;
        if (parse_mod_column_key(key, &column, &source, &is_dest)) {
            if (is_dest) inst->mod_dest[column] = parse_enum(value, g_mod_destinations, DEST_COUNT);
            else inst->mod_amount[column][source] = sf_clampf(strtof(value, NULL), -1.0f, 1.0f);
            return;
        }
        if (strcmp(key, "mod_edit_col") == 0) {
            inst->mod_edit_col = (int)clamp_enum(strtof(value, NULL), SF_MOD_COLUMNS);
            return;
        }
        if (parse_mod_edit_key(key, &source, &is_dest)) {
            int edit_col = inst->mod_edit_col;
            if (is_dest) inst->mod_dest[edit_col] = parse_enum(value, g_mod_destinations, DEST_COUNT);
            else inst->mod_amount[edit_col][source] = sf_clampf(strtof(value, NULL), -1.0f, 1.0f);
            return;
        }
        if (parse_macro_slot_key(key, &macro_index, &macro_slot, &is_dest)) {
            if (is_dest) inst->macro_dest[macro_index][macro_slot] = parse_enum(value, g_mod_destinations, DEST_COUNT);
            else inst->macro_amount[macro_index][macro_slot] = sf_clampf(strtof(value, NULL), -1.0f, 1.0f);
            return;
        }
        set_numeric_param(inst, key, strtof(value, NULL));
    }
}

/*
 * The grid's parameter names are generated (modcol3_lfo1, macro2_amt4), so it
 * is read by building the same names rather than with one macro line each.
 */
static void read_grid(sf_instance_t *inst, const char *json) {
    char key[24];
    for (int column = 0; column < SF_MOD_COLUMNS; ++column) {
        for (int field = 0; field <= MODSRC_COUNT; ++field) {
            const char *suffix = field == 0 ? "dest" : g_mod_slot_suffix[field - 1];
            snprintf(key, sizeof(key), "modcol%d_%s", column + 1, suffix);
            float value;
            if (json_number(inst, json, key, &value)) set_numeric_param(inst, key, value);
        }
    }
    for (int macro = 0; macro < SF_MACROS; ++macro) {
        for (int slot = 0; slot < SF_MACRO_SLOTS; ++slot) {
            snprintf(key, sizeof(key), "macro%d_dest%d", macro + 1, slot + 1);
            float value;
            if (json_number(inst, json, key, &value)) set_numeric_param(inst, key, value);
            snprintf(key, sizeof(key), "macro%d_amt%d", macro + 1, slot + 1);
            if (json_number(inst, json, key, &value)) set_numeric_param(inst, key, value);
        }
    }
    {
        static const char *const seq_keys[] = {
            "seq_mode", "seq_arp_mode", "seq_arp_oct", "seq_div",
            "seq_gate", "seq_swing", "seq_length"
        };
        float value;
        char data[SF_SEQ_DATA_MAX];
        for (size_t i = 0; i < sizeof(seq_keys) / sizeof(seq_keys[0]); ++i)
            if (json_number(inst, json, seq_keys[i], &value)) set_numeric_param(inst, seq_keys[i], value);
        if (json_string(inst, json, "seq_data", data, sizeof(data))) sf_seq_set_data(&inst->seq, data);
        /* A saved sequencer section lists every lane in use, so lanes it does
         * not mention are off. */
        if (json_number(inst, json, "seq_mode", &value)) {
            for (int lane = 0; lane < SF_SEQ_LANES; ++lane) {
                inst->seq.lane_dest[lane] = 0;
                inst->seq.lane_smooth[lane] = 1;
                sf_seq_set_lane_data(&inst->seq, lane, NULL);
            }
        }
        for (int lane = 1; lane <= SF_SEQ_LANES; ++lane) {
            snprintf(key, sizeof(key), "seq_lane%d_dest", lane);
            if (json_number(inst, json, key, &value)) set_numeric_param(inst, key, value);
            snprintf(key, sizeof(key), "seq_lane%d_smooth", lane);
            if (json_number(inst, json, key, &value)) set_numeric_param(inst, key, value);
            snprintf(key, sizeof(key), "seq_lane%d_data", lane);
            if (json_string(inst, json, key, data, sizeof(data))) sf_seq_set_lane_data(&inst->seq, lane - 1, data);
        }
    }
    for (int lfo = 1; lfo <= 2; ++lfo) {
        float value;
        snprintf(key, sizeof(key), "lfo%d_shp_steps", lfo);
        if (json_number(inst, json, key, &value)) set_numeric_param(inst, key, value);
        snprintf(key, sizeof(key), "lfo%d_shp_mode", lfo);
        if (json_number(inst, json, key, &value)) set_numeric_param(inst, key, value);
        for (int field = 0; field < 4; ++field) {
            for (int step = 1; step <= SF_SHAPER_STEPS; ++step) {
                snprintf(key, sizeof(key), "lfo%d_shp_%s%d", lfo, g_shaper_fields[field], step);
                if (json_number(inst, json, key, &value)) set_numeric_param(inst, key, value);
            }
        }
    }
}

static int has_cycle_matrix_route(const sf_instance_t *inst) {
    for (int column = 0; column < SF_MOD_COLUMNS; ++column) {
        if (inst->mod_dest[column] != DEST_OFF
            && fabsf(inst->mod_amount[column][MODSRC_CYCLE]) > 0.000001f)
            return 1;
    }
    return 0;
}

/* Older generated/imported states have matrix depths but no global Cycle
 * Amount. They also predate the switch that separates that amount from the
 * synth's direct filter shortcut. Recover the MiniFreak behavior on load. */
static void repair_imported_cycle_source(sf_instance_t *inst, const char *json,
                                         int preset_index) {
    float direct;
    if (preset_index < SF_ORIGINAL_PRESET_COUNT
        || json_number(inst, json, "cycle_filter_direct", &direct)
        || !has_cycle_matrix_route(inst))
        return;
    inst->cycle_amount = 1.0f;
    inst->cycle_filter_direct = 0;
}

/*
 * State version 8 replaced the six-route list with the grid. Each old route is
 * placed on the grid: a macro source becomes a macro slot, Pressure joins the
 * Velo + AT row (the hardware has one row for both), Random has no row and is
 * dropped, and each destination gets a column of its own.
 */
static void migrate_route_grid(sf_instance_t *inst, const char *json) {
    static const int row_for_source[9] = {-1, MODSRC_LFO1, MODSRC_LFO2, MODSRC_ENV,
                                          MODSRC_CYCLE, MODSRC_VELO_AT, MODSRC_VELO_AT,
                                          MODSRC_KEYTRACK, -1};
    for (int route = 0; route < 6; ++route) {
        char key[24];
        float source, destination, amount;
        snprintf(key, sizeof(key), "mod%d_source", route + 1);
        if (!json_number(inst, json, key, &source)) continue;
        snprintf(key, sizeof(key), "mod%d_destination", route + 1);
        if (!json_number(inst, json, key, &destination)) continue;
        snprintf(key, sizeof(key), "mod%d_amount", route + 1);
        if (!json_number(inst, json, key, &amount)) amount = 0.0f;
        int source_index = (int)clamp_enum(source, 13);
        int dest = (int)clamp_enum(destination, DEST_COUNT);
        if (source_index <= 0 || dest == DEST_OFF || amount == 0.0f) continue;

        if (source_index >= 9) {                    /* Macro 1..4 */
            int macro_index = source_index - 9;
            for (int slot = 0; slot < SF_MACRO_SLOTS; ++slot) {
                if (inst->macro_dest[macro_index][slot] != DEST_OFF) continue;
                inst->macro_dest[macro_index][slot] = dest;
                inst->macro_amount[macro_index][slot] = amount;
                break;
            }
            continue;
        }
        int row = source_index <= 8 ? row_for_source[source_index] : -1;
        if (row < 0) continue;                      /* Random */
        int column = -1;
        for (int i = 0; i < SF_MOD_COLUMNS; ++i)
            if (inst->mod_dest[i] == dest) { column = i; break; }
        if (column < 0)
            for (int i = 0; i < SF_MOD_COLUMNS; ++i)
                if (inst->mod_dest[i] == DEST_OFF) { column = i; inst->mod_dest[i] = dest; break; }
        if (column < 0) continue;
        inst->mod_amount[column][row] = amount;
    }
}

static void apply_state_json(sf_instance_t *inst, const char *json) {
    json_index_build(&inst->json_ix, json);
#define RESTORE(name) do { float v; if (json_number(inst, json, #name, &v)) set_numeric_param(inst, #name, v); } while (0)
    float state_version = 0.0f;
    if (!json_number(inst, json, "version", &state_version)) state_version = 0.0f;
    {
        /* Version 18 dropped the wavetable-based Additive mode (Osc 1 index 5,
         * Osc 2 index 6); Harmo is the real additive oscillator and takes
         * its place, and every later mode moves down one slot. */
        float mode;
        if (json_number(inst, json, "osc1_mode", &mode)) {
            if (state_version > 0.0f && state_version < 18.0f && mode >= 5.0f)
                mode = mode == 5.0f ? (float)OSC_HARMO : mode - 1.0f;
            set_numeric_param(inst, "osc1_mode", mode);
        }
        if (json_number(inst, json, "osc2_mode", &mode)) {
            if (state_version > 0.0f && state_version < 18.0f && mode >= 6.0f)
                mode = mode == 6.0f ? 7.0f : mode - 1.0f;   /* Harmo is 7 here */
            /* Version 19 dropped Cross FM (index 2); FM / RM is its MiniFreak
             * counterpart. */
            if (state_version > 0.0f && state_version < 19.0f && mode >= 2.0f)
                mode = mode == 2.0f ? (float)ENGINE_FM_RM : mode - 1.0f;
            set_numeric_param(inst, "osc2_mode", mode);
        }
    }
    RESTORE(osc1_table); RESTORE(osc1_position);
    RESTORE(osc1_wave); RESTORE(osc1_timbre); RESTORE(osc1_shape); RESTORE(osc1_tune); RESTORE(osc1_quantize); RESTORE(osc1_level);
    RESTORE(osc2_table); RESTORE(osc2_position);
    RESTORE(osc2_wave); RESTORE(osc2_timbre); RESTORE(osc2_shape); RESTORE(osc2_tune); RESTORE(osc2_quantize); RESTORE(osc2_level);
    if (state_version > 0.0f && state_version < 11.0f
        && inst->osc1_mode == OSC_WAVETABLE) {
        float legacy_family;
        if (json_number(inst, json, "osc1_wave", &legacy_family)) {
            inst->osc1_table = (int)sf_clampf(roundf(legacy_family
                * (SF_WT_FAMILY_COUNT - 1)), 0.0f, (float)(SF_WT_FAMILY_COUNT - 1));
            inst->osc1_wave = inst->osc1_timbre;
            inst->osc1_timbre = inst->osc1_shape;
            inst->osc1_shape = 0.5f;
        }
    }
    if (state_version > 0.0f && state_version < 17.0f && inst->osc2_mode == ENGINE_WAVETABLE) {
        /* Before version 17 Oscillator 2's Wavetable type used Wave as a bank
         * crossfade (over the then 16 banks) and Timbre as the position. */
        float legacy_bank;
        if (json_number(inst, json, "osc2_wave", &legacy_bank)) {
            inst->osc2_table = (int)sf_clampf(roundf(legacy_bank * 15.0f), 0.0f, 15.0f);
            inst->osc2_wave = inst->osc2_timbre;
            inst->osc2_timbre = 0.0f;
            inst->osc2_shape = 0.5f;
        }
    }
    {
        /* The cutoff domain changed twice: before state version 6 the field
         * held Hz, versions 6-8 held the 0..100 log value, and version 9 holds
         * the normalised 0..1 value. Reading the version out of the blob
         * converts an old patch exactly, rather than by the magnitude heuristic
         * a bare set_param write has to use. */
        float v;
        if (json_number(inst, json, "cutoff", &v)) {
            float cutoff_value;
            if (state_version < 6.0f)          cutoff_value = cutoff_param_from_hz(v);
            else if (state_version < 9.0f)     cutoff_value = v / 100.0f;
            else                               cutoff_value = v;
            set_numeric_param(inst, "cutoff", cutoff_value);
        }
    }
    /* A patch saved before Filter FM has neither key: start them from off. */
    inst->filter_fm = 0.0f;
    inst->filter_fm_src = 1;
    RESTORE(mix); RESTORE(resonance); RESTORE(filter_mode); RESTORE(filter_env); RESTORE(filter_fm); RESTORE(filter_fm_src);
    RESTORE(attack); RESTORE(decay); RESTORE(sustain); RESTORE(release); RESTORE(lfo1_rate);
    RESTORE(attack_curve); RESTORE(decay_curve); RESTORE(release_curve);
    RESTORE(env_retrig); RESTORE(vel_vca); RESTORE(vel_vcf);
    RESTORE(vel_env); RESTORE(vel_time); RESTORE(velo_source);
    RESTORE(kbd_source);
    RESTORE(lfo1_depth); RESTORE(lfo1_shape); RESTORE(lfo2_rate); RESTORE(lfo2_depth); RESTORE(lfo2_shape);
    RESTORE(lfo1_sync); RESTORE(lfo1_div); RESTORE(lfo1_retrig);
    RESTORE(lfo2_sync); RESTORE(lfo2_div); RESTORE(lfo2_retrig);
    RESTORE(osc_phase); RESTORE(control_rate);
    RESTORE(cycle_rate); RESTORE(cycle_depth);
    RESTORE(cycle_mode); RESTORE(cycle_rise); RESTORE(cycle_fall);
    {
        float hold;
        if (json_number(inst, json, "cycle_hold", &hold)) {
            /* Before v11 the Env-mode Hold knob was incorrectly serialized as
             * milliseconds. Migrate its old pot position to the new Sustain. */
            if (state_version < 11.0f && inst->cycle_mode == CYCLE_ENV)
                hold = cbrtf(sf_clampf(hold / 10000.0f, 0.0f, 1.0f));
            set_numeric_param(inst, "cycle_hold", hold);
        }
    }
    RESTORE(cycle_amount); RESTORE(cycle_filter_direct);
    RESTORE(cycle_rise_curve); RESTORE(cycle_fall_curve);
    RESTORE(voice_mode); RESTORE(glide); RESTORE(glide_mode); RESTORE(bend_range); RESTORE(spread); RESTORE(unison_voices); RESTORE(mod_wheel);
    RESTORE(unison_mode); RESTORE(chord); RESTORE(chord_notes); RESTORE(chord_note1); RESTORE(chord_note2);
    RESTORE(chord_note3); RESTORE(chord_note4); RESTORE(chord_note5);
    RESTORE(lfo1_point1); RESTORE(lfo1_point2); RESTORE(lfo1_point3); RESTORE(lfo1_point4);
    RESTORE(lfo1_point5); RESTORE(lfo1_point6); RESTORE(lfo1_point7); RESTORE(lfo1_point8);
    RESTORE(lfo2_point1); RESTORE(lfo2_point2); RESTORE(lfo2_point3); RESTORE(lfo2_point4);
    RESTORE(lfo2_point5); RESTORE(lfo2_point6); RESTORE(lfo2_point7); RESTORE(lfo2_point8);
    RESTORE(macro1); RESTORE(macro2); RESTORE(macro3); RESTORE(macro4);
    RESTORE(stereo); RESTORE(output);
    RESTORE(fx1_type); RESTORE(fx1_subtype); RESTORE(fx1_mix); RESTORE(fx1_time); RESTORE(fx1_feedback);
    RESTORE(fx1_low); RESTORE(fx1_mid); RESTORE(fx1_high);
    RESTORE(fx1_frequency); RESTORE(fx1_gain); RESTORE(fx1_width);
    RESTORE(fx2_type); RESTORE(fx2_subtype); RESTORE(fx2_mix); RESTORE(fx2_time); RESTORE(fx2_feedback);
    RESTORE(fx2_low); RESTORE(fx2_mid); RESTORE(fx2_high);
    RESTORE(fx2_frequency); RESTORE(fx2_gain); RESTORE(fx2_width);
    RESTORE(fx3_type); RESTORE(fx3_subtype); RESTORE(fx3_mix); RESTORE(fx3_time); RESTORE(fx3_feedback);
    RESTORE(fx3_low); RESTORE(fx3_mid); RESTORE(fx3_high);
    RESTORE(fx3_frequency); RESTORE(fx3_gain); RESTORE(fx3_width);
    if (state_version < 12.0f) {
        /* Before v12 every effect's Time ran 1..350. Delay still reads it as
         * milliseconds, so free-running delays keep their value; every other
         * use (including the Sync delays' division) reads a knob position and
         * is rescaled onto the new 1..1000 range. */
        for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
            if (inst->fx_type[slot] == FX_DELAY && inst->fx_subtype[slot] < 7) continue;
            inst->fx_time[slot] = sf_clampf(1.0f + (inst->fx_time[slot] - 1.0f)
                                         * (SF_FX_TIME_MAX - 1.0f) / 349.0f,
                                         1.0f, SF_FX_TIME_MAX);
        }
    }
    if (state_version < 13.0f) {
        /* v13 measured the MiniFreak's envelopes and LFOs. Decay and release
         * now run to -60 dB instead of -43 dB, so old times stretch by
         * ln(1000)/5 to keep the same fall rate. The Cycling Envelope's curves
         * got 5.5x stronger, so old Run/Loop curve values are pulled toward
         * linear.
         * Run and Loop swapped meaning (Run is now the free-running one), and
         * the LFO divisions became the MiniFreak's longer list. */
        inst->decay_ms = sf_clampf(inst->decay_ms * 1.3815511f, 1.0f, SF_ENV_MAX_MS);
        inst->release_ms = sf_clampf(inst->release_ms * 1.3815511f, 1.0f, SF_ENV_MAX_MS);
        if (inst->cycle_mode == CYCLE_ENV) {
            /* Env mode's curve knobs became the ADSR's Default / Quick or
             * Percussive switch; Default is the closer match to the old
             * shapes. */
            inst->cycle_rise_curve = 0.0f;
            inst->cycle_fall_curve = 0.0f;
        } else {
            inst->cycle_rise_curve = 0.5f + (inst->cycle_rise_curve - 0.5f) * (4.0f / 22.0f);
            inst->cycle_fall_curve = 0.5f + (inst->cycle_fall_curve - 0.5f) * (4.0f / 22.0f);
        }
        if (inst->cycle_mode == CYCLE_RUN) inst->cycle_mode = CYCLE_LOOP;
        else if (inst->cycle_mode == CYCLE_LOOP) inst->cycle_mode = CYCLE_RUN;
        float div;
        if (json_number(inst, json, "lfo1_div", &div))
            inst->lfo1_div = g_lfo_div_v12[(int)clamp_enum(div, 20)];
        if (json_number(inst, json, "lfo2_div", &div))
            inst->lfo2_div = g_lfo_div_v12[(int)clamp_enum(div, 20)];
    }
    read_grid(inst, json);
    if (state_version < 8.0f) migrate_route_grid(inst, json);
    if (state_version < 14.0f) {
        /* v14 measured the destination scales. Pitch routes went from 12 to
         * 60 semitones per unit, so older amounts shrink to keep their
         * depth. */
        for (int column = 0; column < SF_MOD_COLUMNS; ++column) {
            int dest = inst->mod_dest[column];
            if (dest != DEST_PITCH && dest != DEST_PITCH1 && dest != DEST_PITCH2) continue;
            for (int row = 0; row < MODSRC_COUNT; ++row) inst->mod_amount[column][row] *= 0.2f;
        }
        for (int macro = 0; macro < SF_MACROS; ++macro) {
            for (int slot = 0; slot < SF_MACRO_SLOTS; ++slot) {
                int dest = inst->macro_dest[macro][slot];
                if (dest == DEST_PITCH || dest == DEST_PITCH1 || dest == DEST_PITCH2)
                    inst->macro_amount[macro][slot] *= 0.2f;
            }
        }
    }
    if (state_version < 15.0f) {
        /* v15 glides in a straight line in semitones, as MiniFreak V does,
         * where it used to approach the note exponentially with Glide as the
         * time constant. Three time constants (95 % of the way) is the closest
         * straight-line length. */
        inst->glide_ms = sf_clampf(inst->glide_ms * 3.0f, 0.0f, SF_GLIDE_MAX_MS);
    }
    if (state_version < 16.0f) {
        /* v16 stretches Decay and Release by exp(K x Velo > Time x velocity),
         * as MiniFreak V does, where it used to be 1 + amount x velocity.
         * Keep a full-velocity note's old length. */
        inst->vel_time = sf_clampf(logf(1.0f + inst->vel_time) / SF_VEL_TIME_K, 0.0f, 1.0f);
    }
#undef RESTORE
    migrate_legacy_engine_mix(inst, json);
    float preset;
    if (json_number(inst, json, "preset", &preset))
        inst->preset_index = preset < 0.0f ? -1 : (int)clamp_enum(preset, SF_FACTORY_PRESET_COUNT);
    repair_imported_cycle_source(inst, json, inst->preset_index);
    inst->json_ix.text = NULL;   /* the caller's string may be gone after this */
}

/* Switching presets swaps every parameter at once. Voices left sounding
 * would jump to a different engine, filter and level mid-waveform, and the
 * effects restart their buffers, so the old sound is cut instead: the
 * sequencer lets go, the voices stop, and the effects stay bypassed until
 * their memory is clear. What would still be a step in the output - the last
 * sample the old sound played - fades to zero over 10 ms under the new
 * preset, so the change never clicks. */
#define SF_DECLICK_SAMPLES 441

static void cut_for_preset_change(sf_instance_t *inst) {
    seq_flush(inst);
    for (int i = 0; i < SF_MAX_VOICES; ++i) {
        sf_voice_t *voice = &inst->voices[i];
        voice->active = 0;
        voice->steal_pending = 0;
        voice->env = 0.0f;
        voice->last_env_out = 0.0f;
        voice->filter.low = 0.0f;
        voice->filter.band = 0.0f;
        voice->dc_in = 0.0f;
        voice->dc_out = 0.0f;
        voice->fm_dc_in = 0.0f;
        voice->fm_dc_out = 0.0f;
        voice->fm_prev_signal = 0.0f;
        voice->fm_prev_src = 0.0f;
    }
    inst->declick[0] = inst->last_out[0];
    inst->declick[1] = inst->last_out[1];
    inst->declick_left = SF_DECLICK_SAMPLES;
}

static void load_factory_preset(sf_instance_t *inst, int index) {
    const char *state;
    if (index < 0 || index >= SF_FACTORY_PRESET_COUNT) return;
    state = factory_preset_state(index);
    if (!state) return;
    cut_for_preset_change(inst);
    reset_patch_parameters(inst);
    apply_state_json(inst, state);
    repair_imported_cycle_source(inst, state, index);
    sf_effect_state_init(&inst->fx_state, inst->fx_mix, inst->fx_eq_db,
                         inst->fx_peak_frequency, inst->fx_peak_gain, inst->fx_peak_width);
    inst->preset_index = index;
    mark_contract_dirty(inst);
}

static void sf_set_param(void *instance, const char *key, const char *value) {
    sf_instance_t *inst = (sf_instance_t *)instance;
    if (!inst || !key || !value) return;
    if (strcmp(key, "preset") == 0) {
        char *end = NULL;
        long index = strtol(value, &end, 10);
        if (end != value && index >= 0 && index < SF_FACTORY_PRESET_COUNT)
            load_factory_preset(inst, (int)index);
        return;
    }
    if (strcmp(key, "state") == 0) {
        apply_state_json(inst, value);
        return;
    }
    set_param_value(inst, key, value);
}

static int write_string(char *buf, int buf_len, const char *value) {
    size_t n;
    if (!buf || buf_len < 1) return -1;
    n = strlen(value);
    if (n >= (size_t)buf_len) return -1;
    memcpy(buf, value, n + 1u);
    return (int)n;
}

/* The Move matrix selector: one row per hardware column, named after the
 * destination it drives. Choosing a row writes its index to mod_edit_col, and
 * the one editor page then addresses that column through the aliases. */
static int format_mod_matrix_items(const sf_instance_t *inst, char *buf, int buf_len) {
    int written = snprintf(buf, (size_t)buf_len, "[");
    if (written < 0 || written >= buf_len) return -1;
    for (int column = 0; column < SF_MOD_COLUMNS; ++column) {
        int dest = (int)clamp_enum((float)inst->mod_dest[column], DEST_COUNT);
        const char *name = dest == DEST_OFF ? "Empty" : g_mod_destinations[dest];
        int n = snprintf(buf + written, (size_t)(buf_len - written),
                         "%s{\"index\":%d,\"label\":\"%d: %s\"}",
                         column ? "," : "", column, column + 1, name);
        if (n < 0 || written + n >= buf_len) return -1;
        written += n;
    }
    if (written + 2 >= buf_len) return -1;
    buf[written++] = ']';
    buf[written] = '\0';
    return written;
}

static int sf_get_param(void *instance, const char *key, char *buf, int buf_len) {
    sf_instance_t *inst = (sf_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;
    if (strcmp(key, "chain_params") == 0) {
        int n = format_chain_params(inst, buf, buf_len);
        if (n < 0) return n;
        strip_modcol_entries(buf);
        return strip_descriptions(buf);
    }
    /* The same document with its descriptions, for the browser build's
     * tooltips and the parameter reference. */
    if (strcmp(key, "chain_params_full") == 0) return format_chain_params(inst, buf, buf_len);
    if (strcmp(key, "ui_hierarchy") == 0) return format_ui_hierarchy(inst, buf, buf_len);
    /* Keys whose voices are sounding (not releasing), as a comma list, for
     * the browser build's on-screen keyboard. Read-only. */
    if (strcmp(key, "sounding_notes") == 0) {
        int len = 0;
        buf[0] = '\0';
        for (int i = 0; i < SF_MAX_VOICES; ++i) {
            const sf_voice_t *v = &inst->voices[i];
            if (!v->active || v->steal_pending || v->env_stage == ENV_RELEASE || v->env_stage == ENV_OFF) continue;
            int n = snprintf(buf + len, (size_t)(buf_len - len), "%s%d", len ? "," : "", v->key >= 0 ? v->key : v->note);
            if (n < 0 || n >= buf_len - len) break;
            len += n;
        }
        return len;
    }
    if (strcmp(key, "is_loading") == 0) {
        if (inst->contract_dirty_blocks > 0 || inst->contract_was_dirty) {
            inst->contract_was_dirty = 0;
            return write_string(buf, buf_len, "1");
        }
        return write_string(buf, buf_len, "0");
    }
    if (strcmp(key, "preset_count") == 0) return snprintf(buf, (size_t)buf_len, "%d", SF_FACTORY_PRESET_COUNT);
    if (strcmp(key, "preset") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->preset_index);
    if (strcmp(key, "preset_name") == 0) {
        int index = inst->preset_index;
        if (index < 0 || index >= SF_FACTORY_PRESET_COUNT) return write_string(buf, buf_len, "Init");
        return write_string(buf, buf_len, factory_preset_name(index));
    }
    /* The whole factory list as one JSON array. The Remote UI builds its preset
     * selector from this instead of reading 265 names one at a time. */
    if (strcmp(key, "preset_names") == 0) {
        int written = snprintf(buf, (size_t)buf_len, "[");
        if (written < 0 || written >= buf_len) return -1;
        for (int i = 0; i < SF_FACTORY_PRESET_COUNT; ++i) {
            int n = snprintf(buf + written, (size_t)(buf_len - written), "%s\"%s\"",
                             i ? "," : "", factory_preset_name(i));
            if (n < 0 || written + n >= buf_len) return -1;
            written += n;
        }
        if (written + 2 >= buf_len) return -1;
        buf[written++] = ']';
        buf[written] = '\0';
        return written;
    }
    if (strcmp(key, "mod_matrix_items") == 0) return format_mod_matrix_items(inst, buf, buf_len);
    if (strcmp(key, "name") == 0) return write_string(buf, buf_len, "Bizar");
    if (strcmp(key, "last_note") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->last_note);
    /* Live source values in grid row order (cyc env lfo1 lfo2 vel whl kbd),
     * or "" while no voice sounds. */
    if (strcmp(key, "mod_sources") == 0) {
        if (!inst->ui_source_live) return write_string(buf, buf_len, "");
        const float *s = inst->ui_source;
        return snprintf(buf, (size_t)buf_len, "[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]",
                        s[0], s[1], s[2], s[3], s[4], s[5], s[6]);
    }
    /* The pattern step now playing, or -1 when the step sequencer is idle. */
    if (strcmp(key, "seq_step") == 0) {
        int length = inst->seq.length < 1 ? 1 : inst->seq.length;
        int step = inst->seq.mode == SEQ_MODE_SEQ && inst->seq.running && inst->seq.tick > 0
                 ? (int)((inst->seq.tick - 1u) % (unsigned)length) : -1;
        return snprintf(buf, (size_t)buf_len, "%d", step);
    }
#ifdef SF_PROFILE
    if (strcmp(key, "cpu_us") == 0) return snprintf(buf, (size_t)buf_len, "%.1f", (double)inst->cpu_us);
#endif
    if (strcmp(key, "polyphony") == 0) {
        int active = 0;
        for (int i = 0; i < SF_MAX_VOICES; ++i) active += inst->voices[i].active;
        return snprintf(buf, (size_t)buf_len, "%d", active);
    }
    if (strcmp(key, "state") == 0) {
        int n = snprintf(buf, (size_t)buf_len,
            "{\"version\":19,\"preset\":%d,\"osc1_mode\":%d,\"osc1_table\":%d,\"osc1_wave\":%.6g,\"osc1_timbre\":%.6g,"
            "\"osc1_shape\":%.6g,\"osc1_tune\":%.6g,\"osc1_quantize\":%d,\"osc1_level\":%.6g,"
            "\"osc2_mode\":%d,\"osc2_table\":%d,\"osc2_wave\":%.6g,\"osc2_timbre\":%.6g,"
            "\"osc2_shape\":%.6g,\"osc2_tune\":%.6g,\"osc2_quantize\":%d,\"osc2_level\":%.6g,"
            "\"cutoff\":%.6g,\"resonance\":%.6g,\"filter_mode\":%d,\"filter_env\":%.6g,\"filter_fm\":%.6g,\"filter_fm_src\":%d,"
            "\"attack\":%.6g,\"decay\":%.6g,\"sustain\":%.6g,\"release\":%.6g,"
            "\"attack_curve\":%d,\"decay_curve\":%d,\"release_curve\":%d,\"env_retrig\":%d,"
            "\"vel_vca\":%.6g,\"vel_vcf\":%.6g,\"vel_env\":%.6g,\"vel_time\":%.6g,\"velo_source\":%d,\"kbd_source\":%d,"
            "\"lfo1_rate\":%.6g,\"lfo1_depth\":%.6g,\"lfo1_shape\":%d,"
            "\"lfo1_sync\":%d,\"lfo1_div\":%d,\"lfo1_retrig\":%d,"
            "\"lfo2_rate\":%.6g,\"lfo2_depth\":%.6g,\"lfo2_shape\":%d,"
            "\"lfo2_sync\":%d,\"lfo2_div\":%d,\"lfo2_retrig\":%d,"
            "\"osc_phase\":%d,\"control_rate\":%d,"
            "\"cycle_mode\":%d,\"cycle_rise\":%.6g,\"cycle_rise_curve\":%.6g,\"cycle_fall\":%.6g,\"cycle_fall_curve\":%.6g,\"cycle_hold\":%.6g,\"cycle_amount\":%.6g,\"cycle_filter_direct\":%d,"
            "\"voice_mode\":%d,\"glide\":%.6g,\"glide_mode\":%d,\"bend_range\":%.6g,"
            "\"lfo1_point1\":%.6g,\"lfo1_point2\":%.6g,\"lfo1_point3\":%.6g,\"lfo1_point4\":%.6g,"
            "\"lfo1_point5\":%.6g,\"lfo1_point6\":%.6g,\"lfo1_point7\":%.6g,\"lfo1_point8\":%.6g,"
            "\"lfo2_point1\":%.6g,\"lfo2_point2\":%.6g,\"lfo2_point3\":%.6g,\"lfo2_point4\":%.6g,"
            "\"lfo2_point5\":%.6g,\"lfo2_point6\":%.6g,\"lfo2_point7\":%.6g,\"lfo2_point8\":%.6g,"
            "\"macro1\":%.6g,\"macro2\":%.6g,\"macro3\":%.6g,\"macro4\":%.6g,\"mod_wheel\":%.6g,"
            "\"spread\":%.6g,\"unison_voices\":%d,\"stereo\":%.6g,\"output\":%.6g,"
            "\"fx1_type\":%d,\"fx1_subtype\":%d,\"fx1_mix\":%.6g,\"fx1_time\":%.6g,\"fx1_feedback\":%.6g,"
            "\"fx1_low\":%.6g,\"fx1_mid\":%.6g,\"fx1_high\":%.6g,\"fx1_frequency\":%.6g,\"fx1_gain\":%.6g,\"fx1_width\":%.6g,"
            "\"fx2_type\":%d,\"fx2_subtype\":%d,\"fx2_mix\":%.6g,\"fx2_time\":%.6g,\"fx2_feedback\":%.6g,"
            "\"fx2_low\":%.6g,\"fx2_mid\":%.6g,\"fx2_high\":%.6g,\"fx2_frequency\":%.6g,\"fx2_gain\":%.6g,\"fx2_width\":%.6g,"
            "\"fx3_type\":%d,\"fx3_subtype\":%d,\"fx3_mix\":%.6g,\"fx3_time\":%.6g,\"fx3_feedback\":%.6g,"
            "\"fx3_low\":%.6g,\"fx3_mid\":%.6g,\"fx3_high\":%.6g,\"fx3_frequency\":%.6g,\"fx3_gain\":%.6g,\"fx3_width\":%.6g",
            inst->preset_index, inst->osc1_mode, inst->osc1_table, inst->osc1_wave, inst->osc1_timbre,
            inst->osc1_shape, inst->osc1_tune, inst->osc1_quantize, inst->osc1_level,
            inst->osc2_mode, inst->osc2_table, inst->osc2_wave, inst->osc2_timbre,
            inst->osc2_shape, inst->osc2_tune, inst->osc2_quantize, inst->osc2_level,
            inst->cutoff, inst->resonance, inst->filter_mode, inst->filter_env, inst->filter_fm, inst->filter_fm_src,
            inst->attack_ms, inst->decay_ms, inst->sustain, inst->release_ms,
            inst->attack_curve, inst->decay_curve, inst->release_curve, inst->env_retrig,
            inst->vel_vca, inst->vel_vcf, inst->vel_env, inst->vel_time, inst->velo_source, inst->kbd_source,
            inst->lfo1_rate, inst->lfo1_depth, inst->lfo1_shape,
            inst->lfo1_sync, inst->lfo1_div, inst->lfo1_retrig,
            inst->lfo2_rate, inst->lfo2_depth, inst->lfo2_shape,
            inst->lfo2_sync, inst->lfo2_div, inst->lfo2_retrig,
            inst->osc_phase, inst->control_rate,
            inst->cycle_mode, inst->cycle_rise_ms, inst->cycle_rise_curve,
            inst->cycle_fall_ms, inst->cycle_fall_curve,
            inst->cycle_hold_ms, inst->cycle_amount, inst->cycle_filter_direct,
            inst->voice_mode, inst->glide_ms, inst->glide_mode, inst->bend_range,
            inst->lfo1_shaper.amp[0], inst->lfo1_shaper.amp[1], inst->lfo1_shaper.amp[2], inst->lfo1_shaper.amp[3],
            inst->lfo1_shaper.amp[4], inst->lfo1_shaper.amp[5], inst->lfo1_shaper.amp[6], inst->lfo1_shaper.amp[7],
            inst->lfo2_shaper.amp[0], inst->lfo2_shaper.amp[1], inst->lfo2_shaper.amp[2], inst->lfo2_shaper.amp[3],
            inst->lfo2_shaper.amp[4], inst->lfo2_shaper.amp[5], inst->lfo2_shaper.amp[6], inst->lfo2_shaper.amp[7],
            inst->macros[0], inst->macros[1], inst->macros[2], inst->macros[3], inst->wheel,
            inst->spread_cents, inst->unison_voices, inst->stereo, inst->output,
            inst->fx_type[0], inst->fx_subtype[0], inst->fx_mix[0], inst->fx_time[0], inst->fx_feedback[0],
            inst->fx_eq_db[0][0], inst->fx_eq_db[0][1], inst->fx_eq_db[0][2],
            inst->fx_peak_frequency[0], inst->fx_peak_gain[0], inst->fx_peak_width[0],
            inst->fx_type[1], inst->fx_subtype[1], inst->fx_mix[1], inst->fx_time[1], inst->fx_feedback[1],
            inst->fx_eq_db[1][0], inst->fx_eq_db[1][1], inst->fx_eq_db[1][2],
            inst->fx_peak_frequency[1], inst->fx_peak_gain[1], inst->fx_peak_width[1],
            inst->fx_type[2], inst->fx_subtype[2], inst->fx_mix[2], inst->fx_time[2], inst->fx_feedback[2],
            inst->fx_eq_db[2][0], inst->fx_eq_db[2][1], inst->fx_eq_db[2][2],
            inst->fx_peak_frequency[2], inst->fx_peak_gain[2], inst->fx_peak_width[2]);
        if (n < 0 || n >= buf_len) return -1;
        /* The grid writes itself from the same key generator the reader uses,
         * instead of 128 more conversion arguments. */
        int written = n;
        for (int column = 0; column < SF_MOD_COLUMNS; ++column) {
            int step = snprintf(buf + written, (size_t)(buf_len - written),
                                ",\"modcol%d_dest\":%d",
                                column + 1, inst->mod_dest[column]);
            if (step < 0 || written + step >= buf_len) return -1;
            written += step;
            for (int source = 0; source < MODSRC_COUNT; ++source) {
                step = snprintf(buf + written, (size_t)(buf_len - written),
                                ",\"modcol%d_%s\":%.6g", column + 1,
                                g_mod_slot_suffix[source], inst->mod_amount[column][source]);
                if (step < 0 || written + step >= buf_len) return -1;
                written += step;
            }
        }
        for (int macro = 0; macro < SF_MACROS; ++macro) {
            for (int slot = 0; slot < SF_MACRO_SLOTS; ++slot) {
                int step = snprintf(buf + written, (size_t)(buf_len - written),
                                    ",\"macro%d_dest%d\":%d,\"macro%d_amt%d\":%.6g",
                                    macro + 1, slot + 1, inst->macro_dest[macro][slot],
                                    macro + 1, slot + 1, inst->macro_amount[macro][slot]);
                if (step < 0 || written + step >= buf_len) return -1;
                written += step;
            }
        }
        {
            int step = snprintf(buf + written, (size_t)(buf_len - written),
                                ",\"unison_mode\":%d,\"chord\":%d,\"chord_notes\":%d",
                                inst->unison_mode, inst->chord, inst->chord_notes);
            if (step < 0 || written + step >= buf_len) return -1;
            written += step;
            for (int i = 0; i < SF_CHORD_MAX_NOTES; ++i) {
                step = snprintf(buf + written, (size_t)(buf_len - written),
                                ",\"chord_note%d\":%d", i + 1, inst->chord_offset[i]);
                if (step < 0 || written + step >= buf_len) return -1;
                written += step;
            }
        }
        for (int lfo = 0; lfo < 2; ++lfo) {
            const sf_shaper_t *shaper = shaper_for((sf_instance_t *)inst, lfo);
            if (sf_shaper_is_default_extension(shaper)) continue;
            int step = snprintf(buf + written, (size_t)(buf_len - written),
                                ",\"lfo%d_shp_mode\":%d,\"lfo%d_shp_steps\":%d",
                                lfo + 1, shaper->mode, lfo + 1, shaper->steps);
            if (step < 0 || written + step >= buf_len) return -1;
            written += step;
            for (int i = 0; i < SF_SHAPER_STEPS; ++i) {
                step = snprintf(buf + written, (size_t)(buf_len - written),
                                ",\"lfo%d_shp_amp%d\":%.6g,\"lfo%d_shp_curve%d\":%.6g,"
                                "\"lfo%d_shp_slope%d\":%.6g,\"lfo%d_shp_on%d\":%d",
                                lfo + 1, i + 1, shaper->amp[i], lfo + 1, i + 1, shaper->curve[i],
                                lfo + 1, i + 1, shaper->slope[i], lfo + 1, i + 1, shaper->on[i]);
                if (step < 0 || written + step >= buf_len) return -1;
                written += step;
            }
        }
        if (seq_has_content(inst)) {
            int step = snprintf(buf + written, (size_t)(buf_len - written),
                                ",\"seq_mode\":%d,\"seq_arp_mode\":%d,\"seq_arp_oct\":%d,"
                                "\"seq_div\":%d,\"seq_gate\":%.6g,\"seq_swing\":%.6g,"
                                "\"seq_length\":%d,\"seq_data\":\"",
                                inst->seq.mode, inst->seq.arp_mode, inst->seq.arp_octaves,
                                inst->seq.division, inst->seq.gate, inst->seq.swing,
                                inst->seq.length);
            if (step < 0 || written + step >= buf_len) return -1;
            written += step;
            step = sf_seq_get_data(&inst->seq, buf + written, buf_len - written);
            if (step < 0 || written + step + 2 >= buf_len) return -1;
            written += step;
            buf[written++] = '"';
            for (int lane = 0; lane < SF_SEQ_LANES; ++lane) {
                if (!sf_seq_lane_in_use(&inst->seq, lane)) continue;
                step = snprintf(buf + written, (size_t)(buf_len - written),
                                ",\"seq_lane%d_dest\":%d,\"seq_lane%d_smooth\":%d,"
                                "\"seq_lane%d_data\":\"",
                                lane + 1, inst->seq.lane_dest[lane], lane + 1,
                                inst->seq.lane_smooth[lane], lane + 1);
                if (step < 0 || written + step >= buf_len) return -1;
                written += step;
                step = sf_seq_get_lane_data(&inst->seq, lane, buf + written, buf_len - written);
                if (step < 0 || written + step + 2 >= buf_len) return -1;
                written += step;
                buf[written++] = '"';
            }
        }
        int step = snprintf(buf + written, (size_t)(buf_len - written), "}");
        return step >= 0 && written + step < buf_len ? written + step : -1;
    }

#define GET_FLOAT(name, field) if (strcmp(key, name) == 0) return snprintf(buf, (size_t)buf_len, "%.6g", inst->field)
#define GET_ENUM(name, field, labels, count) if (strcmp(key, name) == 0) return write_string(buf, buf_len, labels[clamp_enum((float)inst->field, count)])
    GET_ENUM("osc1_mode", osc1_mode, g_osc1_modes, SF_OSC1_MODE_COUNT);
    GET_FLOAT("osc1_wave", osc1_wave);
    GET_FLOAT("osc1_timbre", osc1_timbre);
    GET_FLOAT("osc1_shape", osc1_shape);
    GET_FLOAT("osc1_tune", osc1_tune);
    GET_ENUM("osc1_quantize", osc1_quantize, g_mod_quantize_modes, 11);
    GET_FLOAT("osc1_level", osc1_level);
    GET_ENUM("osc2_mode", osc2_mode, g_osc2_modes, SF_OSC2_MODE_COUNT);
    GET_FLOAT("osc2_wave", osc2_wave);
    GET_FLOAT("osc2_timbre", osc2_timbre);
    GET_FLOAT("osc2_shape", osc2_shape);
    GET_FLOAT("osc2_tune", osc2_tune);
    GET_ENUM("osc2_quantize", osc2_quantize, g_mod_quantize_modes, 11);
    GET_FLOAT("osc2_level", osc2_level);
    if (strcmp(key, "osc1_table") == 0)
        return write_string(buf, buf_len, g_wavetable_names[clamp_enum((float)inst->osc1_table, SF_WT_FAMILY_COUNT)]);
    if (strcmp(key, "osc1_position") == 0) return snprintf(buf, (size_t)buf_len, "%.6g", inst->osc1_wave);
    if (strcmp(key, "osc2_table") == 0)
        return write_string(buf, buf_len, g_wavetable_names[clamp_enum((float)inst->osc2_table, SF_WT_FAMILY_COUNT)]);
    if (strcmp(key, "osc2_position") == 0) return snprintf(buf, (size_t)buf_len, "%.6g", inst->osc2_wave);
    GET_FLOAT("mix", mix);
    GET_FLOAT("cutoff", cutoff);
    GET_FLOAT("resonance", resonance);
    GET_ENUM("filter_mode", filter_mode, g_filter_modes, 3);
    GET_FLOAT("filter_env", filter_env);
    GET_FLOAT("filter_fm", filter_fm);
    GET_ENUM("filter_fm_src", filter_fm_src, g_filter_fm_srcs, 3);
    GET_FLOAT("attack", attack_ms);
    GET_FLOAT("decay", decay_ms);
    GET_FLOAT("sustain", sustain);
    GET_FLOAT("release", release_ms);
    GET_ENUM("attack_curve", attack_curve, g_env_attack_curves, 2);
    GET_ENUM("decay_curve", decay_curve, g_env_curves, 2);
    GET_ENUM("release_curve", release_curve, g_env_curves, 2);
    GET_ENUM("env_retrig", env_retrig, g_env_retrig_modes, 2);
    GET_FLOAT("vel_vca", vel_vca);
    GET_FLOAT("vel_vcf", vel_vcf);
    GET_FLOAT("vel_env", vel_env);
    GET_FLOAT("vel_time", vel_time);
    GET_ENUM("velo_source", velo_source, g_velo_sources, 3);
    GET_ENUM("kbd_source", kbd_source, g_kbd_sources, KBD_SOURCE_COUNT);
    GET_FLOAT("lfo1_rate", lfo1_rate);
    GET_FLOAT("lfo1_depth", lfo1_depth);
    if (strcmp(key, "cutoff_hz") == 0)
        return snprintf(buf, (size_t)buf_len, "%.6g", cutoff_hz_from_param(inst->cutoff));
    GET_ENUM("lfo1_shape", lfo1_shape, g_lfo_shapes, LFO_SHAPE_COUNT);
    GET_ENUM("lfo1_sync", lfo1_sync, g_lfo_sync_modes, 2);
    GET_ENUM("lfo1_retrig", lfo1_retrig, g_lfo_retrig_modes, LFO_RETRIG_COUNT);
    GET_ENUM("osc_phase", osc_phase, g_osc_phase_modes, 2);
    GET_ENUM("control_rate", control_rate, g_control_rates, SF_CONTROL_RATE_COUNT);
    GET_FLOAT("lfo2_rate", lfo2_rate);
    GET_FLOAT("lfo2_depth", lfo2_depth);
    GET_ENUM("lfo2_shape", lfo2_shape, g_lfo_shapes, LFO_SHAPE_COUNT);
    GET_ENUM("lfo2_sync", lfo2_sync, g_lfo_sync_modes, 2);
    GET_ENUM("lfo2_retrig", lfo2_retrig, g_lfo_retrig_modes, LFO_RETRIG_COUNT);
    if (strcmp(key, "lfo1_div") == 0)
        return write_string(buf, buf_len,
            g_lfo_divs[clamp_enum((float)inst->lfo1_div, SF_LFO_DIV_COUNT)].name);
    if (strcmp(key, "lfo2_div") == 0)
        return write_string(buf, buf_len,
            g_lfo_divs[clamp_enum((float)inst->lfo2_div, SF_LFO_DIV_COUNT)].name);
    GET_ENUM("cycle_mode", cycle_mode, g_cycle_modes, 3);
    GET_FLOAT("cycle_rise", cycle_rise_ms);
    GET_FLOAT("cycle_fall", cycle_fall_ms);
    GET_FLOAT("cycle_hold", cycle_hold_ms);
    GET_FLOAT("cycle_rise_curve", cycle_rise_curve);
    GET_FLOAT("cycle_fall_curve", cycle_fall_curve);
    GET_FLOAT("cycle_amount", cycle_amount);
    if (strcmp(key, "cycle_filter_direct") == 0)
        return snprintf(buf, (size_t)buf_len, "%d", inst->cycle_filter_direct);
    /* The retired keys still answer, derived from the envelope they now map
     * onto, so a scene or a lane that addressed them is not left reading
     * nothing. */
    if (strcmp(key, "cycle_rate") == 0) {
        float half = inst->cycle_rise_ms > 1.0f ? inst->cycle_rise_ms : 1.0f;
        return snprintf(buf, (size_t)buf_len, "%.6g", sf_clampf(500.0f / half, 0.01f, 20.0f));
    }
    if (strcmp(key, "cycle_depth") == 0)
        return snprintf(buf, (size_t)buf_len, "%.6g", inst->cycle_amount);
    GET_ENUM("voice_mode", voice_mode, g_voice_modes, 3);
    GET_FLOAT("glide", glide_ms);
    GET_ENUM("glide_mode", glide_mode, g_glide_modes, 2);
    GET_FLOAT("bend_range", bend_range);
    GET_FLOAT("spread", spread_cents);
    GET_FLOAT("mod_wheel", wheel);
    if (strcmp(key, "unison_voices") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->unison_voices);
    GET_ENUM("unison_mode", unison_mode, g_unison_modes, 2);
    GET_ENUM("chord", chord, g_off_on, 2);
    if (strcmp(key, "chord_notes") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->chord_notes);
    if (strncmp(key, "chord_note", 10) == 0 && key[10] >= '1'
        && key[10] < '1' + SF_CHORD_MAX_NOTES && key[11] == '\0')
        return snprintf(buf, (size_t)buf_len, "%d", inst->chord_offset[key[10] - '1']);
    GET_FLOAT("stereo", stereo);
    GET_FLOAT("output", output);
    if (strcmp(key, "seq_data") == 0) return sf_seq_get_data(&inst->seq, buf, buf_len);
    if (strcmp(key, "seq_mode") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->seq.mode);
    if (strcmp(key, "seq_arp_mode") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->seq.arp_mode);
    if (strcmp(key, "seq_arp_oct") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->seq.arp_octaves);
    if (strcmp(key, "seq_div") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->seq.division);
    if (strcmp(key, "seq_gate") == 0) return snprintf(buf, (size_t)buf_len, "%.6g", inst->seq.gate);
    if (strcmp(key, "seq_swing") == 0) return snprintf(buf, (size_t)buf_len, "%.6g", inst->seq.swing);
    if (strcmp(key, "seq_length") == 0) return snprintf(buf, (size_t)buf_len, "%d", inst->seq.length);
    {
        int lane, lane_field;
        if (parse_seq_lane_key(key, &lane, &lane_field)) {
            if (lane_field == SEQ_LANE_DATA) return sf_seq_get_lane_data(&inst->seq, lane, buf, buf_len);
            return snprintf(buf, (size_t)buf_len, "%d", lane_field == SEQ_LANE_DEST
                            ? inst->seq.lane_dest[lane] : inst->seq.lane_smooth[lane]);
        }
    }
    {
        int lfo, point;
        if (parse_lfo_curve_key(key, &lfo, &point)) {
            float value = lfo == 0 ? inst->lfo1_shaper.amp[point] : inst->lfo2_shaper.amp[point];
            return snprintf(buf, (size_t)buf_len, "%.6g", value);
        }        int shp_lfo, shp_field, shp_step;
        if (parse_shaper_ext_key(key, &shp_lfo, &shp_field, &shp_step)) {
            const sf_shaper_t *shaper = shaper_for(inst, shp_lfo);
            float value = shp_field == 0 ? (float)shaper->steps
                        : shp_field == 5 ? (float)shaper->mode
                        : shp_field == 1 ? shaper->amp[shp_step]
                        : shp_field == 2 ? shaper->curve[shp_step]
                        : shp_field == 3 ? shaper->slope[shp_step]
                        : (float)shaper->on[shp_step];
            return snprintf(buf, (size_t)buf_len, "%.6g", value);
        }
        int macro;
        if (parse_macro_key(key, &macro))
            return snprintf(buf, (size_t)buf_len, "%.6g", inst->macros[macro]);
    }
    {
        int column, source, is_dest, macro_index, macro_slot;
        if (parse_mod_column_key(key, &column, &source, &is_dest)) {
            if (is_dest)
                return write_string(buf, buf_len, g_mod_destinations[clamp_enum((float)inst->mod_dest[column], DEST_COUNT)]);
            return snprintf(buf, (size_t)buf_len, "%.6g", inst->mod_amount[column][source]);
        }
        if (strcmp(key, "mod_edit_col") == 0)
            return snprintf(buf, (size_t)buf_len, "%d", inst->mod_edit_col);
        if (parse_mod_edit_key(key, &source, &is_dest)) {
            int edit_col = inst->mod_edit_col;
            if (is_dest)
                return write_string(buf, buf_len, g_mod_destinations[clamp_enum((float)inst->mod_dest[edit_col], DEST_COUNT)]);
            return snprintf(buf, (size_t)buf_len, "%.6g", inst->mod_amount[edit_col][source]);
        }
        if (parse_macro_slot_key(key, &macro_index, &macro_slot, &is_dest)) {
            if (is_dest)
                return write_string(buf, buf_len, g_mod_destinations[clamp_enum((float)inst->macro_dest[macro_index][macro_slot], DEST_COUNT)]);
            return snprintf(buf, (size_t)buf_len, "%.6g", inst->macro_amount[macro_index][macro_slot]);
        }
    }
    {
        int slot, field;
        if (parse_fx_param_key(key, &slot, &field)) {
            if (field == 0)
                return write_string(buf, buf_len, g_fx_types[clamp_enum((float)inst->fx_type[slot], FX_TYPE_COUNT)]);
            if (field == 1) return snprintf(buf, (size_t)buf_len, "%.6g", inst->fx_mix[slot]);
            if (field == 2) return snprintf(buf, (size_t)buf_len, "%.6g", inst->fx_time[slot]);
            if (field == 3) return snprintf(buf, (size_t)buf_len, "%.6g", inst->fx_feedback[slot]);
            if (field >= 4 && field <= 6)
                return snprintf(buf, (size_t)buf_len, "%.6g", inst->fx_eq_db[slot][field - 4]);
            if (field == 7)
                return write_string(buf, buf_len,
                    fx_preset_name(inst->fx_type[slot], inst->fx_subtype[slot]));
            if (field == 8) return snprintf(buf, (size_t)buf_len, "%.6g", inst->fx_peak_frequency[slot]);
            if (field == 9) return snprintf(buf, (size_t)buf_len, "%.6g", inst->fx_peak_gain[slot]);
            return snprintf(buf, (size_t)buf_len, "%.6g", inst->fx_peak_width[slot]);
        }
    }
#undef GET_ENUM
#undef GET_FLOAT
    return -1;
}

static int sf_get_error(void *instance, char *buf, int buf_len) {
    (void)instance;
    if (!buf || buf_len < 1) return 0;
    buf[0] = '\0';
    return 0;
}

/* Flush-to-zero on aarch64 (FPCR.FZ, bit 24) to avoid denormal slowdowns. */
static uint64_t sf_enable_ftz(void) {
#if defined(__aarch64__)
    uint64_t old;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(old));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(old | (1ULL << 24)));
    return old;
#else
    return 0;
#endif
}

static void sf_restore_fpcr(uint64_t old) {
#if defined(__aarch64__)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(old));
#else
    (void)old;
#endif
}

static void sf_render_block(void *instance, int16_t *out, int frames) {
    sf_instance_t *inst = (sf_instance_t *)instance;
    if (!out || frames <= 0) return;
    if (!inst) {
        for (int i = 0; i < frames * 2; ++i) out[i] = 0;
        return;
    }
#ifdef SF_PROFILE
    struct timespec prof_t0, prof_t1;
    clock_gettime(CLOCK_MONOTONIC, &prof_t0);
#endif
    uint64_t saved_fpcr = sf_enable_ftz();

    if (inst->contract_dirty_blocks > 0) --inst->contract_dirty_blocks;
    sf_effect_clear_buffers(&inst->fx_state, inst->fx_type);
    /* The FX reset clears megabytes of delay line: once per block at most. */
    int fx_reset_done = 0;

    /* A short one-pole ramp on high-impact controls prevents knob and
     * automation changes from making discontinuities in the audio path. */
    const float smooth = 0.22f;
    /* Targets are still one-poled once per block, but the value the audio
     * path sees ramps linearly from the previous block's value per sample. */
    enum { SM_W1, SM_T1, SM_S1, SM_L1, SM_W2, SM_T2, SM_S2, SM_L2, SM_CUT, SM_RES, SM_OUT, SM_FM, SM_COUNT };
    float *const sm_ptr[SM_COUNT] = {
        &inst->osc1_wave_smooth, &inst->osc1_timbre_smooth, &inst->osc1_shape_smooth,
        &inst->osc1_level_smooth, &inst->osc2_wave_smooth, &inst->osc2_timbre_smooth,
        &inst->osc2_shape_smooth, &inst->osc2_level_smooth, &inst->cutoff_smooth,
        &inst->resonance_smooth, &inst->output_smooth, &inst->filter_fm_smooth };
    const float sm_target[SM_COUNT] = {
        inst->osc1_wave, inst->osc1_timbre, inst->osc1_shape, inst->osc1_level,
        inst->osc2_wave, inst->osc2_timbre, inst->osc2_shape, inst->osc2_level,
        inst->cutoff, inst->resonance, inst->output, inst->filter_fm };
    float sm_cur[SM_COUNT], sm_inc[SM_COUNT];
    for (int k = 0; k < SM_COUNT; ++k) {
        float prev = *sm_ptr[k];
        *sm_ptr[k] += smooth * (sm_target[k] - *sm_ptr[k]);
        sm_cur[k] = prev;
        sm_inc[k] = frames > 0 ? (*sm_ptr[k] - prev) / (float)frames : 0.0f;
    }
    {
        float eq_db[SF_FX_SLOTS][3];
        float peak_hz[SF_FX_SLOTS], peak_gain[SF_FX_SLOTS], peak_width[SF_FX_SLOTS];
        for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
            memcpy(eq_db[slot], inst->fx_eq_db[slot], sizeof(eq_db[slot]));
            peak_hz[slot] = inst->fx_peak_frequency[slot];
            peak_gain[slot] = inst->fx_peak_gain[slot];
            peak_width[slot] = inst->fx_peak_width[slot];
            sf_effect_mod_eq(inst->fx_type[slot], inst->fx_route[slot], eq_db[slot],
                             &peak_hz[slot], &peak_gain[slot], &peak_width[slot]);
        }
        sf_effect_state_smooth(&inst->fx_state, inst->fx_mix, eq_db,
                               peak_hz, peak_gain, peak_width, smooth);
    }

    float pan_for_voice[SF_MAX_VOICES] = {0.0f};
    if (inst->voice_mode != VOICE_MONO && inst->stereo > 0.0f) {
        static const float slot_pan[SF_MAX_VOICES] = {
            -1.0f, 1.0f, -0.6f, 0.6f, -0.2f, 0.2f
        };
        for (int v = 0; v < SF_MAX_VOICES; ++v)
            pan_for_voice[v] = slot_pan[v] * inst->stereo * 0.45f;
    }

    /*
     * A FIXED per-voice gain, not a function of how many voices are sounding.
     *
     * This used to be 0.56/sqrt(active_voices), which meant every already-held
     * note ducked by up to 6 dB the instant another note started: hold a
     * sustained note, play a stab chord, and the sustained note stepped down and
     * came back when the chord released. That is not what the hardware does —
     * it sums its voices and does not renormalise them — and it is audible
     * exactly in the playing style the user reported.
     *
     * Headroom for a full six-voice chord comes from the master soft clipper
     * below instead, which limits the peaks without touching the level of the
     * notes underneath them.
     */
    const float voice_gain = 0.45f;
    /* Velocity-to-time is per note, so the envelope steps are resolved once per
     * block per voice instead of once per sample. */
    /*
     * Tempo-synced LFOs. Move's tempo is read as a seqlock copy with no
     * syscalls, so it is safe here and the LFOs follow a tempo change live.
     * `valid` 0 or a non-positive tempo means Move is not publishing a set
     * right now, which is not the same as "120 BPM": the fallback only keeps
     * the maths finite.
     */
    float tempo = SF_DEFAULT_TEMPO;
    /* The host's tempo (MIDI clock, then the set) when it publishes one, and
     * its beat position while the transport runs, so synced LFOs stay on the
     * beat as Amino's and Crest's do. */
    double beat = -1.0;
    if (g_host && g_host->get_bpm) {
        float b = g_host->get_bpm();
        if (b > 20.0f && b < 400.0f) tempo = b;
        if (g_host->get_clock_status && g_host->get_beat_position
            && g_host->get_clock_status() == MOVE_CLOCK_STATUS_RUNNING)
            beat = g_host->get_beat_position();
    } else {
        move_info_t mi;
        if (move_info_read(&mi) && mi.valid && mi.tempo > 0.0f) tempo = mi.tempo;
    }
    float lfo1_hz = inst->lfo1_sync
        ? (tempo / 60.0f) / g_lfo_divs[clamp_enum((float)inst->lfo1_div, SF_LFO_DIV_COUNT)].beats
        : inst->lfo1_rate;
    float lfo2_hz = inst->lfo2_sync
        ? (tempo / 60.0f) / g_lfo_divs[clamp_enum((float)inst->lfo2_div, SF_LFO_DIV_COUNT)].beats
        : inst->lfo2_rate;
    /* A MiniFreak-mode Shaper gives every step a whole LFO period, so its
     * phase runs once per pattern rather than once per period. */
    float lfo1_steps = (inst->lfo1_shape == LFO_CUSTOM && inst->lfo1_shaper.mode == SHAPER_MINIFREAK
                        && inst->lfo1_shaper.steps > 1) ? (float)inst->lfo1_shaper.steps : 1.0f;
    float lfo2_steps = (inst->lfo2_shape == LFO_CUSTOM && inst->lfo2_shaper.mode == SHAPER_MINIFREAK
                        && inst->lfo2_shaper.steps > 1) ? (float)inst->lfo2_shaper.steps : 1.0f;
    float lfo1_inc = sf_clampf(lfo1_hz, 0.01f, SF_LFO_MAX_HZ) / (SF_SAMPLE_RATE * lfo1_steps);
    float lfo2_inc = sf_clampf(lfo2_hz, 0.01f, SF_LFO_MAX_HZ) / (SF_SAMPLE_RATE * lfo2_steps);
    /* A synced free-running LFO follows the transport: the same beat always
     * gives the same phase. The block then runs on from there at its rate. */
    if (beat >= 0.0) {
        for (int n = 0; n < 2; ++n) {
            int sync = n ? inst->lfo2_sync : inst->lfo1_sync;
            int retrig = n ? inst->lfo2_retrig : inst->lfo1_retrig;
            if (!sync || retrig != LFO_RETRIG_FREE) continue;
            int div = n ? inst->lfo2_div : inst->lfo1_div;
            double cycle = (double)g_lfo_divs[clamp_enum((float)div, SF_LFO_DIV_COUNT)].beats
                         * (double)(n ? lfo2_steps : lfo1_steps);
            double cyc = beat / cycle;
            float target = (float)(cyc - floor(cyc));
            float *phase = n ? &inst->lfo2_phase_global : &inst->lfo1_phase_global;
            if (target < *phase - 0.5f) {   /* crossed a cycle start: a new S&H draw */
                if (n) ++inst->lfo2_wraps; else ++inst->lfo1_wraps;
            }
            *phase = target;
        }
    }
    float attack_steps[SF_MAX_VOICES];
    float decay_steps[SF_MAX_VOICES];
    sf_env_shape_t attack_shapes[SF_MAX_VOICES];
    sf_env_shape_t decay_shapes[SF_MAX_VOICES];
    float sustain_voice[SF_MAX_VOICES];
    float glide_samples[SF_MAX_VOICES];
    float lfo1_inc_voice[SF_MAX_VOICES];
    float lfo2_inc_voice[SF_MAX_VOICES];
    float lfo1_amp_voice[SF_MAX_VOICES];
    float lfo2_amp_voice[SF_MAX_VOICES];
    float cycle_amp_voice[SF_MAX_VOICES];
    float cycle_rise_voice[SF_MAX_VOICES];
    float cycle_fall_voice[SF_MAX_VOICES];
    float cycle_hold_voice[SF_MAX_VOICES];
    /*
     * Effects are one shared chain, so routes into one cannot be per voice:
     * they are resolved once per block from whichever voice started last.
     */
    float fx_time_voice[SF_FX_SLOTS];
    /* Step-automation lanes, read at the block start for the block-rate
     * destinations and every sample for the rest. */
    float seq_lane[SF_SEQ_LANES];
    for (int lane = 0; lane < SF_SEQ_LANES; ++lane)
        seq_lane[lane] = sf_seq_lane_value(&inst->seq, lane);
    float fx_feedback_voice[SF_FX_SLOTS];
    float fx_mix_voice[SF_FX_SLOTS];
    for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
        fx_time_voice[slot] = inst->fx_time[slot];
        fx_feedback_voice[slot] = inst->fx_feedback[slot];
        fx_mix_voice[slot] = inst->fx_state.mix_smooth[slot];
    }
    /*
     * Envelope times, glide and the LFO rates and amplitudes shape the steps a
     * voice will take rather than a per-sample value, so they are resolved
     * once per block, from the grid evaluated against the state each voice is
     * in when the block starts.
     */
    for (int i = 0; i < SF_MAX_VOICES; ++i) {
        sf_voice_t *voice = &inst->voices[i];
        if (lfo_shared(inst->lfo1_retrig)) voice->lfo1_phase = inst->lfo1_phase_global;
        if (lfo_shared(inst->lfo2_retrig)) voice->lfo2_phase = inst->lfo2_phase_global;
        float velocity_time_scale = expf(SF_VEL_TIME_K * inst->vel_time * voice->velocity);
        float attack_factor = 1.0f, decay_factor = 1.0f, release_factor = 1.0f;
        float glide_factor = 1.0f, rate1 = 1.0f, rate2 = 1.0f;
        float amp1 = 1.0f, amp2 = 1.0f, ampc = 1.0f;
        float sustain_now = inst->sustain;
        cycle_rise_voice[i] = inst->cycle_rise_ms;
        cycle_fall_voice[i] = inst->cycle_fall_ms;
        cycle_hold_voice[i] = inst->cycle_hold_ms;
        if (voice->active) {
            float slow_source[MODSRC_COUNT];
            slow_source[MODSRC_CYCLE] = sf_voice_cycle_output(voice) * inst->cycle_amount;
            slow_source[MODSRC_ENV] = voice->last_env_out
                * (1.0f - inst->vel_env * (1.0f - voice->velocity));
            slow_source[MODSRC_LFO1] = lfo_output(inst->lfo1_shape, inst->lfo1_retrig, voice->lfo1_phase, voice->lfo1_done, &inst->lfo1_shaper, &voice->lfo1_snh);
            slow_source[MODSRC_LFO2] = lfo_output(inst->lfo2_shape, inst->lfo2_retrig, voice->lfo2_phase, voice->lfo2_done, &inst->lfo2_shaper, &voice->lfo2_snh);
            slow_source[MODSRC_VELO_AT] = velo_at_source(inst, voice->velocity, voice->pressure);
            slow_source[MODSRC_WHEEL] = inst->wheel;
            slow_source[MODSRC_KEYTRACK] = keyboard_source(inst, voice);
            sf_mod_input_t slow_grid = {
                .column_dest = inst->mod_dest,
                .column_amount = &inst->mod_amount[0][0],
                .macro_dest = &inst->macro_dest[0][0],
                .macro_amount = &inst->macro_amount[0][0],
                .source_value = slow_source,
                .macro_value = inst->macros,
                .offset_dest = inst->seq.lane_dest,
                .offset_value = seq_lane,
                .offset_count = SF_SEQ_LANES
            };
            sf_mod_values_t slow;
            sf_mod_apply(&slow_grid, &slow);
            attack_factor = sf_env_time_mod(ENV_ATTACK, inst->attack_ms,
                                            slow.dest[DEST_ATTACK]) / inst->attack_ms;
            decay_factor = sf_env_time_mod(ENV_DECAY, inst->decay_ms,
                                           slow.dest[DEST_DECAY]) / inst->decay_ms;
            release_factor = sf_env_time_mod(ENV_RELEASE, inst->release_ms,
                                             slow.dest[DEST_RELEASE]) / inst->release_ms;
            glide_factor = sf_mod_time_factor(slow.dest[DEST_GLIDE], 1.0f);
            rate1 = lfo_rate_mod(lfo1_hz, slow.dest[DEST_LFO1_RATE]) / lfo1_hz;
            rate2 = lfo_rate_mod(lfo2_hz, slow.dest[DEST_LFO2_RATE]) / lfo2_hz;
            /* Gain routes (see route_value) never push past unity. */
            amp1 = sf_clampf(1.0f + slow.dest[DEST_LFO1_AMP], 0.0f, 1.0f);
            amp2 = sf_clampf(1.0f + slow.dest[DEST_LFO2_AMP], 0.0f, 1.0f);
            ampc = sf_clampf(1.0f + slow.dest[DEST_CYCLE_AMP], 0.0f, 1.0f);
            sustain_now = sf_clampf(inst->sustain + slow.dest[DEST_SUSTAIN], 0.0f, 1.0f);
            cycle_times_mod(inst->cycle_mode, slow.dest[DEST_CYCLE_RISE], slow.dest[DEST_CYCLE_FALL],
                            slow.dest[DEST_CYCLE_HOLD], &cycle_rise_voice[i],
                            &cycle_fall_voice[i], &cycle_hold_voice[i]);
            voice->release_scale = release_factor;
        }
        float attack_len = sf_env_shape_setup(&attack_shapes[i], ENV_ATTACK,
                                              inst->attack_curve,
                                              inst->attack_ms * attack_factor);
        float decay_len = sf_env_shape_setup(&decay_shapes[i], ENV_DECAY,
                                             inst->decay_curve,
                                             inst->decay_ms * velocity_time_scale
                                             * decay_factor);
        attack_steps[i] = 1.0f / (attack_len * 0.001f * SF_SAMPLE_RATE);
        /* The decay runs down to the modulated sustain, so its step is sized
         * for that span: sized for the knob's, a Sustain route on a knob at
         * 1 left the step at 0 and the note stuck at full level. */
        decay_steps[i] = (1.0f - sustain_now) / (decay_len * 0.001f * SF_SAMPLE_RATE);
        sustain_voice[i] = sustain_now;
        /* Glide length in samples for a Time glide; Rate mode works out its
         * length from the interval when a slide starts. */
        glide_samples[i] = inst->glide_ms * glide_factor * 0.001f * SF_SAMPLE_RATE;
        lfo1_inc_voice[i] = lfo1_inc * rate1;
        lfo2_inc_voice[i] = lfo2_inc * rate2;
        lfo1_amp_voice[i] = amp1;
        lfo2_amp_voice[i] = amp2;
        cycle_amp_voice[i] = ampc;
    }
    /* Rise, Fall and Hold can be modulated per voice, so each voice gets its
     * own steps. Env mode runs Rise/Fall as an ADSR's attack, decay and
     * release. */
    float cycle_rise_step[SF_MAX_VOICES];
    float cycle_fall_step[SF_MAX_VOICES];
    float cycle_hold_arg[SF_MAX_VOICES];
    sf_cycle_env_t cycle_env[SF_MAX_VOICES];
    for (int i = 0; i < SF_MAX_VOICES; ++i) {
        if (!inst->voices[i].active) continue;
        cycle_rise_step[i] = 1.0f / (cycle_rise_voice[i] * 0.001f * SF_SAMPLE_RATE);
        cycle_fall_step[i] = 1.0f / (cycle_fall_voice[i] * 0.001f * SF_SAMPLE_RATE);
        cycle_hold_arg[i] = inst->cycle_mode == CYCLE_ENV
            ? cycle_hold_voice[i] : cycle_hold_voice[i] * 0.001f * SF_SAMPLE_RATE;
        if (inst->cycle_mode != CYCLE_ENV) continue;
        /* The hardware's own centre value (32767/65535, stored here as 0.5)
         * is still Default; anything above it switches. */
        int quick_rise = inst->cycle_rise_curve > 0.5f;
        int perc_fall = inst->cycle_fall_curve > 0.5f;
        sf_cycle_env_t *ce = &cycle_env[i];
        float rise_len = sf_env_shape_setup(&ce->rise, ENV_ATTACK, quick_rise, cycle_rise_voice[i]);
        float fall_len = sf_env_shape_setup(&ce->fall, ENV_DECAY, perc_fall, cycle_fall_voice[i]);
        float release_len = sf_env_shape_setup(&ce->release, ENV_RELEASE, perc_fall,
                                               sf_env_release_for_decay(cycle_fall_voice[i]));
        ce->rise_step = 1.0f / (rise_len * 0.001f * SF_SAMPLE_RATE);
        ce->fall_step = (1.0f - sf_clampf(cycle_hold_arg[i], 0.0f, 1.0f))
                      / (fall_len * 0.001f * SF_SAMPLE_RATE);
        ce->release_samples = release_len * 0.001f * SF_SAMPLE_RATE;
        if (ce->release_samples < 1.0f) ce->release_samples = 1.0f;
    }
    float osc1_tune_ratio = powf(2.0f, inst->osc1_tune / 12.0f);
    float osc2_tune_ratio = powf(2.0f, inst->osc2_tune / 12.0f);

    {
        sf_voice_t *lead = NULL;
        for (int i = 0; i < SF_MAX_VOICES; ++i) {
            if (!inst->voices[i].active) continue;
            if (!lead || inst->voices[i].age > lead->age) lead = &inst->voices[i];
        }
        inst->ui_source_live = lead != NULL;
        if (!lead) memset(inst->fx_route, 0, sizeof(inst->fx_route));
        if (lead) {
            float fx_source[MODSRC_COUNT];
            fx_source[MODSRC_CYCLE] = sf_voice_cycle_output(lead) * inst->cycle_amount;
            fx_source[MODSRC_ENV] = lead->last_env_out;
            fx_source[MODSRC_LFO1] = lfo_output(inst->lfo1_shape, inst->lfo1_retrig, lead->lfo1_phase, lead->lfo1_done, &inst->lfo1_shaper, &lead->lfo1_snh);
            fx_source[MODSRC_LFO2] = lfo_output(inst->lfo2_shape, inst->lfo2_retrig, lead->lfo2_phase, lead->lfo2_done, &inst->lfo2_shaper, &lead->lfo2_snh);
            fx_source[MODSRC_VELO_AT] = velo_at_source(inst, lead->velocity, lead->pressure);
            fx_source[MODSRC_WHEEL] = inst->wheel;
            fx_source[MODSRC_KEYTRACK] = keyboard_source(inst, lead);
            memcpy(inst->ui_source, fx_source, sizeof(fx_source));
            sf_mod_input_t fx_grid = {
                .column_dest = inst->mod_dest,
                .column_amount = &inst->mod_amount[0][0],
                .macro_dest = &inst->macro_dest[0][0],
                .macro_amount = &inst->macro_amount[0][0],
                .source_value = fx_source,
                .macro_value = inst->macros,
                .offset_dest = inst->seq.lane_dest,
                .offset_value = seq_lane,
                .offset_count = SF_SEQ_LANES
            };
            sf_mod_values_t fx;
            sf_mod_apply(&fx_grid, &fx);
            for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
                /* Routes move the MiniFreak knob positions; Intensity's is
                 * feedback / 0.95. */
                fx_time_voice[slot] = sf_clampf(sf_effect_mod_time(inst->fx_type[slot], inst->fx_subtype[slot],
                                                                inst->fx_time[slot],
                                                                fx.dest[DEST_FX1_TIME + slot * 3]),
                                             1.0f, SF_FX_TIME_MAX);
                fx_feedback_voice[slot] = sf_clampf(inst->fx_feedback[slot]
                                                 + fx.dest[DEST_FX1_INTENSITY + slot * 3] * 0.95f,
                                                 0.0f, 0.95f);
                fx_mix_voice[slot] = sf_effect_mod_mix(inst->fx_type[slot],
                                                       inst->fx_state.mix_smooth[slot],
                                                       fx.dest[DEST_FX1_AMOUNT + slot * 3]);
                for (int k = 0; k < 3; ++k)
                    inst->fx_route[slot][k] = fx.dest[DEST_FX1_TIME + slot * 3 + k];
            }
        }
    }

    /* A tick's ramp, LFO span and countdown all use the divider in force when
     * it runs, so changing Mod Rate mid-note only changes the next tick. */
    const int control_div = g_control_divs[clamp_enum((float)inst->control_rate,
                                                      SF_CONTROL_RATE_COUNT)];
    const float control_inv = 1.0f / (float)control_div;
    /* Routing only changes between blocks, so every voice's control ticks
     * share one plan of the live routes. */
    sf_mod_plan_t mod_plan;
    {
        sf_mod_input_t routing = {
            .column_dest = inst->mod_dest,
            .column_amount = &inst->mod_amount[0][0],
            .macro_dest = &inst->macro_dest[0][0],
            .macro_amount = &inst->macro_amount[0][0],
            .offset_dest = inst->seq.lane_dest,
            .offset_count = SF_SEQ_LANES
        };
        sf_mod_plan_build(&routing, &mod_plan);
    }
    for (int frame = 0; frame < frames; ++frame) {
        if (inst->seq.mode != SEQ_MODE_OFF || inst->seq.running || inst->seq.sounding_count) {
            sf_seq_event_t seq_events[SF_SEQ_EVENTS];
            int seq_count = sf_seq_advance(&inst->seq, tempo, SF_SAMPLE_RATE,
                                           seq_events, SF_SEQ_EVENTS);
            for (int e = 0; e < seq_count; ++e) {
                if (seq_events[e].on) start_note(inst, seq_events[e].note, seq_events[e].velocity, 0);
                else release_note(inst, seq_events[e].note, 0);
            }
            for (int lane = 0; lane < SF_SEQ_LANES; ++lane)
                seq_lane[lane] = sf_seq_lane_value(&inst->seq, lane);
        }
        for (int k = 0; k < SM_COUNT; ++k) sm_cur[k] += sm_inc[k];
        float g1_next = inst->lfo1_phase_global + lfo1_inc;
        float g2_next = inst->lfo2_phase_global + lfo2_inc;
        int g1_wrap = g1_next >= 1.0f, g2_wrap = g2_next >= 1.0f;
        if (g1_wrap) g1_next -= 1.0f;
        if (g2_wrap) g2_next -= 1.0f;
        float left = 0.0f;
        float right = 0.0f;
        for (int i = 0; i < SF_MAX_VOICES; ++i) {
            sf_voice_t *voice = &inst->voices[i];
            if (!voice->active) continue;

            float env = sf_voice_envelope_sample(voice, attack_steps[i], decay_steps[i],
                                                 sustain_voice[i], &attack_shapes[i],
                                                 &decay_shapes[i]);
            if (!voice->active) continue;
            /* The Cycling Envelope steps every sample like the amp envelope:
             * ticking it would round each stage to whole ticks, which
             * detunes a 1 ms Run/Loop cycle by up to a tenth. Only its value
             * is read at control rate. */
            int cycle_gated = voice->env_stage != ENV_RELEASE;
            float cycle = sf_voice_cycle_sample(voice, inst->cycle_mode, cycle_gated,
                                                cycle_rise_step[i], cycle_fall_step[i],
                                                cycle_hold_arg[i],
                                                inst->cycle_rise_curve,
                                                inst->cycle_fall_curve, &cycle_env[i]);

            sf_voice_ctl_t *ctl = &inst->voice_ctl[i];
            int fresh = !ctl->valid || ctl->age != voice->age;
            if (fresh || ctl->countdown <= 0) {
                /* Control tick: everything below covers the next
                 * control_div samples. */
                float target_hz = voice->base_note_hz * voice->unison_ratio;
                if (target_hz != voice->glide_target_hz) {
                    /* A new slide. MiniFreak V moves in a straight line in
                     * semitones: Time mode takes the Glide time whatever the
                     * interval, Rate mode covers 4.1 octaves per Glide time. */
                    voice->glide_target_hz = target_hz;
                    float octaves = voice->current_hz > 0.0f
                        ? fabsf(log2f(target_hz / voice->current_hz)) : 0.0f;
                    float length = inst->glide_mode == 1
                        ? glide_samples[i] * octaves / 4.1f : glide_samples[i];
                    voice->glide_ratio = length >= 1.0f && octaves > 1e-5f
                        ? exp2f(octaves / length) : 1.0f;
                }
                if (voice->glide_ratio > 1.0f && voice->current_hz > 0.0f) {
                    for (int s = 0; s < control_div && voice->current_hz != target_hz; ++s) {
                        if (voice->current_hz < target_hz) {
                            voice->current_hz *= voice->glide_ratio;
                            if (voice->current_hz >= target_hz) voice->current_hz = target_hz;
                        } else {
                            voice->current_hz /= voice->glide_ratio;
                            if (voice->current_hz <= target_hz) voice->current_hz = target_hz;
                        }
                    }
                } else {
                    voice->current_hz = target_hz;
                }

                /* LFOs: read at the tick, then stepped over it. A shared LFO
                 * follows the global phase, and draws a new Sample & Hold
                 * value whenever that wrapped since the last tick. */
                int snh1 = inst->lfo1_shape == LFO_SNH || inst->lfo1_shape == LFO_SLEW;
                int snh2 = inst->lfo2_shape == LFO_SNH || inst->lfo2_shape == LFO_SLEW;
                if (lfo_shared(inst->lfo1_retrig)) {
                    voice->lfo1_phase = inst->lfo1_phase_global;
                    if (!fresh && snh1 && ctl->lfo1_wraps != inst->lfo1_wraps)
                        voice->lfo1_snh.hold = snh_bipolar_random(&voice->lfo_rng);
                }
                if (lfo_shared(inst->lfo2_retrig)) {
                    voice->lfo2_phase = inst->lfo2_phase_global;
                    if (!fresh && snh2 && ctl->lfo2_wraps != inst->lfo2_wraps)
                        voice->lfo2_snh.hold = snh_bipolar_random(&voice->lfo_rng);
                }
                ctl->lfo1_wraps = inst->lfo1_wraps;
                ctl->lfo2_wraps = inst->lfo2_wraps;
                float lfo1 = lfo_output(inst->lfo1_shape, inst->lfo1_retrig, voice->lfo1_phase, voice->lfo1_done, &inst->lfo1_shaper, &voice->lfo1_snh);
                float lfo2 = lfo_output(inst->lfo2_shape, inst->lfo2_retrig, voice->lfo2_phase, voice->lfo2_done, &inst->lfo2_shaper, &voice->lfo2_snh);
                float span1 = lfo1_inc_voice[i] * (float)control_div;
                float span2 = lfo2_inc_voice[i] * (float)control_div;
                if (inst->lfo1_shape == LFO_SLEW) sf_mod_lfo_slew_advance(&voice->lfo1_snh, span1);
                if (inst->lfo2_shape == LFO_SLEW) sf_mod_lfo_slew_advance(&voice->lfo2_snh, span2);
                if (!lfo_shared(inst->lfo1_retrig)) {
                    float lp1 = voice->lfo1_phase + span1;
                    int lw1 = lp1 >= 1.0f;
                    if (lw1) lp1 -= floorf(lp1);
                    if (inst->lfo1_retrig == LFO_RETRIG_ONE) {
                        /* One: after its single cycle the LFO holds its end value. */
                        if (voice->lfo1_done) { lp1 = 0.0f; lw1 = 0; }
                        else if (lw1) { voice->lfo1_done = 1; lp1 = 0.0f; }
                    }
                    voice->lfo1_phase = lp1;
                    if (lw1 && snh1) voice->lfo1_snh.hold = snh_bipolar_random(&voice->lfo_rng);
                }
                if (!lfo_shared(inst->lfo2_retrig)) {
                    float lp2 = voice->lfo2_phase + span2;
                    int lw2 = lp2 >= 1.0f;
                    if (lw2) lp2 -= floorf(lp2);
                    if (inst->lfo2_retrig == LFO_RETRIG_ONE) {
                        if (voice->lfo2_done) { lp2 = 0.0f; lw2 = 0; }
                        else if (lw2) { voice->lfo2_done = 1; lp2 = 0.0f; }
                    }
                    voice->lfo2_phase = lp2;
                    if (lw2 && snh2) voice->lfo2_snh.hold = snh_bipolar_random(&voice->lfo_rng);
                }

                float pressure = voice->pressure;
                float source_value[MODSRC_COUNT];
                source_value[MODSRC_CYCLE] = cycle * inst->cycle_amount * cycle_amp_voice[i];
                source_value[MODSRC_ENV] = env
                    * (1.0f - inst->vel_env * (1.0f - voice->velocity));
                source_value[MODSRC_LFO1] = lfo1 * lfo1_amp_voice[i];
                source_value[MODSRC_LFO2] = lfo2 * lfo2_amp_voice[i];
                source_value[MODSRC_VELO_AT] = velo_at_source(inst, voice->velocity, pressure);
                source_value[MODSRC_WHEEL] = inst->wheel;
                source_value[MODSRC_KEYTRACK] = keyboard_source(inst, voice);
                sf_mod_input_t grid = {
                    .column_dest = inst->mod_dest,
                    .column_amount = &inst->mod_amount[0][0],
                    .macro_dest = &inst->macro_dest[0][0],
                    .macro_amount = &inst->macro_amount[0][0],
                    .source_value = source_value,
                    .macro_value = inst->macros,
                    .offset_dest = inst->seq.lane_dest,
                    .offset_value = seq_lane,
                    .offset_count = SF_SEQ_LANES
                };
                sf_mod_values_t routes;
                sf_mod_plan_apply(&mod_plan, &grid, &routes);
                float pitch_common = inst->lfo1_depth * lfo1 + pressure * 0.15f
                                   + routes.dest[DEST_PITCH] * SF_PITCH_MOD_SEMITONES;
                float pitch1 = quantize_pitch_mod(pitch_common
                    + routes.dest[DEST_PITCH1] * SF_PITCH_MOD_SEMITONES, inst->osc1_quantize);
                float pitch2 = quantize_pitch_mod(pitch_common
                    + routes.dest[DEST_PITCH2] * SF_PITCH_MOD_SEMITONES, inst->osc2_quantize);
                float bend_ratio = inst->bend_ratio[voice->channel];
                float hz1 = voice->current_hz * osc1_tune_ratio * bend_ratio
                          * semitone_ratio_approx(pitch1);
                float hz2 = voice->current_hz * osc2_tune_ratio * bend_ratio
                          * semitone_ratio_approx(pitch2);

                float target[CTL_COUNT];
                target[CTL_INC1] = sf_clampf(hz1 / SF_SAMPLE_RATE, 0.00001f, 0.49f);
                target[CTL_INC2] = sf_clampf(hz2 / SF_SAMPLE_RATE, 0.00001f, 0.49f);
                target[CTL_WAVE1] = sf_clampf(sm_cur[SM_W1] + routes.dest[DEST_WAVE1], 0.0f, 1.0f);
                target[CTL_WAVE2] = sf_clampf(sm_cur[SM_W2] + routes.dest[DEST_WAVE2], 0.0f, 1.0f);
                target[CTL_TIMBRE1] = sf_clampf(sm_cur[SM_T1] + routes.dest[DEST_TABLE1_POSITION], 0.0f, 1.0f);
                target[CTL_TIMBRE2] = sf_clampf(sm_cur[SM_T2] + routes.dest[DEST_TABLE2_POSITION], 0.0f, 1.0f);
                target[CTL_SHAPE1] = sf_clampf(sm_cur[SM_S1] + routes.dest[DEST_SHAPE1], 0.0f, 1.0f);
                target[CTL_SHAPE2] = sf_clampf(sm_cur[SM_S2] + routes.dest[DEST_SHAPE2], 0.0f, 1.0f);
                target[CTL_LEVEL1] = sf_clampf(sm_cur[SM_L1] + routes.dest[DEST_VOLUME1], 0.0f, 1.0f);
                target[CTL_LEVEL2] = sf_clampf(sm_cur[SM_L2] + routes.dest[DEST_MIX], 0.0f, 1.0f);
                target[CTL_RES] = sf_clampf(sm_cur[SM_RES] + routes.dest[DEST_RESONANCE], 0.0f, 1.0f);

                /* Velocity depths, in the MiniFreak Envelope page order. Vel to Env
                 * scales the envelope filter depth, Vel to VCF shifts the cutoff
                 * with the note, both reading 0 as "no velocity at all". */
                float env_amount = sf_clampf(inst->filter_env + routes.dest[DEST_ENV_AMOUNT],
                                          -1.0f, 1.0f);
                float vel_vcf_scale = 1.0f - inst->vel_vcf * (1.0f - voice->velocity);
                float modulation = env * vel_vcf_scale * env_amount
                                 + lfo2 * inst->lfo2_depth
                                 + (inst->cycle_filter_direct
                                    ? (cycle - 0.5f) * 2.0f * inst->cycle_amount : 0.0f)
                                 + pressure * 0.15f + routes.dest[DEST_CUTOFF];
                /* Cutoff is a normalised 0..1 log control spanning 20 Hz..18 kHz
                 * (log2(900) = 9.813781 octaves), so a full +1.0 modulation depth
                 * sweeps the full knob range, matching the other 0..1 destinations.
                 * As on MiniFreak V, the modulated position stops at the knob's
                 * ends. */
                float cutoff_pos = sf_clampf(sm_cur[SM_CUT] + modulation, 0.0f, 1.0f);
                float mod_cutoff = SF_CUTOFF_MIN_HZ * octave_ratio_approx(cutoff_pos * 9.813781f);
                target[CTL_FILTER_G] = fast_svf_g(mod_cutoff);
                target[CTL_FILTER_W] = fast_svf_w(mod_cutoff);
                {
                    float fm = sf_clampf(sm_cur[SM_FM] + routes.dest[DEST_FILTER_FM], -1.0f, 1.0f);
                    target[CTL_FM] = fm * fabsf(fm);
                }

                float vel_amp = 1.0f - inst->vel_vca * (1.0f - voice->velocity);
                /* Routes open or close the VCA (see route_value); it never goes past
                 * fully open. */
                float vca = sf_clampf(1.0f + routes.dest[DEST_VCA], 0.0f, 1.0f);
                float voice_output = sf_clampf(sm_cur[SM_OUT] + routes.dest[DEST_OUTPUT], 0.0f, 1.0f);
                target[CTL_AMP] = vel_amp * vca * voice_output;

                if (fresh) {
                    /* A new note starts on its own values, not a ramp. */
                    for (int k = 0; k < CTL_COUNT; ++k) {
                        ctl->value[k] = target[k];
                        ctl->step[k] = 0.0f;
                    }
                    ctl->osc1 = (sf_engine_voice_t){
                        .rng = &voice->rng,
                        .noise_state = &voice->noise_state1,
                        .noise_count = &voice->noise_count1,
                        .stack_phase = voice->super_phase1,
                        .chord_phase = voice->chord_phase1,
                        .vanalog_state = &voice->vanalog1,
                        .filter_state = &voice->osc1_filter,
                        .delay_buf = voice->delay1,
                        .delay_pos = &voice->delay_pos1,
                        .delay_low = &voice->delay_low1,
                        .excitation_left = &voice->karplus_excitation_left1,
                        .formant_dc = &voice->formant_dc1,
                        .modal_state = &voice->modal1,
                        .speech_state = &voice->speech1,
                        .apf = voice->apf,
                        .apf_fb = &voice->apf_fb,
                        .sh_hold = &voice->sh_hold,
                        .sh_count = &voice->sh_count
                    };
                    ctl->osc2 = (sf_engine_voice_t){
                        .rng = &voice->rng,
                        .noise_state = &voice->noise_state2,
                        .noise_count = &voice->noise_count2,
                        .stack_phase = voice->super_phase2,
                        .chord_phase = voice->chord_phase2,
                        .vanalog_state = &voice->vanalog2,
                        .filter_state = &voice->osc2_filter,
                        .multifilter_states = voice->multifilter,
                        .multifilter_lp6 = &voice->multifilter_lp6,
                        .delay_buf = voice->delay2,
                        .delay_pos = &voice->delay_pos2,
                        .delay_low = &voice->delay_low2,
                        .excitation_left = &voice->karplus_excitation_left2,
                        .formant_dc = &voice->formant_dc2,
                        .modal_state = &voice->modal2,
                        .speech_state = &voice->speech2,
                        .apf = voice->apf,
                        .apf_fb = &voice->apf_fb,
                        .sh_hold = &voice->sh_hold,
                        .sh_count = &voice->sh_count
                    };
                    ctl->valid = 1;
                    ctl->age = voice->age;
                } else {
                    for (int k = 0; k < CTL_COUNT; ++k)
                        ctl->step[k] = (target[k] - ctl->value[k]) * control_inv;
                }
                ctl->countdown = control_div;
            }
            --ctl->countdown;

            /* Audio rate: oscillators, DC blocker, filter and VCA. */
            const float *v = ctl->value;
            sf_engine_params_t osc1_params = {inst->osc1_mode, v[CTL_WAVE1], v[CTL_TIMBRE1],
                                               v[CTL_SHAPE1], inst->osc1_table};
            sf_engine_params_t osc2_params = {inst->osc2_mode, v[CTL_WAVE2], v[CTL_TIMBRE2],
                                               v[CTL_SHAPE2], inst->osc2_table};
            int gate = voice->env_stage != ENV_RELEASE;
            float inc1 = v[CTL_INC1];
            float inc2 = v[CTL_INC2];
            ctl->osc1.phase = voice->phase1;
            ctl->osc1.increment = inc1;
            ctl->osc1.sub_phase = voice->sub_phase1;
            ctl->osc1.gate = gate;
            ctl->osc2.phase = voice->phase2;
            ctl->osc2.increment = inc2;
            ctl->osc2.sub_phase = voice->sub_phase2;
            ctl->osc2.gate = gate;
            float raw1 = sf_engine1_sample(&osc1_params, &ctl->osc1, &ctl->osc2);
            float osc2_pre = 0.0f;
            float signal = sf_engine2_mix(&osc2_params, &ctl->osc2, &ctl->osc1,
                                          raw1, v[CTL_LEVEL1], v[CTL_LEVEL2], &osc2_pre);
            /* An 8 Hz DC blocker: some engine settings (V.Analog's narrowest
             * pulses) sit almost wholly off centre, which MiniFreak V's output
             * does not, and a Unison stack adds that offset up in phase. */
            {
                float blocked = signal - voice->dc_in + 0.99886f * voice->dc_out;
                voice->dc_in = signal;
                voice->dc_out = blocked;
                signal = blocked;
            }
            float filtered;
            if (v[CTL_FM] != 0.0f) {
                /* Filter FM. The source is taken before the level knobs (so a
                 * muted Osc 2 still modulates), DC-blocked like the audio and
                 * limited to +/-1. The filter runs twice per sample at double
                 * rate on the interpolated input and source, and the two
                 * outputs are averaged, which keeps the cutoff's sidebands
                 * from folding back as hard. */
                float fm_src = inst->filter_fm_src == 0 ? raw1
                             : inst->filter_fm_src == 1 ? osc2_pre
                             : 0.5f * (raw1 + osc2_pre);
                float fm_blocked = fm_src - voice->fm_dc_in + 0.99886f * voice->fm_dc_out;
                voice->fm_dc_in = fm_src;
                voice->fm_dc_out = fm_blocked;
                fm_src = sf_clampf(fm_blocked, -1.0f, 1.0f);
                float depth = v[CTL_FM] * SF_FILTER_FM_OCTAVES;
                float first = fm_filter_step(&voice->filter, inst->filter_mode, v[CTL_FILTER_W],
                                             depth * 0.5f * (voice->fm_prev_src + fm_src),
                                             v[CTL_RES], 0.5f * (voice->fm_prev_signal + signal));
                float second = fm_filter_step(&voice->filter, inst->filter_mode, v[CTL_FILTER_W],
                                              depth * fm_src, v[CTL_RES], signal);
                filtered = 0.5f * (first + second);
                voice->fm_prev_src = fm_src;
            } else {
                filtered = sf_filter_process(&voice->filter, inst->filter_mode,
                                             v[CTL_FILTER_G], v[CTL_RES], signal);
                voice->fm_prev_src = 0.0f;
                voice->fm_dc_in = 0.0f;
                voice->fm_dc_out = 0.0f;
            }
            voice->fm_prev_signal = signal;
            float sample = filtered * env * v[CTL_AMP] * voice_gain * voice->unison_gain;
            if (!isfinite(sample)) {
                /* Kill a voice that produced NaN/inf so it cannot latch. */
                sample = 0.0f;
                voice->filter.low = 0.0f;
                voice->filter.band = 0.0f;
                voice->active = 0;
            }
            float pan = pan_for_voice[i];
            left += sample * (0.5f - 0.5f * pan);
            right += sample * (0.5f + 0.5f * pan);

            voice->phase1 += inc1;
            if (voice->phase1 >= 1.0f) voice->phase1 -= 1.0f;
            voice->phase2 += inc2;
            if (voice->phase2 >= 1.0f) voice->phase2 -= 1.0f;
            voice->sub_phase1 += inc1 * 0.5f;
            if (voice->sub_phase1 >= 1.0f) voice->sub_phase1 -= 1.0f;
            voice->sub_phase2 += inc2 * 0.5f;
            if (voice->sub_phase2 >= 1.0f) voice->sub_phase2 -= 1.0f;
            for (int k = 0; k < CTL_COUNT; ++k) ctl->value[k] += ctl->step[k];
        }

        inst->lfo1_phase_global = g1_next;
        inst->lfo2_phase_global = g2_next;
        if (g1_wrap) ++inst->lfo1_wraps;
        if (g2_wrap) ++inst->lfo2_wraps;

        for (int slot = 0; slot < SF_FX_SLOTS; ++slot) {
            float next_l, next_r;
            sf_effect_process(&inst->fx_state, slot, inst->fx_type[slot],
                              inst->fx_subtype[slot],
                              fx_mix_voice[slot], fx_time_voice[slot],
                              fx_feedback_voice[slot], tempo,
                              left, right, &next_l, &next_r);
            left = next_l;
            right = next_r;
        }
        if (!isfinite(left) || !isfinite(right)) {
            /* Non-finite master sample: output silence and clear FX state so
             * a NaN in a feedback path cannot latch forever. */
            left = 0.0f;
            right = 0.0f;
            if (!fx_reset_done) {
                sf_effect_reset_nonfinite(&inst->fx_state);
                fx_reset_done = 1;
            }
        }
        left = soft_clip(left);
        right = soft_clip(right);
        if (inst->declick_left > 0) {
            float fade = (float)inst->declick_left * (1.0f / (float)SF_DECLICK_SAMPLES);
            left = sf_clampf(left + inst->declick[0] * fade, -1.0f, 1.0f);
            right = sf_clampf(right + inst->declick[1] * fade, -1.0f, 1.0f);
            --inst->declick_left;
        }
        inst->last_out[0] = left;
        inst->last_out[1] = right;
        /* Round, not truncate: truncation toward zero is a dead band that
         * puts distortion on the harmonics of quiet, dark sounds. */
        out[frame * 2] = (int16_t)lrintf(left * 32767.0f);
        out[frame * 2 + 1] = (int16_t)lrintf(right * 32767.0f);
    }
    sf_restore_fpcr(saved_fpcr);
#ifdef SF_PROFILE
    clock_gettime(CLOCK_MONOTONIC, &prof_t1);
    inst->cpu_us += 0.05f * ((float)(prof_t1.tv_sec - prof_t0.tv_sec) * 1e6f
                             + (float)(prof_t1.tv_nsec - prof_t0.tv_nsec) * 1e-3f - inst->cpu_us);
#endif
}

static plugin_api_v2_t g_plugin_api = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = sf_create_instance,
    .destroy_instance = sf_destroy_instance,
    .on_midi = sf_on_midi,
    .set_param = sf_set_param,
    .get_param = sf_get_param,
    .get_error = sf_get_error,
    .render_block = sf_render_block
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_plugin_api;
}
