"""Which bytes of the GBA data objects come from baserom.gba.

Asset extraction (tools/asset_processor) copies the game's graphics, maps,
sound and text out of baserom.gba into build/<version>/assets, and the data
sources .incbin them. Everything else in a data object is written in the
repository's sources (macros, .byte lists, ...).

To tell the two apart, every data object is assembled twice more, with each
extracted asset file swapped for a file of the same size filled with FILLS[0],
then FILLS[1]. A byte is asset content exactly when it differs between the two.
The generated .s files among the assets only wrap .incbin and stay as they are.
"""

import os
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from .gbaobj import load_object

FILLS = (0x55, 0xAA)
TEXT_SUFFIXES = {".s", ".inc", ".h", ".txt"}
ASFLAGS = ["-mcpu=arm7tdmi", "--defsym", "USA=1", "--defsym", "REVISION=0", "--defsym", "ENGLISH=1"]


def _variant_tree(root: Path, gba_build: Path, vroot: Path, fill: int):
    """A copy of the source tree (by symlinks) whose extracted assets hold only fill bytes."""
    if vroot.exists():
        shutil.rmtree(vroot)
    vroot.mkdir(parents=True)
    for entry in root.iterdir():
        if entry.name != "build":
            (vroot / entry.name).symlink_to(entry)
    vbuild = vroot / gba_build.relative_to(root)
    vbuild.mkdir(parents=True)
    (vbuild / "enum_include").symlink_to(gba_build / "enum_include")
    assets = gba_build / "assets"
    for dirpath, _, files in os.walk(assets):
        out = vbuild / "assets" / Path(dirpath).relative_to(assets)
        out.mkdir(parents=True, exist_ok=True)
        for name in files:
            src = Path(dirpath) / name
            if src.suffix in TEXT_SUFFIXES:
                (out / name).symlink_to(src)
            else:
                (out / name).write_bytes(bytes([fill]) * src.stat().st_size)


def _assemble(vroot: Path, gba_build_rel: Path, source: Path, out: Path):
    inc = ["-I", str(gba_build_rel / "assets"), "-I", str(gba_build_rel / "enum_include")]
    out.parent.mkdir(parents=True, exist_ok=True)
    pre = subprocess.run(["tools/bin/preproc", "tmc", str(source), "--", *inc], cwd=vroot, capture_output=True, check=True)
    subprocess.run(["arm-none-eabi-as", *ASFLAGS, *inc, "-o", str(out)], input=pre.stdout, cwd=vroot,
                   capture_output=True, check=True)


def asset_masks(root: Path, gba_build: Path, objects: list, work: Path) -> dict:
    """(object rel, section name) -> bytes, 1 where the section's byte is extracted asset content."""
    gba_build_rel = gba_build.relative_to(root)
    sources = {}  # object file -> its .s, relative to the root
    for obj in objects:
        sources[obj.path] = obj.path.relative_to(gba_build).with_suffix(".s")
    variants = []
    for fill in FILLS:
        vroot = work / f"fill_{fill:02x}"
        _variant_tree(root, gba_build, vroot, fill)
        variants.append(vroot)
    jobs = [(v, src, v / "out" / src.with_suffix(".o")) for v in variants for src in sorted(set(sources.values()))]
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        for future in [pool.submit(_assemble, v, gba_build_rel, src, out) for v, src, out in jobs]:
            future.result()

    masks = {}
    loaded = {}
    for obj in objects:
        src = sources[obj.path]
        pair = []
        for v in variants:
            key = (v, src)
            if key not in loaded:
                prefixes = (".rodata", ".data", ".text")
                loaded[key] = {s.name: s.data for s in load_object(v / "out" / src.with_suffix(".o"), v / "out", prefixes).sections}
            pair.append(loaded[key])
        start = getattr(obj, "region_start", None)
        for sec in obj.sections:
            name = ".text" if start is not None else sec.name
            a, b = pair[0][name], pair[1][name]
            if start is not None:
                a, b = a[start : start + len(sec.data)], b[start : start + len(sec.data)]
            if len(a) != len(sec.data) or len(b) != len(sec.data):
                raise ValueError(f"{obj.rel}{sec.name}: reassembled size differs")
            masks[(obj.rel, sec.name)] = bytes(x != y for x, y in zip(a, b))
    for v in variants:
        shutil.rmtree(v)
    return masks
