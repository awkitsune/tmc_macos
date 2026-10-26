#!/usr/bin/env python3
"""Compare data layouts between two sets of objects compiled with -g.

    python3 -I port/dsi/tools/layout_diff.py GBA_OBJ_DIR DSI_OBJ_DIR

Both directories hold the same game files compiled for the ARM9, once with the
GBA layout (no PORT_* defines) and once as the DSi build (PORT_HOSTED without
PORT_LAYOUT_64). The DSi reads data straight from the ROM and keeps the GBA RAM
map, so every struct, union and global variable must come out the same: same
size, same member offsets and sizes. Compares object by object and prints
each one that differs (with an object it appears in) and exits 1 if any do. Types that exist in only one
build are listed separately; they are fine if only port code uses them.
Needs pyelftools.
"""

import os
import sys

from elftools.elf.elffile import ELFFile


def attr(die, name, default=None):
    value = die.attributes.get(name)
    return value.value if value is not None else default


def name_of(die):
    value = attr(die, "DW_AT_name")
    return value.decode() if isinstance(value, bytes) else value


def strip(die):
    """Follow typedefs and qualifiers down to the underlying type."""
    while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
        if "DW_AT_type" not in die.attributes:
            return None
        die = die.get_DIE_from_attribute("DW_AT_type")
    return die


def type_size(die):
    die = strip(die)
    if die is None:
        return 0
    if die.tag == "DW_TAG_array_type":
        count = 1
        for child in die.iter_children():
            if child.tag == "DW_TAG_subrange_type":
                if "DW_AT_count" in child.attributes:
                    count *= attr(child, "DW_AT_count")
                elif "DW_AT_upper_bound" in child.attributes:
                    count *= attr(child, "DW_AT_upper_bound") + 1
                else:
                    count = 0
        return count * type_size(die.get_DIE_from_attribute("DW_AT_type"))
    return attr(die, "DW_AT_byte_size", 0)


def describe(die):
    """Size and (member, offset, size, bit offset, bits) for a struct or union."""
    members = []
    for child in die.iter_children():
        if child.tag != "DW_TAG_member":
            continue
        offset = attr(child, "DW_AT_data_member_location", 0)
        bits = (attr(child, "DW_AT_data_bit_offset"), attr(child, "DW_AT_bit_size"))
        member_type = child.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in child.attributes else None
        members.append((name_of(child), offset, type_size(member_type), bits))
    return (attr(die, "DW_AT_byte_size", 0), tuple(members))


def collect(directory):
    types = {}
    variables = {}
    for file_name in sorted(os.listdir(directory)):
        if not file_name.endswith(".o"):
            continue
        with open(os.path.join(directory, file_name), "rb") as stream:
            elf = ELFFile(stream)
            if not elf.has_dwarf_info():
                continue
            for unit in elf.get_dwarf_info().iter_CUs():
                for die in unit.iter_DIEs():
                    if die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
                        if attr(die, "DW_AT_declaration"):
                            continue
                        key = name_of(die)
                        if key is None:
                            continue  # anonymous: compared through its typedef below
                        types.setdefault((die.tag[7:-5], key, file_name, attr(die, "DW_AT_decl_line")), describe(die))
                    elif die.tag == "DW_TAG_typedef" and "DW_AT_type" in die.attributes:
                        target = die.get_DIE_from_attribute("DW_AT_type")
                        if target.tag in ("DW_TAG_structure_type", "DW_TAG_union_type") and name_of(target) is None:
                            types.setdefault(("typedef", name_of(die), file_name, attr(die, "DW_AT_decl_line")), describe(target))
                    elif die.tag == "DW_TAG_variable" and die.get_parent().tag == "DW_TAG_compile_unit":
                        if "DW_AT_type" not in die.attributes or name_of(die) is None:
                            continue
                        size = type_size(die.get_DIE_from_attribute("DW_AT_type"))
                        variables.setdefault((name_of(die), file_name), size)
    return types, variables


def main():
    gba_types, gba_vars = collect(sys.argv[1])
    dsi_types, dsi_vars = collect(sys.argv[2])
    differ = set()
    shown = set()
    # Types are compared per object and declaration line (both builds compile the
    # same lines): names such as "fixme" are reused for different local types.
    for key in sorted(set(gba_types) & set(dsi_types), key=lambda k: (k[1] or "", k[2])):
        gba, dsi = gba_types[key], dsi_types[key]
        if gba == dsi:
            continue
        differ.add(key[:2])
        if (key[:2], gba, dsi) in shown:
            continue
        shown.add((key[:2], gba, dsi))
        print("%s %s: GBA %d bytes, DSi %d bytes (%s)" % (key[0], key[1], gba[0], dsi[0], key[2]))
        gba_members = {m[0]: m for m in gba[1]}
        for member in dsi[1]:
            if gba_members.get(member[0]) != member:
                print("    %-28s GBA %-28s DSi %s" % (member[0], gba_members.get(member[0], (None,))[1:], member[1:]))
        for name in sorted(set(gba_members) - {m[0] for m in dsi[1]}, key=str):
            print("    %-28s GBA %-28s DSi (missing)" % (name, gba_members[name][1:]))
    for key in sorted(set(gba_vars) & set(dsi_vars)):
        if gba_vars[key] != dsi_vars[key]:
            differ.add(("variable", key[0]))
            print("variable %s: GBA %d bytes, DSi %d bytes (%s)" % (key[0], gba_vars[key], dsi_vars[key], key[1]))
    for key in sorted(set(dsi_types) - set(gba_types), key=str):
        print("only DSi: %s %s (%s:%s)" % key)
    for key in sorted(set(gba_types) - set(dsi_types), key=str):
        print("only GBA: %s %s (%s:%s)" % key)
    print("compared %d type and %d global definitions: %d names differ"
          % (len(set(gba_types) & set(dsi_types)), len(set(gba_vars) & set(dsi_vars)), len(differ)))
    sys.exit(1 if differ else 0)


if __name__ == "__main__":
    main()
