"""Pointer map of the save state segment (port/src/state.c), from the C types.

    python -m datagen.statemap --root ROOT --gba-build DIR --sources LIST --out state_map.c

A save state is the __TMCSTATE segment, which is full of native pointers: into
the game's data, to functions, into the segment itself. To load a state at
another address, or into another build, each pointer has to be written as
"symbol + offset" and found again, and every other word left alone. This works
out which bytes of each variable can hold a pointer from the variables' C
types, read with libclang (function bodies skipped, as datagen's TypeDB):

- pointer fields, and the integer types made to hold one (PtrU32, uintptr_t,
  intptr_t), at any depth of structs, unions and arrays;
- entity slots, whose layout depends on what lives in them: the pools named in
  statemap.txt are read slot by slot, the kind and id picking the subtype struct
  (ArmosEntity, BridgeManager...) through the entity function tables and the
  types those functions and their files use; statemap.txt settles the ids whose
  layout also depends on the type field;
- heap blocks (gzHeap): each zMalloc call records the type it allocates
  (port/src/heap.c), read here from the call's sizeof(T) or (T*) cast;
- variables with no C type (datagen's RAM blocks no source declares, assembly
  symbols, statics inside functions) are listed in statemap.txt as holding no
  pointers. The program refuses to save a state with a variable the map does not
  cover, and the build checks that (TMC_STATE_MAP_CHECK).

A pointer-capable word holds a pointer when its value is 2^32 or more: the
game's integers are 32-bit, and the program lives above 4 GB (__PAGEZERO). That
is how a field holding either (PtrU32, an Entity* the lilypad keeps a position
in) is read exactly.

Layouts are the arm64 ones; x86_64 has the same (both LP64, natural alignment).
The output is C (port/include/port/state_map.h), and a report next to it
(state_map.txt): how each entity kind and id was resolved, and the ambiguous
places (unions mixing pointers and data, 64-bit integers taken for data,
variables declared with different types).
"""

import argparse
import json
import os
import re
import sys
from multiprocessing import Pool
from pathlib import Path

import clang.cindex as ci

from .ctypes_db import clang_args

K = ci.TypeKind
C = ci.CursorKind

# Integer typedefs that hold pointers natively.
PTRINT = {"PtrU32", "uintptr_t", "intptr_t"}
# The records entity slots start with: a struct is an entity subtype if its first
# field (at offset 0) is one of these or another subtype.
ENTITY_BASES = {"struct Entity_", "GenericEntity"}
MANAGER_BASES = {"struct Manager"}
# EntityKind (include/entity.h) -> function table indexed by id.
KIND_TABLES = {3: "gEnemyFunctions", 4: "gProjectileFunctions", 6: "gObjectFunctions", 7: "gNPCFunctions",
               8: "gPlayerItemFunctions", 9: "gMiscManagerunctions"}
PLAYER_KIND, MANAGER_KIND = 1, 9
# What an empty slot holds, for each pool.
SLOT_TYPES = {"entity": "GenericEntity", "manager": "Temp"}

ZMALLOC_RE = re.compile(rb"\bzMalloc\s*\(")
SIZEOF_RE = re.compile(r"zMalloc\s*\(\s*sizeof\s*\(\s*(?:struct\s+)?(\w+)\s*(\[[^\]]*\])?\s*\)")
CAST_RE = re.compile(r"\(\s*(?:struct\s+)?(\w+)\s*\*\s*\)\s*zMalloc\s*\(")


def fnv32(name):
    h = 0x811C9DC5
    for b in name.encode():
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def desugar(t):
    """One step of typedef/elaborated sugar off t, or None."""
    if t.kind == K.ELABORATED:
        return t.get_named_type()
    if t.kind == K.TYPEDEF:
        return t.get_declaration().underlying_typedef_type
    return None


def is_ptrint(t):
    while t is not None:
        if t.kind == K.TYPEDEF and t.get_declaration().spelling in PTRINT:
            return True
        t = desugar(t)
    return False


def array_element(t):
    """Element type of an array type, keeping its typedefs (the canonical type drops them, and const)."""
    while t.kind not in (K.CONSTANTARRAY, K.INCOMPLETEARRAY):
        t = desugar(t)
    return t.element_type


def is_const(t):
    while True:
        if t.is_const_qualified():
            return True
        if t.kind in (K.CONSTANTARRAY, K.INCOMPLETEARRAY):
            t = t.element_type
        elif desugar(t) is not None:
            t = desugar(t)
        else:
            return t.get_canonical().is_const_qualified()


class Layouts:
    """Per translation unit: (size, pointer offsets, notes) of types. Notes are
    (relative path, what) for the ambiguous places."""

    def __init__(self, script_offset=None):
        self.cache = {}
        # GenericEntity.nativeScriptContext: ENTITY_SCRIPT_CONTEXT keeps a script
        # context there in any entity, whatever its subtype declares.
        self.script_offset = script_offset

    def of(self, t):
        key = t.spelling + "|" + t.get_canonical().spelling
        if key not in self.cache:
            self.cache[key] = self._of(t)
        return self.cache[key]

    def _of(self, t):
        c = t.get_canonical()
        if c.kind == K.POINTER:
            return (8, (0,), ())
        if c.kind in (K.CONSTANTARRAY, K.INCOMPLETEARRAY):
            esize, eptrs, enotes = self.of(array_element(t))
            count = c.element_count if c.kind == K.CONSTANTARRAY else 0
            ptrs = tuple(i * esize + p for i in range(count) for p in eptrs)
            notes = tuple((f"[]{p}", w) for p, w in enotes)
            return (esize * count, ptrs, notes)
        if c.kind == K.RECORD:
            return self.record(c)
        size = max(c.get_size(), 0)
        if size == 8 and c.kind != K.DOUBLE:
            if is_ptrint(t):
                return (8, (0,), ())
            return (8, (), (("", "64-bit integer taken for data"),))
        return (size, (), ())

    def record(self, c):
        union = c.get_declaration().kind == C.UNION_DECL
        members = []
        for f in c.get_fields():
            if f.is_bitfield():
                continue
            size, ptrs, notes = self.of(f.type)
            members.append((f.spelling or "(anonymous)", f.get_field_offsetof() // 8, size, ptrs, notes))
        ptrs = sorted({off + p for _, off, _, mp, _ in members for p in mp})
        notes = [(f".{name}{p}", w) for name, _, _, _, mn in members for p, w in mn]
        # A type that declares the script context's halves (PlayerEntity) holds it
        # there; others with data there only while scripted (see Slot below).
        so = self.script_offset
        if so is not None and subtype_root(c) == "entity" and so not in ptrs and \
                any(name.startswith("nativeScriptContext") and off <= so + 4 for name, off, _, _, _ in members):
            ptrs = sorted(ptrs + [so])
        if union:
            for name, off, size, mp, _ in members:
                for p in mp:
                    for other, ooff, osize, op, _ in members:
                        if other != name and off + p - ooff not in op and ooff < off + p + 8 and \
                                ooff + osize > off + p:
                            notes.append((f".{name}", f"pointer overlaps data in union member .{other}"))
                            break
        return (c.get_size(), tuple(ptrs), tuple(notes))


def has_data_at(t, lo, hi, base=0):
    """Whether type t, placed at base, keeps anything but a pointer at lo in [lo, hi)."""
    c = t.get_canonical()
    size = max(c.get_size(), 0)
    if base >= hi or base + size <= lo:
        return False
    if c.kind == K.POINTER:
        return base != lo
    if c.kind == K.RECORD:
        for f in c.get_fields():
            off = base + f.get_field_offsetof() // 8
            if f.is_bitfield():
                if lo <= off < hi or off <= lo < off + max(f.type.get_canonical().get_size(), 1):
                    return True
            elif has_data_at(f.type, lo, hi, off):
                return True
        return False
    if c.kind == K.CONSTANTARRAY:
        elem = c.element_type
        esize = max(elem.get_canonical().get_size(), 1)
        first = max(0, (lo - base) // esize)
        return any(has_data_at(elem, lo, hi, base + i * esize)
                   for i in range(first, min(c.element_count, (hi - base) // esize + 1)))
    return True


def subtype_root(c, depth=0):
    """'entity' or 'manager' if record type c starts with an entity or manager base."""
    c = c.get_canonical()
    if c.kind != K.RECORD or depth > 8:
        return None
    if c.spelling in ENTITY_BASES:
        return "entity"
    if c.spelling in MANAGER_BASES:
        return "manager"
    fields = list(c.get_fields())
    if not fields or fields[0].get_field_offsetof() != 0:
        return None
    return subtype_root(fields[0].type, depth + 1)


def base_record(c, depth=0):
    """The entity or manager base record a subtype c starts with."""
    c = c.get_canonical()
    if c.spelling in ENTITY_BASES | MANAGER_BASES or depth > 8:
        return c
    return base_record(next(iter(c.get_fields())).type, depth + 1)


def data_after(t, lo, base=0):
    """Whether type t, placed at base, keeps anything but pointers past byte lo."""
    c = t.get_canonical()
    if c.kind == K.POINTER or is_ptrint(t):
        return False
    if c.kind == K.RECORD:
        return any(f.is_bitfield() and base + f.get_field_offsetof() // 8 >= lo or
                   not f.is_bitfield() and data_after(f.type, lo, base + f.get_field_offsetof() // 8)
                   for f in c.get_fields())
    if c.kind == K.CONSTANTARRAY:
        esize = max(c.element_type.get_canonical().get_size(), 1)
        return any(data_after(c.element_type, lo, base + i * esize) for i in range(c.element_count))
    return base + max(c.get_size(), 0) > lo


def rel_location(c):
    """file:line of record type c's declaration, from include/ or src/ on."""
    loc = c.get_declaration().location
    path = os.path.normpath(loc.file.name) if loc.file else "?"
    cut = max(path.rfind("/include/"), path.rfind("/src/"))
    return f"{path[cut + 1:] if cut >= 0 else path}:{loc.line}"


def room_data_pointer(c, ptrs):
    """For a manager subtype: a pointer that moves its room data fields, or None.

    RegisterRoomEntity (src/room.c) lays a manager's 16-byte room data out from
    sizeof(Manager) + 0x10, where the GBA has it (0x30-0x3f) once the 8-byte
    header pointers are counted. A field the subtype declares there or after
    only lines up with it while no 8-byte pointer comes before it in the subtype's
    own fields (a pointer past the data, from sizeof(Manager) + 0x20, is fine)."""
    m = base_record(c).get_size()
    early = [p for p in ptrs if m <= p < m + 0x20]
    return early[0] if early and data_after(c, m + 0x10) else None


def designator_index(cur):
    for d in cur.walk_preorder():
        if d.kind == C.DECL_REF_EXPR and d.referenced is not None and d.referenced.kind == C.ENUM_CONSTANT_DECL:
            return d.referenced.enum_value
        if d.kind == C.INTEGER_LITERAL:
            return int(next(d.get_tokens()).spelling, 0)
    return None


def table_entries(var):
    """{index: [function names]} of a function table's initializer."""
    lists = [x for x in var.get_children() if x.kind == C.INIT_LIST_EXPR]
    if not lists:
        return None
    result = {}
    index = 0
    for entry in lists[0].get_children():
        refs = [d.referenced.spelling for d in entry.walk_preorder()
                if d.kind in (C.DECL_REF_EXPR, C.UNEXPOSED_EXPR) and d.referenced is not None
                and d.referenced.kind == C.FUNCTION_DECL]
        kids = list(entry.get_children())
        # [ID] = value: the designator comes first, an enum constant or a number.
        if len(kids) >= 2 and kids[0].kind not in (C.INIT_LIST_EXPR, C.TYPE_REF) and \
                designator_index(kids[0]) is not None:
            index = designator_index(kids[0])
        result[index] = sorted(set(refs))
        index += 1
    return result


def scan(job):
    path, args = job
    tu = ci.Index.create().parse(str(path), args=args, options=ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)
    fatal = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Fatal]
    if fatal:
        raise RuntimeError(f"{path}: {fatal[0].spelling}")
    main = str(path)
    text = Path(path).read_bytes()
    script_offset = None
    for cur in tu.cursor.get_children():
        if cur.kind == C.TYPEDEF_DECL and cur.spelling == "GenericEntity":
            script_offset = cur.underlying_typedef_type.get_canonical().get_offset("nativeScriptContext") // 8
            break
    lay = Layouts(script_offset)
    variables, functions, subtypes, tables, types, subtype_notes = {}, {}, {}, {}, {}, set()
    room_data = {}

    def note_subtype(t):
        c = t.get_canonical()
        if c.kind == K.POINTER:
            c = c.get_pointee().get_canonical()
        root = subtype_root(c)
        if root is not None and c.spelling not in ENTITY_BASES | MANAGER_BASES:
            size, ptrs, notes = lay.of(c)
            # Whether the script context word can be taken for a pointer whatever
            # the entity does, or only while it is scripted (its type keeps data there).
            script = root == "entity" and (script_offset in ptrs or
                                           not has_data_at(c, script_offset, script_offset + 8))
            subtypes[c.spelling] = (root, size, ptrs, script)
            if root == "manager":
                room_data[c.spelling] = (rel_location(c), room_data_pointer(c, ptrs))
            subtype_notes.update(f"{c.spelling}{p}: {w}" for p, w in notes)
            return c.spelling
        return None

    for cur in tu.cursor.get_children():
        here = cur.location.file is not None and cur.location.file.name == main
        if cur.kind == C.VAR_DECL:
            if cur.spelling in KIND_TABLES.values() and cur.is_definition():
                tables[cur.spelling] = table_entries(cur)
            if is_const(cur.type) or cur.type.get_canonical().kind in (K.FUNCTIONPROTO, K.FUNCTIONNOPROTO):
                continue
            size, ptrs, notes = lay.of(cur.type)
            elem = None
            if cur.type.get_canonical().kind in (K.CONSTANTARRAY, K.INCOMPLETEARRAY):
                esize, eptrs, _ = lay.of(array_element(cur.type))
                elem = (esize, eptrs)
            variables.setdefault(cur.spelling, []).append({"size": size, "ptrs": ptrs, "elem": elem,
                                                            "notes": notes, "type": cur.type.spelling})
        elif cur.kind == C.FUNCTION_DECL and here:
            params = list(cur.get_arguments())
            sub = note_subtype(params[0].type) if params else None
            for p in params[1:]:
                note_subtype(p.type)
            # With bodies skipped, a definition's extent stops before its "{".
            defined = text[cur.extent.end.offset:].lstrip()[:1] == b"{"
            functions[cur.spelling] = (sub, defined)
        elif cur.kind in (C.TYPEDEF_DECL, C.STRUCT_DECL, C.UNION_DECL):
            t = cur.underlying_typedef_type if cur.kind == C.TYPEDEF_DECL else cur.type
            if cur.kind == C.TYPEDEF_DECL or cur.is_definition():
                types.setdefault(cur.spelling, t)
            if here and t.get_canonical().kind == K.RECORD and t.get_canonical().get_declaration().is_definition():
                note_subtype(t)

    bases = {}
    for name in ("GenericEntity", "Temp", "PlayerEntity"):
        if name in types:
            size, ptrs, _ = lay.of(types[name])
            bases[name] = (size, ptrs, script_offset in ptrs)
    offsets = {}
    if "Entity" in types and script_offset is not None:
        e = types["Entity"].get_canonical()
        offsets = {f: e.get_offset(f) // 8 for f in ("kind", "id", "type", "flags")}
        offsets["script"] = script_offset

    # zMalloc calls: the type each allocates.
    heap = []
    # (Not in zMalloc's own files, which define ZMALLOC_DEFINITION.)
    if ZMALLOC_RE.search(text) and b"#define ZMALLOC_DEFINITION" not in text:
        for number, line in enumerate(text.decode(errors="replace").splitlines(), 1):
            if not ZMALLOC_RE.search(line.encode()) or re.match(r"\s*(//|\*|/\*)", line):
                continue
            m = SIZEOF_RE.search(line) or CAST_RE.search(line)
            if m is None:
                heap.append((Path(main).name, number, None, line.strip()))
                continue
            name = m.group(1)
            t = types.get(name)
            if t is None and name in ("u8", "u16", "u32", "s8", "s16", "s32", "int"):
                heap.append((Path(main).name, number, (name, 0, (), 0), line.strip()))  # no pointers
                continue
            if t is None:
                heap.append((Path(main).name, number, None, line.strip()))
                continue
            # An array (sizeof(T[n]), or a typedef of one) repeats its element over the block.
            total = lay.of(t)[0] if m.re is SIZEOF_RE else 0
            array = m.re is SIZEOF_RE and m.group(2) is not None
            if array:
                bound = m.group(2).strip("[] ")
                define = re.search(rb"#define\s+" + bound.encode() + rb"\s+(\w+)", text)
                bound = define.group(1).decode() if define else bound
                total = total * int(bound, 0) if re.fullmatch(r"0x[0-9a-fA-F]+|\d+", bound) else 0
            elif t.get_canonical().kind in (K.CONSTANTARRAY, K.INCOMPLETEARRAY):
                t, array = array_element(t), True
            size, ptrs, _ = lay.of(t)
            name = t.get_canonical().spelling + ("[]" if array else "")
            heap.append((Path(main).name, number, (name, size if array else 0, ptrs, (total + 3) & ~3), line.strip()))
    return {"file": main, "variables": variables, "functions": functions, "subtypes": subtypes,
            "tables": tables, "bases": bases, "offsets": offsets, "heap": heap,
            "subtype_notes": sorted(subtype_notes), "room_data": room_data}


def asm_table(path, label):
    """Function names of a .4byte table in GBA assembly data (gProjectileFunctions)."""
    if not path.exists():
        return {}
    names, active = [], False
    for line in path.read_text().splitlines():
        if line.startswith(label + "::"):
            active = True
            continue
        if active:
            m = re.match(r"\s*\.4byte\s+(\w+)", line)
            if m:
                names.append(m.group(1))
            elif line.strip() and not line.strip().startswith("@"):
                break
    return {i: [n] for i, n in enumerate(names)}


def load_hints(path):
    noptr, pools, subtypes = {}, {}, {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        words = line.split("#", 1)[0].split()
        if not words:
            continue
        if words[0] == "noptr" and len(words) == 2:
            noptr[words[1]] = line.split("#", 1)[1].strip() if "#" in line else ""
        elif words[0] == "pool" and len(words) == 3 and words[2] in ("entity", "manager", "heap"):
            pools[words[1]] = words[2]
        elif words[0] == "subtype" and len(words) in (4, 5):
            # subtype KIND ID [type=N] TYPE
            kind, id_ = int(words[1], 0), int(words[2], 0)
            type_ = int(words[3].split("=", 1)[1], 0) if len(words) == 5 and words[3].startswith("type=") else None
            subtypes[(kind, id_, type_)] = words[-1]
        else:
            sys.exit(f"{path}:{number}: cannot read '{line}'")
    return noptr, pools, subtypes


def runs(offsets):
    """Pointer offsets as (offset, stride, count) arithmetic runs."""
    offsets = sorted(set(offsets))
    out, i = [], 0
    while i < len(offsets):
        if i + 1 < len(offsets):
            stride = offsets[i + 1] - offsets[i]
            j = i + 1
            while j + 1 < len(offsets) and offsets[j + 1] - offsets[j] == stride:
                j += 1
            if j - i >= 2:
                out.append((offsets[i], stride, j - i + 1))
                i = j + 1
                continue
        out.append((offsets[i], 8, 1))
        i += 1
    return out


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--gba-build", type=Path, required=True)
    ap.add_argument("--sources", type=Path, required=True, help="file listing the state's C sources, one per line")
    ap.add_argument("--hints", type=Path, default=Path(__file__).with_name("statemap.txt"))
    ap.add_argument("--include", type=Path, action="append", default=[], help="more include directories")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()

    root = a.root.resolve()
    args = clang_args(root, a.gba_build.resolve()) + [f"-I{d.resolve()}" for d in a.include]
    files = [Path(line) for line in a.sources.read_text().split() if line]
    with Pool(a.jobs) as pool:
        units = pool.map(scan, [(f, args) for f in files], chunksize=8)
    noptr, pools, subtype_hints = load_hints(a.hints)
    errors, report = [], []

    variables, functions, subtypes, tables, bases, offsets, heap = {}, {}, {}, {}, {}, {}, []
    file_subtypes, subtype_notes, room_data = {}, set(), {}
    for u in units:
        for name, decls in u["variables"].items():
            for d in decls:
                variables.setdefault(name, {}).setdefault(json.dumps([d["ptrs"], d["elem"]]), d)
        for name, (sub, defined) in u["functions"].items():
            entry = functions.setdefault(name, {"sub": None, "file": None})
            entry["sub"] = entry["sub"] or sub
            if defined:
                entry["file"] = u["file"]
        subtypes.update(u["subtypes"])
        tables.update({k: v for k, v in u["tables"].items() if v is not None})
        bases.update(u["bases"])
        offsets = offsets or u["offsets"]
        heap += u["heap"]
        subtype_notes.update(u["subtype_notes"])
        room_data.update(u["room_data"])
        file_subtypes[u["file"]] = sorted(u["subtypes"])
    tables["gProjectileFunctions"] = asm_table(root / "data/projectile_functions.s", "gProjectileFunctions")
    for name in ("GenericEntity", "Temp", "PlayerEntity"):
        if name not in bases:
            errors.append(f"no layout for {name}")
    if set(offsets) != {"kind", "id", "type", "flags", "script"}:
        errors.append("no layout for Entity")

    # ---- manager subtypes against the slot and the room data -----------------------
    for name, (where, pointer) in sorted(room_data.items()):
        if "Temp" in bases and subtypes[name][1] > bases["Temp"][0]:
            errors.append(f"{where}: {name} is {subtypes[name][1]:#x} bytes, more than a manager slot "
                          f"(Temp in src/entity.c, {bases['Temp'][0]:#x})")
        if pointer is not None:
            errors.append(f"{where}: {name} has a pointer at {pointer:#x}, before fields at or after the room data "
                          "that src/room.c RegisterRoomEntity lays out at the GBA offsets 0x30-0x3f: they read "
                          "other bytes natively. Make it a u32 if it is only compared, or move it past the data "
                          "(port/notes/native-hazards.md, \"Pointers read at a GBA byte offset\")")

    # ---- slot and block types ------------------------------------------------------
    types, type_index = [], {}

    def index_of(name, ptrs=None, stride=0):
        if name not in type_index:
            script = False
            if ptrs is None:
                ptrs, script = (subtypes[name][2], subtypes[name][3]) if name in subtypes else bases[name][1:3]
            type_index[name] = len(types)
            types.append((name, ptrs, stride, script))
        return type_index[name]

    if not errors:
        index_of("GenericEntity")
        index_of("Temp")

    # ---- variables -------------------------------------------------------------------------
    out_vars = []
    for name in sorted(set(variables) | set(noptr) | set(pools)):
        decls = list(variables.get(name, {}).values())
        size = max([d["size"] for d in decls] + [0])
        if name in pools:
            out_vars.append((name, [], pools[name], size))
            continue
        offsets_, repeat = set(), None
        if len({json.dumps(d["ptrs"]) for d in decls if d["ptrs"]}) > 1 or \
                (len(decls) > 1 and any(d["ptrs"] for d in decls) and any(not d["ptrs"] for d in decls)):
            report.append(f"{name}: declared as {' and '.join(sorted(d['type'] for d in decls))}; a pointer field "
                          "in any of them counts")
        for d in decls:
            if d["elem"] is not None and d["elem"][1] and d["elem"][0] > 0:
                repeat = d["elem"] if repeat in (None, d["elem"]) else False
            offsets_.update(d["ptrs"])
            for path, what in d["notes"]:
                report.append(f"{name}{path}: {what}")
        if repeat and all(d["elem"] == repeat or (d["elem"] is None and not d["ptrs"]) for d in decls) and \
                all(d["elem"] == repeat for d in decls if d["ptrs"] or d["elem"]):
            # Arrays repeat to the end of their storage: code indexes past the
            # declared count into the rest of a GBA RAM region.
            out_vars.append((name, [(p, repeat[0], 0) for p in repeat[1]], None, size))
        else:
            out_vars.append((name, runs(offsets_), None, size))

    # ---- entity slots ------------------------------------------------------------------------
    kind_map, by_type, resolved = {}, [], []
    if not errors:
        for i in range(256):
            kind_map[(PLAYER_KIND, i)] = index_of("PlayerEntity")
    for kind, table in KIND_TABLES.items() if not errors else ():
        root_kind = "manager" if kind == MANAGER_KIND else "entity"
        for i, funcs in sorted(tables.get(table, {}).items()):
            hinted = {t: s for (k, id_, t), s in subtype_hints.items() if (k, id_) == (kind, i)}
            if hinted:
                for t, s in sorted(hinted.items(), key=lambda x: (x[0] is not None, x[0] or 0)):
                    if s not in subtypes and s not in bases:
                        errors.append(f"statemap.txt: subtype {kind} {i}: no struct {s}")
                        continue
                    if t is None:
                        kind_map[(kind, i)] = index_of(s)
                    else:
                        by_type.append((kind, i, t, index_of(s)))
                    resolved.append(f"kind {kind} id {i:3d}{'' if t is None else f' type {t}'}: {s} (statemap.txt)")
                if None not in hinted:
                    kind_map[(kind, i)] = index_of(SLOT_TYPES[root_kind])
                continue
            cands = set()
            for f in funcs:
                info = functions.get(f)
                if info is None:
                    continue
                if info["sub"] is not None and subtypes[info["sub"]][0] == root_kind:
                    cands.add(info["sub"])
                elif info["file"] is not None:
                    cands.update(s for s in file_subtypes[info["file"]] if subtypes[s][0] == root_kind)
            if not cands:
                choice, why = SLOT_TYPES[root_kind], "no subtype"
            elif len({subtypes[s][2] for s in cands}) == 1:
                choice, why = sorted(cands)[0], "from " + ", ".join(funcs)
            else:
                errors.append(f"kind {kind} id {i} ({', '.join(funcs)}) can hold {', '.join(sorted(cands))}, with "
                              "different pointer fields: say which in statemap.txt (subtype KIND ID [type=N] TYPE)")
                continue
            kind_map[(kind, i)] = index_of(choice)
            resolved.append(f"kind {kind} id {i:3d}: {choice} ({why})")

    # ---- heap blocks ------------------------------------------------------------------------------
    sites = []
    for file, line, t, text in sorted(heap):
        if t is None:
            errors.append(f"{file}:{line}: cannot tell what this zMalloc allocates ({text})")
            continue
        name, stride, ptrs, size = t
        index_of(name, ptrs, stride)
        sites.append((file, line, name, size))
        resolved.append(f"heap {file}:{line}: {name}")

    if errors:
        sys.exit("statemap: " + "\n statemap: ".join(errors))

    used = {t[0] for t in types}
    report += [n for n in subtype_notes if n.split(".", 1)[0].split("[", 1)[0] in used]

    # ---- output -------------------------------------------------------------------------------------
    run_list = []

    def add_runs(rs):
        first = len(run_list)
        run_list.extend(rs)
        return first, len(rs)

    pool_c = {"entity": "STATE_POOL_ENTITY", "manager": "STATE_POOL_MANAGER", "heap": "STATE_POOL_HEAP",
              None: "STATE_POOL_NONE"}
    var_rows = []
    for name, rs, pool, size in out_vars:
        first, count = add_runs(rs)
        var_rows.append(f'    {{ "_{name}", {first}, {count}, {pool_c[pool]}, {size} }},')
    type_rows = []
    for name, ptrs, stride, script in types:
        first, count = add_runs([(p, stride, 0) for p in ptrs] if stride else runs(ptrs))
        type_rows.append(f"    {{ {first}, {count}, 0x{fnv32(name):08x}u, {int(script)}, {c_string(name)} }},")
        if not script and name in subtypes and subtypes[name][0] == "entity":
            report.append(f"{name}: keeps data where ENTITY_SCRIPT_CONTEXT keeps a pointer; taken for one only "
                          "while the entity is scripted (ENT_SCRIPTED)")
    subtype_rows = []
    for kind in range(10):
        empty = type_index["Temp"] if kind == MANAGER_KIND else type_index["GenericEntity"]
        row = [kind_map.get((kind, i), empty) for i in range(256)]
        subtype_rows.append("    { " + ", ".join(map(str, row)) + " },")
    by_type_rows = [f"    {{ {k}, {i}, {t}, {x} }}," for k, i, t, x in by_type] or ["    { 0, 0, 0, 0 },"]
    site_rows = [f"    {{ {c_string(f)}, {line}, 0x{fnv32(n):08x}u, {size} }}," for f, line, n, size in sites] or \
        ['    { "", 0, 0, 0 },']

    lines = [
        "// Generated by port/tools/datagen/statemap.py from the C types; do not edit.",
        "// Which bytes of the save state segment can hold a pointer (port/include/port/state_map.h).",
        "",
        '#include "port/state_map.h"',
        "",
        "const StateMapRun gStateMapRuns[] = {",
        *[f"    {{ {o}, {s}, {c} }}," for o, s, c in run_list],
        "    { 0, 0, 0 },",
        "};",
        "",
        "const StateMapVar gStateMapVars[] = {",
        *var_rows,
        "};",
        "const uint32_t gStateMapVarCount = sizeof(gStateMapVars) / sizeof(gStateMapVars[0]);",
        "",
        "const StateMapType gStateMapTypes[] = {",
        *type_rows,
        "};",
        "const uint32_t gStateMapTypeCount = sizeof(gStateMapTypes) / sizeof(gStateMapTypes[0]);",
        "",
        "const uint16_t gStateMapSubtype[10][256] = {",
        *subtype_rows,
        "};",
        "",
        "const StateMapByType gStateMapByType[] = {",
        *by_type_rows,
        "};",
        f"const uint32_t gStateMapByTypeCount = {len(by_type)};",
        "",
        "const StateMapHeapSite gStateMapHeapSites[] = {",
        *site_rows,
        "};",
        f"const uint32_t gStateMapHeapSiteCount = {len(sites)};",
        "",
        f"const uint32_t gStateMapEntitySlot = {bases['GenericEntity'][0]}, "
        f"gStateMapManagerSlot = {bases['Temp'][0]};",
        f"const uint32_t gStateMapKindOffset = {offsets['kind']}, gStateMapIdOffset = {offsets['id']}, "
        f"gStateMapTypeOffset = {offsets['type']}, gStateMapFlagsOffset = {offsets['flags']}, "
        f"gStateMapScriptOffset = {offsets['script']};",
        "",
    ]
    a.out.write_text("\n".join(lines))
    a.out.with_suffix(".txt").write_text("\n".join([
        "# Save state pointer map: its decisions and the ambiguous places (port/tools/datagen/statemap.py)", "",
        "## Ambiguous", *sorted(set(report)), "", "## Entity slots and heap blocks", *resolved, ""]))
    print(f"statemap: {len(out_vars)} variables, {len(types)} slot and block types, {len(sites)} heap sites")


if __name__ == "__main__":
    main()
