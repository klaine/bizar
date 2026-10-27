#include "modulation.h"

#include <math.h>
#include <string.h>

/* The MiniFreak's Triangle is in phase with its Sine: it starts at 0, peaks at
 * a quarter cycle and bottoms out at three quarters. */
float sf_mod_triangle(float phase) {
    if (phase < 0.25f) return 4.0f * phase;
    if (phase < 0.75f) return 2.0f - 4.0f * phase;
    return 4.0f * phase - 4.0f;
}

void sf_shaper_default(sf_shaper_t *shaper) {
    static const float base[8] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 0.5f, 0.0f, -0.5f};
    shaper->mode = SHAPER_LEVELS;
    shaper->steps = 8;
    for (int i = 0; i < SF_SHAPER_STEPS; ++i) {
        shaper->amp[i] = i < 8 ? base[i] : 0.0f;
        shaper->curve[i] = 0.5f;
        shaper->slope[i] = 1.0f;
        shaper->on[i] = 1;
    }
}

int sf_shaper_is_default_extension(const sf_shaper_t *shaper) {
    if (shaper->mode != SHAPER_LEVELS || shaper->steps != 8) return 0;
    for (int i = 0; i < SF_SHAPER_STEPS; ++i) {
        if (shaper->curve[i] != 0.5f || shaper->slope[i] != 1.0f || !shaper->on[i])
            return 0;
        if (i >= 8 && shaper->amp[i] != 0.0f) return 0;
    }
    return 1;
}

/* Normalised exponential bend of the glide; 0.5 is linear. */
static float shaper_bend(float t, float curve) {
    float k = (curve - 0.5f) * 8.0f;
    if (k > -0.01f && k < 0.01f) return t;
    return (1.0f - expf(-k * t)) / (1.0f - expf(-k));
}

static float shaper_level(const sf_shaper_t *shaper, int step) {
    return shaper->on[step] ? shaper->amp[step] : 0.0f;
}

static float shaper_sample(const sf_shaper_t *shaper, float phase) {
    int steps = shaper->steps < 1 ? 1 : (shaper->steps > SF_SHAPER_STEPS
                                         ? SF_SHAPER_STEPS : shaper->steps);
    float position = phase * (float)steps;
    int step = (int)position;
    if (step < 0) step = 0;
    if (step >= steps) step = steps - 1;
    float fraction = position - (float)step;
    float current = shaper_level(shaper, step);
    float slope = shaper->slope[step];
    if (slope <= 0.0005f) return current;
    float start = 1.0f - slope;
    if (fraction <= start) return current;
    float t = (fraction - start) / slope;
    float next = shaper_level(shaper, (step + 1) % steps);
    return current + (next - current) * shaper_bend(t, shaper->curve[step]);
}

/* MiniFreak Shaper curve: bend strength per Curve knob position, measured.
 * Negative starts slowly, positive quickly; 0.35..0.65 is straight. */
static const float g_shaper_mf_k[21] = {
    -29.3f, -21.5f, -15.2f, -10.2f, -6.5f, -3.76f, -1.94f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f, 1.89f, 3.7f, 6.4f, 10.17f, 15.23f, 21.76f, 29.93f
};

static float shaper_mf_bend(float x, float curve) {
    float c = curve < 0.0f ? 0.0f : (curve > 1.0f ? 1.0f : curve);
    float pos = c * 20.0f;
    int i = (int)pos;
    if (i > 19) i = 19;
    float k = g_shaper_mf_k[i] + (g_shaper_mf_k[i + 1] - g_shaper_mf_k[i]) * (pos - (float)i);
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    if (k > -0.01f && k < 0.01f) return x;
    return (1.0f - expf(-k * x)) / (1.0f - expf(-k));
}

/* `phase` spans the whole pattern: the render loop slows a MiniFreak-mode
 * Shaper by its step count, so each step gets one LFO period. */
static float shaper_mf_sample(const sf_shaper_t *shaper, float phase) {
    int n = shaper->steps < 1 ? 1 : (shaper->steps > SF_SHAPER_STEPS
                                     ? SF_SHAPER_STEPS : shaper->steps);
    float position = phase * (float)n;
    int step = (int)position;
    if (step < 0) step = 0;
    if (step >= n) step = n - 1;
    /* A tied step continues the segment of the step it is tied to. */
    int start = step, back = 0;
    while (shaper->on[start] == 2 && back < n - 1) {
        start = (start + n - 1) % n;
        ++back;
    }
    int length = back + 1;
    while (length < n && shaper->on[(start + length) % n] == 2) ++length;
    if (shaper->on[start] == 0) return 0.0f;
    float t = ((float)back + (position - (float)step)) / (float)length;
    float curve = shaper->curve[start];
    int type = (int)(shaper->slope[start] * 4.0f);
    float shape;
    if (type <= 0) shape = shaper_mf_bend(t, curve);
    else if (type == 2) shape = shaper_mf_bend(t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t, curve);
    else shape = shaper_mf_bend(1.0f - t, curve);
    return shaper->amp[start] * shape;
}

float sf_mod_lfo_sample(int shape, float phase, const sf_shaper_t *shaper,
                        const sf_lfo_snh_t *snh) {
    if (shape == LFO_SINE) return sinf(6.2831853f * phase);
    /* MiniFreak's Saw is a falling sawtooth: it jumps to +1 and drops to -1.
     * (The manual calls the rising variant a ramp; use ExpRamp for a rising
     * unipolar shape.) Getting this backwards mutes any preset that gates an
     * oscillator volume with a retriggered Saw. */
    if (shape == LFO_SAW) return 1.0f - phase * 2.0f;
    if (shape == LFO_SQUARE) return phase < 0.5f ? 1.0f : -1.0f;
    if (shape == LFO_CUSTOM)
        return shaper->mode == SHAPER_MINIFREAK ? shaper_mf_sample(shaper, phase)
                                                : shaper_sample(shaper, phase);
    /* ExpSaw and ExpRamp are the MiniFreak's unipolar exponential waves,
     * measured as e^-5t: ExpSaw falls from 1 toward 0, and ExpRamp is the same
     * curve played backwards, so it rises slowly and then sharply. */
    if (shape == LFO_EXPSAW) return expf(-5.0f * phase);
    if (shape == LFO_EXPRAMP) return expf(5.0f * (phase - 1.0f));
    /* Sample & Hold holds the value sampled at the last cycle start; Slew S&H
     * glides toward it. The render loop re-samples `hold` whenever the LFO
     * phase wraps, so this function just reads/advances the state. */
    if (shape == LFO_SNH) return snh ? snh->hold : 0.0f;
    if (shape == LFO_SLEW) {
        return snh ? snh->slew : 0.0f;
    }
    return sf_mod_triangle(phase);
}

/* Advance the Slew S&H glide by one sample. Call exactly once per sample per
 * voice. The MiniFreak glides in a straight line at a fixed slope of 8 units
 * per LFO cycle, so a full -1..+1 jump takes a quarter cycle and a small one
 * proportionally less. */
void sf_mod_lfo_slew_advance(sf_lfo_snh_t *snh, float phase_inc) {
    float step = phase_inc * 8.0f;
    float diff = snh->hold - snh->slew;
    if (diff > step) snh->slew += step;
    else if (diff < -step) snh->slew -= step;
    else snh->slew = snh->hold;
}

/*
 * One route's contribution. Everything adds amount * source, except the
 * gains, measured on MiniFreak V: the VCA and the LFO 1 / LFO 2 / CycEnv
 * amplitudes. A positive route opens the gain with its source rather than
 * boosting it, so the gain is 1 - a + a * source (closed at source 0, unity at
 * source 1), while a negative route closes it by |a| * source. Both are
 * written as offsets from the resting 1. On the VCA a bipolar source (the
 * LFOs and the keyboard) is closed at -1 for a positive route and at +1 for a
 * negative one, so either sign gives 1 - |a| + a * source: a -0.5 Keyboard >
 * VCA route plays C3 4.5 dB down, and LFO > VCA at +/-0.5 swings the gain from
 * 0 to 1. The VCA routes' closing offsets add up but never take the resting
 * gain below 0 (sf_mod_apply caps them at 1): LFO 2 +0.63 with Keyboard -0.57
 * still opens to 0.74. The LFO / CycEnv amplitudes keep the plain law.
 */
static int is_gain_destination(int destination) {
    return destination == DEST_VCA || destination == DEST_LFO1_AMP
        || destination == DEST_LFO2_AMP || destination == DEST_CYCLE_AMP;
}

static float route_offset(int destination, float amount, int bipolar) {
    if (!is_gain_destination(destination)) return 0.0f;
    if (bipolar && destination == DEST_VCA) return fabsf(amount);
    return amount > 0.0f ? amount : 0.0f;
}

static int is_bipolar_source(int source) {
    return source == MODSRC_LFO1 || source == MODSRC_LFO2 || source == MODSRC_KEYTRACK;
}

/*
 * Sum the grid into one value per destination. Columns that are off contribute
 * nothing, and a source row that is zero in a column is skipped, so a typical
 * patch costs a handful of multiply-adds rather than the full grid. Several
 * columns - or a column and a macro slot - may share a destination, which is
 * what makes a source able to reach one target by more than one path.
 *
 * Two kinds of destination are themselves grid state, so they resolve first:
 * the macros (a column can sweep a macro) and the matrix cells (a column or a
 * macro slot can move how much a source row feeds a column, "Mod 4:3"). The
 * columns aimed at either are summed from their stored amounts, which is what
 * keeps a cell from modulating the very route that moves it. Every other
 * column then reads its amounts with the cell offsets added.
 */
static int is_cell(int destination) {
    return destination >= DEST_CELL_BASE && destination < DEST_CELL_END;
}

static int is_resolved_first(int destination) {
    return (destination >= DEST_MACRO1 && destination <= DEST_MACRO4)
        || is_cell(destination);
}

/* route_offset is non-zero only on the gains, so only they need settling. */
enum { GAIN_COUNT = 4 };
static const int g_gains[GAIN_COUNT] = {DEST_VCA, DEST_LFO1_AMP, DEST_LFO2_AMP, DEST_CYCLE_AMP};

static float gain_settle(int destination, float offset) {
    return destination == DEST_VCA && offset > 1.0f ? 1.0f : offset;
}

void sf_mod_apply(const sf_mod_input_t *in, sf_mod_values_t *out) {
    float offset[DEST_COUNT] = {0};
    /* This runs every control tick for every voice, so the 84 cells are only
     * scanned when something actually aimed at one. */
    int cells_aimed = 0;
    memset(out, 0, sizeof(*out));

    for (int column = 0; column < SF_MOD_COLUMNS; ++column) {
        int destination = in->column_dest[column];
        if (destination <= DEST_OFF || destination >= DEST_COUNT) continue;
        if (!is_resolved_first(destination)) continue;
        const float *amount = in->column_amount + column * MODSRC_COUNT;
        for (int source = 0; source < MODSRC_COUNT; ++source)
            if (amount[source] != 0.0f)
                out->dest[destination] += amount[source] * in->source_value[source];
        cells_aimed |= is_cell(destination);
    }

    /* Step-automation lanes are plain offsets in knob units, so a lane can
     * also push a macro or a cell. */
    for (int i = 0; i < in->offset_count; ++i) {
        int destination = in->offset_dest[i];
        if (destination <= DEST_OFF || destination >= DEST_COUNT) continue;
        out->dest[destination] += in->offset_value[i];
        cells_aimed |= is_cell(destination);
    }

    /* A macro is itself a destination: the grid columns above have already
     * summed into DEST_MACRO1..4, so fold that onto each macro's knob value
     * before its slots are applied. */
    float macro_value[SF_MACROS];
    for (int macro = 0; macro < SF_MACROS; ++macro) {
        float value = in->macro_value[macro] + out->dest[DEST_MACRO1 + macro];
        macro_value[macro] = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }

    /* The macros are the hardware's four destination slots per macro rather
     * than grid rows, so they add in separately. A slot may aim at a cell. */
    for (int slot = 0; slot < SF_MACROS * SF_MACRO_SLOTS; ++slot) {
        int destination = in->macro_dest[slot];
        if (destination <= DEST_OFF || destination >= DEST_COUNT) continue;
        if (destination >= DEST_MACRO1 && destination <= DEST_MACRO4) continue;
        out->dest[destination] += in->macro_amount[slot] * macro_value[slot / SF_MACRO_SLOTS];
        if (is_cell(destination)) cells_aimed = 1;
        else offset[destination] += route_offset(destination, in->macro_amount[slot], 0);
    }

    int cells_moved = 0;
    for (int cell = 0; cells_aimed && cell < DEST_CELL_COUNT && !cells_moved; ++cell)
        cells_moved = out->dest[DEST_CELL_BASE + cell] != 0.0f;

    for (int column = 0; column < SF_MOD_COLUMNS; ++column) {
        int destination = in->column_dest[column];
        if (destination <= DEST_OFF || destination >= DEST_COUNT) continue;
        if (is_resolved_first(destination)) continue;
        const float *amount = in->column_amount + column * MODSRC_COUNT;
        float sum = 0.0f;
        for (int source = 0; source < MODSRC_COUNT; ++source) {
            float depth = amount[source];
            if (cells_moved) {
                depth += out->dest[DEST_CELL_BASE + source * SF_MOD_COLUMNS + column];
                depth = depth < -1.0f ? -1.0f : (depth > 1.0f ? 1.0f : depth);
            }
            if (depth != 0.0f) {
                sum += depth * in->source_value[source];
                offset[destination] += route_offset(destination, depth,
                                                    is_bipolar_source(source));
            }
        }
        out->dest[destination] += sum;
    }
    for (int g = 0; g < GAIN_COUNT; ++g) {
        int destination = g_gains[g];
        if (offset[destination] > 0.0f)
            out->dest[destination] -= gain_settle(destination, offset[destination]);
    }
}

/* Every step below mirrors sf_mod_apply in the same order, so the floats sum
 * the same way. Without a cell destination no cell can move, so each column
 * reads its stored amounts unclamped, as sf_mod_apply does then. */
void sf_mod_plan_build(const sf_mod_input_t *in, sf_mod_plan_t *plan) {
    float offset[DEST_COUNT] = {0};
    memset(plan, 0, sizeof(*plan));
    plan->fallback = in->offset_count > SF_MOD_PLAN_OFFSETS;

    for (int column = 0; column < SF_MOD_COLUMNS; ++column) {
        int destination = in->column_dest[column];
        if (destination <= DEST_OFF || destination >= DEST_COUNT) continue;
        if (!is_resolved_first(destination)) continue;
        if (is_cell(destination)) plan->fallback = 1;
        const float *amount = in->column_amount + column * MODSRC_COUNT;
        for (int source = 0; source < MODSRC_COUNT; ++source)
            if (amount[source] != 0.0f)
                plan->macro_routes[plan->macro_route_count++] =
                    (sf_mod_plan_route_t){(unsigned char)destination, (unsigned char)source,
                                          amount[source]};
    }

    for (int i = 0; i < in->offset_count && !plan->fallback; ++i) {
        int destination = in->offset_dest[i];
        if (destination <= DEST_OFF || destination >= DEST_COUNT) continue;
        if (is_cell(destination)) plan->fallback = 1;
        plan->offsets[plan->offset_count++] =
            (sf_mod_plan_route_t){(unsigned char)destination, (unsigned char)i, 0.0f};
    }

    for (int slot = 0; slot < SF_MACROS * SF_MACRO_SLOTS; ++slot) {
        int destination = in->macro_dest[slot];
        if (destination <= DEST_OFF || destination >= DEST_COUNT) continue;
        if (destination >= DEST_MACRO1 && destination <= DEST_MACRO4) continue;
        if (is_cell(destination)) plan->fallback = 1;
        plan->slots[plan->slot_count++] = (sf_mod_plan_route_t){
            (unsigned char)destination, (unsigned char)(slot / SF_MACRO_SLOTS),
            in->macro_amount[slot]};
        offset[destination] += route_offset(destination, in->macro_amount[slot], 0);
    }

    for (int column = 0; column < SF_MOD_COLUMNS; ++column) {
        int destination = in->column_dest[column];
        if (destination <= DEST_OFF || destination >= DEST_COUNT) continue;
        if (is_resolved_first(destination)) continue;
        const float *amount = in->column_amount + column * MODSRC_COUNT;
        int count = 0;
        for (int source = 0; source < MODSRC_COUNT; ++source) {
            if (amount[source] == 0.0f) continue;
            plan->columns[plan->column_count].routes[count++] =
                (sf_mod_plan_route_t){(unsigned char)destination, (unsigned char)source,
                                      amount[source]};
            offset[destination] += route_offset(destination, amount[source],
                                                is_bipolar_source(source));
        }
        if (count == 0) continue;
        plan->columns[plan->column_count].dest = destination;
        plan->columns[plan->column_count].count = count;
        ++plan->column_count;
    }

    for (int g = 0; g < GAIN_COUNT; ++g) {
        int destination = g_gains[g];
        if (offset[destination] <= 0.0f) continue;
        plan->gain_dest[plan->gain_count] = destination;
        plan->gain_offset[plan->gain_count++] = gain_settle(destination, offset[destination]);
    }
}

void sf_mod_plan_apply(const sf_mod_plan_t *plan, const sf_mod_input_t *in,
                       sf_mod_values_t *out) {
    if (plan->fallback) {
        sf_mod_apply(in, out);
        return;
    }
    const float *source_value = in->source_value;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < plan->macro_route_count; ++i) {
        const sf_mod_plan_route_t *route = &plan->macro_routes[i];
        out->dest[route->dest] += route->amount * source_value[route->source];
    }
    for (int i = 0; i < plan->offset_count; ++i)
        out->dest[plan->offsets[i].dest] += in->offset_value[plan->offsets[i].source];

    float macro_value[SF_MACROS];
    for (int macro = 0; macro < SF_MACROS; ++macro) {
        float value = in->macro_value[macro] + out->dest[DEST_MACRO1 + macro];
        macro_value[macro] = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }
    for (int i = 0; i < plan->slot_count; ++i) {
        const sf_mod_plan_route_t *slot = &plan->slots[i];
        out->dest[slot->dest] += slot->amount * macro_value[slot->source];
    }

    for (int c = 0; c < plan->column_count; ++c) {
        float sum = 0.0f;
        for (int i = 0; i < plan->columns[c].count; ++i) {
            const sf_mod_plan_route_t *route = &plan->columns[c].routes[i];
            sum += route->amount * source_value[route->source];
        }
        out->dest[plan->columns[c].dest] += sum;
    }
    for (int g = 0; g < plan->gain_count; ++g)
        out->dest[plan->gain_dest[g]] -= plan->gain_offset[g];
}
