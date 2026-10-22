# Native macOS port

Native arm64 macOS build of the decompiled game code: a Metal app with keyboard
and gamepad input, plus a headless runner for automated testing. The GBA build in the repository root stays
byte-matching: every port change is either under `port/` or behind
`#ifdef PLATFORM_NATIVE`.

## Setup

One-time machine setup. The native build reads the GBA build's data objects, so
both toolchains are needed.

1. **Tools:** Xcode Command Line Tools, CMake ≥ 3.24 and Python 3.

   ```bash
   brew install cmake python3
   ```

2. **ROM:** place an original USA ROM (SHA1
   `b4bd50e4131b027c334547b4524e2dbbd4227130`) at `baserom.gba` in the
   repository root. A symlink works and `*.gba` is gitignored:

   ```bash
   ln -s ~/tmc_usa.gba baserom.gba
   ```

3. **Host tools and assets:** see [Building](#building) below.

4. **GBA toolchain:** `arm-none-eabi-gcc` and agbcc.
   agbcc is cloned next to this repository and installs into `tools/agbcc`; its
   install also leaves untracked `tools/include/` and `tools/lib/`, which are not
   committed.

   ```bash
   brew install arm-none-eabi-gcc arm-none-eabi-binutils
   ```

   ```bash
   git clone https://github.com/pret/agbcc ../agbcc
   ```

   ```bash
   (cd ../agbcc && sh build.sh && sh install.sh ../tmc_macos)
   ```

   Replace `../tmc_macos` with the path to this checkout if it lives elsewhere.

5. **Python packages:** the GBA `make` needs `pycparser`, and the native data
   converter needs `pyelftools` and `libclang`. Homebrew's Python refuses global
   installs, so use the repository's `.venv` (gitignored):

   ```bash
   python3 -m venv .venv && .venv/bin/pip install -r port/requirements.txt
   ```

## Building

Build the host tools and extract assets (CMake 4 needs the policy override for a
bundled dependency; recent clang needs the warning relaxed):

```bash
CMAKE_POLICY_VERSION_MINIMUM=3.5 cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=tools -DCMAKE_CXX_FLAGS=-Wno-error=deprecated-literal-operator -S tools -B tools/cmake-build
```

```bash
cmake --build tools/cmake-build -j --target install
```

Build the GBA ROM. This also extracts the assets, and its objects and `tmc.elf`
are the input to the native data converter. A matching build ends with
`tmc.gba: OK`:

```bash
PATH="$PWD/.venv/bin:$PATH" make -j$(sysctl -n hw.ncpu)
```

Configure and build the native game. The first configure converts the game data
(about a minute); `cmake --build build/native --target datagen` reruns it after
data or hint changes:

```bash
cmake -S port -B build/native
```

```bash
cmake --build build/native -j
```

This builds three programs from the same game code:

- `build/native/TMC.app`, the macOS app (see [Playing](#playing)).
- `build/native/tmc`, the headless runner for automated runs (see below).
- `build/native/tmc-songs`, the song exporter (see [Exporting the music](#exporting-the-music)).

Build just one with `--target tmc_app`, `--target tmc` or `--target tmc_songs`. If the link ever breaks, `cmake --build build/native --target
link-report` groups the unresolved symbols by where the GBA build defines them.

### Development and distributable builds

The build above is a development build: the game's assets are built into the
programs, so they run as they are, but they must not be shared.
`-DTMC_BAKE_DATA=OFF` makes a distributable build without them; each program
extracts them from the player's own ROM instead (see
[First launch](#first-launch)). Use a separate build folder:

```bash
cmake -S port -B build/native-rom -DTMC_BAKE_DATA=OFF
```

```bash
cmake --build build/native-rom -j
```

What is left out is decided by where each byte comes from, not by whether it
equals the ROM:

- **Assets** are what asset extraction (`tools/asset_processor`, `assets.json`)
  copies out of `baserom.gba` into `build/USA/assets`, and the data sources
  `.incbin`: graphics, tilesets, palettes, maps, entity and room data blobs,
  sound samples and songs, text. They come from the player's ROM: 13.1 of the
  13.8 MB of game data.
- **Everything written in the repository** stays compiled in: the C code and
  its tables, data spelled out in `data/*.s` (`.byte`/`.4byte` lists and
  macros, about 670 KB), and the pointer tables datagen lays out. All of it is
  public in the decomp.

To find the assets' bytes, datagen assembles every data object twice more:
once with each extracted asset file replaced by a file of the same size full
of `0x55`, once full of `0xaa`. The bytes that differ are asset bytes
(`port/tools/datagen/provenance.py`). Those become zeros plus a run each
(native address, ROM offset, length), about 1,000 runs; every run is checked
against `tmc.gba`. Building still needs the GBA build and its extracted
assets. The programs are about 15 MB: the zeros take file space, because
nearly every data section also holds source-written bytes. They compress to
almost nothing.

To check that a program carries no asset content:

```bash
python3 port/scripts/asset_overlap.py build/native-rom/TMC.app/Contents/MacOS/TMC build/USA/assets
```

It lists every stretch of 64 bytes or more that the program shares with an
extracted asset file, with the file and the program's symbol, and exits with 1
if there is one. A development build shares 12.7 MB. A distributable build
shares 5 stretches, 710 bytes. These are source-written tables whose bytes
also occur inside asset dumps: entity lists from `data/map/entity_headers.s`
that the ROM holds again inside a blob, one equal to a USA-demo-only asset, and
a C table in `src/manager/diggingCaveEntranceManager.c`.

In a development build with `TMC_ROM=path` set, `tmc` checks every run of the
manifest against that ROM before it starts (`built-in data: all 46923 manifest
runs ... match the ROM`).

### Playing

```bash
open build/native/TMC.app
```

The app runs the game at the GBA's 59.73 Hz and shows it scaled by a whole
number (or, with integer scaling off, as large as the 3:2 shape allows).
Sound comes from the game's own m4a engine (see [Sound](#sound)). The Sound
menu has Mute (⌥⌘M) and High-Quality Audio, which mixes at 48 kHz in floating
point instead of the GBA's 15.7 kHz 8-bit mix; both are remembered.

| Keyboard | GBA | Gamepad |
|---|---|---|
| Arrow keys | D-pad | D-pad or left stick |
| X | A | Button labelled A (Xbox A, PlayStation ✕, Nintendo A), or X |
| Z | B | Button labelled B (Xbox B, PlayStation ○, Nintendo B), or Y |
| A / S | L / R | Shoulders or triggers |
| Return | Start | Menu |
| Backspace or `\` | Select | Options |
| Tab (hold) | Fast-forward (4×) | Right stick click |

Keys are matched by position, so other keyboard layouts use the same physical
keys. Any extended gamepad (Xbox, PlayStation, Switch Pro, MFi) works and can be
connected or disconnected while the game runs. Display Settings → Gamepad A/B
switches Xbox and PlayStation pads to the GBA's positions instead (right and top
buttons = A, bottom and left = B); Nintendo pads already have A on the right, so
they are the same either way.

Menu shortcuts: ⌘1–⌘5 pick the filter (nearest neighbour, sharp bilinear,
smooth bilinear, LCD grid, CRT scanlines), ⌘I toggles integer scaling, ⌃⌘F
toggles full screen (Escape leaves it), ⌘P pauses, ⌘R resets (the game's own
A+B+Select+Start reset). The filter and scaling choice are remembered.

View → Display Settings… (⌘,) adjusts the filters live; values are saved and
Reset to Defaults restores these:

| Knob | Default | Range | Effect |
|---|---|---|---|
| LCD gap strength | 0.35 | 0–1 | How dark the gaps between pixels are |
| LCD gap width | 0.15 | 0–0.5 | Gap width, as a fraction of a GBA pixel |
| LCD subpixel strength | 0.35 | 0–1 | How strongly each RGB stripe dims the other two channels |
| LCD gap bloom | 0.00 | 0–1 | Bright pixels shrink their gaps (1 = no gap on white) |
| LCD brightness | 1.30 | 0.5–2 | Makes up for the light the gaps and stripes take |
| CRT scanline strength | 1.00 | 0–1 | Depth of the dark lines between scanlines |
| CRT bloom | 0.80 | 0–1 | How much a line's height follows its brightness (0 = all lines alike) |
| CRT beam width (dark) | 0.14 | 0.05–0.6 | Beam width on black, as a Gaussian sigma in scanlines |
| CRT beam width (bright) | 0.34 | 0.05–0.6 | Beam width on white |
| CRT aperture grille | 0.10 | 0–1 | RGB column mask, one screen pixel per column |
| CRT brightness | 1.35 | 0.5–2 | Brightness of the scanline beams |
| GBA colour correction | 0.00 | 0–1 | Blend towards the GBA screen's own colours (Pokefan531's gba-color matrix): less saturated and warmer |
| Gamma | 1.00 | 0.5–2 | Output = colour^gamma; above 1 darkens mid-tones |

The LCD and CRT masks are box-filtered over each screen pixel, so they average
to neutral at any scale: greys stay grey in a window, at fractional scales and
in full screen. Beam and gap sizes follow one brightness value per pixel
(halfway between luma and the brightest channel), the same for all three
channels, so bloom adds no colour fringes. Each CRT pixel sums the beams of its
own scanline and the two around it, so bright lines spread into dark ones.

The save is `~/Library/Application Support/tmc_macos/tmc.sav`, an 8 KB EEPROM
image (*The Minish Cap* menu → Show Save Folder). `TMC_SAVE=path` overrides it.

### First launch

A distributable build asks for the ROM the first time it starts:

1. If there is no extracted data, an open panel asks for *The Minish Cap* (USA)
   ROM.
2. The app checks the file's size and SHA1. It names the problem when the file
   is the European, Japanese or a demo release, has a 512-byte copier header,
   is patched or a bad dump, is another game or cannot be read. The alert
   offers another file or quitting.
3. A progress window shows the extraction, on a background thread: verifying,
   then tables, graphics, maps and sound, then saving. It takes well under a
   second.
4. The game window opens.

The data goes to `~/Library/Application Support/tmc_macos/gamedata.bin`
(13.1 MB). It records the ROM's SHA1 and the version of the build's data
manifest. Each launch fills the game data from it in about 5–20 ms.

The app remembers where the ROM is, following it if it moves. When a new build
needs different data, it extracts again from that ROM without asking, or asks
again if the ROM is gone. *The Minish Cap* menu has Change ROM… and Re-extract
Game Data, which rewrite the data file while the game keeps running. Deleting
`gamedata.bin` brings back the first-launch flow.

For tests, `TMC_ROM=path` uses that ROM without asking (and without remembering
it), and `TMC_CACHE_DIR=dir` keeps the data file in `dir`. With `TMC_SELFTEST`,
a failure prints the alert's text and exits with status 1 instead of waiting
for a click. The progress window and any alert are saved as
`romsetup_progress.png` and `romsetup_error.png`.

A first launch from a clean data folder, through to the title screen:

```bash
TMC_CACHE_DIR=/tmp/tmc-data TMC_ROM=baserom.gba TMC_SAVE=/tmp/selftest.sav TMC_SELFTEST=/tmp/tmc-selftest build/native-rom/TMC.app/Contents/MacOS/TMC
```

`tmc` and `tmc-songs` from a distributable build read the same data file. If
it is missing or outdated, they extract it from `TMC_ROM`, or stop and say so.

`TMC_SELFTEST=dir` makes the app play itself through the title screen with
synthesized key presses, saving what it presented as PNGs in `dir`, then quit;
it needs no screen-recording permission. Besides the game screens it draws a
grey/colour test card (`pattern_<filter>_<integer|fractional|fullscreen>.png`)
with each filter at a whole-number scale, a fractional one and in full screen,
using the default knob values, and an image of the settings window:

```bash
TMC_SAVE=/tmp/selftest.sav TMC_SELFTEST=/tmp/tmc-selftest build/native/TMC.app/Contents/MacOS/TMC
```

### Running headless

Environment variables control the headless runner:

| Variable | Effect |
|---|---|
| `TMC_FRAMES=n` | Exit after n frames |
| `TMC_INPUT="frame:keys:duration,..."` | Hold buttons, e.g. `600:a:3,900:start+a:5` (`a b select start right left up down r l`) |
| `TMC_TRACE_TASK=1` | Print each change of the game's task/state/substate |
| `TMC_TRACE_DMA=1` | Log every DMA transfer |
| `TMC_WATCHDOG=n` | After n seconds, print a backtrace and exit (finds hangs) |
| `TMC_SAVE=path` | Back the emulated EEPROM with this file |
| `TMC_SCREENSHOT="frame,frame,..."` or `"every:n"` | Save PNG screenshots, rendered by the software renderer |
| `TMC_SCREENSHOT_DIR=path` | Where screenshots go (default `screenshots`) |
| `TMC_DUMP_IO=1` | Print display and interrupt registers with each screenshot |
| `TMC_WAV=path` | Record the run's sound to a WAV file (15768 Hz, or 48 kHz with `TMC_SOUND_QUALITY=hq`) |
| `TMC_ROM=path` | Distributable build: extract the game data from this ROM if needed. Development build: check the built-in data against it |
| `TMC_CACHE_DIR=dir` | Where the extracted game data lives (default `~/Library/Application Support/tmc_macos`) |
| `TMC_ROMDATA_VERBOSE=1` | Print how the game data was loaded and how long it took |

To see the logos, title screen and file select (Start, then A to leave the title
screen):

```bash
TMC_FRAMES=900 TMC_INPUT="400:start:5,600:a:5" TMC_SCREENSHOT="every:30" build/native/tmc
```

Crashes print a backtrace with the load address. Without debug info, macOS crash
reports in `~/Library/Logs/DiagnosticReports/tmc-*.ips` name the functions too.
For memory errors, configure a separate build with `-DTMC_SANITIZE=ON` and run with
`ASAN_OPTIONS=symbolize=0`, then resolve the printed offsets offline:

```bash
atos -o build/native-asan/tmc -arch arm64 0x100026464
```

(Attaching lldb or letting ASan symbolize may wait on a macOS developer-tools
permission prompt.)

### Exporting the music

`tmc-songs` renders the game's songs to WAV files with the same engine, without
running the game. `list` shows every song with its number, constant (from
`include/sound.h`), data label and music player:

```bash
build/native/tmc-songs list
```

`render` takes `all`, `bgm`, `sfx`, song numbers, ranges (`1-20`), constants
(`BGM_HYRULE_FIELD`) or labels (`bgmHyruleField`):

```bash
build/native/tmc-songs render bgm
```

```bash
build/native/tmc-songs render --hq BGM_HYRULE_FIELD 23 0x10A
```

Each song plays on its own music player until it ends. A song that loops plays
its loop twice (`--loops N`) and then fades out over 8 seconds (`--fade
SECONDS`), the way the game fades music. A song's loop is counted per track (a
backward GOTO), and the song has looped when its least-looped running track has.
Songs that neither end nor loop stop at `--max` seconds (600; SFX at 60).

Files land in `songs/authentic/` or `songs/hq/` under the current directory
(`--out DIR` to change), named by number and constant, e.g.
`025-BGM_HYRULE_FIELD.wav`, with title, album, track number and a comment in a
LIST/INFO chunk. Silent entries (the unused BGM slots) are skipped; `render all`
writes about 500 files (240 MB authentic) in about ten seconds.

| Mode | Output |
|---|---|
| default (authentic) | What the GBA plays: the engine's 8-bit mono DirectSound mix at 15768 Hz with its linear interpolation and 8-bit wraparound, plus the CGB channels, as 16-bit stereo at 15768 Hz |
| `--hq` | The same engine, envelopes and reverb, mixed in floating point at 48 kHz (`--rate HZ`) with cubic interpolation and per-frame volume ramps |

## Layout

| Path | Purpose |
|---|---|
| `port/include/port/hw.h` | Host buffers for I/O registers, palette, VRAM, OAM; DMA and BIOS entry points |
| `port/include/port/platform.h` | Frame boundary and soft reset, implemented by the platform layer |
| `port/src/hw.c` | Hardware buffers and DMA (immediate DMA works; timed DMA is latched for the renderer) |
| `port/src/bios.c` | BIOS calls in C (copies, decompression, division, affine, VBlankIntrWait) |
| `port/src/interrupts.c` | `IntrMain` replacement dispatching through the game's handler table |
| `port/src/headless.c` | Headless platform and process entry; `TMC_FRAMES=n` stops after n frames |
| `port/src/frame.c` | Frame boundary shared by both platforms: latch keys, raise VCOUNT and VBLANK |
| `port/src/macos/game.c` | App platform layer: game thread, 59.73 Hz pacing, frame hand-off |
| `port/src/macos/app.m` | Window, menus, full screen, save location |
| `port/src/macos/renderer.m`, `shaders.metal` | Metal presentation and filters (shaders compiled at launch) |
| `port/src/macos/input.m` | Keyboard and GameController input |
| `port/src/macos/settings.m` | Display Settings window and the saved filter knobs |
| `port/src/macos/selftest.m` | `TMC_SELFTEST` scripted run with PNG captures |
| `port/src/macos/romsetup.m` | First launch: ROM open panel, progress window, errors, Change ROM… |
| `port/src/romdata.c` | ROM checks, extraction to `gamedata.bin`, filling the game data from it |
| `port/tools/datagen/provenance.py` | Tells asset bytes (from `baserom.gba`) from source-written ones |
| `port/scripts/asset_overlap.py` | Lists what a program shares with the extracted asset files |
| `port/src/data_glue.c` | Code embedded in data, demo-only symbols, the save header |
| `port/src/asm/` | C versions of every function in `asm/src/*.s` |
| `port/src/eeprom.c` | Emulated save EEPROM (replaces `src/eeprom.c`) |
| `port/src/sound/` | Native sound: `asm/lib/m4a_asm.s` in C, the mixer, CGB channels, WAV output (see [Sound](#sound)) |
| `port/src/macos/audio.c` | Core Audio output with drift control |
| `port/src/tools/tmc_songs.c` | The song exporter |
| `port/src/trace.c` | Game-state tracing for the headless runner |
| `port/src/ppu.c` | Software GBA renderer (BG modes 0-2, sprites, windows, blending) and PNG output |
| `port/tools/datagen/` | GBA data objects → native arm64 assembly (see below) |
| `port/scripts/link_report.py` | Classifies unresolved symbols into the remaining worklist |

Header hooks (all `PLATFORM_NATIVE`-only): `gba/defines.h` and `gba/io_reg.h`
point `PLTT`/`VRAM`/`OAM`/`REG_BASE` at the host buffers; `gba/macro.h` routes
`DmaSet`/`DmaStop` to the port; `gba/syscall.h` routes `SystemCall`;
`global.h` disables GBA struct-size asserts; `gba/types.h` adds `PtrU32`, an
integer field that holds a pointer (`u32` on GBA, `uintptr_t` natively);
`gba/defines.h` adds `GBA_PTR(addr)` for fixed GBA addresses in C code, mapped
natively to the host buffer or variable at that address.

## Phase 0 results

**Compile:** 608 of 619 C files build for arm64. The other 11 (`cutscene.c` and
ten `worldEvent*.c`) are listed in `TMC_NATIVE_PENDING`: their static
`EntityData` tables store pointers in the `u32 spritePtr` field, which cannot be
a constant initializer with 64-bit pointers.

Source changes needed to get there, all verified to keep `tmc.gba` matching:
five GCC lvalue casts rewritten in standard C, five `static` definitions that
conflicted with `extern` declarations, and the m4a `swi 0x2A` routed through
`SystemCall`.

**Link:** 1348 unresolved symbols, all accounted for:

| Count | Defined in | Native plan |
|---:|---|---|
| 539 | `data/*.s` tables | Main Phase 1 decision, see below |
| 507 | songs generated from assets | Phase 4 (audio) |
| 130 | `linker.ld` fixed EWRAM/IWRAM addresses | 121 become ordinary globals; 9 `ram_*` IWRAM copies alias their ROM functions |
| 117 | `asm/src/*.s` | Rewrite in C; 23 are interworking veneers that become aliases |
| 29 | `asm/lib/m4a_asm.s` | Phase 4, C mixer |
| 12 | `asm/lib/libagbsyscall.s` (BIOS) | Phase 1: `CpuSet`, `Div`/`Mod`, `LZ77UnComp*`, affine sets, etc. in C |
| 14 | the 11 pending C files | Unblocked by the data decision |

Remaining asm functions by file: `code_08003FC4.s` 25, `veneer.s` 23,
`code_080043E8.s` 16, `code_08001A7C.s` 13, `player.s` 12, `script.s` 10,
`enemy.s` 9, others 10.

**64-bit hazards** flagged by the compiler (the Phase 1 worklist, visible in the
normal build output): 68 pointer→int casts, 46 int→pointer casts, 17 more via
`void*`, plus 24 raw GBA address literals such as `(SaveHeader*)0x2000000`.

## Game data (phase 1)

`port/tools/datagen` converts the assembled GBA data objects
(`build/USA/data/**/*.o`, about 13.7 MB with 14k pointers) into arm64 assembly
in `build/native/data`:

- **Types** come from the game sources through libclang. GBA layouts follow
  agbcc's rules (4-byte pointers, structs rounded up to 4 bytes) and match all
  of the decomp's struct-size asserts.
- **Pointer-free data** (98% of the bytes) is copied byte for byte: each section
  is reproduced in one piece so labels keep their exact relative offsets.
- **Data with pointers** is re-laid out in a separate section: pointer tables
  are widened to 8 bytes, typed tables are converted field by field (bitfields
  and unions included), and pointers into the middle of tables are remapped.
- **Script bytecode** keeps its bytes; each pointer slot holds a tagged index
  into a per-object table of native pointers (`script_tag.h` has the tag).
- **RAM variables** placed by `linker.ld` become native storage (`ram.s`),
  with shared addresses kept as aliases.
- **Songs** stay raw: their track data holds absolute ROM addresses without
  relocations. `sound_map.s` (`gSoundRomMap`) maps every GBA address in the
  sound data to its native counterpart, linearly for byte-for-byte data and by
  element for the re-laid-out voicegroups, and the sound engine translates
  addresses through it.

In a distributable build (`--no-bake`) every asset byte keeps its place but is
left zero. The run tables in `__DATA,__tmcruns` say
where it goes. `rom_manifest.s` holds the ROM extents the data file stores,
and `rom_manifest.txt` lists every run.

About 1,100 tables have no C declaration; `hints.txt` types them, mostly by the
data macro they start with. `build/native/data/notes.txt` lists C declarations
that need a pointer type natively (e.g. pointer tables declared as `u32[]`).

## Status

The native build links with no unresolved symbols and runs, in the app or
headless: through the Nintendo/Capcom logos and title screen into file select, and for tens of
thousands of frames when left alone. The software renderer draws these screens
correctly (screenshots via `TMC_SCREENSHOT`); mid-frame effects are not modelled
yet. Every function in `asm/src` has a C
version, the BIOS calls are in C, and the save EEPROM is emulated.

Remaining for phase 1, so that gameplay runs:

- About 170 compiler warnings for pointer/integer casts, mostly in
  `enemy/chuchuBoss.c` and `entity.c`. Many are decomp idioms that access fields
  by GBA byte offset (`*(u8*)((int)entity + 0x85)`), which need real field names
  natively. Creating a new file currently crashes on one of these in text code.
- The C declarations listed in `build/native/data/notes.txt` (pointer tables
  declared as `u32[]`).
- Callers that declare `DoTileInteraction`'s returned pointer as `s32`.

## Sound

The game's m4a engine runs natively: `src/gba/m4a.c` as it is (with
`PLATFORM_NATIVE` fixes for 64-bit pointers, hardware setup and GBA addresses in
song headers) and `asm/lib/m4a_asm.s` rewritten in C in `port/src/sound`:

| File | Contents |
|---|---|
| `m4a_engine.c` | The sequencer (`MPlayMain`), song commands (`ply_*`), note allocation, channel chains |
| `m4a_mixer.c` | `SoundMain` and the DirectSound mixer (`SoundMainRAM`), authentic and HQ |
| `psg.c` | The CGB channels (two squares, wave, noise) synthesised from the sound registers `CgbSound` writes |
| `m4a_addr.c` | `M4aGbaPtr`: GBA addresses in song data to native pointers through `gSoundRomMap` |
| `wav.c` | WAV writer and `TMC_WAV` |

Once per frame (the mid-frame VCOUNT interrupt, as on the GBA) `SoundMain` runs
the music players and `CgbSound`, mixes the DirectSound channels, synthesises the
CGB channels for the same frame and hands 264 stereo samples (at the engine's
15768 Hz) to the platform: the app's Core Audio output, `TMC_WAV`, or the song
exporter. TMC's engine mixes DirectSound in mono; only the CGB channels are
panned.

In the app the samples go through a lock-free ring to an output unit whose
render callback resamples them to the device rate. The game keeps its own
59.73 Hz timer; the resampling ratio follows the ring's fill level (about 50 ms)
by at most 0.5%, so the two clocks never drift apart. Fast-forward drops audio
once a quarter second is buffered; pausing fades out. `TMC_AUDIO_WAV=path`
records what the app actually sent to the device and prints underrun, drop and
drift statistics on quit.

Known differences from hardware:

- The CGB channels are synthesised from the registers' state once per frame
  (as `CgbSound` leaves it), not from each write; m4a only writes them once per
  frame, so this matters only for the order of writes within a frame.
- CGB output is box-filtered (oversampled to ~96 kHz) rather than produced by
  the GBA's PWM DAC; the authentic mode does not model the DAC's 9-bit
  resampling at 65.5 kHz or the speaker's response.
- The final mix is DC-blocked (5 Hz high-pass), like the GBA's output coupling.
- Key-split voices (`voice_keysplit`) pick the note's key directly; TMC has none.
