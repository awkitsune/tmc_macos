#!/usr/bin/env python3
"""Generate the DSi build's link inputs from the GBA build.

    python3 -I port/dsi/tools/gen_link.py --relocs-elf RELINK.elf --map build/USA/tmc.map \
        --linker-ld build/USA/linker.ld --native-defs DEFS.txt --raw-pointers RAW.txt --out build/dsi/gen

RELINK.elf is the GBA link redone with -q (--emit-relocs), see port/dsi/README.md.
DEFS.txt lists the global symbols the DSi objects define (one per line, from nm):
the binary's own definitions win over the ROM copy's. RAW.txt lists the pointer
fields of data objects that hold a raw GBA address with no relocation, such as a
tileset's VRAM destination (port/tools/datagen --raw-pointers: ROM address and
value per line); the hardware ones go to the shadows, as on the Mac.

Writes into --out:

- rom_symbols.ld: the ROM copy's address (__rom_copy) and every global symbol
  defined in a GBA data object (data/**) as __rom_copy plus its offset in the
  copy, so game code reaches ROM data in the copy with no source changes. The
  copy holds only the data objects: their ROM stretches (extents), merged where
  they touch and packed in ROM order, each keeping its ROM address modulo 32;
- ram_layout.ld: the GBA EWRAM symbols at their GBA addresses (the binary is
  linked at 0x02040000, leaving 0x02000000-0x0203FFFF to them) and the IWRAM
  symbols in gDsiIwram, a 32 KB block in main RAM;
- rom_relocs.s: the extents (ROM offset, length, offset in the copy) and their
  version, which tmc/gamedata.bin records; the pointer words inside data
  objects that the boot code patches in the copy (port/dsi/src/rom.c), the
  EWRAM ones it only checks, and the expected counts per kind;
- gen_report.txt: the figures.

Everything written is offsets and symbol names from the link; no ROM bytes. The
plan is port/dsi/PLAN.md ("ROM loading and relocation at boot"). Needs pyelftools.
"""

import argparse
import bisect
import collections
import hashlib
import os
import struct
import re
import sys

from elftools.elf.elffile import ELFFile

ROM_BASE = 0x08000000
EWRAM_BASE, EWRAM_SIZE = 0x02000000, 0x40000
IWRAM_BASE, IWRAM_SIZE = 0x03000000, 0x8000
# GBA hardware regions and their shadows (port/include/port/hw.h).
HW_SHADOWS = {0x04: ("gHwIo", 0x400, "I/O"), 0x05: ("gHwPltt", 0x400, "palette"),
              0x06: ("gHwVram", 0x18000, "VRAM"), 0x07: ("gHwOam", 0x400, "OAM")}
# The ROM copy sits at the top of main RAM, below the 48 KB libnds keeps
# (DTCM and the loader's area from 0x02FF4000).
RAM_TOP = 0x02FF4000
R_ARM_ABS32 = 2
# Each extent keeps its ROM address modulo this in the copy (DMA and cache lines).
EXTENT_ALIGN = 32

# Data tables inside asm code objects, as (first label, end label); the same
# list as datagen's ASM_DATA_REGIONS (port/tools/datagen/gbaobj.py). Their
# symbols and pointers are handled like those of data objects.
ASM_DATA_REGIONS = [
    ("gUnk_08001A7C", "GetFuserId"),
    ("gUnk_0800275C", "GetNextFunction"),
]

MAP_LINE = re.compile(r"^ (\.[A-Za-z_.0-9]+)\s+0x(0[89][0-9a-f]{6})\s+0x([0-9a-f]+)\s+(\S+)")
RAM_SYMBOL = re.compile(r"^\s*\. = 0x([0-9A-Fa-f]+); (\w+) = \.;")
ABS_SYMBOL = re.compile(r"^(\w+) = (0x[0-9A-Fa-f]+|\d+);")


def load_sections(map_path):
    """(address, size, section, object) for every non-empty ROM input section."""
    sections = []
    for line in open(map_path):
        match = MAP_LINE.match(line)
        if match and int(match.group(3), 16):
            sections.append((int(match.group(2), 16), int(match.group(3), 16), match.group(1), match.group(4)))
    sections.sort()
    return sections


def load_ram_symbols(linker_ld):
    """{region: [(offset, name)]} from the preprocessed GBA linker script, plus absolute symbols."""
    regions = {"ewram": [], "iwram": []}
    absolute = []
    region = None
    for line in open(linker_ld):
        stripped = line.strip()
        if stripped.startswith("ewram (NOLOAD)"):
            region = "ewram"
        elif stripped.startswith("iwram (NOLOAD)"):
            region = "iwram"
        elif stripped.startswith("}"):
            region = None
        elif region:
            match = RAM_SYMBOL.match(line)
            if match:
                regions[region].append((int(match.group(1), 16), match.group(2)))
        else:
            match = ABS_SYMBOL.match(stripped)
            if match:
                absolute.append((match.group(1), int(match.group(2), 0)))
    return regions, absolute


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--relocs-elf", required=True)
    parser.add_argument("--map", required=True)
    parser.add_argument("--linker-ld", required=True)
    parser.add_argument("--native-defs", required=True)
    parser.add_argument("--raw-pointers", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    sections = load_sections(args.map)
    starts = [s[0] for s in sections]
    native = set(line.strip() for line in open(args.native_defs) if line.strip())

    def section_at(address):
        i = bisect.bisect_right(starts, address) - 1
        if i >= 0 and sections[i][0] <= address < sections[i][0] + sections[i][1]:
            return sections[i]
        return None

    data_regions = []

    def is_data(address):
        section = section_at(address)
        if section is not None and section[3].startswith("data/"):
            return True
        return any(start <= address < end for start, end in data_regions)

    elf = ELFFile(open(args.relocs_elf, "rb"))
    rom = elf.get_section_by_name("rom")
    rom_bytes = rom.data()
    if rom["sh_addr"] != ROM_BASE:
        sys.exit("unexpected ROM section address 0x%08x" % rom["sh_addr"])

    def word(address):
        offset = address - ROM_BASE
        return int.from_bytes(rom_bytes[offset : offset + 4], "little")

    # Names at each address (globals first) and their types.
    by_address = collections.defaultdict(list)
    sym_type = {}
    name_count = collections.Counter()
    for sym in elf.get_section_by_name(".symtab").iter_symbols():
        if not sym.name or sym.name.startswith("$") or sym["st_shndx"] == "SHN_UNDEF":
            continue
        name_count[sym.name] += 1
        if sym["st_info"]["type"] not in ("STT_FUNC", "STT_OBJECT", "STT_NOTYPE"):
            continue
        is_global = sym["st_info"]["bind"] == "STB_GLOBAL"
        # Thumb functions have bit 0 set; data symbols can be at odd addresses.
        is_func = sym["st_info"]["type"] == "STT_FUNC"
        by_address[sym["st_value"] & ~1 if is_func else sym["st_value"]].append((not is_global, sym.name))
        sym_type[sym.name] = sym["st_info"]["type"]
    for names in by_address.values():
        names.sort()
    addresses = sorted(by_address)

    errors = []
    report = []

    values = {sym.name: sym["st_value"] for sym in elf.get_section_by_name(".symtab").iter_symbols() if sym.name}
    for first, end in ASM_DATA_REGIONS:
        data_regions.append((values[first], values[end] & ~1))

    # ---- extents: the data objects' ROM stretches, packed into the copy ----------
    stretches = sorted([(a, a + s) for a, s, _, o in sections if o.startswith("data/")] + data_regions)
    extents = []  # [rom start, rom end, copy offset]
    for start, end in stretches:
        if extents and start <= extents[-1][1]:
            extents[-1][1] = max(extents[-1][1], end)
        else:
            extents.append([start, end, 0])
    copy_size = 0
    for extent in extents:
        copy_size += (extent[0] - copy_size) % EXTENT_ALIGN
        extent[2] = copy_size
        copy_size += extent[1] - extent[0]
    copy_size = (copy_size + 3) & ~3
    rom_copy = (RAM_TOP - copy_size) & ~0xFFF
    extent_starts = [e[0] for e in extents]
    # What tmc/gamedata.bin holds depends on the extents only.
    layout = hashlib.sha1(b"".join(struct.pack("<3I", e[0] - ROM_BASE, e[1] - e[0], e[2]) for e in extents))
    layout_version = struct.unpack("<2I", layout.digest()[:8])

    def copy_offset(address, size=1):
        """The copy offset of ROM address..address+size, or None if not all in one extent."""
        i = bisect.bisect_right(extent_starts, address) - 1
        if i >= 0 and address + size <= extents[i][1]:
            return extents[i][2] + address - extents[i][0]
        return None

    def global_names(address):
        return [name for local, name in by_address.get(address, []) if not local and name_count[name] == 1]

    # ---- ROM data symbols ------------------------------------------------------
    rom_symbols = []
    for address in addresses:
        if not (ROM_BASE <= address < ROM_BASE + 0x02000000) or not is_data(address):
            continue
        for name in global_names(address):
            if name in native:
                continue
            offset = copy_offset(address, 0)
            if offset is None:
                errors.append("%s at 0x%08X: not in an extent" % (name, address))
                continue
            rom_symbols.append((name, offset))
    rom_symbols.sort(key=lambda item: (item[1], item[0]))

    with open(os.path.join(args.out, "rom_symbols.ld"), "w") as out:
        out.write("/* Generated by port/dsi/tools/gen_link.py; do not edit. */\n")
        out.write("/* The GBA ROM's data objects in %d extents, loaded into RAM at boot. */\n" % len(extents))
        out.write("__rom_copy = 0x%08X;\n" % rom_copy)
        out.write("__rom_copy_size = 0x%X;\n" % copy_size)
        out.write("__rom_copy_end = __rom_copy + __rom_copy_size;\n")
        for name, offset in rom_symbols:
            out.write("PROVIDE(%s = __rom_copy + 0x%06X);\n" % (name, offset))

    # ---- RAM layout --------------------------------------------------------------
    regions, absolute = load_ram_symbols(args.linker_ld)
    ram_native = []
    with open(os.path.join(args.out, "ram_layout.ld"), "w") as out:
        out.write("/* Generated by port/dsi/tools/gen_link.py; do not edit. */\n")
        out.write("/* GBA EWRAM at its own addresses (0x%08X, %d KB, below the binary). */\n"
                  % (EWRAM_BASE, EWRAM_SIZE // 1024))
        for offset, name in regions["ewram"]:
            if name in native:
                ram_native.append(name)
            out.write("PROVIDE(%s = 0x%08X);\n" % (name, EWRAM_BASE + offset))
        # PORT_HOSTED names for what the GBA addresses directly (save.h: the
        # save header at the start of EWRAM).
        out.write("PROVIDE(gNativeSaveHeader = 0x%08X);\n" % EWRAM_BASE)
        out.write("/* GBA IWRAM in gDsiIwram (port/dsi/src/hw_dsi.c). The ram_* routines the\n"
                  "   GBA copies there are C functions in the binary. */\n")
        for offset, name in regions["iwram"]:
            if name.startswith("ram_") or name.endswith("_stack_begin"):
                continue
            if name in native:
                ram_native.append(name)
            out.write("PROVIDE(%s = gDsiIwram + 0x%04X);\n" % (name, offset))
        for name, value in absolute:
            out.write("PROVIDE(%s = 0x%X);\n" % (name, value))
        out.write("ASSERT(ORIGIN(ewram) >= 0x%08X, \"the binary must start above the GBA EWRAM block\")\n"
                  % (EWRAM_BASE + EWRAM_SIZE))
        out.write("ASSERT(__end__ <= __rom_copy, \"the binary overlaps the ROM copy\")\n")
    if ram_native:
        errors.append("GBA RAM symbols also defined by the DSi objects (they would move): "
                      + ", ".join(sorted(ram_native)))

    # ---- relocations -------------------------------------------------------------
    adds = []
    kept = []
    symbol_entries = []
    kinds = collections.Counter()
    other_types = collections.Counter()
    unaligned = 0
    for reloc in elf.get_section_by_name(".relrom").iter_relocations():
        offset = reloc["r_offset"]
        if not is_data(offset):
            continue  # code and C rodata are compiled for the ARM9; their copies are unused
        if reloc["r_info_type"] != R_ARM_ABS32:
            other_types[reloc["r_info_type"]] += 1
            continue
        if copy_offset(offset, 4) is None:
            errors.append("pointer word at 0x%08X: not inside one extent" % offset)
            continue
        if offset % 4:
            unaligned += 1
        value = word(offset)
        region = value >> 24
        # A pointer with bit 0 set is a Thumb function or a byte-aligned datum.
        target = value
        names = global_names(value)
        if value & 1 and not names and any(sym_type[n] == "STT_FUNC" for n in global_names(value & ~1)):
            target = value & ~1
            names = global_names(target)
        native_names = [n for n in names if n in native]
        if region in (8, 9) and native_names:
            symbol_entries.append((offset, native_names[0], value - target if sym_type[native_names[0]] != "STT_FUNC" else 0))
            kinds["to a DSi definition (" + ("code" if sym_type[native_names[0]] == "STT_FUNC" else "data") + ")"] += 1
        elif region in (8, 9) and is_data(target):
            if copy_offset(value, 0) is None:
                errors.append("0x%08X -> 0x%08X: not in an extent" % (offset, value))
            adds.append(offset)
            kinds["ROM data (add copy base)"] += 1
        elif region in (8, 9):
            i = bisect.bisect_right(addresses, target) - 1
            exact = names
            if exact:
                name = exact[0]
                addend = 0 if sym_type[name] == "STT_FUNC" else value - target
            else:
                candidates = global_names(addresses[i]) if i >= 0 else []
                if not candidates:
                    errors.append("0x%08X -> 0x%08X: no global symbol" % (offset, value))
                    continue
                name = candidates[0]
                if sym_type[name] == "STT_FUNC":
                    errors.append("0x%08X -> 0x%08X: inside function %s" % (offset, value, name))
                    continue
                addend = value - addresses[i]
            if name not in native:
                errors.append("0x%08X -> %s: not defined by the DSi objects" % (offset, name))
            section = section_at(target)
            kind = "code" if section and section[2] == ".text" else "rodata"
            kinds["to a DSi definition (%s)" % kind] += 1
            symbol_entries.append((offset, name, addend))
        elif region == 2:
            if not (EWRAM_BASE <= value < EWRAM_BASE + EWRAM_SIZE):
                errors.append("0x%08X -> 0x%08X: outside EWRAM" % (offset, value))
            kept.append(offset)
            kinds["EWRAM (kept: same address)"] += 1
        elif region == 3:
            symbol_entries.append((offset, "gDsiIwram", value - IWRAM_BASE))
            kinds["IWRAM (into gDsiIwram)"] += 1
        else:
            errors.append("0x%08X -> 0x%08X: unexpected target" % (offset, value))
    # Raw GBA addresses in pointer fields (datagen's list): the hardware ones point
    # into the shadows, EWRAM stays, IWRAM goes to gDsiIwram.
    for line in open(args.raw_pointers):
        offset, value = (int(field, 16) for field in line.split())
        region = value >> 24
        if not is_data(offset) or copy_offset(offset, 4) is None:
            errors.append("raw pointer at 0x%08X: not in a data object in the copy" % offset)
        elif word(offset) != value:
            errors.append("raw pointer at 0x%08X: the GBA link has 0x%08X, not 0x%08X" % (offset, word(offset), value))
        elif region in HW_SHADOWS and value - (region << 24) < HW_SHADOWS[region][1]:
            symbol_entries.append((offset, HW_SHADOWS[region][0], value - (region << 24)))
            kinds["raw %s address (into %s)" % (HW_SHADOWS[region][2], HW_SHADOWS[region][0])] += 1
        elif region == 2 and EWRAM_BASE <= value < EWRAM_BASE + EWRAM_SIZE:
            kept.append(offset)
            kinds["raw EWRAM address (kept)"] += 1
        elif region == 3:
            symbol_entries.append((offset, "gDsiIwram", value - IWRAM_BASE))
            kinds["raw IWRAM address (into gDsiIwram)"] += 1
        else:
            errors.append("raw pointer at 0x%08X -> 0x%08X: not handled" % (offset, value))
    adds.sort()
    kept.sort()
    symbol_entries.sort()

    # Copy offsets as byte deltas in halfwords; 0 escapes a 32-bit delta.
    def delta_stream(offsets):
        stream = []
        previous = 0
        for offset in offsets:
            offset = copy_offset(offset, 4)
            delta = offset - previous
            previous = offset
            if 0 < delta < 0x10000:
                stream.append(delta)
            else:
                stream.extend((0, delta & 0xFFFF, delta >> 16))
        return stream

    stream = delta_stream(adds)
    kept_stream = delta_stream(kept)
    with open(os.path.join(args.out, "rom_relocs.s"), "w") as out:
        out.write("@ Generated by port/dsi/tools/gen_link.py; do not edit.\n")
        out.write("@ The copy's extents, and the pointer words in it by offset from its start\n")
        out.write("@ (port/dsi/src/rom.c).\n")
        out.write("\t.section .rodata.rom_relocs, \"a\"\n\t.balign 4\n")
        out.write("\t.global gRomLayoutVersion\ngRomLayoutVersion:\n\t.word 0x%08X, 0x%08X\n" % layout_version)
        out.write("\t.global gRomExtentCount\ngRomExtentCount:\n\t.word %d\n" % len(extents))
        out.write("\t.global gRomExtents\ngRomExtents:\n")
        for start, end, offset in extents:
            out.write("\t.word 0x%06X, 0x%06X, 0x%06X\n" % (start - ROM_BASE, end - start, offset))
        out.write("\t.global gRomRelocAddCount\ngRomRelocAddCount:\n\t.word %d\n" % len(adds))
        out.write("\t.global gRomRelocSymbolCount\ngRomRelocSymbolCount:\n\t.word %d\n" % len(symbol_entries))
        out.write("\t.global gRomRelocKeptCount\ngRomRelocKeptCount:\n\t.word %d\n" % len(kept))
        out.write("\t.global gRomRelocSymbols\ngRomRelocSymbols:\n")
        for offset, name, addend in symbol_entries:
            target = name if addend == 0 else "%s + 0x%X" % (name, addend)
            out.write("\t.word 0x%06X, %s\n" % (copy_offset(offset, 4), target))
        for label, values in (("gRomRelocAdds", stream), ("gRomRelocKept", kept_stream)):
            out.write("\t.global %s\n%s:\n" % (label, label))
            for i in range(0, len(values), 16):
                out.write("\t.hword %s\n" % ", ".join(str(v) for v in values[i : i + 16]))
            out.write("\t.global %sEnd\n%sEnd:\n" % (label, label))

    report.append("ROM copy: %d extents of 0x%08X-0x%08X, %d bytes (%d KB) at 0x%08X-0x%08X, version %08x%08x"
                  % (len(extents), extents[0][0], extents[-1][1], copy_size, copy_size // 1024, rom_copy,
                     rom_copy + copy_size, layout_version[1], layout_version[0]))
    report.append("ROM data symbols: %d" % len(rom_symbols))
    report.append("RAM symbols: %d EWRAM, %d IWRAM, %d absolute"
                  % (len(regions["ewram"]), len(regions["iwram"]), len(absolute)))
    report.append("pointer words in data objects: %d" % sum(kinds.values()))
    for kind, count in sorted(kinds.items(), key=lambda item: -item[1]):
        report.append("  %6d  %s" % (count, kind))
    report.append("applied at boot: %d add-base (%d bytes encoded), %d symbol entries (%d bytes); %d unaligned"
                  % (len(adds), 2 * len(stream), len(symbol_entries), 8 * len(symbol_entries), unaligned))
    report.append("checked at boot: %d kept EWRAM words (%d bytes encoded)" % (len(kept), 2 * len(kept_stream)))
    if other_types:
        report.append("other relocation types in data objects (code inside them, not used): %s" % dict(other_types))
    report.extend("ERROR: " + e for e in errors)
    with open(os.path.join(args.out, "gen_report.txt"), "w") as out:
        out.write("\n".join(report) + "\n")
    print("\n".join(report))
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
