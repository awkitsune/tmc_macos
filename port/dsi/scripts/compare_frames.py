#!/usr/bin/env python3
"""Compare the DSi port's per-frame records with the Mac headless run (milestone 4).

    TMC_FRAMES=1024 TMC_HW_HASH=mac.txt build/native/tmc
    python3 -I port/dsi/scripts/melonds_run.py build/dsi/tmc_dsi.nds build/dsi/tmc_dsi.elf ... \\
        --gdb "dump binary value dsi.bin gDsiFrames"
    python3 -I port/dsi/scripts/compare_frames.py mac.txt dsi.bin [--frames N]

mac.txt has "frame crc32 task state substate fade" per line (TMC_HW_HASH);
dsi.bin is gDsiFrames, 8 bytes per frame (u32 CRC, then task, state, substate,
fade). Prints the first frame that differs, or that all compared frames match.
"""

import argparse
import struct
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mac")
    parser.add_argument("dsi")
    parser.add_argument("--frames", type=int, help="compare only the first N frames")
    args = parser.parse_args()

    mac = []
    for line in open(args.mac):
        frame, crc, task, state, substate, fade = line.split()
        if int(frame) != len(mac):
            sys.exit("%s: frame %s out of order" % (args.mac, frame))
        mac.append((int(crc, 16), int(task), int(state), int(substate), int(fade)))
    data = open(args.dsi, "rb").read()
    dsi = [struct.unpack_from("<IBBBB", data, i) for i in range(0, len(data) - 7, 8)]

    count = min(len(mac), len(dsi))
    if args.frames is not None:
        count = min(count, args.frames)
    for frame in range(count):
        if mac[frame] != dsi[frame]:
            print("frame %d differs: Mac crc %08x task %d/%d/%d fade %d, DSi crc %08x task %d/%d/%d fade %d"
                  % ((frame,) + mac[frame] + dsi[frame]))
            same = sum(1 for f in range(frame, count) if mac[f][0] == dsi[f][0])
            print("frames %d-%d: %d of %d CRCs equal after that" % (frame, count - 1, same, count - frame))
            sys.exit(1)
    distinct = len(set(m[0] for m in mac[:count]))
    print("frames 0-%d match: CRC and task on every frame (%d distinct CRCs)" % (count - 1, distinct))
    last = mac[count - 1]
    print("frame %d: crc %08x task %d state %d substate %d fade %d" % ((count - 1,) + last))


if __name__ == "__main__":
    main()
