"""Read assembled GBA data objects: sections, labels and pointer relocations."""

import bisect
from dataclasses import dataclass, field
from pathlib import Path

from elftools.elf.elffile import ELFFile

R_ARM_ABS32 = 2
R_ARM_THM_CALL = 10


@dataclass
class Reloc:
    offset: int  # within the section
    symbol: str  # target symbol name ("" for a section-relative target)
    addend: int  # implicit addend read from the data word
    section: str = ""  # target section when the reloc goes through a section symbol
    kind: int = R_ARM_ABS32


@dataclass
class Label:
    name: str
    offset: int
    end: int
    is_global: bool
    section: "Section" = field(repr=False, default=None)

    @property
    def size(self) -> int:
        return self.end - self.offset


@dataclass
class Section:
    obj: "DataObject" = field(repr=False)
    name: str
    data: bytes
    align: int
    labels: list
    relocs: dict  # offset -> Reloc
    base: int = 0  # ROM address of the section in the linked ELF

    def has_relocs_in(self, start: int, end: int) -> bool:
        if getattr(self, "_sorted_relocs", None) is None or len(self._sorted_relocs) != len(self.relocs):
            self._sorted_relocs = sorted(self.relocs)
        i = bisect.bisect_left(self._sorted_relocs, start)
        return i < len(self._sorted_relocs) and self._sorted_relocs[i] < end

    def word_aligned(self, offset: int) -> bool:
        return (self.base + offset) % 4 == 0

    def label_at(self, offset: int) -> Label:
        """The label whose range contains offset (the last one at or before it)."""
        best = None
        for label in self.labels:
            if label.offset <= offset:
                best = label
            else:
                break
        return best


@dataclass
class DataObject:
    path: Path
    rel: str  # path relative to the build dir, used for output naming
    sections: list


def _is_label(sym) -> bool:
    return (
        sym.name
        and not sym.name.startswith("$")
        and sym["st_info"]["type"] in ("STT_NOTYPE", "STT_OBJECT", "STT_FUNC")
        and isinstance(sym["st_shndx"], int)
    )


def load_object(path: Path, build_dir: Path, section_prefixes=(".rodata", ".data", ".text")) -> DataObject:
    obj = DataObject(path=path, rel=path.relative_to(build_dir).with_suffix("").as_posix(), sections=[])
    with open(path, "rb") as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name(".symtab")
        symbols = list(symtab.iter_symbols())
        sections = list(elf.iter_sections())
        for idx, sec in enumerate(sections):
            if sec["sh_type"] != "SHT_PROGBITS" or not sec.name.startswith(section_prefixes) or sec["sh_size"] == 0:
                continue
            data = sec.data()
            offsets = sorted(
                {(s["st_value"], s.name, s["st_info"]["bind"] == "STB_GLOBAL") for s in symbols if s["st_shndx"] == idx and _is_label(s)}
            )
            labels = []
            for i, (off, name, glob) in enumerate(offsets):
                end = next((o for o, _, _ in offsets[i + 1 :] if o > off), sec["sh_size"])
                labels.append(Label(name, off, end, glob))
            section = Section(obj=obj, name=sec.name, data=data, align=sec["sh_addralign"], labels=labels, relocs={})
            for label in labels:
                label.section = section
            rel = elf.get_section_by_name(".rel" + sec.name)
            if rel is not None:
                for r in rel.iter_relocations():
                    sym = symbols[r["r_info_sym"]]
                    off = r["r_offset"]
                    kind = r["r_info_type"]
                    addend = int.from_bytes(data[off : off + 4], "little", signed=True) if kind == R_ARM_ABS32 else 0
                    if sym["st_info"]["type"] == "STT_SECTION":
                        section.relocs[off] = Reloc(off, "", addend, section=sections[sym["st_shndx"]].name, kind=kind)
                    else:
                        section.relocs[off] = Reloc(off, sym.name, addend, kind=kind)
            obj.sections.append(section)
    return obj


def linked_symbols(elf_path: Path) -> dict:
    """name -> address for every symbol in the linked GBA ELF."""
    with open(elf_path, "rb") as f:
        return {s.name: s["st_value"] for s in ELFFile(f).get_section_by_name(".symtab").iter_symbols() if s.name}


def load_all(build_dir: Path, addrs: dict, subdir: str = "data") -> list:
    objects = [load_object(p, build_dir) for p in sorted((build_dir / subdir).rglob("*.o"))]
    _assign_bases(objects, addrs)
    return objects


def _assign_bases(objects: list, addrs: dict):
    """Find each section's ROM address from a global label in the linked ELF."""
    for obj in objects:
        for sec in obj.sections:
            for label in sec.labels:
                if label.is_global and label.name in addrs:
                    sec.base = (addrs[label.name] & ~1 if sec.name.startswith(".text") else addrs[label.name]) - label.offset
                    break
            else:
                raise ValueError(f"{obj.rel}{sec.name}: no global label to locate it in the linked ELF")
