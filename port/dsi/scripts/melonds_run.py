#!/usr/bin/env python3
"""Boot an .nds in melonDS (DSi mode) and read results back as text.

    python3 port/dsi/scripts/melonds_run.py ROM.nds ELF [--seconds N]
        [--symbol NAME] [--done-symbol NAME] [--sd-file PATH]

Runs melonDS with its GDB stub on, waits the given time, prints the ELF's
NUL-terminated string symbol and whether its done flag is set, quits melonDS normally (so it flushes the SD image) and, if asked,
prints a file from the SD image with mtools. melonDS.toml is backed up first
and restored afterwards; melonDS must not be running already.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

MELONDS = "/Applications/melonDS.app/Contents/MacOS/melonDS"
CONFIG = os.path.expanduser("~/Library/Preferences/melonDS/melonDS.toml")
GDB = "/opt/wonderful/toolchain/gcc-arm-none-eabi/bin/arm-none-eabi-gdb"
GDB_PORT = 3333


def set_key(text, section, key, value):
    match = re.search(r"(?ms)^\[%s\]\n(.*?)(?=^\[|\Z)" % re.escape(section), text)
    if match is None:
        return text.rstrip("\n") + "\n\n[%s]\n%s = %s\n" % (section, key, value)
    body = match.group(1)
    if re.search(r"(?m)^%s = " % re.escape(key), body):
        body = re.sub(r"(?m)^%s = .*$" % re.escape(key), "%s = %s" % (key, value), body)
    else:
        body = body.rstrip("\n") + "\n%s = %s\n\n" % (key, value)
    return text[: match.start(1)] + body + text[match.end(1) :]


def gdb(elf, commands):
    args = [GDB, "-q", "-batch", "-nx", elf, "-ex", "set pagination off",
            "-ex", "target remote localhost:%d" % GDB_PORT]
    for command in commands:
        args += ["-ex", command]
    args += ["-ex", "detach"]
    result = subprocess.run(args, capture_output=True, text=True, timeout=60)
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("rom")
    parser.add_argument("elf")
    parser.add_argument("--seconds", type=float, default=8)
    parser.add_argument("--symbol", default="gHelloOut")
    parser.add_argument("--done-symbol", default="gHelloDone")
    parser.add_argument("--sd-file")
    parser.add_argument("--gdb", action="append", default=[], help="extra GDB command; its output is printed")
    parser.add_argument("--sd-image", default=os.path.expanduser("~/dsi/support/dsisd.bin"),
                        help="'none' turns the DSi SD slot off")
    parser.add_argument("--dldi-image", help="also offer this FAT image as DLDI (fat:); melonDS 1.1 "
                        "hangs on DSi SD writes, DLDI writes work")
    args = parser.parse_args()

    if subprocess.run(["pgrep", "-x", "melonDS"], capture_output=True).returncode == 0:
        sys.exit("melonDS is running; quit it first (it rewrites melonDS.toml)")

    sd = args.sd_image != "none"
    image_mtime = os.path.getmtime(args.sd_image) if sd else None
    backup = CONFIG + ".melonds_run-backup"
    shutil.copyfile(CONFIG, backup)
    try:
        text = open(CONFIG).read()
        text = set_key(text, "Emu", "ConsoleType", "1")
        text = set_key(text, "Emu", "DirectBoot", "true")
        text = set_key(text, "DSi.SD", "Enable", "true" if sd else "false")
        if sd:
            text = set_key(text, "DSi.SD", "ImagePath", '"%s"' % args.sd_image)
        text = set_key(text, "DLDI", "Enable", "true" if args.dldi_image else "false")
        if args.dldi_image:
            text = set_key(text, "DLDI", "ImagePath", '"%s"' % os.path.abspath(args.dldi_image))
            text = set_key(text, "DLDI", "FolderSync", "false")
        text = set_key(text, "Instance0.Gdb", "Enabled", "true")
        text = set_key(text, "Instance0.Gdb.ARM9", "Port", str(GDB_PORT))
        text = set_key(text, "Instance0.Gdb.ARM9", "BreakOnStartup", "false")
        open(CONFIG, "w").write(text)

        log = open(os.path.splitext(args.rom)[0] + ".melonds.log", "w")
        emu = subprocess.Popen([MELONDS, os.path.abspath(args.rom)], stdout=log, stderr=subprocess.STDOUT)
        try:
            # The stub stalls on a second connection, so read once, after the wait.
            time.sleep(args.seconds)
            out = gdb(args.elf, ["set print elements 0",
                                 "x/wx &%s" % args.done_symbol,
                                 "x/s &%s" % args.symbol] + args.gdb)
            done = re.search(r"<%s>:\s+0x0*1\b" % re.escape(args.done_symbol), out) is not None
            text = re.search(r'<%s>:\s+"(.*)"' % re.escape(args.symbol), out, re.S)
            if text is None:
                print(out)
            else:
                print(text.group(1).encode().decode("unicode_escape"), end="")
            print("done flag: %s" % ("set" if done else "NOT set"))
            if args.gdb:
                print("--- gdb ---")
                print(out[out.index('"', out.index("<%s>" % args.symbol)):].split("\n", 1)[-1])
        finally:
            subprocess.run(["osascript", "-e", 'quit app "melonDS"'], capture_output=True)
            try:
                emu.wait(timeout=10)
            except subprocess.TimeoutExpired:
                emu.kill()
                print("melonDS did not quit; killed (SD image writes may be lost)")
    finally:
        shutil.copyfile(backup, CONFIG)
        os.remove(backup)

    if args.sd_file and args.dldi_image:
        result = subprocess.run(["mtype", "-i", args.dldi_image, "::/" + args.sd_file],
                                capture_output=True, text=True)
        print("--- fat:/%s (DLDI image) ---" % args.sd_file)
        print(result.stdout or result.stderr)
    if args.sd_file and sd:
        print("sd image %s" % ("modified" if os.path.getmtime(args.sd_image) != image_mtime else "NOT modified"))
        result = subprocess.run(["mtype", "-i", args.sd_image, "::/" + args.sd_file],
                                capture_output=True, text=True)
        print("--- sd:/%s ---" % args.sd_file)
        print(result.stdout or result.stderr)


if __name__ == "__main__":
    main()
