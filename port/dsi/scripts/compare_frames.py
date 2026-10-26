#!/usr/bin/env python3
"""Compare the DSi port's per-frame records with the Mac headless run (milestones 4, 5).

    TMC_FRAMES=1024 TMC_HW_HASH=mac.txt TMC_FRAME_HASH=mac_pic.txt TMC_PPU_DS_EFFECTS=1 build/native/tmc
    make -C port/dsi -j4 DSI_CHECKS=1
    python3 -I port/dsi/scripts/melonds_run.py build/dsi/tmc_dsi.nds build/dsi/tmc_dsi.elf ... \\
        --gdb "dump binary value dsi.bin gDsiFrames" --gdb "dump binary value dsi_pic.bin gDsiPictures"
    python3 -I port/dsi/scripts/compare_frames.py mac.txt dsi.bin [--frames N] \\
        [--pictures mac_pic.txt dsi_pic.bin]

mac.txt has "frame crc32 task state substate fade" per line (TMC_HW_HASH);
dsi.bin is gDsiFrames, 8 bytes per frame (u32 CRC, then task, state, substate,
fade). Prints the first frame that differs, or that all compared frames match.

With --pictures, mac_pic.txt has "frame crc32" of the Mac's picture per line
(TMC_FRAME_HASH, with TMC_PPU_DS_EFFECTS=1 for the DS's blend rounding);
dsi_pic.bin is gDsiPictures, 8 bytes per even frame (u32 CRC of the captured
240x160 picture, u32 border pixels that differ from the border colour).
"""

import argparse
import struct
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mac")
    parser.add_argument("dsi")
    parser.add_argument("--frames", type=int, help="compare only the first N frames")
    parser.add_argument("--pictures", nargs=2, metavar=("MAC_PIC", "DSI_PIC"),
                        help="also compare the captured pictures of the even frames")
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
    if args.pictures is not None:
        compare_pictures(args.pictures[0], args.pictures[1], count)


def compare_pictures(mac_path, dsi_path, count):
    mac = {}
    for line in open(mac_path):
        frame, crc = line.split()
        mac[int(frame)] = int(crc, 16)
    data = open(dsi_path, "rb").read()
    pictures = [struct.unpack_from("<II", data, i) for i in range(0, len(data) - 7, 8)]
    frames = [2 * k for k in range(len(pictures)) if 2 * k < count and 2 * k in mac]
    differ = [f for f in frames if pictures[f // 2][0] != mac[f]]
    border = [f for f in frames if pictures[f // 2][1] != 0]
    if differ or border:
        print("pictures: %d of %d differ (first %s), %d with border pixels off (first %s)"
              % (len(differ), len(frames), differ[:8], len(border), border[:8]))
        sys.exit(1)
    print("pictures of the even frames 0-%d match (%d), border clean on every one" % (frames[-1], len(frames)))


if __name__ == "__main__":
    main()
