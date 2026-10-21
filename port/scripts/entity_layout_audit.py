#!/usr/bin/env python3
"""List Entity subtype fields whose native offset differs from where the
generic entity fields at the same GBA offset live natively.

Entity code often reaches the same bytes through different views: a subtype
struct, GenericEntity.field_0xNN, the EntityData copied to 0x78 when a room
loads an entity, the script context at 0x84. On GBA all views agree by
offset. Natively GenericEntity keeps GBA 0x68..0x87 at base+0..0x1f, and a
pointer declared at 0x84 (the script context) at base+0x20; a subtype field
whose native offset is not the mapped one is read and written somewhere else
than the other views.

    entity_layout_audit.py build/native/data/types.json
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from datagen.ctypes_db import TypeDB  # noqa: E402

GBA_ENTITY = 0x68


def mapped(native_entity, gba, ft):
    if gba == 0x84 and ft[0] == "ptr":
        return native_entity + 0x20
    return native_entity + gba - GBA_ENTITY


def flatten(types, key, gba_base, native_base, prefix, out):
    """Leaf fields of a record as (name, gba offset, native offset, type)."""
    _, gba_fields = types.record_layout(key, "gba")
    _, nat_fields = types.record_layout(key, "native")
    for (name, ft, goff, width, _), (_, _, noff, _, _) in zip(gba_fields, nat_fields):
        if width is not None:
            continue  # bitfields: offsets are bit positions, checked through their unit
        if ft[0] == "record" and not types.records[ft[1]]["union"]:
            flatten(types, ft[1], gba_base + goff, native_base + noff, prefix + name + ".", out)
        else:
            out.append((prefix + name, gba_base + goff, native_base + noff, ft))


def main(path):
    types = TypeDB.load(Path(path))
    native_entity = types.record_layout("struct Entity_", "native")[0].size
    problems = 0
    for key, rec in sorted(types.records.items()):
        fields = rec["fields"]
        if not fields or fields[0][0] != "base" or fields[0][1] != ("record", "struct Entity_") or rec["union"]:
            continue
        size = types.record_layout(key, "native")[0].size
        leaves = []
        flatten(types, key, 0, 0, "", leaves)
        bad = [
            (n, g, nat, ft)
            for n, g, nat, ft in leaves
            if g >= GBA_ENTITY and g < 0x88 and nat != mapped(native_entity, g, ft) and not n.startswith("nativeSlack")
        ]
        if size > 256:
            print(f"{key}: native size {size:#x} exceeds the 256-byte entity slot")
            problems += 1
        for n, g, nat, ft in bad:
            kind = "pointer" if ft[0] == "ptr" else "data"
            print(f"{key}.{n}: GBA {g:#x} -> native +{nat - native_entity:#x}, generic +{mapped(native_entity, g, ft) - native_entity:#x} ({kind})")
            problems += 1
    print(f"{problems} findings")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/native/data/types.json")
