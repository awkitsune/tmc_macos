"""Summarise a room smoke test run (port/scripts/room_smoke.sh).

usage: room_smoke_report.py OUT

Reads OUT/rooms.txt and OUT/logs/AA_RR.log, writes OUT/results.tsv (one row per
room) and OUT/summary.txt (counts, and the failures grouped by the function they
happened in), and prints the summary.

Outcomes: ok; crash (signal, and the first game function in the backtrace);
rom_write (a store into ROM data, logged and skipped by TMC_ROM_WRITES; the run
went on); hang (TMC_WATCHDOG fired); script (the runner exited with status 3);
error (any other exit). reached: the run ended (or crashed) in the room it
warped to; "other" means the game put the player somewhere else (a story
redirect such as Hyrule Town to Festival Town, or an exit taken while playing).
"""

import collections
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SKIP = {"ReportAndExit", "OnCrash", "_sigtramp", "OnWatchdog"}


def room_names():
    """(area, room) -> "AREA_X/ROOM_Y" from include/area.h and include/roomid.h."""
    area_h = open(os.path.join(ROOT, "include", "area.h")).read()
    body = re.search(r"typedef enum \{\s*(AREA_MINISH_WOODS.*?)\}", area_h, re.S).group(1)
    areas = [line.split("//")[0].strip().rstrip(",") for line in body.splitlines()]
    areas = [a for a in areas if a]
    rooms, current = {}, None
    for line in open(os.path.join(ROOT, "include", "roomid.h")).read().splitlines():
        line = line.strip()
        m = re.fullmatch(r"//\s*(AREA_\w+)", line)
        if m:
            current = m.group(1)
            rooms[current] = []
        elif current and (m := re.match(r"(ROOM_\w+)", line)):
            rooms[current].append(m.group(1))
    names = {}
    for a, area in enumerate(areas):
        for r, room in enumerate(rooms.get(area, [])):
            names[(a, r)] = room[len("ROOM_"):]
        names[(a, None)] = area[len("AREA_"):]
    return names


def parse(log, area, room):
    text = open(log, errors="replace").read()
    lines = text.splitlines()
    exit_code = int(re.findall(r"^exit (\d+)$", text, re.M)[-1]) if re.search(r"^exit \d+$", text, re.M) else -1
    end = re.search(r"^warp: end frame (\d+) area (\w+) room (\w+)", text, re.M)
    crashed_in = re.search(r"^warp: crashed in area (\w+) room (\w+)", text, re.M)
    signal = re.search(r"^(SIGSEGV|SIGBUS|signal|watchdog) at (\S+), frame (\d+)", text, re.M)
    first = re.search(r"^frame (\d+): loaded state", text, re.M)
    arrived = re.search(r"^frame (\d+): warp arrived", text, re.M)
    rom = re.findall(r"^rom write: (\S+) from (\S+), frame (\d+)", text, re.M)

    where, frame, detail, func = None, None, "", ""
    if end:
        frame, where = int(end.group(1)), (int(end.group(2), 16), int(end.group(3), 16))
    if signal:
        frame = int(signal.group(3))
        if crashed_in:
            where = (int(crashed_in.group(1), 16), int(crashed_in.group(2), 16))
        # backtrace_symbols_fd lines: "N   tmc   0xADDR Symbol + off"
        stack = [m.group(1) for m in re.finditer(r"^\d+\s+\S+\s+0x[0-9a-f]+ (\S+) \+ \d+$", text, re.M)]
        stack = [s for s in stack if s not in SKIP]
        func = stack[0] if stack else "?"
        detail = f"{signal.group(1)} at {signal.group(2)}; " + " < ".join(stack[:4])
    if signal and signal.group(1) == "watchdog":
        outcome = "hang"
    elif signal:
        outcome = "crash"
    elif exit_code == 0 and rom:
        outcome = "rom_write"
        func = rom[0][1].split("+")[0]
        detail = "; ".join(f"{t} from {f}" for t, f, _ in rom)
    elif exit_code == 0:
        outcome = "ok"
    elif exit_code == 3:
        outcome = "script"
        detail = next((l for l in lines if l.startswith("TMC_INPUT")), "")
    else:
        outcome = "error"
        detail = next((l for l in lines if l.strip() and not l.startswith("frame ")), f"exit {exit_code}")
    if rom and outcome != "rom_write":
        detail += " | rom writes: " + "; ".join(f"{t} from {f}" for t, f, _ in rom)
    frames = frame - int(first.group(1)) if frame is not None and first else ""
    if where is None:
        reached = "unknown"
    elif where == (area, room):
        reached = "yes"
    else:
        reached = "other"
    return {
        "outcome": outcome,
        "func": func,
        "reached": reached,
        "arrived": "yes" if arrived else "no",
        "end": f"{where[0]:02x}:{where[1]:02x}" if where else "",
        "frames": frames,
        "detail": detail,
        "rom": rom,
    }


def main():
    out = sys.argv[1]
    names = room_names()
    rooms = [line.strip() for line in open(os.path.join(out, "rooms.txt")) if line.strip()]
    rows = []
    for spec in rooms:
        area, room = (int(x, 16) for x in spec.split(":")[:2])
        log = os.path.join(out, "logs", spec.replace(":", "_") + ".log")
        r = parse(log, area, room) if os.path.exists(log) else {
            "outcome": "error", "func": "", "reached": "unknown", "arrived": "no", "end": "", "frames": "",
            "detail": "no log", "rom": []}
        r["room"] = f"{area:02x}:{room:02x}"
        r["name"] = names.get((area, room), names.get((area, None), "?"))
        rows.append(r)

    cols = ["room", "name", "outcome", "func", "reached", "arrived", "end", "frames", "detail"]
    with open(os.path.join(out, "results.tsv"), "w") as f:
        f.write("\t".join(cols) + "\n")
        for r in rows:
            f.write("\t".join(str(r[c]) for c in cols) + "\n")

    counts = collections.Counter(r["outcome"] for r in rows)
    reached = collections.Counter(r["reached"] for r in rows)
    lines = [
        f"rooms: {len(rows)}",
        "outcomes: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())),
        "reached (ended in the warped-to room): " + ", ".join(f"{k} {v}" for k, v in sorted(reached.items())),
        f"warp arrived with control: {sum(r['arrived'] == 'yes' for r in rows)}",
        "",
    ]
    groups = collections.defaultdict(list)
    for r in rows:
        if r["outcome"] != "ok":
            groups[(r["outcome"], r["func"])].append(r)
        for target, func, _ in r["rom"]:
            if r["outcome"] not in ("rom_write",):
                groups[("rom_write (also)", func.split("+")[0])].append(r)
    for (outcome, func), members in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        lines.append(f"{outcome} {func or '-'}: {len(members)} rooms")
        lines.append("    e.g. " + members[0]["detail"][:300])
        lines.append("    " + " ".join(f"{m['room']}({m['name']})" for m in members[:40]))
        if len(members) > 40:
            lines.append(f"    ... and {len(members) - 40} more")
    text = "\n".join(lines) + "\n"
    open(os.path.join(out, "summary.txt"), "w").write(text)
    print(text, end="")


if __name__ == "__main__":
    main()
