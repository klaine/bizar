# Bizar parameter reference

Version 0.8.2. Generated from the module's own `ui_hierarchy` and `chain_params` by `scripts/generate_parameter_reference.py`; do not edit by hand.

The Move grid shows up to eight knobs per page. A row marked `shown when` is hidden until the named control reaches that value, so an effect slot shows its EQ controls only while it is running that effect.

## Pages

### Main

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `macro1` | Macro 1 | 0 ... 1 | 0 |  |
| 2 | `macro2` | Macro 2 | 0 ... 1 | 0 |  |
| 3 | `macro3` | Macro 3 | 0 ... 1 | 0 |  |
| 4 | `output` | Output | 0 ... 1 | 0.7 |  |
| 5 | `cutoff` | Cutoff | 0 ... 1 step 0.02 | 0.898 |  |
| 6 | `resonance` | Resonance | 0 ... 1 | 0.12 |  |
| 7 | `attack` | Attack | 1 ... 50000 ms step 1 | 10 |  |
| 8 | `release` | Release | 1 ... 50000 ms step 1 | 280 |  |

### Factory Patches

List page: browses `preset` names from `preset_name`, with `preset_count` entries.

### Oscillator 1

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `osc1_mode` | Type 1 | Enum: BasicWaves, Two Op FM, Wavefolder, Noise, Wavetable, SuperWave, Harmo, Formant, V.Analog, Waveshaper, Chords, KarplusStr, SawX, Harm, Bass, Modal, Speech, Sample, Particles, Cloud Grains, Hit Grains, Frozen, Skan, Lick, Raster, Audio In | 0 |  |
| 2 | `osc1_wave` | Wave 1 | 0 ... 1 | 0.5 |  |
| 3 | `osc1_timbre` | Timbre 1 | 0 ... 1 | 0.35 |  |
| 4 | `osc1_shape` | Shape 1 | 0 ... 1 | 0.5 |  |
| 5 | `osc1_tune` | Tune 1 | -48.5 ... 48.5 st step 0.01 | 0 |  |
| 6 | `osc1_table` | Wavetable Preset | Enum: Harmonic, Microwave 2, Bell 03, VoxSynth, Spectral, SuperSaw, Fourier 2, Noise Wave, Folding, Drone, Sitar, Tidal, Harmonix, Morphing, Wavetrip, Sine to Saw, Saw Bend, Basic Morph, Tidy 006, AM Sine, Hmmm, Iterative, Osmaos, Twist 02, Sine Mutate, Reso 00, Sohler 34, Sohler 35, PD 101, Sohler 43, Mellow D, Tidy 007 | - | shown when Type 1 = Wavetable |
| 7 | `osc1_quantize` | Mod Quantize 1 | Enum: Continuous, Chromatic, Octaves, Fifths, Minor, Major, Phrygian Dominant, Minor 9th, Major 9th, Minor Pentatonic, Major Pentatonic | - |  |
| 8 | `osc1_level` | Volume 1 | 0 ... 1 | 0.4 |  |

### Oscillator 2

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `osc2_mode` | Type 2 | Enum: BasicWaves, Ring, Wavefolder, Noise, Wavetable, SuperWave, Harmo, Chord, MultiFilter, Crusher, Comb Filter, Surgeon Filter, Phaser Filter, Destroy, V.Analog, Waveshaper, KarplusStr, SawX, Harm, Bass, Modal, Speech, Sample, Particles, Cloud Grains, Hit Grains, Frozen, Skan, Lick, Raster, Audio In, Formant, Two Op FM, FM / RM | 0 |  |
| 2 | `osc2_wave` | Wave 2 | 0 ... 1 | 0.5 |  |
| 3 | `osc2_timbre` | Timbre 2 | 0 ... 1 | 0.35 |  |
| 4 | `osc2_shape` | Shape 2 | 0 ... 1 | 0.5 |  |
| 5 | `osc2_tune` | Tune 2 | -48.5 ... 48.5 st step 0.01 | 0 |  |
| 6 | `osc2_table` | Wavetable Preset | Enum: Harmonic, Microwave 2, Bell 03, VoxSynth, Spectral, SuperSaw, Fourier 2, Noise Wave, Folding, Drone, Sitar, Tidal, Harmonix, Morphing, Wavetrip, Sine to Saw, Saw Bend, Basic Morph, Tidy 006, AM Sine, Hmmm, Iterative, Osmaos, Twist 02, Sine Mutate, Reso 00, Sohler 34, Sohler 35, PD 101, Sohler 43, Mellow D, Tidy 007 | - | shown when Type 2 = Wavetable |
| 7 | `osc2_quantize` | Mod Quantize 2 | Enum: Continuous, Chromatic, Octaves, Fifths, Minor, Major, Phrygian Dominant, Minor 9th, Major 9th, Minor Pentatonic, Major Pentatonic | - |  |
| 8 | `osc2_level` | Volume 2 | 0 ... 1 | 0.3 |  |

### Filter

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `filter_mode` | Filter Type | Enum: LP, BP, HP | 0 |  |
| 2 | `cutoff` | Cutoff | 0 ... 1 step 0.02 | 0.898 |  |
| 3 | `resonance` | Resonance | 0 ... 1 | 0.12 |  |
| 4 | `filter_env` | Env Amount | -1 ... 1 | 0.35 |  |
| 5 | `cutoff_hz` | Cutoff Hz | 20 ... 18000 Hz step 1 | - | read-only |
| 6 | `filter_fm` | Filter FM | -1 ... 1 | 0 |  |
| 7 | `filter_fm_src` | Filter FM Source | Enum: OSC 1, OSC 2, Mix | 1 |  |

### Envelope

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `attack` | Attack | 1 ... 50000 ms step 1 | 10 |  |
| 2 | `decay` | Decay | 1 ... 50000 ms step 1 | 240 |  |
| 3 | `sustain` | Sustain | 0 ... 1 | 0.72 |  |
| 4 | `release` | Release | 1 ... 50000 ms step 1 | 280 |  |
| 5 | `attack_curve` | Attack Curve | Enum: Default, Quick | - |  |
| 6 | `decay_curve` | Decay Curve | Enum: Default, Percussive | - |  |
| 7 | `release_curve` | Release Curve | Enum: Default, Percussive | - |  |
| 8 | `env_retrig` | Env Retrig | Enum: Env Reset, Env Continue | - |  |

### Velocity Targets

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `vel_vca` | Vel to VCA | 0 ... 1 | - |  |
| 2 | `vel_vcf` | Velo > VCF | 0 ... 1 | - |  |
| 3 | `vel_env` | Velo > Env | 0 ... 1 | - |  |
| 4 | `vel_time` | Velo > Time | 0 ... 1 | - |  |
| 5 | `velo_source` | Velo Source | Enum: Velo + AT, Velo, AT | - |  |
| 6 | `kbd_source` | Kbd Source | Enum: Linear, S Curve, Random, Voices | - |  |

### Cycling Envelope

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `cycle_rise` | Cycle Rise | 1 ... 50000 ms step 1 | - |  |
| 2 | `cycle_fall` | Cycle Fall | 1 ... 50000 ms step 1 | - |  |
| 3 | `cycle_hold` | Cycle Hold/Sustain | 0 ... 10000 ms step 1 | - |  |
| 4 | `cycle_amount` | Cycle Amount | 0 ... 1 | - |  |
| 5 | `cycle_mode` | Cycle Mode | Enum: Env, Run, Loop | - |  |
| 6 | `cycle_rise_curve` | Cycle Rise Shape | 0 ... 1 | - |  |
| 7 | `cycle_fall_curve` | Cycle Fall Shape | 0 ... 1 | - |  |

### LFO 1

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `lfo1_shape` | LFO 1 Shape | Enum: Triangle, Saw, Square, Shaper, Sine, S&H, Slew S&H, ExpSaw, ExpRamp | 0 |  |
| 2 | `lfo1_rate` | LFO 1 Rate | 0.01 ... 100 Hz | 4 |  |
| 3 | `lfo1_depth` | Direct LFO 1 Pitch (extra) | 0 ... 12 st | 0 |  |
| 4 | `lfo1_sync` | LFO 1 Sync | Enum: Off, On | - |  |
| 5 | `lfo1_div` | LFO 1 Div | Enum: 8 Bar D, 8 Bar, 4 Bar D, 8 Bar T, 4 Bar, 2 Bar D, 4 Bar T, 2 Bar, 1 Bar D, 2 Bar T, 1 Bar, 1/2D, 1 Bar T, 1/2, 1/4D, 1/2T, 1/4, 1/8D, 1/4T, 1/8, 1/16D, 1/8T, 1/16, 1/32D, 1/16T, 1/32, 1/32T | - |  |
| 6 | `lfo1_retrig` | LFO 1 Retrig | Enum: Free, Poly Kbd, Mono Kbd, Legato Kbd, One | - |  |

### LFO 1 Curve

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `lfo1_point1` | LFO 1 P1 | -1 ... 1 | -1 |  |
| 2 | `lfo1_point2` | P2 | -1 ... 1 | -0.5 |  |
| 3 | `lfo1_point3` | P3 | -1 ... 1 | 0 |  |
| 4 | `lfo1_point4` | P4 | -1 ... 1 | 0.5 |  |
| 5 | `lfo1_point5` | P5 | -1 ... 1 | 1 |  |
| 6 | `lfo1_point6` | P6 | -1 ... 1 | 0.5 |  |
| 7 | `lfo1_point7` | P7 | -1 ... 1 | 0 |  |
| 8 | `lfo1_point8` | P8 | -1 ... 1 | -0.5 |  |

### LFO 2

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `lfo2_shape` | LFO 2 Shape | Enum: Triangle, Saw, Square, Shaper, Sine, S&H, Slew S&H, ExpSaw, ExpRamp | 0 |  |
| 2 | `lfo2_rate` | LFO 2 Rate | 0.01 ... 100 Hz | 0.35 |  |
| 3 | `lfo2_depth` | Direct LFO 2 Cutoff (extra) | -1 ... 1 | 0 |  |
| 4 | `lfo2_sync` | LFO 2 Sync | Enum: Off, On | - |  |
| 5 | `lfo2_div` | LFO 2 Div | Enum: 8 Bar D, 8 Bar, 4 Bar D, 8 Bar T, 4 Bar, 2 Bar D, 4 Bar T, 2 Bar, 1 Bar D, 2 Bar T, 1 Bar, 1/2D, 1 Bar T, 1/2, 1/4D, 1/2T, 1/4, 1/8D, 1/4T, 1/8, 1/16D, 1/8T, 1/16, 1/32D, 1/16T, 1/32, 1/32T | - |  |
| 6 | `lfo2_retrig` | LFO 2 Retrig | Enum: Free, Poly Kbd, Mono Kbd, Legato Kbd, One | - |  |

### LFO 2 Curve

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `lfo2_point1` | LFO 2 P1 | -1 ... 1 | -1 |  |
| 2 | `lfo2_point2` | P2 | -1 ... 1 | -0.5 |  |
| 3 | `lfo2_point3` | P3 | -1 ... 1 | 0 |  |
| 4 | `lfo2_point4` | P4 | -1 ... 1 | 0.5 |  |
| 5 | `lfo2_point5` | P5 | -1 ... 1 | 1 |  |
| 6 | `lfo2_point6` | P6 | -1 ... 1 | 0.5 |  |
| 7 | `lfo2_point7` | P7 | -1 ... 1 | 0 |  |
| 8 | `lfo2_point8` | P8 | -1 ... 1 | -0.5 |  |

### Mod Matrix

Selector page: lists the items in `mod_matrix_items`, selects with `mod_edit_col`, and navigates to `modmatrix_edit`.

### Matrix Editor

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|

### Macros

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `macro1` | Macro 1 | 0 ... 1 | 0 |  |
| 2 | `macro2` | Macro 2 | 0 ... 1 | 0 |  |
| 3 | `macro3` | Macro 3 | 0 ... 1 | 0 |  |
| 4 | `macro4` | Macro 4 | 0 ... 1 | 0 |  |
| 5 | `mod_wheel` | Mod Wheel | 0 ... 1 | - |  |

### Macro 1 Slots

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `macro1_dest1` | Macro 1 Dest 1 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `macro1_amt1` | Macro 1 Amount 1 | -1 ... 1 | - |  |
| 3 | `macro1_dest2` | Macro 1 Dest 2 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 4 | `macro1_amt2` | Macro 1 Amount 2 | -1 ... 1 | - |  |
| 5 | `macro1_dest3` | Macro 1 Dest 3 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 6 | `macro1_amt3` | Macro 1 Amount 3 | -1 ... 1 | - |  |
| 7 | `macro1_dest4` | Macro 1 Dest 4 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 8 | `macro1_amt4` | Macro 1 Amount 4 | -1 ... 1 | - |  |

### Macro 2 Slots

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `macro2_dest1` | Macro 2 Dest 1 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `macro2_amt1` | Macro 2 Amount 1 | -1 ... 1 | - |  |
| 3 | `macro2_dest2` | Macro 2 Dest 2 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 4 | `macro2_amt2` | Macro 2 Amount 2 | -1 ... 1 | - |  |
| 5 | `macro2_dest3` | Macro 2 Dest 3 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 6 | `macro2_amt3` | Macro 2 Amount 3 | -1 ... 1 | - |  |
| 7 | `macro2_dest4` | Macro 2 Dest 4 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 8 | `macro2_amt4` | Macro 2 Amount 4 | -1 ... 1 | - |  |

### Macro 3 Slots

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `macro3_dest1` | Macro 3 Dest 1 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `macro3_amt1` | Macro 3 Amount 1 | -1 ... 1 | - |  |
| 3 | `macro3_dest2` | Macro 3 Dest 2 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 4 | `macro3_amt2` | Macro 3 Amount 2 | -1 ... 1 | - |  |
| 5 | `macro3_dest3` | Macro 3 Dest 3 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 6 | `macro3_amt3` | Macro 3 Amount 3 | -1 ... 1 | - |  |
| 7 | `macro3_dest4` | Macro 3 Dest 4 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 8 | `macro3_amt4` | Macro 3 Amount 4 | -1 ... 1 | - |  |

### Macro 4 Slots

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `macro4_dest1` | Macro 4 Dest 1 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `macro4_amt1` | Macro 4 Amount 1 | -1 ... 1 | - |  |
| 3 | `macro4_dest2` | Macro 4 Dest 2 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 4 | `macro4_amt2` | Macro 4 Amount 2 | -1 ... 1 | - |  |
| 5 | `macro4_dest3` | Macro 4 Dest 3 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 6 | `macro4_amt3` | Macro 4 Amount 3 | -1 ... 1 | - |  |
| 7 | `macro4_dest4` | Macro 4 Dest 4 | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 8 | `macro4_amt4` | Macro 4 Amount 4 | -1 ... 1 | - |  |

### Effect 1

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `fx1_type` | FX 1 Type | Enum: Off, Delay, Reverb, Chorus, Drive, Crusher, Phaser, 3-Band EQ, Flanger, Super Unison, Peak EQ, Multi Comp | - |  |
| 2 | `fx1_subtype` | FX 1 Preset | Enum: Off | - |  |
| 3 | `fx1_mix` | FX 1 Mix | 0 ... 1 | - |  |
| 4 | `fx1_time` | FX 1 Time | 1 ... 1000 ms | - |  |
| 5 | `fx1_feedback` | FX 1 Feedback | 0 ... 0.95 | - |  |
| 6 | `fx1_low` | FX 1 Low | -15 ... 6 dB step 0.1 | - | shown when FX 1 Type = 3-Band EQ |
| 7 | `fx1_mid` | FX 1 Mid | -15 ... 6 dB step 0.1 | - | shown when FX 1 Type = 3-Band EQ |
| 8 | `fx1_high` | FX 1 High | -15 ... 6 dB step 0.1 | - | shown when FX 1 Type = 3-Band EQ |
| 9 | `fx1_frequency` | FX 1 Peak Frequency | 30 ... 15000 Hz step 1 | - | shown when FX 1 Type = Peak EQ |
| 10 | `fx1_gain` | FX 1 Peak Gain | -15 ... 6 dB | - | shown when FX 1 Type = Peak EQ |
| 11 | `fx1_width` | FX 1 Peak Width | 0.1 ... 10 Q | - | shown when FX 1 Type = Peak EQ |

### Effect 2

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `fx2_type` | FX 2 Type | Enum: Off, Delay, Reverb, Chorus, Drive, Crusher, Phaser, 3-Band EQ, Flanger, Super Unison, Peak EQ, Multi Comp | - |  |
| 2 | `fx2_subtype` | FX 2 Preset | Enum: Off | - |  |
| 3 | `fx2_mix` | FX 2 Mix | 0 ... 1 | - |  |
| 4 | `fx2_time` | FX 2 Time | 1 ... 1000 ms | - |  |
| 5 | `fx2_feedback` | FX 2 Feedback | 0 ... 0.95 | - |  |
| 6 | `fx2_low` | FX 2 Low | -15 ... 6 dB step 0.1 | - | shown when FX 2 Type = 3-Band EQ |
| 7 | `fx2_mid` | FX 2 Mid | -15 ... 6 dB step 0.1 | - | shown when FX 2 Type = 3-Band EQ |
| 8 | `fx2_high` | FX 2 High | -15 ... 6 dB step 0.1 | - | shown when FX 2 Type = 3-Band EQ |
| 9 | `fx2_frequency` | FX 2 Peak Frequency | 30 ... 15000 Hz step 1 | - | shown when FX 2 Type = Peak EQ |
| 10 | `fx2_gain` | FX 2 Peak Gain | -15 ... 6 dB | - | shown when FX 2 Type = Peak EQ |
| 11 | `fx2_width` | FX 2 Peak Width | 0.1 ... 10 Q | - | shown when FX 2 Type = Peak EQ |

### Effect 3

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `fx3_type` | FX 3 Type | Enum: Off, Delay, Reverb, Chorus, Drive, Crusher, Phaser, 3-Band EQ, Flanger, Super Unison, Peak EQ, Multi Comp | - |  |
| 2 | `fx3_subtype` | FX 3 Preset | Enum: Off | - |  |
| 3 | `fx3_mix` | FX 3 Mix | 0 ... 1 | - |  |
| 4 | `fx3_time` | FX 3 Time | 1 ... 1000 ms | - |  |
| 5 | `fx3_feedback` | FX 3 Feedback | 0 ... 0.95 | - |  |
| 6 | `fx3_low` | FX 3 Low | -15 ... 6 dB step 0.1 | - | shown when FX 3 Type = 3-Band EQ |
| 7 | `fx3_mid` | FX 3 Mid | -15 ... 6 dB step 0.1 | - | shown when FX 3 Type = 3-Band EQ |
| 8 | `fx3_high` | FX 3 High | -15 ... 6 dB step 0.1 | - | shown when FX 3 Type = 3-Band EQ |
| 9 | `fx3_frequency` | FX 3 Peak Frequency | 30 ... 15000 Hz step 1 | - | shown when FX 3 Type = Peak EQ |
| 10 | `fx3_gain` | FX 3 Peak Gain | -15 ... 6 dB | - | shown when FX 3 Type = Peak EQ |
| 11 | `fx3_width` | FX 3 Peak Width | 0.1 ... 10 Q | - | shown when FX 3 Type = Peak EQ |

### Voice

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `voice_mode` | Voice Mode | Enum: Poly, Mono, Unison | 0 |  |
| 2 | `glide` | Glide | 0 ... 12000 ms | 0 |  |
| 3 | `glide_mode` | Glide Mode | Enum: Time, Rate | - |  |
| 4 | `bend_range` | Bend Range | 0 ... 24 st step 1 | - |  |
| 5 | `unison_voices` | Unison Voices | 2 ... 6 step 1 | - |  |
| 6 | `unison_mode` | Unison Mode | Enum: Mono, Poly | - |  |
| 7 | `spread` | Unison Spread | 0 ... 7200 cent | 24 |  |
| 8 | `stereo` | Stereo Width | 0 ... 1 | 0.35 |  |

### Chord

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `chord` | Chord | Enum: Off, On | - |  |
| 2 | `chord_notes` | Chord Notes | 1 ... 5 step 1 | - |  |
| 3 | `chord_note1` | Chord Note 1 | -48 ... 48 st step 1 | - |  |
| 4 | `chord_note2` | Chord Note 2 | -48 ... 48 st step 1 | - |  |
| 5 | `chord_note3` | Chord Note 3 | -48 ... 48 st step 1 | - |  |
| 6 | `chord_note4` | Chord Note 4 | -48 ... 48 st step 1 | - |  |
| 7 | `chord_note5` | Chord Note 5 | -48 ... 48 st step 1 | - |  |

### Settings

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `osc_phase` | Osc Freerun | Enum: Trig, Free | - |  |
| 2 | `control_rate` | Mod Rate | Enum: Full, 1/2, 1/4, 1/8 | - |  |

## Additional levels

These levels stay in the document for the Remote UI and for state compatibility, but the Move walk does not visit them.

### Mod 1

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol1_dest` | Mod 1 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol1_cyc` | Mod 1 Cyc | -1 ... 1 | - |  |
| 3 | `modcol1_env` | Mod 1 Env | -1 ... 1 | - |  |
| 4 | `modcol1_lfo1` | Mod 1 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol1_lfo2` | Mod 1 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol1_vel` | Mod 1 Vel | -1 ... 1 | - |  |
| 7 | `modcol1_whl` | Mod 1 Whl | -1 ... 1 | - |  |
| 8 | `modcol1_kbd` | Mod 1 Kbd | -1 ... 1 | - |  |

### Mod 2

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol2_dest` | Mod 2 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol2_cyc` | Mod 2 Cyc | -1 ... 1 | - |  |
| 3 | `modcol2_env` | Mod 2 Env | -1 ... 1 | - |  |
| 4 | `modcol2_lfo1` | Mod 2 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol2_lfo2` | Mod 2 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol2_vel` | Mod 2 Vel | -1 ... 1 | - |  |
| 7 | `modcol2_whl` | Mod 2 Whl | -1 ... 1 | - |  |
| 8 | `modcol2_kbd` | Mod 2 Kbd | -1 ... 1 | - |  |

### Mod 3

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol3_dest` | Mod 3 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol3_cyc` | Mod 3 Cyc | -1 ... 1 | - |  |
| 3 | `modcol3_env` | Mod 3 Env | -1 ... 1 | - |  |
| 4 | `modcol3_lfo1` | Mod 3 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol3_lfo2` | Mod 3 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol3_vel` | Mod 3 Vel | -1 ... 1 | - |  |
| 7 | `modcol3_whl` | Mod 3 Whl | -1 ... 1 | - |  |
| 8 | `modcol3_kbd` | Mod 3 Kbd | -1 ... 1 | - |  |

### Mod 4

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol4_dest` | Mod 4 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol4_cyc` | Mod 4 Cyc | -1 ... 1 | - |  |
| 3 | `modcol4_env` | Mod 4 Env | -1 ... 1 | - |  |
| 4 | `modcol4_lfo1` | Mod 4 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol4_lfo2` | Mod 4 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol4_vel` | Mod 4 Vel | -1 ... 1 | - |  |
| 7 | `modcol4_whl` | Mod 4 Whl | -1 ... 1 | - |  |
| 8 | `modcol4_kbd` | Mod 4 Kbd | -1 ... 1 | - |  |

### Mod 5

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol5_dest` | Mod 5 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol5_cyc` | Mod 5 Cyc | -1 ... 1 | - |  |
| 3 | `modcol5_env` | Mod 5 Env | -1 ... 1 | - |  |
| 4 | `modcol5_lfo1` | Mod 5 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol5_lfo2` | Mod 5 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol5_vel` | Mod 5 Vel | -1 ... 1 | - |  |
| 7 | `modcol5_whl` | Mod 5 Whl | -1 ... 1 | - |  |
| 8 | `modcol5_kbd` | Mod 5 Kbd | -1 ... 1 | - |  |

### Mod 6

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol6_dest` | Mod 6 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol6_cyc` | Mod 6 Cyc | -1 ... 1 | - |  |
| 3 | `modcol6_env` | Mod 6 Env | -1 ... 1 | - |  |
| 4 | `modcol6_lfo1` | Mod 6 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol6_lfo2` | Mod 6 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol6_vel` | Mod 6 Vel | -1 ... 1 | - |  |
| 7 | `modcol6_whl` | Mod 6 Whl | -1 ... 1 | - |  |
| 8 | `modcol6_kbd` | Mod 6 Kbd | -1 ... 1 | - |  |

### Mod 7

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol7_dest` | Mod 7 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol7_cyc` | Mod 7 Cyc | -1 ... 1 | - |  |
| 3 | `modcol7_env` | Mod 7 Env | -1 ... 1 | - |  |
| 4 | `modcol7_lfo1` | Mod 7 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol7_lfo2` | Mod 7 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol7_vel` | Mod 7 Vel | -1 ... 1 | - |  |
| 7 | `modcol7_whl` | Mod 7 Whl | -1 ... 1 | - |  |
| 8 | `modcol7_kbd` | Mod 7 Kbd | -1 ... 1 | - |  |

### Mod 8

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol8_dest` | Mod 8 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol8_cyc` | Mod 8 Cyc | -1 ... 1 | - |  |
| 3 | `modcol8_env` | Mod 8 Env | -1 ... 1 | - |  |
| 4 | `modcol8_lfo1` | Mod 8 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol8_lfo2` | Mod 8 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol8_vel` | Mod 8 Vel | -1 ... 1 | - |  |
| 7 | `modcol8_whl` | Mod 8 Whl | -1 ... 1 | - |  |
| 8 | `modcol8_kbd` | Mod 8 Kbd | -1 ... 1 | - |  |

### Mod 9

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol9_dest` | Mod 9 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol9_cyc` | Mod 9 Cyc | -1 ... 1 | - |  |
| 3 | `modcol9_env` | Mod 9 Env | -1 ... 1 | - |  |
| 4 | `modcol9_lfo1` | Mod 9 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol9_lfo2` | Mod 9 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol9_vel` | Mod 9 Vel | -1 ... 1 | - |  |
| 7 | `modcol9_whl` | Mod 9 Whl | -1 ... 1 | - |  |
| 8 | `modcol9_kbd` | Mod 9 Kbd | -1 ... 1 | - |  |

### Mod 10

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol10_dest` | Mod 10 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol10_cyc` | Mod 10 Cyc | -1 ... 1 | - |  |
| 3 | `modcol10_env` | Mod 10 Env | -1 ... 1 | - |  |
| 4 | `modcol10_lfo1` | Mod 10 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol10_lfo2` | Mod 10 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol10_vel` | Mod 10 Vel | -1 ... 1 | - |  |
| 7 | `modcol10_whl` | Mod 10 Whl | -1 ... 1 | - |  |
| 8 | `modcol10_kbd` | Mod 10 Kbd | -1 ... 1 | - |  |

### Mod 11

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol11_dest` | Mod 11 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol11_cyc` | Mod 11 Cyc | -1 ... 1 | - |  |
| 3 | `modcol11_env` | Mod 11 Env | -1 ... 1 | - |  |
| 4 | `modcol11_lfo1` | Mod 11 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol11_lfo2` | Mod 11 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol11_vel` | Mod 11 Vel | -1 ... 1 | - |  |
| 7 | `modcol11_whl` | Mod 11 Whl | -1 ... 1 | - |  |
| 8 | `modcol11_kbd` | Mod 11 Kbd | -1 ... 1 | - |  |

### Mod 12

Gate: shown when mod_edit_col > 999.

| Knob | Key | Name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `modcol12_dest` | Mod 12 Dest | Enum: Off, Pitch, Cut, Shp1, Shp2, Vol2, Res, Out, Tim1, Tim2, Wav1, Wav2, Ptc1, Ptc2, Vol1, VCA, EnvA, Gld, Atk, Dec, Sus, Rel, L1Rt, L2Rt, L1Am, L2Am, CyAm, F1T, F1I, F1A, F2T, F2I, F2A, F3T, F3I, F3A, Mac1, Mac2, Mac3, Mac4, Rise, Fall, Hold, M1:1, M1:2, M1:3, M1:4, M1:5, M1:6, M1:7, M1:8, M1:9, M1:10, M1:11, M1:12, M2:1, M2:2, M2:3, M2:4, M2:5, M2:6, M2:7, M2:8, M2:9, M2:10, M2:11, M2:12, M3:1, M3:2, M3:3, M3:4, M3:5, M3:6, M3:7, M3:8, M3:9, M3:10, M3:11, M3:12, M4:1, M4:2, M4:3, M4:4, M4:5, M4:6, M4:7, M4:8, M4:9, M4:10, M4:11, M4:12, M5:1, M5:2, M5:3, M5:4, M5:5, M5:6, M5:7, M5:8, M5:9, M5:10, M5:11, M5:12, M6:1, M6:2, M6:3, M6:4, M6:5, M6:6, M6:7, M6:8, M6:9, M6:10, M6:11, M6:12, M7:1, M7:2, M7:3, M7:4, M7:5, M7:6, M7:7, M7:8, M7:9, M7:10, M7:11, M7:12, FFM | - |  |
| 2 | `modcol12_cyc` | Mod 12 Cyc | -1 ... 1 | - |  |
| 3 | `modcol12_env` | Mod 12 Env | -1 ... 1 | - |  |
| 4 | `modcol12_lfo1` | Mod 12 LFO1 | -1 ... 1 | - |  |
| 5 | `modcol12_lfo2` | Mod 12 LFO2 | -1 ... 1 | - |  |
| 6 | `modcol12_vel` | Mod 12 Vel | -1 ... 1 | - |  |
| 7 | `modcol12_whl` | Mod 12 Whl | -1 ... 1 | - |  |
| 8 | `modcol12_kbd` | Mod 12 Kbd | -1 ... 1 | - |  |

## Other parameters

Served by the module and used by presets or the Remote UI, but not on a knob page.

| Key | Name | Range | Notes |
|---|---|---|---|
| `cycle_rate` | Cycle Rate (legacy) | 0.01 ... 20 Hz |  |
| `cycle_depth` | Cycle Depth (legacy) | 0 ... 1 |  |
