# Native macOS port

Native arm64 macOS build of the decompiled game code, with a Metal renderer and
GameController input planned. The GBA build in the repository root stays
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

This builds `build/native/tmc`, which runs the game headless (no window, picture
or sound yet). If the link ever breaks, `cmake --build build/native --target
link-report` groups the unresolved symbols by where the GBA build defines them.

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

## Layout

| Path | Purpose |
|---|---|
| `port/include/port/hw.h` | Host buffers for I/O registers, palette, VRAM, OAM; DMA and BIOS entry points |
| `port/include/port/platform.h` | Frame boundary and soft reset, implemented by the platform layer |
| `port/src/hw.c` | Hardware buffers and DMA (immediate DMA works; timed DMA is latched for the renderer) |
| `port/src/bios.c` | BIOS calls in C (copies, decompression, division, affine, VBlankIntrWait) |
| `port/src/interrupts.c` | `IntrMain` replacement dispatching through the game's handler table |
| `port/src/headless.c` | Headless platform and process entry; `TMC_FRAMES=n` stops after n frames |
| `port/src/data_glue.c` | Code embedded in data, demo-only symbols, the save header |
| `port/src/asm/` | C versions of every function in `asm/src/*.s` |
| `port/src/eeprom.c` | Emulated save EEPROM (replaces `src/eeprom.c`) |
| `port/src/sound_stub.c` | Silent m4a API until the native sound engine (replaces `src/gba/m4a.c`) |
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
- **Songs** stay raw for now: their track data holds absolute ROM addresses
  without relocations, which the native sound engine will translate (phase 4).

About 1,100 tables have no C declaration; `hints.txt` types them, mostly by the
data macro they start with. `build/native/data/notes.txt` lists C declarations
that need a pointer type natively (e.g. pointer tables declared as `u32[]`).

## Status

The native build links with no unresolved symbols and runs headless: through the
Nintendo/Capcom logos and title screen into file select, and for tens of
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

Sound is silent until phase 4: the m4a engine reads raw GBA addresses from song
data, so `src/gba/m4a.c` is replaced by a stub for now.
