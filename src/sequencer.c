#include "sequencer.h"
#include "dsp_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int seq_clamp_int(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

void sf_seq_init(sf_seq_t *seq) {
    memset(seq, 0, sizeof(*seq));
    seq->mode = SEQ_MODE_OFF;
    seq->arp_mode = ARP_UP;
    seq->division = 10; /* 1/16 */
    seq->gate = 0.5f;
    seq->length = 16;
    seq->rng = 0x9e3779b9u;
    for (int lane = 0; lane < SF_SEQ_LANES; ++lane) seq->lane_smooth[lane] = 1;
}

/* The division list is three entries per halving - dotted, straight, triplet -
 * with the straight values at indices 1, 4, 7, 10 and 13 (1/2 .. 1/32). */
float sf_seq_division_beats(int division) {
    int index = seq_clamp_int(division, 0, SF_SEQ_DIV_COUNT - 1);
    int group = index / 3;
    int kind = index - (3 * group + 1);
    float beats = 2.0f / (float)(1 << group);
    if (kind < 0) return beats * 1.5f;
    if (kind > 0) return beats * (2.0f / 3.0f);
    return beats;
}

static const char *seq_parse_int(const char *p, int *out) {
    int value = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (value < 100000) value = value * 10 + (*p - '0');
        ++p;
        ++digits;
    }
    *out = digits ? value : 0;
    return p;
}

void sf_seq_set_data(sf_seq_t *seq, const char *data) {
    memset(seq->steps, 0, sizeof(seq->steps));
    if (!data) return;
    int step = 0;
    const char *p = data;
    while (*p && step < SF_SEQ_MAX_STEPS) {
        sf_seq_step_t *current = &seq->steps[step];
        while (*p && *p != ',') {
            int pitch, velocity, length;
            const char *start = p;
            p = seq_parse_int(p, &pitch);
            if (*p == '.') ++p;
            p = seq_parse_int(p, &velocity);
            if (*p == '.') ++p;
            p = seq_parse_int(p, &length);
            if (p == start) { ++p; continue; } /* skip junk */
            if (current->count < SF_SEQ_LAYERS) {
                int n = current->count++;
                current->pitch[n] = (uint8_t)seq_clamp_int(pitch, 0, 127);
                current->velocity[n] = (uint8_t)seq_clamp_int(velocity, 1, 127);
                current->length[n] = (uint8_t)seq_clamp_int(length, 0, 127);
            }
            while (*p && *p != ',' && *p != '+') ++p;
            if (*p == '+') ++p;
        }
        if (*p == ',') ++p;
        ++step;
    }
}

int sf_seq_get_data(const sf_seq_t *seq, char *buf, int buf_len) {
    int last = seq->length - 1;
    for (int i = 0; i < SF_SEQ_MAX_STEPS; ++i)
        if (seq->steps[i].count && i > last) last = i;
    last = seq_clamp_int(last, 0, SF_SEQ_MAX_STEPS - 1);
    int written = 0;
    if (buf_len < 1) return -1;
    for (int i = 0; i <= last; ++i) {
        const sf_seq_step_t *step = &seq->steps[i];
        if (i > 0) {
            if (written + 1 >= buf_len) return -1;
            buf[written++] = ',';
        }
        for (int n = 0; n < step->count; ++n) {
            int w = snprintf(buf + written, (size_t)(buf_len - written), "%s%d.%d.%d",
                             n ? "+" : "", step->pitch[n], step->velocity[n],
                             step->length[n]);
            if (w < 0 || written + w >= buf_len) return -1;
            written += w;
        }
    }
    buf[written] = '\0';
    return written;
}

void sf_seq_set_lane_data(sf_seq_t *seq, int lane, const char *data) {
    if (lane < 0 || lane >= SF_SEQ_LANES) return;
    float *values = seq->lane_value[lane];
    memset(values, 0, sizeof(seq->lane_value[lane]));
    if (!data) return;
    const char *p = data;
    for (int step = 0; *p && step < SF_SEQ_MAX_STEPS; ++step) {
        char *end;
        float value = strtof(p, &end);
        if (end != p && value == value) /* not NaN */
            values[step] = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
        p = end;
        while (*p && *p != ',') ++p; /* skip junk */
        if (*p == ',') ++p;
    }
}

int sf_seq_get_lane_data(const sf_seq_t *seq, int lane, char *buf, int buf_len) {
    if (buf_len < 1) return -1;
    buf[0] = '\0';
    if (lane < 0 || lane >= SF_SEQ_LANES) return 0;
    const float *values = seq->lane_value[lane];
    int last = -1;
    for (int i = 0; i < SF_SEQ_MAX_STEPS; ++i)
        if (values[i] != 0.0f) last = i;
    int written = 0;
    for (int i = 0; i <= last; ++i) {
        int w = values[i] != 0.0f
            ? snprintf(buf + written, (size_t)(buf_len - written), "%s%.4g", i ? "," : "", values[i])
            : snprintf(buf + written, (size_t)(buf_len - written), "%s", i ? "," : "");
        if (w < 0 || written + w >= buf_len) return -1;
        written += w;
    }
    return written;
}

int sf_seq_lane_in_use(const sf_seq_t *seq, int lane) {
    if (lane < 0 || lane >= SF_SEQ_LANES) return 0;
    if (seq->lane_dest[lane] != 0) return 1;
    for (int i = 0; i < SF_SEQ_MAX_STEPS; ++i)
        if (seq->lane_value[lane][i] != 0.0f) return 1;
    return 0;
}

float sf_seq_lane_value(const sf_seq_t *seq, int lane) {
    if (lane < 0 || lane >= SF_SEQ_LANES || seq->lane_dest[lane] == 0) return 0.0f;
    if (seq->mode != SEQ_MODE_SEQ || !seq->running || seq->tick == 0) return 0.0f;
    unsigned length = (unsigned)seq_clamp_int(seq->length, 1, SF_SEQ_MAX_STEPS);
    unsigned step = (seq->tick - 1u) % length;
    const float *values = seq->lane_value[lane];
    float value = values[step];
    if (!seq->lane_smooth[lane] || seq->step_duration <= 0.0) return value;
    double fraction = 1.0 - seq->countdown / seq->step_duration;
    fraction = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
    return value + (values[(step + 1u) % length] - value) * (float)fraction;
}

void sf_seq_key_down(sf_seq_t *seq, int note, int velocity) {
    sf_seq_key_up(seq, note);
    if (seq->held_count >= SF_SEQ_HELD) {
        memmove(&seq->held_note[0], &seq->held_note[1],
                sizeof(int) * (SF_SEQ_HELD - 1));
        memmove(&seq->held_velocity[0], &seq->held_velocity[1],
                sizeof(int) * (SF_SEQ_HELD - 1));
        --seq->held_count;
    }
    seq->held_note[seq->held_count] = note;
    seq->held_velocity[seq->held_count] = velocity;
    ++seq->held_count;
}

void sf_seq_key_up(sf_seq_t *seq, int note) {
    for (int i = 0; i < seq->held_count; ++i) {
        if (seq->held_note[i] != note) continue;
        for (int j = i; j + 1 < seq->held_count; ++j) {
            seq->held_note[j] = seq->held_note[j + 1];
            seq->held_velocity[j] = seq->held_velocity[j + 1];
        }
        --seq->held_count;
        return;
    }
}

static void seq_emit(sf_seq_event_t *events, int *count, int max_events,
                     int on, int note, int velocity) {
    if (*count >= max_events) return;
    events[*count].on = on;
    events[*count].note = note;
    events[*count].velocity = velocity;
    ++*count;
}

static void seq_release_sounding(sf_seq_t *seq, sf_seq_event_t *events,
                                 int *count, int max_events) {
    for (int i = 0; i < seq->sounding_count; ++i)
        seq_emit(events, count, max_events, 0, seq->sounding_note[i], 0);
    seq->sounding_count = 0;
}

int sf_seq_panic(sf_seq_t *seq, sf_seq_event_t *events, int max_events) {
    int count = 0;
    seq_release_sounding(seq, events, &count, max_events);
    seq->held_count = 0;
    seq->running = 0;
    seq->tick = 0;
    seq->countdown = 0.0;
    return count;
}

/* Start a note: retrigger it if the same pitch is already sounding. */
static void seq_start_note(sf_seq_t *seq, sf_seq_event_t *events, int *count,
                           int max_events, int note, int velocity,
                           double length_samples) {
    note = seq_clamp_int(note, 0, 127);
    for (int i = 0; i < seq->sounding_count; ++i) {
        if (seq->sounding_note[i] != note) continue;
        seq_emit(events, count, max_events, 0, note, 0);
        seq->sounding_note[i] = seq->sounding_note[seq->sounding_count - 1];
        seq->sounding_left[i] = seq->sounding_left[seq->sounding_count - 1];
        --seq->sounding_count;
        break;
    }
    seq_emit(events, count, max_events, 1, note, velocity);
    if (seq->sounding_count < SF_SEQ_SOUNDING) {
        seq->sounding_note[seq->sounding_count] = note;
        seq->sounding_left[seq->sounding_count] = length_samples < 1.0 ? 1.0 : length_samples;
        ++seq->sounding_count;
    }
}

static void seq_fire_pattern(sf_seq_t *seq, sf_seq_event_t *events, int *count,
                             int max_events, double step_samples) {
    int length = seq_clamp_int(seq->length, 1, SF_SEQ_MAX_STEPS);
    const sf_seq_step_t *step = &seq->steps[seq->tick % (unsigned)length];
    int transpose = seq->held_note[seq->held_count - 1] - SF_SEQ_ROOT_NOTE;
    for (int n = 0; n < step->count; ++n) {
        double steps_long = (double)step->length[n] + (double)seq->gate;
        seq_start_note(seq, events, count, max_events, (int)step->pitch[n] + transpose,
                       step->velocity[n], steps_long * step_samples);
    }
}

static void seq_fire_arp(sf_seq_t *seq, sf_seq_event_t *events, int *count,
                         int max_events, double step_samples) {
    int pool_note[SF_SEQ_HELD], pool_velocity[SF_SEQ_HELD];
    int pool = seq->held_count;
    memcpy(pool_note, seq->held_note, sizeof(int) * (size_t)pool);
    memcpy(pool_velocity, seq->held_velocity, sizeof(int) * (size_t)pool);
    if (seq->arp_mode != ARP_PLAYED) {
        for (int i = 1; i < pool; ++i) { /* ascending by pitch */
            int note = pool_note[i], velocity = pool_velocity[i], j = i - 1;
            while (j >= 0 && pool_note[j] > note) {
                pool_note[j + 1] = pool_note[j];
                pool_velocity[j + 1] = pool_velocity[j];
                --j;
            }
            pool_note[j + 1] = note;
            pool_velocity[j + 1] = velocity;
        }
    }
    double length_samples = (double)seq->gate * step_samples;
    if (seq->arp_mode == ARP_CHORD || seq->arp_mode == ARP_POLY) {
        for (int i = 0; i < pool; ++i)
            seq_start_note(seq, events, count, max_events, pool_note[i],
                           pool_velocity[i], length_samples);
        return;
    }
    int octaves = seq_clamp_int(seq->arp_octaves, 0, 3) + 1;
    int total = pool * octaves;
    unsigned t = seq->tick;
    int index;
    switch (seq->arp_mode) {
    case ARP_DOWN:
        index = total - 1 - (int)(t % (unsigned)total);
        break;
    case ARP_UPDOWN: {
        int period = total > 1 ? 2 * total - 2 : 1;
        int c = (int)(t % (unsigned)period);
        index = c < total ? c : period - c;
        break;
    }
    case ARP_RANDOM:
    case ARP_RANDOM2:
        index = (int)(sf_xorshift32(&seq->rng) % (unsigned)total);
        break;
    default: /* Up, Played */
        index = (int)(t % (unsigned)total);
        break;
    }
    int voice = index % pool;
    int octave = index / pool;
    seq_start_note(seq, events, count, max_events, pool_note[voice] + 12 * octave,
                   pool_velocity[voice], length_samples);
}

int sf_seq_advance(sf_seq_t *seq, float bpm, float sample_rate,
                   sf_seq_event_t *events, int max_events) {
    int count = 0;
    /* Note lengths run out first so a gate of 100% re-triggers cleanly. */
    for (int i = 0; i < seq->sounding_count;) {
        seq->sounding_left[i] -= 1.0;
        if (seq->sounding_left[i] <= 0.0) {
            seq_emit(events, &count, max_events, 0, seq->sounding_note[i], 0);
            seq->sounding_note[i] = seq->sounding_note[seq->sounding_count - 1];
            seq->sounding_left[i] = seq->sounding_left[seq->sounding_count - 1];
            --seq->sounding_count;
        } else {
            ++i;
        }
    }

    if (seq->mode == SEQ_MODE_OFF || seq->held_count == 0) {
        if (seq->running) {
            seq_release_sounding(seq, events, &count, max_events);
            seq->running = 0;
            seq->tick = 0;
        }
        return count;
    }
    if (!seq->running) {
        seq->running = 1;
        seq->tick = 0;
        seq->countdown = 0.0;
    }

    seq->countdown -= 1.0;
    if (seq->countdown > 0.0) return count;

    if (bpm < 20.0f) bpm = 20.0f;
    double step_samples = 60.0 / (double)bpm
                        * (double)sf_seq_division_beats(seq->division)
                        * (double)sample_rate;
    if (step_samples < 1.0) step_samples = 1.0;

    if (seq->mode == SEQ_MODE_SEQ) seq_fire_pattern(seq, events, &count, max_events, step_samples);
    else seq_fire_arp(seq, events, &count, max_events, step_samples);

    /* Swing delays every second tick by lengthening the step before it. */
    double swing = (double)(seq->swing < 0.0f ? 0.0f : (seq->swing > 1.0f ? 1.0f : seq->swing)) * 0.5;
    seq->step_duration = step_samples * ((seq->tick & 1u) ? 1.0 - swing : 1.0 + swing);
    seq->countdown += seq->step_duration;
    ++seq->tick;
    return count;
}
