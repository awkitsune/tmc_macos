"""Index the first statement after each label in the data assembly sources.

Data tables are written with typed macros (entity lists, map definitions, ...),
so the macro a label starts with identifies its C type even when no C code
declares the label.
"""

import re
from pathlib import Path

LABEL_RE = re.compile(r"^([A-Za-z_][\w.]*)::?")
SKIP_RE = re.compile(r"^\.(if|ifdef|ifndef|else|elseif|endif|align|balign|p2align|global|globl|section)\b")


def first_statements(root: Path) -> dict:
    """label name -> first macro or directive name used after it."""
    result = {}
    for path in sorted((root / "data").rglob("*")):
        if path.suffix not in (".s", ".inc") or not path.is_file():
            continue
        pending = []
        for raw in path.read_text(errors="replace").splitlines():
            line = raw.split("@", 1)[0].strip()
            if not line:
                continue
            m = LABEL_RE.match(line)
            if m:
                pending.append(m.group(1))
                line = line[m.end() :].strip()
                if not line:
                    continue
            if SKIP_RE.match(line):
                continue
            if pending:
                word = line.split()[0]
                for name in pending:
                    result.setdefault(name, word)
                pending = []
    return result
