#!/bin/sh
# Install the built module on a Move over SSH (macOS / Linux counterpart of
# install-windows.ps1).
#
# Usage: sh scripts/deploy.sh [--personal]
#   MOVE_HOST  default move.local (an ~/.ssh/config Host entry works too)
#   MOVE_USER  default: whatever ssh picks for MOVE_HOST
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARCHIVE="$ROOT/dist/bizar-module.tar.gz"
if [ "${1:-}" = "--personal" ]; then
    ARCHIVE="$ROOT/dist/bizar-module-personal.tar.gz"
fi
HOST=${MOVE_HOST:-move.local}
REMOTE=${MOVE_USER:+$MOVE_USER@}$HOST
MODULES=/data/UserData/schwung/modules/sound_generators
# Staged on /data, not /tmp: Move's root filesystem (and so /tmp) is nearly
# full, and staging on the module's own filesystem makes the swap a rename.
STAGE=/data/UserData/schwung/.bizar-deploy

if [ ! -f "$ARCHIVE" ]; then
    printf 'No %s: build first (sh scripts/build.sh)\n' "$ARCHIVE" >&2
    exit 1
fi

# -4: move.local also advertises IPv6 addresses, and ssh fails trying those first.
SSH_OPTS="-4 -o BatchMode=yes -o ConnectTimeout=10"

ssh $SSH_OPTS "$REMOTE" "rm -rf $STAGE && mkdir -p $STAGE $MODULES/bizar"
scp $SSH_OPTS "$ARCHIVE" "$REMOTE:$STAGE/bizar-module.tar.gz"

# Each file is renamed into place rather than overwritten: a running Schwung
# has dsp.so mapped, and truncating it in place would crash the audio thread.
# Files the archive does not carry (config.json, written by schwung-manager)
# are left alone.
ssh $SSH_OPTS "$REMOTE" "set -e
    tar -xzf $STAGE/bizar-module.tar.gz -C $STAGE
    for f in $STAGE/bizar/*; do mv -f \"\$f\" $MODULES/bizar/; done
    rm -rf $STAGE
    grep '\"version\"' $MODULES/bizar/module.json"

printf 'Installed %s to %s:%s/bizar\n' "$(basename "$ARCHIVE")" "$HOST" "$MODULES"
printf 'Rescan modules or restart Schwung on Move to load it.\n'
