#!/usr/bin/env python3
"""The relocated ROM copy's CRC32, computed on the host (milestone 3 check).

    python3 -I port/dsi/tools/rom_crc.py build/dsi/tmc_dsi.elf baserom.gba

Applies the relocation lists the DSi binary carries (rom_relocs.s, read back
from the ELF) to the ROM, as port/dsi/src/rom.c does at boot, and prints the
SHA-1, the counts and the CRC32 of the copy. The boot log must show the same.
Nothing is written. Needs pyelftools.
"""

import argparse
import hashlib
import struct
import sys
import zlib

from elftools.elf.elffile import ELFFile

ROM_BASE = 0x08000000


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("elf")
    parser.add_argument("rom")
    args = parser.parse_args()

    elf = ELFFile(open(args.elf, "rb"))
    symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}

    def read(address, size):
        for segment in elf.iter_segments():
            start = segment["p_vaddr"]
            if segment["p_type"] == "PT_LOAD" and start <= address and address + size <= start + segment["p_filesz"]:
                return segment.data()[address - start : address - start + size]
        sys.exit("0x%08X is not in the ELF's loaded data" % address)

    def word(name):
        return struct.unpack("<I", read(symbols[name], 4))[0]

    def offsets(start, end):
        data = read(symbols[start], symbols[end] - symbols[start])
        values = struct.unpack("<%dH" % (len(data) // 2), data)
        offset, i = 0, 0
        while i < len(values):
            step = values[i]
            i += 1
            if step == 0:
                step = values[i] | values[i + 1] << 16
                i += 2
            offset += step
            yield offset

    copy_base = symbols["__rom_copy"]
    size = symbols["__rom_copy_end"] - copy_base
    rom = open(args.rom, "rb").read()
    print("SHA-1 %s" % hashlib.sha1(rom).hexdigest())
    copy = bytearray(rom[:size])
    delta = copy_base - ROM_BASE
    unexpected = 0

    adds = 0
    for offset in offsets("gRomRelocAdds", "gRomRelocAddsEnd"):
        value = struct.unpack_from("<I", copy, offset)[0]
        unexpected += not (ROM_BASE <= value < ROM_BASE + size)
        struct.pack_into("<I", copy, offset, (value + delta) & 0xFFFFFFFF)
        adds += 1
    count = word("gRomRelocSymbolCount")
    table = struct.unpack("<%dI" % (2 * count), read(symbols["gRomRelocSymbols"], 8 * count))
    for i in range(count):
        offset, value = table[2 * i], table[2 * i + 1]
        # ROM, IWRAM, or a raw GBA hardware address (into its shadow), as rom.c.
        unexpected += struct.unpack_from("<I", copy, offset)[0] >> 24 not in (0x08, 0x09, 0x03, 0x04, 0x05, 0x06, 0x07)
        struct.pack_into("<I", copy, offset, value)
    kept = 0
    for offset in offsets("gRomRelocKept", "gRomRelocKeptEnd"):
        unexpected += not (0x02000000 <= struct.unpack_from("<I", copy, offset)[0] < 0x02040000)
        kept += 1

    print("relocated: %d add-base, %d symbol; %d EWRAM kept; %d unexpected" % (adds, count, kept, unexpected))
    print("expected: %d add-base, %d EWRAM" % (word("gRomRelocAddCount"), word("gRomRelocKeptCount")))
    print("copy 0x%08X, %d bytes, CRC32 %08x" % (copy_base, size, zlib.crc32(copy)))


if __name__ == "__main__":
    main()
