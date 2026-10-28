# Feature backlog

Features proposed but not started. Each entry says what done looks like; move it to `CHANGELOG.md`
when it ships.

## Scale-lock the arpeggiator and sequencer to the Move set

Shared with Crest (its `BACKLOG.md` has the same entry); the two can be done together.

- **What:** an optional scale quantizer on the notes the arpeggiator and step sequencer play
  (`src/sequencer.c`, fed from `src/bizar.c`). Options: Off, Set (follow the Move set's root and
  scale) and a few fixed scales.
- **Model:** Amino's Scale = Set option (`quantize_scale` and `amino_move_scale_update` in Amino's
  `src/dsp/voice.c`), which reads the set's root and scale through `move_info.h`. Bizar already reads
  `move_info` for tempo, a lock-free seqlock copy that is safe on the audio thread; resolve it first
  in `create_instance`, as Amino does.
- **Rules:** append the parameter (never renumber saved indices), regenerate
  `PARAMETER_REFERENCE.md` and `docs/PRESETS.md` with `sh scripts/build.sh --reference`, add a
  `CHANGELOG.md` entry.
- **Done:** the parameter on the device and in the Remote UI, a test in `tests/test_plugin.c`, and
  `CC=cc sh scripts/build.sh --test` passing.
