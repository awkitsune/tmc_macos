#!/usr/bin/env python3
"""Compare two save states (port/src/state.c format) variable by variable.

    port/scripts/state_diff.py A.tmcs B.tmcs [--bytes N]

Prints each variable whose bytes differ, with the first differing offsets.
With one file, prints the header and the largest variables instead.
--ignore-pointers skips 8-byte words that point to the same place in each
state's program (states of one build from runs with address randomization).
"""
import argparse
import struct
import sys
import zlib

HEADER = struct.Struct("<8sIII16sQQQII")


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, header_size, flags, uuid, base, frame, checksum, body_size, packed_size = HEADER.unpack_from(data)
    if magic != b"TMCSTATE":
        sys.exit(f"{path}: not a save state")
    body = zlib.decompress(data[header_size:header_size + packed_size])
    pos = 0

    def u32():
        nonlocal pos
        (v,) = struct.unpack_from("<I", body, pos)
        pos += 4
        return v

    def name():
        nonlocal pos
        (n,) = struct.unpack_from("<H", body, pos)
        s = body[pos + 2:pos + 2 + n].decode()
        pos += 2 + n
        return s

    size = u32()
    segment = body[pos:pos + size]
    pos += size
    symbols = []
    for _ in range(u32()):
        offset = u32()
        symbols.append((offset, name()))
    return {
        "version": version, "flags": flags, "uuid": uuid.hex(), "base": base, "frame": frame, "checksum": checksum,
        "segment": segment, "symbols": symbols,
    }


def extents(state):
    """{name: (start, end)}; a variable runs to the next address (aliases share it)."""
    syms = state["symbols"]
    starts = sorted({o for o, _ in syms}) + [len(state["segment"])]
    following = {starts[i]: starts[i + 1] for i in range(len(starts) - 1)}
    return {n: (o, following[o]) for o, n in syms}


IMAGE_SPAN = 0x10000000  # generous upper bound on the program's size


def pointer_words(state, offset, data):
    """{position: offset from the program's base} for the 8-byte (segment-aligned)
    words of data that point into the program."""
    base = state["base"]
    found = {}
    for i in range((-offset) % 8, len(data) - 7, 8):
        (v,) = struct.unpack_from("<Q", data, i)
        if base <= v < base + IMAGE_SPAN:
            found[i] = v - base
    return found


def mask_pointers(a, oa, xa, b, ob, xb):
    """Zero the words that point to the same place in the program in both states
    (pointers moved by a different load address)."""
    xa, xb = bytearray(xa), bytearray(xb)
    pa, pb = pointer_words(a, oa, xa), pointer_words(b, ob, xb)
    for i in pa.keys() & pb.keys():
        if pa[i] == pb[i]:
            xa[i:i + 8] = bytes(8)
            xb[i:i + 8] = bytes(8)
    return bytes(xa), bytes(xb)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a")
    parser.add_argument("b", nargs="?")
    parser.add_argument("--bytes", type=int, default=8, help="differing offsets to show per variable")
    parser.add_argument("--ignore-pointers", action="store_true", help="skip words that point into the program")
    args = parser.parse_args()

    a = load(args.a)
    print(f"{args.a}: frame {a['frame']} checksum {a['checksum']:016x} build {a['uuid']} "
          f"segment {len(a['segment'])} bytes, {len(a['symbols'])} variables")
    if args.b is None:
        sizes = sorted(((e - o, n) for n, (o, e) in extents(a).items()), reverse=True)
        for size, n in sizes[:20]:
            print(f"  {size:8d}  {n}")
        return
    b = load(args.b)
    print(f"{args.b}: frame {b['frame']} checksum {b['checksum']:016x} build {b['uuid']}")
    ea, eb = extents(a), extents(b)
    differing = 0
    for n, (oa, enda) in sorted(ea.items(), key=lambda kv: kv[1][0]):
        if n not in eb:
            print(f"  {n}: only in A")
            continue
        ob, endb = eb[n]
        xa, xb = a["segment"][oa:enda], b["segment"][ob:endb]
        if args.ignore_pointers:
            xa, xb = mask_pointers(a, oa, xa, b, ob, xb)
        if xa == xb:
            continue
        differing += 1
        offs = [i for i in range(min(len(xa), len(xb))) if xa[i] != xb[i]]
        shown = ", ".join(f"+0x{i:x}: {xa[i]:02x}/{xb[i]:02x}" for i in offs[:args.bytes])
        more = f" (+{len(offs) - args.bytes} more)" if len(offs) > args.bytes else ""
        sizes = f" size {len(xa)}/{len(xb)}" if len(xa) != len(xb) else ""
        print(f"  {n}: {len(offs)} bytes differ{sizes}: {shown}{more}")
    for n in eb:
        if n not in ea:
            print(f"  {n}: only in B")
    print(f"{differing} variables differ")


if __name__ == "__main__":
    main()
