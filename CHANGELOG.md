# Changelog

## Unreleased: Bizar

- **Additive removed.** The MiniFreak has no such engine: Bizar's Additive was a
  lookup into the harmonic wavetable family, and the MiniFreak's additive sound
  is Harmo. Later Osc 1/Osc 2 type indices shift down one (state version 18);
  older saved patches are converted on load, with Additive patches becoming
  Harmo. Three factory presets (Breath Flute, Musette, Harmonic Field) now use
  Harmo.
- **Cross FM removed.** The MiniFreak's Osc 2 has no such mode (FM / RM is its
  FM processor). Osc 2 type indices after Ring shift down one (state version
  19); older patches using Cross FM load as FM / RM.
- **Remote UI patch list on the device.** Move's Remote UI only passes `preset`,
  `preset_count` and `preset_name` to the page, so the patch selector stayed on
  "Loading..." and never showed the device's patch. The build now embeds the
  factory names in the shipped `web_ui.html` (`scripts/embed_preset_names.py`);
  the browser build still reads them from the module. Version 0.8.2.
- **Osc 2 page on the device.** Knob 8 showed Mod 1's destination ("Cut")
  instead of Volume 2: the hierarchy's printf arguments for the Osc 2 level
  sat after the twelve matrix-column labels, so those labels were shifted too.
  The Wavetable selector on both oscillator pages now shows only when that
  oscillator's type is Wavetable, the one engine that reads it.
- **Filter FM.** A bipolar `Filter FM` knob (-1..1, 0 = off, squared response
  so the low end has the fine control) frequency-modulates the filter cutoff
  at audio rate by the signal chosen with `Filter FM Source` (Osc 1, Osc 2 or
  Mix; default Osc 2). The source is taken before the oscillator level knobs,
  so a muted Osc 2 can still modulate; it is DC-blocked and limited to +/-1,
  and a processor engine in Osc 2 (Multifilter, Comb, Crusher...) modulates
  with its wet signal. Full depth is 6 octaves per unit of source. While it is
  on, the filter runs at twice the sample rate (about +30-70% CPU on the
  voice, nothing when off) to keep the cutoff sidebands from aliasing as
  hard. It is also a mod-matrix destination (`FFM`), appended after the
  matrix cells so saved patches keep their indices; patches saved before it
  load with it off. The matrix and sequencer-lane destination lists in the
  Remote UI still stop before the cell entries, so lanes cannot pick it yet.
- **Matrix cells are destinations.** As on the MiniFreak, a column or a macro
  slot can aim at a cell of the matrix: `M4:3` is row 4 (LFO 2) of column 3, so
  a column's Envelope can set how much LFO 2 reaches Osc 1 Timbre, or Macro 3
  can open an LFO 2 > Cutoff route. 84 new destinations (`M1:1` to `M7:12`,
  row first) are appended after Hold, so saved patches keep their indices.
  The cell's depth is its stored amount plus the offsets aimed at it, clamped
  to -1..1. Cells are resolved from stored amounts, so a cell never moves the
  route that moves it; macro-aimed columns ignore cell offsets for the same
  reason.
- **chain_params has room again.** The device copy leaves out the 96 hidden
  `modcol<N>_*` entries (about 10 KB): from 32.6 KB, which pays for the 84 cell destinations in the macro sockets (31.9 KB worst case). The
  keys still work in get/set and state; `chain_params_full` keeps them, and
  the Remote UI rebuilds their metadata when they are missing.
- **Renamed** from SchwungFreak to Bizar. The module id is now `bizar`, the
  package is `bizar-module.tar.gz`, and the source file is `src/bizar.c`.
  Patches saved under the old module id do not carry over automatically.
- **New factory bank.** 128 original presets in eight categories (Bass, Leads,
  Keys, Polysynths, Pads, Plucks, Arpeggios and Effects) replace the eight
  earlier patches. They are defined in `scripts/generate_factory_bank.py`,
  level-matched with `tools/measure_factory_levels.c`, and listed in
  `docs/PRESETS.md`. Basses, leads and keys were redesigned after modern
  references (808s, Reese and neuro mid-basses, acid, supersaw and glide
  leads, DX-style FM electric pianos), with mono low end on the basses. Pads,
  polysynths, plucks, arpeggios and effects followed, after current pad,
  pluck, arp and transition-FX practice (slow detuned stacks with two LFOs,
  short-attack plucks with an octave layer, 1/16 arps, risers and impacts).
- **Performance macros on every preset**: 1 Timbre (filter plus the engines'
  signature controls and drive) and 2 Space (delay, reverb, release), plus
  3 Motion (the LFO's depth) on wobble, gate and pump patches. They are
  generated from each patch by `scripts/generate_factory_bank.py`.
- **No click on preset change.** Loading a preset used to swap every
  parameter under voices that were still sounding and restart the effects
  over their old buffers, which could step the output hard. The old sound is
  now cut (the sequencer lets go, voices stop, effects stay bypassed until
  clear) and its last sample fades out over 10 ms under the new preset.
- **Osc 1 Wavetable knobs now do what they say.** An earlier parity pass
  remapped them to match MiniFreak V, so "Position" set the waveform width
  (and was pinned at maximum above 0.5) while "Width" moved the position over
  its top quarter only. Both oscillators now read Position / Width / Tone as
  labelled; the MiniFreak importer converts a bank's values instead (static
  knob values exactly, modulation into those knobs approximately).
- **Remote UI previews.** The Chords preview is now the engine's own chord,
  inversion and registration (divide-down below Shape 0.55, soft wavetable
  above); the Comb Filter preview simulates the tuned feedback comb; the Two
  Op FM ratio readout uses the engine's ratio quantizer (1:1 is at 0.24).
- **Factory sound fixes.** Every "sine" layer is now Two Op FM at index 0, a
  true sine (Harmo always carries odd partials). V.Analog presets were
  rewritten for that engine's real knob order (Wave detune, Timbre pulse and
  sync, Shape saw). Organs centre their chords on the key; marimba, kalimba,
  bells and glass were rebuilt from their partials; the choirs use Speech
  vowels; drums, Downlifter, Wind Tunnel and Underwater were redone.
- **Factory bank loudness** now matches the imported MiniFreak bank (median
  peak about -14 dBFS for one held note). The first calibration measured
  fresh instances, whose effects stay bypassed while their delay memory
  clears, so it set the bank about 10 dB too quiet. Both tools now warm the
  instance up first.
- **Two Op FM settings.** Factory FM patches now use the engine's exact ratio
  positions (1:1 is Wave 0.242) and a clean, centred Shape (it is a bipolar
  feedback control around 0.5).
- `tools/render_factory_preset.c` renders presets to WAV files for listening.
- **MiniFreak import is opt-in.** A `.mnfx` export is no longer picked up from
  the repository root. Set `MINIFREAK_BANK` to build with it; such builds are
  written under `*-personal.*` names so they cannot be published by mistake
  (see `docs/MINIFREAK_IMPORT.md`).

## Unreleased: envelope and LFO parity pass

The ADSR, Cycling Envelope and both LFOs were measured against MiniFreak V
(each modulator routed to pitch and tracked, or read off the amp envelope)
and rebuilt to the measured curves, and then every modulation destination's
scale was measured the same way. State is now v14; older states are migrated
so they keep their timing and pitch depths.

- **ADSR.** Attack is an RC curve that overshoots its target (k 1.55, nearly
  straight; Quick k 4.5). Decay and Release are exponentials, and a stage's
  time is now its time to -60 dB. Percussive is a fast ~10-25 ms drop plus a
  slow tail whose balance follows the knob. Ranges reach 50 s, as the
  hardware's do. The importer uses the measured knob tables (the old cubic
  taper made mid-range attacks two to three times too long).
- **Time modulation** (Attack, Decay, Release and LFO Rate routes) now moves
  the knob, as the hardware does: +0.25 on Decay 0.5 gives Decay 0.75, six
  times longer, rather than a fixed 2x per unit.
- **Cycling Envelope.** Env mode is a second ADSR: Rise uses the attack curve,
  Fall the decay curve toward Hold, and note-off releases on the release
  curve; its curve knobs switch to Quick/Percussive above 0.5. Run is now the
  free-running loop and Loop restarts with each note (they were swapped), and
  both keep cycling after note-off. In Run and Loop, Rise/Fall/Hold import as
  10 s * x^4 and the curve bend is 5.5x stronger, matching the hardware.
- **LFOs.** Rate covers 0.06..100 Hz on the measured knob curve (the importer
  had 0.01..30 Hz on a plain exponential, so a mid-knob LFO ran 9x slow).
  Synced LFOs use the MiniFreak's 27 divisions; the most common factory
  setting, 1/4, used to import as 1/8. Triangle now starts at 0 in phase with
  Sine, ExpSaw/ExpRamp are e^-5t (ExpRamp was bent the wrong way), and Slew
  S&H glides at a fixed slope. Retrig adds Mono Kbd, Legato Kbd and One (a
  single cycle per note that then holds its end value).
- **Keyboard source** is (note - 60) / 64, as measured; it was /48, so every
  key-tracked route was a third too strong.
- **Comb Filter (Osc 2)** rebuilt from measurements through MiniFreak V's
  Audio In: a feedback comb tuned to Osc 2's pitch (Wave is a ratio of it,
  x0.092..x7.7, unison at 0.65), Timbre trading input gain for feedback (up
  to +20 dB on the teeth), Shape a 5 kHz..490 Hz loop low-pass, and a
  hard-knee limiter in the loop, so a high-Timbre comb keeps ringing as its
  input fades. It was a single feed-forward echo at a fixed pitch. The voice
  delay lines grew to 4096 samples for the low Wave settings. Flute now sits
  within 1 dB of the hardware from full input down to 3 % of it, and Harp,
  silent before, sounds.
- **VCA routes** follow the measured law: a positive route opens the VCA
  with its source (gain 1 - a + a * source) instead of boosting it up to 2x,
  and a negative one closes it by |a| * source. Karcomb's level before its
  comb went from 8.3 dB hot to within 0.7 dB; 26 of the 65 presets that
  route to the VCA now sit within 3 dB of the hardware's level (21 before).
- **Shaper LFO.** Imported Shapers play the MiniFreak's step model, measured
  from MiniFreak V: every step lasts a whole LFO period (or synced
  division), so the pattern repeats every Length steps; each step draws its
  own segment from 0 to its level; Slope is a four-way menu (rise, fall,
  rise-then-fall, fall) and Curve bends the segment with a dead zone around
  the middle; En is rest / play / tie. One mode plays the pattern once and
  holds its last value. It is a new Shaper mode (`lfoN_shp_mode` 1), so
  Bizar's own level-and-glide Shapers and the Remote UI editor keep
  working; the editor's preview draws both. Test patterns match the hardware
  to 0.01-0.04 of full scale.
- **Sequencer step automation.** Four lanes, each a modulation destination
  plus one offset per step, smooth or stepped, saved as `seq_laneN_*` state
  keys and edited under the pattern in the Remote UI. The importer reads the
  bank's Seq_Autom_* destinations, ModState step masks and Mod_S*_I* step
  values; 14 of the 15 factory presets that automate get their lanes. Old
  states load unchanged (no lanes), so lanes needed no version bump of
  their own.
- **Destination scales.** Every destination was measured on MiniFreak V by
  routing the Mod Wheel into it and matching the result against a sweep of
  the destination's own knob. Wave, Timbre, Shape, both Volumes, Cutoff,
  Resonance, Env Amt, Sustain and the effects' Time, Intensity and Amount
  all add the route straight onto the knob position (1:1) and stop at the
  knob's ends. Changes that follow:
  - **Pitch** routes (Pitch, Pitch 1, Pitch 2) move 60 semitones per unit,
    linear down to the smallest vibrato depths; they were 12. v14 migrates
    older pitch amounts by 1/5 so saved patches keep their depth.
  - **LFO 1/2 Amp and CycEnv Amp** follow the VCA's law (a positive route
    opens the gain with its source, never past unity) instead of boosting it
    up to 2x. Macros follow the same law.
  - **Effect routes** move the MiniFreak knob position: Time no longer scales
    by 4x per unit (and no longer runs backwards), Intensity moves 0.95 of
    feedback per unit instead of 0.5, the free Delay's Time goes through its
    millisecond table, and Delay/Reverb Amount through the Send law.
  - **Cutoff** modulation now stops at the knob's 20 Hz and 18 kHz ends.
  - **Importer addresses.** Probing the destination addresses on MiniFreak V
    showed four wrong entries. Pitch 1/2 are 0x90C4/0x90C5 (they were read
    one slot late, so Pitch 1 routes moved Osc 2). The LFO rates are
    0x9030/0x9037. What was imported as "LFO 2 Rate" (0x9028) is the Cycling
    Envelope's Fall. 102 LFO-rate routes that were dropped now import, and 51
    Fall routes no longer play as LFO rate.
- **Cycling Envelope Rise, Fall and Hold destinations** (`Rise`, `Fall`,
  `Hold` in the destination menus, imported from 0x9027-0x9029). They move
  the knobs per voice: Rise and Fall on the attack and decay curves in Env
  mode and on 10 s * x^4 in Run and Loop; Hold is the sustain level in Env
  mode and a time in Run and Loop. Macro sockets now list the four macros
  too (a socket aimed at one does nothing), so these keep the same index
  everywhere.
- **Velo Source** (`velo_source`, on the Velocity Targets page): what the
  matrix's Velo + AT row reads: Velo + AT (the default and the old
  behaviour), Velo, or AT. MiniFreak's Mx_VeloAt imports onto it. 42
  factory presets use aftertouch only, and in 33 of them a velocity route
  used to act on every note where the hardware leaves it at rest until you
  press harder.
- **Kbd Source** (`kbd_source`, on the Velocity Targets page): what the
  matrix's Keyboard row reads, measured on MiniFreak V and imported from
  Kbd_Src. Linear (the default and the old behaviour) is 1/64 per semitone
  from middle C. S Curve is 1/30 per semitone out to +/-0.8 two octaves
  away, then flattens to +/-1. Random is a fresh -1..1 draw on every note.
  Voices gives the six voices 0, +1, -1, 0, +1/2, -1/2: round-robin in
  Poly, all six across a Unison stack, and 0 in Mono. 53 factory presets use
  a non-Linear source. Full Moon is Random in a six-voice Unison, so every
  voice gets its own Kbd -> Decay time, and the longest one sets each arp
  step's tail. That preset's loudness-contour error falls from 12.5 to
  7.2 dB at C3, and from 21 to 7 dB at C2. Kbd, LFO and the other bipolar
  sources move a time destination's knob by amount x value, with no offset,
  as the code already did.
- **Voice stealing.** Re-striking a chord over long release tails dropped
  notes: with no free voice, the allocator stole the voice with the lowest
  envelope phase, and a voice started a moment earlier in the same chord
  sits at the bottom of its attack, so a chord's later notes stole its first
  ones. Released voices are now taken first (quietest first), then the
  oldest held voice.
- **EQ effects as route targets.** Routes into a 3 Bands EQ's Time,
  Intensity and Amount move its low, mid and high gains, and into a Peak
  EQ's its frequency (on its measured knob table), gain and width, as on
  the hardware. They used to land on the delay-style Time and the mix,
  which the EQs ignore, so Giga Saw's LFO sweep of its Peak EQ was silent
  (its brightness contour error went from 1.15 to 0.71 octaves).
- **Synced Cycling Envelope.** Measured on MiniFreak V: a tempo-synced
  stage lasts round(60 * knob) sixteenth notes and Fall one more, and Rise 0
  is an instant jump. The importer split the shortest loop into a 62.5 ms
  rise and a 72.5 ms fall, so Fractal Arp's pluck had a soft attack; it now
  imports as an instant rise and a 125 ms fall (at 120 BPM).
- **Unison and Sample & Hold.** A Unison stack now shares one S&H seed per
  note, as the MiniFreak's single stacked voice does (measured: unison adds
  no spread to an S&H pitch sweep). Each stacked voice drew its own value,
  so their timbre sweeps averaged out and Full Moon's arp repeats all
  sounded alike.
- **Keyboard Chord, Unison Mode and the full Unison Spread.** Measured on
  MiniFreak V. Spread's step between stacked voices climbs in a straight
  line to a semitone at 0.5 and on to an octave at 1, so the five factory
  patches at 0.773 stack fifths (PS1: +/-7, 14 and 21 semitones). The
  importer had bent the whole range onto the first line, so PS1 played an
  even +/-914-cent smear; `spread` now reaches 7200 cents (six octave
  steps). New `chord`, `chord_notes` and `chord_note1`..`chord_note5` (a
  Chord page, and in the Remote UI's Voice tab) add up to five notes to every
  key in Poly, Mono and Unison, imported from Kbd Chord (PS1: +12, +19). In
  Unison a chord takes all six voices whatever the Count and deals them out
  over its notes, each voice keeping its own offset, as MiniFreak V does.
  New `unison_mode` imports Unison Mode: Mono is the one stack that follows
  the last key, and Poly (and Para) gives every key its own stack of Count
  voices, two held keys sharing the six. Against MiniFreak V renders (FX
  off, quarter-tone band spectra), PS1's correlation went from -0.22 to
  0.77, 90 Ravers' from -0.24 to 0.88, Cathedral's from -0.61 to 0.86 and
  Extatic's from 0.60 to 0.92.
- **Osc 2 FM / RM and Two Op FM.** Two new Osc 2 types measured on
  MiniFreak V. FM / RM is the MiniFreak's processor: Osc 2 is a sine ->
  triangle -> saw -> square oscillator (Wave) added at its Volume, which
  Osc 1's audio, taken before Osc 1's Volume, phase-modulates (Timbre,
  52.5 rad per full-level sine) and ring-modulates through a comparator
  (Shape). Osc 1 passes through dry. Two Op FM is Osc 1's FM engine on
  Osc 2. MiniFreak's Osc 2 types 6 and 15 used to import as the older
  cross-FM mode, which added a full extra tone at Osc 1's pitch. That mode
  keeps its index and is now listed as Cross FM. Across the 45 factory
  presets using either type, the spectral-balance error fell from a median
  11.2 dB to 6.5 dB. Against a sine Osc 1, the sidebands match the hardware
  to about 1-2 dB.
- **Reverb level and pre-delay.** The wet arrives after the MiniFreak's
  per-preset pre-delay (Default and Hall 30 ms, Long 90 ms, Echoes 50 ms,
  Room and Dark Room 10 ms), and each preset's level is refitted. The old
  calibration used a deconvolved impulse response that overstated the
  MiniFreak's wet, so Default played about 5 dB and Hall about 9 dB too wet.
  The wet now rises 1.5 dB per doubling of the decay, as measured. Low and
  mid bands match MiniFreak V within about 2 dB across all six presets. 80s
  Mean's Hall tail is within 1 dB through 4 kHz, where it was 10 dB hot.
- **Modal rebuilt to MiniFreak V.** Measured with Osc 1 alone over a
  5 x 5 x 4 Wave/Timbre/Shape grid and from note 24 to 96, MiniFreak's
  Modal is Plaits' modal voice with 12 modes and a fixed accent of 0.8
  (velocity does not change it). The accent lifts Shape's damping and
  Timbre's brightness. A mode's decay depends on its index, not its pitch.
  The fundamental loses 44 dB/s at Shape 0.3, and the rate halves every
  0.094 of Shape. Our port ran 24 modes with no accent, so at Shape 0.2 its
  fundamental fell about 400 dB/s, where the hardware's falls 89 dB/s.
  It also leaked the Wave stiffness into the tuning (partials sat up to 3 %
  flat at high Wave; mode 3 now stays on the third harmonic) and struck the
  modes before the smoothed Wave had settled, which gave every Wave
  harmonic mode amplitudes. The output trim drops from +19.5 dB to
  +4.1 dB. Now every mode's onset level and decay rate are within
  about 1 dB (and 1 dB/s) of the hardware's, and the level is within 1 dB
  at every note tested, Osc 2 Modal included. 80s Mean went from 11 dB to
  4 dB loud (the engine alone now matches at its settings, so the rest is
  elsewhere in the voice), Conquest's brightness went from 0.39 octave
  bright to 0.03, and House Mode is unchanged.
- **Filter type import.** MiniFreak's Vcf_Type sits on a 1/6 grid (0 LP,
  1/6 BP, 2/6 HP, measured). Read as a three-way menu, the bank's 8 BP
  presets played as LP and its 21 HP presets as BP.
- **Filter self-oscillation.** Resonance now runs to 1.0, and above 0.95 the
  filter self-oscillates as MiniFreak V's does from about 0.985. The ring
  holds at the measured level (13.4 dB under a full sine) at any cutoff and
  in every mode, and starts from silence. Imported resonance from 0.985 up
  lands in that zone. Values up to 0.95 are unchanged. Flute Zut, Alias
  Sings, Orpheus and Whole Tone are tones made by the filter alone, and they
  had imported silent.
- **Filter resonance.** Measured on LP, BP and HP: as Resonance rises, the
  MiniFreak's passband sinks (-3.5 dB at 0.5, -6 at 0.8, -7.5 at 0.95) while
  the resonant peak keeps its height. The filter now scales its input by
  that loss and narrows its damping to match. The state's soft clip,
  x / (1 + 0.26|x|), acted every sample even on quiet signals, which held the
  peak 12-15 dB under the hardware's. It is now x / sqrt(1 + 0.3 x^2), which
  is linear for small signals and still limits a loud resonance. Against a
  saw, per-harmonic responses now match within about 3 dB at Resonance
  0.5-0.95 (they differed by up to 15 dB). A cutoff lane's closed steps in
  the sequencer test now keep a little more of the cutoff's glide.

- **Voice mode and Unison import.** Measured with two overlapping notes:
  MiniFreak's Note Mode 0 is Mono, 1/3 is Unison (a mono stack), and the
  rest play polyphonically. These now import onto Voice Mode. Unison,
  measured: Count stacks 2 to 6 voices (new `unison_voices`, default 6),
  which sit on whole steps of 197 cents x Spread either side of the note,
  never closer than about 2.75 cents, and play 3 dB down each. The stack
  measures 0 / +1.7 / +2.9 / +3.9 / +4.7 dB for 2-6 voices against the
  hardware's 0 / +1.8 / +3.0 / +4.0 / +4.8. Bizar's Spread is six of
  those steps, so an old six-voice patch keeps its span. Free-running Unison
  voices now start at random phases (sub oscillator, SuperWave copies and
  chord voices included); before, a stack could open 15 dB louder in phase.
  Older Unison patches play 3 dB quieter.
- **Glide.** MiniFreak V glides in a straight line in semitones. In Time
  mode every glide takes the knob's time whatever the interval (about
  11.2 s x knob^3, 10 ms at 0.05); Rate mode covers 4.1 octaves per that
  time. Bizar used an exponential approach capped at 500 ms. Glide
  now runs to 12 s, the imported knob follows the measured curve, and state
  v15 triples older patches' Glide so they reach the note in about the same
  time. Measured on the hardware's own curve, an octave or a fifth lands
  within 1 % in Time mode and 5 % in Rate mode.
- **Mono fixes.** A Mono note-on is legato only while another key is held,
  so a voice left sounding by a different patch no longer swallows the
  attack. A note-off that cancels a queued note now lets the waiting voice
  release normally; a Mono sequencer re-uses one voice for every step, and
  its final note-off used to cut the tail dead.

Loudness and brightness pass (state v16), each change measured on MiniFreak V:

- **Mod Wheel.** Presets store a wheel position (80 factory presets, Comets
  at 1.0) and MiniFreak V restores it. New `mod_wheel` parameter (the
  grid's Wheel source at load; MIDI CC 1 still moves it), imported.
- **Velo > Time** stretches Decay and Release by exp(3.45 x amount x
  velocity), leaving Attack alone. It was 1 + amount x velocity. v16 states
  keep a full-velocity note's old length.
- **VCA routes.** Gain is clamp(1 - min(sum of offsets, 1) + sum of a x s,
  0, 1), with an offset of |a| for bipolar sources (LFOs, Keyboard), a for a
  positive unipolar route and 0 for a negative one. Keyboard -> VCA did
  nothing at C3 before.
- **Sequencer gate.** A step's note lasts 0.08 + 0.92 x Gate of the step.
- **Engines.** Two Op FM has its own modulator phase (it jumped every two
  cycles unless 2 x ratio was whole) and a 2 x Timbre^2-cycle index, with
  the Shape feedback measured both ways. Noise is rebuilt from the
  MiniFreak's dust, filter and 808-metal tables. Multi Filter has the right
  mode order (LP36..LP6, HP6..HP36, BP, Notch), cutoff table and one-pole
  coefficient. Phaser is rebuilt from its measured transfer function: the
  chain's input and output are mixed (it was the all-pass alone, with no
  notches), the output is fed back with k = 0.972 x Timbre through
  0.45 sample of delay, and the level falls by (1 - k)^0.25, so near full
  Timbre the notches stay put while the response sinks 9 dB under one sharp
  peak. It matches the hardware within 1.9 dB median (3.5 dB worst) across
  Wave, Timbre and Shape; it was up to 11 dB off at high Timbre. Chord above
  Shape 0.55 plays the measured soft wavetable.
  Comb feedback rings as long as the hardware's near full Timbre. Bass
  noise is low-passed. Harm is four sine partials at measured amplitudes
  and phases: Wave spreads them from unison through harmonics to octaves,
  Timbre rectifies them, and Shape drives a tanh with pink noise. Its
  partials used to jump every cycle at non-integer spacings, which was
  +1.2 oct too bright. SawX is 1 - 2 frac(phase x (1 + 7 Wave)), which
  matches the hardware's harmonics within about 1 dB; it was 5.5 dB loud,
  a different base wave, and noisy. Its Timbre is still approximate. SuperWave
  was 9 dB loud, its copies started in phase, and its saw/square/triangle/
  sine switch points were at thirds rather than quarters.
- **Cycling Envelope.** Tempo-synced Run and Loop stages now import at
  their measured length (a 1/32 note plus up to four bars at 120 BPM, linear
  in the knob); they were read as free times, so a synced Fall of 0.056
  became a 1 ms stage and the loop ran at audio rate (KiK, Welded, Wob). Env
  mode's Rise and Fall reach 50 s like the ADSR they follow (they stopped at
  10 s, so Big Dipper's 30 s Fall ran three times fast). Env and Loop
  restart on legato notes too, as the hardware does with Retrig Source on
  Poly or Mono Kbd.
- **Output** rounds to 16 bits rather than truncating.
- **chain_params fits the device again.** It had grown to 34.8 KB (FX set
  to Delay), past Move's 32 KiB read buffer, which broke the parameter list
  on the device. The device copy now leaves out the `description` fields
  (31.5 KB worst case, and a test keeps it under 32 KiB). The new
  `chain_params_full` key keeps them for the browser build's tooltips and
  for PARAMETER_REFERENCE.md. The Remote UI on the device loses its tooltips.
- **KarplusStr** rebuilt from a Bow x Position x Decay grid measured on
  MiniFreak V (held C2/C3/C5). Wave is Bow again: a pitched bow (a pulse a
  period plus noise) under the pluck, whose plateau rises with Bow and Decay
  and saturates near -19 dB, so Bow 0 is now a bare pluck that dies away
  (there was always a fixed bow under every note, and the Bow knob moved the
  strike position). Decay is a loss per period of the string (higher notes
  die faster), Position slides a strike-position comb, and the strike is the
  same burst on every note, as on the hardware. Against the grid the
  loudness-contour error fell from 9.1 to 2.9 dB and the spectrum error from
  8.7 to 5.9 dB (early) and 9.3 to 6.0 dB (late); Funky Klav's preset contour
  went from 16.9 to 15.0 dB (its remaining gap is the closed filter, not the
  string).

Scored on both level (within 6 dB) and brightness (centroid within half an
octave) against MiniFreak V, 198 of 256 factory presets now pass both (131
before this pass); 229 are within 6 dB and 215 within half an octave.

Together with the Osc 2, Modal and Reverb work above, the share of the 257
factory presets within 6 dB of MiniFreak V's loudness (FX off, one held
note) went from 165 to 200, within 3 dB from 93 to 129, and within half an
octave of its brightness from 125 to 149.

Across all 257 factory presets (FX off on both synths, one held note), the
loudness contour error fell from a median 6.0 dB to 4.2 dB. The
destination-scale pass mostly moved brightness: 42 presets track the
hardware's timbre contour more closely and 30 less (median 0.415 -> 0.393
octaves); loudness went 21 closer and 16 further by more than 1 dB. Most
presets that moved away had a second mismatch that the old scales were
hiding. Their CycEnv or LFO routes are now right, but the path behind them is
not: an Env-mode CycEnv into a dark cutoff (Steq, Tubular B3K, House Mode,
SaltimBanj), LFO 2 into Osc 2 Wave (Arbos), engine level against pitch
(Mindfull), and a filter baseline that velocity used to brighten (Dub Smash).

## 0.8.0: FX parity pass

The effects were measured against MiniFreak V: impulse responses, swept
noise and sine tests through its Audio In engine, and renders of the same
patch, then each effect was reworked to follow the measured curves.

- **Importer.** Reverb and Delay were swapped in the FX type table, so every
  imported reverb played as a delay and vice versa (278 of the factory bank's
  FX slots). Subtypes (`FXn_Opt1`) are now imported, including the Delay's
  interleaved Sync entries, and Send/Insert routing is converted onto the
  Delay/Reverb mix law. Newer exports with the longer FX menu are recognised.
- **Time and Intensity** now mean the MiniFreak's knob positions for every
  effect. FX Time runs 1..1000 (state v12); Delay still reads it as ms and
  now reaches 1 s. Older states are migrated so non-delay effects keep their
  knob position.
- **Mix laws** follow the hardware per effect: Delay and Reverb keep the dry
  up to Amount 0.5 and then fade it, Chorus/Flanger/Distortion/Bit Crusher
  crossfade at equal power, and Phaser tops out at an even blend.
- **Reverb**: decay set directly as a T60 (0.9 s to 18 s), Intensity as
  in-loop damping, preset decay ratios measured, early onset within a few ms.
  Fixed a modulation bug that read freshly written samples and cut every
  tail short.
- **Delay**: Stereo, Ping-Pong, Mono and the Filtered (band-pass in the loop)
  modes rebuilt to their measured structures; the 17 Sync divisions and
  their knob steps are the MiniFreak's.
- **Chorus, Flanger, Phaser**: measured rates and depths; the phaser is six
  all-pass stages with feedback (three notches), swept 275..1480 Hz.
- **Distortion, Bit Crusher, Multi Comp, 3-band EQ**: drive curve and clip
  ceiling, downsampling ratios and bit floor, the Multi Comp's upward static
  curve and band tilt, and shelf/bell EQ fitted to within 0.12 dB.

Per effect, against MiniFreak V on the same patch: reverb decay within about
10 % below Intensity 0.9, delay times and repeat levels within 1 dB,
modulation rates exact, nonlinear effects within 1.2-1.6 dB of band level
(about the residual dry-patch difference). Across 60 factory presets the
error of the FX contribution (FX on versus off, both synths) fell from a
median 7.5 dB to 4.2 dB while notes sustain and from 6.7 dB to 5.0 dB in the
release; what remains is mostly engine and envelope differences feeding the
nonlinear effects, and FX-amount modulation routes.

## 0.7.0: engine parity pass

Engines were compared against MiniFreak V by rendering both with the same notes
and knob settings and measuring harmonics and band spectra. Where a difference
was structural the engine was reworked; where it was a scale or routing
difference the knob is remapped; levels were trimmed to match.

- **BasicWaves** is rebuilt as MiniFreak builds it. Wave crossfades a pulse
  into a saw (0 to 0.5), then the saw into a double saw (0.5 to 1). Timbre sets
  the offset between the two saws (pulse width, and the double saw's spread).
  Shape lowers the oscillator and brings in an octave-down sine. It matches the
  reference to about 0.2 dB per harmonic.
- **Harmo** now plays odd partials only: Wave slides a peak up the series,
  Timbre fills in the floor and a thin upper tail. The old eight-table drawbar
  model is gone.
- **V.Analog**: MiniFreak's Wave, Timbre and Shape now drive our Timbre, Shape
  and Wave knobs respectively. **Wavetable**: Wave and Timbre are swapped and
  all three knobs are re-curved. **KarplusStr**: MiniFreak Wave drives Timbre
  and Shape drives Shape. MiniFreak's string ignores Timbre, so the engine's own
  Wave (bow) is held near its low end.
- **Speech** from Wave 0.375 up plays a spoken word once per note; Timbre
  stretches it. Below 0.375 it is the sustained voice. Shape picks one of about
  ten words on MiniFreak, and every Shape plays the same generic word here, so
  word mode stays a rough match. There is no recorded or sampled data.
- **Levels** of Wavetable, KarplusStr, Modal and Speech are trimmed to the
  reference.

Effect on existing presets, measured on a held note with effects off: of the
265 built-in presets (8 original, 257 imported), 122 use a changed engine.
48 of those are near-identical (BasicWaves mostly); about 40 sound
clearly different and 17 of those strongly so (Harmo, V.Analog, Wavetable and
KarplusStr presets, plus Soft Bloom). Presets you saved with these engines will
sound different too, because the knob meanings changed. 17 of the 122 get more
than 6 dB louder (10 by more than 10 dB, Heavy B and Chapel Organ the most) and
3 more than 6 dB quieter, so check output level before playing them loud.

Not changed: Noise, Harm, SuperWave, Bass, SawX, Formant, Waveshaper, Two Op FM
and Modal's response (only its level) were left as they were because no
remapping improved them reliably. The Osc 2 processor modes, effects, filter
and envelopes were not compared in this pass.
