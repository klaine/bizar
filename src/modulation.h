#ifndef BIZAR_MODULATION_H
#define BIZAR_MODULATION_H

/*
 * The modulation grid, modelled on the MiniFreak's matrix: a fixed set of
 * source rows feeding destination columns, one destination per column. A preset
 * says what each column points at and how much of each source row reaches it,
 * which is how the hardware stores it too - so an imported preset needs no
 * translation into "routes", and no preset can run out of them. The bank's
 * widest preset modulates twelve destinations at once, which is why there are
 * twelve columns.
 */

/* Source rows, in the order the hardware lists them. Velo + AT is velocity and
 * aftertouch summed into one row, exactly as the hardware names it. */
enum {
    MODSRC_CYCLE = 0,
    MODSRC_ENV = 1,
    MODSRC_LFO1 = 2,
    MODSRC_LFO2 = 3,
    MODSRC_VELO_AT = 4,
    MODSRC_WHEEL = 5,
    MODSRC_KEYTRACK = 6,
    MODSRC_COUNT = 7
};

#define SF_MOD_COLUMNS 12
#define SF_MACROS 4
#define SF_MACRO_SLOTS 4

/* LFO shapes. The first four predate the MiniFreak bank import and keep their
 * ordinal so older state blobs land on the same shape; the MiniFreak-only
 * waves (Sine, S&H, Slew S&H, ExpSaw, ExpRamp) are appended after them. */
enum {
    LFO_TRIANGLE = 0,
    LFO_SAW = 1,
    LFO_SQUARE = 2,
    LFO_CUSTOM = 3,
    LFO_SINE = 4,
    LFO_SNH = 5,
    LFO_SLEW = 6,
    LFO_EXPSAW = 7,
    LFO_EXPRAMP = 8,
    LFO_SHAPE_COUNT = 9
};

/* Destination columns. Everything up to DEST_WAVE2 predates the grid and keeps
 * its index so older state blobs still land on the right destination; the rest
 * were appended with the grid and are appended to from here on. */
enum {
    DEST_OFF = 0,
    DEST_PITCH = 1,
    DEST_CUTOFF = 2,
    DEST_SHAPE1 = 3,
    DEST_SHAPE2 = 4,
    DEST_MIX = 5,
    DEST_RESONANCE = 6,
    DEST_OUTPUT = 7,
    DEST_TABLE1_POSITION = 8,
    DEST_TABLE2_POSITION = 9,
    DEST_WAVE1 = 10,
    DEST_WAVE2 = 11,
    DEST_PITCH1 = 12,
    DEST_PITCH2 = 13,
    DEST_VOLUME1 = 14,
    DEST_VCA = 15,
    DEST_ENV_AMOUNT = 16,
    /* Appended with the matrix import. The envelope times, glide and the LFO
     * rates and amplitudes are resolved once per block per voice, because they
     * shape the stage steps rather than a per-sample value. */
    DEST_GLIDE = 17,
    DEST_ATTACK = 18,
    DEST_DECAY = 19,
    DEST_SUSTAIN = 20,
    DEST_RELEASE = 21,
    DEST_LFO1_RATE = 22,
    DEST_LFO2_RATE = 23,
    DEST_LFO1_AMP = 24,
    DEST_LFO2_AMP = 25,
    DEST_CYCLE_AMP = 26,
    /* The three effect slots. The chain is shared rather than per voice, so a
     * route into one takes its value from the most recently started voice. */
    DEST_FX1_TIME = 27,
    DEST_FX1_INTENSITY = 28,
    DEST_FX1_AMOUNT = 29,
    DEST_FX2_TIME = 30,
    DEST_FX2_INTENSITY = 31,
    DEST_FX2_AMOUNT = 32,
    DEST_FX3_TIME = 33,
    DEST_FX3_INTENSITY = 34,
    DEST_FX3_AMOUNT = 35,
    /* The four macros. A column pointed at one moves that macro's value, and
     * the macro's own slots then follow it, so a source can sweep every
     * destination a macro drives. Macro slots cannot target a macro (no loops),
     * and a macro moved this way never changes the stored knob. */
    DEST_MACRO1 = 36,
    DEST_MACRO2 = 37,
    DEST_MACRO3 = 38,
    DEST_MACRO4 = 39,
    /* The Cycling Envelope's knobs, resolved per voice once per block like the
     * envelope times. */
    DEST_CYCLE_RISE = 40,
    DEST_CYCLE_FALL = 41,
    DEST_CYCLE_HOLD = 42,
    /* The matrix's own cells, as on the MiniFreak ("Mod 4:3" is row 4, LFO 2,
     * of column 3): a column or a macro slot aimed at one moves how much that
     * source row feeds that column. Row-major, row first as the hardware
     * names them, so cell (row, column) is DEST_CELL_BASE + row * 12 + column
     * with both counted from 0. */
    DEST_CELL_BASE = 43,
    DEST_CELL_COUNT = MODSRC_COUNT * SF_MOD_COLUMNS,
    DEST_CELL_END = DEST_CELL_BASE + DEST_CELL_COUNT,
    /* Appended after the cells so saved patches keep their cell indices. Adds
     * to the Filter FM knob. */
    DEST_FILTER_FM = DEST_CELL_END,
    DEST_COUNT = DEST_FILTER_FM + 1
};

typedef struct {
    const int *column_dest;      /* SF_MOD_COLUMNS entries */
    const float *column_amount;  /* SF_MOD_COLUMNS * MODSRC_COUNT, row major */
    const int *macro_dest;       /* SF_MACROS * SF_MACRO_SLOTS, row major */
    const float *macro_amount;   /* SF_MACROS * SF_MACRO_SLOTS, row major */
    const float *source_value;   /* MODSRC_COUNT entries, indexed as above */
    const float *macro_value;    /* SF_MACROS entries */
    /* Optional direct offsets added onto destinations before the macros are
     * folded in: the sequencer's step-automation lanes. NULL/0 = none. */
    const int *offset_dest;      /* offset_count entries */
    const float *offset_value;   /* offset_count entries */
    int offset_count;
} sf_mod_input_t;

typedef struct {
    float dest[DEST_COUNT];
} sf_mod_values_t;

typedef struct {
    float hold;   /* S&H / Slew S&H: the random value sampled for this cycle */
    float slew;   /* Slew S&H: the one-pole-glided value moving toward hold */
} sf_lfo_snh_t;

/* The Shaper LFO: up to 16 equal steps across one cycle, as on the MiniFreak.
 * `amp` is bipolar (-1..1) and a muted step (on == 0) sits at 0. The last
 * `slope` fraction of each step glides to the next step's level (0 = a hard
 * staircase, 1 = a continuous ramp); `curve` bends that glide (0.5 = linear).
 * The default - 8 steps, slope 1, curve 0.5 - is the original eight-point
 * linear curve, so patches saved before this model keep their sound. */
#define SF_SHAPER_STEPS 16
/* Shaper modes. Levels is Bizar's own editor model above. MiniFreak
 * is the hardware's, measured from MiniFreak V: every step lasts a whole LFO
 * period (so the pattern repeats every `steps` periods) and draws its own
 * segment from 0 to its level `amp`. `slope` picks the segment, read as
 * floor(slope * 4): rise, fall, rise-then-fall, fall. `curve` bends it, with
 * a dead zone around 0.5. `on` is 0 rest, 1 play, 2 tie (stretch the
 * previous step's segment over this one). */
enum { SHAPER_LEVELS = 0, SHAPER_MINIFREAK = 1 };
typedef struct {
    int mode;
    int steps;
    float amp[SF_SHAPER_STEPS];
    float curve[SF_SHAPER_STEPS];
    float slope[SF_SHAPER_STEPS];
    unsigned char on[SF_SHAPER_STEPS];
} sf_shaper_t;

void sf_shaper_default(sf_shaper_t *shaper);
/* True when the shaper has only the original eight-point content, so state
 * writers can skip the extended keys. */
int sf_shaper_is_default_extension(const sf_shaper_t *shaper);

float sf_mod_triangle(float phase);
float sf_mod_lfo_sample(int shape, float phase, const sf_shaper_t *shaper,
                        const sf_lfo_snh_t *snh);
void sf_mod_lfo_slew_advance(sf_lfo_snh_t *snh, float phase_inc);
void sf_mod_apply(const sf_mod_input_t *in, sf_mod_values_t *out);

/*
 * sf_mod_apply with the routing resolved ahead of time. The routing (column
 * and macro destinations and amounts, lane destinations) only changes between
 * blocks, so build a plan once per block and apply it per voice per control
 * tick: only the live routes are walked, and the gain offsets, which depend on
 * the amounts alone, are settled in advance. The result is bit-identical to
 * sf_mod_apply. A patch that aims anything at a matrix cell falls back to it.
 */
#define SF_MOD_PLAN_OFFSETS 8
typedef struct {
    unsigned char dest;
    unsigned char source;   /* a source row, a macro, or a lane index */
    float amount;
} sf_mod_plan_route_t;

typedef struct {
    int fallback;
    int macro_route_count, offset_count, slot_count, column_count;
    sf_mod_plan_route_t macro_routes[SF_MOD_COLUMNS * MODSRC_COUNT];
    sf_mod_plan_route_t offsets[SF_MOD_PLAN_OFFSETS];
    sf_mod_plan_route_t slots[SF_MACROS * SF_MACRO_SLOTS];
    struct {
        int dest;
        int count;
        sf_mod_plan_route_t routes[MODSRC_COUNT];
    } columns[SF_MOD_COLUMNS];
    int gain_count;
    int gain_dest[4];
    float gain_offset[4];
} sf_mod_plan_t;

/* `in` supplies the routing; its values are not read. */
void sf_mod_plan_build(const sf_mod_input_t *in, sf_mod_plan_t *plan);
/* `in` supplies the values (sources, macros, lane offsets) for this tick. */
void sf_mod_plan_apply(const sf_mod_plan_t *plan, const sf_mod_input_t *in,
                       sf_mod_values_t *out);

#endif
