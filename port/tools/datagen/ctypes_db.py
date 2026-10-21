"""C type information for data symbols, with GBA (agbcc) and native (arm64) layouts.

Types are read with libclang from the game sources and flattened into plain
tuples so they can be merged across translation units:

    ("int", gba_size, native_size)      scalars, enums
    ("ptr", pointee)                    pointee is a type tuple, or ("func",)
    ("array", elem, count)              count is None for incomplete arrays
    ("record", key)                     key into TypeDB.records
    ("func",)
    ("void",)

agbcc differs from arm64 clang in two ways that matter for data: pointers and
`long` are 4 bytes, and every non-packed struct/union has its size and alignment
rounded up to 4 (APCS structure-size-boundary).
"""

import json
import os
import subprocess
from dataclasses import dataclass
from multiprocessing import Pool
from pathlib import Path

import clang.cindex as ci

K = ci.TypeKind

_SCALARS = {
    K.BOOL: (1, 1), K.CHAR_U: (1, 1), K.UCHAR: (1, 1), K.CHAR_S: (1, 1), K.SCHAR: (1, 1),
    K.USHORT: (2, 2), K.SHORT: (2, 2), K.UINT: (4, 4), K.INT: (4, 4),
    K.ULONG: (4, 8), K.LONG: (4, 8), K.ULONGLONG: (8, 8), K.LONGLONG: (8, 8),
    K.FLOAT: (4, 4), K.DOUBLE: (8, 8),
}


def clang_args(root: Path, assets_dir: Path, version: str = "USA") -> list:
    # libclang's bundled library does not know the SDK or its own builtin headers;
    # without them u16 & co. silently degrade to int.
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=True).stdout.strip()
    resource_dir = subprocess.run(["clang", "-print-resource-dir"], capture_output=True, text=True, check=True).stdout.strip()
    return [
        "-target", "arm64-apple-macos13", "-std=gnu11", "-w",
        "-isysroot", sdk, "-resource-dir", resource_dir,
        "-DPLATFORM_NATIVE=1", "-DNON_MATCHING=1", f"-D{version}", "-DENGLISH", "-DREVISION=0",
        f"-I{root}/include", f"-I{assets_dir}", f"-I{root}/port/include",
    ]


class _Flattener:
    def __init__(self):
        self.records = {}

    def type(self, t):
        t = t.get_canonical()
        kind = t.kind
        if kind in _SCALARS:
            gba, native = _SCALARS[kind]
            return ("int", gba, native)
        if kind == K.ENUM:
            return ("int", 4, t.get_size())
        if kind == K.VOID:
            return ("void",)
        if kind == K.POINTER:
            pointee = t.get_pointee().get_canonical()
            if pointee.kind in (K.FUNCTIONPROTO, K.FUNCTIONNOPROTO):
                return ("ptr", ("func",))
            return ("ptr", self.type(pointee))
        if kind == K.CONSTANTARRAY:
            return ("array", self.type(t.element_type), t.element_count)
        if kind == K.INCOMPLETEARRAY:
            return ("array", self.type(t.element_type), None)
        if kind in (K.FUNCTIONPROTO, K.FUNCTIONNOPROTO):
            return ("func",)
        if kind == K.RECORD:
            return ("record", self.record(t))
        raise ValueError(f"unsupported type {t.spelling} ({kind})")

    def record(self, t):
        decl = t.get_declaration()
        key = t.spelling
        if key in self.records:
            return key
        self.records[key] = None  # break cycles through pointers
        packed = any(c.kind == ci.CursorKind.PACKED_ATTR for c in decl.get_children())
        fields = []
        for f in t.get_fields():
            width = f.get_bitfield_width() if f.is_bitfield() else None
            fields.append((f.spelling, self.type(f.type), width, t.get_offset(f.spelling)))
        self.records[key] = {
            "union": decl.kind == ci.CursorKind.UNION_DECL,
            "packed": packed,
            "fields": fields,
            "native_size": t.get_size(),
            "native_align": t.get_align(),
        }
        return key


def _scan(job):
    path, args = job
    tu = ci.Index.create().parse(str(path), args=args, options=ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)
    # A fatal error (e.g. a missing header) means types were guessed; plain errors such
    # as the known non-constant initializers do not affect declarations.
    fatal = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Fatal]
    if fatal:
        raise RuntimeError(f"{path}: {fatal[0].spelling}")
    flat = _Flattener()
    decls = {}
    for cur in tu.cursor.get_children():
        try:
            if cur.kind == ci.CursorKind.VAR_DECL:
                decls[cur.spelling] = flat.type(cur.type)
            elif cur.kind == ci.CursorKind.TYPEDEF_DECL or (
                cur.kind in (ci.CursorKind.STRUCT_DECL, ci.CursorKind.UNION_DECL) and cur.is_definition()
            ):
                # File-local structs too, so hints can name types no variable is declared with.
                t = cur.underlying_typedef_type if cur.kind == ci.CursorKind.TYPEDEF_DECL else cur.type
                if t.get_canonical().kind == K.RECORD and t.get_canonical().get_declaration().is_definition():
                    flat.type(t)
        except ValueError:
            pass
    return decls, flat.records


def _to_json(obj):
    return json.loads(json.dumps(obj))


@dataclass
class Layout:
    size: int
    align: int


def freeze(t):
    """Type descriptions as nested tuples, so they can key the layout caches."""
    if isinstance(t, list):
        return tuple(freeze(x) for x in t)
    return t


class TypeDB:
    def __init__(self, decls: dict, records: dict):
        self.decls = {name: freeze(t) for name, t in decls.items()}
        self.records = {}
        for key, rec in records.items():
            rec = dict(rec)
            rec["fields"] = tuple((name, freeze(ft), width, bits) for name, ft, width, bits in rec["fields"])
            self.records[key] = rec
        self._layout_cache = {}

    @classmethod
    def build(cls, root: Path, assets_dir: Path, jobs: int = max(1, (os.cpu_count() or 2) // 2)) -> "TypeDB":
        args = clang_args(root, assets_dir)
        files = sorted((root / "src").rglob("*.c"))
        decls, records = {}, {}
        with Pool(jobs) as pool:
            for d, r in pool.imap_unordered(_scan, [(f, args) for f in files], chunksize=8):
                for name, t in _to_json(d).items():
                    # Prefer the most specific declaration (complete arrays over incomplete ones).
                    if name not in decls or _specificity(t) > _specificity(decls[name]):
                        decls[name] = t
                for key, rec in _to_json(r).items():
                    if rec is not None:
                        records.setdefault(key, rec)
        return cls(decls, records)

    def save(self, path: Path):
        path.write_text(json.dumps({"decls": self.decls, "records": self.records}))

    @classmethod
    def load(cls, path: Path) -> "TypeDB":
        d = json.loads(path.read_text())
        return cls(d["decls"], d["records"])

    # ---- layout -----------------------------------------------------------

    def layout(self, t, target: str) -> Layout:
        key = (t, target)
        if key not in self._layout_cache:
            self._layout_cache[key] = self._layout(t, target)
        return self._layout_cache[key]

    def _layout(self, t, target):
        kind = t[0]
        if kind == "int":
            size = t[1] if target == "gba" else t[2]
            return Layout(size, min(size, 4) if target == "gba" else size)
        if kind == "ptr":
            return Layout(4, 4) if target == "gba" else Layout(8, 8)
        if kind == "array":
            elem = self.layout(t[1], target)
            return Layout(elem.size * (t[2] or 0), elem.align)
        if kind == "record":
            return self.record_layout(t[1], target)[0]
        raise ValueError(f"no layout for {t}")

    def record_layout(self, key, target):
        """(Layout, [(field name, type, offset, bit width, bit offset)]) for a record."""
        cache_key = ("rec", key, target)
        if cache_key in self._layout_cache:
            return self._layout_cache[cache_key]
        rec = self.records[key]
        if target == "native":
            fields = [
                (name, ft, (bits // 8) if width is None else None, width, bits if width is not None else None)
                for name, ft, width, bits in rec["fields"]
            ]
            result = (Layout(rec["native_size"], rec["native_align"]), fields)
        else:
            result = self._gba_record(rec)
        self._layout_cache[cache_key] = result
        return result

    def _gba_record(self, rec):
        packed = rec["packed"]
        align = 1 if packed else 4
        offset_bits = 0
        size_bits = 0
        fields = []
        for name, ft, width, _ in rec["fields"]:
            lay = self.layout(ft, "gba")
            falign = 1 if packed else lay.align
            if width is not None:
                unit = lay.size * 8
                if width == 0:
                    offset_bits = -(-offset_bits // unit) * unit
                    continue
                if not packed and (offset_bits // unit) != ((offset_bits + width - 1) // unit):
                    offset_bits = -(-offset_bits // unit) * unit
                bit = 0 if rec["union"] else offset_bits
                fields.append((name, ft, None, width, bit))
                if not rec["union"]:
                    offset_bits += width
                size_bits = max(size_bits, bit + width)
                align = max(align, falign)
                continue
            if rec["union"]:
                fields.append((name, ft, 0, None, None))
                size_bits = max(size_bits, lay.size * 8)
            else:
                offset_bits = -(-offset_bits // (falign * 8)) * (falign * 8)
                fields.append((name, ft, offset_bits // 8, None, None))
                offset_bits += lay.size * 8
                size_bits = offset_bits
            align = max(align, falign)
        if packed:
            # aligned(n) on a packed struct (FORCE_WORD_ALIGNED) shows up as its native alignment.
            align = max(align, min(rec["native_align"], 4))
        size = -(-(-(-size_bits // 8)) // align) * align
        return Layout(size, align), fields

    def contains_pointer(self, t, rec=None) -> bool:
        if rec is not None:
            return any(self.contains_pointer(ft) for _, ft, _, _ in rec["fields"])
        kind = t[0]
        if kind == "ptr":
            return True
        if kind == "array":
            return self.contains_pointer(t[1])
        if kind == "record":
            return self.contains_pointer(None, self.records[t[1]])
        return False

    def same_layout(self, t) -> bool:
        """True when GBA bytes can be used natively as-is for this type."""
        kind = t[0]
        if kind in ("ptr", "func", "void"):
            return False
        if kind == "int":
            return t[1] == t[2]
        if kind == "array":
            return self.same_layout(t[1])
        gba_lay, gba_fields = self.record_layout(t[1], "gba")
        nat_lay, nat_fields = self.record_layout(t[1], "native")
        if gba_lay.size != nat_lay.size:
            return False
        for g, n in zip(gba_fields, nat_fields):
            if g[2] != n[2] or g[4] != n[4] or not self.same_layout(g[1]):
                return False
        return True


def _specificity(t) -> int:
    if t[0] == "array":
        return (2 if t[2] is not None else 1) + _specificity(t[1])
    if t[0] in ("record", "ptr"):
        return 3
    return 0
