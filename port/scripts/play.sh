#!/bin/sh
# Run the headless game with an input script and periodic screenshots, then tile
# the screenshots into a contact sheet. Usage:
#   port/scripts/play.sh SCRIPT FRAMES OUTDIR [EVERY]
# SCRIPT is a TMC_INPUT file (port/scripts/play/*.txt). The save starts empty
# unless TMC_SAVE is set. Extra TMC_* variables pass through.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
script=$1 frames=$2 out=$3 every=${4:-300}
rm -rf "$out" && mkdir -p "$out"
save=${TMC_SAVE:-$out/tmc.sav}
[ -n "$TMC_SAVE" ] || rm -f "$save"
status=0
TMC_SAVE=$save TMC_FRAMES=$frames TMC_INPUT=@$script TMC_SCREENSHOT=every:$every TMC_SCREENSHOT_DIR=$out/shots \
    TMC_TRACE_PLAYER=${TMC_TRACE_PLAYER:-60} TMC_WATCHDOG=${TMC_WATCHDOG:-300} \
    "${TMC_BIN:-$root/build/native/tmc}" > "$out/log.txt" 2>&1 || status=$?
tail -3 "$out/log.txt"
python3 "$here/contact_sheet.py" "$out/sheet.png" --cols 8 "$out"/shots/*.png
exit $status
