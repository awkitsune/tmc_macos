#!/usr/bin/env python3
"""Tile headless screenshots into one PNG for quick review (stdlib only).

    contact_sheet.py OUT.png [--cols N] [--scale S] frame_*.png ...

Reads the 8-bit RGB, unfiltered PNGs that the headless runner writes and labels
each tile with the frame number from its file name.
"""

import re
import struct
import sys
import zlib

# 3x5 digits for the frame labels.
DIGITS = {
    "0": "111101101101111", "1": "010110010010111", "2": "111001111100111",
    "3": "111001111001111", "4": "101101111001001", "5": "111100111001111",
    "6": "111100111101111", "7": "111001001001001", "8": "111101111101111",
    "9": "111101111001111",
}


def read_png(path):
    data = open(path, "rb").read()
    pos, idat, width, height = 8, b"", 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 3 + 1
    rows = []
    for y in range(height):
        row = raw[y * stride:(y + 1) * stride]
        if row[0] != 0:
            raise ValueError(f"{path}: filtered PNG rows are not supported")
        rows.append(row[1:])
    return width, height, rows


def write_png(path, width, height, pixels):
    raw = b"".join(b"\0" + bytes(pixels[y * width * 3:(y + 1) * width * 3]) for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main(argv):
    out, cols, scale, files = None, 6, 1, []
    i = 0
    while i < len(argv):
        if argv[i] == "--cols":
            cols = int(argv[i + 1]); i += 2
        elif argv[i] == "--scale":
            scale = int(argv[i + 1]); i += 2
        elif out is None:
            out = argv[i]; i += 1
        else:
            files.append(argv[i]); i += 1
    if not files:
        print(__doc__)
        return 1
    images = [(f, read_png(f)) for f in files]
    tw, th = images[0][1][0] * scale, images[0][1][1] * scale
    gap = 2
    rows = (len(images) + cols - 1) // cols
    width, height = cols * (tw + gap), rows * (th + gap)
    pixels = bytearray(width * height * 3)
    for n, (name, (w, h, src)) in enumerate(images):
        ox, oy = (n % cols) * (tw + gap), (n // cols) * (th + gap)
        for y in range(th):
            srow = src[y // scale]
            base = ((oy + y) * width + ox) * 3
            if scale == 1:
                pixels[base:base + w * 3] = srow
            else:
                for x in range(tw):
                    sx = (x // scale) * 3
                    pixels[base + x * 3:base + x * 3 + 3] = srow[sx:sx + 3]
        label = re.findall(r"\d+", name.rsplit("/", 1)[-1])
        label = str(int(label[-1])) if label else ""
        # Black box with white digits in the top-left corner of the tile.
        for y in range(7):
            for x in range(len(label) * 4 + 1):
                base = ((oy + y) * width + ox + x) * 3
                pixels[base:base + 3] = b"\0\0\0"
        for d, ch in enumerate(label):
            for k, bit in enumerate(DIGITS[ch]):
                if bit == "1":
                    x, y = ox + 1 + d * 4 + k % 3, oy + 1 + k // 3
                    base = (y * width + x) * 3
                    pixels[base:base + 3] = b"\xff\xff\xff"
    write_png(out, width, height, pixels)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
