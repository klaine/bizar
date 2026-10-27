#!/bin/sh
# Build Bizar: generated headers, the aarch64 dsp.so, dist/bizar-module.tar.gz
# and (with zig) dist/bizar-web.html. Run from anywhere:
#
#   sh scripts/build.sh              # device build + package (+ web build)
#   sh scripts/build.sh --test       # native plugin tests
#   sh scripts/build.sh --reference  # regenerate PARAMETER_REFERENCE.md, docs/PRESETS.md
#   sh scripts/build.sh --web        # browser build only
#
# The device compiler is $CC if set, else aarch64-linux-gnu-gcc, else zig cc
# (brew install zig; set ZIG=/path/to/zig to override the lookup).
set -eu

case "${1:-}" in
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
esac

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="$ROOT/build"
DIST_DIR="$ROOT/dist/bizar"
GENERATED_DIR="$BUILD_DIR/generated"

mkdir -p "$BUILD_DIR" "$DIST_DIR" "$GENERATED_DIR"
PYTHON=${PYTHON:-python3}
"$PYTHON" "$ROOT/scripts/inline_extras.py"
if [ ! -f "$GENERATED_DIR/wavetable_bank.h" ] || [ "$ROOT/scripts/generate_wavetables.py" -nt "$GENERATED_DIR/wavetable_bank.h" ]; then
    "$PYTHON" "$ROOT/scripts/generate_wavetables.py" "$GENERATED_DIR/wavetable_bank.h"
fi

# Bizar's own factory bank is generated on every build.
"$PYTHON" "$ROOT/scripts/generate_factory_bank.py" "$GENERATED_DIR/factory_bank.h"

# The MiniFreak bank import is opt-in and for personal builds only: Arturia's
# presets are not part of this project and must not be redistributed. Point
# MINIFREAK_BANK at your own .mnfx export (see docs/MINIFREAK_IMPORT.md).
# Without it the generated header is empty, so a stale import never leaks
# into a later build.
BANK=${MINIFREAK_BANK:-}
if [ -n "$BANK" ] && [ ! -f "$BANK" ]; then
    printf 'MINIFREAK_BANK=%s: no such file\n' "$BANK" >&2
    exit 1
fi
"$PYTHON" "$ROOT/scripts/generate_minifreak_bank.py" "$GENERATED_DIR/minifreak_bank.h" ${BANK:+--bank "$BANK"}

"$PYTHON" "$ROOT/scripts/generate_wavetable_preview.py" "$GENERATED_DIR/wavetable-preview.js"

# The parameter reference is generated from the module's own UI contract, so a
# key or range that is not in get_param() cannot appear in the document. The
# preset list in docs/PRESETS.md is regenerated with it. This
# target needs a native compiler (not the ARM64 cross compiler); CI runs it and
# fails if the checked-in document is stale.
if [ "${1:-}" = "--reference" ]; then
    REF_CC=${REF_CC:-${CC:-cc}}
    "$REF_CC" -std=c11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE \
        -I"$ROOT/include" \
        -I"$GENERATED_DIR" \
        "$ROOT/src/bizar.c" "$ROOT/src/engines.c" "$ROOT/src/effects.c" \
        "$ROOT/src/filter.c" "$ROOT/src/modulation.c" "$ROOT/src/voice.c" "$ROOT/src/sequencer.c" \
        "$ROOT/tools/dump_contract.c" \
        -lm -o "$BUILD_DIR/dump_contract"
    "$BUILD_DIR/dump_contract" \
        "$BUILD_DIR/hierarchy.json" "$BUILD_DIR/chain_params.json"
    "$PYTHON" "$ROOT/scripts/generate_parameter_reference.py" \
        "$BUILD_DIR/hierarchy.json" "$BUILD_DIR/chain_params.json" \
        "$ROOT/module.json" "$ROOT/PARAMETER_REFERENCE.md"
    "$PYTHON" "$ROOT/scripts/generate_factory_bank.py" --markdown "$ROOT/docs/PRESETS.md"
    exit 0
fi

# Browser build only: one self-contained HTML file (UI + WASM DSP + WebMIDI),
# written to dist/bizar-web.html. Needs zig; scripts/build.sh without
# arguments also builds it when zig is on PATH.
if [ "${1:-}" = "--web" ]; then
    "$PYTHON" "$ROOT/web/build.py"
    sh "$ROOT/scripts/pages.sh"   # dist/bizar-web.html as index.html on the local pages branch (push: scripts/pages.sh --push)
    exit 0
fi

if [ "${1:-}" = "--test" ]; then
    TEST_CC=${TEST_CC:-${CC:-cc}}
    "$TEST_CC" -std=c11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE \
        -I"$ROOT/include" \
        -I"$GENERATED_DIR" \
        "$ROOT/src/bizar.c" "$ROOT/src/engines.c" "$ROOT/src/effects.c" \
        "$ROOT/src/filter.c" "$ROOT/src/modulation.c" "$ROOT/src/voice.c" "$ROOT/src/sequencer.c" \
        "$ROOT/tests/test_plugin.c" \
        -lm -o "$BUILD_DIR/test_plugin"
    "$BUILD_DIR/test_plugin"
    exit 0
fi

# zig cc is the fallback when no GNU cross compiler is installed (the usual
# case on macOS). The target flags can't live in $CC, which is quoted below.
ZIG=${ZIG:-zig}
CC_TARGET=
if [ -z "${CC:-}" ]; then
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        CC=aarch64-linux-gnu-gcc
    elif command -v "$ZIG" >/dev/null 2>&1; then
        CC=$ZIG
        CC_TARGET="cc -target aarch64-linux-gnu.2.28"
    else
        printf 'no aarch64 compiler: brew install zig, or set CC\n' >&2
        exit 1
    fi
fi
# -ffile-prefix-map: no absolute build paths (and so no user name) in shipped binaries
"$CC" $CC_TARGET -std=c11 -O3 -fPIC -shared -Wall -Wextra -Werror -D_GNU_SOURCE \
    -ffile-prefix-map="$ROOT"=. \
    -I"$ROOT/include" \
    -I"$GENERATED_DIR" \
    "$ROOT/src/bizar.c" "$ROOT/src/engines.c" "$ROOT/src/effects.c" \
    "$ROOT/src/filter.c" "$ROOT/src/modulation.c" "$ROOT/src/voice.c" "$ROOT/src/sequencer.c" \
    -lm -o "$DIST_DIR/dsp.so"
# zig leaves a .so.o next to the output on some versions
rm -f "$DIST_DIR/dsp.so.o"

cp "$ROOT/module.json" "$DIST_DIR/module.json"
cp "$ROOT/README.md" "$DIST_DIR/README.md"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$DIST_DIR/THIRD_PARTY_NOTICES.md"
cp "$ROOT/assets/wavetables/README.md" "$DIST_DIR/WAVETABLES.md"
cp "$ROOT/LICENSE" "$DIST_DIR/LICENSE"
cp "$ROOT/docs/PRESETS.md" "$DIST_DIR/PRESETS.md"
cp "$ROOT/web_ui.html" "$DIST_DIR/web_ui.html"
# Move's Remote UI cannot read preset_names from the module, so the shipped
# page carries the factory names (Bizar's bank plus any imported bank).
"$PYTHON" "$ROOT/scripts/embed_preset_names.py" "$GENERATED_DIR" "$DIST_DIR/web_ui.html"
cp "$GENERATED_DIR/wavetable-preview.js" "$DIST_DIR/wavetable-preview.js"
if [ -f "$ROOT/PARAMETER_REFERENCE.md" ]; then
    cp "$ROOT/PARAMETER_REFERENCE.md" "$DIST_DIR/PARAMETER_REFERENCE.md"
fi

# module.json is the single source of truth for the release version. The
# canonical tarball keeps the name the Module Catalog expects in asset_name;
# the versioned copy is the same bytes under an archival, attachable name.
VERSION=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/module.json" | head -n 1)
if [ -z "$VERSION" ]; then
    printf 'module.json has no "version" field\n' >&2
    exit 1
fi

# COPYFILE_DISABLE keeps macOS tar from adding ._ AppleDouble files.
if [ -n "$BANK" ]; then
    # A personal build: never under the release name.
    TARBALL="$ROOT/dist/bizar-module-personal.tar.gz"
    COPYFILE_DISABLE=1 tar -czf "$TARBALL" --exclude '.DS_Store' -C "$ROOT/dist" bizar
    printf 'Built %s (includes MiniFreak presets: personal use only, do not publish)\n' "$TARBALL"
else
    TARBALL="$ROOT/dist/bizar-module.tar.gz"
    VERSIONED="$ROOT/dist/bizar-module-$VERSION.tar.gz"
    COPYFILE_DISABLE=1 tar -czf "$TARBALL" --exclude '.DS_Store' -C "$ROOT/dist" bizar
    cp "$TARBALL" "$VERSIONED"
    printf 'Built %s\n' "$TARBALL"
    printf 'Built %s (version %s)\n' "$VERSIONED" "$VERSION"
fi

if command -v "$ZIG" >/dev/null 2>&1; then
    ZIG="$ZIG" "$PYTHON" "$ROOT/web/build.py"
    sh "$ROOT/scripts/pages.sh"   # dist/bizar-web.html as index.html on the local pages branch (push: scripts/pages.sh --push)
else
    printf 'zig not found: skipped dist/bizar-web.html (sh scripts/build.sh --web)\n'
fi
