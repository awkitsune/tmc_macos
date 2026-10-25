#!/bin/sh
# Write the checkpoints along port/scripts/play/01_new_game.txt (its save:NAME
# steps) as save states in port/states (or DIR), for play.sh -s NAME,
# TMC_LOAD_STATE and the app's File > Load State. Usage:
#   port/scripts/checkpoints.sh [DIR]
# Their pointer lists are exact (port/src/state.c), so they load into other
# builds (the app, a rebuilt tmc) too. Takes a few seconds; rerun after changing
# the game. TMC_BIN picks the program (default build/native/tmc).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/port/states}
bin=${TMC_BIN:-$root/build/native/tmc}
script=$here/play/01_new_game.txt
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$out"
if ! TMC_SAVE="$tmp/states.sav" TMC_FRAMES=26000 TMC_INPUT=@"$script" TMC_WATCHDOG=300 TMC_STATE_DIR="$out" \
    "$bin" > "$tmp/states.log" 2>&1; then
    tail -5 "$tmp/states.log"
    echo "checkpoints: the run failed" >&2
    exit 1
fi
grep 'saved state' "$tmp/states.log"
