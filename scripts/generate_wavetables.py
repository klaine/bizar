#!/usr/bin/env python3
"""Build the compact, mipmapped default bank from WaveEdit Online CC0 tables."""

import math
from pathlib import Path
import struct
import sys
import wave


BANKS = (
    ("Harmonic", "HARMONIC.WAV"),
    ("Microwave 2", "MICROW02.WAV"),
    ("Bell 03", "BELL03.WAV"),
    ("VoxSynth", "VOXSYNTH.WAV"),
    ("Spectral", "SPECTRAL.WAV"),
    ("SuperSaw", "SUPERSAW.WAV"),
    ("Fourier 2", "FOURIER2.WAV"),
    ("Noise Wave", "NOISE_WA.WAV"),
    ("Folding", "FOLDING_.WAV"),
    ("Drone", "DRONE.WAV"),
    ("Sitar", "SITAR.WAV"),
    ("Tidal", "TIDAL.WAV"),
    ("Harmonix", "HARMONIX.WAV"),
    ("Morphing", "MORPHING.WAV"),
    ("Wavetrip", "WAVETRIP.WAV"),
    ("Sine to Saw", "SINE2SAW.WAV"),
    ("Saw Bend", "SAW_BEND.WAV"),
    ("Basic Morph", "BASIC_WA.WAV"),
    ("Tidy 006", "TIDYB006.WAV"),
    ("AM Sine", "AM_SINE.WAV"),
    ("Hmmm", "HMMMMMMM.WAV"),
    ("Iterative", "ITERATIV.WAV"),
    ("Osmaos", "OSMAOS.WAV"),
    ("Twist 02", "TWIST02.WAV"),
    ("Sine Mutate", "SINE_MUT.WAV"),
    ("Reso 00", "RESO_P00.WAV"),
    ("Sohler 34", "SOHLER34.WAV"),
    ("Sohler 35", "SOHLER35.WAV"),
    ("PD 101", "PD101.WAV"),
    ("Sohler 43", "SOHLER43.WAV"),
    ("Mellow D", "MELLOW_D.WAV"),
    ("Tidy 007", "TIDYB007.WAV"),
)
TABLE_SIZE = 256
FRAMES = 64
MAX_HARMONICS = 127
LEVELS = (127, 64, 32, 16, 8, 4, 2, 1)
ASSET_DIR = Path(__file__).resolve().parents[1] / "assets" / "wavetables" / "wavedit-online"


def read_bank(path: Path) -> list[list[float]]:
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getnframes()) != (1, 2, FRAMES * TABLE_SIZE):
            raise ValueError(f"{path.name}: expected mono 16-bit PCM with {FRAMES * TABLE_SIZE} frames")
        raw = source.readframes(source.getnframes())
    samples = [value[0] / 32768.0 for value in struct.iter_unpack("<h", raw)]
    return [samples[start:start + TABLE_SIZE] for start in range(0, len(samples), TABLE_SIZE)]


def fourier_mips(cycle: list[float], cosine: list[list[float]],
                 sine: list[list[float]]) -> dict[int, list[int]]:
    mean = sum(cycle) / TABLE_SIZE
    centered = [value - mean for value in cycle]
    scale = 2.0 / TABLE_SIZE
    coefficients = []
    for harmonic in range(MAX_HARMONICS):
        coefficients.append((
            scale * sum(sample * basis for sample, basis in zip(centered, cosine[harmonic])),
            scale * sum(sample * basis for sample, basis in zip(centered, sine[harmonic])),
        ))

    wanted = set(LEVELS)
    result: dict[int, list[int]] = {}
    accum = [0.0] * TABLE_SIZE
    for harmonic, (cosine_gain, sine_gain) in enumerate(coefficients, start=1):
        cos_basis = cosine[harmonic - 1]
        sin_basis = sine[harmonic - 1]
        for sample_index in range(TABLE_SIZE):
            accum[sample_index] += (cosine_gain * cos_basis[sample_index]
                                    + sine_gain * sin_basis[sample_index])
        if harmonic in wanted:
            peak = max((abs(value) for value in accum), default=0.0)
            if peak < 1e-12:
                result[harmonic] = [0] * TABLE_SIZE
            else:
                gain = 0.95 * 32767.0 / peak
                result[harmonic] = [
                    max(-32768, min(32767, int(round(value * gain))))
                    for value in accum
                ]
    return result


def generate() -> str:
    phase = [2.0 * math.pi * sample / TABLE_SIZE for sample in range(TABLE_SIZE)]
    cosine = [[math.cos(harmonic * angle) for angle in phase]
              for harmonic in range(1, MAX_HARMONICS + 1)]
    sine = [[math.sin(harmonic * angle) for angle in phase]
            for harmonic in range(1, MAX_HARMONICS + 1)]

    rows = [
        "/* Generated from WaveEdit Online CC0 wavetable banks; do not edit. */",
        "#ifndef BIZAR_WAVETABLE_BANK_H",
        "#define BIZAR_WAVETABLE_BANK_H",
        f"#define SF_WT_FAMILIES {len(BANKS)}",
        f"#define SF_WT_FRAMES {FRAMES}",
        f"#define SF_WT_LEVELS {len(LEVELS)}",
        f"#define SF_WT_SIZE {TABLE_SIZE}",
        "static const unsigned sf_wt_max_harmonics[SF_WT_LEVELS] = {"
        + ", ".join(f"{value}u" for value in LEVELS) + "};",
        "static const int16_t sf_wavetable_bank[SF_WT_FAMILIES][SF_WT_FRAMES]"
        "[SF_WT_LEVELS][SF_WT_SIZE] = {",
    ]

    for _, filename in BANKS:
        wavetable_frames = read_bank(ASSET_DIR / filename)
        rows.append("  {")
        for frame in wavetable_frames:
            mips = fourier_mips(frame, cosine, sine)
            rows.append("    {")
            for max_harmonic in LEVELS:
                rows.append("      {")
                values = mips[max_harmonic]
                for start in range(0, TABLE_SIZE, 16):
                    rows.append("        " + ", ".join(str(value) for value in values[start:start + 16]) + ",")
                rows.append("      },")
            rows.append("    },")
        rows.append("  },")
    rows.extend(["};", "#endif", ""])
    return "\n".join(rows)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate_wavetables.py OUTPUT_HEADER")
    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(generate(), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
