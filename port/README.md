# Native macOS port

Native arm64 macOS build of the decompiled game code, with a Metal renderer and
GameController input planned. The GBA build in the repository root stays
byte-matching: every port change is either under `port/` or behind
`#ifdef PLATFORM_NATIVE`.

## Setup

One-time machine setup. The native build only needs steps 1–3; steps 4–5 are
for checking that the GBA build still matches, which every change to shared code
should do.

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

4. **GBA toolchain** (matching check only): `arm-none-eabi-gcc` and agbcc.
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

5. **pycparser** (matching check only): the GBA `make` imports it. Homebrew's
   Python refuses global installs, so use a venv and put it on `PATH` when
   running `make`:

   ```bash
   python3 -m venv .venv && .venv/bin/pip install pycparser
   ```

   ```bash
   PATH="$PWD/.venv/bin:$PATH" make -j$(sysctl -n hw.ncpu)
   ```

   A matching build ends with `tmc.gba: OK`.

## Building

Build the host tools and extract assets (CMake 4 needs the policy override for a
bundled dependency; recent clang needs the warning relaxed):

```bash
CMAKE_POLICY_VERSION_MINIMUM=3.5 cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=tools -DCMAKE_CXX_FLAGS=-Wno-error=deprecated-literal-operator -S tools -B tools/cmake-build
```

```bash
cmake --build tools/cmake-build -j --target install
```

```bash
make extract_assets
```

Configure and build the native game objects:

```bash
cmake -S port -B build/native
```

```bash
cmake --build build/native -j
```

The `tmc` executable does not link yet. To see what is still missing:

```bash
cmake --build build/native --target link-report
```

This writes `build/native/link_report.txt`, grouping every unresolved symbol by
where the GBA build defines it.

To confirm the GBA build still matches, run `make` as in setup step 5.

## Layout

| Path | Purpose |
|---|---|
| `port/include/port/hw.h` | Host buffers for I/O registers, palette, VRAM, OAM; DMA and BIOS entry points |
| `port/src/hw.c` | Their implementation (immediate DMA works; timed DMA is latched for the renderer) |
| `port/src/main.c` | Process entry, calls `AgbMain` |
| `port/scripts/link_report.py` | Classifies unresolved symbols into the remaining worklist |

Header hooks (all `PLATFORM_NATIVE`-only): `gba/defines.h` and `gba/io_reg.h`
point `PLTT`/`VRAM`/`OAM`/`REG_BASE` at the host buffers; `gba/macro.h` routes
`DmaSet`/`DmaStop` to the port; `gba/syscall.h` routes `SystemCall`;
`global.h` disables GBA struct-size asserts.

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

## The Phase 1 decision: pointers in data

The data in `data/*.s` holds about 13,800 `.4byte` words, many of them pointers,
and the script bytecode embeds 32-bit pointers to functions and data (the `Call`
macro emits `.4byte \function`). There are two ways to bring that to a 64-bit
host:

1. **Convert data to C** so the compiler lays out real pointers. This is the
   cleanest result and keeps the code natural, but it is the larger job:
   structs with pointer fields change size, and script bytecode needs a pointer
   table (scripts store an index or offset instead of an address).
2. **Keep data 32-bit and translate at use.** Data stays in its GBA layout
   inside a loaded image, every stored pointer is a GBA address, and accessors
   map addresses to host memory or native functions. There is less to change up
   front, but every pointer load in the C code has to go through the translator.

This needs deciding before Phase 1 work starts.
