#!/usr/bin/env python3
"""The DSi menu icon from the app icon (port/src/macos/icon/icon_32.png).

    python3 -I port/dsi/tools/icon_bmp.py icon_32.png out.bmp
    python3 -I port/dsi/tools/icon_bmp.py --check icon_32.png tmc_dsi.nds

A DS icon is 32x32 with 16 colours, colour 0 transparent. The PNG is indexed
with 15 colours in use, all opaque; they become colours 1-15 of a 4-bit BMP for
ndstool, and colour 0 is a key colour no pixel uses. --check decodes the icon
ndstool put in the .nds banner and compares it with the PNG at the banner's
5 bits per channel. Plain Python (zlib), no imaging library.
"""

import argparse
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("%s: not a PNG" % path)
    chunks, i, idat = {}, 8, b""
    while i < len(data):
        length, kind = struct.unpack(">I4s", data[i : i + 8])
        body = data[i + 8 : i + 8 + length]
        if kind == b"IDAT":
            idat += body
        else:
            chunks[kind] = body
        i += 12 + length
    width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", chunks[b"IHDR"])
    if colour != 3 or depth != 8 or interlace:
        sys.exit("%s: expected an 8-bit indexed PNG, not interlaced" % path)
    palette = [tuple(chunks[b"PLTE"][3 * k : 3 * k + 3]) for k in range(len(chunks[b"PLTE"]) // 3)]
    alpha = list(chunks.get(b"tRNS", b""))
    raw = zlib.decompress(idat)
    rows, prev, p = [], bytearray(width), 0
    for _ in range(height):
        kind, line = raw[p], bytearray(raw[p + 1 : p + 1 + width])
        p += 1 + width
        for x in range(width):
            a = line[x - 1] if x else 0
            b = prev[x]
            c = prev[x - 1] if x else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return width, height, rows, palette, alpha


def icon_colours(path):
    width, height, rows, palette, alpha = read_png(path)
    if (width, height) != (32, 32):
        sys.exit("%s: the icon must be 32x32, not %dx%d" % (path, width, height))
    used = sorted(set(v for row in rows for v in row))
    if any(k < len(alpha) and alpha[k] != 255 for k in used):
        sys.exit("%s: transparent pixels are not handled" % path)
    if len(used) > 15:
        sys.exit("%s: %d colours; a DS icon has 15 and a transparent one" % (path, len(used)))
    index = {k: n + 1 for n, k in enumerate(used)}
    colours = [(255, 0, 255)] + [palette[k] for k in used]
    colours += [(0, 0, 0)] * (16 - len(colours))
    return [[index[v] for v in row] for row in rows], colours


def write_bmp(path, pixels, colours):
    rows = b""
    for row in reversed(pixels):  # bottom-up
        rows += bytes((row[x] << 4) | row[x + 1] for x in range(0, 32, 2))
    header_size = 14 + 40 + 16 * 4
    info = struct.pack("<IiiHHIIiiII", 40, 32, 32, 1, 4, 0, len(rows), 2835, 2835, 16, 16)
    table = b"".join(bytes((b, g, r, 0)) for r, g, b in colours)
    with open(path, "wb") as out:
        out.write(struct.pack("<2sIHHI", b"BM", header_size + len(rows), 0, 0, header_size))
        out.write(info + table + rows)


def check(png, nds):
    pixels, colours = icon_colours(png)
    rom = open(nds, "rb").read()
    banner = struct.unpack_from("<I", rom, 0x68)[0]
    bitmap = rom[banner + 0x20 : banner + 0x220]
    palette = struct.unpack_from("<16H", rom, banner + 0x220)
    wrong = 0
    for y in range(32):
        for x in range(32):
            tile = (y // 8) * 4 + x // 8
            byte = bitmap[tile * 32 + (y % 8) * 4 + (x % 8) // 2]
            k = (byte >> 4) if x & 1 else (byte & 15)
            bgr = palette[k] if k else None
            want = tuple(c >> 3 for c in colours[pixels[y][x]])
            got = (bgr & 31, (bgr >> 5) & 31, (bgr >> 10) & 31) if bgr is not None else None
            wrong += got != want
    print("icon in %s: %d of 1024 pixels differ from %s" % (nds, wrong, png))
    return wrong == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="compare the icon in an .nds with the PNG")
    parser.add_argument("png")
    parser.add_argument("out")
    args = parser.parse_args()
    if args.check:
        sys.exit(0 if check(args.png, args.out) else 1)
    pixels, colours = icon_colours(args.png)
    write_bmp(args.out, pixels, colours)


if __name__ == "__main__":
    main()
