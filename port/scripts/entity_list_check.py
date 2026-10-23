#!/usr/bin/env python3
"""Check that every room's entity lists read natively as they do on the GBA.

Runs the headless game with TMC_CHECK_ENTITY_LISTS (port/src/entitylistcheck.c),
which walks each room's entity lists through the native EntityData layout, and
walks the same lists in tmc.gba with 16-byte GBA entries. Every list must be
found by both, with the same number of entries before the 0xFF terminator, and
every entry's kind must be 1-9. Exits with 1 on any difference.

usage: port/scripts/entity_list_check.py [--tmc build/native/tmc] [--native-list FILE]
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "port/tools"))

from datagen.entitylists import TERMINATOR, consumed_properties  # noqa: E402
from datagen.gbaobj import linked_symbols  # noqa: E402

ROM_BASE = 0x08000000
AREA_TABLE_SIZE = 0x90
MAX_ROOMS = 64
MAX_ENTRIES = 256
GBA_ENTRY = 16


def gba_lists(rom: bytes, symbols: dict) -> set:
    """('list', area, room, property, entries) and 'bad'/'unterminated' lines, from the ROM."""

    def word(addr):
        return struct.unpack_from("<I", rom, addr - ROM_BASE)[0]

    out = set()
    for area in range(AREA_TABLE_SIZE):
        rooms = word(symbols["gAreaTable"] + 4 * area)
        headers = word(symbols["gAreaRoomHeaders"] + 4 * area)
        if not rooms or not headers:
            continue
        for room in range(MAX_ROOMS):
            if struct.unpack_from("<H", rom, headers + 10 * room - ROM_BASE)[0] == 0xFFFF:
                break
            props = word(rooms + 4 * room)
            if not props:
                continue
            queued, work = {0, 1, 2}, [0, 1, 2]
            while work:
                p = work.pop()
                addr = word(props + 4 * p)
                if not addr:
                    continue
                i = 0
                while i < MAX_ENTRIES:
                    entry = rom[addr + GBA_ENTRY * i - ROM_BASE :][:GBA_ENTRY]
                    if entry[0] == TERMINATOR:
                        break
                    if not 1 <= (entry[0] & 0xF) <= 9:
                        out.add(("bad", area, room, p, i, entry[0]))
                    for nxt in consumed_properties(entry):
                        if nxt < 0x80 and nxt not in queued:
                            queued.add(nxt)
                            work.append(nxt)
                    i += 1
                out.add(("unterminated", area, room, p) if i == MAX_ENTRIES else ("list", area, room, p, i))
    return out


def parse(text: str) -> set:
    return {(f[0], *map(int, f[1:])) for f in (line.split() for line in text.splitlines()) if f}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tmc", type=Path, default=ROOT / "build/native/tmc")
    ap.add_argument("--native-list", type=Path, help="use this TMC_CHECK_ENTITY_LISTS output instead of running tmc")
    args = ap.parse_args()

    if args.native_list:
        native = parse(args.native_list.read_text())
    else:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "lists.txt"
            env = dict(os.environ, TMC_CHECK_ENTITY_LISTS=str(out), TMC_FRAMES="1", TMC_SAVE=str(Path(tmp) / "tmc.sav"))
            subprocess.run([str(args.tmc)], env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            native = parse(out.read_text())
    gba = gba_lists((ROOT / "tmc.gba").read_bytes(), linked_symbols(ROOT / "tmc.elf"))

    problems = sorted(x for x in native | gba if x[0] != "list")
    only_native, only_gba = sorted(native - gba), sorted(gba - native)
    for x in problems:
        print("native" if x in native else "gba", *x)
    for x in only_native:
        if x[0] == "list":
            print("native only:", *x)
    for x in only_gba:
        if x[0] == "list":
            print("gba only:", *x)
    lists = sum(1 for x in gba if x[0] == "list")
    rooms = len({x[1:3] for x in gba})
    if problems or only_native or only_gba:
        print(f"entity lists differ: {len(only_native)} native-only, {len(only_gba)} GBA-only lines, {len(problems)} bad")
        return 1
    print(f"{lists} entity lists in {rooms} rooms: native and GBA agree, every entry kind 1-9")
    return 0


if __name__ == "__main__":
    sys.exit(main())
