#!/bin/sh
# Run the headless game with an input script and periodic screenshots, then tile
# the screenshots into a contact sheet. Usage:
#   port/scripts/play.sh [-s STATE] SCRIPT FRAMES OUTDIR [EVERY]
# SCRIPT is a TMC_INPUT file (port/scripts/play/*.txt). FRAMES counts from the
# start, or from the state. The save starts empty unless TMC_SAVE is set. Extra
# TMC_* variables pass through.
#
# -s STATE starts from a save state: a checkpoint name (port/states/NAME.tmcs,
# see port/scripts/checkpoints.sh) or a .tmcs path. If SCRIPT has a save:NAME
# step for it, the script carries on after that step.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
state=
if [ "$1" = "-s" ]; then
    state=$2
    shift 2
    case $state in
        */* | *.tmcs) ;;
        *) state=$root/port/states/$state.tmcs ;;
    esac
    [ -f "$state" ] || { echo "play.sh: no state $state (port/scripts/checkpoints.sh makes them)" >&2; exit 1; }
fi
script=$1 frames=$2 out=$3 every=${4:-300}
rm -rf "$out" && mkdir -p "$out"
save=${TMC_SAVE:-$out/tmc.sav}
[ -n "$TMC_SAVE" ] || rm -f "$save"
if [ -n "$state" ]; then
    export TMC_LOAD_STATE="$state"
    name=$(basename "$state" .tmcs)
    grep -qE "save:$name([^A-Za-z0-9_]|$)" "$script" && export TMC_INPUT_FROM="$name"
fi
status=0
TMC_SAVE=$save TMC_FRAMES=+$frames TMC_INPUT=@$script TMC_SCREENSHOT=every:$every TMC_SCREENSHOT_DIR=$out/shots \
    TMC_TRACE_PLAYER=${TMC_TRACE_PLAYER:-60} TMC_WATCHDOG=${TMC_WATCHDOG:-300} TMC_MAPDUMP=$out/maps \
    "${TMC_BIN:-$root/build/native/tmc}" > "$out/log.txt" 2>&1 || status=$?
tail -3 "$out/log.txt"
python3 "$here/contact_sheet.py" "$out/sheet.png" --cols 8 "$out"/shots/*.png
exit $status
