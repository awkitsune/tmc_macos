#!/usr/bin/env python3
"""Count the ROM-data pointers a DSi build has to relocate at boot.

The GBA link normally drops relocations, so relink it with -q (--emit-relocs)
into a scratch ELF first; the ROM section comes out identical apart from the
header bytes gbafix sets afterwards:

    cd build/USA && arm-none-eabi-ld -n -q -T linker.ld -o /tmp/tmc_relocs.elf \
        -L ../../tools/agbcc/lib -lc
    python3 -I port/dsi/tools/reloc_report.py /tmp/tmc_relocs.elf build/USA/tmc.map [--raw]

Every R_ARM_ABS32 relocation is classified by where it sits (data object,
compiled C, assembly) and what it points at. For pointers inside data objects
(the bytes a DSi build reads from the ROM copy) it checks that each one can be
resolved at link time: ROM data to ROM data by adding the copy's base address,
everything else by a uniquely named global symbol plus addend.

--raw also scans the data objects for aligned words that hold a ROM address
without a relocation (pointers inside .incbin'd blobs, or coincidences).
Output is counts and symbol names only, never ROM bytes. Needs pyelftools.
"""

import argparse
import bisect
import collections
import re

from elftools.elf.elffile import ELFFile

MAP_LINE = re.compile(r"^ (\.[A-Za-z_.0-9]+)\s+0x(0[89][0-9a-f]{6})\s+0x([0-9a-f]+)\s+(\S+)")
REGIONS = {2: "EWRAM", 3: "IWRAM", 4: "IO", 5: "PLTT", 6: "VRAM", 7: "OAM", 0: "BIOS/zero"}


def load_sections(map_path):
    sections = []
    for line in open(map_path):
        match = MAP_LINE.match(line)
        if match and int(match.group(3), 16):
            sections.append((int(match.group(2), 16), int(match.group(3), 16), match.group(1), match.group(4)))
    sections.sort()
    return sections


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("elf")
    parser.add_argument("map")
    parser.add_argument("--raw", action="store_true")
    args = parser.parse_args()

    sections = load_sections(args.map)
    starts = [s[0] for s in sections]

    def section_at(address):
        i = bisect.bisect_right(starts, address) - 1
        if i >= 0 and sections[i][0] <= address < sections[i][0] + sections[i][1]:
            return sections[i]
        return None

    def kind(section):
        if section is None:
            return "unmapped"
        name, obj = section[2], section[3]
        if obj.startswith("data/"):
            return "data"
        origin = "C" if obj.startswith("src/") else "asm"
        return origin + (".code" if name == ".text" else ".rodata")

    elf = ELFFile(open(args.elf, "rb"))
    rom = elf.get_section_by_name("rom")
    rom_bytes, base = rom.data(), rom["sh_addr"]
    end = base + len(rom_bytes)

    def word(address):
        return int.from_bytes(rom_bytes[address - base : address - base + 4], "little")

    symbols = collections.defaultdict(list)
    name_count = collections.Counter()
    for sym in elf.get_section_by_name(".symtab").iter_symbols():
        if not sym.name or sym.name.startswith("$"):
            continue
        name_count[sym.name] += 1
        if sym["st_info"]["type"] in ("STT_FUNC", "STT_OBJECT", "STT_NOTYPE"):
            symbols[sym["st_value"] & ~1].append((sym["st_info"]["bind"] == "STB_GLOBAL", sym.name))
    addresses = sorted(symbols)

    by_type = collections.Counter()
    by_place = collections.Counter()
    data_needs = collections.Counter()
    data_objects = collections.Counter()
    data_offsets = []
    reloc_offsets = set()
    for reloc in elf.get_section_by_name(".relrom").iter_relocations():
        by_type[reloc["r_info_type"]] += 1
        reloc_offsets.add(reloc["r_offset"])
        if reloc["r_info_type"] != 2:  # R_ARM_ABS32
            continue
        offset = reloc["r_offset"]
        value = word(offset)
        where = kind(section_at(offset))
        region = value >> 24
        target = "ROM:" + kind(section_at(value & ~1)) if region in (8, 9) else REGIONS.get(region, "other")
        by_place[(where, target)] += 1
        if where != "data":
            continue
        data_offsets.append(offset)
        data_objects[section_at(offset)[3]] += 1
        if target == "ROM:data":
            data_needs["ROM data -> ROM data (add copy base)"] += 1
            continue
        exact = symbols.get(value & ~1)
        if exact:
            global_names = [n for g, n in exact if g] or [n for g, n in exact]
            unique = name_count[global_names[0]] == 1
            data_needs["%s -> symbol%s" % (target, "" if unique else " (NAME NOT UNIQUE)")] += 1
        else:
            i = bisect.bisect_right(addresses, value & ~1) - 1
            data_needs["%s -> symbol+addend (%s)" % (target, symbols[addresses[i]][0][1] if i >= 0 else "?")] += 1

    print("relocation types:", dict(by_type), "(2 = R_ARM_ABS32)")
    print("\nR_ARM_ABS32 by location -> target:")
    for (where, target), count in sorted(by_place.items(), key=lambda item: -item[1]):
        print("%7d  %-10s -> %s" % (count, where, target))
    print("\npointers inside data objects: %d" % len(data_offsets))
    for need, count in sorted(data_needs.items(), key=lambda item: -item[1]):
        print("%7d  %s" % (count, need))
    print("top data objects:", data_objects.most_common(6))
    data_offsets.sort()
    gaps = [b - a for a, b in zip(data_offsets, data_offsets[1:])]
    print("gaps between data pointers: %d < 256 B, %d < 64 KB (of %d)"
          % (sum(g < 256 for g in gaps), sum(g < 65536 for g in gaps), len(gaps)))

    sizes = collections.Counter()
    for section in sections:
        sizes[kind(section)] += section[1]
    print("\nROM bytes by kind:", dict(sizes), "end 0x%08x" % end)

    if args.raw:
        hits = collections.Counter()
        for address, size, _, obj in sections:
            if not obj.startswith("data/"):
                continue
            for offset in range((address + 3) & ~3, address + size - 3, 4):
                if offset in reloc_offsets:
                    continue
                value = word(offset)
                if base <= value < end:
                    hits["symbol start" if (value & ~1) in symbols else "elsewhere"] += 1
                    if (value & ~1) in symbols:
                        hits[obj] += 1
        print("\nunrelocated ROM-address words in data objects:")
        for what, count in hits.most_common(12):
            print("%7d  %s" % (count, what))


if __name__ == "__main__":
    main()
