#!/usr/bin/env python3
"""Compare two save states (port/src/state.c format) variable by variable.

    port/scripts/state_diff.py A.tmcs B.tmcs [--bytes N]
    port/scripts/state_diff.py --check-pointers A.tmcs B.tmcs

Prints each variable whose bytes differ, with the first differing offsets.
Pointers are compared by what they point at (the states' pointer lists), so
states from runs at different addresses, or from different builds, compare.
With one file, prints the header and the largest variables instead.

--check-pointers checks the pointer lists themselves: A and B are states of the
same frame of the same run made by one build at two load addresses (address
randomization). Every word that moved with the program must be listed in both,
at the same target; any other difference is reported too (nondeterminism).
"""
import argparse
import struct
import sys
import zlib

HEADER_V1 = struct.Struct("<8sIII16s4xQQQII")
HEADER_V2_EXTRA = struct.Struct("<QII")


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, header_size, flags, uuid, base, frame, checksum, body_size, packed_size = \
        HEADER_V1.unpack_from(data)
    if magic != b"TMCSTATE":
        sys.exit(f"{path}: not a save state")
    image_size = cpu = 0
    if version >= 2:
        image_size, cpu, _ = HEADER_V2_EXTRA.unpack_from(data, HEADER_V1.size)
    body = zlib.decompress(data[header_size:header_size + packed_size])
    pos = 0

    def take(fmt):
        nonlocal pos
        (v,) = struct.unpack_from(fmt, body, pos)
        pos += struct.calcsize(fmt)
        return v

    def name():
        nonlocal pos
        n = take("<H")
        s = body[pos:pos + n].decode()
        pos += n
        return s

    size = take("<I")
    segment = body[pos:pos + size]
    pos += size
    symbols = [(take("<I"), name()) for _ in range(take("<I"))]
    pointers = {}  # segment offset -> (target name, offset from it)
    if version == 1:
        for _ in range(take("<I")):
            offset, delta = take("<I"), take("<Q")
            pointers[offset] = (name(), delta)
    else:
        targets = [name() for _ in range(take("<I"))]
        for _ in range(take("<I")):
            offset, target, delta = take("<I"), take("<I"), take("<Q")
            pointers[offset] = (targets[target], delta)
    return {
        "version": version, "flags": flags, "uuid": uuid.hex(), "base": base, "image_size": image_size,
        "cpu": cpu, "frame": frame, "checksum": checksum, "segment": segment, "symbols": symbols,
        "pointers": pointers,
    }


def extents(state):
    """{name: (start, end)}; a variable runs to the next address (aliases share it)."""
    syms = state["symbols"]
    starts = sorted({o for o, _ in syms}) + [len(state["segment"])]
    following = {starts[i]: starts[i + 1] for i in range(len(starts) - 1)}
    return {n: (o, following[o]) for o, n in syms}


def canonical(state, start, end):
    """The bytes of [start, end) with each listed pointer replaced by its target (a version 2
    list is exact; a version 1 list was guessed and may include data)."""
    data = bytearray(state["segment"][start:end])
    marks = {}
    for offset, (target, delta) in state["pointers"].items():
        if start <= offset < end:
            data[offset - start:offset - start + 8] = bytes(8)
            marks[offset - start] = (target, delta)
    return bytes(data), marks


def check_pointers(a, b):
    if a["uuid"] != b["uuid"] or len(a["segment"]) != len(b["segment"]):
        sys.exit("--check-pointers: the states are from different builds")
    if a["base"] == b["base"]:
        sys.exit("--check-pointers: the states were saved at the same address")
    slide = b["base"] - a["base"]
    sa, sb = a["segment"], b["segment"]
    owner = sorted(a["symbols"])
    problems = 0

    def where(offset):
        best = None
        for o, n in owner:
            if o <= offset:
                best = (o, n)
        return f"{best[1]}+0x{offset - best[0]:x}" if best else f"0x{offset:x}"

    differing = {i for i in range(0, len(sa) - 7) if sa[i:i + 8] != sb[i:i + 8]}
    for offset in sorted(set(a["pointers"]) | set(b["pointers"])):
        pa, pb = a["pointers"].get(offset), b["pointers"].get(offset)
        if pa != pb:
            print(f"  {where(offset)}: listed as {pa} in A, {pb} in B")
            problems += 1
    moved = set()
    for i in range(0, len(sa) - 7):
        if sa[i:i + 8] == sb[i:i + 8]:
            continue
        (va,), (vb,) = struct.unpack_from("<Q", sa, i), struct.unpack_from("<Q", sb, i)
        if (vb - va) & 0xFFFFFFFFFFFFFFFF == slide & 0xFFFFFFFFFFFFFFFF:
            moved.add(i)
    for i in sorted(moved):
        if i not in a["pointers"]:
            print(f"  {where(i)}: moved with the program but is not listed (0x{struct.unpack_from('<Q', sa, i)[0]:x})")
            problems += 1
    for offset in sorted(a["pointers"]):
        if offset not in moved:
            print(f"  {where(offset)}: listed but did not move with the program")
            problems += 1
    other = sorted(i for i in differing if not any(j in moved for j in range(i - 7, i + 8)))
    if other:
        print(f"  {len(other)} other differing positions, first at {where(other[0])} (not pointers: nondeterminism?)")
        problems += 1
    print(f"{len(a['pointers'])} pointers listed, {len(moved)} moved with the program: "
          f"{'OK' if problems == 0 else f'{problems} problems'}")
    return problems == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a")
    parser.add_argument("b", nargs="?")
    parser.add_argument("--bytes", type=int, default=8, help="differing offsets to show per variable")
    parser.add_argument("--check-pointers", action="store_true", help="check the pointer lists (see above)")
    args = parser.parse_args()

    a = load(args.a)
    print(f"{args.a}: version {a['version']} frame {a['frame']} checksum {a['checksum']:016x} build {a['uuid']} "
          f"at 0x{a['base']:x}, segment {len(a['segment'])} bytes, {len(a['symbols'])} variables, "
          f"{len(a['pointers'])} pointers")
    if args.b is None:
        sizes = sorted(((e - o, n) for n, (o, e) in extents(a).items()), reverse=True)
        for size, n in sizes[:20]:
            print(f"  {size:8d}  {n}")
        return
    b = load(args.b)
    print(f"{args.b}: version {b['version']} frame {b['frame']} checksum {b['checksum']:016x} build {b['uuid']} "
          f"at 0x{b['base']:x}")
    if args.check_pointers:
        sys.exit(0 if check_pointers(a, b) else 1)
    ea, eb = extents(a), extents(b)
    differing = 0
    for n, (oa, enda) in sorted(ea.items(), key=lambda kv: kv[1][0]):
        if n not in eb:
            print(f"  {n}: only in A")
            continue
        ob, endb = eb[n]
        (xa, ma), (xb, mb) = canonical(a, oa, enda), canonical(b, ob, endb)
        offs = [i for i in range(min(len(xa), len(xb))) if xa[i] != xb[i]]
        offs += [i for i in sorted(ma.keys() | mb.keys()) if ma.get(i) != mb.get(i)]
        if not offs and len(xa) == len(xb):
            continue
        differing += 1
        offs = sorted(set(offs))

        def show(i):
            if i in ma or i in mb:
                return f"+0x{i:x}: {ma.get(i)}/{mb.get(i)}"
            return f"+0x{i:x}: {xa[i]:02x}/{xb[i]:02x}"

        shown = ", ".join(show(i) for i in offs[:args.bytes])
        more = f" (+{len(offs) - args.bytes} more)" if len(offs) > args.bytes else ""
        sizes = f" size {len(xa)}/{len(xb)}" if len(xa) != len(xb) else ""
        print(f"  {n}: {len(offs)} bytes differ{sizes}: {shown}{more}")
    for n in eb:
        if n not in ea:
            print(f"  {n}: only in B")
    print(f"{differing} variables differ")


if __name__ == "__main__":
    main()
