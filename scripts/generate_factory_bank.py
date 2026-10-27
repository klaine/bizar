#!/usr/bin/env python3
"""Bizar's own factory bank: 128 original presets, written as module state.

Every preset here was designed for Bizar from scratch. None is copied or
converted from another instrument's preset. Each one is a sparse state blob:
the module resets every patch parameter to its default before it applies a
preset, so a preset only has to name what it changes.

Presets are grouped into eight categories of sixteen, and the name carries a
two-letter category tag so the list reads well on Move's small display:

    BA Bass   LD Lead   KY Keys   PY Poly   PD Pad   PL Pluck   AR Arp/Seq   FX Effects

Usage:  generate_factory_bank.py [OUTPUT_HEADER] [--list] [--markdown PATH]

--markdown rewrites the preset list in docs/PRESETS.md.
"""

import argparse
import math
import sys
from pathlib import Path

STATE_VERSION = 19

# Engine menus, in the order of src/engines.h (OSC_* and ENGINE_*).
OSC1 = {name: i for i, name in enumerate([
    "BasicWaves", "Two Op FM", "Wavefolder", "Noise", "Wavetable",
    "SuperWave", "Harmo", "Formant", "V.Analog", "Waveshaper", "Chords",
    "KarplusStr", "SawX", "Harm", "Bass", "Modal", "Speech"])}
OSC2 = {name: i for i, name in enumerate([
    "BasicWaves", "Ring", "Wavefolder", "Noise", "Wavetable",
    "SuperWave", "Harmo", "Chord", "MultiFilter", "Crusher",
    "Comb Filter", "Surgeon Filter", "Phaser Filter", "Destroy", "V.Analog",
    "Waveshaper", "KarplusStr", "SawX", "Harm", "Bass", "Modal", "Speech"])}
OSC2.update({"Formant": 31, "Two Op FM": 32, "FM / RM": 33})

# The 32 built-in WaveEdit Online banks (assets/wavetables/README.md).
TABLES = {name: i for i, name in enumerate([
    "Harmonic", "Microwave 2", "Bell 03", "VoxSynth", "Spectral", "SuperSaw",
    "Fourier 2", "Noise Wave", "Folding", "Drone", "Sitar", "Tidal", "Harmonix",
    "Morphing", "Wavetrip", "Sine to Saw", "Saw Bend", "Basic Morph", "Tidy 006",
    "AM Sine", "Hmmm", "Iterative", "Osmaos", "Twist 02", "Sine Mutate",
    "Reso 00", "Sohler 34", "Sohler 35", "PD 101", "Sohler 43", "Mellow D",
    "Tidy 007"])}

# Modulation destinations, in the order of g_mod_destinations in src/bizar.c.
DEST = {name: i for i, name in enumerate([
    "Off", "Pitch", "Cut", "Shp1", "Shp2", "Vol2", "Res", "Out", "Tim1", "Tim2",
    "Wav1", "Wav2", "Ptc1", "Ptc2", "Vol1", "VCA", "EnvA", "Gld", "Atk", "Dec",
    "Sus", "Rel", "L1Rt", "L2Rt", "L1Am", "L2Am", "CyAm", "F1T", "F1I", "F1A",
    "F2T", "F2I", "F2A", "F3T", "F3I", "F3A", "Mac1", "Mac2", "Mac3", "Mac4",
    "Rise", "Fall", "Hold"])}
SOURCES = ("cyc", "env", "lfo1", "lfo2", "vel", "whl", "kbd")

FX = {name: i for i, name in enumerate([
    "Off", "Delay", "Reverb", "Chorus", "Drive", "Crusher", "Phaser", "EQ",
    "Flanger", "Super Unison", "Peak EQ", "Multi Comp"])}
# Per-type preset menus (src/bizar.c g_fx_presets_*), the ones used here.
DELAY = {name: i for i, name in enumerate([
    "Digital", "Stereo", "Ping-Pong", "Mono", "Filtered", "Filtered Ping-Pong",
    "Tape", "Digital Sync", "Stereo Sync", "Ping-Pong Sync", "Mono Sync",
    "Filtered Sync", "Filtered Ping-Pong Sync", "Tape Sync"])}
REVERB = {name: i for i, name in enumerate(
    ["Default", "Long", "Hall", "Echoes", "Room", "Dark Room"])}
CHORUS = {name: i for i, name in enumerate(["Default", "Lush", "Dark", "Shaded", "Single"])}
DRIVE = {name: i for i, name in enumerate(
    ["Classic", "Soft Clip", "Germanium", "Dual Fold", "Climb", "Tape"])}
PHASER = {name: i for i, name in enumerate(
    ["Default", "Default Sync", "Space", "Space Sync", "SnH", "SnH Sync"])}
FLANGER = {name: i for i, name in enumerate(["Default", "Default Sync", "Silly", "Silly Sync"])}
SUPER_UNISON = {name: i for i, name in enumerate(
    ["Classic", "Ravey", "Soli", "Slow", "Slow Trig", "Wide Trig", "Mono Trig", "Wavy"])}
MULTICOMP = {name: i for i, name in enumerate(
    ["OPP", "Bass Ctrl", "High Ctrl", "All Up", "Tighter"])}
FX_PRESETS = {"Delay": DELAY, "Reverb": REVERB, "Chorus": CHORUS, "Drive": DRIVE,
              "Phaser": PHASER, "Flanger": FLANGER, "Super Unison": SUPER_UNISON,
              "Multi Comp": MULTICOMP}

# Sync Delay divisions (src/effects.c g_delay_divs), read off the Time knob.
DELAY_DIVS = ["1/32T", "1/32", "1/16T", "1/32D", "1/16", "1/8T", "1/16D", "1/8",
              "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1/1T", "1/2D", "1/1"]
# LFO tempo divisions (lfoN_div).
LFO_DIVS = ["8 Bar D", "8 Bar", "4 Bar D", "8 Bar T", "4 Bar", "2 Bar D", "4 Bar T",
            "2 Bar", "1 Bar D", "2 Bar T", "1 Bar", "1/2D", "1 Bar T", "1/2", "1/4D",
            "1/2T", "1/4", "1/8D", "1/4T", "1/8", "1/16D", "1/8T", "1/16", "1/32D",
            "1/16T", "1/32", "1/32T"]
# Sequencer / arpeggiator rates (seq_div).
SEQ_DIVS = ["1/2D", "1/2", "1/2T", "1/4D", "1/4", "1/4T", "1/8D", "1/8", "1/8T",
            "1/16D", "1/16", "1/16T", "1/32D", "1/32", "1/32T"]
ARP = {name: i for i, name in enumerate(
    ["Up", "Down", "UpDown", "Random", "Played", "Random 2", "Chord", "Poly"])}
LFO_SHAPES = {name: i for i, name in enumerate(
    ["Triangle", "Saw", "Square", "Shaper", "Sine", "S&H", "Slew S&H", "ExpSaw", "ExpRamp"])}
VOICE = {"Poly": 0, "Mono": 1, "Unison": 2}
FILTER = {"LP": 0, "BP": 1, "HP": 2}
CYCLE = {"Env": 0, "Run": 1, "Loop": 2}
RETRIG = {"Free": 0, "Poly": 1, "Mono": 2, "Legato": 3, "One": 4}


# Two Op FM's Wave is a ratio quantizer: these land on the exact ratios.
FM_1, FM_2, FM_3, FM_4, FM_8 = 0.242, 0.488, 0.65, 0.75, 1.0
# Its Shape is a bipolar feedback around the centre: 0.5 is a clean sine pair.
FM_CLEAN = 0.5
# The engine's ratio quantizer (src/engines.c, from Plaits), in semitones per
# 1/128 of Wave; fm_ratio() finds the Wave for any ratio it can reach, so
# bells and glass can sit on non-integer ratios.
FM_QUANTIZER = [
    -12.0, -12.0, -12.0, -11.84, -11.84, -11.84, -11.11, -10.38,
    -9.65, -8.92, -8.19, -7.46, -6.73, -6.0, -6.0, -6.0,
    -5.5455116, -5.0910232, -4.6365348, -4.1820464, -4.1820464, -4.1820464, -3.6592906, -3.1365348,
    -2.6137790, -2.0910232, -1.5682674, -1.0455116, -0.5227558, 0.0, 0.0, 0.0,
    0.16, 0.16, 0.16, 0.89, 1.62, 2.35, 3.08, 3.81,
    4.54, 5.27, 6.0, 6.0, 6.0, 6.4544884, 6.9089768, 7.3634652,
    7.8179536, 7.8179536, 7.8179536, 8.2855299, 8.7531063, 9.2206827, 9.6882591, 9.6882591,
    9.6882591, 10.2661943, 10.8441295, 11.4220648, 12.0, 12.0, 12.0, 12.16,
    12.16, 12.16, 12.6297750, 13.0995500, 13.5693250, 14.0391000, 14.0391000, 14.0391000,
    14.9076199, 15.7761397, 16.6446596, 17.5131794, 17.5131794, 17.5131794, 18.0, 18.0,
    18.0, 18.5097750, 19.0195500, 19.0195500, 19.0195500, 19.8179536, 19.8179536, 19.8179536,
    20.6638643, 21.5097750, 21.5097750, 21.5097750, 22.1323313, 22.7548875, 23.3774438, 24.0,
    24.0, 24.0, 24.5097750, 25.0195500, 25.0195500, 25.0195500, 25.4740384, 25.9285268,
    26.3830152, 26.8375036, 26.8375036, 26.8375036, 27.3503204, 27.8631371, 27.8631371, 27.8631371,
    28.3973529, 28.9315686, 29.4657843, 30.0, 30.0, 30.0, 30.75, 31.5,
    32.25, 33.0, 33.75, 34.5, 35.25, 36.0, 36.0, 36.0,
    36.0,
]


def fm_ratio(ratio):
    semis = 12.0 * math.log2(ratio)
    for i in range(128):
        a, b = FM_QUANTIZER[i], FM_QUANTIZER[i + 1]
        if a <= semis <= b and b > a:
            return round((i + (semis - a) / (b - a)) / 128.0, 4)
    raise ValueError("FM ratio %g is out of reach" % ratio)


def hz(frequency):
    """Cutoff in Hz -> the normalised 0..1 log knob (20 Hz .. 18 kHz)."""
    frequency = min(max(float(frequency), 20.0), 18000.0)
    return round(math.log(frequency / 20.0) / math.log(900.0), 4)


class Patch:
    """A preset under construction. Keyword arguments are state keys; the
    helper methods cover the parts that need translating."""

    def __init__(self, name, **values):
        self.name = name
        self.state = {"version": STATE_VERSION}
        self.columns = 0
        self.set(**values)

    def set(self, **values):
        for key, value in values.items():
            if key in ("osc1_mode",):
                value = OSC1[value] if isinstance(value, str) else value
            elif key in ("osc2_mode",):
                value = OSC2[value] if isinstance(value, str) else value
            elif key in ("osc1_table", "osc2_table"):
                value = TABLES[value] if isinstance(value, str) else value
            elif key == "voice_mode":
                value = VOICE[value] if isinstance(value, str) else value
            elif key == "filter_mode":
                value = FILTER[value] if isinstance(value, str) else value
            elif key == "cycle_mode":
                value = CYCLE[value] if isinstance(value, str) else value
            elif key in ("lfo1_shape", "lfo2_shape"):
                value = LFO_SHAPES[value] if isinstance(value, str) else value
            elif key in ("lfo1_div", "lfo2_div"):
                value = LFO_DIVS.index(value) if isinstance(value, str) else value
            elif key in ("lfo1_retrig", "lfo2_retrig"):
                value = RETRIG[value] if isinstance(value, str) else value
            elif key == "cutoff_hz":
                key, value = "cutoff", hz(value)
            self.state[key] = value
        return self

    def mod(self, dest, **amounts):
        """One grid column: a destination fed by any of the seven sources."""
        self.columns += 1
        if self.columns > 12:
            raise ValueError("%s: more than twelve matrix columns" % self.name)
        column = self.columns
        self.state["modcol%d_dest" % column] = DEST[dest]
        for source, amount in amounts.items():
            if source not in SOURCES:
                raise ValueError("%s: unknown source %s" % (self.name, source))
            self.state["modcol%d_%s" % (column, source)] = amount
        return self

    def macro(self, index, value=0.0, *slots):
        """A macro's knob position and up to four (destination, amount) sockets."""
        self.state["macro%d" % index] = value
        for socket, (dest, amount) in enumerate(slots, start=1):
            self.state["macro%d_dest%d" % (index, socket)] = DEST[dest]
            self.state["macro%d_amt%d" % (index, socket)] = amount
        return self

    def motion(self, depth=1.0, *extra):
        """Macro 3 as the LFO 1 depth: the knob is the depth (0 = still,
        1 = full), so a wobble or gate can be dialled in or out live."""
        return self.macro(3, depth, ("L1Am", 1.0), *extra)

    def fx(self, slot, kind, preset=None, mix=None, time=None, intensity=None, **extra):
        """An effect slot. Time and Intensity are 0..1 knob positions, except
        that a free-running Delay's time is in ms and a Sync Delay's time is a
        note division name."""
        p = "fx%d_" % slot
        self.state[p + "type"] = FX[kind]
        if preset is not None:
            self.state[p + "subtype"] = FX_PRESETS[kind][preset]
        if mix is not None:
            self.state[p + "mix"] = mix
        if time is not None:
            if isinstance(time, str):
                knob = DELAY_DIVS.index(time) / (len(DELAY_DIVS) - 1.0)
                self.state[p + "time"] = round(1.0 + knob * 999.0, 2)
            elif kind == "Delay":
                self.state[p + "time"] = time
            else:
                self.state[p + "time"] = round(1.0 + time * 999.0, 2)
        if intensity is not None:
            self.state[p + "feedback"] = round(intensity * 0.95, 4)
        for key, value in extra.items():
            self.state[p + key] = value
        return self

    def unison(self, voices=4, spread=18.0, mode="Mono"):
        self.state["voice_mode"] = VOICE["Unison"]
        self.state["unison_voices"] = voices
        self.state["spread"] = spread
        self.state["unison_mode"] = 1 if mode == "Poly" else 0
        return self

    def chord(self, *intervals):
        self.state["chord"] = 1
        self.state["chord_notes"] = len(intervals)
        for i, interval in enumerate(intervals, start=1):
            self.state["chord_note%d" % i] = interval
        return self

    def arp(self, mode="Up", octaves=1, rate="1/16", gate=0.5, swing=0.0):
        self.state.update({"seq_mode": 1, "seq_arp_mode": ARP[mode],
                           "seq_arp_oct": octaves - 1, "seq_div": SEQ_DIVS.index(rate),
                           "seq_gate": gate, "seq_swing": swing})
        return self

    def seq(self, steps, rate="1/16", gate=0.5, swing=0.0, velocity=100):
        """A step pattern relative to C (note 60). Each step is a semitone
        offset, a tuple of offsets (a chord), '-' to tie the previous step on,
        or None for a rest. Patterns transpose with the key that is held."""
        out = []
        for step in steps:
            if step is None:
                out.append("")
                continue
            if step == "-":
                if not out or not out[-1]:
                    raise ValueError("%s: tie with nothing to hold" % self.name)
                notes = out[-1].split("+")
                out[-1] = "+".join("%s.%s.%d" % (n.split(".")[0], n.split(".")[1],
                                                 int(n.split(".")[2]) + 1) for n in notes)
                out.append("")
                continue
            offsets = step if isinstance(step, tuple) else (step,)
            out.append("+".join("%d.%d.0" % (60 + o, velocity) for o in offsets))
        self.state.update({"seq_mode": 2, "seq_div": SEQ_DIVS.index(rate),
                           "seq_gate": gate, "seq_swing": swing,
                           "seq_length": len(steps), "seq_data": ",".join(out)})
        return self

    def lane(self, index, dest, values, smooth=True):
        """A step-automation lane: one offset per step (-1..1, 0 = none)."""
        self.state["seq_lane%d_dest" % index] = DEST[dest]
        self.state["seq_lane%d_smooth" % index] = 1 if smooth else 0
        self.state["seq_lane%d_data" % index] = ",".join(
            "" if v == 0 else "%g" % v for v in values)
        return self


BANK = []


def patch(tag, name, **values):
    p = Patch("%s %s" % (tag, name), **values)
    BANK.append(p)
    return p


# Shared building blocks -----------------------------------------------------

def room(p, slot=3, mix=0.18, time=0.22, preset="Room"):
    return p.fx(slot, "Reverb", preset, mix=mix, time=time, intensity=0.45)


def hall(p, slot=3, mix=0.3, time=0.45, preset="Hall"):
    return p.fx(slot, "Reverb", preset, mix=mix, time=time, intensity=0.5)


def vibrato(p, depth=0.12, rate=5.2):
    """Direct LFO 1 vibrato in semitones, with the mod wheel adding more."""
    return p.set(lfo1_shape="Sine", lfo1_rate=rate, lfo1_depth=depth)


def keytrack(p, amount=0.3):
    """Key tracking on the cutoff, so higher notes stay as bright."""
    return p.mod("Cut", kbd=amount)


# --- BA: Bass (16) ----------------------------------------------------------
# Modelled on the bass archetypes of current club and bass music: 808s, Reese,
# neuro and wobble mid-basses, acid, house and techno basses, and clean subs.
# The low end stays mono (Stereo Width 0, or a touch on the mid-basses) and
# the oscillators restart on every note, so each note hits the same way.

def bass(name, **values):
    values.setdefault("voice_mode", "Mono")
    values.setdefault("stereo", 0.0)
    values.setdefault("osc_phase", 0)
    return patch("BA", name, **values)


SINE = dict(osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.0, osc1_shape=FM_CLEAN)
SUB_SINE = dict(osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.0, osc2_shape=FM_CLEAN)

# Trap 808: a sine with a fast pitch drop, a long tail, slides between legato
# notes and soft-clip harmonics so it reads on small speakers.
p = bass("808 Slide", **SINE, osc1_level=0.9, osc2_level=0.0, cutoff_hz=4000,
         resonance=0.0, filter_env=0.0, attack=1, decay=3500, sustain=0.0,
         release=400, glide=90, vel_vca=0.5)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=140, cycle_hold=0.0)
p.mod("Pitch", cyc=0.12)
p.fx(1, "Drive", "Soft Clip", mix=0.6, time=0.45, intensity=0.5)

# Acid: a resonant saw with velocity-driven accent, driven hard.
p = bass("Acid Squelch", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.85,
         osc2_level=0.0, cutoff_hz=300, resonance=0.82, filter_env=0.6, attack=1,
         decay=700, sustain=0.05, release=120, glide=60, vel_vcf=0.8, vel_env=0.7)
keytrack(p, 0.2)
p.fx(1, "Drive", "Classic", mix=0.6, time=0.55, intensity=0.45)
p.fx(2, "Delay", "Filtered Sync", mix=0.12, time="1/8D", intensity=0.35)

# Drum and bass Reese: two saws a few cents apart beating against each other,
# a sine sub, slow drift on the waveforms and tape grit on top.
p = bass("Reese Engine", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_shape=0.6,
         osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=0.18,
         osc2_level=0.55, cutoff_hz=1400, resonance=0.2, filter_env=0.1, attack=3,
         decay=800, sustain=0.9, release=200, glide=50, stereo=0.1,
         lfo2_shape="Sine", lfo2_rate=0.15)
p.mod("Tim1", lfo2=0.15)
p.mod("Tim2", lfo2=-0.15)
p.fx(1, "Drive", "Tape", mix=0.55, time=0.7, intensity=0.45)
p.fx(2, "Multi Comp", "OPP", mix=0.5)

# Neuro growl: a saw over its own sine sub, run through a comb filter whose
# delay is swept on the eighths, so the harmonics "talk"; driven and
# compressed hard. Macro 3 is the movement.
p = bass("Neuro Growl", osc1_mode="BasicWaves", osc1_wave=0.62, osc1_timbre=0.3,
         osc1_shape=0.45, osc1_level=0.8, osc2_mode="Comb Filter", osc2_wave=0.45,
         osc2_timbre=0.65, osc2_shape=0.25, osc2_level=0.65, cutoff_hz=3500,
         resonance=0.2, filter_env=0.0, attack=2, decay=800, sustain=0.95,
         release=150, glide=40, stereo=0.15, lfo1_shape="Triangle", lfo1_sync=1,
         lfo1_div="1/8", lfo1_retrig="Mono")
p.mod("Wav2", lfo1=0.22)
p.mod("Cut", lfo1=0.12)
p.fx(1, "Drive", "Classic", mix=0.5, time=0.55, intensity=0.45)
p.fx(2, "Multi Comp", "OPP", mix=0.65)
p.motion(0.8)

# House sub: a square with its sub oscillator, a short filter blip.
p = bass("House Sub", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.5,
         osc1_shape=0.8, osc1_level=0.8, osc2_level=0.0, cutoff_hz=500,
         resonance=0.15, filter_env=0.35, attack=1, decay=900, sustain=0.3,
         release=150, vel_vcf=0.4)
keytrack(p, 0.2)

# Nineties house organ bass: a triangle with a sine 19 semitones up.
p = bass("Organ Bass", osc1_mode="SuperWave", osc1_wave=0.66, osc1_timbre=0.0,
         osc1_shape=0.0, osc1_level=0.8, **SUB_SINE, osc2_tune=19, osc2_level=0.3,
         cutoff_hz=2500, resonance=0.0, filter_env=0.15, attack=1, decay=900,
         sustain=0.0, release=100)

# FM slap bass: a 1:1 FM pair with a touch of feedback for edge, whose index
# snaps shut after the attack (harder with velocity), over a clean sine sub.
p = bass("FM Slap", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.28,
         osc1_shape=0.58, osc1_level=0.75, **SUB_SINE, osc2_tune=-12, osc2_level=0.35,
         cutoff_hz=5000, resonance=0.0, filter_env=0.0, attack=1, decay=1800,
         sustain=0.4, release=150)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=220, cycle_hold=0.0)
p.mod("Tim1", cyc=0.3, vel=0.2)

# Rolling techno: a dark square with a two-stage filter envelope (a fast blip
# from the Cycling Envelope over a slower sweep) and a dark room behind it.
p = bass("Rolling Techno", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.4,
         osc1_level=0.8, osc2_level=0.0, cutoff_hz=220, resonance=0.35,
         filter_env=0.45, attack=1, decay=1200, sustain=0.2, release=150)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=120, cycle_hold=0.0)
p.mod("Cut", cyc=0.15, kbd=0.2)
p.fx(3, "Reverb", "Dark Room", mix=0.12, time=0.2, intensity=0.6)

# Dubstep wobble: synced LFO on cutoff and pulse shape, driven and compressed.
p = bass("Wobble 140", osc1_mode="V.Analog", osc1_wave=0.5, osc1_timbre=0.6,
         osc1_shape=0.9, osc1_level=0.7, osc2_mode="BasicWaves", osc2_wave=0.0,
         osc2_tune=-12, osc2_level=0.4, cutoff_hz=300, resonance=0.45,
         filter_env=0.0, attack=2, decay=800, sustain=0.9, release=150, glide=40,
         stereo=0.1, lfo1_shape="Sine", lfo1_sync=1, lfo1_div="1/8",
         lfo1_retrig="Mono")
p.mod("Cut", lfo1=0.35)
p.mod("Tim1", lfo1=0.2)
p.fx(1, "Drive", "Germanium", mix=0.65, time=0.65, intensity=0.4)
p.fx(2, "Multi Comp", "OPP", mix=0.5)
p.motion(0.6)

# UK garage warp bass: a saw pumping its filter on the quarter note.
p = bass("Garage Warp", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_shape=0.5,
         osc1_level=0.7, osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=12.1,
         osc2_level=0.25, cutoff_hz=600, resonance=0.3, filter_env=0.2, attack=2,
         decay=700, sustain=0.6, release=150, glide=70, stereo=0.1,
         lfo1_shape="Sine", lfo1_sync=1, lfo1_div="1/4", lfo1_retrig="Mono")
p.mod("Cut", lfo1=0.25)
p.fx(1, "Phaser", "Default", mix=0.25, time=0.2, intensity=0.4)
p.motion(0.6)

# Funk ladder bass: saw plus pulse, plucked by velocity, with a quick glide.
p = bass("Funk Ladder", osc1_mode="V.Analog", osc1_wave=0.5, osc1_timbre=0.4,
         osc1_shape=0.85, osc1_level=0.7, osc2_mode="V.Analog", osc2_wave=0.5,
         osc2_timbre=0.4, osc2_shape=0.85, osc2_tune=-12, osc2_level=0.4,
         cutoff_hz=450, resonance=0.35, filter_env=0.45, attack=1, decay=600,
         sustain=0.35, release=150, glide=30, vel_vcf=0.7)
keytrack(p, 0.3)

# Synthwave: an octave-stacked saw that drives eighth-note lines.
p = bass("Synthwave Drive", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.7,
         osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=12.05, osc2_level=0.35,
         cutoff_hz=800, resonance=0.2, filter_env=0.35, attack=1, decay=700,
         sustain=0.25, release=150, stereo=0.1)
p.fx(1, "Chorus", "Dark", mix=0.2, time=0.25, intensity=0.4)

# Phonk 808: a folded, distorted 808 with a harder pitch drop.
p = bass("Phonk 808", **SINE, osc1_level=0.9, osc2_mode="Wavefolder", osc2_wave=0.5,
         osc2_timbre=0.25, osc2_level=0.5, cutoff_hz=6000, resonance=0.0,
         filter_env=0.0, attack=1, decay=2500, sustain=0.0, release=300, glide=120)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=90, cycle_hold=0.0)
p.mod("Pitch", cyc=0.15)
p.fx(1, "Drive", "Climb", mix=0.75, time=0.75, intensity=0.5)
p.fx(2, "Multi Comp", "Bass Ctrl", mix=0.5)

# Minimal blip: a short sine pluck with an FM click on the attack.
p = bass("Minimal Blip", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.1,
         osc1_shape=FM_CLEAN, osc1_level=0.8, osc2_level=0.0, cutoff_hz=6000,
         resonance=0.0, filter_env=0.0, attack=1, decay=500, sustain=0.0,
         release=80)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=40, cycle_hold=0.0)
p.mod("Tim1", cyc=0.4)
p.fx(3, "Reverb", "Room", mix=0.08, time=0.15, intensity=0.5)

# Clean sub: a pure sine for layering under kicks; Macro 1 adds an octave
# harmonic so it also reads on small speakers.
p = bass("Clean Sub", **SINE, osc1_level=0.9, **SUB_SINE, osc2_tune=12,
         osc2_level=0.0, cutoff_hz=2000, resonance=0.0, filter_env=0.0, attack=3,
         decay=400, sustain=1.0, release=120, glide=40)

# Talking mid: a wavetable mid-bass swept on the beat, over its own sine sub.
p = bass("Talking Mid", osc1_mode="Wavetable", osc1_table="Microwave 2",
         osc1_wave=0.5, osc1_timbre=0.0, osc1_shape=0.5, osc1_level=0.6,
         **SUB_SINE, osc2_tune=-12, osc2_level=0.5, cutoff_hz=4000, resonance=0.15,
         filter_env=0.0, attack=2, decay=800, sustain=0.9, release=150, glide=40,
         stereo=0.15, lfo1_shape="Saw", lfo1_sync=1, lfo1_div="1/4",
         lfo1_retrig="Mono")
p.mod("Wav1", lfo1=0.4)
p.fx(1, "Drive", "Dual Fold", mix=0.4, time=0.6, intensity=0.5)
p.fx(2, "Multi Comp", "OPP", mix=0.6)
p.motion(0.6)

# --- LD: Lead (16) ----------------------------------------------------------
# Supersaw anthems, distorted plucks, octave-doubled glide saws, synthwave and
# chip squares, classic mono and sync leads, and a few voiced and FM colours.
# Most are mono with glide, with vibrato from LFO 1 and delay and reverb
# throws behind them.



def lead(name, **values):
    values.setdefault("voice_mode", "Mono")
    values.setdefault("stereo", 0.4)
    return patch("LD", name, **values)


p = patch("LD", "Supersaw Anthem", osc1_mode="SuperWave", osc1_wave=0.0,
          osc1_timbre=0.35, osc1_shape=0.85, osc1_level=0.6, osc2_mode="SuperWave",
          osc2_wave=0.0, osc2_timbre=0.3, osc2_shape=0.8, osc2_tune=12,
          osc2_level=0.3, cutoff_hz=7000, resonance=0.1, filter_env=0.1, attack=5,
          decay=800, sustain=0.85, release=450, stereo=0.7)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.2, time="1/8D", intensity=0.4)
hall(p, mix=0.3, time=0.5)

p = patch("LD", "Future Pluck Lead", osc1_mode="SuperWave", osc1_wave=0.0,
          osc1_timbre=0.25, osc1_shape=0.75, osc1_level=0.65, osc2_mode="BasicWaves",
          osc2_wave=0.5, osc2_tune=12, osc2_level=0.25, cutoff_hz=500,
          resonance=0.2, filter_env=0.5, attack=1, decay=1100, sustain=0.0,
          release=500, vel_vcf=0.5, stereo=0.6)
keytrack(p, 0.25)
p.fx(1, "Drive", "Soft Clip", mix=0.4, time=0.5, intensity=0.55)
p.fx(2, "Delay", "Ping-Pong Sync", mix=0.22, time="1/8D", intensity=0.45)
hall(p, mix=0.28)

p = lead("Glide Saw", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.6,
         osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=12.06, osc2_level=0.4,
         cutoff_hz=5000, resonance=0.15, filter_env=0.1, attack=3, decay=600,
         sustain=0.85, release=300, glide=120)
vibrato(p, 0.1, 5.5)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.2, time="1/8D", intensity=0.4)
hall(p, mix=0.25)

p = lead("Synthwave Lead", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.3,
         osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=0.08,
         osc2_level=0.4, cutoff_hz=3500, resonance=0.15, filter_env=0.2, attack=4,
         decay=900, sustain=0.75, release=350, glide=30)
vibrato(p, 0.12, 5.5)
p.fx(1, "Chorus", "Lush", mix=0.3, time=0.3, intensity=0.45)
p.fx(2, "Delay", "Tape Sync", mix=0.25, time="1/4D", intensity=0.45)
hall(p, mix=0.3)

p = lead("Mono Classic", osc1_mode="V.Analog", osc1_wave=0.5, osc1_timbre=0.45,
         osc1_shape=0.85, osc1_level=0.7, osc2_mode="V.Analog", osc2_wave=0.5,
         osc2_timbre=0.45, osc2_shape=0.85, osc2_tune=-12, osc2_level=0.4,
         cutoff_hz=1500, resonance=0.3, filter_env=0.4, attack=2, decay=1200,
         sustain=0.5, release=250, glide=60, vel_vcf=0.4, stereo=0.2)
keytrack(p, 0.3)
vibrato(p, 0.08, 5.0)
p.fx(1, "Drive", "Classic", mix=0.3, time=0.4, intensity=0.5)
p.fx(2, "Delay", "Stereo Sync", mix=0.15, time="1/8D", intensity=0.35)

p = lead("Sync Sweep", osc1_mode="V.Analog", osc1_wave=0.5, osc1_timbre=0.55,
         osc1_shape=0.9, osc1_level=0.7, osc2_level=0.0, cutoff_hz=6000,
         resonance=0.1, filter_env=0.1, attack=2, decay=900, sustain=0.75,
         release=250, glide=45)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=1200, cycle_hold=0.2)
p.mod("Tim1", cyc=0.25)
p.fx(1, "Drive", "Climb", mix=0.35, time=0.45, intensity=0.5)
p.fx(2, "Delay", "Stereo Sync", mix=0.18, time="1/4", intensity=0.35)
room(p)

p = lead("Chip Square", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.5,
         osc1_level=0.65, osc2_level=0.0, cutoff_hz=12000, resonance=0.0,
         filter_env=0.0, attack=1, decay=300, sustain=0.8, release=60, osc_phase=0,
         stereo=0.2)
vibrato(p, 0.25, 6.5)
p.fx(1, "Crusher", None, mix=0.3, time=0.15, intensity=0.15)
p.fx(2, "Delay", "Digital Sync", mix=0.2, time="1/8", intensity=0.35)

p = lead("Hyper Saw", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.5,
         osc1_shape=1.0, osc1_level=0.55, osc2_mode="SuperWave", osc2_wave=0.0,
         osc2_timbre=0.45, osc2_shape=1.0, osc2_tune=12, osc2_level=0.35,
         cutoff_hz=12000, resonance=0.0, filter_env=0.0, attack=2, decay=600,
         sustain=0.9, release=300, glide=40, stereo=0.8)
p.unison(voices=4, spread=20.0)
p.fx(1, "Drive", "Soft Clip", mix=0.4, time=0.55, intensity=0.6)
p.fx(2, "Delay", "Ping-Pong Sync", mix=0.18, time="1/8", intensity=0.4)
hall(p, mix=0.25)

p = lead("Whistle Sine", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.0,
         osc1_shape=FM_CLEAN, osc1_level=0.8, osc2_level=0.0, cutoff_hz=9000,
         resonance=0.0, filter_env=0.0, attack=15, decay=600, sustain=0.9,
         release=300, glide=180)
vibrato(p, 0.25, 5.8)
p.fx(1, "Delay", "Stereo Sync", mix=0.2, time="1/4", intensity=0.35)
room(p, mix=0.2)

p = lead("Wavetable Scream", osc1_mode="Wavetable", osc1_table="Microwave 2",
         osc1_wave=0.3, osc1_timbre=0.0, osc1_shape=0.6, osc1_level=0.7,
         osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=-12, osc2_level=0.25,
         cutoff_hz=6000, resonance=0.15, filter_env=0.1, attack=3, decay=900,
         sustain=0.75, release=300, glide=40)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=1200, cycle_hold=0.35)
p.mod("Wav1", cyc=0.35)
p.fx(1, "Drive", "Climb", mix=0.35, time=0.45, intensity=0.55)
p.fx(2, "Delay", "Ping-Pong Sync", mix=0.18, time="1/8D", intensity=0.4)
room(p)

p = lead("Vowel Lead", osc1_mode="Formant", osc1_wave=0.35, osc1_timbre=0.45,
         osc1_shape=0.5, osc1_level=0.75, osc2_mode="BasicWaves", osc2_wave=0.5,
         osc2_level=0.15, cutoff_hz=7000, resonance=0.1, filter_env=0.0,
         attack=20, decay=700, sustain=0.85, release=350, glide=50,
         lfo2_shape="Triangle", lfo2_rate=0.5)
vibrato(p, 0.12, 5.0)
p.mod("Tim1", lfo2=0.15)
hall(p, mix=0.25)

p = lead("Breath Flute", osc1_mode="Harmo", osc1_wave=0.1, osc1_timbre=0.25,
         osc1_shape=0.5, osc1_level=0.7, osc2_mode="Noise", osc2_wave=0.5,
         osc2_timbre=1.0, osc2_shape=0.0, osc2_level=0.06, cutoff_hz=5000,
         resonance=0.05, filter_env=0.1, attack=60, decay=500, sustain=0.85,
         release=350, glide=30)
vibrato(p, 0.1, 5.0)
hall(p, mix=0.25)

p = lead("FM Brass Lead", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.4,
         osc1_shape=0.6, osc1_level=0.7, osc2_mode="BasicWaves", osc2_wave=0.5,
         osc2_tune=0.06, osc2_level=0.2, cutoff_hz=6000, resonance=0.0,
         filter_env=0.1, attack=40, decay=900, sustain=0.75, release=300, glide=30)
p.mod("Tim1", env=0.2, vel=0.15)
vibrato(p, 0.08, 5.2)
p.fx(1, "Chorus", "Default", mix=0.2, time=0.3, intensity=0.4)
room(p)

p = lead("PWM Lead", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.3,
         osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.0, osc2_timbre=0.3,
         osc2_tune=0.1, osc2_level=0.45, cutoff_hz=3800, resonance=0.15,
         filter_env=0.15, attack=4, decay=800, sustain=0.8, release=300, glide=35,
         lfo2_shape="Triangle", lfo2_rate=0.7)
p.mod("Tim1", lfo2=0.3)
p.mod("Tim2", lfo2=-0.3)
p.fx(1, "Chorus", "Default", mix=0.25, time=0.3, intensity=0.4)
p.fx(2, "Delay", "Stereo Sync", mix=0.18, time="1/8D", intensity=0.35)
room(p)

p = lead("Rave Hoover Lead", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.6,
         osc1_shape=0.9, osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.5,
         osc2_tune=-12, osc2_level=0.3, cutoff_hz=5000, resonance=0.1,
         filter_env=0.0, attack=5, decay=800, sustain=0.9, release=350, glide=150)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=200, cycle_hold=0.0)
p.mod("Pitch", cyc=-0.015)
p.fx(1, "Super Unison", "Ravey", mix=0.45, time=0.5, intensity=0.5)
room(p, mix=0.2)

p = lead("Crystal Lead", osc1_mode="Two Op FM", osc1_wave=FM_3, osc1_timbre=0.3,
         osc1_shape=FM_CLEAN, osc1_level=0.65, osc2_mode="BasicWaves", osc2_wave=0.5,
         osc2_level=0.2, cutoff_hz=9000, resonance=0.0, filter_env=0.0, attack=2,
         decay=1500, sustain=0.6, release=500, glide=25)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=600, cycle_hold=0.0)
p.mod("Tim1", cyc=0.25)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.22, time="1/8D", intensity=0.45)
hall(p, mix=0.28)

# --- KY: Keys (16) ----------------------------------------------------------
# Electric pianos are FM pairs in the DX tradition: a 1:1 body plus an 8:1
# "tine" that only rings for the first instant (the Cycling Envelope opens
# its volume), with chorus or phaser and a room behind them.


def tine(p, level=0.3, fall=250):
    p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=fall, cycle_hold=0.0)
    return p.mod("Vol2", cyc=level)


p = patch("KY", "Tine EP", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.2,
          osc1_shape=FM_CLEAN, osc1_level=0.7, osc2_mode="Two Op FM",
          osc2_wave=FM_8, osc2_timbre=0.3, osc2_shape=FM_CLEAN, osc2_level=0.02,
          cutoff_hz=8000, resonance=0.0, filter_env=0.0, attack=1, decay=3500,
          sustain=0.2, release=500)
tine(p)
p.mod("Tim1", vel=0.2)
p.fx(1, "Phaser", "Default", mix=0.25, time=0.2, intensity=0.3)
room(p)

p = patch("KY", "Ballad EP", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.26,
          osc1_shape=0.55, osc1_level=0.7, osc2_mode="Two Op FM", osc2_wave=FM_8,
          osc2_timbre=0.35, osc2_shape=FM_CLEAN, osc2_level=0.02, cutoff_hz=9000,
          resonance=0.0, filter_env=0.0, attack=1, decay=4000, sustain=0.2,
          release=700, stereo=0.6)
tine(p, 0.35, 200)
p.mod("Tim1", vel=0.25)
p.fx(1, "Chorus", "Lush", mix=0.35, time=0.3, intensity=0.45)
hall(p, mix=0.25)

p = patch("KY", "Lofi Keys", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.18,
          osc1_shape=FM_CLEAN, osc1_level=0.7, osc2_mode="Two Op FM",
          osc2_wave=FM_8, osc2_timbre=0.25, osc2_shape=FM_CLEAN, osc2_level=0.02,
          cutoff_hz=2200, resonance=0.05, filter_env=0.0, attack=2, decay=3000,
          sustain=0.25, release=500, lfo1_shape="Slew S&H", lfo1_rate=2.0,
          lfo1_depth=0.08)
tine(p, 0.25, 250)
p.fx(1, "Drive", "Tape", mix=0.3, time=0.35, intensity=0.4)
p.fx(2, "Crusher", None, mix=0.2, time=0.2, intensity=0.1)
p.fx(3, "Chorus", "Dark", mix=0.3, time=0.15, intensity=0.5)

p = patch("KY", "Reed EP", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.3,
          osc1_shape=0.65, osc1_level=0.6, osc2_mode="Wavefolder", osc2_wave=0.3,
          osc2_timbre=0.2, osc2_level=0.5, cutoff_hz=4500, resonance=0.0,
          filter_env=0.1, attack=1, decay=2500, sustain=0.3, release=400)
p.mod("Tim2", vel=0.35)
p.fx(1, "Chorus", "Default", mix=0.2, time=0.25, intensity=0.4)
room(p)

p = patch("KY", "Clav Funk", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.75,
          osc1_level=0.7, osc2_mode="Comb Filter", osc2_wave=0.7, osc2_timbre=0.4,
          osc2_shape=0.5, osc2_level=0.3, cutoff_hz=1500, resonance=0.3,
          filter_env=0.45, attack=1, decay=1200, sustain=0.0, release=120,
          vel_vcf=0.8)
keytrack(p, 0.25)
p.fx(1, "Phaser", "Default", mix=0.25, time=0.3, intensity=0.3)

# Drawbar organ: the Chords engine's OCT chord stacks octaves like drawbars,
# and above Shape 0.575 it plays soft few-harmonic waves (below that it is a
# buzzy string-machine divide-down). Inv/Trsp near 0.3 centres the chord on
# the key (at 0 it sits two octaves down). A slow chorus stands in for the
# rotary.
p = patch("KY", "Drawbar Organ", osc1_mode="Chords", osc1_wave=0.0, osc1_timbre=0.3,
          osc1_shape=0.6, osc1_level=0.75, osc2_level=0.0, cutoff_hz=9000,
          resonance=0.0, filter_env=0.0, attack=3, decay=200, sustain=1.0, release=70,
          vel_vca=0.0, stereo=0.6)
p.fx(1, "Drive", "Soft Clip", mix=0.25, time=0.3, intensity=0.5)
p.fx(2, "Chorus", "Default", mix=0.3, time=0.35, intensity=0.35)
room(p)

# Gospel organ: the same drawbar stack, brighter, with the third-harmonic
# percussion click (a sine a twelfth up that only sounds at the attack),
# overdriven into a fast "rotary" chorus.
p = patch("KY", "Gospel Organ", osc1_mode="Chords", osc1_wave=0.0, osc1_timbre=0.3,
          osc1_shape=0.66, osc1_level=0.7, osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.0, osc2_shape=FM_CLEAN, osc2_tune=19,
          osc2_level=0.0, cutoff_hz=9000, resonance=0.0, filter_env=0.0, attack=2,
          decay=200, sustain=1.0, release=80, vel_vca=0.0, stereo=0.6)
tine(p, 0.45, 300)
p.fx(1, "Drive", "Germanium", mix=0.45, time=0.5, intensity=0.5)
p.fx(2, "Chorus", "Default", mix=0.35, time=0.85, intensity=0.4)
room(p)

p = patch("KY", "Rave Piano", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.35,
          osc1_shape=0.6, osc1_level=0.6, osc2_mode="Two Op FM", osc2_wave=FM_2,
          osc2_timbre=0.25, osc2_shape=FM_CLEAN, osc2_tune=0.05, osc2_level=0.4,
          cutoff_hz=9000, resonance=0.0, filter_env=0.0, attack=1, decay=2500,
          sustain=0.15, release=400, stereo=0.5)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=400, cycle_hold=0.0)
p.mod("Tim1", cyc=0.25)
p.fx(1, "Chorus", "Default", mix=0.2, time=0.3, intensity=0.4)
room(p)

p = patch("KY", "Vibes", osc1_mode="Two Op FM", osc1_wave=FM_4, osc1_timbre=0.15,
          osc1_shape=FM_CLEAN, osc1_level=0.7, osc2_mode="Two Op FM", osc2_wave=FM_1,
          osc2_timbre=0.0, osc2_shape=FM_CLEAN, osc2_level=0.3, cutoff_hz=9000,
          resonance=0.0, filter_env=0.0, attack=1, decay=4500, sustain=0.0,
          release=1500, lfo2_shape="Sine", lfo2_rate=5.0)
p.mod("Vol1", lfo2=0.15)
p.mod("Vol2", lfo2=0.1)
room(p, mix=0.22)

# Marimba: a wooden bar is a fundamental plus a tuned partial two octaves up
# that dies away almost at once. Two sines do exactly that.
p = patch("KY", "Marimba", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.0, osc1_shape=FM_CLEAN, osc1_level=0.85, osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.0, osc2_shape=FM_CLEAN,
          osc2_tune=24, osc2_level=0.0, cutoff_hz=9000, resonance=0.0,
          filter_env=0.0, attack=1, decay=1400, sustain=0.0, release=450,
          osc_phase=0, stereo=0.4)
tine(p, 0.5, 160)
room(p)

# Music box: a soft Modal tine (a low Timbre keeps the strike gentle) an
# octave up, with a faint sine partial on top and a long ring.
p = patch("KY", "Music Box", osc1_mode="Modal", osc1_wave=0.55, osc1_timbre=0.3,
          osc1_shape=0.65, osc1_tune=12, osc1_level=0.8, osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.0, osc2_shape=FM_CLEAN,
          osc2_tune=24, osc2_level=0.12, cutoff_hz=10000, resonance=0.0,
          filter_env=0.0, attack=1, decay=3500, sustain=0.0, release=1500, stereo=0.6)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.12, time="1/8D", intensity=0.3)
hall(p, mix=0.28)

p = patch("KY", "Felt Keys", osc1_mode="Harmo", osc1_wave=0.0, osc1_timbre=0.15,
          osc1_shape=0.0, osc1_level=0.7, osc2_mode="Two Op FM", osc2_wave=FM_1,
          osc2_timbre=0.12, osc2_shape=FM_CLEAN, osc2_level=0.25, cutoff_hz=1500,
          resonance=0.0, filter_env=0.2, attack=4, decay=3000, sustain=0.2,
          release=1200, vel_vcf=0.6)
p.fx(1, "Chorus", "Dark", mix=0.15, time=0.2, intensity=0.4)
hall(p, mix=0.35, time=0.5)

p = patch("KY", "Harpsichord", osc1_mode="KarplusStr", osc1_wave=0.0, osc1_timbre=0.85,
          osc1_shape=0.62, osc1_level=0.8, osc2_mode="KarplusStr", osc2_wave=0.0,
          osc2_timbre=0.7, osc2_shape=0.58, osc2_tune=12, osc2_level=0.35,
          cutoff_hz=9000, resonance=0.0, filter_env=0.0, attack=1, decay=2600,
          sustain=0.15, release=450, vel_vca=0.3)
room(p)

p = patch("KY", "Poly Keys", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.4,
          osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=-0.06,
          osc2_level=0.3, cutoff_hz=2500, resonance=0.1, filter_env=0.3, attack=2,
          decay=1500, sustain=0.4, release=400, stereo=0.6,
          lfo1_shape="Triangle", lfo1_rate=0.4)
p.mod("Tim1", lfo1=0.2)
p.fx(1, "Chorus", "Lush", mix=0.35, time=0.3, intensity=0.45)
room(p)

# Glass: a pure sine body and a second sine at 2.76x, the inharmonic partial
# that makes struck glass sound like glass, with an FM burst on the strike.
p = patch("KY", "Glass Keys", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.0, osc1_shape=FM_CLEAN, osc1_level=0.7,
          osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.08,
          osc2_shape=FM_CLEAN, osc2_tune=17.6, osc2_level=0.3, cutoff_hz=12000,
          resonance=0.0, filter_env=0.0, attack=1, decay=3500, sustain=0.15,
          release=1200, stereo=0.7)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=120, cycle_hold=0.0)
p.mod("Tim2", cyc=0.35)
p.fx(1, "Chorus", "Default", mix=0.2, time=0.25, intensity=0.35)
p.fx(2, "Delay", "Ping-Pong Sync", mix=0.15, time="1/8D", intensity=0.4)
hall(p, mix=0.3)

p = patch("KY", "Musette", osc1_mode="Harmo", osc1_wave=0.3, osc1_timbre=0.4,
          osc1_shape=0.5, osc1_level=0.55, osc2_mode="Harmo", osc2_wave=0.3,
          osc2_timbre=0.4, osc2_shape=0.5, osc2_tune=0.15, osc2_level=0.45,
          cutoff_hz=5000, resonance=0.0, filter_env=0.0, attack=15, decay=200,
          sustain=1.0, release=150, vel_vca=0.3)
room(p)

# --- PY: Polysynths and brass (16) -----------------------------------------
# Stabs, chords, brass and string machines: detuned oscillator pairs into a
# filter envelope, chorus for width, and short delay and room throws.

patch("PY", "Init", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.6,
      osc2_mode="BasicWaves", osc2_wave=0.5, osc2_level=0.0)

# Analog brass: two detuned saws through a low filter that opens with the
# envelope's attack and settles back, a small upward scoop into each note,
# and more bite the harder you play.
p = patch("PY", "Analog Brass", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.6,
          osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=0.1, osc2_level=0.55,
          cutoff_hz=420, resonance=0.15, filter_env=0.5, attack=45, decay=900,
          sustain=0.65, release=250, vel_vcf=0.6, stereo=0.5)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=90, cycle_hold=0.0)
p.mod("Pitch", cyc=-0.004)
keytrack(p, 0.35)
p.fx(1, "Chorus", "Default", mix=0.2, time=0.3, intensity=0.4)
room(p)

p = patch("PY", "Super Stab", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.3,
          osc1_shape=0.8, osc1_level=0.6, osc2_mode="SuperWave", osc2_wave=0.0,
          osc2_timbre=0.25, osc2_shape=0.7, osc2_tune=-12, osc2_level=0.35,
          cutoff_hz=1200, resonance=0.15, filter_env=0.45, attack=1, decay=1300,
          sustain=0.2, release=400, vel_vcf=0.5, stereo=0.7)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.18, time="1/8D", intensity=0.35)
hall(p, mix=0.22)

# Silk: soft triangles stacked with a quiet saw, filtered low, with a slow
# attack edge and a wide, lush chorus.
p = patch("PY", "Silk Poly", osc1_mode="SuperWave", osc1_wave=0.66, osc1_timbre=0.15,
          osc1_shape=0.5, osc1_level=0.65, osc2_mode="BasicWaves", osc2_wave=0.5,
          osc2_tune=-0.07, osc2_level=0.25, cutoff_hz=1100, resonance=0.05,
          filter_env=0.15, attack=25, decay=2500, sustain=0.75, release=900, stereo=0.8)
p.fx(1, "Chorus", "Lush", mix=0.4, time=0.25, intensity=0.45)
hall(p, mix=0.25)

# Dub techno: a minor-seventh chord on one key, dark and short, drowned in tape
# echo and a dark room.
p = patch("PY", "Dub Chord", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.6,
          osc2_mode="BasicWaves", osc2_wave=0.0, osc2_tune=0.08, osc2_level=0.4,
          cutoff_hz=600, resonance=0.3, filter_env=0.35, attack=1, decay=800,
          sustain=0.0, release=300, stereo=0.5)
p.chord(3, 7, 10)
p.fx(1, "Delay", "Tape Sync", mix=0.3, time="1/4D", intensity=0.6)
p.fx(2, "Reverb", "Dark Room", mix=0.25, time=0.35, intensity=0.6)

# Future bass: a wide supersaw chord ducked on every beat, as a sidechain would.
p = patch("PY", "Pumping Saws", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.4,
          osc1_shape=0.9, osc1_level=0.6, osc2_mode="SuperWave", osc2_wave=0.0,
          osc2_timbre=0.35, osc2_shape=0.8, osc2_tune=12, osc2_level=0.3,
          cutoff_hz=6000, resonance=0.05, filter_env=0.0, attack=5, decay=1000,
          sustain=0.9, release=400, stereo=0.8, lfo1_shape="Saw", lfo1_sync=1,
          lfo1_div="1/4", lfo1_retrig="Free")
p.mod("VCA", lfo1=-0.6)
hall(p, mix=0.25)
p.motion(1.0)

p = patch("PY", "Sync Brass", osc1_mode="V.Analog", osc1_wave=0.5, osc1_timbre=0.5,
          osc1_shape=0.8, osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.5,
          osc2_tune=0.1, osc2_level=0.4, cutoff_hz=1500, resonance=0.1,
          filter_env=0.35, attack=30, decay=1500, sustain=0.6, release=400, stereo=0.5)
p.set(cycle_mode="Env", cycle_rise=40, cycle_fall=900, cycle_hold=0.3)
p.mod("Tim1", cyc=0.18, vel=0.06)
p.fx(1, "Chorus", "Default", mix=0.2, time=0.3, intensity=0.4)
room(p)

# String machine: a gently detuned supersaw an octave stack deep, through an
# ensemble chorus. The octave layer is a supersaw too, so nothing beats
# against a fixed pulse.
p = patch("PY", "String Machine", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.18,
          osc1_shape=0.5, osc1_level=0.6, osc2_mode="SuperWave", osc2_wave=0.0,
          osc2_timbre=0.12, osc2_shape=0.4, osc2_tune=12, osc2_level=0.22,
          cutoff_hz=3000, resonance=0.05, filter_env=0.0, attack=300, decay=1500,
          sustain=0.9, release=1200, stereo=0.8)
keytrack(p, 0.2)
p.fx(1, "Chorus", "Lush", mix=0.35, time=0.2, intensity=0.4)
hall(p, mix=0.3)

p = patch("PY", "Wavetable Poly", osc1_mode="Wavetable", osc1_table="Morphing",
          osc1_wave=0.2, osc1_timbre=0.0, osc1_shape=0.5, osc1_level=0.65,
          osc2_mode="Wavetable", osc2_table="Harmonic", osc2_wave=0.5, osc2_tune=12,
          osc2_level=0.3, cutoff_hz=3600, resonance=0.1, filter_env=0.2, attack=5,
          decay=1800, sustain=0.6, release=600, stereo=0.6,
          lfo2_shape="Triangle", lfo2_rate=0.2)
p.mod("Wav1", env=0.2, lfo2=0.15)
p.fx(1, "Chorus", "Default", mix=0.2, time=0.3, intensity=0.4)
hall(p, mix=0.22)

p = patch("PY", "Eighties Poly", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.3,
          osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=0.06,
          osc2_level=0.5, cutoff_hz=2400, resonance=0.18, filter_env=0.3, attack=4,
          decay=1600, sustain=0.5, release=600, stereo=0.7,
          lfo1_shape="Triangle", lfo1_rate=0.6)
p.mod("Tim1", lfo1=0.15)
p.fx(1, "Chorus", "Lush", mix=0.35, time=0.3, intensity=0.45)
p.fx(2, "Delay", "Stereo Sync", mix=0.15, time="1/4", intensity=0.35)
hall(p, mix=0.2)

p = patch("PY", "Power Fifths", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.6,
          osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=-12.05, osc2_level=0.45,
          cutoff_hz=2000, resonance=0.2, filter_env=0.35, attack=2, decay=1500,
          sustain=0.5, release=500, stereo=0.5)
p.chord(7, 12)
p.fx(1, "Drive", "Classic", mix=0.3, time=0.45, intensity=0.5)
p.fx(2, "Delay", "Ping-Pong Sync", mix=0.15, time="1/4D", intensity=0.35)
room(p)

# Choir: the Speech engine's sustained vowels, "a" and "o" a few cents apart,
# spread into an ensemble by Super Unison, then chorus and a hall.
p = patch("PY", "Vowel Choir", osc1_mode="Speech", osc1_wave=0.0, osc1_timbre=0.5,
          osc1_shape=0.0, osc1_level=0.8, osc2_mode="Speech", osc2_wave=0.0,
          osc2_timbre=0.45, osc2_shape=0.75, osc2_tune=0.08, osc2_level=0.6,
          cutoff_hz=5000, resonance=0.0, filter_env=0.0, attack=220, decay=1500,
          sustain=0.9, release=1200, stereo=0.85)
p.fx(1, "Super Unison", "Slow", mix=0.5, time=0.4, intensity=0.5)
p.fx(2, "Chorus", "Lush", mix=0.3, time=0.2, intensity=0.4)
hall(p, mix=0.32)

p = patch("PY", "Chord Memory", osc1_mode="Chords", osc1_wave=0.45, osc1_timbre=0.3,
          osc1_shape=0.75, osc1_level=0.7, osc2_mode="BasicWaves", osc2_wave=0.5,
          osc2_tune=-12, osc2_level=0.3, cutoff_hz=2600, resonance=0.15,
          filter_env=0.3, attack=2, decay=1500, sustain=0.4, release=600, stereo=0.6,
          lfo1_shape="S&H", lfo1_sync=1, lfo1_div="1/2", lfo1_retrig="Poly")
p.mod("Wav1", lfo1=0.25)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.18, time="1/8D", intensity=0.4)
hall(p, mix=0.22)

p = patch("PY", "Trance Supersaw", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.4,
          osc1_shape=0.85, osc1_level=0.6, osc2_mode="SuperWave", osc2_wave=0.0,
          osc2_timbre=0.35, osc2_shape=0.8, osc2_tune=12, osc2_level=0.3,
          cutoff_hz=5000, resonance=0.1, filter_env=0.1, attack=3, decay=1200,
          sustain=0.75, release=600, stereo=0.8)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.2, time="1/8D", intensity=0.4)
hall(p, mix=0.28)
p.macro(1, 0.0, ("Sus", -0.75), ("EnvA", 0.35), ("Cut", -0.2), ("F1A", 0.35))

p = patch("PY", "Bell Poly", osc1_mode="Two Op FM", osc1_wave=FM_3, osc1_timbre=0.25,
          osc1_shape=FM_CLEAN, osc1_level=0.55, osc2_mode="BasicWaves", osc2_wave=0.5,
          osc2_tune=0.06, osc2_level=0.35, cutoff_hz=4000, resonance=0.05,
          filter_env=0.15, attack=2, decay=2500, sustain=0.35, release=900, stereo=0.6)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=500, cycle_hold=0.0)
p.mod("Tim1", cyc=0.25)
p.fx(1, "Chorus", "Default", mix=0.25, time=0.3, intensity=0.4)
hall(p, mix=0.25)

p = patch("PY", "Soft Horns", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.0,
          osc1_level=0.55, osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.3,
          osc2_shape=0.6, osc2_level=0.4, cutoff_hz=900, resonance=0.1,
          filter_env=0.3, attack=90, decay=1500, sustain=0.7, release=500, stereo=0.5)
p.mod("Tim2", env=0.15)
p.fx(1, "Chorus", "Default", mix=0.2, time=0.3, intensity=0.4)
hall(p, mix=0.22)

# --- PD: Pads (16) ----------------------------------------------------------
# Slow attacks and long releases, detuned stacks, two LFOs at unrelated rates
# so the movement never quite repeats, and long reverbs that become part of
# the sound.

def pad(name, **values):
    values.setdefault("stereo", 0.85)
    values.setdefault("vel_vca", 0.4)
    return patch("PD", name, **values)


p = pad("Warm Blanket", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.6,
        osc2_mode="BasicWaves", osc2_wave=0.0, osc2_tune=-0.12, osc2_level=0.5,
        cutoff_hz=1100, resonance=0.1, filter_env=0.15, attack=900, decay=2500,
        sustain=0.85, release=2500, lfo1_shape="Sine", lfo1_rate=0.13,
        lfo2_shape="Triangle", lfo2_rate=0.07)
p.mod("Tim2", lfo1=0.25)
p.mod("Cut", lfo2=0.05)
p.fx(1, "Chorus", "Lush", mix=0.35, time=0.2, intensity=0.45)
hall(p, mix=0.35, time=0.55)

p = pad("Glass Cathedral", osc1_mode="Wavetable", osc1_table="Bell 03",
        osc1_wave=0.4, osc1_timbre=0.0, osc1_shape=0.6, osc1_level=0.6,
        osc2_mode="Wavetable", osc2_table="Harmonix", osc2_wave=0.6, osc2_tune=12,
        osc2_level=0.35, cutoff_hz=6000, resonance=0.05, filter_env=0.0,
        attack=1200, decay=2500, sustain=0.85, release=4000,
        lfo1_shape="Sine", lfo1_rate=0.11, lfo2_shape="Triangle", lfo2_rate=0.07)
p.mod("Wav1", lfo1=0.25)
p.mod("Wav2", lfo2=-0.2)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.2, time="1/4D", intensity=0.55)
hall(p, mix=0.42, time=0.65, preset="Long")

p = pad("Slow Bloom", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.2,
        osc1_shape=0.6, osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.5,
        osc2_tune=12, osc2_level=0.25, cutoff_hz=450, resonance=0.15,
        filter_env=0.0, attack=1500, decay=3000, sustain=0.9, release=3500)
p.set(cycle_mode="Env", cycle_rise=4000, cycle_fall=6000, cycle_hold=0.7)
p.mod("Cut", cyc=0.35)
p.fx(1, "Chorus", "Lush", mix=0.3, time=0.2, intensity=0.4)
hall(p, mix=0.38, time=0.55)

# Breathing choir: sustained "a" and "o" vowels that slowly trade places, as
# if the choir breathes between them, in an ensemble and a long hall.
p = pad("Breathing Choir", osc1_mode="Speech", osc1_wave=0.0, osc1_timbre=0.4,
        osc1_shape=0.0, osc1_level=0.75, osc2_mode="Speech", osc2_wave=0.0,
        osc2_timbre=0.35, osc2_shape=0.75, osc2_tune=-0.1, osc2_level=0.65,
        cutoff_hz=4500, resonance=0.0, filter_env=0.0, attack=1200, decay=2500,
        sustain=0.9, release=3500, lfo2_shape="Sine", lfo2_rate=0.12)
p.mod("Vol1", lfo2=0.2)
p.mod("Vol2", lfo2=-0.2)
p.fx(1, "Super Unison", "Slow", mix=0.5, time=0.4, intensity=0.5)
p.fx(2, "Chorus", "Lush", mix=0.3, time=0.2, intensity=0.4)
hall(p, mix=0.42, time=0.6, preset="Long")

p = pad("Dark Matter", osc1_mode="Wavetable", osc1_table="Drone",
        osc1_wave=0.3, osc1_timbre=0.0, osc1_shape=0.35, osc1_level=0.65,
        osc2_mode="BasicWaves", osc2_wave=0.5, osc2_shape=0.6, osc2_tune=-12,
        osc2_level=0.35, cutoff_hz=800, resonance=0.25, filter_env=0.0,
        attack=2000, decay=3000, sustain=0.9, release=4500,
        lfo1_shape="Slew S&H", lfo1_rate=0.2, lfo2_shape="Sine", lfo2_rate=0.05)
p.mod("Wav1", lfo1=0.15, lfo2=0.2)
p.mod("Cut", lfo2=0.08)
hall(p, mix=0.42, time=0.7, preset="Long")

p = pad("Shimmer Strings", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.3,
        osc1_shape=0.75, osc1_level=0.55, osc2_mode="SuperWave", osc2_wave=0.66,
        osc2_timbre=0.2, osc2_shape=0.6, osc2_tune=24, osc2_level=0.25,
        cutoff_hz=4500, resonance=0.05, filter_env=0.0, attack=900, decay=2500,
        sustain=0.9, release=3500)
p.fx(1, "Chorus", "Lush", mix=0.3, time=0.2, intensity=0.45)
p.fx(2, "Delay", "Ping-Pong Sync", mix=0.22, time="1/4D", intensity=0.6)
hall(p, mix=0.45, time=0.7, preset="Long")

p = pad("Tidal Drift", osc1_mode="Wavetable", osc1_table="Tidal", osc1_wave=0.3,
        osc1_timbre=0.0, osc1_shape=0.55, osc1_level=0.6, osc2_mode="Wavetable",
        osc2_table="Morphing", osc2_wave=0.6, osc2_tune=7, osc2_level=0.35,
        cutoff_hz=3500, resonance=0.1, filter_env=0.0, attack=1500, decay=2500,
        sustain=0.9, release=3500, lfo1_shape="Sine", lfo1_rate=0.17,
        lfo2_shape="Sine", lfo2_rate=0.06)
p.mod("Wav1", lfo1=0.3)
p.mod("Wav2", lfo2=0.3)
p.mod("L1Rt", lfo2=0.2)
hall(p, mix=0.4, time=0.6, preset="Long")

p = pad("Lofi Pad", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.3,
        osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=-0.1,
        osc2_level=0.4, cutoff_hz=1400, resonance=0.05, filter_env=0.0,
        attack=700, decay=2500, sustain=0.85, release=2500,
        lfo1_shape="Slew S&H", lfo1_rate=1.5, lfo1_depth=0.07)
p.fx(1, "Drive", "Tape", mix=0.35, time=0.35, intensity=0.4)
p.fx(2, "Chorus", "Dark", mix=0.35, time=0.15, intensity=0.5)
hall(p, mix=0.32)

p = pad("Airy Strings", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.25,
        osc1_shape=0.7, osc1_level=0.6, osc2_mode="Noise", osc2_wave=0.5,
        osc2_timbre=1.0, osc2_shape=0.0, osc2_level=0.08, cutoff_hz=2800,
        resonance=0.05, filter_env=0.0, attack=1000, decay=2500, sustain=0.9,
        release=3000, lfo2_shape="Sine", lfo2_rate=0.1)
p.mod("Cut", lfo2=0.06)
p.fx(1, "Chorus", "Lush", mix=0.3, time=0.2, intensity=0.45)
hall(p, mix=0.4, time=0.6, preset="Long")

p = pad("Pulse Width Pad", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.4,
        osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.0, osc2_timbre=0.4,
        osc2_tune=0.1, osc2_level=0.5, cutoff_hz=1800, resonance=0.1,
        filter_env=0.05, attack=700, decay=2000, sustain=0.85, release=2500,
        lfo1_shape="Triangle", lfo1_rate=0.35, lfo2_shape="Triangle", lfo2_rate=0.27)
p.mod("Tim1", lfo1=0.35)
p.mod("Tim2", lfo2=0.35)
p.fx(1, "Chorus", "Default", mix=0.25, time=0.2, intensity=0.4)
hall(p, mix=0.32)

# Sine Mutate only changes over the lower half of its frames, so the pad
# drifts there.
p = pad("Frozen Lake", osc1_mode="Wavetable", osc1_table="Sine Mutate",
        osc1_wave=0.15, osc1_timbre=0.0, osc1_shape=0.6, osc1_level=0.6,
        osc2_mode="Noise", osc2_wave=0.5, osc2_timbre=1.0, osc2_shape=0.0,
        osc2_level=0.06, cutoff_hz=3500, resonance=0.1, filter_env=0.0,
        attack=2000, decay=3000, sustain=0.9, release=5000,
        lfo2_shape="Sine", lfo2_rate=0.06)
p.mod("Wav1", lfo2=0.15)
p.fx(1, "Delay", "Filtered Ping-Pong Sync", mix=0.22, time="1/2", intensity=0.55)
hall(p, mix=0.48, time=0.8, preset="Long")

p = pad("Warm Sine Pad", osc1_mode="Harmo", osc1_wave=0.0, osc1_timbre=0.2,
        osc1_shape=0.6, osc1_level=0.65, osc2_mode="Harmo", osc2_wave=0.0,
        osc2_timbre=0.0, osc2_shape=0.4, osc2_tune=12.05, osc2_level=0.35,
        cutoff_hz=3000, resonance=0.0, filter_env=0.0, attack=1200, decay=2500,
        sustain=0.9, release=3000, lfo1_shape="Sine", lfo1_rate=0.2)
p.mod("Tim1", lfo1=0.1)
p.fx(1, "Chorus", "Lush", mix=0.3, time=0.2, intensity=0.4)
hall(p, mix=0.38, time=0.55)

p = pad("Spectral Haze", osc1_mode="Wavetable", osc1_table="Spectral",
        osc1_wave=0.3, osc1_timbre=0.0, osc1_shape=0.45, osc1_level=0.6,
        osc2_mode="Wavetable", osc2_table="VoxSynth", osc2_wave=0.5, osc2_tune=-0.1,
        osc2_level=0.45, cutoff_hz=3000, resonance=0.1, filter_env=0.0,
        attack=1500, decay=2500, sustain=0.85, release=4000,
        lfo1_shape="Sine", lfo1_rate=0.09, lfo2_shape="Slew S&H", lfo2_rate=0.25)
p.mod("Wav1", lfo1=0.3)
p.mod("Wav2", lfo2=0.2)
p.fx(1, "Phaser", "Space", mix=0.3, time=0.1, intensity=0.4)
hall(p, mix=0.42, time=0.6, preset="Long")

p = pad("Phased Sunrise", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.2,
        osc1_shape=0.6, osc1_level=0.65, osc2_mode="Phaser Filter", osc2_wave=0.4,
        osc2_timbre=0.5, osc2_shape=0.5, osc2_level=0.6, cutoff_hz=4000,
        resonance=0.05, filter_env=0.0, attack=1200, decay=2500, sustain=0.85,
        release=3000, lfo2_shape="Sine", lfo2_rate=0.1)
p.mod("Wav2", lfo2=0.3)
hall(p, mix=0.38, time=0.5)

p = pad("Harmonic Field", osc1_mode="Harmo", osc1_wave=0.6, osc1_timbre=0.3,
        osc1_shape=0.5, osc1_level=0.6, osc2_mode="Harmo", osc2_wave=0.3,
        osc2_timbre=0.5, osc2_shape=0.6, osc2_tune=7, osc2_level=0.35,
        cutoff_hz=5000, resonance=0.0, filter_env=0.0, attack=1500, decay=2500,
        sustain=0.9, release=3500, lfo1_shape="Sine", lfo1_rate=0.07,
        lfo2_shape="Triangle", lfo2_rate=0.11)
p.mod("Tim1", lfo1=0.25)
p.mod("Tim2", lfo2=0.25)
hall(p, mix=0.42, time=0.6, preset="Long")

p = pad("Ensemble Wash", osc1_mode="Chords", osc1_wave=0.2, osc1_timbre=0.2,
        osc1_shape=0.85, osc1_level=0.6, osc2_mode="SuperWave", osc2_wave=0.0,
        osc2_timbre=0.2, osc2_shape=0.6, osc2_level=0.3, cutoff_hz=2500,
        resonance=0.05, filter_env=0.0, attack=1300, decay=2500, sustain=0.9,
        release=3500)
p.fx(1, "Chorus", "Lush", mix=0.35, time=0.2, intensity=0.5)
hall(p, mix=0.4, time=0.6, preset="Long")

# --- PL: Plucks and mallets (16) -------------------------------------------
# Short attack, medium decay, little or no sustain; the synth plucks layer an
# octave-down oscillator and open with a snappy filter envelope, and most sit
# in a ping-pong delay and a room.

def pluck(name, **values):
    values.setdefault("stereo", 0.6)
    return patch("PL", name, **values)


p = pluck("Nylon Pick", osc1_mode="KarplusStr", osc1_wave=0.0, osc1_timbre=0.45,
          osc1_shape=0.6, osc1_level=0.85, osc2_level=0.0, cutoff_hz=6000,
          resonance=0.0, filter_env=0.0, attack=1, decay=2500, sustain=0.0,
          release=600)
room(p)

p = pluck("Melodic Techno", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.6,
          osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=-12.08, osc2_level=0.45,
          cutoff_hz=350, resonance=0.25, filter_env=0.5, attack=3, decay=1300,
          sustain=0.0, release=600, vel_vcf=0.5)
keytrack(p, 0.25)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.25, time="1/8D", intensity=0.5)
hall(p, mix=0.3)

p = pluck("Prog House Pluck", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.3,
          osc1_shape=0.8, osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.0,
          osc2_tune=12, osc2_level=0.25, cutoff_hz=450, resonance=0.2,
          filter_env=0.5, attack=1, decay=1100, sustain=0.0, release=500,
          vel_vcf=0.4, stereo=0.8)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.22, time="1/8D", intensity=0.45)
hall(p, mix=0.28)

# Bell: FM at 3.5:1, the classic inharmonic bell ratio, whose brightness
# fades slower than the strike, over a quiet hum an octave down.
p = pluck("Bell Pluck", osc1_mode="Two Op FM", osc1_wave=fm_ratio(3.5), osc1_timbre=0.28,
          osc1_shape=FM_CLEAN, osc1_level=0.75, osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.0, osc2_shape=FM_CLEAN, osc2_tune=-12,
          osc2_level=0.1, cutoff_hz=12000, resonance=0.0, filter_env=0.0, attack=1,
          decay=5000, sustain=0.0, release=2500, osc_phase=0)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=1500, cycle_hold=0.0)
p.mod("Tim1", cyc=0.15)
hall(p, mix=0.3)

# Kalimba: a sine tine with its inharmonic "plink" partial (about 5.9x) that
# only rings for the first moment.
p = pluck("Kalimba", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.0, osc1_shape=FM_CLEAN, osc1_level=0.85, osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.0, osc2_shape=FM_CLEAN,
          osc2_tune=30.7, osc2_level=0.0, cutoff_hz=9000, resonance=0.0,
          filter_env=0.0, attack=1, decay=2400, sustain=0.0, release=700, osc_phase=0)
tine(p, 0.45, 110)
room(p)

p = pluck("Harp Glint", osc1_mode="KarplusStr", osc1_wave=0.0, osc1_timbre=0.6,
          osc1_shape=0.75, osc1_level=0.8, osc2_mode="KarplusStr", osc2_wave=0.0,
          osc2_timbre=0.4, osc2_shape=0.7, osc2_tune=12, osc2_level=0.3,
          cutoff_hz=9000, resonance=0.0, filter_env=0.0, attack=1, decay=3000,
          sustain=0.0, release=1200)
hall(p, mix=0.3)

p = pluck("Synth Koto", osc1_mode="KarplusStr", osc1_wave=0.0, osc1_timbre=0.8,
          osc1_shape=0.45, osc1_level=0.8, osc2_mode="Comb Filter", osc2_wave=0.6,
          osc2_timbre=0.3, osc2_shape=0.5, osc2_level=0.25, cutoff_hz=7000,
          resonance=0.0, filter_env=0.0, attack=1, decay=1800, sustain=0.0, release=600)
p.fx(1, "Delay", "Stereo Sync", mix=0.15, time="1/4", intensity=0.3)
room(p)

p = pluck("Square Blip", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.0,
          osc1_level=0.65, osc2_mode="BasicWaves", osc2_wave=0.0, osc2_timbre=0.5,
          osc2_tune=12, osc2_level=0.3, cutoff_hz=1500, resonance=0.2,
          filter_env=0.4, attack=1, decay=600, sustain=0.0, release=300)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.25, time="1/8", intensity=0.45)
room(p)

# Sitar: a bright string plucked near the bridge, through a comb that adds
# the buzzing jawari resonance, sliding up into each note.
p = pluck("Sitar", osc1_mode="KarplusStr", osc1_wave=0.0, osc1_timbre=0.9,
          osc1_shape=0.75, osc1_level=0.85, osc2_mode="Comb Filter", osc2_wave=0.72,
          osc2_timbre=0.6, osc2_shape=0.2, osc2_level=0.35, cutoff_hz=9000,
          resonance=0.0, filter_env=0.0, attack=1, decay=3500, sustain=0.0,
          release=900, glide=40, voice_mode="Mono")
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=180, cycle_hold=0.0)
p.mod("Pitch", cyc=-0.005)
hall(p, mix=0.25)

p = pluck("Resonant Drip", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.65,
          osc2_mode="BasicWaves", osc2_wave=0.0, osc2_tune=0.06, osc2_level=0.35,
          cutoff_hz=300, resonance=0.7, filter_env=0.5, attack=1, decay=900,
          sustain=0.0, release=400, vel_vcf=0.5)
keytrack(p, 0.3)
p.fx(1, "Delay", "Filtered Ping-Pong Sync", mix=0.25, time="1/8D", intensity=0.5)
room(p)

p = pluck("Wavetable Pluck", osc1_mode="Wavetable", osc1_table="Wavetrip",
          osc1_wave=0.6, osc1_timbre=0.0, osc1_shape=0.5, osc1_level=0.65,
          osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.0, osc2_shape=FM_CLEAN,
          osc2_tune=-12, osc2_level=0.25, cutoff_hz=1800, resonance=0.15,
          filter_env=0.4, attack=1, decay=900, sustain=0.0, release=500, vel_vcf=0.5)
p.mod("Wav1", env=-0.4)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.2, time="1/8D", intensity=0.4)
room(p)

p = pluck("Muted Guitar", osc1_mode="KarplusStr", osc1_wave=0.0, osc1_timbre=0.3,
          osc1_shape=0.4, osc1_level=0.9, osc2_level=0.0, cutoff_hz=3500,
          resonance=0.0, filter_env=0.0, attack=1, decay=1100, sustain=0.0, release=250)
p.fx(1, "Chorus", "Single", mix=0.2, time=0.3, intensity=0.4)
room(p, mix=0.15)

# Plucked glass: the partials of Glass Keys, struck harder and shorter.
p = pluck("Plucked Glass", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.0, osc1_shape=FM_CLEAN, osc1_level=0.7,
          osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.1,
          osc2_shape=FM_CLEAN, osc2_tune=17.6, osc2_level=0.3, cutoff_hz=12000,
          resonance=0.0, filter_env=0.0, attack=1, decay=1600, sustain=0.0,
          release=900, stereo=0.7)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=90, cycle_hold=0.0)
p.mod("Tim2", cyc=0.4)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.22, time="1/8D", intensity=0.45)
hall(p, mix=0.28)

# Melodic house: a sine with a tiny downward pitch blip on the attack.
p = pluck("Deep House Pluck", osc1_mode="Harmo", osc1_wave=0.0, osc1_timbre=0.25,
          osc1_shape=0.0, osc1_level=0.8, osc2_mode="Two Op FM", osc2_wave=FM_2,
          osc2_timbre=0.15, osc2_shape=FM_CLEAN, osc2_level=0.2, cutoff_hz=5000,
          resonance=0.0, filter_env=0.0, attack=2, decay=1000, sustain=0.0,
          release=500, osc_phase=0, stereo=0.2)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=60, cycle_hold=0.0)
p.mod("Pitch", cyc=0.02)
p.fx(1, "Chorus", "Single", mix=0.15, time=0.3, intensity=0.3)
p.fx(2, "Delay", "Ping-Pong Sync", mix=0.2, time="1/8D", intensity=0.4)
room(p)

p = pluck("Glock", osc1_mode="Modal", osc1_wave=0.7, osc1_timbre=0.8,
          osc1_shape=0.5, osc1_level=0.75, osc2_level=0.0, cutoff_hz=14000,
          resonance=0.0, filter_env=0.0, attack=1, decay=2500, sustain=0.0, release=1200)
hall(p, mix=0.22)

p = pluck("Chord Pluck", osc1_mode="Chords", osc1_wave=0.4, osc1_timbre=0.3,
          osc1_shape=0.9, osc1_level=0.7, osc2_level=0.0, cutoff_hz=900, resonance=0.2,
          filter_env=0.45, attack=1, decay=1000, sustain=0.0, release=500)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.2, time="1/8D", intensity=0.4)
room(p)

# --- AR: Arpeggios and sequences (16) --------------------------------------
# Hold a key (or a chord for the arpeggios) and they run at Move's tempo.
# Mostly 1/16 rates, mono-ish plucks, with delay and a little reverb.

def arp(name, **values):
    values.setdefault("stereo", 0.5)
    return patch("AR", name, **values)


p = arp("Up Sparkle", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.3,
        osc1_level=0.65, osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=12,
        osc2_level=0.25, cutoff_hz=1500, resonance=0.2, filter_env=0.4, attack=1,
        decay=600, sustain=0.0, release=300)
p.arp("Up", octaves=2, rate="1/16", gate=0.5)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.2, time="1/8D", intensity=0.4)
room(p)

p = arp("Down Pulse", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.2,
        osc1_level=0.65, osc2_mode="BasicWaves", osc2_wave=0.5, osc2_tune=-12,
        osc2_level=0.35, cutoff_hz=900, resonance=0.35, filter_env=0.45, attack=1,
        decay=600, sustain=0.0, release=250)
p.arp("Down", octaves=2, rate="1/16", gate=0.4)
p.fx(1, "Delay", "Stereo Sync", mix=0.18, time="1/8", intensity=0.3)
room(p)

p = arp("Random Bells", osc1_mode="Two Op FM", osc1_wave=FM_3, osc1_timbre=0.35,
        osc1_shape=FM_CLEAN, osc1_level=0.7, osc2_level=0.0, cutoff_hz=9000,
        resonance=0.0, filter_env=0.0, attack=1, decay=1200, sustain=0.0, release=700,
        stereo=0.7)
p.arp("Random", octaves=3, rate="1/16", gate=0.4)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.25, time="1/8D", intensity=0.5)
hall(p, mix=0.3)

p = arp("Berlin School", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.65,
        osc2_mode="BasicWaves", osc2_wave=0.0, osc2_tune=12, osc2_level=0.25,
        cutoff_hz=700, resonance=0.45, filter_env=0.4, attack=1, decay=500,
        sustain=0.0, release=200, lfo2_shape="Triangle", lfo2_sync=1, lfo2_div="2 Bar")
p.mod("Cut", lfo2=0.15)
p.seq([0, 12, 7, 12, 3, 12, 7, 12, 0, 12, 7, 12, 5, 12, 7, 15], rate="1/16", gate=0.5)
p.fx(1, "Delay", "Stereo Sync", mix=0.2, time="1/8D", intensity=0.4)
room(p)

p = arp("Acid Line", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.85,
        osc2_level=0.0, cutoff_hz=280, resonance=0.8, filter_env=0.55, attack=1,
        decay=500, sustain=0.0, release=80, voice_mode="Mono", glide=60,
        vel_vcf=0.8, vel_env=0.6, stereo=0.0, osc_phase=0)
p.seq([0, 0, 12, None, 0, 10, None, 12, 0, None, 3, 15, 0, 12, None, 7],
      rate="1/16", gate=0.55, velocity=96)
p.fx(1, "Drive", "Classic", mix=0.5, time=0.5, intensity=0.4)
p.fx(2, "Delay", "Filtered Sync", mix=0.15, time="1/8D", intensity=0.35)

p = arp("Chord Stabber", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.25,
        osc1_shape=0.6, osc1_level=0.6, osc2_mode="BasicWaves", osc2_wave=0.5,
        osc2_tune=-12, osc2_level=0.3, cutoff_hz=1500, resonance=0.2,
        filter_env=0.35, attack=1, decay=700, sustain=0.0, release=300, stereo=0.7)
p.arp("Chord", rate="1/8", gate=0.35, swing=0.2)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.2, time="1/8D", intensity=0.4)
room(p)

p = arp("Pluck Cascade", osc1_mode="KarplusStr", osc1_wave=0.0, osc1_timbre=0.5,
        osc1_shape=0.6, osc1_level=0.85, osc2_level=0.0, cutoff_hz=7000,
        resonance=0.0, filter_env=0.0, attack=1, decay=1500, sustain=0.0, release=600,
        stereo=0.7)
p.arp("UpDown", octaves=2, rate="1/16T", gate=0.5)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.22, time="1/8D", intensity=0.45)
hall(p, mix=0.25)

p = arp("Glass Steps", osc1_mode="Wavetable", osc1_table="Bell 03",
        osc1_wave=0.3, osc1_timbre=0.0, osc1_shape=0.6, osc1_level=0.65,
        osc2_level=0.0, cutoff_hz=3000, resonance=0.15, filter_env=0.3, attack=1,
        decay=900, sustain=0.0, release=400, stereo=0.7)
p.seq([0, 7, 12, 14, 0, 7, 12, 19, 0, 7, 12, 14, 0, 7, 15, 12], rate="1/16", gate=0.5)
p.lane(1, "Wav1", [0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0, -0.1, -0.2, -0.1])
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.22, time="1/8D", intensity=0.45)
hall(p, mix=0.25)

p = arp("Minor Pulse Seq", osc1_mode="V.Analog", osc1_wave=0.5, osc1_timbre=0.4,
        osc1_shape=0.8, osc1_level=0.7, osc2_level=0.0, cutoff_hz=800,
        resonance=0.4, filter_env=0.45, attack=1, decay=500, sustain=0.0, release=200,
        voice_mode="Mono")
p.seq([0, None, 0, 3, None, 3, 7, None, 5, None, 5, 3, None, 3, 2, None],
      rate="1/16", gate=0.6)
p.lane(1, "Cut", [0, 0, 0.1, 0.1, 0, 0, 0.2, 0.2, 0, 0, 0.1, 0.1, 0, 0, 0.25, 0.25],
       smooth=False)
p.fx(1, "Delay", "Stereo Sync", mix=0.18, time="1/8D", intensity=0.4)
room(p)

p = arp("Octave Runner", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.65,
        osc2_mode="BasicWaves", osc2_wave=0.0, osc2_tune=-12, osc2_level=0.3,
        cutoff_hz=1100, resonance=0.3, filter_env=0.35, attack=1, decay=500,
        sustain=0.1, release=200)
p.arp("Up", octaves=4, rate="1/16", gate=0.45)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.15, time="1/8D", intensity=0.3)
room(p)

p = arp("Played Order", osc1_mode="Harmo", osc1_wave=0.3, osc1_timbre=0.3,
        osc1_shape=0.4, osc1_level=0.7, osc2_level=0.0, cutoff_hz=3000,
        resonance=0.1, filter_env=0.3, attack=1, decay=900, sustain=0.0, release=400)
p.arp("Played", octaves=1, rate="1/8T", gate=0.5)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.22, time="1/4T", intensity=0.45)
hall(p, mix=0.22)

p = arp("Trance Gate", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.35,
        osc1_shape=0.8, osc1_level=0.6, osc2_mode="SuperWave", osc2_wave=0.0,
        osc2_timbre=0.3, osc2_shape=0.7, osc2_tune=12, osc2_level=0.3,
        cutoff_hz=3500, resonance=0.1, filter_env=0.0, attack=5, decay=800,
        sustain=0.9, release=300, stereo=0.8, lfo1_shape="Square", lfo1_sync=1,
        lfo1_div="1/16", lfo1_retrig="Poly")
p.mod("VCA", lfo1=-0.5)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.18, time="1/8D", intensity=0.4)
hall(p, mix=0.25)
p.motion(1.0)

p = arp("Bubbly FM", osc1_mode="Two Op FM", osc1_wave=FM_2, osc1_timbre=0.4,
        osc1_shape=FM_CLEAN, osc1_level=0.7, osc2_level=0.0, cutoff_hz=2500,
        resonance=0.2, filter_env=0.3, attack=1, decay=500, sustain=0.0, release=250,
        lfo1_shape="S&H", lfo1_sync=1, lfo1_div="1/16")
p.mod("Tim1", lfo1=0.2)
p.arp("Random 2", octaves=2, rate="1/16", gate=0.4)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.2, time="1/8D", intensity=0.4)
room(p)

# House bassline on the offbeats, the classic rolling pattern.
p = arp("Offbeat Bassline", osc1_mode="BasicWaves", osc1_wave=0.0, osc1_timbre=0.4,
        osc1_shape=0.6, osc1_level=0.8, osc2_level=0.0, cutoff_hz=450,
        resonance=0.3, filter_env=0.35, attack=1, decay=450, sustain=0.1,
        release=100, voice_mode="Mono", stereo=0.0, osc_phase=0)
p.seq([None, None, 0, None, None, None, 0, 12, None, None, 0, None, None, None, 0, 10],
      rate="1/16", gate=0.5)

p = arp("Melodic Techno Arp", osc1_mode="Harmo", osc1_wave=0.2, osc1_timbre=0.3,
        osc1_shape=0.2, osc1_level=0.7, osc2_mode="BasicWaves", osc2_wave=0.5,
        osc2_tune=-12, osc2_level=0.25, cutoff_hz=1200, resonance=0.25,
        filter_env=0.4, attack=2, decay=700, sustain=0.0, release=400, stereo=0.6,
        lfo2_shape="Triangle", lfo2_sync=1, lfo2_div="4 Bar")
p.mod("Cut", lfo2=0.15)
p.arp("UpDown", octaves=2, rate="1/16", gate=0.5)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.25, time="1/8D", intensity=0.5)
hall(p, mix=0.3)

p = arp("Future Chord Arp", osc1_mode="SuperWave", osc1_wave=0.0, osc1_timbre=0.4,
        osc1_shape=0.85, osc1_level=0.6, osc2_mode="SuperWave", osc2_wave=0.66,
        osc2_timbre=0.2, osc2_shape=0.6, osc2_tune=12, osc2_level=0.3,
        cutoff_hz=4000, resonance=0.1, filter_env=0.15, attack=1, decay=800,
        sustain=0.2, release=400, stereo=0.85)
p.arp("Chord", rate="1/8T", gate=0.6)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.2, time="1/8D", intensity=0.4)
hall(p, mix=0.3)

# --- FX: Effects, drones and percussion (16) -------------------------------
# Transition effects (risers, downlifters, impacts), drones and textures, and
# synthesized drums. Hold the key for the risers.

p = patch("FX", "Noise Riser", osc1_mode="Noise", osc1_wave=0.5, osc1_timbre=1.0,
          osc1_shape=0.0, osc1_level=0.7, osc2_mode="SuperWave", osc2_wave=0.0,
          osc2_timbre=0.4, osc2_shape=0.7, osc2_level=0.35, cutoff_hz=200,
          resonance=0.4, filter_env=0.0, attack=6000, decay=1000, sustain=1.0,
          release=1500, stereo=0.8, lfo1_shape="Square", lfo1_sync=1,
          lfo1_div="1/16", lfo1_retrig="Poly")
p.set(cycle_mode="Env", cycle_rise=8000, cycle_fall=2000, cycle_hold=1.0)
p.mod("Cut", cyc=0.7)
p.mod("Pitch", cyc=0.2)
p.mod("VCA", lfo1=-0.3)
hall(p, mix=0.35, time=0.5)
p.motion(0.0)

# Downlifter: noise and a supersaw that fall two octaves over seven seconds
# while the filter closes, swept by a flanger, into a long hall.
p = patch("FX", "Downlifter", osc1_mode="Noise", osc1_wave=0.5, osc1_timbre=1.0,
          osc1_shape=0.0, osc1_level=0.6, osc2_mode="SuperWave", osc2_wave=0.0,
          osc2_timbre=0.45, osc2_shape=0.8, osc2_level=0.45, cutoff_hz=1500,
          resonance=0.35, filter_env=0.55, attack=1, decay=7000, sustain=0.0,
          release=3000, stereo=0.9)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=7000, cycle_hold=0.0)
p.mod("Pitch", cyc=0.4)
p.fx(1, "Flanger", "Silly", mix=0.35, time=0.15, intensity=0.6)
hall(p, mix=0.4, time=0.65, preset="Long")

p = patch("FX", "Sub Drop Impact", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.0,
          osc1_shape=FM_CLEAN, osc1_level=0.9, osc2_mode="Noise", osc2_wave=0.5,
          osc2_timbre=1.0, osc2_shape=0.0, osc2_level=0.3, cutoff_hz=3000,
          resonance=0.0, filter_env=0.3, attack=1, decay=5000, sustain=0.0,
          release=2000, voice_mode="Mono", stereo=0.3, osc_phase=0)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=2500, cycle_hold=0.0)
p.mod("Pitch", cyc=0.25)
p.fx(1, "Drive", "Soft Clip", mix=0.4, time=0.45, intensity=0.5)
hall(p, mix=0.35, time=0.55)

p = patch("FX", "Laser Zap", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.7,
          osc2_mode="BasicWaves", osc2_wave=0.0, osc2_tune=7, osc2_level=0.3,
          cutoff_hz=4000, resonance=0.5, filter_env=0.3, attack=1, decay=600,
          sustain=0.0, release=300)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=220, cycle_hold=0.0)
p.mod("Pitch", cyc=0.6)
p.fx(1, "Delay", "Ping-Pong Sync", mix=0.25, time="1/8", intensity=0.5)

p = patch("FX", "Robot Voice", osc1_mode="Speech", osc1_wave=0.6, osc1_timbre=0.5,
          osc1_shape=0.5, osc1_level=0.8, osc2_mode="Ring", osc2_wave=0.0,
          osc2_timbre=0.3, osc2_tune=-5, osc2_level=0.3, cutoff_hz=9000,
          resonance=0.0, filter_env=0.0, attack=1, decay=1500, sustain=0.3, release=300)
p.fx(1, "Crusher", None, mix=0.25, time=0.25, intensity=0.2)
room(p)

p = patch("FX", "Metal Hit", osc1_mode="Modal", osc1_wave=0.9, osc1_timbre=0.7,
          osc1_shape=0.6, osc1_level=0.8, osc2_mode="Ring", osc2_wave=0.5,
          osc2_timbre=0.4, osc2_tune=11, osc2_level=0.3, cutoff_hz=12000,
          resonance=0.0, filter_env=0.0, attack=1, decay=2500, sustain=0.0, release=1500)
hall(p, mix=0.32)

p = patch("FX", "Wind Tunnel", osc1_mode="Noise", osc1_wave=0.5, osc1_timbre=1.0,
          osc1_shape=0.0, osc1_level=0.7, osc2_level=0.0, cutoff_hz=500,
          resonance=0.45, filter_mode="BP", filter_env=0.0, attack=1500, decay=2500,
          sustain=1.0, release=3000, stereo=0.9, lfo1_shape="Slew S&H", lfo1_rate=0.4,
          lfo2_shape="Sine", lfo2_rate=0.07)
p.mod("Cut", lfo1=0.07, lfo2=0.08)
hall(p, mix=0.38, time=0.6, preset="Long")

# Kick: a sine whose pitch drops about 18 semitones in the first 60 ms, with
# a short noise click on the attack.
p = patch("FX", "Analog Kick", osc1_mode="Two Op FM", osc1_wave=FM_1, osc1_timbre=0.0, osc1_shape=FM_CLEAN, osc1_level=0.95, osc2_mode="Noise",
          osc2_wave=0.5, osc2_timbre=1.0, osc2_shape=0.0, osc2_level=0.0,
          cutoff_hz=6000, resonance=0.0, filter_env=0.0, attack=1, decay=750,
          sustain=0.0, release=150, voice_mode="Mono", stereo=0.0, osc_phase=0,
          vel_vca=0.5)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=60, cycle_hold=0.0)
p.mod("Pitch", cyc=0.3)
p.mod("Vol2", cyc=0.25)
p.fx(1, "Drive", "Soft Clip", mix=0.35, time=0.4, intensity=0.5)

# Snare: a tuned body that drops in pitch, under a brighter, longer noise.
p = patch("FX", "Noise Snare", osc1_mode="Noise", osc1_wave=0.5, osc1_timbre=1.0,
          osc1_shape=0.0, osc1_level=0.75, osc2_mode="Two Op FM", osc2_wave=FM_1, osc2_timbre=0.0, osc2_shape=FM_CLEAN, osc2_tune=7,
          osc2_level=0.0, cutoff_hz=9000, resonance=0.1, filter_env=0.0, attack=1,
          decay=420, sustain=0.0, release=200, osc_phase=0, stereo=0.3)
p.set(cycle_mode="Env", cycle_rise=1, cycle_fall=80, cycle_hold=0.0)
p.mod("Ptc2", cyc=0.1)
p.mod("Vol2", cyc=0.55)
room(p, mix=0.2)

p = patch("FX", "Metallic Hat", osc1_mode="Noise", osc1_wave=1.0, osc1_timbre=0.8,
          osc1_shape=0.0, osc1_level=1.0, osc2_mode="Noise", osc2_wave=0.5,
          osc2_timbre=1.0, osc2_shape=0.0, osc2_level=0.4, cutoff_hz=1500,
          resonance=0.1, filter_mode="HP", filter_env=0.0, attack=1, decay=200,
          sustain=0.0, release=80, stereo=0.4)

p = patch("FX", "Sci-Fi Drone", osc1_mode="Harm", osc1_wave=0.5, osc1_timbre=0.3,
          osc1_shape=0.2, osc1_level=0.65, osc2_mode="Comb Filter", osc2_wave=0.4,
          osc2_timbre=0.6, osc2_shape=0.3, osc2_level=0.5, cutoff_hz=3000,
          resonance=0.2, filter_env=0.0, attack=2000, decay=3000, sustain=1.0,
          release=4000, stereo=0.9, lfo1_shape="Sine", lfo1_rate=0.05,
          lfo2_shape="Slew S&H", lfo2_rate=0.3)
p.mod("Wav1", lfo1=0.3)
p.mod("Wav2", lfo2=0.15)
p.fx(1, "Flanger", "Default", mix=0.3, time=0.1, intensity=0.5)
hall(p, mix=0.42, time=0.65, preset="Long")

p = patch("FX", "Destroyer", osc1_mode="BasicWaves", osc1_wave=0.5, osc1_level=0.65,
          osc2_mode="Destroy", osc2_wave=0.5, osc2_timbre=0.5, osc2_shape=0.6,
          osc2_level=0.8, cutoff_hz=5000, resonance=0.2, filter_env=0.2, attack=1,
          decay=1000, sustain=0.6, release=200, voice_mode="Mono",
          lfo1_shape="S&H", lfo1_sync=1, lfo1_div="1/16")
p.mod("Tim2", lfo1=0.25)

p = patch("FX", "Radio Tuning", osc1_mode="Formant", osc1_wave=0.6, osc1_timbre=0.5,
          osc1_shape=0.5, osc1_level=0.6, osc2_mode="Noise", osc2_wave=0.5,
          osc2_timbre=0.6, osc2_shape=0.0, osc2_level=0.2, cutoff_hz=2500,
          resonance=0.5, filter_mode="BP", filter_env=0.0, attack=200, decay=1000,
          sustain=1.0, release=800, lfo1_shape="Slew S&H", lfo1_rate=2.5)
p.mod("Tim1", lfo1=0.3)
p.mod("Pitch", lfo1=0.03)
p.fx(1, "Crusher", None, mix=0.3, time=0.3, intensity=0.2)
room(p)

# Underwater: a muffled, slowly wobbling tone with resonant "bubbles" from a
# fast random filter, phased and sunk in a dark room.
p = patch("FX", "Underwater", osc1_mode="Harmo", osc1_wave=0.1, osc1_timbre=0.3,
          osc1_shape=0.5, osc1_level=0.75, osc2_mode="Noise", osc2_wave=0.5,
          osc2_timbre=1.0, osc2_shape=0.0, osc2_level=0.12, cutoff_hz=380,
          resonance=0.55, filter_env=0.0, attack=400, decay=2000, sustain=0.9,
          release=2500, stereo=0.8, lfo1_shape="S&H", lfo1_rate=7.0,
          lfo2_shape="Sine", lfo2_rate=0.4)
p.mod("Cut", lfo1=0.1)
p.mod("Pitch", lfo2=0.004)
p.fx(1, "Phaser", "Space", mix=0.35, time=0.1, intensity=0.5)
p.fx(2, "Chorus", "Dark", mix=0.3, time=0.15, intensity=0.5)
p.fx(3, "Reverb", "Dark Room", mix=0.35, time=0.45, intensity=0.6)

p = patch("FX", "Glitch Burst", osc1_mode="Waveshaper", osc1_wave=0.5, osc1_timbre=0.6,
          osc1_shape=0.5, osc1_level=0.65, osc2_mode="Crusher", osc2_wave=0.6,
          osc2_timbre=0.6, osc2_shape=0.5, osc2_level=0.6, cutoff_hz=6000,
          resonance=0.2, filter_env=0.0, attack=1, decay=1000, sustain=0.5, release=300,
          lfo1_shape="S&H", lfo1_sync=1, lfo1_div="1/32", lfo2_shape="S&H",
          lfo2_sync=1, lfo2_div="1/16T")
p.mod("Wav1", lfo1=0.4)
p.mod("Pitch", lfo2=0.08)
p.fx(1, "Delay", "Digital Sync", mix=0.25, time="1/16", intensity=0.5)

p = patch("FX", "Singing Bowl", osc1_mode="Modal", osc1_wave=0.55, osc1_timbre=0.45,
          osc1_shape=0.95, osc1_level=0.8, osc2_mode="Two Op FM", osc2_wave=FM_1,
          osc2_timbre=0.0, osc2_shape=FM_CLEAN, osc2_tune=0.15, osc2_level=0.15,
          cutoff_hz=8000, resonance=0.0, filter_env=0.0, attack=1, decay=6000,
          sustain=0.0, release=5000, stereo=0.7, lfo2_shape="Sine", lfo2_rate=3.0)
p.mod("Vol1", lfo2=0.1)
hall(p, mix=0.42, time=0.7, preset="Long")


# Macros ---------------------------------------------------------------------
# Every preset gets the same two macros, so they play the same way across the
# bank:
#
#   Macro 1  Timbre  opens the filter (where it has room) and moves each engine's
#                    signature control (supersaw detune, FM index, fold,
#                    wavetable position, sync...) and the drive with it
#   Macro 2  Space   delay and reverb amount, feedback and length, and the
#                    release; a patch without either effect gets a silent
#                    room for it (or, with no slot free, pushes its other
#                    effects)
#
# Presets built on an LFO (wobbles, gates, pumps) add Macro 3, Motion: the
# knob is the LFO's depth (see Patch.motion).
#
# Both start at 0, where the preset sounds as designed, and each socket moves
# its control toward the end that has room, so turning a macro up always
# changes the sound audibly.

# The control that makes each engine sound most like itself.
OSC1_CHARACTER = {
    "BasicWaves": ("Tim1", 0.35), "Two Op FM": ("Tim1", 0.35), "Wavefolder": ("Tim1", 0.4),
    "Noise": ("Tim1", 0.35), "Wavetable": ("Wav1", 0.45),
    "SuperWave": ("Tim1", 0.4), "Harmo": ("Tim1", 0.4), "Formant": ("Tim1", 0.35),
    "V.Analog": ("Tim1", 0.25), "Waveshaper": ("Tim1", 0.4), "Chords": ("Shp1", 0.35),
    "KarplusStr": ("Tim1", 0.35), "SawX": ("Wav1", 0.4), "Harm": ("Tim1", 0.4),
    "Bass": ("Tim1", 0.4), "Modal": ("Wav1", 0.3), "Speech": ("Tim1", 0.35),
}
OSC2_CHARACTER = {
    "BasicWaves": ("Tim2", 0.3), "Ring": ("Tim2", 0.35),
    "Wavefolder": ("Tim2", 0.35), "Wavetable": ("Wav2", 0.4),
    "SuperWave": ("Tim2", 0.35), "Harmo": ("Tim2", 0.35), "Chord": ("Shp2", 0.3),
    "MultiFilter": ("Wav2", 0.3), "Crusher": ("Wav2", 0.3), "Comb Filter": ("Tim2", 0.3),
    "Surgeon Filter": ("Wav2", 0.3), "Phaser Filter": ("Wav2", 0.3), "Destroy": ("Tim2", 0.35),
    "V.Analog": ("Tim2", 0.25), "Waveshaper": ("Tim2", 0.35), "KarplusStr": ("Tim2", 0.3),
    "SawX": ("Wav2", 0.35), "Harm": ("Tim2", 0.35), "Bass": ("Tim2", 0.35),
    "Modal": ("Wav2", 0.3), "Formant": ("Tim2", 0.3), "Two Op FM": ("Tim2", 0.35),
    "FM / RM": ("Tim2", 0.3),
}
# Where each destination's control sits now, as a 0..1 knob position.
DEST_KEY = {"Tim1": "osc1_timbre", "Tim2": "osc2_timbre", "Wav1": "osc1_wave",
            "Wav2": "osc2_wave", "Shp1": "osc1_shape", "Shp2": "osc2_shape"}
DEST_DEFAULT = {"osc1_timbre": 0.35, "osc2_timbre": 0.35, "osc1_wave": 0.5,
                "osc2_wave": 0.5, "osc1_shape": 0.5, "osc2_shape": 0.5}


def toward_room(current, want):
    """+want if the knob has room above it, else -want."""
    return round(want if current + want * 0.5 <= 1.0 else -want, 3)


def filter_slots(p):
    """Open the filter, as far as it has room to (a bright patch skips it, so
    the macro never darkens what the character controls brighten)."""
    s = p.state
    room = 1.0 - s.get("cutoff", 0.898)
    slots = [("Cut", round(min(0.3, room), 3))] if room >= 0.1 else []
    if s.get("resonance", 0.12) < 0.6:
        slots.append(("Res", 0.15))
    return slots


def character_macro(p):
    s = p.state
    slots = []
    for names, mode_key, table in ((OSC1, "osc1_mode", OSC1_CHARACTER),
                                   (OSC2, "osc2_mode", OSC2_CHARACTER)):
        if mode_key == "osc2_mode" and s.get("osc2_level", 0.6) <= 0.0:
            continue
        mode = {v: k for k, v in names.items()}[s.get(mode_key, 0)]
        if mode not in table:
            continue
        dest, want = table[mode]
        osc = mode_key[:4]
        if mode == "BasicWaves" and abs(s.get(osc + "_wave", 0.5) - 0.5) < 0.1:
            # A plain saw ignores Timbre: morph it toward the double saw.
            dest = "Wav" + osc[3]
        key = DEST_KEY[dest]
        slots.append((dest, toward_room(s.get(key, DEST_DEFAULT[key]), want)))
    if s.get("osc1_mode") == OSC1["SuperWave"]:
        slots.append(("Shp1", toward_room(s.get("osc1_shape", 0.5), 0.25)))
    for slot in (1, 2, 3):
        if s.get("fx%d_type" % slot) == FX["Drive"]:
            slots.append(("F%dT" % slot, 0.25))
            break
    return slots


def timbre_macro(p):
    """Cutoff first, then the engines' character and the drive, then
    resonance, in the four sockets a macro has."""
    filt = filter_slots(p)
    return (filt[:1] + character_macro(p) + filt[1:])[:4]


def space_macro(p):
    s = p.state
    slots = []
    release = ("Rel", -0.2 if s.get("release", 280) > 2500 else 0.25)
    kinds = {slot: s.get("fx%d_type" % slot) for slot in (1, 2, 3)}
    for slot, kind in kinds.items():
        if kind == FX["Reverb"]:
            slots += [("F%dA" % slot, 0.4), ("F%dT" % slot, 0.3)]
            break
    for slot, kind in kinds.items():
        if kind == FX["Delay"]:
            slots += [("F%dA" % slot, 0.35), ("F%dI" % slot, 0.3)]
            break
    for slot, kind in kinds.items():
        if kind in (FX["Chorus"], FX["Phaser"], FX["Flanger"], FX["Super Unison"]):
            slots.append(("F%dA" % slot, 0.3))
    if not slots:
        free = [slot for slot in (1, 2, 3) if kinds[slot] is None]
        if free:
            p.fx(free[-1], "Reverb", "Room", mix=0.0, time=0.3, intensity=0.45)
            slots = [("F%dA" % free[-1], 0.6), ("F%dT" % free[-1], 0.3)]
        else:
            # No slot left for a room: push the effects the patch does have.
            slots = [("F%dA" % slot, 0.3) for slot, kind in kinds.items()
                     if kind not in (FX["EQ"], FX["Off"], None)]
    return (slots[:3] + [release])[:4]


def auto_macros(p):
    makers = {1: timbre_macro, 2: space_macro}
    for index, maker in makers.items():
        if "macro%d_dest1" % index in p.state:
            continue
        slots = [(dest, amount) for dest, amount in maker(p) if amount != 0.0]
        if slots:
            p.macro(index, 0.0, *slots)


# Loudness trims in dB, measured with tools/measure_factory_levels.c so the
# bank plays at an even level: sustained sounds by their RMS, percussive ones
# (Sustain at or below 0.3) by their peak. A trim moves Output from its 0.7
# default first (or, for a cut on a driven patch, an EQ after the Drive); what Output cannot reach (it stops at 1) goes into the
# generator oscillators' levels. Processor modes on Osc 2 blend dry and wet
# with their Volume, so that knob is never used as a gain.
LEVELS = {
    'BA 808 Slide': 3.9,
    'BA Acid Squelch': 8.2,
    'BA Reese Engine': -5.0,
    'BA Neuro Growl': 1.9,
    'BA House Sub': 24.0,
    'BA Organ Bass': 24.0,
    'BA FM Slap': 5.1,
    'BA Rolling Techno': 20.9,
    'BA Wobble 140': -7.6,
    'BA Garage Warp': 24.0,
    'BA Funk Ladder': 4.7,
    'BA Synthwave Drive': 24.0,
    'BA Phonk 808': -2.1,
    'BA Minimal Blip': 6.5,
    'BA Clean Sub': -1.5,
    'BA Talking Mid': -8.6,
    'LD Supersaw Anthem': 5.9,
    'LD Future Pluck Lead': 24.0,
    'LD Glide Saw': 8.8,
    'LD Synthwave Lead': 7.2,
    'LD Mono Classic': -2.4,
    'LD Sync Sweep': -0.2,
    'LD Chip Square': 5.4,
    'LD Hyper Saw': -1.8,
    'LD Whistle Sine': -0.1,
    'LD Wavetable Scream': -8.5,
    'LD Vowel Lead': 3.9,
    'LD Breath Flute': 7.2,
    'LD FM Brass Lead': 1.6,
    'LD PWM Lead': 6.6,
    'LD Rave Hoover Lead': 6.4,
    'LD Crystal Lead': 3.0,
    'KY Tine EP': 5.8,
    'KY Ballad EP': 4.5,
    'KY Lofi Keys': 0.1,
    'KY Reed EP': 9.0,
    'KY Clav Funk': 24.0,
    'KY Drawbar Organ': -2.5,
    'KY Gospel Organ': -1.0,
    'KY Rave Piano': 3.7,
    'KY Vibes': 2.8,
    'KY Marimba': 0.9,
    'KY Music Box': 1.9,
    'KY Felt Keys': 6.5,
    'KY Harpsichord': 0.7,
    'KY Poly Keys': 24.0,
    'KY Glass Keys': 1.8,
    'KY Musette': 3.6,
    'PY Init': 12.8,
    'PY Analog Brass': 10.4,
    'PY Super Stab': 11.0,
    'PY Silk Poly': 8.8,
    'PY Dub Chord': 3.0,
    'PY Pumping Saws': 11.0,
    'PY Sync Brass': 5.4,
    'PY String Machine': 5.0,
    'PY Wavetable Poly': -2.5,
    'PY Eighties Poly': 24.0,
    'PY Power Fifths': 5.3,
    'PY Glass Choir': 4.2,
    'PY Chord Memory': 24.0,
    'PY Trance Supersaw': 6.4,
    'PY Bell Poly': 9.3,
    'PY Soft Horns': 5.1,
    'PD Warm Blanket': 4.5,
    'PD Glass Cathedral': -5.6,
    'PD Slow Bloom': 4.3,
    'PD Breathing Choir': 4.3,
    'PD Dark Matter': -4.8,
    'PD Shimmer Strings': 6.6,
    'PD Tidal Drift': -4.0,
    'PD Lofi Pad': 2.3,
    'PD Airy Strings': 4.8,
    'PD Pulse Width Pad': 3.3,
    'PD Frozen Lake': -8.0,
    'PD Warm Sine Pad': -0.5,
    'PD Spectral Haze': -3.8,
    'PD Phased Sunrise': 6.7,
    'PD Harmonic Field': 3.4,
    'PD Ensemble Wash': 2.1,
    'PL Nylon Pick': 5.5,
    'PL Melodic Techno': 11.8,
    'PL Prog House Pluck': 12.0,
    'PL Bell Pluck': 3.4,
    'PL Kalimba': 0.4,
    'PL Harp Glint': 2.9,
    'PL Synth Koto': 7.3,
    'PL Square Blip': 12.8,
    'PL Soft Mallet': -0.8,
    'PL Resonant Drip': 10.1,
    'PL Wavetable Pluck': -1.7,
    'PL Muted Guitar': 8.2,
    'PL Plucked Glass': 1.9,
    'PL Deep House Pluck': 18.5,
    'PL Glock': 2.7,
    'PL Chord Pluck': 3.8,
    'AR Up Sparkle': 11.2,
    'AR Down Pulse': 10.9,
    'AR Random Bells': 1.0,
    'AR Berlin School': 12.7,
    'AR Acid Line': 10.0,
    'AR Chord Stabber': 5.8,
    'AR Pluck Cascade': 1.5,
    'AR Glass Steps': -3.0,
    'AR Minor Pulse Seq': 5.7,
    'AR Octave Runner': 12.3,
    'AR Played Order': 4.1,
    'AR Trance Gate': 8.8,
    'AR Bubbly FM': 4.6,
    'AR Offbeat Bassline': 20.7,
    'AR Melodic Techno Arp': 3.1,
    'AR Future Chord Arp': 5.6,
    'FX Noise Riser': 11.9,
    'FX Downlifter': 0.4,
    'FX Sub Drop Impact': 0.9,
    'FX Laser Zap': 24.0,
    'FX Robot Voice': 5.8,
    'FX Metal Hit': 3.3,
    'FX Wind Tunnel': 13.7,
    'FX Analog Kick': 1.7,
    'FX Noise Snare': 1.8,
    'FX Metallic Hat': 6.6,
    'FX Sci-Fi Drone': 0.5,
    'FX Destroyer': 1.1,
    'FX Radio Tuning': 11.7,
    'FX Underwater': 5.9,
    'FX Glitch Burst': -6.4,
    'FX Singing Bowl': 2.1,
    'PY Vowel Choir': 2.5,
    'PL Sitar': 8.8,
}

PROCESSORS = {OSC2[name] for name in ("Wavefolder", "MultiFilter", "Crusher", "Comb Filter",
                                      "Surgeon Filter", "Phaser Filter", "Destroy", "Ring")}


def apply_level(p, trim_db):
    """Apply a loudness trim in dB without changing how the patch sounds:
    cuts on a driven patch go after the Drive, boosts go first into Output,
    then into the generator oscillators' levels, then up to +6 dB from a flat
    3-Band EQ in a free effect slot."""
    drives = [slot for slot in (1, 2, 3) if p.state.get("fx%d_type" % slot) == FX["Drive"]]
    free = [slot for slot in (1, 2, 3) if "fx%d_type" % slot not in p.state]

    def eq(db):
        slot = "fx%d_" % free[-1]
        p.state.update({slot + "type": FX["EQ"], slot + "mix": 1.0,
                        slot + "low": round(db, 2), slot + "mid": round(db, 2),
                        slot + "high": round(db, 2)})

    if trim_db < 0.0 and drives and free and free[-1] > drives[0]:
        cut = max(trim_db, -15.0)
        eq(cut)
        trim_db -= cut
    gain = 10.0 ** (trim_db / 20.0)
    output = 0.7 * gain
    rest = 1.0
    if output > 1.0:
        rest, output = output, 1.0
    p.state["output"] = round(max(output, 0.05), 3)
    if rest <= 1.0:
        return
    levels = ["osc1_level"]
    if p.state.get("osc2_mode", 0) not in PROCESSORS and p.state.get("osc2_level", 0.6) > 0.0:
        levels.append("osc2_level")
    headroom = min(1.0 / max(p.state.get(key, 0.8), 1e-3) for key in levels)
    boost = min(rest, max(headroom, 1.0))
    for key in levels:
        p.state[key] = round(min(1.0, p.state.get(key, 0.8) * boost), 3)
    rest /= boost
    if rest > 1.0 and free:
        eq(min(6.0, 20.0 * math.log10(rest)))


def render(bank):
    lines = [
        "/* Generated by scripts/generate_factory_bank.py; do not edit. */",
        "#ifndef BIZAR_FACTORY_BANK_H",
        "#define BIZAR_FACTORY_BANK_H",
        "",
        "#define SF_ORIGINAL_PRESET_COUNT %d" % len(bank),
        "",
        "static const char *const g_factory_names[SF_ORIGINAL_PRESET_COUNT] = {",
    ]
    for p in bank:
        lines.append('    "%s",' % p.name)
    lines.append("};")
    lines.append("")
    lines.append("static const char *const g_factory_states[SF_ORIGINAL_PRESET_COUNT] = {")
    for p in bank:
        body = ",".join('"%s":%s' % (k, '"%s"' % v if isinstance(v, str) else repr(v))
                        for k, v in p.state.items())
        lines.append('    "{%s}",' % body.replace("\\", "\\\\").replace('"', '\\"'))
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    return "\n".join(lines)


CATEGORIES = [("BA", "Bass"), ("LD", "Leads"), ("KY", "Keys"), ("PY", "Polysynths and brass"),
              ("PD", "Pads"), ("PL", "Plucks and mallets"), ("AR", "Arpeggios and sequences"),
              ("FX", "Effects, drones and percussion")]
OSC1_NAMES = {v: k for k, v in OSC1.items()}
OSC2_NAMES = {v: k for k, v in OSC2.items()}


def describe(p):
    s = p.state
    engines = OSC1_NAMES[s.get("osc1_mode", 0)]
    if s.get("osc2_level", 0.6) > 0.0:
        engines += " + " + OSC2_NAMES[s.get("osc2_mode", 0)]
    notes = []
    if s.get("seq_mode") == 1:
        notes.append("arpeggiator")
    elif s.get("seq_mode") == 2:
        notes.append("%d-step sequence" % s["seq_length"])
    if s.get("voice_mode") == VOICE["Mono"]:
        notes.append("mono")
    elif s.get("voice_mode") == VOICE["Unison"]:
        notes.append("unison")
    if s.get("chord"):
        notes.append("chord")
    if s.get("macro3_dest1") == DEST["L1Am"]:
        notes.append("Macro 3 is Motion")
    return engines, ", ".join(notes)


def markdown(bank):
    lines = ["# Factory presets", "",
             "Bizar ships 128 original presets, sixteen in each of eight categories. "
             "The two-letter tag at the start of each name is its category, so the "
             "list stays easy to browse on Move's display. Arpeggios and sequences "
             "follow Move's tempo and start when you hold a key.", "",
             "Every preset has two macros: **1 Timbre** sweeps the filter and moves "
             "the engines' signature controls and the drive with it, and **2 Space** "
             "raises the delay, reverb and release. Presets built on an LFO add "
             "**Macro 3, Motion**: the knob is the LFO's depth.", "",
             "This file is generated by `scripts/generate_factory_bank.py --markdown "
             "docs/PRESETS.md`; edit the script, not this list.", ""]
    for tag, title in CATEGORIES:
        lines += ["## %s (%s)" % (title, tag), "", "| # | Name | Engines | Notes |",
                  "|---|---|---|---|"]
        for index, p in enumerate(bank):
            if p.name.startswith(tag + " "):
                engines, notes = describe(p)
                lines.append("| %d | %s | %s | %s |" % (index, p.name[3:], engines, notes))
        lines.append("")
    return "\n".join(lines)


def check(bank):
    names = [p.name for p in bank]
    if len(bank) != 128:
        sys.exit("factory bank: %d presets, expected 128" % len(bank))
    if len(set(names)) != len(names):
        sys.exit("factory bank: duplicate names")
    for p in bank:
        if len(p.name) > 24:
            sys.exit("factory bank: name too long: %s" % p.name)
    counts = {}
    for p in bank:
        counts[p.name[:2]] = counts.get(p.name[:2], 0) + 1
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output", nargs="?")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--markdown")
    args = ap.parse_args()
    for p in BANK:
        if p.name in LEVELS:
            apply_level(p, LEVELS[p.name])
        auto_macros(p)
    counts = check(BANK)
    if args.list:
        for i, p in enumerate(BANK):
            print("%3d %s" % (i, p.name))
        print(counts)
    if args.markdown:
        Path(args.markdown).write_text(markdown(BANK), encoding="utf-8", newline="\n")
        print("wrote %s" % args.markdown)
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(render(BANK), encoding="utf-8", newline="\n")
        print("wrote %s (%d presets)" % (out, len(BANK)))


if __name__ == "__main__":
    main()
