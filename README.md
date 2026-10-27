# Bizar

Bizar is a hybrid synthesizer for Ableton Move, running as a sound-generator
module in the [Schwung](https://github.com/charlesvestal/schwung) framework.
It has two interacting digital engines, a modulation grid, a Cycling
Envelope, a sequencer and arpeggiator, and three effect slots, with 128
original factory presets.

**Try it live:** [play Bizar in your browser](https://klaine.github.io/bizar/) (WebAssembly build, WebMIDI).

Bizar takes its workflow from two-engine performance synths like Arturia's
MiniFreak. It is not an Arturia product and is not affiliated with Arturia.
It contains no Arturia code, firmware, presets or sample data. If you own a
MiniFreak, you can build Bizar with your own exported presets for personal
use; see [docs/MINIFREAK_IMPORT.md](docs/MINIFREAK_IMPORT.md).

## Features

- **Voices.** Six-voice polyphony, plus Mono and Unison (mono or poly stacks),
  a keyboard Chord mode, voice stealing and portamento. Responds to MIDI
  velocity, pitch bend, channel pressure and polyphonic aftertouch.
- **Two engines.** Oscillator 1 offers BasicWaves, Two Op FM, Wavefolder,
  Noise, Wavetable, SuperWave, Harmo, Formant, V.Analog, Waveshaper,
  Chords, KarplusStr, SawX, Harm, Bass, an inharmonic Modal resonator and a
  Speech formant engine. Oscillator 2 has the same generators plus processor
  modes that work on Oscillator 1: Ring, Wavefolder, MultiFilter,
  Crusher, Comb Filter, Surgeon Filter, Phaser Filter, Destroy and FM / RM.
  Each oscillator has Tune, Type, Wave, Timbre, Shape and Volume, and the three
  timbre controls mean something specific to each engine. Several engines are
  adapted from Emilie Gillet's Plaits; see `THIRD_PARTY_NOTICES.md`.
- **Wavetables.** Thirty-two 64-frame banks from the CC0 WaveEdit Online
  collection, with anti-aliased mip levels. Both oscillators pick a bank with a
  separate Wavetable selector; the knobs are then Position, Width and Tone.
  Sources are listed in `assets/wavetables/README.md`.
- **Filter and envelopes.** A per-voice resonant LP/BP/HP filter, an ADSR with
  Quick and Percussive curve options and a retrigger switch, and velocity
  targets for VCA, VCF, envelope depth and envelope time.
- **Modulation.** Two LFOs with nine shapes (including an editable Shaper),
  tempo sync and retrigger modes; a Cycling Envelope with Rise/Hold/Fall and
  Env/Run/Loop modes; and a modulation grid of twelve destination columns fed
  by seven sources (Cycling Envelope, Envelope, LFO 1, LFO 2, Velocity +
  Aftertouch, Wheel and Key Track). Four macros each drive four destinations, and any
  matrix cell (`M4:3` = LFO 2 into column 3) can itself be a destination.
- **Effects.** Three serial stereo slots with Delay (seven characters, each
  also tempo-synced), Reverb, Chorus, Flanger, Super Unison, Drive, Bit
  Crusher, Phaser, 3-band EQ, Peak EQ and a three-band Multi Comp. Each effect
  has its own preset list.
- **Sequencer and arpeggiator.** See below.
- **Move integration.** Schwung's `ui_hierarchy`, `chain_params` and `state`
  contracts drive Move's parameter pages, automation and patch saving. Every
  page and control is listed in [PARAMETER_REFERENCE.md](PARAMETER_REFERENCE.md).
- **Remote UI.** A browser editor with knobs, draggable envelope and Shaper
  curves, a filter response view, waveform previews, the full modulation
  matrix with hardware-style mod assign, and a sequencer editor.

## Factory presets

Bizar ships 128 original presets, sixteen in each of eight categories. Each
name starts with a two-letter category tag so the list is easy to browse on
Move's small display:

| Tag | Category | Tag | Category |
|---|---|---|---|
| `BA` | Bass | `PD` | Pads |
| `LD` | Leads | `PL` | Plucks and mallets |
| `KY` | Keys | `AR` | Arpeggios and sequences |
| `PY` | Polysynths and brass | `FX` | Effects, drones and percussion |

`PY Init` is a plain saw starting point. The full list is in
[docs/PRESETS.md](docs/PRESETS.md).

Every preset has the same two macros, so they play the same way across the
bank. Each one moves several controls at once:

| Macro | Name | What it moves |
|---|---|---|
| 1 | Timbre | Opens the filter (where it has room) and moves the engines' signature controls with it: supersaw detune and stack, FM index, fold, wavetable position, sync, formant… plus the drive |
| 2 | Space | Reverb and delay amount, length and feedback, and a longer release |

Presets built on an LFO (wobbles, gates, pumps and the riser) add **Macro 3,
Motion**: the knob is the LFO's depth. The macros start at 0, where the preset
sounds as designed.

The bank is defined in `scripts/generate_factory_bank.py`, which the build
turns into a C header. To change or add a preset, edit that script. Levels
are evened out with `tools/measure_factory_levels.c`, which plays every preset
and prints its RMS and peak level; the script's `LEVELS` table holds the trims.
`tools/render_factory_preset.c` writes presets to WAV files (basses play a
short riff, the rest a held note or chord) so a change can be auditioned
before it goes on Move.

## Sequencer and arpeggiator

A step sequencer and an arpeggiator are built in, clocked from Move's tempo
like the synced LFOs. Holding keys starts them:

- **Seq** plays a pattern of up to 64 steps, with up to six notes per step,
  transposed by the last key held (relative to middle C).
- **Arp** arpeggiates the held keys: Up, Down, Up/Down, Random, Played,
  Chord or Poly, over one to four octaves.

Rate is a 15-entry division list, with gate and swing. Up to four step
automation lanes each point at a modulation destination and hold one offset
per step; a smooth lane glides between steps and a stepped lane holds each
value. Lanes play in Seq mode.

The sequencer is edited in the Remote UI. Its settings (`seq_mode`,
`seq_arp_mode`, `seq_arp_oct`, `seq_div`, `seq_gate`, `seq_swing`,
`seq_length`, `seq_data`, and `seq_lane1..4_dest`, `_smooth` and `_data`) are
saved with the patch but are not on Move's own parameter pages.

## Browser build

**Try it live: <https://klaine.github.io/bizar/>** (the single-file build, served from the `pages` branch).

`sh scripts/build.sh --web` writes `dist/bizar-web.html`, a single
self-contained file with the Remote UI and the same DSP compiled to
WebAssembly. It needs [zig](https://ziglang.org/), and a normal
`scripts/build.sh` run first to create the generated headers.

Serve it over https (or from localhost), or open it from disk. Click **Start
audio**, then play from a WebMIDI device (Chrome or Edge; Firefox needs its
MIDI add-on), the computer keyboard in Live's layout (A–K white keys,
W E T Y U O P black keys, Z/X octave, C/V velocity), or the on-screen
**⌨ Keys** panel. A BPM box drives the synced LFOs and the sequencer; the
**MIDI** menu picks the input and channel and can follow MIDI clock;
Alt-click (or L) on a knob learns a MIDI controller. Your sound is saved in
the browser and restored on the next visit.

The Remote UI, in the browser build and on Move, has undo/redo, saved user
patches, A/B compare, patch export and import, share links (the patch travels
in the link's `#p=` fragment) and a patch browser with favourites.

## Building

The module targets Schwung's API-v2 sound-generator ABI and Move's aarch64
Linux.

On Linux or macOS with an AArch64 cross compiler (`aarch64-linux-gnu-gcc`,
or set `CC`), or with [zig](https://ziglang.org/), which the script falls
back to as `zig cc` when no cross compiler is found:

```bash
sh scripts/build.sh
```

With Docker:

```bash
docker build -f scripts/Dockerfile -t bizar-builder .
```

```bash
docker run --rm -v "$PWD:/work" -w /work bizar-builder
```

On Windows, start Docker Desktop with its Linux engine, then run from
PowerShell in the project folder:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-windows.ps1
```

This builds the container toolchain, runs the tests, cross-builds the module
and writes the tarballs under `dist\`.

Build output:

- `dist/bizar/` — the module directory (`dsp.so`, `module.json`, the Remote
  UI and the docs)
- `dist/bizar-module.tar.gz` — the canonical package name, which the module
  catalog's `asset_name` expects
- `dist/bizar-module-<version>.tar.gz` — the same bytes under the version from
  `module.json`
- `dist/bizar-web.html` — the browser build, when `zig` is on `PATH`

### Tests and generated docs

Run the desktop DSP and integration tests with a native compiler:

```bash
CC=cc sh scripts/build.sh --test
```

Regenerate `PARAMETER_REFERENCE.md` and `docs/PRESETS.md`:

```bash
sh scripts/build.sh --reference
```

The parameter reference comes from the module's own `ui_hierarchy` and
`chain_params`, so it cannot drift from the code. CI regenerates both
documents and fails if the checked-in copies are stale.

## Installing on Move

Install the tarball with Schwung Manager, or extract the module directory
into `/data/UserData/schwung/modules/sound_generators/bizar/` on the Move,
then rescan modules or restart Schwung. Bizar appears under Sound Generators.

From Windows, with OpenSSH and a root SSH key added through Move's
`http://move.local/development/ssh` page:

```powershell
ssh-keygen -t rsa -b 3072 -C "bizar-move" -f "$HOME\.ssh\bizar_move_rsa"
(Get-Content "$HOME\.ssh\bizar_move_rsa.pub" -Raw).Trim() | Set-Clipboard
# Paste the key into Development > SSH Keys in Move Manager and confirm on Move, then:
powershell -ExecutionPolicy Bypass -File scripts/install-windows.ps1 `
  -MoveHost move.local -SshKeyPath "$HOME\.ssh\bizar_move_rsa"
```

## Using your own MiniFreak presets

If you own a MiniFreak or MiniFreak V, you can compile an exported preset bank
into a personal build of Bizar:

```bash
MINIFREAK_BANK=private/MyBank.mnfx sh scripts/build.sh
```

The import is off unless you ask for it, and a build with it is written as
`dist/bizar-module-personal.tar.gz` so it is never mistaken for a release.
Do not publish such a build. Export steps, the Windows option, what is
imported and the known gaps are in
[docs/MINIFREAK_IMPORT.md](docs/MINIFREAK_IMPORT.md).

## Releasing

`module.json` is the single source of truth for the version. Bump its
`"version"`, rebuild from a clean checkout without `MINIFREAK_BANK`, and the
packaging step writes both tarballs. Keep the archive's top-level directory
named `bizar` so it extracts to a folder that matches the module id, as the
catalog installer requires.

Publishing through Schwung Manager also needs:

1. A `release.json` on the default branch with the current `version` and a
   `download_url` pointing at the tagged tarball, for example
   `releases/download/v0.8.0/bizar-module.tar.gz`.
2. A module catalog entry with `id` (`bizar`), `name`, `description`,
   `author`, `component_type` (`sound_generator`), `github_repo`,
   `default_branch`, `asset_name` (`bizar-module.tar.gz`) and
   `min_host_version`.

## Status and known limitations

Bizar is a beta at version 0.8.0. It passes the desktop DSP and integration
tests and cross-compiles for Move; a sustained six-voice run with all effects
on Move hardware has not been recorded for this revision.

- **Sample and granular engines.** Sample, Particles, Cloud Grains, Hit Grains,
  Frozen, Skan, Lick and Raster load and respond to their controls, but each
  plays a shared placeholder tone. They need an asset-loading model that keeps
  file I/O and allocation off the render path. **Audio In** is a placeholder
  too, because the module declares no `audio_in` capability.
- **Vocoder effects and user wavetable import** are not implemented.
- **Modal** is Plaits' modal resonator with 12 modes and a fixed accent. It is
  struck only; the sustained (dust-excited) mode is not wired up.
- **Effect routing.** The three slots always run in series, FX1 → FX2 → FX3;
  per-slot send/insert routing is not implemented.
- **Move hardware validation.** The parameter pages, the two-page modulation
  matrix and the Remote UI have been exercised on desktop and in automated
  tests, but not yet on a device for this revision.
- **Device profiling** — CPU and memory headroom at full polyphony with every
  effect on — is still outstanding.

Changes are recorded in [CHANGELOG.md](CHANGELOG.md).

## Project layout

| Path | Contents |
|---|---|
| `src/bizar.c` | Plugin adapter: parameters, state, presets, voice allocation and rendering |
| `src/engines.c`, `effects.c`, `filter.c`, `modulation.c`, `voice.c`, `sequencer.c` | Allocation-free DSP units |
| `scripts/generate_factory_bank.py` | The 128 factory presets |
| `scripts/generate_minifreak_bank.py` | Optional MiniFreak bank importer (personal builds) |
| `scripts/generate_wavetables.py` | Converts the CC0 wavetables into mip levels |
| `scripts/build.sh` | Build, test, reference and web targets |
| `web_ui.html`, `web_ui_extras.js` | Remote UI (the extras are inlined by `scripts/inline_extras.py`) |
| `web/` | Browser build: WebAssembly glue, host shim and bundler |
| `tests/test_plugin.c` | Desktop DSP and integration tests |
| `tools/` | Contract dump, factory level meter and renderer, wavetable matching helper |
| `include/` | Schwung ABI declarations, so the module builds without a Schwung checkout |

## Implementation notes

- The module reads no files and does no heap allocation in its plugin
  callbacks. It uses a fixed pool of eight instances and clears delay memory
  incrementally, a bounded amount per render block.
- Audio is rendered at 44.1 kHz in stereo int16 blocks. The delay buffer holds
  about 2.97 seconds so Sync presets reach a whole note at common tempos.
  Effects stay bypassed until their buffers are cleared; the reverb clears its
  own region whenever its slot changes effect type, so a delay tail is never
  read back as a room.
- Engine and effect order is part of the patch format, so new engines are
  appended and existing indices never move. Older saved states are migrated
  on load; each state version change is explained in `src/bizar.c`.
- Envelope decay and release times are times to −60 dB, so they read longer
  than on synths that quote a time constant.
- Voices are summed with a fixed per-voice gain and a cubic soft clipper on
  the master, without renormalising for the number of active voices, so a held
  note does not duck when another one starts.

## License

Bizar is released under the MIT License; see `LICENSE`. Parts of the engines
are adapted from Plaits (MIT); see `THIRD_PARTY_NOTICES.md`. The built-in
WaveEdit Online wavetables are CC0 1.0; see `assets/wavetables/README.md`.
Schwung and Ableton Move are separate projects under their own licenses.
