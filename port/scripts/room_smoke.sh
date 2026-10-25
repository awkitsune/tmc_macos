#!/bin/sh
# Room smoke test: warp into every room the game has (TMC_WARP, port/src/warp.c),
# play a few hundred frames of walking, sword, item and A presses there, and
# report crashes, writes into ROM data and hangs. Usage:
#   port/scripts/room_smoke.sh [OUT] [AA:RR ...]
# OUT (default build/room_smoke) gets one log per room in logs/, results.tsv
# (one row per room) and summary.txt (the failures grouped by function). Rooms
# default to every valid one in the game's room tables (room_smoke_rooms.py,
# which writes the others with the reason to skipped.txt). Environment:
#   TMC_BIN    the runner (default build/native/tmc)
#   STATE      the state to start from (default port/states/hyrule_field_north.tmcs)
#   FRAMES     frames after the warp starts (default 720: the warp takes ~40)
#   JOBS       runs at a time (default 4)
#   WATCHDOG   seconds before a run counts as hung (default 30)
#   ITEMS      A,B: item ids (include/item.h) to equip on A and B at the warp
#              (TMC_WARP_ITEMS), with bombs and arrows; default: the state's own
# TMC_ROM_WRITES is on (arm64 only), so a ROM write is logged and skipped
# rather than ending the run.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/build/room_smoke}
[ $# -gt 0 ] && shift
bin=${TMC_BIN:-$root/build/native/tmc}
state=${STATE:-$root/port/states/hyrule_field_north.tmcs}
frames=${FRAMES:-720}
jobs=${JOBS:-4}
watchdog=${WATCHDOG:-30}

mkdir -p "$out/logs"
rm -f "$out"/logs/*.log
if [ $# -gt 0 ]; then
    printf '%s\n' "$@" > "$out/rooms.txt"
else
    python3 "$here/room_smoke_rooms.py" "$out/rooms.txt" "$out/skipped.txt"
fi

# Light activity, starting once the warp is under way: walk a square, swing the
# sword (B), use the R item and press A (talk, lift, read) between the legs.
input="+60:right:24"
i=0
while [ $i -lt 8 ]; do
    input="$input,+24:a:2,+4:down:24,+24:b:6,+8:left:24,+24:a:2,+4:up:24,+24:r:6,+8:b:20,+22:right:24"
    i=$((i + 1))
done

export bin state frames watchdog input out
[ -n "$ITEMS" ] && export TMC_WARP_ITEMS="$ITEMS"
xargs -P "$jobs" -n 1 sh -c '
    log="$out/logs/$(echo "$0" | tr : _).log"
    TMC_LOAD_STATE="$state" TMC_WARP="$0" TMC_FRAMES=+$frames TMC_INPUT="$input" TMC_ROM_WRITES=1 \
        TMC_WATCHDOG=$watchdog "$bin" > "$log" 2>&1
    echo "exit $?" >> "$log"
' < "$out/rooms.txt"

python3 "$here/room_smoke_report.py" "$out"
