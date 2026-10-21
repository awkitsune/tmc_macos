#!/usr/bin/env python3
"""Summarize unresolved symbols from a failed native link.

Each undefined symbol is attributed to where the GBA build defines it (asm code,
asm libraries, data tables, linker script, or C files not yet built natively), so
the output reads as the remaining native-port worklist.

usage: link_report.py <link.log> <repo root> <extracted assets dir>
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

LABEL_RE = re.compile(r"^\s*([A-Za-z_][\w.]*)::?", re.M)
SET_RE = re.compile(r"^\s*\.(?:set|equ)\s+([A-Za-z_]\w*)\s*,", re.M)
FUNC_RE = re.compile(r"^\s*(?:thumb_func_start|arm_func_start|non_word_aligned_thumb_func_start)\s+(\w+)", re.M)
SCRIPT_RE = re.compile(r"^\s*SCRIPT_START\s+(\w+)", re.M)
LD_ASSIGN_RE = re.compile(r"\b([A-Za-z_]\w*)\s*=\s*[^;=]+;")
C_DEF_RE = r"^[^\s#/][^;(]*\b{}\b\s*(\[|=|\()"


def category_for(path: Path, root: Path) -> str:
    rel = path.relative_to(root).as_posix()
    if rel.startswith("asm/lib/"):
        return f"asm library ({path.stem})"
    if rel.startswith("asm/"):
        return "asm code"
    if rel.startswith("data/"):
        return "data tables (asm)"
    if rel.startswith("sound/"):
        return "sound data (asm)"
    return rel


def index_assets(assets: Path, defs: dict) -> None:
    for path in sorted(assets.rglob("*.s")):
        kind = "songs (generated asm)" if path.parent.name == "sounds" else "assets (generated asm)"
        for name in LABEL_RE.findall(path.read_text(errors="replace")):
            defs.setdefault(name, kind)


def index_definitions(root: Path) -> dict:
    defs = {}
    for base in ("asm", "data", "sound"):
        for path in sorted((root / base).rglob("*")):
            if path.suffix not in (".s", ".inc") or not path.is_file():
                continue
            text = path.read_text(errors="replace")
            cat = category_for(path, root)
            for rx in (FUNC_RE, SCRIPT_RE, SET_RE, LABEL_RE):
                for name in rx.findall(text):
                    defs.setdefault(name, cat)
    for name in LD_ASSIGN_RE.findall((root / "linker.ld").read_text()):
        defs.setdefault(name, "linker script (fixed address)")
    return defs


def parse_undefined(log: str) -> list:
    syms = []
    in_block = False
    for line in log.splitlines():
        if line.startswith("Undefined symbols for architecture"):
            in_block = True
            continue
        if in_block:
            m = re.match(r'\s+"_?([^"]+)", referenced from:', line)
            if m:
                syms.append(m.group(1))
            elif line.startswith("ld:"):
                in_block = False
    return syms


def main() -> int:
    log_path, root, assets = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    log = log_path.read_text(errors="replace")
    undefined = parse_undefined(log)
    if not undefined:
        if "error" in log:
            print(log)
            return 1
        print("tmc linked with no unresolved symbols.")
        return 0

    defs = index_definitions(root)
    index_assets(assets / "assets", defs)
    cmake = (root / "port" / "CMakeLists.txt").read_text()
    pending_block = re.search(r"set\(TMC_NATIVE_PENDING(.*?)\)", cmake, re.S).group(1)
    pending_c = [root / p for p in re.sub(r"#.*", "", pending_block).split()]
    groups = defaultdict(list)
    for sym in sorted(set(undefined)):
        cat = defs.get(sym)
        if cat is None:
            pattern = re.compile(C_DEF_RE.format(re.escape(sym)), re.M)
            for path in pending_c:
                if pattern.search(path.read_text(errors="replace")):
                    cat = f"C pending native build ({path.relative_to(root).as_posix()})"
                    break
        groups[cat or "unknown"].append(sym)

    print(f"{len(set(undefined))} unresolved symbols\n")
    for cat, syms in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print(f"{len(syms):5d}  {cat}")
    report = log_path.with_name("link_report.txt")
    with report.open("w") as out:
        for cat, syms in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            out.write(f"## {cat} ({len(syms)})\n")
            out.writelines(f"{s}\n" for s in syms)
            out.write("\n")
    print(f"\nfull list: {report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
