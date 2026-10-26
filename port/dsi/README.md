# DSi port: toolchain and test setup

A native DSi-mode build of the decompiled game, run from TWiLight Menu++ or
Unlaunch, is planned in [PLAN.md](PLAN.md). This page covers the toolchain, the
melonDS setup and the test programs that exist so far. Nothing here is ROM-derived;
the player's own ROM is read from the SD card at run time.

Contents:

- `hello/`: a DSi-mode hello world. It reports free main RAM, lists the SD card
  root, writes and reads back `hello.txt`.
- `scripts/melonds_run.py`: boots an `.nds` in melonDS and reads its results back
  as text through melonDS's GDB stub.
- `tools/reloc_report.py`: counts the ROM-data pointers the port has to relocate
  (the figures in [PLAN.md](PLAN.md)).

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

## ROM-data relocation figures

```bash
cd build/USA && arm-none-eabi-ld -n -q -T linker.ld -o /tmp/tmc_relocs.elf -L ../../tools/agbcc/lib -lc && cd ../..
python3 -I port/dsi/tools/reloc_report.py /tmp/tmc_relocs.elf build/USA/tmc.map --raw
```

This relinks the GBA build with its relocations kept. The ROM section matches
`baserom.gba` except the 174 header bytes gbafix sets. The script needs
pyelftools, which the repository's `.venv` has. Results are in
[PLAN.md](PLAN.md#measurements).
