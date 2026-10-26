#!/usr/bin/env python3
"""Compare a DSi display capture with the Mac's picture of the same frame (milestone 5).

    TMC_FRAMES=N+1 TMC_SCREENSHOT=N TMC_SCREENSHOT_DIR=shots build/native/tmc
    make -C port/dsi -j4 DSI_DEFINES=-DDSI_CAPTURE_DUMP=N   (N even, under 1024)
    python3 -I port/dsi/scripts/melonds_run.py ... --gdb \\
        "dump binary memory cap.bin (char*)gDsiCaptureDump (char*)gDsiCaptureDump+98304"
    python3 -I port/dsi/scripts/compare_picture.py shots/frame_00000N.png cap.bin

The PNG is the Mac's software PPU (port/src/ppu.c, 8 bits per channel from 5);
cap.bin is the DS screen, 256x192 15-bit colours with the picture at (8, 16).
Prints how many pixels differ, where, and the first few with both colours.
Plain Python (zlib).
"""

import argparse
import struct
import sys
import zlib

WIDTH, HEIGHT, LEFT, TOP = 240, 160, 8, 16


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("%s: not a PNG" % path)
    i, idat, header = 8, b"", None
    while i < len(data):
        length, kind = struct.unpack(">I4s", data[i : i + 8])
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", data[i + 8 : i + 21])
        elif kind == b"IDAT":
            idat += data[i + 8 : i + 8 + length]
        i += 12 + length
    width, height, depth, colour = header[:4]
    if (width, height, depth, colour) != (WIDTH, HEIGHT, 8, 2):
        sys.exit("%s: expected a 240x160 RGB PNG from the Mac port" % path)
    raw, stride, rows, prev = zlib.decompress(idat), 1 + width * 3, [], bytearray(width * 3)
    for y in range(height):
        kind, line = raw[y * stride], bytearray(raw[y * stride + 1 : (y + 1) * stride])
        for x in range(len(line)):
            a = line[x - 3] if x >= 3 else 0
            b, c = prev[x], prev[x - 3] if x >= 3 else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([(line[3 * x] >> 3) | (line[3 * x + 1] >> 3) << 5 | (line[3 * x + 2] >> 3) << 10
                     for x in range(width)])
        prev = line
    return rows


def rgb(c):
    return "%2d,%2d,%2d" % (c & 31, (c >> 5) & 31, c >> 10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("png")
    parser.add_argument("capture")
    parser.add_argument("--show", type=int, default=12, help="differing pixels to list")
    args = parser.parse_args()

    mac = read_png(args.png)
    data = open(args.capture, "rb").read()
    ds = struct.unpack("<%dH" % (256 * 192), data[: 256 * 192 * 2])
    diffs = [(x, y, mac[y][x], ds[(y + TOP) * 256 + x + LEFT] & 0x7FFF)
             for y in range(HEIGHT) for x in range(WIDTH)
             if mac[y][x] != ds[(y + TOP) * 256 + x + LEFT] & 0x7FFF]
    if not diffs:
        print("picture matches: 0 of %d pixels differ" % (WIDTH * HEIGHT))
        return
    xs, ys = [d[0] for d in diffs], [d[1] for d in diffs]
    print("%d of %d pixels differ, x %d-%d, y %d-%d, on %d lines"
          % (len(diffs), WIDTH * HEIGHT, min(xs), max(xs), min(ys), max(ys), len(set(ys))))
    for x, y, m, d in diffs[: args.show]:
        print("  (%3d,%3d) Mac %s  DSi %s" % (x, y, rgb(m), rgb(d)))
    sys.exit(1)


if __name__ == "__main__":
    main()
