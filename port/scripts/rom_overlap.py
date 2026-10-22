#!/usr/bin/env python3
"""Find stretches of a ROM that also appear in a binary.

usage: rom_overlap.py BINARY ROM [--min BYTES] [--top N]

Checks that a TMC_BAKE_DATA=OFF build carries no ROM data. Every common
substring of at least --min bytes (default 64) is reported, merged and
attributed to the binary's section and nearest symbol. Stretches made of fewer
than 8 distinct byte values (zero fill, 0xff padding, simple patterns) are
skipped as noise. Exit status 1 if anything is found.
"""

import argparse
import bisect
import re
import subprocess
import sys
from pathlib import Path

WINDOW = 32  # ROM windows are indexed every WINDOW bytes, so matches >= 2*WINDOW-1 are found
MIN_DISTINCT = 8


def sections(binary: Path) -> list:
    """(file offset, size, vm address, name) of each section with file contents."""
    out = subprocess.run(["otool", "-l", str(binary)], capture_output=True, text=True, check=True).stdout
    result = []
    for block in out.split("Section\n")[1:]:
        fields = dict(re.findall(r"^\s*(\w+) (\S+)$", block, re.M))
        if "sectname" not in fields or int(fields.get("offset", "0")) == 0:
            continue
        result.append((int(fields["offset"]), int(fields["size"], 16), int(fields["addr"], 16),
                       f"{fields['segname']},{fields['sectname']}"))
    return sorted(result)


def symbols(binary: Path) -> tuple:
    out = subprocess.run(["nm", "-n", str(binary)], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms.append((int(parts[0], 16), parts[2]))
    return [a for a, _ in syms], [n for _, n in syms]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("binary", type=Path)
    ap.add_argument("rom", type=Path)
    ap.add_argument("--min", type=int, default=64)
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args()
    if args.min < 2 * WINDOW - 1:
        sys.exit(f"--min must be at least {2 * WINDOW - 1}")

    data = args.binary.read_bytes()
    rom = args.rom.read_bytes()
    index = {}
    for i in range(0, len(rom) - WINDOW + 1, WINDOW):
        w = rom[i : i + WINDOW]
        if len(set(w)) >= MIN_DISTINCT:
            index.setdefault(w, i)

    matches = []  # (binary start, binary end, rom start)
    covered = 0
    i = 0
    while i <= len(data) - WINDOW:
        if i < covered:
            i = covered
            continue
        rom_at = index.get(data[i : i + WINDOW])
        if rom_at is None:
            i += 1
            continue
        start, r = i, rom_at
        while start > covered and r > 0 and data[start - 1] == rom[r - 1]:
            start, r = start - 1, r - 1
        end, re_ = i + WINDOW, rom_at + WINDOW
        while end < len(data) and re_ < len(rom) and data[end] == rom[re_]:
            end, re_ = end + 1, re_ + 1
        if end - start >= args.min and len(set(data[start:end])) >= MIN_DISTINCT:
            matches.append((start, end, r))
        covered = end
        i = end

    secs = sections(args.binary)
    sym_addrs, sym_names = symbols(args.binary)
    total = sum(e - s for s, e, _ in matches)
    print(f"{args.binary}: {len(matches)} stretches of >= {args.min} bytes shared with {args.rom.name}, {total} bytes in all")
    by_section = {}
    for s, e, r in matches:
        sec = next((x for x in reversed(secs) if x[0] <= s < x[0] + x[1]), None)
        name = sec[3] if sec else "(headers)"
        by_section[name] = by_section.get(name, 0) + e - s
    for name, n in sorted(by_section.items(), key=lambda x: -x[1]):
        print(f"  {name}: {n} bytes")
    for s, e, r in sorted(matches, key=lambda m: m[0] - m[1])[: args.top]:
        sec = next((x for x in reversed(secs) if x[0] <= s < x[0] + x[1]), None)
        where = ""
        if sec is not None:
            addr = sec[2] + s - sec[0]
            k = bisect.bisect_right(sym_addrs, addr) - 1
            where = f"{sec[3]} {sym_names[k]}+{addr - sym_addrs[k]:#x}" if k >= 0 else sec[3]
        print(f"  {e - s:7d} bytes at file {s:#x} = ROM {r:#08x}  {where}")
    return 1 if matches else 0


if __name__ == "__main__":
    sys.exit(main())
