#!/usr/bin/env python3
"""Import an Arturia MiniFreak bank export (.mnfx) as a Bizar preset bank.

The export is a ZIP of Boost text archives, one per preset, each holding ~2370
`<len> Key value` pairs. Values are normalised: a continuous control is 0..1 and
an enum is `index / (count - 1)`.

Engine indices are NOT documented anywhere Arturia publishes, so the tables below
were derived from the bank itself — preset descriptions name engines outright
("basic waves and chord osc types", "based on the karplus engine", "using the
Comb Filter of Oscillator 2 as a resonator", "the noise engine", "the VA
engine"), and the rest follow from those anchors plus the manual's section
order. Slots marked CONFIRMED are pinned by a description; the others are the
best fit and are listed in one place so they can be corrected without touching
anything else.

Only the parameters Bizar actually has are carried over. The MiniFreak's
modulation matrix is deliberately dropped (shaper curves are imported; see
import_shaper; the built-in sequencer/arpeggiator and its step automation
are imported; see import_sequencer and import_seq_automation): the matrix stores numeric destination IDs from a table Arturia does not publish,
so mapping them would be guesswork.

Still to import: the hardware's "Mod x:y" matrix outputs, "Vib AM", and the
discrete destinations (Osc Types, LFO Wave shapes). Together they are about 2%
of the bank's routes. Everything else in the matrix goes onto the grid.

Usage:  generate_minifreak_bank.py OUTPUT_HEADER [--bank PATH] [--report]
"""

import math
import argparse
import re
import sys
import zipfile
from pathlib import Path

PAIR = re.compile(r"(?<=\s)(\d+)\s+([A-Za-z_][A-Za-z0-9_]*)\s+([-+0-9][0-9.eE+-]*)(?=\s)")

# --- Modulation destinations -------------------------------------------------
# A route stores a 16-bit destination address, round(value * 65535), with 0x8000
# meaning "None".  Arturia does not publish the table, so it was recovered by
# pairing a MiniFreak V screenshot of a named preset (Enou) with that same
# preset's values here: its two macro slots and its matrix columns name their
# destinations on screen.  This is only the part that screenshot pins down — the
# addresses that came out (Osc1 Wave, Osc1 Volume, Osc2 Volume, Cutoff,
# Resonance, Attack, Decay) are in strict parameter order, which means the
# destination menu is ordered by address and one more screenshot of the open
# menu yields the rest.
MF_DEST_NAMES = {
    0x8000: "None",
    # Pinned twice: the Enou screenshot (its macro slots name four on screen)
    # and a probe export in which the matrix columns were assigned by hand.
    0x9004: "Osc1 Wave",
    0x9005: "Osc1 Timbre",
    0x9006: "Osc1 Shape",
    0x900A: "Osc1 Volume",
    0x900E: "Osc2 Wave",
    0x900F: "Osc2 Timbre",
    0x9010: "Osc2 Shape",
    0x9014: "Osc2 Volume",
    0x9015: "Glide",
    0x9019: "Cutoff",
    0x901A: "Reso",
    0x901B: "Env Amt",
    0x901C: "VCA",
    0x901D: "Attack",
    0x901E: "Decay",
    0x901F: "Sustain",
    0x9020: "Release",
    0x90C7: "LFO 1 Amp",
    0x90C8: "LFO 2 Amp",
    0x90C9: "CycEnv Amp",
}

# Destination address -> Bizar destination index, for the entries the
# grid actually implements today. Anything missing is left unrouted rather
# than guessed, so a preset never gains modulation it did not have.
MF_DEST_ORDINAL = {
    0x9004: 10,   # Osc1 Wave    -> Wave 1
    0x9005: 8,    # Osc1 Timbre  -> Timbre 1
    0x9006: 3,    # Osc1 Shape   -> Shape 1
    0x900A: 14,   # Osc1 Volume  -> Volume 1
    0x900E: 11,   # Osc2 Wave    -> Wave 2
    0x900F: 9,    # Osc2 Timbre  -> Timbre 2
    0x9010: 4,    # Osc2 Shape   -> Shape 2
    0x9014: 5,    # Osc2 Volume  -> Volume 2
    0x9015: 17,   # Glide
    0x9019: 2,    # Cutoff
    0x901A: 6,    # Reso
    0x901B: 16,   # Env Amt
    0x901C: 15,   # VCA
    0x901D: 18,   # Attack
    0x901E: 19,   # Decay
    0x901F: 20,   # Sustain
    0x9020: 21,   # Release
    # Probed on MiniFreak V: 0x9027/0x9028/0x9029 are the Cycling Envelope's
    # Rise, Fall and Hold (0x9028 used to be read as LFO 2 Rate), and the
    # free LFO rates are 0x9030 and 0x9037 (one seven-slot run per LFO; the
    # synced rates at 0x9031/0x9038 are not imported).
    0x9027: 40,   # CycEnv Rise
    0x9028: 41,   # CycEnv Fall
    0x9029: 42,   # CycEnv Hold
    0x9030: 22,   # LFO 1 Rate
    0x9037: 23,   # LFO 2 Rate
    # Measured on MiniFreak V: 0x90C4 moves only Osc 1's pitch and 0x90C5
    # only Osc 2's; 0x90C6 moved neither (nor any LFO or vibrato depth).
    0x90C4: 12,   # Pitch 1
    0x90C5: 13,   # Pitch 2
    0x90C7: 24,   # LFO 1 Amp
    0x90C8: 25,   # LFO 2 Amp
    0x90C9: 26,   # CycEnv Amp
    0xA002: 27,   # FX 1 Time
    0xA003: 28,   # FX 1 Intensity
    0xA004: 29,   # FX 1 Amount
    0xA00B: 30,   # FX 2 Time
    0xA00C: 31,   # FX 2 Intensity
    0xA00D: 32,   # FX 2 Amount
    0xA014: 33,   # FX 3 Time
    0xA015: 34,   # FX 3 Intensity
    0xA016: 35,   # FX 3 Amount
}

# The matrix is a grid of seven source rows by thirteen destination columns, and
# the bank stores it that way: Mx_Dot_0..27 is the four fixed columns and
# Mx_AssignDot_0..62 the nine assignable ones, both as
# index = column + columns * row. Enou pins the reading - its live cells land
# exactly on the routing read off MiniFreak V - and across the bank it leaves
# only 0.7% of live cells in a column with no destination.
MATRIX_ROWS = 7
FIXED_COLUMNS = 4
ASSIGN_COLUMNS = 9
# The four fixed columns stand for Pitch, Osc1 Wave, Osc1 Timbre and Cutoff;
# on V.Analog, MiniFreak's "Timbre Osc 1" column drives the pulse Shape knob.
FIXED_DEST = [1, 10, 8, 2]                      # Pitch, Osc1 Wave, Osc1 Timbre, Cutoff
SLOT_SUFFIX = ["cyc", "env", "lfo1", "lfo2", "vel", "whl", "kbd"]


def read_matrix(p):
    """(destination, row) -> amount for every live cell of the grid."""
    osc1 = MF_OSC1[enum_index(p.get("Osc1_Type", "0"), len(MF_OSC1))]
    fixed_dest = [1, 10, 3 if osc1 == "OSC_VANALOG" else 8, 2]
    cells = {}
    for i in range(MATRIX_ROWS * FIXED_COLUMNS):
        key = "Mx_Dot_%d" % i if i < MATRIX_ROWS * FIXED_COLUMNS - 1 else "Mx_Dot_Last"
        value = p.get(key)
        if value is None or abs(float(value) - 0.5) <= 1e-4:
            continue
        cells[(fixed_dest[i % FIXED_COLUMNS], i // FIXED_COLUMNS)] = (float(value) - 0.5) * 2.0

    columns = {}
    for i in range(ASSIGN_COLUMNS - 1):
        address = p.get("Mx_ColId_%d" % i)
        if address is not None:
            columns[i] = int(round(float(address) * 65535))
    address = p.get("Mx_ColId_Last")
    if address is not None:
        columns[ASSIGN_COLUMNS - 1] = int(round(float(address) * 65535))

    secondary = []
    for i in range(MATRIX_ROWS * ASSIGN_COLUMNS):
        key = "Mx_AssignDot_%d" % i if i < MATRIX_ROWS * ASSIGN_COLUMNS - 1 else "Mx_AssignDot_Last"
        value = p.get(key)
        if value is None or abs(float(value) - 0.5) <= 1e-4:
            continue
        amount = (float(value) - 0.5) * 2.0
        col_addr = columns.get(i % ASSIGN_COLUMNS, 0x8000)
        destination = MF_DEST_ORDINAL.get(col_addr)
        if destination is not None:
            cells[(destination, i // ASSIGN_COLUMNS)] = amount
        elif 0x9060 <= col_addr <= 0x90B9 and (i // ASSIGN_COLUMNS) == 4:
            secondary.append((col_addr - 0x9060, amount))

    # Fold Velo + AT -> Mod r:c depth routes (0x9060..0x90B9) into the target
    # matrix cell at normal playing velocity so velocity-scaled filter and
    # timbre sweeps (such as FM Bass's Velo -> Env->Cutoff) are preserved.
    for offset, amount in secondary:
        if offset < MATRIX_ROWS * FIXED_COLUMNS:
            target_dest = fixed_dest[offset % FIXED_COLUMNS]
            target_row = offset // FIXED_COLUMNS
        else:
            assign_offset = offset - MATRIX_ROWS * FIXED_COLUMNS
            target_dest = MF_DEST_ORDINAL.get(columns.get(assign_offset % ASSIGN_COLUMNS, 0x8000))
            target_row = assign_offset // ASSIGN_COLUMNS
        if target_dest is not None:
            cur = cells.get((target_dest, target_row), 0.0)
            cells[(target_dest, target_row)] = clamp(cur + amount * (100.0 / 127.0), -1.0, 1.0)
    return cells
# How the addresses were recovered, so the next batch can be checked the same
# way. Each oscillator owns a six-slot run - Wave +0, Timbre +1, Shape +2,
# Volume +6 - and the two runs sit ten apart: 0x9004..0x900A and 0x900E..0x9014.
# Osc1 Volume and Osc2 Volume are pinned by the Enou screenshot's macro slots
# and Osc1 Wave by the same screenshot, which fixes both runs. Everything from
# 0x9019 to 0x9020 then follows the destination menu's own order (Cutoff, Reso,
# Env Amt, VCA, Attack, Decay, Sustain, Release), and the usage counts in the
# bank agree: Cutoff is the single most-modulated destination (374 routes),
# Attack through Release are each used 28-60 times, and Glide only 10, which is
# what a portamento control should look like. 0x9015 is the one address in the
# middle pinned by position alone.
#
# Still unknown, in order of how much of the bank they account for:
#   0xA002/0xA00B/0xA014  FX Time 1/2/3      and the neighbouring Intensity and
#   0xA003/0xA00C/0xA015  Amount slots         (nine-slot stride per FX)
#   0xA004/0xA00D/0xA016
#   0x9030, 0x9028, 0x90C0, 0x90C4, 0x90C5
# The FX block alone is ~10% of everything the bank modulates.
# The full menu, read off the same screenshot: None, Osc1 Type, Osc1 Wave,
# Osc1 Timbre, Osc1 Shape, Osc1 Volume, Osc2 Type, Osc2 Wave, Osc2 Timbre,
# Osc2 Shape, Osc2 Volume, Glide, Cutoff, Reso, Env Amt, VCA, Attack, Decay,
# Sustain, Release, Rise, Fall, Hold, LFO1 Wave, LFO1 Rate, LFO1 Rate (Synced),
# LFO2 Wave, LFO2 Rate, LFO2 Rate (Synced), Macro 1, Macro 2, Uni Spread,
# Mod 1:1 .. Mod 7:13, Vib AM, Pitch 1, Pitch 2, LFO1 AM, LFO2 AM, CycEnv AM,
# Vibrato Rate, Filter FM, Pan, MxCtrl Val x6, then Time/Intensity/Amount 1-3.
# It is a CATEGORISED menu, not an address-ordered one (LFO1 AM sits near the
# end of the menu but is address 0x90C7), so the rest of the table cannot be
# read off it; only the anchors above are safe. What the addresses do imply:
#   * each oscillator owns a six-slot stride - Wave +0, Volume +6 - so
#     0x9005/0x9006 and 0x900E/0x900F/0x9010 are Osc1 Timbre/Shape and
#     Osc2 Wave/Timbre/Shape. Those five are also the five most-used
#     destinations in the whole bank (151, 123, 121, 117 and 95 routes), which
#     is what a Timbre/Shape pair should look like.
#   * the FX slots repeat on a nine-wide stride: 0xA002/0xA00D/0xA016 are the
#     same parameter in FX 1, 2 and 3.
# Sending a preset whose nine matrix columns are set to known destinations
# would turn all of that into confirmed data in one round.

# --- Oscillator 1: MiniFreak index -> Bizar OSC_* mode -----------------
# 0..13 are the 14 modes shared with Osc 2; 14 is Audio In, which Osc 1 alone
# has. This bank predates Osc 2's later additions, so it has 15 slots.
MF_OSC1 = [
    "OSC_VA",           # 0  CONFIRMED (42 presets; "basic waves")
    "OSC_SUPERWAVE",    # 1  CONFIRMED ("the fattest supersaw")
    "OSC_HARMO",        # 2  CONFIRMED (organs, additive pads)
    "OSC_KARPLUS",      # 3  CONFIRMED ("based on the karplus engine")
    "OSC_VANALOG",      # 4  CONFIRMED by MiniFreak V's Daft Lead engine display
    "OSC_WAVESHAPER",   # 5  CONFIRMED (manual section order)
    "OSC_FM",           # 6  CONFIRMED ("FM Bass", 11 FM descriptions)
    "OSC_FORMANT",      # 7  CONFIRMED ("speech and formant oscillator types")
    "OSC_SPEECH",       # 8  CONFIRMED (manual order: Chords is Osc 2 only, so
                        #    Speech follows Formant on Osc 1)
    "OSC_MODAL",        # 9  CONFIRMED (80s Mean, Conquest, Evil, House Mode,
                        #    Hyperpercs and Lo-bell all sit here)
    "OSC_NOISE",        # 10 CONFIRMED ("the noise engine")
    "OSC_BASS",         # 11 CONFIRMED (manual order; Noise Engineering trio)
    "OSC_SAWX",         # 12 CONFIRMED (manual order; Noise Engineering trio)
    "OSC_HARM",         # 13 CONFIRMED ("Harming Bass")
    "OSC_AUDIOIN",      # 14 the export's last Osc 1 type (24-type hardware
                        #    menu; the V bank stores a 15-entry subset)
]

# --- Oscillator 2: MiniFreak index -> Bizar ENGINE_* mode --------------
# 21 types: the 14 generators shared with Osc 1, plus Chords (Osc 2 only) and
# six processors at 15..20. FM/RM is the processor at 15; the filter processors
# follow it in the manual's section order.
MF_OSC2 = [
    "ENGINE_VA",            # 0  CONFIRMED (66 presets, the default)
    "ENGINE_SUPERWAVE",     # 1  CONFIRMED ("macro 2 controls the level of the superwave engine")
    "ENGINE_HARMO",         # 2  CONFIRMED (manual order)
    "ENGINE_KARPLUS",       # 3  CONFIRMED (manual order)
    "ENGINE_VANALOG",       # 4  CONFIRMED ("bass-line based on the VA engine")
    "ENGINE_WAVESHAPER",    # 5  CONFIRMED (manual order)
    "ENGINE_TWO_OP_FM",     # 6  Two Op. FM generator (Osc 1's engine, measured
                            #    on MiniFreak V to match it on Osc 2)
    "ENGINE_FORMANT",       # 7  Formant (Osc 2 has its own formant generator)
    "ENGINE_CHORD",         # 8  CONFIRMED ("using chord engine", "chord osc types")
    "ENGINE_SPEECH",        # 9  CONFIRMED (three "speech" descriptions cluster here)
    "ENGINE_MODAL",         # 10 CONFIRMED (manual order)
    "ENGINE_NOISE",         # 11 CONFIRMED (six "noise" descriptions cluster here)
    "ENGINE_BASS",          # 12 CONFIRMED (manual order)
    "ENGINE_SAWX",          # 13 CONFIRMED (manual order)
    "ENGINE_HARM",          # 14 CONFIRMED ("Enou" is Karplus + this)
    "ENGINE_FM_RM",         # 15 FM/RM processor CONFIRMED (ten FM descriptions)
    "ENGINE_MULTIFILTER",   # 16 processor CONFIRMED ("wah-wah keyboard")
    "ENGINE_SURGEON",       # 17 processor CONFIRMED ("The Surgeon")
    "ENGINE_COMB",          # 18 processor CONFIRMED ("using the Comb Filter of Oscillator 2")
    "ENGINE_PHASERFILTER",  # 19 processor
    "ENGINE_DESTROY",       # 20 processor
]

# --- FX rack: MiniFreak index -> Bizar FX_* ---------------------------
# The FX type menu, in MiniFreak V's own order (Reverb before Delay). Exports
# from the 4.x firmware list ten types (index / 9); newer ones add Super
# Unison and the two vocoders (index / 12). The subtype counts per slot confirm
# the order: index 3 carries six presets (Reverb), index 4 twelve (Delay).
MF_FX = [
    "FX_CHORUS",        # 0
    "FX_PHASER",        # 1
    "FX_FLANGER",       # 2
    "FX_REVERB",        # 3
    "FX_DELAY",         # 4
    "FX_DRIVE",         # 5  Distortion
    "FX_CRUSH",         # 6  Bit Crusher
    "FX_EQ",            # 7  3 Bands EQ
    "FX_PEAK_EQ",       # 8  Peak EQ
    "FX_MULTICOMP",     # 9  Multi Comp
    "FX_SUPER_UNISON",  # 10
    None,               # 11 Vocoder (Self) - no Bizar equivalent
    None,               # 12 Vocoder (Ext)
]

# Number of presets (FXn_Opt1 positions) per MiniFreak effect. The menus
# match Bizar's order one for one, except Delay: the MiniFreak
# interleaves each character with its Sync version (Digital, Digital Sync,
# Stereo, ...) where Bizar lists the seven free-running characters
# first and their Sync versions after them.
MF_FX_PRESETS = {
    "FX_CHORUS": 5, "FX_PHASER": 6, "FX_FLANGER": 4, "FX_REVERB": 6,
    "FX_DELAY": 12, "FX_DRIVE": 6, "FX_MULTICOMP": 5, "FX_SUPER_UNISON": 8,
}

# MiniFreak Delay Time (knob position -> ms), measured from MiniFreak V.
MF_DELAY_MS = [
    (0.0, 2.0), (0.05, 10.45), (0.1, 20.27), (0.15, 31.7), (0.2, 45.0),
    (0.3, 78.3), (0.4, 123.3), (0.5, 184.1), (0.6, 266.0), (0.7, 376.9),
    (0.8, 526.1), (0.9, 728.1), (0.95, 854.0), (1.0, 1000.0),
]
# Wet level (relative to full wet) against Amount for the Send routing.
MF_DELAY_SEND_WET = [(0.0, 0.0), (0.25, 0.0776), (0.5, 0.221), (0.75, 0.49), (1.0, 1.0)]
MF_REVERB_SEND_WET = [(0.0, 0.0), (0.25, 0.202), (0.5, 0.285), (0.75, 0.519), (1.0, 1.0)]
# Peak EQ frequency (Hz) against Param1.
MF_PEAK_HZ = [(0.0, 32.0), (0.125, 54.0), (0.375, 205.0), (0.625, 1098.0),
              (0.875, 6255.0), (1.0, 15019.0)]

# Bizar enum ordinal for each engine, matching src/engines.h.
OSC1_ORDINAL = {
    "OSC_VA": 0, "OSC_FM": 1, "OSC_FOLD": 2, "OSC_NOISE": 3, "OSC_WAVETABLE": 4,
    "OSC_SUPERWAVE": 5, "OSC_HARMO": 6, "OSC_FORMANT": 7,
    "OSC_VANALOG": 8, "OSC_WAVESHAPER": 9, "OSC_CHORD": 10, "OSC_KARPLUS": 11,
    "OSC_SAWX": 12, "OSC_HARM": 13, "OSC_BASS": 14, "OSC_MODAL": 15,
    "OSC_SPEECH": 16, "OSC_SAMPLE": 17, "OSC_PARTICLES": 18, "OSC_CLOUD": 19,
    "OSC_HITGRAINS": 20, "OSC_FROZEN": 21, "OSC_SKAN": 22, "OSC_LICK": 23,
    "OSC_RASTER": 24, "OSC_AUDIOIN": 25,
}
OSC2_ORDINAL = {
    "ENGINE_VA": 0, "ENGINE_RING": 1, "ENGINE_FOLD": 2, "ENGINE_NOISE": 3,
    "ENGINE_WAVETABLE": 4, "ENGINE_SUPERWAVE": 5, "ENGINE_HARMO": 6,
    "ENGINE_CHORD": 7, "ENGINE_MULTIFILTER": 8, "ENGINE_CRUSHER": 9,
    "ENGINE_COMB": 10, "ENGINE_SURGEON": 11, "ENGINE_PHASERFILTER": 12,
    "ENGINE_DESTROY": 13, "ENGINE_VANALOG": 14, "ENGINE_WAVESHAPER": 15,
    "ENGINE_KARPLUS": 16, "ENGINE_SAWX": 17, "ENGINE_HARM": 18,
    "ENGINE_BASS": 19, "ENGINE_MODAL": 20, "ENGINE_SPEECH": 21,
    "ENGINE_SAMPLE": 22, "ENGINE_PARTICLES": 23, "ENGINE_CLOUD": 24,
    "ENGINE_HITGRAINS": 25, "ENGINE_FROZEN": 26, "ENGINE_SKAN": 27,
    "ENGINE_LICK": 28, "ENGINE_RASTER": 29, "ENGINE_AUDIOIN": 30,
    "ENGINE_FORMANT": 31, "ENGINE_TWO_OP_FM": 32, "ENGINE_FM_RM": 33,
}
FX_ORDINAL = {
    "FX_OFF": 0, "FX_DELAY": 1, "FX_REVERB": 2, "FX_CHORUS": 3, "FX_DRIVE": 4,
    "FX_CRUSH": 5, "FX_PHASER": 6, "FX_EQ": 7, "FX_FLANGER": 8,
    "FX_SUPER_UNISON": 9, "FX_PEAK_EQ": 10, "FX_MULTICOMP": 11,
}

# MiniFreak LFO wave menu order (9 entries): Sine, Triangle, Saw, Square,
# Sample & Hold, Slew S&H, ExpSaw, ExpRamp, Shaper. Bizar's shapes
# match this one for one: Triangle, Saw, Square and a custom 8-point curve for
# Shaper, plus Sine, S&H, Slew S&H, ExpSaw and ExpRamp (see modulation.h).
LFO_SHAPE_ORDINAL = [
    4,  # Sine     -> LFO_SINE
    0,  # Triangle -> LFO_TRIANGLE
    1,  # Saw      -> LFO_SAW
    2,  # Square   -> LFO_SQUARE
    5,  # S&H      -> LFO_SNH
    6,  # Slew S&H -> LFO_SLEW
    7,  # ExpSaw   -> LFO_EXPSAW
    8,  # ExpRamp  -> LFO_EXPRAMP
    3,  # Shaper   -> LFO_CUSTOM
]
# Bizar's tempo division list, which the RateSync rank is mapped onto.
# MiniFreak V's 27 tempo divisions (8 dotted bars .. 1/32 triplet), in the
# same order as Bizar's list, read as floor(value * 27).
LFO_DIV_COUNT = 27
# LFO Retrig menu: Free, Poly Kbd, Mono Kbd, Legato Kbd, One, LFO, CycEnv,
# Seq Start, read as floor(value * 8). Bizar has the first five; the
# last three restart on notes in practice, so they import as Mono Kbd.
LFO_RETRIG_MAP = [0, 1, 2, 3, 4, 2, 2, 2]
# Free-running LFO rate (Hz) per knob position, measured from MiniFreak V.
LFO_RATE_HZ = [(0.0, 0.06), (0.025, 0.1), (0.05, 0.147), (0.1, 0.264),
               (0.15, 0.422), (0.2, 0.636), (0.3, 1.314), (0.4, 2.548),
               (0.5, 4.796), (0.6, 8.889), (0.7, 16.39), (0.8, 29.85),
               (0.9, 55.56), (1.0, 100.0)]
# Envelope stage times per knob position, measured from MiniFreak V. Attack
# is the time to reach the peak; decay and release are the time to fall
# 60 dB (6.91 time constants), which is how Bizar reads them.
ENV_ATTACK_MS = [(0.0, 0.5), (0.05, 4.0), (0.1, 9.0), (0.2, 30.0), (0.3, 80.0),
                 (0.4, 204.0), (0.5, 505.0), (0.6, 1150.0), (0.7, 2360.0),
                 (0.8, 4950.0), (0.9, 10050.0), (1.0, 19800.0)]
_ENV_X = [0.0, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]
ENV_DECAY_MS = [(x, 6.9078 * t) for x, t in zip(_ENV_X, (
    0.3, 0.92, 2.45, 21.0, 53.0, 105.0, 194.0, 392.0, 864.0, 1769.0, 3486.0, 6657.0))]
ENV_RELEASE_MS = [(x, 6.9078 * t) for x, t in zip(_ENV_X, (
    3.2, 5.2, 14.5, 41.3, 79.2, 141.5, 250.0, 516.0, 1034.0, 1987.0, 3700.0, 6740.0))]
CYCLE_MODES = ["Env", "Run", "Loop"]


# Glide, measured on MiniFreak V as the length of a straight-line (in
# semitones) slide, which in Time mode is the same for any interval. About
# 11.2 s x knob^3 from 0.2 up.
GLIDE_MS = [(0.05, 10.0), (0.1, 17.0), (0.15, 36.0), (0.2, 96.0), (0.3, 309.0),
            (0.4, 726.0), (0.5, 1411.0), (0.6, 2236.0), (0.7, 3857.0),
            (0.8, 5301.0), (0.9, 8190.0), (1.0, 11234.0)]

LFO_CUSTOM_SHAPE = 3  # Bizar's LFO_CUSTOM, the Shaper

# The Shaper, measured from MiniFreak V: up to 16 steps (Shp<n>_Step_*_0..14,
# then _Last), Length = (steps - 1) / 15, and every step lasts one whole LFO
# period. Each step draws its own segment from 0 to its level (Amp, 0.5 is 0):
# Slope is a menu read as floor(Slope * 4) - rise, fall, rise-then-fall, fall -
# and Curve bends it (straight from 0.35 to 0.65). En is a three-way menu,
# read as round(En * 2): rest, play, or tie (stretch the previous step's
# segment over this one). The module plays this as its MiniFreak Shaper mode.
def import_shaper(p, lfo, s):
    if ("Shp%s_Length" % lfo) not in p:
        return
    steps = int(round(clamp(float(p["Shp%s_Length" % lfo]), 0.0, 1.0) * 15.0)) + 1
    s["lfo%s_shp_mode" % lfo] = 1
    s["lfo%s_shp_steps" % lfo] = steps
    for i in range(16):
        suffix = "Last" if i == 15 else str(i)
        def get(name, default):
            return float(p.get("Shp%s_Step_%s_%s" % (lfo, name, suffix), default))
        n = i + 1
        s["lfo%s_shp_amp%d" % (lfo, n)] = round(clamp(get("Amp", 0.5), 0.0, 1.0) * 2.0 - 1.0, 4)
        s["lfo%s_shp_curve%d" % (lfo, n)] = round(clamp(get("Curve", 0.5), 0.0, 1.0), 4)
        s["lfo%s_shp_slope%d" % (lfo, n)] = round(clamp(get("Slope", 0.0), 0.0, 1.0), 4)
        s["lfo%s_shp_on%d" % (lfo, n)] = int(round(clamp(get("En", 0.0), 0.0, 1.0) * 2.0))


# The built-in sequencer / arpeggiator. Enum assumptions (best fit, stored as
# index / (count - 1) like every other enum): Seq_Mode is off / arp / seq (3
# entries); Arp_Mode has 8 entries (index = round(v * 7)); Arp_Oct is 0..3
# (round(v * 3)); Seq_TimeDiv is a 15-entry tempo division list with the
# straight divisions at indices 1, 4, 7, 10, 13 = 1/2, 1/4, 1/8, 1/16, 1/32,
# the dotted value one below each and the triplet one above (10 = 1/16 is the
# common default). Seq_Length is (steps - 1) / 63, except that the untouched
# default 0.25 means 16 steps. A step is live when StepState_S<i> > 0.1 and holds
# up to six note layers (Pitch_S<i>_I<j>, 1.0 = unused layer) with Velo/Length.
# Step automation is imported by import_seq_automation; ratchet, repeat, mutate
# and spice are NOT imported.
def import_sequencer(p, s):
    def num(name, default):
        try:
            return float(p.get(name, default))
        except ValueError:
            return float(default)
    import_seq_automation(p, s)
    mode = int(round(num("Seq_Mode", 0.0) * 3.0))
    mode = 0 if num("Seq_Mode", 0.0) >= 0.999 else int(clamp(mode, 0, 2))
    if mode == 0:
        return
    s["seq_mode"] = mode
    s["seq_arp_mode"] = int(clamp(round(num("Arp_Mode", 0.0) * 7.0), 0, 7))
    s["seq_arp_oct"] = int(clamp(round(num("Arp_Oct", 0.0) * 3.0), 0, 3))
    s["seq_div"] = int(clamp(round(num("Seq_TimeDiv", 0.733) * 14.0), 0, 14))
    # Measured on MiniFreak V: a step's note lasts 0.08 + 0.92 x Seq_Gate of
    # the step, so Gate 0 still plays a 10 ms note at 1/16 and 120 BPM.
    s["seq_gate"] = round(0.08 + 0.92 * clamp(num("Seq_Gate", 0.5), 0.0, 1.0), 4)
    s["seq_swing"] = round(clamp(num("Seq_Swing", 0.0), 0.0, 1.0), 4)
    length_value = num("Seq_Length", 0.25)
    if abs(length_value - 0.25) < 0.001:
        length = 16
    else:
        length = int(clamp(round(length_value * 63.0) + 1, 1, 64))
    steps = []
    last = -1
    for i in range(64):
        notes = []
        if num("StepState_S%d" % i, 0.0) > 0.1:
            for j in range(6):
                pitch = num("Pitch_S%d_I%d" % (i, j), 1.0)
                if pitch >= 0.999:
                    continue
                notes.append("%d.%d.%d" % (
                    int(clamp(round(pitch * 128.0), 0, 127)),
                    int(clamp(round(num("Velo_S%d_I%d" % (i, j), 0.85)* 127.0), 1, 127)),
                    int(clamp(round(num("Length_S%d_I%d" % (i, j), 0.0) * 127.0), 0, 127))))
        steps.append("+".join(notes))
        if notes:
            last = i
    count = min(64, max(last + 1, length))
    s["seq_length"] = length
    s["seq_data"] = ",".join(steps[:count])


# Step automation (the MiniFreak's modulation sequence): four lanes, read from
# the bank as
#   Seq_Autom_Dest_0..2/_Last    destination address (the matrix's 16-bit
#                                table above; 0x8000 = none)
#   Seq_Autom_Smooth_*           smooth (1) or stepped; every factory lane is 1
#   Seq_Autom_Set_*              the knob value when the lane was recorded
#                                (matches the preset's knob on most lanes)
#   Seq_Autom_Val_*              usually 0.5; a stale live value, not used
#   Mod_S<step>_I<lane>          the step's offset, 0.5 = no change
#   ModState_S<step>             round(value * 65536); its low four bits say
#                                which lanes hold a value on that step
# The step values are offsets rather than absolute positions: across the bank
# they average 0.5 whatever the knob sits at, and a recording starts at 0.5.
# They are read as bipolar knob units, (v - 0.5) * 2, the same scale as a
# matrix amount; that scale is a best fit, not measured on MiniFreak V. A lane
# whose destination Bizar does not have (addresses 0x9030, 0x9037,
# 0x903D and 0x903E in the factory bank) is dropped, like a matrix route would
# be. Lanes are imported even when the sequencer is off, so turning it on
# brings them back.
SEQ_LANES = 4


def import_seq_automation(p, s):
    def key(base, lane):
        return "%s_%s" % (base, "Last" if lane == SEQ_LANES - 1 else lane)
    masks = []
    for step in range(64):
        try:
            masks.append(int(round(float(p.get("ModState_S%d" % step, 0.0)) * 65536.0)) & 0xF)
        except ValueError:
            masks.append(0)
    for lane in range(SEQ_LANES):
        try:
            address = int(round(float(p.get(key("Seq_Autom_Dest", lane), 0.5)) * 65535.0))
        except ValueError:
            continue
        dest = MF_DEST_ORDINAL.get(address)
        if dest is None:
            continue
        values = []
        for step in range(64):
            value = 0.0
            if masks[step] >> lane & 1:
                try:
                    value = (float(p.get("Mod_S%d_I%d" % (step, lane), 0.5)) - 0.5) * 2.0
                except ValueError:
                    value = 0.0
            value = round(clamp(value, -1.0, 1.0), 3)
            values.append("" if value == 0.0 else ("%g" % value))
        while values and values[-1] == "":
            values.pop()
        if not values:
            continue
        n = lane + 1
        s["seq_lane%d_dest" % n] = dest
        s["seq_lane%d_smooth" % n] = 1 if float(p.get(key("Seq_Autom_Smooth", lane), 1.0)) >= 0.5 else 0
        s["seq_lane%d_data" % n] = ",".join(values)


CHORD_MAX_NOTES = 5


def unison_step_cents(spread):
    """MiniFreak V's unison step for a Spread position (measured, see convert)."""
    x = clamp(float(spread), 0.0, 1.0)
    return 200.0 * x if x <= 0.5 else 100.0 + 2200.0 * (x - 0.5)


def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def enum_floor(value, count):
    """Normalised enum -> index the way MiniFreak V reads its longer menus
    (LFO divisions, Retrig, Cycling Envelope mode): floor(value * count)."""
    return min(int(clamp(float(value), 0.0, 1.0) * count + 1e-6), count - 1)


def enum_index(value, count):
    """Normalised enum -> index, using the export's own index/(count-1) scale."""
    return int(round(clamp(float(value), 0.0, 1.0) * (count - 1)))


def table_map(table, x, geometric=False):
    """Piecewise-linear (or geometric) lookup in a [(x, y), ...] table."""
    x = clamp(float(x), table[0][0], table[-1][0])
    for (x0, y0), (x1, y1) in zip(table, table[1:]):
        if x <= x1:
            f = (x - x0) / (x1 - x0) if x1 > x0 else 0.0
            if geometric:
                return y0 * (y1 / y0) ** f
            return y0 + f * (y1 - y0)
    return table[-1][1]


def fx_type_index(value):
    """FXn_Type -> MF_FX index. Exports store index / (menu length - 1), and the
    menu grew across firmware versions (10, 11, 12 and 13 types, the same order
    with entries appended). The ten-type scale is tried first because values
    such as 1/3 land on whole indices in several scales; the longer menus are
    only used for values that fall off its grid."""
    v = clamp(float(value), 0.0, 1.0)
    best = None
    for denominator in (9, 12, 11, 10):
        k = v * denominator
        off = abs(k - round(k))
        if off < 0.02:
            return int(round(k))
        if best is None or off < best[0]:
            best = (off, int(round(k)))
    return best[1]


def fx_send_to_mix(wet):
    """Bizar's Delay/Reverb mix law keeps the dry at full and raises
    the wet as (2 * mix)^2 up to mix 0.5, so a wanted wet level maps to
    mix = sqrt(wet) / 2."""
    return 0.5 * clamp(wet, 0.0, 1.0) ** 0.5


# Tempo-synced Run/Loop stage time at 120 BPM, measured on MiniFreak V: a
# 1/32 note at 0, rising in a straight line by four bars (8 s) to 1. The
# free-running 10 s * x^4 taper reads Fall 0.056 as 1 ms where the synced
# stage lasts half a second.
# A sixteenth note at 120 BPM: Bizar's Cycling Envelope has no tempo
# sync, so synced stages import at that tempo.
CYCLE_SIXTEENTH_MS = 125.0


def cycle_time_map(value):
    """0..1 -> ms for the Cycling Envelope's Rise, Fall and Hold, measured as
    10 s * x^4 on MiniFreak V."""
    return 10000.0 * clamp(float(value), 0.0, 1.0) ** 4


def read_presets(bank):
    out = []
    with zipfile.ZipFile(bank) as z:
        for entry in sorted(z.namelist()):
            if entry.endswith("/"):
                continue
            name = entry.rsplit("/", 1)[-1]
            text = z.read(entry).decode("utf-8", "replace")
            params = dict((m.group(2), m.group(3)) for m in PAIR.finditer(text))
            if "Osc1_Type" not in params:
                continue
            out.append((name, params))
    return out


# Osc 1 Wavetable, measured against MiniFreak V: its Wave knob sets the
# waveform's width and its Timbre knob the table position, each through the
# curve below (knob position -> engine value, evenly spaced knots). Bizar's
# own knobs read Position / Width / Tone as labelled, so the values are
# converted here, and modulation into Wave and Timbre swaps over with them.
MF_WT_WIDTH = [0.375, 1.0, 1.0]
MF_WT_POSITION = [0.0, 0.0, 0.0, 0.125, 1.0]
MF_WT_TONE = [0.125, 0.625, 0.75, 0.875, 1.0]


def knot_curve(knots, x):
    x = clamp(float(x), 0.0, 1.0) * (len(knots) - 1)
    i = min(int(x), len(knots) - 2)
    return knots[i] + (knots[i + 1] - knots[i]) * (x - i)


def convert_osc1_wavetable(s):
    """MiniFreak Wave/Timbre/Shape -> Bizar Position/Width/Tone."""
    wave, timbre, shape = (s.get("osc1_wave", 0.5), s.get("osc1_timbre", 0.35),
                           s.get("osc1_shape", 0.5))
    width = knot_curve(MF_WT_WIDTH, wave)
    # Bizar's Width compresses the first half-cycle (0.5 - 0.5 x Width^2 is
    # untouched at 0); a MiniFreak width above the middle compresses the
    # second half instead, which has the same harmonic weight.
    folded = width if width <= 0.5 else 1.0 - width
    s["osc1_wave"] = round(knot_curve(MF_WT_POSITION, timbre), 4)
    s["osc1_timbre"] = round(math.sqrt(max(0.0, 0.5 - folded) / 0.5), 4)
    s["osc1_shape"] = round(knot_curve(MF_WT_TONE, shape), 4)
    swap = {10: 8, 8: 10}   # Wave 1 <-> Timbre 1
    for key in list(s):
        if (key.endswith("_dest") or "_dest" in key) and s[key] in swap:
            s[key] = swap[s[key]]


def convert(name, p):
    """One MiniFreak preset -> a Bizar state JSON string."""
    # State version 9: the cutoff is a normalised 0..1 log control (version 8
    # and earlier stored it as 0..100). Version 8 moved the modulation to the
    # grid - twelve destination columns by seven source rows, plus four macros
    # of four destination sockets - which is how the hardware stores the
    # matrix, so presets import without being translated into routes.
    s = {"version": 19}

    s["osc1_mode"] = OSC1_ORDINAL[MF_OSC1[enum_index(p["Osc1_Type"], len(MF_OSC1))]]
    s["osc2_mode"] = OSC2_ORDINAL[MF_OSC2[enum_index(p["Osc2_Type"], len(MF_OSC2))]]

    for slot in ("osc1", "osc2"):
        mf = "Osc1" if slot == "osc1" else "Osc2"
        for src, dst in (("Param1", "wave"), ("Param2", "timbre"), ("Param3", "shape")):
            key = "%s_%s" % (mf, src)
            if key in p:
                s["%s_%s" % (slot, dst)] = round(clamp(float(p[key]), 0.0, 1.0), 4)
        if (mf + "_Volume") in p:
            s[slot + "_level"] = round(clamp(float(p[mf + "_Volume"]), 0.0, 1.0), 4)
        if (mf + "_CoarseTune") in p:
            # Coarse Tune spans +/-48 semitones; FineTune adds +/-50 cents
            # around its normalized midpoint.
            semis = (float(p[mf + "_CoarseTune"]) - 0.5) * 2.0 * 48.0
            if (mf + "_FineTune") in p:
                semis += clamp(float(p[mf + "_FineTune"]), 0.0, 1.0) - 0.5
            s[slot + "_tune"] = round(clamp(semis, -48.5, 48.5), 4)
        # TuneModQuantize is the eleven-way menu index / 10, in menu order.
        if (mf + "_TuneModQuantize") in p:
            s[slot + "_quantize"] = int(round(
                clamp(float(p[mf + "_TuneModQuantize"]), 0.0, 1.0) * 10.0))

    if "Vcf_Cutoff" in p:
        # MiniFreak's cutoff is already a normalised 0..1 log control, which is
        # exactly what Bizar's normalised 0..1 log cutoff is, so this is
        # a straight copy rather than a curve fit or a scale change.
        s["cutoff"] = round(clamp(float(p["Vcf_Cutoff"]), 0.0, 1.0), 4)
    if "Vcf_Resonance" in p:
        # MiniFreak V's filter self-oscillates from about 0.985 up (measured);
        # Bizar's does above 0.967, so those land in that zone.
        r = clamp(float(p["Vcf_Resonance"]), 0.0, 1.0)
        s["resonance"] = round(r * 0.95 if r < 0.985 else 0.97 + (r - 0.985) * 2.0, 4)
    if "Vcf_Type" in p:
        # Vcf_Type sits on a 1/6 grid: 0 LP, 1/6 BP, 2/6 HP (measured on
        # MiniFreak V; anything above plays as HP). Reading it as a three-way
        # menu turned the bank's 8 BP presets into LP and its 21 HP into BP.
        s["filter_mode"] = min(2, int(round(clamp(float(p["Vcf_Type"]), 0.0, 1.0) * 6.0)))
    if "Vcf_EnvAmount" in p:
        s["filter_env"] = round(clamp((float(p["Vcf_EnvAmount"]) - 0.5) * 2.0, -1.0, 1.0), 4)

    # Attack, Decay and Release each have their own measured knob curve, so
    # the stored length is the length the hardware plays.
    for src, dst, table in (("Env_Attack", "attack", ENV_ATTACK_MS),
                            ("Env_Decay", "decay", ENV_DECAY_MS),
                            ("Env_Release", "release", ENV_RELEASE_MS)):
        if src in p:
            s[dst] = round(clamp(table_map(table, p[src], geometric=True), 1.0, 50000.0), 1)
    if "Env_Sustain" in p:
        s["sustain"] = round(clamp(float(p["Env_Sustain"]), 0.0, 1.0), 4)

    # The rest of the Envelope page: stage shapes, the retrigger switch and the
    # four velocity depths. All three curve menus are two options - Default and
    # Quick for the attack, Default and Percussive for decay and release - and
    # "Default" is the straight ramp the envelope has always had. Enou pins the
    # attack: its Attack Curve reads "Quick" with Env_AttackCurve = 1, and its
    # Retrig reads "Env Continue" with Gen_RetrigMode = 1, matching our order.
    # The release field is the odd one: 0.25 is what the hardware calls
    # "Default" (230 of 257 presets) while 1 is Percussive, so anything past
    # the half-way mark is the second option.
    for src, dst in (("Env_AttackCurve", "attack_curve"),
                     ("Env_DecayCurve", "decay_curve"),
                     ("Env_ReleaseCurve", "release_curve")):
        if src in p:
            s[dst] = 1 if float(p[src]) > 0.5 else 0
    if "Gen_RetrigMode" in p:
        s["env_retrig"] = 1 if float(p["Gen_RetrigMode"]) > 0.5 else 0
    for src, dst in (("VeloMod_VCA", "vel_vca"), ("VeloMod_EnvAmount", "vel_vcf"),
                     ("VeloMod_Env", "vel_env"), ("VeloMod_Time", "vel_time")):
        if src in p:
            s[dst] = round(clamp(float(p[src]), 0.0, 1.0), 4)
    # Mx_VeloAt picks what the matrix's Velo row reads, measured on MiniFreak V
    # through a Velo -> VCA route: 0 velocity, 0.5 aftertouch only (a played
    # note leaves the row at 0), 1 both. Bizar's order is Velo + AT,
    # Velo, AT.
    if "Mx_VeloAt" in p:
        s["velo_source"] = {0: 1, 1: 2, 2: 0}[enum_index(p["Mx_VeloAt"], 3)]
    # Kbd_Src picks what the matrix's Keyboard row reads, measured on MiniFreak
    # V through Kbd -> Pitch and Kbd -> Decay routes: Linear, S Curve, Random,
    # Voices, in Bizar's order. Full Moon's Random row in a six-voice
    # Unison gives every voice its own decay, so the longest one sets the tail.
    if "Kbd_Src" in p:
        s["kbd_source"] = enum_index(p["Kbd_Src"], 4)

    # The Cycling Envelope's Hold knob is a sustain level in Env mode and a
    # timed hold in Run/Loop. Convert that shared control according to mode.
    cycle_mode = enum_floor(p.get("CycEnv_Mode", "0"), 3)
    # Tempo Sync changes only Run and Loop (Env mode times are the same either
    # way). Bizar's Cycling Envelope has no sync, so synced stages
    # import at 120 BPM.
    synced_cycle = cycle_mode != 0 and float(p.get("CycEnv_TempoSync", "0")) > 0.5
    for src, dst in (("CycEnv_Rise", "cycle_rise"), ("CycEnv_Fall", "cycle_fall"),
                     ("CycEnv_Hold", "cycle_hold")):
        if src not in p:
            continue
        value = clamp(float(p[src]), 0.0, 1.0)
        if dst == "cycle_hold" and cycle_mode == 0:
            s[dst] = round(value, 4)
        else:
            # Env mode is a second ADSR on the hardware: Rise reads the
            # attack curve and Fall the decay curve. Run and Loop use their
            # own 10 s * x^4 taper. Hold may legitimately be zero; Rise and
            # Fall bottom out at 1 ms.
            if cycle_mode == 0 and dst == "cycle_rise":
                ms = table_map(ENV_ATTACK_MS, value, geometric=True)
            elif cycle_mode == 0 and dst == "cycle_fall":
                ms = table_map(ENV_DECAY_MS, value, geometric=True)
            elif synced_cycle:
                # Measured on MiniFreak V: a synced stage lasts round(60 * knob)
                # sixteenth notes, and Fall one sixteenth more. Rise 0 is an
                # instant jump to the peak, so a loop with every knob at 0 is
                # one sixteenth of Fall (Fractal Arp's pluck), not two
                # sixteenths split between Rise and Fall.
                steps = round(60.0 * value) + (1 if dst == "cycle_fall" else 0)
                ms = CYCLE_SIXTEENTH_MS * steps
            else:
                ms = cycle_time_map(value)
            s[dst] = round(ms, 1) if dst == "cycle_hold" else round(max(ms, 1.0), 1)
    # Rise and Fall each carry a continuous stage shape in Run and Loop: 0.5
    # is linear, lower bends the stage so it moves slowly at first, higher so
    # it moves quickly at first. In Env mode the same knob is a two-way
    # switch: the factory default 32767/65535 is Default, and anything above
    # it is Quick (Rise) or Percussive (Fall). Both are stored 0..1
    # exactly as the module reads them.
    for src, dst in (("CycEnv_RiseCurve", "cycle_rise_curve"),
                     ("CycEnv_FallCurve", "cycle_fall_curve")):
        if src in p:
            # Five places keep 32767/65535 (0.49998) apart from 32768/65535.
            s[dst] = round(clamp(float(p[src]), 0.0, 1.0), 5)
    if "CycEnv_Mode" in p:
        # Env / Run / Loop, read as V reads it: 0.25 is Env, 0.5 Run (free),
        # 0.75 and 1 Loop (restarted by each note).
        s["cycle_mode"] = cycle_mode
    # The MiniFreak reaches the matrix at full level: its "CycEnv AM" is a
    # hidden amount you can only modulate, and it sits at 100 unless a preset
    # moves it. Bizar exposes that amount as Cycle Amount and adds a
    # separate direct-to-filter shortcut, so the shortcut is disabled only when
    # this preset actually has an imported CycEnv matrix route (set below, once
    # the matrix has been decoded).

    for lfo in ("1", "2"):
        synced = float(p.get("LFO%s_SyncEn" % lfo, "0")) > 0.5
        s["lfo%s_sync" % lfo] = 1 if synced else 0
        if synced:
            # Same 27 divisions in the same order as Bizar's list.
            s["lfo%s_div" % lfo] = enum_floor(p.get("LFO%s_RateSync" % lfo, "0.6154"),
                                              LFO_DIV_COUNT)
        else:
            # With sync off, the Rate field is the real free-running rate.
            if ("LFO%s_Rate" % lfo) in p:
                s["lfo%s_rate" % lfo] = round(table_map(LFO_RATE_HZ, p["LFO%s_Rate" % lfo],
                                                        geometric=True), 4)
        s["lfo%s_retrig" % lfo] = LFO_RETRIG_MAP[enum_floor(p.get("LFO%s_Retrig" % lfo, "0"), 8)]
        if ("LFO%s_Wave" % lfo) in p:
            s["lfo%s_shape" % lfo] = LFO_SHAPE_ORDINAL[enum_index(p["LFO%s_Wave" % lfo], 9)]
            if s["lfo%s_shape" % lfo] == LFO_CUSTOM_SHAPE:
                import_shaper(p, lfo, s)

    import_sequencer(p, s)

    if "Osc_Freerun" in p:
        s["osc_phase"] = 1 if float(p["Osc_Freerun"]) > 0.5 else 0

    if "Osc_Glide" in p:
        g = clamp(float(p["Osc_Glide"]), 0.0, 1.0)
        s["glide"] = 0 if g <= 0.0 else int(round(table_map(GLIDE_MS, g, geometric=True)))
    if "Osc_GlideMode" in p:
        # Time, Rate, Sync. Sync is a tempo-locked Time mode in the hardware;
        # Bizar currently models it as Time.
        s["glide_mode"] = 1 if enum_index(p["Osc_GlideMode"], 3) == 1 else 0
    # Voice mode, measured with two overlapping notes: 0 Mono, 1/3 Unison
    # (a mono stack), everything above plays polyphonically (Poly / Para).
    if "Gen_NoteMode" in p:
        mode = float(p["Gen_NoteMode"])
        if mode < 0.1:
            s["voice_mode"] = 1
        elif abs(mode - 1.0 / 3.0) < 0.05 or abs(mode - 0.25) < 0.05:
            s["voice_mode"] = 2
        else:
            s["voice_mode"] = 0
    # Unison, measured: Count 0..1 stacks 2..6 voices, and they sit on whole
    # steps either side of the note, never closer than about 2.75 cents (at
    # Spread 0 the voices still drift apart). The step climbs to a semitone at
    # Spread 0.5 and on to an octave at 1, both lines straight (6 voices:
    # 0.3 -> +/-60/118/178, 0.773 -> +/-702/1400/2100 cents, 1 -> +/-1200...;
    # read with a sine, since the third harmonic images of an upper Spread
    # look like extra intervals). Bizar's Spread is six of those steps.
    if "Gen_UnisonCount" in p:
        s["unison_voices"] = 2 + int(round(clamp(float(p["Gen_UnisonCount"]), 0.0, 1.0) * 4.0))
    if "Gen_UnisonSpread" in p:
        s["spread"] = round(6.0 * math.hypot(2.75, unison_step_cents(p["Gen_UnisonSpread"])), 2)
    # Unison Mode, measured with two overlapping keys: 0 is one stack that
    # follows the last key, 0.5 (Poly) and 1 (Para) give every key a stack of
    # Count voices. Para has twelve voices on the MiniFreak and plays as Poly
    # in Bizar's six.
    if "Gen_UnisonMode" in p and float(p["Gen_UnisonMode"]) > 0.25:
        s["unison_mode"] = 1
    # Keyboard Chord, measured: Offset n holds 0.5 + semitones / 254, and
    # Length index k (of 0..6) plays k + 1 of them over the key, in Poly, Para,
    # Mono and Unison alike. Bizar has the five that six voices hold.
    if float(p.get("Kbd_Chord_En", "0")) > 0.5:
        s["chord"] = 1
        s["chord_notes"] = min(CHORD_MAX_NOTES,
                               int(round(clamp(float(p.get("Kbd_Chord_Length", "0")), 0.0, 1.0) * 6.0)) + 1)
        for i in range(s["chord_notes"]):
            offset = float(p.get("Kbd_Chord_Offset_%d" % i, "0.5"))
            s["chord_note%d" % (i + 1)] = int(clamp(round((offset - 0.5) * 254.0), -48, 48))
    if "Osc_BendRange" in p:
        s["bend_range"] = int(round(clamp(float(p["Osc_BendRange"]), 0.0, 1.0) * 24.0))
    if "Preset_Volume" in p:
        s["output"] = round(clamp(float(p["Preset_Volume"]), 0.0, 1.0), 4)
    # The preset stores where the mod wheel was, and MiniFreak V restores it
    # on load, so every Wheel route in the grid starts from that position.
    if "Mod_Wheel" in p:
        s["mod_wheel"] = round(clamp(float(p["Mod_Wheel"]), 0.0, 1.0), 4)

    # Macros. On the hardware a macro owns four destination sockets, each with
    # its own depth, and the grid's macro pages are that same layout, so these
    # copy across directly. 0x8000 means the socket is empty.
    for macro in (1, 2):
        value_key = "Macro%d_Value" % macro
        if value_key in p:
            s["macro%d" % macro] = round(clamp(float(p[value_key]), 0.0, 1.0), 4)
        for slot, key in enumerate(("0", "1", "2", "Last"), start=1):
            address = p.get("Macro%d_Dest_%s" % (macro, key))
            if address is None:
                continue
            destination = int(round(float(address) * 65535))
            if destination == 0x8000 or destination not in MF_DEST_ORDINAL:
                continue
            s["macro%d_dest%d" % (macro, slot)] = MF_DEST_ORDINAL[destination]
            amount = p.get("Macro%d_Amount_%s" % (macro, key))
            if amount is not None:
                # Stored 0..1 around 0.5, exactly like the grid's amounts.
                s["macro%d_amt%d" % (macro, slot)] = round(
                    clamp((float(amount) - 0.5) * 2.0, -1.0, 1.0), 4)

    # The matrix, straight onto the grid: one column per destination the preset
    # modulates (twelve is the widest the bank ever needs), one amount per
    # source row that reaches it. What is left out is the handful of
    # destinations our grid has no counterpart for - the hardware's "Mod x:y"
    # matrix outputs and "Vib AM" - which together are about 5% of the bank's
    # routes.
    cells = read_matrix(p)
    per_destination = {}
    for (destination, row), amount in cells.items():
        per_destination.setdefault(destination, {})[row] = amount
    for column, destination in enumerate(sorted(per_destination), start=1):
        if column > 12:
            break
        s["modcol%d_dest" % column] = destination
        for row, amount in per_destination[destination].items():
            s["modcol%d_%s" % (column, SLOT_SUFFIX[row])] = round(amount, 4)

    # The MiniFreak reaches the matrix at full level: its "CycEnv AM" is a
    # hidden amount you can only modulate, and it sits at 100 unless a preset
    # moves it. Bizar exposes that amount as Cycle Amount and adds a
    # separate direct-to-filter shortcut. Pin both for any preset with a CycEnv
    # matrix route so an older header without the global default still plays.
    if any(row == 0 for _, row in cells):
        s["cycle_amount"] = 1.0
        s["cycle_filter_direct"] = 0

    # The matrix columns are not imported yet. The grid is read from the same
    # three places the hardware keeps it - Mx_Dot for the four fixed columns,
    # Mx_AssignDot with Mx_ColId for the nine assignable ones - but which
    # source row each dot belongs to is still open (assuming seven rows leaves
    # a third of the live cells in columns that have no destination). Two one-
    # route presets settle it: one routing Velo + AT -> Cutoff, one routing
    # Kbd -> Cutoff. Their two live cell indices differ by exactly the row
    # stride.

    for slot in (1, 2, 3):
        enable = p.get("FX%d_Enable" % slot, "1")
        if float(enable) < 0.5:
            s["fx%d_type" % slot] = 0
            continue
        if ("FX%d_Type" % slot) not in p:
            continue
        index = fx_type_index(p["FX%d_Type" % slot])
        fx = MF_FX[index] if index < len(MF_FX) else None
        if fx is None:
            s["fx%d_type" % slot] = 0
            continue
        s["fx%d_type" % slot] = FX_ORDINAL[fx]
        # Opt1 is the effect's preset menu position (index / (count - 1)).
        count = MF_FX_PRESETS.get(fx, 1)
        preset = enum_index(p.get("FX%d_Opt1" % slot, "0"), count) if count > 1 else 0
        if fx == "FX_DELAY":
            preset = preset // 2 + (7 if preset % 2 else 0)
        s["fx%d_subtype" % slot] = preset
        # Param1/2/3 are the MiniFreak's three user knobs in panel order
        # (Time, Intensity, Amount). Bizar's effects read Time and
        # Intensity as the same knob positions (Time over 1..1000, Intensity
        # over 0..0.95); only the free-running Delay reads Time as ms.
        params = [clamp(float(p.get("FX%d_Param%d" % (slot, n), "0.5")), 0.0, 1.0)
                  for n in (1, 2, 3)]
        s["fx%d_time" % slot] = round(1.0 + params[0] * 999.0, 2)
        s["fx%d_feedback" % slot] = round(params[1] * 0.95, 4)
        s["fx%d_mix" % slot] = round(params[2], 4)
        if fx == "FX_DELAY":
            if preset < 7:
                s["fx%d_time" % slot] = round(table_map(MF_DELAY_MS, params[0]), 2)
            if p.get("Delay_Routing", "1") != "0":
                s["fx%d_mix" % slot] = round(
                    fx_send_to_mix(table_map(MF_DELAY_SEND_WET, params[2])), 4)
            # Insert is Bizar's own law.
        elif fx == "FX_REVERB":
            amount = params[2]
            if p.get("Reverb_Routing", "1") != "0":
                mix = fx_send_to_mix(table_map(MF_REVERB_SEND_WET, amount))
            elif amount <= 0.5:
                # The Reverb's Insert routing is a linear crossfade. Match its
                # wet/dry ratio with the dry kept at full.
                mix = fx_send_to_mix(amount / max(1.0 - amount, 1e-6))
            else:
                mix = 1.0 - 0.5 * ((1.0 - amount) / amount) ** 0.5
            s["fx%d_mix" % slot] = round(mix, 4)
        elif fx == "FX_EQ":
            # The three controls are the low/mid/high band gains, not a wet
            # amount. The MiniFreak EQ spans -15 dB to +6 dB (0 dB at 5/7).
            for band, value in enumerate(params):
                s["fx%d_%s" % (slot, ("low", "mid", "high")[band])] = round(
                    -15.0 + value * 21.0, 4)
            s["fx%d_mix" % slot] = 1.0
        elif fx == "FX_PEAK_EQ":
            # Frequency and width as measured on MiniFreak V: Param1 sweeps
            # 32 Hz..15 kHz, Param3 narrows the bell (Q 1.1 at the centre,
            # about 3.3x per quarter turn).
            s["fx%d_frequency" % slot] = round(table_map(MF_PEAK_HZ, params[0], True), 2)
            s["fx%d_gain" % slot] = round(-15.0 + params[1] * 21.0, 4)
            s["fx%d_width" % slot] = round(clamp(1.1 * 118.0 ** (params[2] - 0.5), 0.1, 10.0), 4)
            s["fx%d_mix" % slot] = 1.0

    if s["osc1_mode"] == OSC1_ORDINAL["OSC_WAVETABLE"]:
        convert_osc1_wavetable(s)

    return name, s


def json_escape(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


def render(presets):
    lines = [
        "/* Generated from a MiniFreak bank export by",
        " * scripts/generate_minifreak_bank.py; do not edit.",
        " *",
        " * Engine indices are a best-effort mapping; see the script's tables for",
        " * which slots are pinned by a preset description and which are inferred. */",
        "#ifndef BIZAR_MINIFREAK_BANK_H",
        "#define BIZAR_MINIFREAK_BANK_H",
        "",
        "#define SF_MF_PRESET_COUNT %d" % len(presets),
        "",
    ]
    if not presets:
        lines += ["/* No bank was found at build time; the module ships its own presets only. */",
                  "", "#endif", ""]
        return "\n".join(lines)

    lines.append("static const char *const sf_mf_preset_names[SF_MF_PRESET_COUNT] = {")
    for name, _ in presets:
        lines.append('    "%s",' % json_escape(name))
    lines.append("};")
    lines.append("")
    lines.append("static const char *const sf_mf_preset_states[SF_MF_PRESET_COUNT] = {")
    for _, state in presets:
        body = ",".join(
            '"%s":%s' % (k, '"%s"' % v if isinstance(v, str) else repr(v))
            for k, v in state.items())
        # Values are numbers or plain strings (seq_data: digits . + , only), so
        # one escape pass over the body covers the C literal.
        lines.append('    "%s",' % json_escape(body))
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--bank")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    # Opt-in only: without --bank the header is empty (see docs/MINIFREAK_IMPORT.md).
    bank = args.bank
    presets = []
    if bank:
        if not Path(bank).exists():
            sys.exit("minifreak bank: %s not found" % bank)
        presets = [convert(n, p) for n, p in read_presets(bank)]

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(render(presets), encoding="utf-8", newline="\n")

    if args.report:
        import collections
        o1 = collections.Counter(OSC1_ORDINAL[MF_OSC1[i]] for i in range(len(MF_OSC1)))
        used1 = collections.Counter()
        used2 = collections.Counter()
        for _, s in presets:
            used1[s["osc1_mode"]] += 1
            used2[s["osc2_mode"]] += 1
        print("presets: %d" % len(presets))
        print("osc1 slots used: %s" % dict(sorted(used1.items())))
        print("osc2 slots used: %s" % dict(sorted(used2.items())))
    print("wrote %s (%d presets, %d bytes)" % (out, len(presets), out.stat().st_size))


if __name__ == "__main__":
    main()
