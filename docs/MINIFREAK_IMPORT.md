# Building with your own MiniFreak presets

Bizar can compile a MiniFreak preset bank into the module, so a MiniFreak
owner can play their own presets on Move. The bank is added after Bizar's own
128 factory presets, in the same preset browser.

**This is for personal use only.** MiniFreak presets belong to Arturia or to
whoever made them. They are not part of Bizar, this repository does not
include any, and a build that contains them must not be shared or published.
The build system helps with that: it never picks up a bank unless you ask
for one, and a build that has one gets a different file name from a release.

Bizar is not an Arturia product. It is not affiliated with or endorsed by
Arturia. MiniFreak and MiniFreak V are Arturia trademarks, used here only to
say which presets the importer reads.

## 1. Export a bank

The importer reads the `.mnfx` bank export written by MiniFreak V, Arturia's
software version of the instrument. Open MiniFreak V, select the bank you want
in its preset browser and export it. To use presets from a hardware MiniFreak,
first move them into MiniFreak V with Arturia's own tools, then export from
there. Menu names change between versions, so see Arturia's
MiniFreak V manual if you cannot find the export option.

Put the export anywhere you like. The repository's `private/` folder is a good
place: git ignores it, and it is inside the project, which the Docker build
needs.

```bash
mkdir -p private
cp ~/Downloads/MyBank.mnfx private/
```

Git also ignores `*.mnfx` and `*.mf_export` files anywhere in the tree.

## 2. Build

Set `MINIFREAK_BANK` to the export when you run the build.

macOS or Linux, with an AArch64 cross compiler:

```bash
MINIFREAK_BANK=private/MyBank.mnfx sh scripts/build.sh
```

Windows, with Docker Desktop (the bank has to be inside the project folder):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-windows.ps1 -MiniFreakBank private\MyBank.mnfx
```

A personal build writes:

- `dist/bizar-module-personal.tar.gz` instead of `dist/bizar-module.tar.gz`
  (no versioned copy is made)
- `dist/bizar-web-personal.html` instead of `dist/bizar-web.html`, when `zig`
  is installed for the browser build

Every build regenerates the bank header. A later build without
`MINIFREAK_BANK` is clean again, so a forgotten import cannot slip into a
release.

## 3. Install

Install the personal tarball the same way as a normal build (see the README).
From Windows:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/install-windows.ps1 -Personal -MoveHost move.local
```

On Move, the imported presets follow the factory bank: the first one is
number 128 in the preset list, and they keep their MiniFreak names.

## Tests

The test suite has extra checks that run only when a bank was imported:

```bash
MINIFREAK_BANK=private/MyBank.mnfx CC=cc sh scripts/build.sh --test
```

Every imported preset has to load, name itself and play. A few checks look
for specific presets of the MiniFreak V factory bank (3 Steps, Daft Lead,
Enou, PS1), so they only pass with that bank. With other banks, expect those
checks to fail.

## What is imported

`scripts/generate_minifreak_bank.py` converts each preset into Bizar's own
state format. Its comments explain each mapping and how sure it is. In short:

- Both oscillators, with their types, Wave/Timbre/Shape, tuning, levels and
  pitch-mod quantize settings.
- The filter, envelope, velocity targets, Cycling Envelope, both LFOs (with
  Shaper curves), voice mode, glide, unison, keyboard chord and bend range.
- The modulation matrix (as twelve grid columns) and the macro assignments.
- All three effect slots with their types, presets and knob positions.
- The sequencer and arpeggiator, with step automation lanes.

## Known gaps

- A few matrix destinations have no counterpart in Bizar yet: the hardware's
  "Mod x:y" matrix outputs, "Vib AM", the discrete destinations (oscillator
  types, LFO wave shapes), glide routes, the synced LFO rates, and a few rarer
  addresses. Routes to these are dropped rather than guessed.
- The sample and granular engines, Audio In and the Vocoder effects are
  placeholders in Bizar, so presets that use them sound different.
- Keyboard Chord holds five notes over the key, not seven. Para Unison (twelve
  voices on the MiniFreak) plays as Poly Unison on Bizar's six voices.
- Sequencer ratchet, repeat, mutate and spice are not imported. The offset
  scale for step automation is a best fit from the bank data, not measured on
  MiniFreak V.
- LFO Retrig's LFO, CycEnv and Seq Start sources import as Mono Kbd.
- Some presets play at a different level than on MiniFreak V, and a few
  resonant or Modal patches differ in tone. `CHANGELOG.md` records what was
  measured and what is still known to differ.
