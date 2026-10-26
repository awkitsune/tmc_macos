# DSi port: toolchain and test setup

A native DSi-mode build of the decompiled game, run from TWiLight Menu++ or
Unlaunch, is planned in [PLAN.md](PLAN.md). This page covers the toolchain, the
melonDS setup and the test programs that exist so far. Nothing here is ROM-derived;
the player's own ROM is read from the SD card at run time.

Contents:

- `Makefile`, `src/`, `ld/`: the port itself (below, and
  [PLAN.md](PLAN.md#where-it-lives)).
- `tools/gen_link.py`: generates the port's ROM symbols, RAM layout and
  relocation list from the GBA build.
- `tools/rom_crc.py`: the relocated ROM copy's CRC32, computed on the host from
  your ROM and the built ELF, to compare with the boot log.
- `tools/icon_bmp.py`: the menu icon from the Mac app's icon.
- `scripts/compare_frames.py`: the game's per-frame records (`gDsiFrames`)
  against the Mac headless run's `TMC_HW_HASH`.
- `hello/`: a DSi-mode hello world. It reports free main RAM, lists the SD card
  root, writes and reads back `hello.txt`.
- `scripts/melonds_run.py`: boots an `.nds` in melonDS and reads its results back
  as text through melonDS's GDB stub.
- `tools/reloc_report.py`: counts the ROM-data pointers the port has to relocate
  (the figures in [PLAN.md](PLAN.md)).
- `tools/layout_diff.py`: compares struct and global layouts (DWARF) between the
  game built for the ARM9 with the GBA layout and as the DSi build. It
  checks that `PORT_LAYOUT_64` covers everything 64-bit
  ([PLAN.md](PLAN.md#defines)).

## Toolchain: BlocksDS on the Wonderful toolchain

BlocksDS (libnds-compatible, DSi-aware) installs through the Wonderful toolchain's
package manager. Both ship native Apple silicon (arm64) builds: no Rosetta and no
Docker. devkitPro is not needed.

The Wonderful installer only supports `/opt/wonderful` on macOS, and `/opt` belongs
to root, so the directory is created once with sudo and handed to your user:

```bash
sudo mkdir -p /opt/wonderful && sudo chown $USER /opt/wonderful
```

The rest needs no sudo. These are the steps of the official installer
(`https://wonderful.asie.pl/bootstrap/wf-installer.sh`, v20260619), done by hand so
that your shell profile is left alone:

```bash
curl -fLO https://wonderful.asie.pl/bootstrap/wf-bootstrap-darwin-aarch64.tar.gz
tar -xzf wf-bootstrap-darwin-aarch64.tar.gz -C /opt/wonderful/
codesign --remove-signature /opt/wonderful/bin/wf-pacman
codesign -s - /opt/wonderful/bin/wf-pacman
export PATH=/opt/wonderful/bin:$PATH
wf-pacman -Syu --noconfirm wf-tools
wf-config repo enable blocksds
wf-pacman -Syu
wf-pacman -S blocksds-toolchain blocksds-docs toolchain-gcc-arm-none-eabi-gdb
```

Results (2026-10-26):

| Component | Version | Location |
|---|---|---|
| GCC (arm-none-eabi), picolibc | 16.2.0, 1.8.13 | `/opt/wonderful/toolchain/gcc-arm-none-eabi/` |
| binutils, GDB | 2.47, 17.2 | same |
| BlocksDS (libnds, ndstool, mkfatimg, grit, ...) | 1.24.0 | `/opt/wonderful/thirdparty/blocksds/core/` |

`file /opt/wonderful/toolchain/gcc-arm-none-eabi/bin/arm-none-eabi-gcc` reports
`Mach-O 64-bit executable arm64`, and so does `cc1`.

BlocksDS's makefiles look for `/opt/blocksds/core` by default. The makefiles here
pass `BLOCKSDS=/opt/wonderful/thirdparty/blocksds/core` themselves, so no symlink
or environment variable is needed. Updates: `wf-pacman -Syu`.

Homebrew's `arm-none-eabi-gcc`, used by the GBA build, has no C library and cannot
build DS programs. The two toolchains don't interfere.

## melonDS

melonDS 1.1 (`/Applications/melonDS.app`, a universal binary with a native arm64
slice) comes from <https://github.com/melonDS-emu/melonDS/releases> as
`melonDS-1.1-macOS-universal.zip`. The Homebrew cask has been disabled since
2026-09-01 because it fails the Gatekeeper check.

**DSi mode needs your own dumps.** melonDS has a built-in replacement BIOS for DS
mode only, so DSi mode needs all of these from your own console. They are not
downloadable and not part of this repository. Here they live in `~/dsi/support/`:

| File | Size | melonDS setting |
|---|---|---|
| `biosnds9.bin`, `biosnds7.bin` | 4 KB, 16 KB | DS mode BIOS (used by DSi mode too) |
| `dsfirmware.bin` | 256 KB | DS mode firmware |
| `biosdsi9.bin`, `biosdsi7.bin` | 64 KB each | DSi mode ARM9/ARM7 BIOS |
| `dsifirmware.bin` | 128 KB | DSi mode firmware |
| `dsinand.bin` | 240 MB (with nocash footer) | DSi mode NAND |
| `dsisd.bin` | 1 GB | DSi mode SD card image (below) |

The settings are under Config > Emu settings, in the DS-mode and DSi-mode tabs:
console type DSi, and "Boot game directly" on.

Without the DSi dumps, homebrew still runs in DS mode, but with 4 MB of RAM and
SD only through DLDI. That is too small for the port, but enough for toolchain
checks.

### SD card image

`~/dsi/support/dsisd.bin` is a 1 GiB FAT32 image without a partition table, made
with BlocksDS's `mkfatimg` from a folder (sizes are in KiB, cluster size in bytes):

```bash
mkdir -p sdroot && cp port/dsi/hello/dsihello.nds sdroot/
/opt/wonderful/thirdparty/blocksds/core/tools/mkfatimg/mkfatimg sdroot ~/dsi/support/dsisd.bin 1048576 4096
```

In melonDS, set DSi mode > SD card > image to the **absolute** path
`/Users/<you>/dsi/support/dsisd.bin` and tick "Enable". A relative name such as
`dsisd.bin` resolves against `~/Library/Preferences/melonDS/`, where melonDS
quietly creates a separate small image.

mtools (`brew install mtools`) reads and writes the image while melonDS is closed:

```bash
mdir -i ~/dsi/support/dsisd.bin ::/
mcopy -o -i ~/dsi/support/dsisd.bin port/dsi/hello/dsihello.nds ::/
mtype -i ~/dsi/support/dsisd.bin ::/hello.txt
```

melonDS writes to the image only while it is running and flushes when it quits
normally. A force-quit (`kill -9`) loses the writes.

## The hello world

```bash
make -C port/dsi/hello -j4
```

This builds `port/dsi/hello/dsihello.nds` (150 KB). Its header has unit code 0x02
(NDS + DSi enhanced), with the DSi extended header, so loaders start it in DSi mode
with 16 MB.

`make -C port/dsi/hello GBA_EWRAM=1` builds `dsihello_gbaewram.nds`, linked at
0x02040000 through `port/dsi/ld/` (BlocksDS's specs with a shifted memory map). It
also checks that 0x02000000-0x0203FFFF, where the game's EWRAM will sit at its GBA
addresses, is RAM nobody else uses. In melonDS DSi mode: `main` at 0x02045789,
0 of 65,536 words wrong through the cache and through the uncached mirror, heap
15,980 KB. Both `.nds` files are also on `~/dsi/support/dsisd.bin`.

The source is `port/dsi/hello/source/main.c`. It prints the report on the top
screen and keeps it in `gHelloOut`, setting `gHelloDone` to 1 at the end.

Measured in melonDS 1.1, DSi mode, with your dumps (DS mode for comparison):

| | DSi mode | DS mode |
|---|---|---|
| heap start / limit | 0x02018DA0 / 0x02FF4000 | 0x020182D4 / 0x023F4000 |
| heap span | 16,236 KB | 3,951 KB |
| malloc total (64 KB blocks) | 16,128 KB | 3,904 KB |
| `fatInitDefault` | ok | ok |
| `fat:/` (DLDI) list, write, `fclose`, `stat`, read back | all ok | all ok |
| `sd:/` (DSi SD slot) list | ok | n/a |
| `sd:/` write | **hangs** (see below) | n/a |

The last 48 KB of main RAM (from 0x02FF4000) are reserved for libnds, the loader and
DTCM, so about 16,330 KB of the 16 MB is usable. The hello binary takes ~99 KB.

**melonDS 1.1 does not finish writes to the DSi SD slot** with BlocksDS 1.24's
driver. It hangs both with this image and with an image melonDS makes itself: the
ARM9 waits on the IPC FIFO, and the ARM7 sits in the BIOS waiting for an interrupt
that never comes. Reads work. Writes through DLDI work in both modes, so automated
tests that need to write use DLDI (`fat:/`). On the real DSi, `sd:/` writes are
expected to work. Check them there with the same program: if they do, its `sd:/`
section ends with the write, `stat` and "read back: ... match" lines, as the
`fat:/` section does.

## Automated runs

```bash
python3 -I port/dsi/scripts/melonds_run.py port/dsi/hello/dsihello.nds port/dsi/hello/build/dsihello.elf \
    --seconds 20 --dldi-image /path/to/scratch_copy_of_dsisd.bin --sd-file hello.txt
```

The script:

1. backs up `~/Library/Preferences/melonDS/melonDS.toml` and switches on DSi mode,
   direct boot, the SD image and the GDB stub (port 3333). With `--dldi-image` it
   also turns on DLDI.
2. starts melonDS on the ROM and waits.
3. reads `gHelloDone` and `gHelloOut` from the ELF's symbols with
   `arm-none-eabi-gdb`.
4. quits melonDS normally and restores the config.
5. prints the given file from the image(s) with `mtype`.

`--gdb CMD` runs more GDB commands (for example `--gdb bt`). melonDS must not be
running beforehand, because it rewrites its config when it quits.

What can be automated:

- Booting, waiting a fixed time, reading any RAM by symbol or address (counters,
  hashes, logs, VRAM and OAM), and backtraces of both CPUs (ARM7 on port 3334).
- Files on a DLDI image, read with mtools after a normal quit.

What can't:

- melonDS has no headless mode or frame dump on the command line, and its window
  opens on screen.
- The melonDS GDB stub stalls when GDB connects a second time in the same run, so
  the script reads once, after the wait.
- melonDS 1.1 doesn't print no$gba-style debug messages to stdout.
- DSi SD writes hang (above).
- Visual checks are done on the real DSi.

## The port

```bash
PATH=$PWD/.venv/bin:$PATH make -j4
make -C port/dsi -j4
```

The first line is the GBA build, which the port needs (as the Mac build does);
`tmc.gba: OK` means it matches. The second builds `build/dsi/tmc_dsi.nds`, with
`tmc_dsi.elf`, the link map `tmc_dsi.map` and the generated files in
`build/dsi/gen/` (figures in `gen_report.txt`). `make -C port/dsi figures`
prints the section sizes. The makefile uses the repository's `.venv` Python
(pyelftools) and Homebrew's `arm-none-eabi-ld` for the GBA relink.

Nothing it writes holds ROM bytes: the generated files are offsets and symbol
names from the GBA link. The player's ROM is read at boot from
`sd:/tmc/baserom.gba` (the DSi SD slot) or `fat:/tmc/baserom.gba` (DLDI).

Run it in melonDS and read the boot log. melonDS reads the DSi SD slot but
does not finish writes to it (above), so a scratch FAT image with your ROM at
`tmc/baserom.gba` serves as the SD card (`--sd-image`) or as DLDI
(`--dldi-image`). Keep the image out of the repository:

```bash
mkdir -p /tmp/tmcsd/tmc && cp baserom.gba /tmp/tmcsd/tmc/
/opt/wonderful/thirdparty/blocksds/core/tools/mkfatimg/mkfatimg /tmp/tmcsd /tmp/tmcsd.img 65536 4096
python3 -I port/dsi/scripts/melonds_run.py build/dsi/tmc_dsi.nds build/dsi/tmc_dsi.elf \
    --seconds 30 --symbol gDsiLog --done-symbol gDsiLogDone --sd-image /tmp/tmcsd.img
.venv/bin/python -I port/dsi/tools/rom_crc.py build/dsi/tmc_dsi.elf baserom.gba
```

At milestone 5 it loads, checks and relocates the ROM, then runs the game
on the top screen, centred with a black border. The DS's A, B, Select,
Start, D-pad, R and L are the GBA's; there is no sound yet. It logs the
frame times after 1,024 frames.

The comparison with the Mac (below) needs a build with the checks,
`make -C port/dsi -j4 DSI_CHECKS=1`. Its first 1,024 frames run at about
half speed, with all keys up: each one records the shadows' CRC and every
even one captures the screen. It logs the frame times after 1,024 frames and
again, without the checks, after 2,048 (the copy's CRC depends on the build;
`rom_crc.py` must print the same one):

```
TMC DSi port
DSi mode: yes
ARM9 at 133 MHz (was 133)
binary end 0x2173e34, ROM copy 0x220c000-0x2ff3da4
heap 0x2174850-0x220c000
read 16777216 bytes from sd:/tmc/baserom.gba in 5610 ms (read 931, SHA-1 4678)
SHA-1 b4bd50e4131b027c334547b4524e2dbbd4227130
relocated: 11547 add-base, 3117 symbol; 1425 EWRAM kept; 0 unexpected
copy CRC32 96314ed8 (1307 ms)
GBA slot unmapped (MPU)
running the game
frames 0-1023 (with the checks), game time per frame (us): min 15 avg 217 max 12973
frame work (lines, DMA, IRQs, prepare) (us): min 5535 avg 5847 max 6326
present (us): min 1592 avg 1597 max 1606, ended by line 217, 0 after VBlank
shadow CRC (us): avg 9046; picture check avg 12543 per 2 frames
DS VBlanks 1759 (716 game frames over one)
presenter so far: two-window lines 0, wrap+OBJ-window lines 0, wrapped sprites 0, bitmap lines 0
presenter so far: per-line DISPCNT in 1024 frames, other registers in 774, up to 2 words
frames 1024-2047 (no checks, keys), game time per frame (us): min 208 avg 211 max 219
frame work (lines, DMA, IRQs, prepare) (us): min 5674 avg 5675 max 5699
present (us): min 1596 avg 1596 max 1605, ended by line 217, 0 after VBlank
DS VBlanks 1025 (0 game frames over one)
...
```

Compare the game's first 1,024 frames with the Mac headless build (a
`DSI_CHECKS=1` build): the
shadows' CRC and the game task on every frame, and the DS's display capture
of every even frame with the Mac's picture. The DS rounds blends and fades
on 6-bit channels, so the Mac draws with the same rounding
(`TMC_PPU_DS_EFFECTS=1`; with the GBA's, pictures differ by at most one step
in one channel):

```bash
(cd build/native && TMC_FRAMES=1024 TMC_HW_HASH=/tmp/mac_hw.txt TMC_FRAME_HASH=/tmp/mac_pic.txt TMC_PPU_DS_EFFECTS=1 ./tmc)
python3 -I port/dsi/scripts/melonds_run.py build/dsi/tmc_dsi.nds build/dsi/tmc_dsi.elf \
    --seconds 120 --symbol gDsiLog --done-symbol gDsiLogDone --sd-image /tmp/tmcsd.img \
    --gdb "dump binary value /tmp/dsi_frames.bin gDsiFrames" \
    --gdb "dump binary value /tmp/dsi_pics.bin gDsiPictures"
python3 -I port/dsi/scripts/compare_frames.py /tmp/mac_hw.txt /tmp/dsi_frames.bin \
    --pictures /tmp/mac_pic.txt /tmp/dsi_pics.bin
```

which prints `frames 0-1023 match: CRC and task on every frame (233 distinct
CRCs)` and `pictures of the even frames 0-1022 match (512), border clean on
every one`. Every 64th frame is shown for three DS frames before its capture,
which checks the picture stays right when the game is slower than the screen.

To find where one picture differs, dump that frame's whole capture and
compare it with the Mac's screenshot:

```bash
(cd build/native && TMC_FRAMES=251 TMC_SCREENSHOT=250 TMC_SCREENSHOT_DIR=/tmp/shots TMC_PPU_DS_EFFECTS=1 ./tmc)
touch port/dsi/src/present.c && make -C port/dsi -j4 DSI_CHECKS=1 DSI_DEFINES=-DDSI_CAPTURE_DUMP=250
python3 -I port/dsi/scripts/melonds_run.py build/dsi/tmc_dsi.nds build/dsi/tmc_dsi.elf \
    --seconds 120 --symbol gDsiLog --done-symbol gDsiLogDone --sd-image /tmp/tmcsd.img \
    --gdb "dump binary memory /tmp/cap.bin (char*)gDsiCaptureDump (char*)gDsiCaptureDump+98304"
python3 -I port/dsi/scripts/compare_picture.py /tmp/shots/frame_000250.png /tmp/cap.bin
```

The menu icon is the Mac app's (`port/src/macos/icon/icon_32.png`), converted
by `tools/icon_bmp.py`; `icon_bmp.py --check` compares the icon in a built
`.nds` with the PNG.

Measured in melonDS 1.1 (its timings are not the DSi's; milestone 6 measures
those):

| Case | Result |
|---|---|
| ROM on the SD slot | read 931 ms, SHA-1 4,678 ms, CRC32 1,743 ms; CRC equals `rom_crc.py` |
| ROM over DLDI (`fat:/`) | read 3,758 ms; same SHA-1, counts and CRC |
| one byte changed | `SHA-1 0b50d0ab…`, "not the USA ROM: expected b4bd50e4…", stops |
| no ROM | "no ROM: put your USA ROM at sd:/tmc/baserom.gba", stops |
| test build `make -C port/dsi DSI_DEFINES=-DDSI_TEST_STRAY_READ` (touch `src/main.c` first) | reading 0x08000000 aborts; the log shows PC in `main` and r3 = 08000000 |
| game, frames 0-1023 | equal to the Mac's (`compare_frames.py`) |
| pictures, even frames 0-1022 | equal to the Mac's with `TMC_PPU_DS_EFFECTS=1`, border clean (`compare_frames.py --pictures`) |

## ROM-data relocation figures

```bash
cd build/USA && arm-none-eabi-ld -n -q -T linker.ld -o /tmp/tmc_relocs.elf -L ../../tools/agbcc/lib -lc && cd ../..
python3 -I port/dsi/tools/reloc_report.py /tmp/tmc_relocs.elf build/USA/tmc.map --raw
```

This relinks the GBA build with its relocations kept. The ROM section matches
`baserom.gba` except the 174 header bytes gbafix sets. The script needs
pyelftools, which the repository's `.venv` has. Results are in
[PLAN.md](PLAN.md#measurements).
