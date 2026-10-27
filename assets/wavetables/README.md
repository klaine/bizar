# Built-in wavetable sources

The default wavetable banks are selected from the community-contributed
[WaveEdit Online collection](https://github.com/smpldsnds/wavedit-online),
which its README releases under CC0 1.0 Universal. Source snapshot:
`1a8d80f7a062f36eca212f92f24f2ca6329b88ff`.

| Built-in bank | Source file |
|---|---|
| Harmonic | `HARMONIC.WAV` |
| Microwave 2 | `MICROW02.WAV` |
| Bell 03 | `BELL03.WAV` |
| VoxSynth | `VOXSYNTH.WAV` |
| Spectral | `SPECTRAL.WAV` |
| SuperSaw | `SUPERSAW.WAV` |
| Fourier 2 | `FOURIER2.WAV` |
| Noise Wave | `NOISE_WA.WAV` |
| Folding | `FOLDING_.WAV` |
| Drone | `DRONE.WAV` |
| Sitar | `SITAR.WAV` |
| Tidal | `TIDAL.WAV` |
| Harmonix | `HARMONIX.WAV` |
| Morphing | `MORPHING.WAV` |
| Wavetrip | `WAVETRIP.WAV` |
| Sine to Saw | `SINE2SAW.WAV` |
| Saw Bend | `SAW_BEND.WAV` |
| Basic Morph | `BASIC_WA.WAV` |
| Tidy 006 | `TIDYB006.WAV` |
| AM Sine | `AM_SINE.WAV` |
| Hmmm | `HMMMMMMM.WAV` |
| Iterative | `ITERATIV.WAV` |
| Osmaos | `OSMAOS.WAV` |
| Twist 02 | `TWIST02.WAV` |
| Sine Mutate | `SINE_MUT.WAV` |
| Reso 00 | `RESO_P00.WAV` |
| Sohler 34 | `SOHLER34.WAV` |
| Sohler 35 | `SOHLER35.WAV` |
| PD 101 | `PD101.WAV` |
| Sohler 43 | `SOHLER43.WAV` |
| Mellow D | `MELLOW_D.WAV` |
| Tidy 007 | `TIDYB007.WAV` |

The second sixteen banks were chosen by measurement, not by name: harmonic
profiles of a 16-position sweep of each hardware-synth wavetable were compared
against every CC0 bank (`tools/minifreak_wt_match.py`), and banks were added
greedily to maximize average best-match similarity (the last slot was swapped
for a bank without near-silent frames). Banks named after
commercial instruments or manufacturers were excluded, and the reference
wavetable data itself is not included or redistributed.

These mono 16-bit PCM source files each contain 64 single-cycle waves of 256
samples. `scripts/generate_wavetables.py` converts them into normalized,
Fourier-filtered mip levels before compilation. No files are read in an audio
callback. The CC0 dedication is available at
<https://creativecommons.org/publicdomain/zero/1.0/>.
