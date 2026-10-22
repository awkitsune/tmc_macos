#!/usr/bin/env python3
"""Take the ROM's bytes out of a linked TMC_BAKE_DATA=OFF program.

usage: scrub_rom_data.py BINARY ROM ROM_MANIFEST_S [--report]

datagen keeps the data/ tables out of the program, but the decomp also writes
tables as C initializers (src/*.c). Compiled natively, the pointer-free ones
are byte for byte what the ROM holds. The link moves __TEXT,__const into the
writable __DATA,__tmcconst; this pass zeroes every stretch of at least MIN_RUN
bytes there (and in __DATA,__data) that equals the ROM inside the cache's
extents, records each as (section, offset, ROM offset, length) in the reserved
__DATA,__tmcscrub table, and re-signs the file. port/src/romdata.c copies them
back from the extracted data at startup. Because only bytes equal to the ROM
are zeroed and the same ROM bytes are copied back, a coincidental match is
harmless; stretches of fewer than MIN_DISTINCT byte values are left alone.
"""

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

MIN_RUN = 16
MIN_DISTINCT = 4  # windows of fewer byte values (zeros, padding) match by chance and need no scrubbing
STRIDE = 16  # big extents are indexed every STRIDE bytes; matches >= 2*STRIDE-1 are found there
SMALL_EXTENT = 1 << 16  # smaller extents (the C tables) are indexed at every offset
SECTIONS = [("__DATA", "__tmcconst"), ("__DATA", "__data")]  # index = section number in the table
TABLE = ("__DATA", "__tmcscrub")
ENTRY = struct.Struct("<IIII")  # section, offset, ROM offset, length
HEADER = struct.Struct("<II")  # count, capacity


def sections(binary: Path) -> dict:
    """(segment, section) -> (file offset, size) for sections with contents."""
    out = subprocess.run(["otool", "-l", str(binary)], capture_output=True, text=True, check=True).stdout
    result = {}
    for block in out.split("Section\n")[1:]:
        fields = dict(re.findall(r"^\s*(\w+) (\S+)$", block, re.M))
        if "sectname" in fields:
            result[(fields["segname"], fields["sectname"])] = (int(fields["offset"]), int(fields["size"], 16))
    return result


def extents(manifest: Path) -> list:
    text = manifest.read_text()
    body = text.split("_gRomExtents:", 1)[1]
    return [(int(a, 16), int(b, 16)) for a, b in re.findall(r"\.long (0x[0-9a-f]+), (0x[0-9a-f]+),", body)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("binary", type=Path)
    ap.add_argument("rom", type=Path)
    ap.add_argument("manifest", type=Path, help="rom_manifest.s from datagen --no-bake")
    ap.add_argument("--report", action="store_true", help="only print what would be scrubbed")
    args = ap.parse_args()

    rom = args.rom.read_bytes()
    index = {}  # window -> (ROM offset, end of its extent)
    for start, length in extents(args.manifest):
        step = 1 if length <= SMALL_EXTENT else STRIDE
        for i in range(start, start + length - MIN_RUN + 1, step):
            window = rom[i : i + MIN_RUN]
            if len(set(window)) >= MIN_DISTINCT:
                index.setdefault(window, (i, start + length))

    data = bytearray(args.binary.read_bytes())
    secs = sections(args.binary)
    if TABLE not in secs:
        sys.exit(f"{args.binary}: no {TABLE[0]},{TABLE[1]} section (not a TMC_BAKE_DATA=OFF link?)")
    entries = []
    for number, name in enumerate(SECTIONS):
        if name not in secs:
            continue
        offset, size = secs[name]
        if offset == 0:
            continue  # zero-fill
        i = 0
        while i <= size - MIN_RUN:
            at = offset + i
            hit = index.get(bytes(data[at : at + MIN_RUN]))
            if hit is None:
                i += 1
                continue
            r, extent_end = hit
            n = MIN_RUN
            while i + n < size and r + n < extent_end and data[at + n] == rom[r + n]:
                n += 1
            entries.append((number, i, r, n))
            i += n
    table_offset, table_size = secs[TABLE]
    capacity = (table_size - HEADER.size) // ENTRY.size
    total = sum(e[3] for e in entries)
    print(f"{args.binary.name}: {len(entries)} stretches, {total} bytes equal to the ROM "
          + ", ".join(f"{s[1]} {sum(e[3] for e in entries if e[0] == k)}" for k, s in enumerate(SECTIONS)))
    if len(entries) > capacity:
        sys.exit(f"{len(entries)} stretches but room for {capacity}; raise the size in rom_scrub.s")
    if args.report:
        return 0
    for number, off, _, n in entries:
        start = secs[SECTIONS[number]][0] + off
        data[start : start + n] = bytes(n)
    table = HEADER.pack(len(entries), capacity) + b"".join(ENTRY.pack(*e) for e in entries)
    data[table_offset : table_offset + len(table)] = table
    args.binary.write_bytes(data)
    subprocess.run(["codesign", "--force", "--sign", "-", str(args.binary)], check=True, capture_output=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
