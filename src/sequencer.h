#ifndef BIZAR_SEQUENCER_H
#define BIZAR_SEQUENCER_H

#include <stdint.h>

/*
 * The built-in step sequencer and arpeggiator. It is clocked from the host
 * tempo and started by held keys: with Mode Seq the stored pattern plays,
 * transposed by the last key held relative to note 60; with Mode Arp the held
 * keys are arpeggiated. It only produces note events; the plugin feeds them to
 * the voice allocator. No allocation, no I/O.
 */

#define SF_SEQ_MAX_STEPS 64
#define SF_SEQ_LAYERS 6
#define SF_SEQ_HELD 16
#define SF_SEQ_SOUNDING 32
#define SF_SEQ_EVENTS 32
#define SF_SEQ_ROOT_NOTE 60
#define SF_SEQ_DIV_COUNT 15
#define SF_SEQ_DATA_MAX 5120
#define SF_SEQ_LANES 4
#define SF_SEQ_LANE_DATA_MAX 768

enum {
    SEQ_MODE_OFF = 0,
    SEQ_MODE_ARP = 1,
    SEQ_MODE_SEQ = 2
};

/* Arp_Mode order from the MiniFreak bank (best fit; see
 * scripts/generate_minifreak_bank.py). Random 2 is a second random flavour and
 * Chord/Poly play every held note on each tick. */
enum {
    ARP_UP = 0, ARP_DOWN = 1, ARP_UPDOWN = 2, ARP_RANDOM = 3,
    ARP_PLAYED = 4, ARP_RANDOM2 = 5, ARP_CHORD = 6, ARP_POLY = 7,
    ARP_MODE_COUNT = 8
};

typedef struct {
    uint8_t count;
    uint8_t pitch[SF_SEQ_LAYERS];
    uint8_t velocity[SF_SEQ_LAYERS];
    uint8_t length[SF_SEQ_LAYERS];
} sf_seq_step_t;

typedef struct {
    int on;        /* 1 = note on, 0 = note off */
    int note;
    int velocity;
} sf_seq_event_t;

typedef struct {
    /* Patch settings. */
    int mode;
    int arp_mode;
    int arp_octaves;   /* 0..3 extra octaves */
    int division;      /* 0..14, see sf_seq_division_beats */
    float gate;        /* 0..1 fraction of a step */
    float swing;       /* 0..1 */
    int length;        /* 1..64 */
    sf_seq_step_t steps[SF_SEQ_MAX_STEPS];
    /* Step automation, the MiniFreak's modulation sequence: up to four lanes,
     * each pointed at a modulation destination (DEST_*, 0 = off) with one
     * bipolar offset per step in knob units (-1..1, 0 = no change). A smooth
     * lane glides from each step's value to the next across the step; a
     * stepped lane holds it. Lanes play with the pattern in Seq mode only. */
    int lane_dest[SF_SEQ_LANES];
    int lane_smooth[SF_SEQ_LANES];
    float lane_value[SF_SEQ_LANES][SF_SEQ_MAX_STEPS];

    /* Runtime state. */
    int held_note[SF_SEQ_HELD];
    int held_velocity[SF_SEQ_HELD];
    int held_count;
    int running;
    double countdown;      /* samples until the next tick */
    double step_duration;  /* samples the current tick lasts, swing included */
    unsigned tick;         /* ticks since the run started */
    int sounding_note[SF_SEQ_SOUNDING];
    double sounding_left[SF_SEQ_SOUNDING];
    int sounding_count;
    uint32_t rng;
} sf_seq_t;

void sf_seq_init(sf_seq_t *seq);
/* Beats per tick for a division index (1/2D .. 1/32T). */
float sf_seq_division_beats(int division);

/* Parse/format the compact pattern string ("60.100.0,,63.90.0+67.90.0").
 * Parsing is forgiving: malformed entries become rests. */
void sf_seq_set_data(sf_seq_t *seq, const char *data);
/* Returns the length written, or -1 if the buffer is too small. */
int sf_seq_get_data(const sf_seq_t *seq, char *buf, int buf_len);

/* Parse/format one automation lane's step values ("0.25,,-0.5"); an empty
 * entry is 0. Returns the length written, or -1 if the buffer is too small. */
void sf_seq_set_lane_data(sf_seq_t *seq, int lane, const char *data);
int sf_seq_get_lane_data(const sf_seq_t *seq, int lane, char *buf, int buf_len);
/* True when a lane has a destination or any nonzero step. */
int sf_seq_lane_in_use(const sf_seq_t *seq, int lane);
/* The lane's current offset: 0 unless the pattern is playing in Seq mode. */
float sf_seq_lane_value(const sf_seq_t *seq, int lane);

void sf_seq_key_down(sf_seq_t *seq, int note, int velocity);
void sf_seq_key_up(sf_seq_t *seq, int note);
/* Drop held keys and stop, emitting note-offs for whatever is sounding. */
int sf_seq_panic(sf_seq_t *seq, sf_seq_event_t *events, int max_events);
/* Advance one sample. Returns the number of events written (<= max_events). */
int sf_seq_advance(sf_seq_t *seq, float bpm, float sample_rate,
                   sf_seq_event_t *events, int max_events);

#endif
