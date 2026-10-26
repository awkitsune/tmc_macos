"""Emit native (Mach-O arm64) assembly for GBA data objects.

Every GBA label becomes a native label. How its bytes are carried over depends
on what is known about it:

  raw      no pointers and a layout that does not differ: bytes are .incbin'd
           straight from a dump of the GBA section
  ptrs     every word is a pointer or zero: widened to an array of .quad
  typed    a C type is known: re-laid out field by field for the native ABI
  split    raw bytes followed by pointer words: the raw part stays in place and
           the pointer words are widened into a separate table
  script   script bytecode: bytes kept, each pointer slot replaced by a tagged
           index into a per-object native pointer table

Pointer targets are translated from GBA offsets to native offsets, so a pointer
into the middle of a re-laid-out table still lands on the same element.

Bytes carried over from the GBA objects are first produced as RomBytes markers
and lowered per section (see Emitter._lower). A baked build .incbin's them, as
before. A ROM build tells asset bytes, which asset extraction copied out of
baserom.gba, from bytes written in the repository's sources (provenance.py).
Source bytes are .incbin'd from a dump with the asset bytes zeroed. Asset bytes
are left as zeros (zero-fill where a section holds nothing else) and every run
of them is listed, as (destination, ROM offset, length), in the
__DATA,__tmcruns section; port/src/romdata.c fills them in from the data
extracted from the player's ROM before the game starts.
"""

import hashlib
import re
from dataclasses import dataclass, field
from pathlib import Path

from .ctypes_db import TypeDB
from .entitylists import RoomLists, list_shaped
from .gbaobj import R_ARM_ABS32, DataObject, Label

SCRIPT_TAG_MASK = 0xFF000000
SCRIPT_TAG_CANDIDATES = range(0xFF, 0x0F, -1)  # top bytes tried, highest first


ROM_BASE = 0x08000000

# Extraction stages, by the data object a run comes from (port/include/port/romdata.h).
CATEGORY_TABLES, CATEGORY_GRAPHICS, CATEGORY_MAPS, CATEGORY_SOUND = range(4)
CATEGORY_PREFIXES = [
    ("data/gfx/", CATEGORY_GRAPHICS),
    ("data/animations/", CATEGORY_GRAPHICS),
    ("data/map/", CATEGORY_MAPS),
    ("data/sound/", CATEGORY_SOUND),
]
MANIFEST_FORMAT = 1


class EmitError(Exception):
    pass


@dataclass(frozen=True)
class RomBytes:
    """GBA section bytes [start, start + length) carried over unchanged, then pad zero bytes."""

    sec: object
    start: int
    length: int
    pad: int = 0


@dataclass(frozen=True)
class RelocBytes:
    """Bytes of a GBA pointer word kept in place but never read natively (the
    object file's addend in a baked build, zeros otherwise)."""

    sec: object
    start: int
    length: int


def category(obj_rel: str) -> int:
    return next((c for prefix, c in CATEGORY_PREFIXES if obj_rel.startswith(prefix)), CATEGORY_TABLES)


def split_table_name(label_name: str) -> str:
    return label_name + "__ptrs"


@dataclass
class LabelInfo:
    label: Label
    mode: str = "raw"  # raw | ptrs | typed | script | code
    type: list = None  # element type for typed labels
    count: int = 0  # number of elements for typed labels
    tail: int = 0  # trailing GBA bytes after the typed elements, emitted raw
    split: int = 0  # for split labels: section offset where the pointer words start
    native_size: int = 0


@dataclass
class Problem:
    label: str
    obj: str
    message: str


class DataModel:
    """All GBA data labels with their chosen representation."""

    def __init__(self, objects: list, types: TypeDB, hints: tuple, first_statements: dict, room_hints: list = ()):
        self.objects = objects
        self.types = types
        self.name_hints, self.macro_hints = hints
        self.first_statements = first_statements
        self.ram_symbols = []  # sorted (address, name) for EWRAM/IWRAM, set by set_ram_symbols
        self.labels = {}  # (obj.rel, label name) -> LabelInfo
        self.globals = {}  # global name -> LabelInfo
        self.problems = []
        self.c_fixes = []  # (label, reason): C declarations that must change for native
        self.warnings = []
        self.raw_pointers = []  # (ROM address, GBA address): raw addresses in pointer fields, made native
        self.room_hints = list(room_hints)
        self.list_type = ("record", "EntityData" if "EntityData" in types.records else "struct EntityData")
        self.derived_lists = []  # room properties typed as entity lists by RoomLists
        self.list_errors = []  # entity lists whose native form would not match (RoomLists.check)
        for obj in objects:
            for sec in obj.sections:
                for label in sec.labels:
                    info = LabelInfo(label)
                    self.labels[(obj.rel, label.name)] = info
                    if label.is_global:
                        self.globals[label.name] = info

    # ---- classification ---------------------------------------------------

    def classify(self):
        for info in self.labels.values():
            sec = info.label.section
            if sec.name.startswith(".text"):
                info.mode = "code" if self._is_code(info.label) else "script"
        # Seed types from C declarations and hints, then follow typed pointers.
        for info in self.labels.values():
            if info.mode != "raw":
                continue
            t = self._declared_type(info.label.name, info.label.section.obj.rel)
            if t is not None:
                self._assign(info, t)
        changed = True
        while changed:
            changed = False
            for info in list(self.labels.values()):
                if info.mode == "typed":
                    for target, t in self._typed_pointer_targets(info):
                        if target.mode == "raw" and self._needs_type(target, t):
                            changed |= self._assign(target, t)
        room_lists = RoomLists(self, self.list_type, self.room_hints)
        self.derived_lists = room_lists.derive()
        for info in self.labels.values():
            if info.mode == "raw" and self._has_relocs(info.label):
                if self._is_pointer_array(info.label):
                    info.mode = "ptrs"
                elif self._split_point(info.label) is not None:
                    info.mode, info.split = "split", self._split_point(info.label)
                else:
                    self._problem(info, "holds pointers but has no C type (add a hint)")
        self.list_errors = room_lists.check()

    def _is_code(self, label: Label) -> bool:
        return any(r.kind != R_ARM_ABS32 for off, r in label.section.relocs.items() if label.offset <= off < label.end)

    def _declared_type(self, name, obj=None):
        # Explicit name hints, then the table's own macro, then the C declaration
        # (some tables are declared loosely, e.g. as a single u32).
        for pattern, t, scope in self.name_hints:
            if pattern.fullmatch(name) and (scope is None or scope == obj):
                return t
        macro = self.first_statements.get(name)
        if macro in self.macro_hints:
            return self.macro_hints[macro]
        return self.types.decls.get(name)

    def _needs_type(self, info, t) -> bool:
        if self._has_relocs(info.label):
            return True
        # Pointer-free entity lists still change layout (16-byte GBA entries, 24 native).
        if t == self.list_type:
            if list_shaped(info.label):
                return True
            self._problem(info, "an EntityData pointer leads here but it is not shaped like an entity list")
        return False

    def _assign(self, info: LabelInfo, t) -> bool:
        """Give a label element type t, repeated to cover its size. Returns True if it changed."""
        if t[0] == "raw":
            if self._has_relocs(info.label):
                self._problem(info, "hinted raw but holds pointers")
            return False
        if t[0] == "array":
            t = t[1]
        if t[0] in ("void", "func") or (t[0] == "int" and t[1] == 1):
            return False  # byte blobs and opaque data stay raw
        if t[0] == "int" and self._has_relocs(info.label):
            if self._is_pointer_array(info.label):
                self.c_fixes.append((info.label.name, "declared as an integer array but holds pointers"))
            return False  # classified as a pointer table later
        if not self._has_relocs(info.label) and self.types.same_layout(t):
            return False  # the contiguous raw copy already matches, even across labels
        gba = self.types.layout(t, "gba")
        if gba.size == 0:
            return False
        count, tail = divmod(info.label.size, gba.size)
        if count == 0:
            if self._has_relocs(info.label):
                self._problem(info, f"type of size {gba.size} larger than label ({info.label.size} bytes)")
            return False  # pointer-free stub smaller than its declared type: kept raw
        tail_start = info.label.offset + count * gba.size
        if tail and any(tail_start <= off < info.label.end for off in info.label.section.relocs):
            if not self._pointer_words(info.label.section, tail_start, info.label.end):
                self._problem(info, f"{tail} trailing bytes after {count} elements contain pointers")
                return False
        if info.mode == "typed":
            return False
        info.mode, info.type, info.count, info.tail = "typed", t, count, tail
        return True

    def _typed_pointer_targets(self, info):
        """(target LabelInfo, pointee type) for each pointer field in a typed label."""
        sec = info.label.section
        stride = self.types.layout(info.type, "gba").size
        for i in range(info.count):
            for off, t in self._pointer_fields(info.type, info.label.offset + i * stride):
                reloc = sec.relocs.get(off)
                if reloc is None or t[1][0] in ("func", "void"):
                    continue
                target = self._resolve(info.label.section.obj, reloc)
                if target is not None and target[1] == 0:
                    yield target[0], t[1]

    def _pointer_fields(self, t, off):
        kind = t[0]
        if kind == "ptr":
            yield off, t
        elif kind == "array":
            elem = self.types.layout(t[1], "gba").size
            for i in range(t[2] or 0):
                yield from self._pointer_fields(t[1], off + i * elem)
        elif kind == "record":
            rec = self.types.records[t[1]]
            if rec["union"]:
                return
            for name, ft, foff, width, _ in self.types.record_layout(t[1], "gba")[1]:
                if width is None:
                    yield from self._pointer_fields(ft, off + foff)

    def _has_relocs(self, label: Label) -> bool:
        return label.section.has_relocs_in(label.offset, label.end)

    def _is_pointer_array(self, label: Label) -> bool:
        return self._pointer_words(label.section, label.offset, label.end)

    def _split_point(self, label: Label):
        """Offset where a pointer-free prefix gives way to pointer words up to the end, if any."""
        first = min(off for off in label.section.relocs if label.offset <= off < label.end)
        for start in range(first, label.offset, -1):
            if label.section.word_aligned(start) and self._pointer_words(label.section, start, label.end):
                return start
        return None

    def _pointer_words(self, sec, start, end) -> bool:
        """True if [start, end) is whole aligned words that are each a pointer or zero."""
        if not sec.word_aligned(start) or (end - start) % 4:
            return False
        return all(w in sec.relocs or sec.data[w : w + 4] == b"\0\0\0\0" for w in range(start, end, 4))

    def _problem(self, info, message):
        self.problems.append(Problem(info.label.name, info.label.section.obj.rel, message))

    # ---- pointer resolution -----------------------------------------------

    def set_rom_index(self, symbols: dict):
        """Index labels by ROM address so raw ROM addresses in data can be resolved."""
        import bisect

        self._rom = sorted(
            (info.label.section.base + info.label.offset, key) for key, info in self.labels.items() if info.label.section.base
        )
        self._rom_addrs = [a for a, _ in self._rom]
        self._rom_symbols = {addr & ~1: name for name, addr in symbols.items() if (addr >> 24) in (0x08, 0x09)}
        self._bisect = bisect

    def rom_location(self, addr: int):
        """(native label, offset) for a ROM address, via data labels or exact code symbols."""
        i = self._bisect.bisect_right(self._rom_addrs, addr) - 1
        if i >= 0:
            base, key = self._rom[i]
            info = self.labels[key]
            if addr < info.label.section.base + info.label.end:
                return self.native_location(info, addr - base)
        name = self._rom_symbols.get(addr & ~1)
        return (name, 0) if name else None

    def set_ram_symbols(self, symbols: dict):
        self.ram_symbols = sorted((addr, name) for name, addr in symbols.items() if (addr >> 24) in (0x02, 0x03))

    def ram_symbol_at(self, addr: int):
        """(name, delta) of the RAM variable containing addr, by linked address."""
        import bisect

        i = bisect.bisect_right(self.ram_symbols, (addr, "\uffff")) - 1
        if i < 0:
            return None
        base, name = self.ram_symbols[i]
        return name, addr - base

    def _resolve(self, obj: DataObject, reloc):
        """(LabelInfo, gba delta) for a reloc that targets data, else None."""
        if reloc.symbol:
            info = self.globals.get(reloc.symbol) or self.labels.get((obj.rel, reloc.symbol))
            if info is None:
                return None
            return info, reloc.addend
        sec = next(s for s in obj.sections if s.name == reloc.section)
        label = sec.label_at(reloc.addend)
        if label is None:
            raise EmitError(f"{obj.rel}: pointer to {reloc.section}+{reloc.addend:#x} before any label")
        return self.labels[(obj.rel, label.name)], reloc.addend - label.offset

    def native_location(self, info: LabelInfo, delta: int):
        """(native label name, byte offset) for a GBA byte offset within a label."""
        if info.mode == "split" and info.label.offset + delta >= info.split:
            rest = info.label.offset + delta - info.split
            if rest % 4:
                raise EmitError(f"pointer into the middle of a pointer in {info.label.name}+{delta:#x}")
            return split_table_name(info.label.name), rest * 2
        return info.label.name, self.native_offset(info, delta)

    def native_offset(self, info: LabelInfo, delta: int) -> int:
        """Native byte offset within a label for a GBA byte offset."""
        if info.mode in ("raw", "script", "code", "split"):
            return delta
        if info.mode == "ptrs":
            if delta % 4:
                raise EmitError(f"pointer into the middle of a pointer in {info.label.name}+{delta:#x}")
            return delta * 2
        stride_gba = self.types.layout(info.type, "gba").size
        stride_nat = self.types.layout(info.type, "native").size
        index, inner = divmod(delta, stride_gba)
        if index >= info.count:
            return info.count * stride_nat + (delta - info.count * stride_gba)
        return index * stride_nat + self._native_inner(info.type, inner, info.label.name)

    def _native_inner(self, t, off, where):
        if off == 0:
            return 0
        kind = t[0]
        if kind == "array":
            g = self.types.layout(t[1], "gba").size
            n = self.types.layout(t[1], "native").size
            i, rest = divmod(off, g)
            return i * n + self._native_inner(t[1], rest, where)
        if kind == "record" and not self.types.records[t[1]]["union"]:
            gfields = self.types.record_layout(t[1], "gba")[1]
            nfields = self.types.record_layout(t[1], "native")[1]
            for g, n in zip(reversed(gfields), reversed(nfields)):
                if g[2] is not None and g[2] <= off:
                    return n[2] + self._native_inner(g[1], off - g[2], where)
        if kind == "int":
            return off
        raise EmitError(f"cannot map GBA offset {off:#x} inside {where}")


class Emitter:
    def __init__(self, model: DataModel, out_dir: Path, bake: bool = True, rom: bytes = None, masks: dict = None):
        self.model = model
        self.masks = masks  # (object, section) -> asset byte mask; None: every byte is a run
        self.types = model.types
        self.out_dir = out_dir
        self.bake = bake
        self.rom = rom  # the GBA build's tmc.gba, to check every run against
        self.script_tag = self.choose_script_tag(model.objects)
        self.runs = []  # (rom offset, length, category, object, native label) for the manifest
        self._run_labels = 0

    def sym(self, name: str) -> str:
        return "_" + name

    def target_expr(self, obj: DataObject, reloc) -> str:
        resolved = self.model._resolve(obj, reloc)
        if resolved is None:
            # Outside the data objects: C code, asm code, RAM or generated assets.
            addend = reloc.addend & ~1 if reloc.addend in (0, 1) else reloc.addend
            declared = self.types.decls.get(reloc.symbol)
            if addend and declared is not None and declared[0] in ("record", "array"):
                addend = self.model._native_inner(declared, addend, reloc.symbol)
            return self.sym(reloc.symbol) + (f" + {addend}" if addend else "")
        info, delta = resolved
        name, native = self.model.native_location(info, delta)
        return self.sym(name) + (f" + {native}" if native else "")

    HW_REGIONS = {0x04: ("gHwIo", 0x400), 0x05: ("gHwPltt", 0x400), 0x06: ("gHwVram", 0x18000), 0x07: ("gHwOam", 0x400)}

    def hardware_address(self, addr: int, label, off) -> str:
        """Native expression for a raw GBA address stored in a pointer field."""
        expr = self._raw_address(addr, label, off)
        if expr is not None:
            self.model.raw_pointers.append((label.section.base + off, addr))
        return expr

    def _raw_address(self, addr: int, label, off) -> str:
        region = addr >> 24
        if region in self.HW_REGIONS:
            name, size = self.HW_REGIONS[region]
            delta = addr & 0xFFFFFF
            if delta < size:
                return self.sym(name) + (f" + {delta}" if delta else "")
        if region in (0x02, 0x03):
            ram = self.model.ram_symbol_at(addr)
            if ram is not None:
                name, delta = ram
                declared = self.types.decls.get(name)
                if delta and declared is not None and declared[0] in ("record", "array"):
                    delta = self.model._native_inner(declared, delta, name)
                return self.sym(name) + (f" + {delta}" if delta else "")
        if region in (0x08, 0x09):
            target = self.model.rom_location(addr)
            if target is not None:
                name, delta = target
                return self.sym(name) + (f" + {delta}" if delta else "")
        if addr >= 0x02000000:
            self.model.warnings.append(f"{label.name}+{off - label.offset:#x}: unmapped address {addr:#010x} kept as a number")
        # Small values are not addresses at all (e.g. a CGB duty cycle in ToneData.wav):
        # the caller keeps the number.
        return None

    def emit_object(self, obj: DataObject) -> Path:
        lines = ["// Generated by port/tools/datagen from " + obj.rel + ".o; do not edit.", ""]
        script_ptrs = []
        self._obj_runs = []
        self._obj_streams = []
        for sec in obj.sections:
            bin_path = self.out_dir / f"{obj.rel}{sec.name}.bin"
            bin_path.parent.mkdir(parents=True, exist_ok=True)
            if self.bake:
                bin_path.write_bytes(sec.data)
            else:
                # Only what the sources spell out; the asset bytes come from the player's ROM.
                mask = self._mask(sec)
                bin_path.write_bytes(bytes(0 if m else b for b, m in zip(sec.data, mask)))
            self._bin = bin_path.resolve()
            self._sec = sec
            body, literal = self._lower(obj, self._stream(obj, sec, script_ptrs))
            if self.bake:
                section = "__DATA,__data" if sec.name.startswith(".data") else "__DATA,__const"
            else:
                # Filled in at startup, so writable; zero-fill unless something else is in it.
                section = "__DATA,__data" if literal else "__DATA,__tmcrom,zerofill"
            lines += ["", f"\t.section {section}", "\t.p2align 3"] + body
            relaid = [self.model.labels[(obj.rel, l.name)] for l in sec.labels]
            relaid = [info for info in relaid if info.mode in ("ptrs", "typed", "split")]
            if relaid:
                if self.bake:
                    section = "__DATA,__data" if sec.name.startswith(".data") else "__DATA,__const"
                else:
                    section = "__DATA,__data"
                relaid_lines = []
                for info in relaid:
                    relaid_lines += self._relaid(obj, info)
                lines += ["", f"\t.section {section}"] + self._lower(obj, relaid_lines)[0]
        if script_ptrs:
            table = self._script_table(obj)
            lines += ["", "\t.section __DATA,__const", "\t.p2align 3", f"\t.globl {table}", f"{table}:"]
            lines += [f"\t.quad {expr}" for expr in script_ptrs]
        if self._obj_streams:
            # (native address, GBA address, length) of each section copied with its GBA layout:
            # HwRomAddress (port/src/hw.c) gives a pointer into one its real GBA address.
            lines += ["", "\t.section __DATA,__tmcstreams", "\t.p2align 3"]
            for label, gba, length in self._obj_streams:
                lines += [f"\t.quad {label}", f"\t.long {gba:#x}, {length:#x}"]
        if self._obj_runs:
            # (native address, ROM offset, length): the manifest port/src/romdata.c fills from.
            lines += ["", "\t.section __DATA,__tmcruns", "\t.p2align 3"]
            for label, rom, length in self._obj_runs:
                lines += [f"\t.quad {label}", f"\t.long {rom:#x}, {length:#x}"]
        out = self.out_dir / f"{obj.rel}.s"
        out.write_text("\n".join(lines) + "\n")
        return out

    def _lower(self, obj, lines) -> tuple:
        """Turn RomBytes/RelocBytes markers into directives and record the runs.

        Consecutive markers that continue each other in both the ROM and the native
        output (labels may sit between them) form one run. Returns the lines and
        whether anything other than labels, alignment and zeros is left, i.e.
        whether the section can not be zero-fill.
        """
        out = []
        literal = False
        run = None  # [label, rom offset, length]

        def close():
            nonlocal run
            if run is not None:
                self._obj_runs.append(tuple(run))
                self.runs.append((run[1], run[2], category(obj.rel), obj.rel, run[0]))
                run = None

        for line in lines:
            if isinstance(line, RomBytes):
                for start, length, asset in self._spans(line):
                    if not asset:
                        close()
                        literal = True
                        out.append(f'\t.incbin "{self._bin_of(line.sec)}", {start}, {length}')
                        continue
                    rom = line.sec.base - ROM_BASE + start
                    if self.rom is not None and self.rom[rom : rom + length] != line.sec.data[start : start + length]:
                        raise EmitError(f"{line.sec.name}+{start:#x}: bytes differ from the ROM at {rom:#x}")
                    if run is not None and run[1] + run[2] == rom:
                        run[2] += length
                    else:
                        close()
                        label = f"Ltmcrom{self._run_labels}"
                        self._run_labels += 1
                        out.append(f"{label}:")
                        run = [label, rom, length]
                    if self.bake:
                        out.append(f'\t.incbin "{self._bin_of(line.sec)}", {start}, {length}')
                    else:
                        out.append(f"\t.space {length}")
                if line.pad:
                    out.append(f"\t.space {line.pad}")
                    close()
            elif isinstance(line, RelocBytes):
                close()
                if self.bake or self.masks is not None:
                    literal = True
                    out.append(f'\t.incbin "{self._bin_of(line.sec)}", {line.start}, {line.length}')
                else:
                    out.append(f"\t.space {line.length}")
            else:
                text = line.strip()
                if not (text == "" or text.startswith("//") or text.endswith(":") or text.startswith(".globl")):
                    close()
                if text.startswith((".byte", ".short", ".long", ".quad")):
                    literal = True
                out.append(line)
        close()
        return out, literal

    def _mask(self, sec) -> bytes:
        if self.masks is None:
            return bytes([1]) * len(sec.data)
        return self.masks[(sec.obj.rel, sec.name)]

    def _spans(self, line):
        """(start, length, is asset) pieces of a RomBytes marker, by provenance."""
        if self.bake or self.masks is None:
            return [(line.start, line.length, True)]
        mask = self._mask(line.sec)
        spans = []
        for i in range(line.start, line.start + line.length):
            asset = bool(mask[i])
            if spans and spans[-1][2] == asset:
                spans[-1][1] += 1
            else:
                spans.append([i, 1, asset])
        return [tuple(s) for s in spans]

    def _bin_of(self, sec) -> Path:
        assert sec is self._sec
        return self._bin

    def write_manifest(self, out: Path, listing: Path) -> Path:
        """rom_manifest.s: the ROM extents the data cache holds, and its version.

        The cache is the ROM bytes of every run, merged into extents (sorted,
        disjoint, one stage each) and stored back to back. Its version hash covers
        the extents, so any change to what is extracted invalidates old caches.
        """
        spans = sorted((rom, rom + length, cat) for rom, length, cat, _, _ in self.runs)
        extents = []  # [rom start, rom end, category]
        for start, end, cat in spans:
            if extents and start <= extents[-1][1] and cat == extents[-1][2]:
                extents[-1][1] = max(extents[-1][1], end)
            elif extents and start < extents[-1][1]:
                raise EmitError(f"ROM {start:#x}: runs of different stages overlap")
            else:
                extents.append([start, end, cat])
        digest = hashlib.sha256(f"tmc rom manifest {MANIFEST_FORMAT}\n".encode())
        for start, end, cat in extents:
            digest.update(f"{start:x} {end - start:x} {cat}\n".encode())
        version = int.from_bytes(digest.digest()[:8], "little")
        total = sum(end - start for start, end, _ in extents)
        lines = [
            "// Generated by port/tools/datagen; do not edit. See port/src/romdata.c.",
            "",
            "\t.section __DATA,__const",
            "\t.p2align 3",
            "\t.globl _gRomManifestVersion",
            "_gRomManifestVersion:",
            f"\t.quad {version:#018x}",
            "\t.globl _gRomDataBaked",
            "_gRomDataBaked:",
            f"\t.long {1 if self.bake else 0}",
            "\t.globl _gRomCacheSize",
            "_gRomCacheSize:",
            f"\t.long {total:#x}",
            "\t.globl _gRomExtentCount",
            "_gRomExtentCount:",
            f"\t.long {len(extents)}",
            "\t.p2align 2",
            "\t.globl _gRomExtents",
            "_gRomExtents:  // ROM offset, length, cache offset, stage",
        ]
        cache = 0
        for start, end, cat in extents:
            lines.append(f"\t.long {start:#x}, {end - start:#x}, {cache:#x}, {cat}")
            cache += end - start
        out.write_text("\n".join(lines) + "\n")
        # The same manifest for people: every run with where it lands.
        text = [f"# version {version:#018x}: {len(self.runs)} runs, {len(extents)} extents, {total} bytes",
                "# rom_offset length stage object native_label"]
        text += [f"{rom:#08x} {length:#x} {cat} {rel} {label}" for rom, length, cat, rel, label in sorted(self.runs)]
        listing.write_text("\n".join(text) + "\n")
        return out

    def _stream(self, obj, sec, script_ptrs) -> list:
        """The section copied byte for byte, keeping every raw label at its GBA offset.

        Re-laid-out labels are defined elsewhere; their GBA bytes stay here unused so
        nothing after them moves. Script sections patch each pointer slot in place.
        The copy starts at the same address modulo 8 as on the GBA.
        """
        lines = []
        if sec.base:
            label = f"Ltmcstream{len(self._obj_streams)}"
            lines.append(f"{label}:")
            self._obj_streams.append((label, sec.base - sec.base % 8, sec.base % 8 + len(sec.data)))
        if sec.base % 8:
            lines.append(f"\t.space {sec.base % 8}")
        events = []
        for label in sec.labels:
            info = self.model.labels[(obj.rel, label.name)]
            if info.mode in ("raw", "script", "split"):
                events.append((label.offset, 0, label))
        if sec.name.startswith(".text"):
            for off in sec.relocs:
                events.append((off, 1, None))
            self._check_script_tags(sec)
        pos = 0
        for off, kind, label in sorted(events, key=lambda e: (e[0], e[1])):
            lines += self._raw(pos, off)
            pos = max(pos, off)
            if kind == 0:
                if label.is_global:
                    lines.append(f"\t.globl {self.sym(label.name)}")
                lines.append(f"{self.sym(label.name)}:")
            else:
                reloc = sec.relocs[off]
                if reloc.kind != R_ARM_ABS32:
                    continue  # inside embedded code, which is replaced natively
                index = len(script_ptrs)
                script_ptrs.append(self.target_expr(obj, reloc))
                lines.append(f"\t.long {self.script_tag | index:#x}")
                pos = off + 4
        lines += self._raw(pos, len(sec.data))
        return lines

    def _script_table(self, obj):
        return "_gScriptPointers_" + re.sub(r"\W", "_", obj.rel)

    def _check_script_tags(self, sec):
        pass  # the tag is chosen up front so that no plain operand can collide with it

    @staticmethod
    def choose_script_tag(objects) -> int:
        """A top byte that no plain (non-pointer) word in any script section uses."""
        used = set()
        for obj in objects:
            for sec in obj.sections:
                if not sec.name.startswith(".text"):
                    continue
                near_reloc = set()
                for r in sec.relocs:
                    near_reloc.update(range(r - 3, r + 4))
                for off in range(0, len(sec.data) - 3, 2):
                    if off not in near_reloc:
                        used.add(sec.data[off + 3])
        for top in SCRIPT_TAG_CANDIDATES:
            if top not in used:
                return top << 24
        raise EmitError("no free top byte for the script pointer tag")

    def _relaid(self, obj, info: LabelInfo) -> list:
        label = info.label
        if info.mode == "split":
            lines = ["\t.p2align 3", f"{self.sym(split_table_name(label.name))}:"]
            for w in range(info.split, label.end, 4):
                reloc = label.section.relocs.get(w)
                lines.append(f"\t.quad {self.target_expr(obj, reloc) if reloc else 0}")
            return lines
        align = 8 if info.mode == "ptrs" else max(1, self.types.layout(info.type, "native").align)
        lines = [f"\t.p2align {align.bit_length() - 1}"]
        if label.is_global:
            lines.append(f"\t.globl {self.sym(label.name)}")
        lines.append(f"{self.sym(label.name)}:")
        if info.mode == "ptrs":
            for w in range(label.offset, label.end, 4):
                reloc = label.section.relocs.get(w)
                lines.append(f"\t.quad {self.target_expr(obj, reloc) if reloc else 0}")
        else:
            stride = self.types.layout(info.type, "gba").size
            for i in range(info.count):
                lines += self._value(obj, info.type, label.offset + i * stride, label)
            if info.tail:
                lines += self._tail(obj, label, label.end - info.tail)
        return lines

    def _tail(self, obj, label, start) -> list:
        if not any(start <= off < label.end for off in label.section.relocs):
            return self._raw(start, label.end)
        lines = []
        for w in range(start, label.end, 4):
            reloc = label.section.relocs.get(w)
            lines.append(f"\t.quad {self.target_expr(obj, reloc) if reloc else 0}")
        return lines

    def _raw(self, start, end):
        """Section bytes [start, end) as they are: ROM bytes, except pointer words."""
        sec = self._sec
        out = []
        pos = start
        for r in sorted(r for r in sec.relocs if r < end and r + 4 > start):
            lo, hi = max(r, start), min(r + 4, end)
            if lo > pos:
                out.append(RomBytes(sec, pos, lo - pos))
            out.append(RelocBytes(sec, lo, hi - lo))
            pos = hi
        if end > pos:
            out.append(RomBytes(sec, pos, end - pos))
        return out

    def _union(self, obj, t, off, label) -> list:
        """Emit a union through the member that matches what the GBA bytes hold."""
        types = self.types
        rec = types.records[t[1]]
        gsize = types.layout(t, "gba").size
        has_reloc = any(off <= r < off + gsize for r in label.section.relocs)
        members = [ft for _, ft, _, _ in rec["fields"]]
        if has_reloc:
            choice = next((m for m in members if any(o == off for o, _ in self.model._pointer_fields(m, off))), None)
        else:
            choice = next((m for m in members if not types.contains_pointer(m)), None)
        if choice is None:
            raise EmitError(f"{label.name}+{off - label.offset:#x}: no member of {t[1]} fits its contents")
        out = self._value(obj, choice, off, label)
        pad = types.layout(t, "native").size - types.layout(choice, "native").size
        if pad:
            out.append(f"\t.space {pad}")
        return out

    # ---- typed values -----------------------------------------------------

    def _value(self, obj, t, off, label) -> list:
        """Lines for one value of type t at GBA section offset off."""
        types = self.types
        relocs = label.section.relocs
        if types.same_layout(t) and not any(off <= r < off + types.layout(t, "gba").size for r in relocs):
            return self._raw(off, off + types.layout(t, "gba").size)
        kind = t[0]
        if kind == "ptr":
            reloc = relocs.get(off)
            if reloc is not None:
                return [f"\t.quad {self.target_expr(obj, reloc)}"]
            word = int.from_bytes(label.section.data[off : off + 4], "little")
            if word:
                expr = self.hardware_address(word, label, off)
                return [f"\t.quad {expr}"] if expr is not None else [RomBytes(label.section, off, 4, 4)]
            return ["\t.quad 0"]
        if kind == "int":
            g, n = t[1], t[2]
            if off in relocs:
                if n == 8:
                    return [f"\t.quad {self.target_expr(obj, relocs[off])}"]  # e.g. PtrU32
                raise EmitError(f"{label.name}+{off - label.offset:#x}: pointer stored in a 32-bit integer field")
            if n < g:
                raise EmitError(f"{label.name}+{off - label.offset:#x}: integer field narrower natively")
            return [RomBytes(label.section, off, g, n - g)]  # zero-extended, as the GBA value is unsigned here
        if kind == "array":
            g = types.layout(t[1], "gba").size
            out = []
            for i in range(t[2] or 0):
                out += self._value(obj, t[1], off + i * g, label)
            return out
        if kind == "record":
            return self._record(obj, t, off, label)
        raise EmitError(f"{label.name}: cannot emit type {t}")

    def _record(self, obj, t, off, label) -> list:
        types = self.types
        rec = types.records[t[1]]
        if rec["union"]:
            return self._union(obj, t, off, label)
        nat_layout, nfields = types.record_layout(t[1], "native")
        _, gfields = types.record_layout(t[1], "gba")
        out = []
        pos = 0  # native bytes emitted so far
        i = 0
        while i < len(nfields):
            name, ft, noff, width, nbit = nfields[i]
            g = gfields[i]
            if width is not None:
                # A run of bitfields: read each value at its GBA bit position and pack
                # it at its native one.
                j = i
                while j < len(nfields) and nfields[j][3] is not None:
                    j += 1
                gba_bits = int.from_bytes(label.section.data[off : off + types.layout(t, "gba").size], "little")
                first = nfields[i][4] // 8
                last = -(-max(f[4] + f[3] for f in nfields[i:j]) // 8)
                packed = 0
                for k in range(i, j):
                    w = nfields[k][3]
                    value = (gba_bits >> gfields[k][4]) & ((1 << w) - 1)
                    packed |= value << (nfields[k][4] - first * 8)
                if first > pos:
                    out.append(f"\t.space {first - pos}")
                packed = packed.to_bytes(last - first, "little")
                gfirst = min(gfields[k][4] for k in range(i, j)) // 8
                if packed == label.section.data[off + gfirst : off + gfirst + len(packed)]:
                    out.append(RomBytes(label.section, off + gfirst, len(packed)))
                else:
                    if not self.bake:
                        raise EmitError(f"{label.name}+{off - label.offset:#x}: bitfields packed differently natively")
                    out.append("\t.byte " + ", ".join(f"{b:#x}" for b in packed))
                pos = last
                i = j
                continue
            if noff > pos:
                out.append(f"\t.space {noff - pos}")
            out += self._value(obj, ft, off + g[2], label)
            pos = noff + types.layout(ft, "native").size
            i += 1
        if nat_layout.size > pos:
            out.append(f"\t.space {nat_layout.size - pos}")
        return out
