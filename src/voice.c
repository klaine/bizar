#include "voice.h"
#include "dsp_common.h"

#include <math.h>
#include <string.h>

static float midi_hz(int note) {
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

/*
 * Envelope stage shapes, measured from MiniFreak V.
 *
 * The attack is an RC charge toward a target above full scale, stopped when
 * it reaches the top: k = 1.55 for Default (it overshoots by 27%, so the
 * rise is nearly straight) and 4.5 for Quick. Default decay and release are
 * plain exponentials; a stage's length is its time to -60 dB (k = ln 1000).
 *
 * Percussive decay and release are a fast drop (a ~10-25 ms time constant)
 * plus a slow tail, and the balance between them depends on the knob: short
 * settings are almost all drop, long ones almost all tail. The tables are
 * per knob position, so the stage's nominal (Default) time is mapped back to
 * the knob first.
 */
#define SF_ENV_K_ATTACK 1.55f
#define SF_ENV_K_QUICK 4.5f
#define SF_ENV_K_EXP 6.9077553f
/* The tables below are sampled at these knob positions. */
#define SF_ENV_POINTS 12
static const float g_env_x[SF_ENV_POINTS] = {
    0.0f, 0.05f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f
};
/* Attack: time to reach the peak, in ms. */
static const float g_env_attack_ms[SF_ENV_POINTS] = {
    0.5f, 4.0f, 9.0f, 30.0f, 80.0f, 204.0f, 505.0f, 1150.0f, 2360.0f, 4950.0f,
    10050.0f, 19800.0f
};
/* Default time constants in ms: decay, then release (which is a little
 * slower than decay through the middle of the knob). */
static const float g_env_tau[2][SF_ENV_POINTS] = {
    {0.3f, 0.92f, 2.45f, 21.0f, 53.0f, 105.0f, 194.0f, 392.0f, 864.0f,
     1769.0f, 3486.0f, 6657.0f},
    {3.2f, 5.2f, 14.5f, 41.3f, 79.2f, 141.5f, 250.0f, 516.0f, 1034.0f,
     1987.0f, 3700.0f, 6740.0f}
};
/* Percussive: fast-component weight, fast and slow time constants (ms). */
static const float g_env_perc_a[2][SF_ENV_POINTS] = {
    {0.70f, 0.97f, 0.90f, 0.98f, 0.944f, 0.883f, 0.784f, 0.657f, 0.484f,
     0.334f, 0.211f, 0.12f},
    {0.5f, 0.997f, 0.991f, 0.965f, 0.91f, 0.855f, 0.754f, 0.613f, 0.458f,
     0.325f, 0.21f, 0.134f}
};
static const float g_env_perc_tf[2][SF_ENV_POINTS] = {
    {0.62f, 1.62f, 3.15f, 9.5f, 12.1f, 14.0f, 16.2f, 18.2f, 19.9f, 20.6f,
     18.7f, 16.0f},
    {4.2f, 6.8f, 9.7f, 12.9f, 15.8f, 17.7f, 20.0f, 22.3f, 24.0f, 24.7f,
     24.0f, 22.0f}
};
static const float g_env_perc_ts[2][SF_ENV_POINTS] = {
    {0.62f, 3.1f, 7.1f, 189.0f, 277.0f, 377.0f, 527.0f, 753.0f, 1200.0f,
     1912.0f, 3246.0f, 5882.0f},
    {4.2f, 83.0f, 151.0f, 246.0f, 347.0f, 433.0f, 595.0f, 875.0f, 1342.0f,
     2060.0f, 3415.0f, 5913.0f}
};

/* Fractional table position of a Default time constant (log interpolation). */
static float env_tau_position(const float *tau, float t) {
    if (t <= tau[0]) return 0.0f;
    for (int i = 1; i < SF_ENV_POINTS; ++i) {
        if (t <= tau[i] && tau[i] > tau[i - 1])
            return (float)(i - 1) + logf(t / tau[i - 1]) / logf(tau[i] / tau[i - 1]);
    }
    return (float)(SF_ENV_POINTS - 1);
}

static float env_table_at(const float *table, float pos, int geometric) {
    int i = (int)pos;
    if (i >= SF_ENV_POINTS - 1) return table[SF_ENV_POINTS - 1];
    float f = pos - (float)i;
    if (geometric) return table[i] * powf(table[i + 1] / table[i], f);
    return table[i] + (table[i + 1] - table[i]) * f;
}

/* The MiniFreak modulates a stage time by moving its knob: a +0.25 route
 * turns Decay 0.5 into Decay 0.75, which is six times longer. Map the time to
 * its knob position, add the modulation there and scale the time by the
 * ratio the knob curve gives, so an unmodulated time is left exactly as is. */
float sf_env_time_mod(int stage, float ms, float amount) {
    if (amount == 0.0f) return ms;
    const float *table = stage == ENV_ATTACK ? g_env_attack_ms
                       : g_env_tau[stage == ENV_RELEASE];
    float value = stage == ENV_ATTACK ? ms : ms / SF_ENV_K_EXP;
    float pos = env_tau_position(table, value);
    int i = (int)pos;
    if (i >= SF_ENV_POINTS - 1) i = SF_ENV_POINTS - 2;
    float x = g_env_x[i] + (pos - (float)i) * (g_env_x[i + 1] - g_env_x[i]);
    float moved = x + amount;
    if (moved < 0.0f) moved = 0.0f;
    if (moved > 1.0f) moved = 1.0f;
    int j = 0;
    while (j < SF_ENV_POINTS - 2 && moved > g_env_x[j + 1]) ++j;
    float moved_pos = (float)j + (moved - g_env_x[j]) / (g_env_x[j + 1] - g_env_x[j]);
    return ms * env_table_at(table, moved_pos, 1) / env_table_at(table, pos, 1);
}

float sf_env_shape_setup(sf_env_shape_t *shape, int stage, int curve, float ms) {
    if (ms < 0.01f) ms = 0.01f;
    shape->a = 0.0f;
    shape->kf = shape->ks = 0.0f;
    if (stage == ENV_ATTACK) {
        shape->k = curve == ENV_CURVE_QUICK ? SF_ENV_K_QUICK : SF_ENV_K_ATTACK;
        shape->norm = 1.0f / (1.0f - expf(-shape->k));
        return ms;
    }
    if (curve != ENV_CURVE_QUICK) {
        shape->k = SF_ENV_K_EXP;
        shape->norm = 1.0f / (1.0f - expf(-SF_ENV_K_EXP));
        return ms;
    }
    int r = stage == ENV_RELEASE;
    float pos = env_tau_position(g_env_tau[r], ms / SF_ENV_K_EXP);
    float a = env_table_at(g_env_perc_a[r], pos, 0);
    float tf = env_table_at(g_env_perc_tf[r], pos, 1);
    float ts = env_table_at(g_env_perc_ts[r], pos, 1);
    /* Run until the larger component is down 60 dB. */
    float t_fast = a > 0.001f ? tf * logf(a / 0.001f) : 0.0f;
    float t_slow = a < 0.999f ? ts * logf((1.0f - a) / 0.001f) : 0.0f;
    float len = t_fast > t_slow ? t_fast : t_slow;
    if (len < 0.01f) len = 0.01f;
    shape->a = a;
    shape->kf = len / tf;
    shape->ks = len / ts;
    float end = a * expf(-shape->kf) + (1.0f - a) * expf(-shape->ks);
    shape->norm = 1.0f / (1.0f - end);
    return len;
}

/* Fraction of the way to the stage target at linear position t. A shape with
 * k == 0 and no Percussive pair is a straight line. */
static float env_travel(const sf_env_shape_t *shape, float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (shape->a > 0.0f) {
        float left = shape->a * expf(-shape->kf * t)
                   + (1.0f - shape->a) * expf(-shape->ks * t);
        return (1.0f - left) * shape->norm;
    }
    if (shape->k < 0.001f) return t;
    return (1.0f - expf(-shape->k * t)) * shape->norm;
}

/* Velocity extends the release by exp(SF_VEL_TIME_K * vel_time * velocity).
 * The scale is resolved here because the release step is fixed when the
 * note ends. */
static void voice_begin_release(sf_voice_t *voice, float release_ms, float vel_time,
                                int release_curve) {
    float scale = expf(SF_VEL_TIME_K * vel_time * voice->velocity) * voice->release_scale;
    float length = sf_env_shape_setup(&voice->release_shape, ENV_RELEASE,
                                      release_curve, release_ms * scale);
    voice->env_stage = ENV_RELEASE;
    /* Start from the last audible (shaped) value, not the raw linear phase,
     * so releasing during Attack/Decay does not jump. env becomes the
     * release stage's own linear phase, running from release_from to 0. */
    voice->release_from = voice->last_env_out;
    voice->env = voice->last_env_out;
    voice->release_step = voice->last_env_out
        / (length * 0.001f * SF_SAMPLE_RATE);
    if (voice->release_step < 0.0000001f) voice->release_step = 0.0000001f;
}

/* Steal order: a released voice before a held one, the quietest released
 * voice first, then the oldest held one. env is a stage's own phase (an
 * Attack starts at 0), so it cannot rank voices: comparing it made a chord's
 * later notes steal the ones started a moment before them. */
static int steal_before(const sf_voice_t *a, const sf_voice_t *b) {
    int a_released = a->env_stage == ENV_RELEASE;
    int b_released = b->env_stage == ENV_RELEASE;
    if (a_released != b_released) return a_released;
    if (a_released && a->last_env_out != b->last_env_out)
        return a->last_env_out < b->last_env_out;
    return a->age < b->age;
}

sf_voice_t *sf_voice_allocate(sf_voice_t voices[SF_MAX_VOICES]) {
    int candidate = -1;
    for (int i = 0; i < SF_MAX_VOICES; ++i) {
        sf_voice_t *voice = &voices[i];
        if (!voice->active) return voice;
        /* A voice already fading out for a steal holds a note that has not
         * started yet; taking it again would drop that note. */
        if (voice->steal_pending) continue;
        if (candidate < 0 || steal_before(voice, &voices[candidate])) candidate = i;
    }
    if (candidate < 0) {
        /* Every voice is mid-steal: overwrite the oldest pending one. */
        candidate = 0;
        for (int i = 1; i < SF_MAX_VOICES; ++i)
            if (voices[i].age < voices[candidate].age) candidate = i;
    }
    return &voices[candidate];
}

static void voice_start_now(sf_voice_t *voice, int index, int note, int velocity,
                            int channel, int legato,
                            const sf_voice_start_params_t *params);

#define SF_STEAL_FADE_SECONDS 0.003f

void sf_voice_start(sf_voice_t *voice, int index, int note, int velocity,
                    int channel, int legato,
                    const sf_voice_start_params_t *params) {
    if (!legato && voice->active && (voice->steal_pending
                                     || voice->last_env_out > 0.001f)) {
        /* Stealing an audible voice: fade it out first, start afterwards. */
        int was_pending = voice->steal_pending;
        voice->steal_pending = 1;
        voice->steal_note = note;
        voice->steal_velocity = velocity;
        voice->steal_channel = channel;
        voice->steal_index = index;
        voice->steal_params = *params;
        if (!was_pending) {
            float from = voice->last_env_out;
            voice->env_stage = ENV_RELEASE;
            voice->release_from = from;
            voice->env = from;
            voice->release_shape = (sf_env_shape_t){0};   /* a straight fade */
            voice->release_step = from / (SF_STEAL_FADE_SECONDS * SF_SAMPLE_RATE);
            if (voice->release_step < 0.0000001f) voice->release_step = 0.0000001f;
        }
        return;
    }
    voice->steal_pending = 0;
    voice_start_now(voice, index, note, velocity, channel, legato, params);
}

int sf_unison_rank(int index, int voices) {
    /* Slots alternate below/above (after the centre voice when the count is
     * odd); the MiniFreak counts each pair above first. */
    if (voices & 1) return index == 0 ? 0 : ((index - 1) ^ 1) + 1;
    return index ^ 1;
}

static void voice_start_now(sf_voice_t *voice, int index, int note, int velocity,
                            int channel, int legato,
                            const sf_voice_start_params_t *params) {
    voice->active = 1;
    voice->note = note;
    voice->key = params->key >= 0 ? params->key : note;
    voice->channel = channel;
    voice->velocity = (float)velocity / 127.0f;
    voice->pressure = params->pressure;
    voice->age = ++(*params->age_counter);
    voice->base_note_hz = midi_hz(note);
    float excitation_period = SF_SAMPLE_RATE / voice->base_note_hz;
    if (excitation_period < 1.0f) excitation_period = 1.0f;
    voice->karplus_excitation_left1 = (unsigned)excitation_period;
    voice->karplus_excitation_left2 = (unsigned)excitation_period;
    /* MiniFreak V's unison, measured: the voices sit on whole steps either
     * side of the note (one on it when the count is odd), the step does not
     * depend on the count, and every voice plays 3 dB down. The step here is
     * Spread / 6, so a full six-voice stack spans +/- Spread / 2. The
     * MiniFreak's step runs up to 12 semitones (an octave per step), which is
     * where the fifths and octaves of its upper Spread range come from. */
    voice->unison_cents = 0.0f;
    voice->unison_gain = 1.0f;
    if (params->mode == VOICE_UNISON) {
        int odd = params->unison_voices & 1;
        int step = odd ? (index + 1) / 2 : index / 2 + 1;
        int below = odd ? (index & 1) : !(index & 1);
        voice->unison_cents = (below ? -1.0f : 1.0f) * (float)step
                            * params->spread_cents / 6.0f;
        voice->unison_gain = 0.70710678f;
    }
    voice->unison_ratio = powf(2.0f, voice->unison_cents / 1200.0f);
    if (voice->release_scale <= 0.0f) voice->release_scale = 1.0f;
    voice->random_bipolar = (float)(sf_xorshift32(&voice->rng) >> 8)
                          * (2.0f / 16777215.0f) - 1.0f;
    /* The Sample & Hold LFOs start from a random level so a note never opens
     * on a stale or zeroed hold. */
    voice->lfo_rng = params->lfo_seed ? params->lfo_seed : (sf_xorshift32(&voice->rng) | 1u);
    voice->lfo1_snh.hold = (float)(sf_xorshift32(&voice->lfo_rng) >> 8)
                         * (2.0f / 16777215.0f) - 1.0f;
    voice->lfo1_snh.slew = voice->lfo1_snh.hold;
    voice->lfo2_snh.hold = voice->lfo1_snh.hold;
    voice->lfo2_snh.slew = voice->lfo2_snh.hold;
    voice->target_hz = voice->base_note_hz * voice->unison_ratio;
    if (!legato || params->env_retrig == 0 || voice->current_hz <= 0.0f) {
        voice->current_hz = voice->target_hz;
        voice->glide_target_hz = voice->target_hz;
        voice->glide_ratio = 1.0f;
        voice->env = 0.0f;
        voice->last_env_out = 0.0f;
        voice->env_stage = ENV_ATTACK;
        /* Oscillator phase is CONDITIONAL. Free-running is what the hardware
         * does (95% of the MiniFreak's factory bank) and what stops stacked
         * notes from phase-locking into one loud transient; Trig restores the
         * reset-on-note behaviour for consistently shaped attacks. */
        if (params->osc_phase == 0) {
            voice->phase1 = 0.0f;
            voice->phase2 = 0.0f;
            voice->sub_phase1 = 0.0f;
            voice->sub_phase2 = 0.0f;
            voice->noise_state1 = 0.0f;
            voice->noise_state2 = 0.0f;
            voice->noise_count1 = 0;
            voice->noise_count2 = 0;
            for (int copy = 0; copy < SF_SUPERWAVE_COPIES; ++copy) {
                voice->super_phase1[copy] = 0.0f;
                voice->super_phase2[copy] = 0.0f;
            }
            voice->vanalog1 = (sf_vanalog_state_t){0};
            voice->vanalog2 = (sf_vanalog_state_t){0};
            for (int phase = 0; phase < SF_CHORD_PHASES; ++phase) {
                voice->chord_phase2[phase] = 0.0f;
                voice->chord_phase1[phase] = 0.0f;
            }
        } else if (params->mode == VOICE_UNISON) {
            /* Free-running unison voices start where their own oscillators
             * would be, not all at the phase they last stopped on; a stack
             * that starts in phase opens up to 15 dB louder than the
             * MiniFreak's. */
            const float to_phase = 1.0f / 16777216.0f;
            voice->phase1 = (float)(sf_xorshift32(&voice->rng) >> 8) * to_phase;
            voice->phase2 = (float)(sf_xorshift32(&voice->rng) >> 8) * to_phase;
            /* The sub oscillator, SuperWave's copies and the chord voices
             * keep phases of their own; left equal across the stack they
             * sum in phase (6 voices: +15.6 dB). */
            voice->sub_phase1 = (float)(sf_xorshift32(&voice->rng) >> 8) * to_phase;
            voice->sub_phase2 = (float)(sf_xorshift32(&voice->rng) >> 8) * to_phase;
            for (int copy = 0; copy < SF_SUPERWAVE_COPIES; ++copy) {
                voice->super_phase1[copy] = (float)(sf_xorshift32(&voice->rng) >> 8) * to_phase;
                voice->super_phase2[copy] = (float)(sf_xorshift32(&voice->rng) >> 8) * to_phase;
            }
            for (int phase = 0; phase < SF_CHORD_PHASES; ++phase) {
                voice->chord_phase1[phase] = (float)(sf_xorshift32(&voice->rng) >> 8) * to_phase;
                voice->chord_phase2[phase] = (float)(sf_xorshift32(&voice->rng) >> 8) * to_phase;
            }
        }
        if (params->lfo1_retrig == LFO_RETRIG_POLY || params->lfo1_retrig == LFO_RETRIG_ONE) {
            voice->lfo1_phase = 0.0f;
            voice->lfo1_done = 0;
        }
        if (params->lfo2_retrig == LFO_RETRIG_POLY || params->lfo2_retrig == LFO_RETRIG_ONE) {
            voice->lfo2_phase = 0.0f;
            voice->lfo2_done = 0;
        }
        voice->filter = (sf_filter_state_t){0};
        voice->dc_in = 0.0f;
        voice->dc_out = 0.0f;
        voice->fm_dc_in = 0.0f;
        voice->fm_dc_out = 0.0f;
        voice->fm_prev_signal = 0.0f;
        voice->fm_prev_src = 0.0f;
        voice->osc1_filter = (sf_filter_state_t){0};
        voice->osc2_filter = (sf_filter_state_t){0};
        /* The delay lines are cleared on every non-legato note so a stolen
         * voice cannot re-pitch whatever the previous note was still ringing.
         * Bounded work: 2 x SF_VOICE_DELAY stores, 32 KB, once per note-on. */
        for (int i = 0; i < SF_VOICE_DELAY; ++i) {
            voice->delay1[i] = 0.0f;
            voice->delay2[i] = 0.0f;
        }
        voice->delay_pos1 = 0;
        voice->delay_pos2 = 0;
        voice->delay_low1 = 0.0f;
        voice->delay_low2 = 0.0f;
        voice->formant_dc1 = 0.0f;
        voice->formant_dc2 = 0.0f;
        /* Modal is a struck model: clear its resonators even when oscillator
         * phase is free-running so the previous note cannot ring through. */
        voice->modal1 = (sf_modal_state_t){0};
        voice->modal2 = (sf_modal_state_t){0};
        /* Speech's glottal pulse is free-running, but the word starts over. */
        voice->speech1 = (sf_speech_state_t){0};
        voice->speech2 = (sf_speech_state_t){0};
        for (int stage = 0; stage < SF_PHASER_MAX_STAGES; ++stage) voice->apf[stage] = 0.0f;
        voice->apf_fb = 0.0f;
        memset(voice->multifilter, 0, sizeof(voice->multifilter));
        voice->multifilter_lp6 = 0.0f;
        voice->sh_hold = 0.0f;
        voice->sh_count = 0;
    }
    /* Env and Loop restart with every note, legato ones included: MiniFreak V
     * does this whatever the amp envelope's retrigger mode (measured with
     * Retrig Source on Poly or Mono Kbd, which 226 of the 239 Env and Loop
     * factory patches use), so a Mono or sequenced line re-opens its sweep
     * on each step. Run free-runs through notes, which is what makes it
     * usable as a never-resting modulation source. */
    if (params->cycle_mode != CYCLE_RUN) {
        voice->cycle_stage = CYCLE_RISE;
        voice->cycle_value = 0.0f;
        voice->cycle_out = 0.0f;
        voice->cycle_hold_left = 0.0f;
    }
}

void sf_voice_release_note(sf_voice_t voices[SF_MAX_VOICES], int note,
                           int channel, float release_ms, float vel_time,
                           int release_curve) {
    for (int i = 0; i < SF_MAX_VOICES; ++i) {
        sf_voice_t *voice = &voices[i];
        int steal_key = voice->steal_params.key >= 0 ? voice->steal_params.key
                                                     : voice->steal_note;
        if (voice->active && voice->steal_pending && steal_key == note
            && voice->steal_channel == channel) {
            /* Note-off before the deferred start: the new note never sounds,
             * and the voice it was waiting on goes back to an ordinary release
             * from where its fade had got to, instead of finishing a 5 ms cut.
             * A Mono sequencer re-uses one voice for every step, so its last
             * note-off lands here and used to chop the tail. */
            voice->steal_pending = 0;
            voice_begin_release(voice, release_ms, vel_time, release_curve);
            continue;
        }
        if (voice->active && voice->key == note && voice->channel == channel
            && voice->env_stage != ENV_RELEASE)
            voice_begin_release(voice, release_ms, vel_time, release_curve);
    }
}

void sf_voice_release_channel(sf_voice_t voices[SF_MAX_VOICES], int channel,
                             int immediate, float release_ms, float vel_time,
                             int release_curve) {
    for (int i = 0; i < SF_MAX_VOICES; ++i) {
        sf_voice_t *voice = &voices[i];
        if (!voice->active || (channel >= 0 && voice->channel != channel)) continue;
        if (immediate) {
            voice->steal_pending = 0;
            voice->active = 0;
            voice->env = 0.0f;
            voice->env_stage = ENV_OFF;
        } else if (voice->steal_pending) {
            voice->steal_pending = 0;
        } else if (voice->env_stage != ENV_RELEASE) {
            voice_begin_release(voice, release_ms, vel_time, release_curve);
        }
    }
}

float sf_voice_envelope_sample(sf_voice_t *voice, float attack_step,
                               float decay_step, float sustain,
                               const sf_env_shape_t *attack_shape,
                               const sf_env_shape_t *decay_shape) {
    float out = voice->env;
    switch (voice->env_stage) {
    case ENV_ATTACK:
        voice->env += attack_step;
        if (voice->env >= 1.0f) {
            voice->env = 1.0f;
            voice->env_stage = ENV_DECAY;
        }
        out = env_travel(attack_shape, voice->env);
        break;
    case ENV_DECAY: {
        voice->env -= decay_step;
        if (voice->env <= sustain) {
            voice->env = sustain;
            voice->env_stage = ENV_SUSTAIN;
        }
        float span = 1.0f - sustain;
        float t = span > 0.0001f ? (1.0f - voice->env) / span : 1.0f;
        out = 1.0f + (sustain - 1.0f) * env_travel(decay_shape, t);
        break;
    }
    case ENV_SUSTAIN:
        voice->env = sustain;
        out = sustain;
        break;
    case ENV_RELEASE: {
        voice->env -= voice->release_step;
        if (voice->env <= 0.0f) {
            voice->env = 0.0f;
            voice->env_stage = ENV_OFF;
            voice->active = 0;
            out = 0.0f;
            if (voice->steal_pending) {
                voice->steal_pending = 0;
                voice_start_now(voice, voice->steal_index, voice->steal_note,
                                voice->steal_velocity, voice->steal_channel, 0,
                                &voice->steal_params);
                voice->last_env_out = 0.0f;
            }
            break;
        }
        float from = voice->release_from > 0.0001f ? voice->release_from : 1.0f;
        float t = 1.0f - voice->env / from;
        out = from * (1.0f - env_travel(&voice->release_shape, t));
        break;
    }
    default:
        voice->env = 0.0f;
        out = 0.0f;
        break;
    }
    voice->last_env_out = out;
    return out;
}

/*
 * The Cycling Envelope: Rise -> Hold -> Fall, in MiniFreak's three modes.
 *
 * Env mode is a second ADSR, measured as one: Rise runs the attack curve,
 * Fall the decay curve toward Hold (its sustain level), and on note-off it
 * releases along the ADSR's release curve for the Fall knob. Each Curve knob
 * acts as the ADSR's two-way menu there: up to half is Default, above half
 * is Quick (Rise) or Percussive (Fall). The shapes come in `env`, built once
 * per block.
 *
 * Run and Loop cycle Rise -> Hold -> Fall regardless of the gate; they differ
 * only in whether a note restarts them (see sf_voice_start). `hold_value` is a
 * Hold time in samples there. Their stage steps stay linear and only the
 * reported value bends, so a shape change never alters how long a stage
 * lasts. The bend is a normalised exponential: k > 0 moves the stage quickly
 * at first, k < 0 slowly, and |k| < 0.001 is the linear default.
 */
/* Rise and Fall bend along the same exponential, measured as e^(22 t) at the
 * ends of the Curve knob. */
#define SF_CYCLE_CURVE_K 22.0f
static float cycle_bend(float value, float k, float *memo_k, float *memo_den) {
    if (value <= 0.0f) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    if (k > -0.001f && k < 0.001f) return value;
    /* The denominator only moves with the Curve knob. A zeroed memo holds
     * k = 0, which the linear case above has already taken. */
    if (*memo_k != k) {
        *memo_k = k;
        *memo_den = 1.0f - expf(-SF_CYCLE_CURVE_K * k);
    }
    return (1.0f - expf(-SF_CYCLE_CURVE_K * k * value)) / *memo_den;
}

float sf_voice_cycle_output(const sf_voice_t *voice) {
    return voice->cycle_out;
}

static float cycle_env_sample(sf_voice_t *voice, int gated, float sustain,
                              const sf_cycle_env_t *env) {
    if (sustain < 0.0f) sustain = 0.0f;
    if (sustain > 1.0f) sustain = 1.0f;
    if (!gated && voice->cycle_stage != CYCLE_IDLE && voice->cycle_stage != CYCLE_RELEASE) {
        voice->cycle_stage = CYCLE_RELEASE;
        voice->cycle_from = voice->cycle_out;
        voice->cycle_value = voice->cycle_out;
    }
    float out = 0.0f;
    switch (voice->cycle_stage) {
    case CYCLE_RISE:
        voice->cycle_value += env->rise_step;
        if (voice->cycle_value >= 1.0f) {
            voice->cycle_value = 1.0f;
            voice->cycle_stage = CYCLE_FALL;
        }
        out = env_travel(&env->rise, voice->cycle_value);
        break;
    case CYCLE_FALL: {
        voice->cycle_value -= env->fall_step;
        if (voice->cycle_value <= sustain) {
            voice->cycle_value = sustain;
            voice->cycle_stage = CYCLE_HOLD;
        }
        float span = 1.0f - sustain;
        float t = span > 0.0001f ? (1.0f - voice->cycle_value) / span : 1.0f;
        out = 1.0f + (sustain - 1.0f) * env_travel(&env->fall, t);
        break;
    }
    case CYCLE_HOLD:
        voice->cycle_value = sustain;
        out = sustain;
        break;
    case CYCLE_RELEASE: {
        float from = voice->cycle_from > 0.0001f ? voice->cycle_from : 1.0f;
        voice->cycle_value -= from / env->release_samples;
        if (voice->cycle_value <= 0.0f) {
            voice->cycle_value = 0.0f;
            voice->cycle_stage = CYCLE_IDLE;
            break;
        }
        out = from * (1.0f - env_travel(&env->release, 1.0f - voice->cycle_value / from));
        break;
    }
    default:
        voice->cycle_value = 0.0f;
        break;
    }
    voice->cycle_out = out;
    return out;
}

float sf_voice_cycle_sample(sf_voice_t *voice, int mode, int gated,
                            float rise_step, float fall_step,
                            float hold_value,
                            float rise_curve, float fall_curve,
                            const sf_cycle_env_t *env) {
    if (mode == CYCLE_ENV) return cycle_env_sample(voice, gated, hold_value, env);

    (void)gated;
    switch (voice->cycle_stage) {
    case CYCLE_RISE:
        voice->cycle_value += rise_step;
        if (voice->cycle_value >= 1.0f) {
            voice->cycle_value = 1.0f;
            voice->cycle_stage = CYCLE_HOLD;
            voice->cycle_hold_left = hold_value;
        }
        break;
    case CYCLE_HOLD:
        voice->cycle_hold_left -= 1.0f;
        if (voice->cycle_hold_left <= 0.0f) voice->cycle_stage = CYCLE_FALL;
        break;
    case CYCLE_FALL:
        voice->cycle_value -= fall_step;
        if (voice->cycle_value <= 0.0f) {
            voice->cycle_value = 0.0f;
            voice->cycle_stage = CYCLE_RISE;
        }
        break;
    default:
        /* Idle, or left in Release by a switch out of Env mode. */
        voice->cycle_value = 0.0f;
        voice->cycle_stage = CYCLE_RISE;
        break;
    }
    float value = voice->cycle_value;
    float out = value;
    if (voice->cycle_stage == CYCLE_RISE)
        out = cycle_bend(value, 2.0f * rise_curve - 1.0f,
                         &voice->cycle_bend_k[0], &voice->cycle_bend_den[0]);
    else if (voice->cycle_stage == CYCLE_FALL)
        out = 1.0f - cycle_bend(1.0f - value, 1.0f - 2.0f * fall_curve,
                                &voice->cycle_bend_k[1], &voice->cycle_bend_den[1]);
    voice->cycle_out = out;
    return out;
}

float sf_env_release_for_decay(float decay_ms) {
    float pos = env_tau_position(g_env_tau[0], decay_ms / SF_ENV_K_EXP);
    return SF_ENV_K_EXP * env_table_at(g_env_tau[1], pos, 1);
}
