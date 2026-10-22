"""Convert assembled GBA data objects into native arm64 assembly.

usage: python -m datagen --root . --gba-build build/USA --out build/native/data [--check] [--no-bake]

By default the ROM-derived bytes are baked into the assembly (.incbin). With
--no-bake they are left as zeros and listed in a manifest instead, so that the
program can fill them in from the player's own ROM (port/src/romdata.c).
"""

import argparse
import collections
import re
import sys
from pathlib import Path

from .asmsource import first_statements
from .ctypes_db import TypeDB
from .emit import DataModel, Emitter, EmitError
from .gbaobj import linked_globals, linked_symbols, load_all
from .ram import emit_ram, emit_ram_map
from .soundmap import emit_sound_map

HINTS = Path(__file__).with_name("hints.txt")


def load_hints(types: TypeDB) -> tuple:
    """(name patterns, macro names) giving C types the game sources do not state."""
    names, macros = [], {}
    for n, line in enumerate(HINTS.read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line or line.startswith("="):
            continue
        pattern, type_name, *scope = line.split()
        key = type_name if type_name in types.records else f"struct {type_name}"
        if type_name == "raw":
            target = ("raw",)
            if pattern.startswith("@"):
                macros[pattern[1:]] = target
            else:
                names.append((re.compile(pattern), target, scope[0] if scope else None))
            continue
        if key not in types.records:
            sys.exit(f"{HINTS}:{n}: unknown type {type_name}")
        if pattern.startswith("@"):
            macros[pattern[1:]] = ("record", key)
        else:
            names.append((re.compile(pattern), ("record", key), scope[0] if scope else None))
    return names, macros


def load_ram_aliases(types: TypeDB) -> dict:
    """RAM symbol -> (container, native byte offset) for `=name container.field` hints."""
    aliases = {}
    for n, line in enumerate(HINTS.read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line.startswith("="):
            continue
        name, target = line[1:].split()
        container, *path = target.split(".")
        t, offset = types.decls.get(container), 0
        for field in path:
            if t is None or t[0] != "record":
                sys.exit(f"{HINTS}:{n}: {target}: not a struct field")
            fields = {f[0]: f for f in types.record_layout(t[1], "native")[1]}
            if field not in fields or fields[field][2] is None:
                sys.exit(f"{HINTS}:{n}: {target}: no field {field}")
            _, t, field_offset, *_ = fields[field]
            offset += field_offset
        aliases[name] = (container, offset)
    return aliases


C_TABLE_SECTIONS = re.compile(r"\s*(\.rodata\S*|\.text)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)\.o$")


def c_tables(map_path: Path) -> list:
    """(ROM offset, length) of the tables port and game C files hold, from the GBA link map.

    That is the read-only data of src/*.c, and the asm/src objects, whose inline
    tables port/src/asm transcribes into C.
    """
    tables = []
    for line in map_path.read_text().splitlines():
        m = C_TABLE_SECTIONS.match(line)
        if m is None:
            continue
        section, addr, size, obj = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
        wanted = obj.startswith("src/") if section != ".text" else obj.startswith("asm/src/")
        if wanted and addr >> 24 == 0x08 and size:
            tables.append((addr - 0x08000000, size))
    return tables


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=Path("."))
    ap.add_argument("--gba-build", type=Path, default=Path("build/USA"))
    ap.add_argument("--out", type=Path, default=Path("build/native/data"))
    ap.add_argument("--check", action="store_true", help="classify and report without writing output")
    ap.add_argument("--no-bake", dest="bake", action="store_false",
                    help="leave ROM bytes out of the output; the program extracts them from a ROM at runtime")
    args = ap.parse_args()

    root = args.root.resolve()
    gba_build = (root / args.gba_build).resolve()
    out = (root / args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    types = TypeDB.build(root, gba_build)
    types.save(out / "types.json")
    addrs = linked_symbols(root / "tmc.elf")
    objects = load_all(gba_build, addrs)
    model = DataModel(objects, types, load_hints(types), first_statements(root))
    model.set_ram_symbols(addrs)
    model.set_rom_index(addrs)
    model.classify()

    modes = collections.Counter(info.mode for info in model.labels.values())
    print(f"{len(objects)} objects, {len(model.labels)} labels: " + ", ".join(f"{n} {m}" for m, n in modes.most_common()))
    if model.problems:
        print(f"\n{len(model.problems)} labels need attention:")
        by_msg = collections.defaultdict(list)
        for p in model.problems:
            by_msg[p.message].append(p)
        for msg, ps in by_msg.items():
            print(f"  {msg}: {len(ps)}")
            for p in ps[:8]:
                print(f"    {p.label} ({p.obj})")
        if args.check:
            return 1

    if args.check:
        return 0

    # The GBA build is byte-identical to the ROM, so every run is checked against it.
    rom_path = root / "tmc.gba"
    emitter = Emitter(model, out, bake=args.bake, rom=rom_path.read_bytes() if rom_path.exists() else None)
    errors = []
    sources = []
    for obj in objects:
        try:
            sources.append(emitter.emit_object(obj))
        except EmitError as e:
            errors.append(f"{obj.rel}: {e}")
    sources.append(emit_ram(gba_build / "linker.ld", addrs, types, load_ram_aliases(types), out / "ram.s"))
    sources.append(emit_ram_map(addrs, linked_globals(root / "tmc.elf"), out / "ram_map.s"))
    sources.append(emit_sound_map(model, objects, out / "sound_map.s"))
    if not errors:
        try:
            sources.append(emitter.write_manifest(out / "rom_manifest.s", out / "rom_manifest.txt",
                                                  c_tables(gba_build / "tmc.map")))
        except EmitError as e:
            errors.append(f"rom_manifest: {e}")
    (out / "mode.txt").write_text(("bake" if args.bake else "rom") + "\n")
    (out / "sources.txt").write_text("\n".join(str(s) for s in sources) + "\n")
    (out / "script_tag.h").write_text(
        "// Generated by port/tools/datagen; do not edit.\n"
        f"#define SCRIPT_POINTER_TAG {emitter.script_tag:#010x}u\n"
        f"#define SCRIPT_POINTER_TAG_MASK 0xff000000u\n"
    )
    notes = [f"{name}: {why}" for name, why in model.c_fixes] + model.warnings
    (out / "notes.txt").write_text("\n".join(notes) + "\n")
    if notes:
        print(f"{len(model.c_fixes)} C declarations to fix and {len(model.warnings)} warnings, see {out / 'notes.txt'}")
    if errors:
        print(f"\n{len(errors)} objects failed to emit:")
        for e in errors[:40]:
            print("  " + e)
        return 1
    covered = sum(length for _, length, *_ in emitter.runs)
    print(f"wrote {len(sources)} assembly files to {out} ({'ROM data baked in' if args.bake else 'ROM data left out'}; "
          f"{len(emitter.runs)} ROM runs, {covered} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
