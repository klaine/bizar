# Bizar: agent notes

Two-engine hybrid `sound_generator` (Schwung API v2), module id `bizar`, formerly "SchwungFreak".
Read [README.md](README.md) for the product, layout and release steps. This file covers working on the
code.

## Commands

```sh
CC=cc sh scripts/build.sh --test    # desktop DSP and integration tests (tests/test_plugin.c)
sh scripts/build.sh                 # dsp.so ($CC, else aarch64-linux-gnu-gcc, else zig cc) + dist/ + web build
sh scripts/build.sh --reference     # regenerate PARAMETER_REFERENCE.md and docs/PRESETS.md
sh scripts/build.sh --web           # dist/bizar-web.html only (needs a full build once)
sh scripts/deploy.sh [--personal]   # staged install on a Move (MOVE_HOST, MOVE_USER)
```

The project skills `/build` and `/deploy` in `.claude/skills/` wrap these.

## Rules

- **MiniFreak material is personal-use only.** Arturia's presets, exports (`*.mnfx`), renders
  (`*.aif`) and `private/` are git-ignored and must never be committed, published or quoted at
  length. Only run `MINIFREAK_BANK=… ` builds when the user names the bank. They write
  `*-personal.tar.gz`, which is never released.
- **Generated docs must stay current.** CI runs `--reference` and fails on a diff in
  `PARAMETER_REFERENCE.md` or `docs/PRESETS.md`. After changing a parameter, a range or a factory
  preset, regenerate both and include them in the change.
- **Patch format:** engine and effect indices are part of saved state, so append new ones. If an
  index has to move, bump the state version (written as `"version"` in `src/bizar.c`'s state JSON,
  currently 19), add the migration to `apply_state_json`, and explain it in `CHANGELOG.md`.
- **Version:** `module.json` holds the only version string. Every user-visible change gets a
  `CHANGELOG.md` entry under the top "Unreleased" heading.
- **Realtime:** no file I/O and no heap allocation in plugin callbacks. The plugin uses a fixed
  pool of 8 instances, and delay memory is cleared incrementally.
- Builds use `-Wall -Wextra -Werror` with both gcc (CI) and clang (zig). Keep both warning-free.

## Where things come from

- `build/generated/`: wavetable mips, the factory bank and the (empty) MiniFreak bank headers,
  written by `scripts/generate_*.py` on every build.
- `web_ui.html` carries `web_ui_extras.js` inlined between the `sfx` markers
  (`scripts/inline_extras.py`). Edit the `.js`, not the inlined block.
- `include/` holds the Schwung ABI declarations (so no Schwung checkout is needed).
