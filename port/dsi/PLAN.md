# DSi port: plan for a proof of concept

Goal: a homebrew `.nds` that runs in DSi mode (TWiLight Menu++ or Unlaunch), reads
the player's own `baserom.gba` from the SD card, and runs the decompiled game
natively on the ARM9. This replaces GBARunner2 and its black borders. The proof of
concept ends at **"boots in DSi mode, loads and relocates the ROM, reaches the title
screen with graphics, no sound"**.

Status: milestones 0 to 4 done, apart from milestone 1's check on the DSi itself
(see [the table](#proof-of-concept-milestones)). The toolchain, the build and
the test setup are in [README.md](README.md).

## Approach in one paragraph

The DSi's ARM946E-S runs the same 32-bit ARM/Thumb code as the GBA, so the game's C
is compiled with the **GBA's 32-bit data layout**. None of the Mac port's 64-bit
re-layout (datagen) applies. At boot the ROM file is read into main RAM. Words in
the ROM data that point into the ROM, at game code or into game RAM are then
patched from a relocation list made at build time from the decomp's own link. The
list holds offsets and symbol names only, no ROM bytes. Game code reaches ROM data
through linker-defined symbols that point into that RAM copy. Game RAM keeps its
GBA addresses where possible. Hardware access goes through the same small
`port/hw.h` layer as the Mac port: shadow buffers that a DSi presenter copies to
the DS 2D engine at VBlank.

## Measurements

From the USA build (`build/USA`, `tmc.elf`, and a `-q` relink, see
[README.md](README.md#rom-data-relocation-figures)), and the BlocksDS toolchain
installed here.

### ROM contents (14,581,156 bytes used of 16 MB)

| Kind | Bytes | Notes |
|---|---|---|
| data objects (`data/**`) | 13,751,384 | assets and data tables; read from the ROM copy |
| of which sound (`data/sound/**`) | 4,104,456 | 3,904,944 samples + 112,824 songs + 86,544 voicegroups |
| C code (`src/**` .text) | 631,300 | compiled for the ARM9 instead |
| C rodata (`src/**` .rodata) | 181,756 | compiled for the ARM9 instead |
| asm code | 16,704 | replaced by `port/src/asm/*.c` |

The ROM is laid out as non-sound data and code interleaved (257 code runs, 254 data
runs) from 0x08000000 to 0x089FC99C, then one sound block of 4.10 MB to 0x08DE7D34,
then 0x70 bytes of `src/eeprom.c` rodata.

### ARM9 code size

All 621 game C files (`src/**/*.c` minus `src/eeprom.c`) compile with BlocksDS GCC
16.2 for `-mthumb -mcpu=arm946e-s`, using the GBA layout (no `PLATFORM_NATIVE`)
with `NON_MATCHING`, `-fwrapv -fno-short-enums`, and `u32` as `unsigned int` (see
[ABI notes](#abi-notes)).

| Flags | .text | .rodata | .bss |
|---|---|---|---|
| `-O2` | 632,340 | 186,366 | 896 |
| `-Os` | 548,270 | 179,242 | 896 |

Agbcc's Thumb code for the same C is 631,300 bytes, so GCC `-O2` Thumb is about
the same size.

Other parts of the binary, estimated:

- the C replacements for the asm (`port/src/asm`, 9 files): ~20 KB;
- libnds, picolibc and FAT: ~80 KB (the hello world's .text plus DLDI is 72 KB);
- the DSi port layer: ~40 KB.

**The ARM9 binary comes to about 1.0 MB.**

Measured at milestone 4 (`make -C port/dsi figures`, `build/dsi/tmc_dsi.map`);
milestones 3 and 2 in brackets:

| Part | Bytes |
|---|---|
| `.text` | 690,832 (687,528; 681,760) |
| `.rodata` (of which the relocation lists 50,904) | 205,812 (205,352; 201,836) |
| `.data`, `.itcm`, `.dldi`, crt0 and `.secure` | 24,416 (24,160; 20,592) |
| **ARM9 image in the `.nds`** | **921,060** (917,040; 904,244) |
| `.bss` | 344,812 (336,032; 334,428) |
| **in RAM, 0x02040000-0x02173E34** | **1,261,108** (1,248,564; 1,237,676) |

- Milestone 3 added the FAT read path (fatInitDefault, `fread`), the ROM checks,
  SHA-1 (4.4 KB of ARM code in ITCM) and the kept-EWRAM list (2,854 bytes).
  Milestone 4 added `frame.c`, `trace.c`, `hw_crc.c` and the frame records (8 KB).
- The game must stay reachable from `main`: the link drops unreachable sections
  (`--gc-sections`), and with `AgbMain` unreachable `.text` falls by 30 KB.
- `frame.c`'s two framebuffers for the Mac's PPU comparison mode (150 KB) are
  left out on the DSi (`PLATFORM_DSI`), which has no software PPU.
- The heap between the binary and the ROM copy is 606 KB (0x02174850-0x0220C000).
- Game C and `port/src` together: 841,021 bytes of code and rodata, close to the
  estimate (818,706 for the game C plus ~20 KB).
- `.bss`: 138 KB DSi layer (VRAM shadow 96 KB, IWRAM block 32 KB, boot log 4 KB,
  palette, OAM and I/O 3 KB) and 191 KB game and port. 176 KB of the latter are
  the Mac mixer's output buffers (`sHqRing`, `sStereo`, `sMono` in
  `port/src/sound/m4a_mixer.c`), unused on the DSi; they go when the ARM7 takes
  over sound.

### Relocation list

The relinked ELF has 37,307 `R_ARM_ABS32` relocations in the ROM. Only those inside
data objects matter, because the code and C rodata are recompiled: **15,849
words**.

| Points to | Count | How it is fixed up |
|---|---|---|
| ROM data | 11,300 | add (copy base − 0x08000000) |
| game functions (`src/**` code) | 3,109 | linker: 3,109 unique global symbols |
| C rodata | 15 | linker: unique global symbols |
| EWRAM | 1,425 | `gMapTop`/`gMapBottom` + addend (1,394) or exact symbol (31); **none at all** if EWRAM keeps its GBA address (below) |
| IWRAM | 0 | |

Every one of the 15,849 resolves: either within the ROM data, or to a uniquely
named global symbol (plus an addend). 15,474 of the gaps between consecutive
offsets are under 256 bytes. The busiest objects are entity headers (4,955),
map headers (2,059), scripts (1,907), voicegroups (1,134) and sprite pointers (591).

Sizes, encoded as a 2-byte delta stream for the "add base" entries and
`.word offset, symbol+addend` pairs (8 bytes) for the symbol entries:

- with EWRAM moved: ~23 KB + 36 KB = ~59 KB;
- with EWRAM at its GBA address: ~23 KB + 25 KB = ~48 KB.

The list holds positions and symbol names from the link, not ROM content. It is
generated from the build, like `tmc.map`. Making the list needs a built GBA tree,
which needs the user's ROM for asset extraction, the same as the Mac build.

**ROM addresses not covered by relocations.** Some aligned words in data objects
hold a value in the ROM range with no relocation. There are 18,342 + 1,179; the
1,179 land exactly on a symbol.

- Outside sound, checked by hand, these are coincidences: tile and compressed map
  bytes, script opcode pairs, and values such as 0x08000000 or 0x08080808.
- **Song data is real:** song headers and tracks are `.incbin`'d raw (for example
  `build/USA/assets/sounds/bgmTitleScreen_header.bin`), so their voicegroup and
  track pointers and GOTO/PATT targets have no relocations. There are 507 hits
  exactly on symbols in `sounds.o`.

  The sound driver will treat song pointers as GBA addresses and translate them
  when it reads them (or walk the song table at boot). This doesn't matter for the
  proof of concept, which has no sound.

### RAM budget (DSi mode)

16,384 KB of main RAM. The top 48 KB (from 0x02FF4000) belong to libnds, the
loader and DTCM. That leaves 16,336 KB, which matches the measurement: a 99 KB
hello binary plus a 16,236 KB heap.

| Item | KB |
|---|---|
| ARM9 binary (code, rodata, data, relocation list) | ~1,020 |
| GBA EWRAM block (256 KB) + IWRAM block (32 KB) | 288 |
| hardware shadows (VRAM 96 KB, palette, OAM, I/O) | 99 |
| libnds heap: FAT cache, file buffers, misc | ~256 |
| **subtotal without ROM** | **~1,663** |

| ROM copy option | ROM KB | Total KB | Left of 16,336 |
|---|---|---|---|
| A. whole used image, 0x08000000-0x08DE7DA4 | 14,239 | 15,902 | **~434** |
| B. data objects only, packed | 13,429 | 15,092 | ~1,244 |
| C. proof of concept: 0x08000000-0x089FC99C (everything before sound) | 10,227 | 11,890 | ~4,446 |

The DSi also has 768 KB of NWRAM. It can take the hardware shadows, the IWRAM
block or sound buffers if main RAM gets tight.

**Conclusion: everything fits, including all music samples in RAM.** Samples don't
need to stay on the SD card.

- The proof of concept loads option C: one contiguous read, and every
  ROM-to-ROM fix-up is a constant add.
- The full game uses option A, which keeps the same simple add and has 0.4 MB to
  spare.
- **Changed at milestone 2: the build uses option A from the start.** The sound
  sequencer runs on the ARM9 from milestone 4 (its RAM state is compared with the
  Mac's), and it reads song data. Measured: the copy is at 0x0220C000-0x02FF3DA4,
  and the heap between the binary and the copy is 618 KB at milestone 3
  (0x02171750-0x0220C000), against the ~256 KB budgeted. `gen_link.py --rom-end`
  still makes an option C build.
- If the binary grows past the estimate, option B (packing out the 0.83 MB of code
  holes) buys 0.8 MB. ROM-to-ROM fix-ups then become run-table lookups at boot.

## Build structure

### Where it lives

As built at milestone 2:

- `port/dsi/Makefile`: its own makefile with BlocksDS's compiler, libnds, its
  default ARM7 core (`arm7_maxmod.elf`) and the specs in `port/dsi/ld/` (linked at
  0x02040000). Outputs go to `build/dsi/` (`tmc_dsi.elf`, `tmc_dsi.nds`,
  `tmc_dsi.map`). Like the Mac build, it needs the GBA build first (`make -j4`).
- `port/dsi/src/`: the DSi layer, which includes libnds and never the game's
  headers. `dsi.h` is its interface; `main.c` (boot log, heap below the copy),
  `rom.c` (loading, which is a stub until milestone 3, and relocation),
  `hw_dsi.c` (`port/hw.h`: shadows, IWRAM block, DMA, address mapping,
  `M4aGbaPtr`), `platform_dsi.c` (`PlatformVBlank`, soft reset, the presenter
  stub).
- `port/dsi/tools/gen_link.py`: generates into `build/dsi/gen/`, from the `-q`
  relink of the GBA build, `tmc.map`, `linker.ld` and the list of symbols the DSi
  objects define (those win over the ROM copy's):
  - `rom_symbols.ld`: `PROVIDE(Sym = __rom_copy + 0x123456);` for the 16,545
    global symbols in data objects and in the two data tables inside
    `asm/src/code_08001A7C.s` (the regions datagen also treats as data);
  - `ram_layout.ld`: the 89 EWRAM symbols at their GBA addresses (plus
    `gNativeSaveHeader` at 0x02000000), the 94 IWRAM ones in `gDsiIwram` (the
    `ram_*` routines are C functions), and checks that the binary is above EWRAM
    and below the copy;
  - `rom_relocs.s`: the relocation list with its counts;
  - `gen_report.txt`: the figures below.
- The game C: `src/**/*.c` minus `src/eeprom.c`, the same set as the Mac
  `tmc_game` target, plus from the Mac port `port/src/{bios,interrupts,eeprom,data_glue}.c`,
  `port/src/asm/*.c` and the sound engine (`m4a_engine.c`, `m4a_mixer.c`,
  `psg.c`). `m4a_addr.c` (datagen's sound map) is replaced by `M4aGbaPtr` in
  `hw_dsi.c`.

Relocation figures from the generator (milestone 2):

| Pointer words in data | Count | At boot |
|---|---|---|
| to ROM data | 11,547 | add (copy base − 0x08000000), 23,102 bytes of deltas |
| to game functions | 3,111 | symbol entry: the linker writes the ARM9 address, Thumb bit set (all 3,111 checked in the ELF) |
| to C rodata and port data | 6 | symbol entry |
| to EWRAM | 1,425 | none: same address |
| **total** | **16,089** | 3,117 symbol entries, 24,936 bytes |
| raw VRAM addresses in pointer fields (since milestone 5) | 352 | symbol entry: `gHwVram` + offset |
| raw EWRAM addresses in pointer fields (since milestone 5) | 17 | none: same address |

The last two rows are not relocations: the data holds GBA hardware or RAM
addresses as plain numbers, such as a tileset's VRAM destination
(`MapDataDefinition.dest`, used by `LoadMapData`). The Mac's datagen already
points those at the shadows because it knows which fields are pointers; the DSi
build runs it with `--raw-pointers` for the list (`raw_pointers.txt`, about a
minute, only when the GBA build changes). Without them, room tilesets went into
the real DS VRAM bank A, the presenter overwrote them, and rooms showed the
previous room's tiles (found after milestone 5, when buttons got past the title
screen).

That is the 15,849 of the measurements plus 240 in the two asm data tables. Nine
targets the measurements counted as C rodata are byte-aligned data symbols
(`gSpriteAnimations_*`) at odd addresses, so they are ROM data. 899 of the words
are not word-aligned (data is 2-aligned), and `rom.c` patches them byte-wise.

Shared files the DSi build needed, all no-ops for the Mac (its objects and
binaries are byte-identical) and the GBA (`tmc.gba: OK`):

- `include/common.h`: `zMalloc`'s save-state wrapper only on the Mac;
- `port/include/port/state.h`: `STATE_EXCLUDE` is empty on the DSi (the Mac's is
  a Mach-O section);
- `port/src/asm/script.c`: script pointer operands are plain addresses (no
  datagen tag);
- `port/src/asm/code_08001A7C.c`: the `GetFuserId` symbol uses the platform's
  label prefix (`_` only on Mach-O);
- `port/src/data_glue.c`: `gNativeSaveHeader` comes from the linker on the DSi.

### Defines

- Game files: `PLATFORM_DSI=1 PORT_HOSTED=1 NON_MATCHING=1 USA ENGLISH REVISION=0`.
- Game file flags: `-mthumb -mcpu=arm946e-s+nofp -O2 -std=gnu11 -fwrapv -fno-short-enums
  -fno-strict-aliasing -fno-aggressive-loop-optimizations -fno-delete-null-pointer-checks
  -ffunction-sections`, and `u32`/`s32` as `int` (ABI notes). GCC 16 defaults to
  C23, where `void f()` takes no arguments, so the Mac's gnu11 is needed. The
  other three keep GCC from assuming what agbcc never did: GCC warned that it
  would cut 7 loops short in `gleerok.c`, `figurineMenu.c` and `pauseMenu.c`,
  which index past an array into the next field.
- A few hot files can be compiled `-marm` or placed in ITCM once profiled.

`PLATFORM_NATIVE` used to mean three things at once:

1. hardware is a host-side buffer (`REG_BASE` → `gHwIo`, VRAM/palette/OAM, `DmaSet` →
   `HwDmaSet`, `GBA_PTR` → `HwGbaPtr`, `SystemCall`, the BIOS-reserved IWRAM words);
2. fixes for behaviour that only agbcc's code got right: reading `r1` after `Div`,
   NULL reads that hit the BIOS on the GBA, unsigned offsets;
3. the 64-bit layout: the entity slots grow (`nativeSlack`, `nativeScriptContext`,
   `Enemy.child`, manager subtypes up to 120 bytes) and the static asserts that
   check it.

The DSi wants 1 and 2 but not 3. Compiled with `PLATFORM_NATIVE` for the ARM9,
the 64-bit static asserts fail in 393 files: "player entity must fill one entity
slot", for example.

**Done (milestone 1b).** The 235 `PLATFORM_NATIVE` sites in `src/` and
`include/` (106 files) are now:

- 198 test `PORT_HOSTED` (kinds 1 and 2);
- 37 test `PORT_LAYOUT_64` (kind 3, in 17 files):
  - the entity-slot structs (`GenericEntity`, `PlayerEntity`, `Enemy`,
    `SpritePriority`), `ENTITY_SCRIPT_CONTEXT`, the manager `Temp`/`gUnk_02033290`
    and `EraseAllEntities` sizes;
  - the subtype overlays in `chuchuBoss`, `chuchu`, `vaatiArm`, `businessScrub`,
    `railtrack`, `removableDust` and `lilypadLarge`, and the code that reaches
    their moved fields (`childSlot`, `sleepBubble`);
  - `RegisterRoomEntity`'s `EntityData` copy;
  - the disabled `static_assert` in `global.h`, so the GBA size asserts are active
    on the DSi and pass.

`port/CMakeLists.txt` defines all three for every target that sees game headers
(`TMC_GAME_DEFINITIONS`), datagen's `ctypes_db.py` passes the same, and
`include/gba/types.h` rejects inconsistent combinations.

How it was checked:

- **GBA:** `tmc.gba: OK`.
- **Mac:** a full rebuild (656 C files and datagen) gives **all 963 objects and
  the `tmc`, `TMC.app` and `tmc-songs` binaries byte-identical** to before the
  split. That is a stronger check than the planned frame-hash and room-smoke runs,
  so those weren't needed.
- **DSi layout:** all 621 game files compile for the ARM9 as the DSi build
  (`-DPLATFORM_DSI=1 -DPORT_HOSTED=1`). `port/dsi/tools/layout_diff.py` compares
  their DWARF with the same files built with the plain GBA layout, per object and
  declaration line: 9,624 type and 6,358 global definitions.
  - One differs, and is fine: `struct_0812816C` in `figurineMenu.c` splits an
    8-byte filler into a pointer and a `u32` at the same offsets.
  - Types seen only in one build are header types that extra code paths pull in;
    their layouts are checked in the objects that have them in both.

Hosted behaviour that milestone 2 has to provide on the DSi:

- **`M4A_PTR`/`M4aGbaPtr`** already treat song data as GBA addresses, which is
  what the unrelocated song pointers need. It must pass through addresses that are
  already relocated (voicegroups).
- **`gNativeSaveHeader`** (`save.h`) and the linker aliases the hosted code
  replaces with field expressions (`gUnk_02035542`, `gUnk_03000420`, ...) still
  resolve by the GBA RAM layout.
- **RAM zeroing:** `main.c` relies on zero-initialised globals when hosted. The
  DSi's NOLOAD EWRAM/IWRAM blocks must be cleared at boot.
- **Writes to "ROM"** (`spearMoblin`, `playerItemGustJar`, `kinstoneMenu`) are
  skipped when hosted. That matters because the DSi's ROM copy is writable.
- Read-only access checks such as `TMC_ROM_WRITES` are Mac-only.

### ABI notes

- arm-none-eabi GCC defaults to **short enums**. That would change the size of
  enum-typed struct fields from the GBA's, and it breaks 6 files with conflicting
  prototypes (`u32` versus enum parameters). The game is built with
  `-fno-short-enums`. BlocksDS's libraries keep short enums; the linker's
  enum-size warning is silenced with `--no-enum-size-warning`. This is safe because
  no enum crosses the boundary between game and libnds.
- The game's `u32`/`s32` come from `uint32_t`, which is `unsigned long` on
  arm-none-eabi. Under `PLATFORM_DSI`, `include/gba/types.h` makes them
  `unsigned int`/`int`, as on the host and in effect on agbcc. The measurement used
  `-D__UINT32_TYPE__` instead.
- Game files never include libnds, because libnds' own `u32` would clash. The DSi
  layer talks to the game only through `port/include/port/hw.h` and plain
  `stdint` types, the same boundary as the Mac port.

### Reusing the Mac port

| Mac port piece | On the DSi |
|---|---|
| `port/include/port/hw.h` interface (`gHwIo`/`gHwVram`/`gHwPltt`/`gHwOam`, `HwDmaSet`, `HwGbaPtr`, `HwSetKeys`) | **same interface**, new implementation in `port/dsi/src/hw_dsi.c` |
| `port/src/bios.c` (GBA SWIs in C: LZ77, CpuSet, Div, Sqrt, ArcTan, affine sets) | **as is**: the DS BIOS differs, so the C versions are the safe choice |
| `port/src/asm/*.c` | as is (check for 64-bit assumptions) |
| `port/src/interrupts.c` (IntrMain dispatch) | as is, called from a libnds VBlank/HBlank IRQ handler |
| `port/src/eeprom.c` (8 KB EEPROM emulation, file-backed) | as is, file on `sd:/` |
| `port/src/sound/m4a_engine.c`, `m4a_mixer.c`, `psg.c` | the sequencer and the integer (authentic) mixer run on the ARM9, so the engine's state evolves exactly as on the Mac; under `PLATFORM_DSI` `SoundMain` stops there and `PsgAdvance` steps the CGB channels' lengths, sweep, envelopes and NR52 with the same clock as `PsgRender`, without synthesising (milestone 4: 54 ms of soft-float mixing a frame became 3.3 ms of frame work). Later the ARM7 plays the sound |
| `port/src/headless.c` input script (`TMC_INPUT` syntax) and frame-hash output | parser moved to a shared file, read from `sd:/tmc/input.txt`; hashes into a RAM ring buffer |
| `port/src/ppu.c` (software PPU) | not used at run time; the Mac's picture is the reference for the DS's (milestone 5). `TMC_PPU_DS_EFFECTS=1` makes it blend and fade with the DS's rounding |
| `port/src/frame.c` (per-line timing) | **as is**: `HwRunFrame` is `HwRunLines` then `HwRunVBlank`. On the DSi the game's lines run before the DS VBlank and record each line's display registers (`gHwLineCapture`); the presenter runs in the VBlank, then `HwRunVBlank` (milestone 5). The PPU hooks on the DSi follow the affine reference points for the presenter (`port/dsi/src/present.c`) |
| datagen, `romdata.c`, `state.c` (save states), `heap.c` type map, `src/apple`/`macos`/`ios` | not used |

## ROM loading and relocation at boot

As built at milestone 3 (`port/dsi/src/rom.c`, `sha1.c`; the CRC is `port/src/hw_crc.c` since milestone 4):

1. **Find the ROM.** `sd:/tmc/baserom.gba` (the DSi SD slot), then
   `fat:/tmc/baserom.gba` (DLDI: flashcards and melonDS tests). It must be
   16,777,216 bytes. A config file naming another path can come later.
2. **Read and hash it in one pass.** 128 KB `fread`s, unbuffered: the first
   14,581,156 bytes land straight in the copy, the rest in a scratch buffer, and
   every chunk goes through SHA-1. The digest must equal the USA ROM's
   `b4bd50e4131b027c334547b4524e2dbbd4227130`, the value `port/README.md` already
   uses. On a mismatch the log shows the digest found and the one expected, and
   the program stops.
   - The DSi has no SHA-1 hardware. Its BIOS has SHA-1 (`swiSHA1*`, SWI 0x24-0x27)
     in software, and in melonDS 1.1 `swiSHA1Update` jumps to 0x02FFC00C (the
     header area) and faults. `sha1.c` is a plain C SHA-1, ARM code in ITCM,
     with the rounds unrolled by five.
3. **Copy layout.** `__rom_copy` is a fixed address, 4 KB-aligned, ending below
   0x02FF4000 (`rom_symbols.ld`): 14,239 KB at 0x0220C000 (option A, see RAM
   budget). `main.c` shortens the libnds heap to end there (`reduceHeapSize`).
   The loader puts the DSi-only `.twl` section (the ARM9i binary, 872 bytes) at
   0x02400000, inside the copy, and crt0 moves it to its place after `.bss`
   before `main`, so the ROM read can overwrite it.
4. **Game code addresses ROM symbols** through `rom_symbols.ld`. Each global
   symbol defined in a data object becomes an absolute symbol at `__rom_copy` plus
   its GBA offset, so `extern const u8 gFoo[]` in the C resolves into the copy with
   no source changes. C-defined tables (`src/**` rodata) are compiled into the
   binary as usual.
5. **Relocate**, checking each word before it changes:
   - "add base" entries: the word must be a ROM address inside the copy, then
     `*(u32*)(copy + off) += (u32)__rom_copy - 0x08000000`;
   - symbol entries: the word must be a ROM, IWRAM or GBA hardware address, then
     `*(u32*)(copy + off) = value`, where the linker has already put the ARM9
     address in `value`. A Thumb function gets bit 0 set by the linker because
     the reference is to an `STT_FUNC` symbol, so a function compiled `-marm`
     later stays correct;
   - kept entries (pointers and raw addresses into EWRAM, which keeps its GBA addresses): the word
     must be in 0x02000000-0x0203FFFF; nothing is written;
   - then the data cache is flushed over the copy.
6. **Self-check.** The counts per kind must equal the generator's (11,547
   add-base, 3,469 symbol, 1,442 kept since milestone 5) and no word may be unexpected. Then the
   log shows the CRC32 of the whole relocated copy. `port/dsi/tools/rom_crc.py`
   computes the same CRC on the host from the ROM and the lists in the built
   ELF.
7. **Stray ROM pointers fault.** On the DSi the GBA-slot region reads back 0xFF
   without faulting, but BlocksDS's MPU setup in DSi mode leaves
   0x08000000-0x0AFFFFFF out of every region, so an access there is already a
   data abort. `DsiProtectGbaSlot` checks this (and switches off any region
   that covers the slot) and installs an exception handler that logs PC, LR, SP
   and r0-r3. Verified with a test build (`DSI_DEFINES=-DDSI_TEST_STRAY_READ`):
   the read of 0x08000000 in `main` aborts and the log shows r3 = 0x08000000.

The MPU regions found (melonDS, DSi mode): 0 I/O 0x04000000 64 MB, 1 BIOS
0xFFFF0000 64 KB, 2 0x00000000 4 KB, 3 shared WRAM 0x03000000 8 MB, 4 ITCM
0x01000000 32 KB, 5 0x0C000000 16 MB (main RAM, uncached mirror), 6 main RAM
0x02000000 16 MB, 7 DTCM 0x02FF4000 16 KB.

## Fixed GBA RAM addresses (EWRAM 0x02xxxxxx, IWRAM 0x03xxxxxx)

The GBA build places every RAM variable at a fixed offset in `linker.ld`: 89 EWRAM
and 94 IWRAM globals, up to 0x02038560 and 0x03007FFA. On top of that, 12 code
sites use literal addresses (`src/fade.c` 0x03005E98, `src/demo.c`,
`figurineMenu.c`, `phonograph.c` and others), already routed through `GBA_PTR` or
`#ifdef`s by the Mac port, and 1,425 data words point into EWRAM.

The Mac port needs `HwGbaPtr` to map GBA addresses to wherever datagen put each
variable on a 64-bit host, by symbol. On the DSi the layout is the GBA's, so the
mapping is an offset at most.

**Preferred: keep EWRAM at its GBA address.**

- Link the ARM9 binary at 0x02040000 in place of 0x02000000; the header's ARM9 RAM
  address follows the ELF.
- Make 0x02000000-0x0203FFFF a NOLOAD section holding `linker.ld`'s EWRAM layout,
  unchanged.
- Then literal EWRAM addresses, the 1,425 data pointers and `GBA_PTR` on EWRAM all
  need no translation. There is also no ambiguity between "a GBA EWRAM address in
  the data" and "a DSi pointer into the binary". Without this, both fall in
  0x0200xxxx, which `HwGbaPtr` cannot tell apart.
- This has to be checked: loaders (TWiLight Menu++, Unlaunch, melonDS direct boot)
  must honour an ARM9 load address other than 0x02000000, and BlocksDS's crt0 and
  its `.secure` section must work there. That is milestone 1's first check.

IWRAM is 32 KB:

- **Option 1:** map the DS shared WRAM (32 KB at 0x03000000) entirely to the ARM9
  (`WRAMCNT`) and keep IWRAM at its GBA address. This needs the ARM7 core to live
  in its own 64 KB WRAM, and shared WRAM is uncached on the ARM9. Note: BlocksDS's
  ARM7 cores put their DSi-only `.twl` code in NWRAM mapped at 0x03000000
  (`ds_arm7.ld`, `twl_iwram`), so this option would also need that moved.
- **Option 2:** put IWRAM as a block in main RAM (cached) and have
  `GBA_PTR`/`HwGbaPtr` add the offset for 0x03xxxxxx. No data word points into
  IWRAM, and the BIOS-reserved words (0x3007FF0/8/C) are already the named
  `gHw*` variables, so the cost is the literal in `src/fade.c` and the
  `ram_*`/`gUnk_03*` symbols, which the linker handles.

Option 2 is the default (simpler, cached); option 1 is a measured alternative.

`HwGbaPtr` on the DSi:

- 0x02 and 0x03: EWRAM (identity) or the IWRAM block;
- 0x04: `gHwIo`; 0x05/0x06/0x07: the palette, VRAM and OAM shadows;
- 0x08/0x09: `__rom_copy`;
- anything else: abort with a message.

Address-range tests in the game (for example "is this in ROM", `ptr >= 0x08000000`)
were made helper-based by the Mac port; they are audited again with the DSi's
ranges.

## Hardware layer

The game keeps writing the GBA shadow buffers (`gHwIo`, `gHwVram`, `gHwPltt`,
`gHwOam`, in main RAM, GBA layout) through the existing `port/hw.h` redirection.
A presenter turns them into DS engine A state at VBlank.

This keeps game behaviour independent of DS register quirks. It also makes the
shadows directly comparable with the Mac headless run, which is the basis for
verification. Direct mapping, with the game writing to real VRAM, is a later
optimisation once hashes match.

**VRAM** (engine A):

- Bank A (128 KB) as `VRAM_A_MAIN_BG` at 0x06000000: GBA BG VRAM 0x06000000-0x0600FFFF.
  The BGxCNT char base (16 KB units) and screen base (2 KB units) fields are the same
  as on the GBA, and the DISPCNT char/screen base offsets stay 0.
- Bank B (128 KB) as `VRAM_B_MAIN_SPRITE` at 0x06400000: GBA OBJ VRAM 0x06010000-0x06017FFF,
  tile *n* at +32*n*. DS 1D OBJ mapping with a 32-byte boundary has the same
  numbering.
- Each VBlank: `DC_FlushAll`, then DMA palette (1 KB), OAM (1 KB) and VRAM (64 KB
  and 32 KB). Measured at milestone 5 in melonDS: the whole present takes 1.6 ms
  and ends by line 217, inside the VBlank (lines 192-262). Later, dirty ranges
  from `HwDmaSet`, LZ77 and CpuSet destinations cut this down, or the game writes
  VRAM and palette directly (the layouts are the same).
- Banks C and D stay free (bottom screen and extended view later); banks E-I are
  unused.

**Display registers**: translated from `gHwIo` once per frame.

- DISPCNT: forced blank bit 7 and layer/window enables bits 8-15 are the same.
  GBA bit 6 (OBJ 1D) becomes DS bit 4 with boundary bits 20-21 = 0. Display mode
  bits 16-17 = 1.
- BG modes differ: DS mode 1 has BG3 affine, GBA mode 1 has BG2. GBA mode 1
  (BG0-1 text, BG2 affine) becomes DS mode 2 with BG3 off; GBA mode 2 is DS mode
  2. The bitmap modes 3-5 are not translated (the game has none; counted).
- BGxCNT, WININ/WINOUT, MOSAIC, BLDCNT/BLDALPHA/BLDY: same offsets on engine A;
  copied with DS-only bits cleared.
- Scroll, affine reference points and window rectangles get the screen offset (see
  Screen).

**Interrupts**: the game's interrupts are played out by `frame.c` on the
shadows, as on the Mac (milestone 4), not taken from the DS. The frame, in the
game's `VBlankIntrWait`:

1. `HwRunLines`: the game's 160 lines, their HBlank/VCount handlers and HBlank DMA.
2. `DsiPresentPrepare`: the DS rows and tables for that frame.
3. `swiWaitForVBlank`, then `DsiPresent`.
4. `HwRunVBlank`: the game's VBlank DMA and handler.

The DS VBlank IRQ only re-arms the shown frame's line 0 and HBlank DMA
(`DsiPresentVBlank`): when a game frame takes two DS frames the picture is shown
twice, and DMA in repeat mode would otherwise run past its table.

**DMA**:

- `HwDmaSet` keeps the Mac semantics: immediate transfers are CPU copies into the
  shadows (cache-friendly, no coherency issue).
- VBlank transfers are latched and run at VBlank.
- **HBlank DMA** (per-line effects: scroll waves, window iris for transitions) is
  rebuilt each frame from the line capture: one row of DS registers per DS line.
  Lines 0-15 and 176-191 are border rows; lines 16-175 are the game's 160 lines,
  translated (scroll, affine reference points, windows).
- Only what changes between lines is played: DS DMA 1 writes DISPCNT, DMA 2 the
  range of register words that change (up to 2 words in the first 2,048 frames),
  both in HBlank start mode with repeat and destination reload. The tables are
  double-buffered.
- Affine reference points: the DS steps them only on lines where the BG is drawn,
  so when an affine BG is on its reference words are in every row.

**Timers**:

- The game touches `REG_TM*` in one file. DS ARM9 timers count at 33.51 MHz
  against the GBA's 16.78 MHz, so prescalers or reload values are scaled when a
  timer has to tick in real time.
- For the proof of concept, timers stay shadow-only, as on the Mac. The m4a sound
  timer is not needed without sound.

**Input**:

- `REG_KEYINPUT` in the shadow is `~keysHeld() & 0x3FF` once per frame. The low 10
  bits (A, B, Select, Start, directions, R, L) are the same on DS.
- X and Y come from the ARM7 through libnds IPC (`KEY_X`, `KEY_Y`, bits 10-11 of
  `keysHeld()`), free for later bindings.
- Scripted input for tests comes from the input-script file (not yet).
- As built (milestone 5): `scanKeys()` and `keysHeld() & 0x3FF` into `HwSetKeys` at
  the start of each frame. A `DSI_CHECKS=1` build keeps all keys up for the
  1,024 recorded frames, so they stay comparable with the Mac.

**Saves**:

- `port/src/eeprom.c` with its backing file at `sd:/tmc/tmc.sav` (8 KB).
- Loaded at boot; on each completed EEPROM write the 8 bytes are written and
  `fflush`ed (or the file is rewritten at the end of the game's save sequence).
- melonDS can only test this through DLDI (`fat:/tmc/`, see README); `sd:/` writes
  are checked on the real DSi.

## Sound on the ARM7 (after the proof of concept)

- The m4a sequencer (Mac `port/src/sound/m4a_engine.c`) runs on the ARM9 each
  frame. It sends per-voice state (sample address, loop, frequency, volume, pan,
  start/stop) to the ARM7 through a small shared ring buffer.
- The ARM7 programs the DS's 16 hardware channels:
  - DirectSound voices go to PCM8 channels. GBA samples are signed 8-bit like DS
    PCM8, so they play straight from the ROM copy in main RAM with no conversion.
    Envelopes are stepped at 60 Hz as m4a does.
  - The PSG square channels go to DS PSG channels 8-13, noise to 14-15, and the
    wave channel to a looped 32-sample PCM channel.
- This leaves the ARM7 almost idle: no software mixing, which its 33 MHz and 96 KB
  couldn't afford.
- Song pointers are raw GBA addresses (see Measurements), so the sequencer
  translates 0x08xxxxxx when it reads headers and tracks.
- Samples stay in RAM (option A fits).

## Screen: 256x192, centred

- The 240x160 picture sits at (8, 16).
- The presenter subtracts 8/16 from BGxHOFS/VOFS. For affine BGs each line's row
  gets the game's reference point for that line (followed as `ppu.c` does) minus
  8·PA (X) or 8·PC (Y). It adds 8/16 to OAM positions, wrapping x at 512 and y
  at 256.
- Sprites the GBA does not draw (mode 3, shape 3, the disable bit) are hidden.
  Sprites that wrap at y 256 differently on the two screens are counted (none so
  far).
- The border (as built, milestone 5; it holds in melonDS):
  - Border lines (0-15, 176-191) have every layer and window off.
  - Picture lines have a wrapped WIN0 (x 248..7) that shows no layers over the
    side columns. The game's window moves to WIN1. Both show the backdrop, with
    the game's colour effect.
  - Two game windows on one line leave no window for the side columns (counted;
    none so far). The DS's windows rule out a wrapped game window together with
    the OBJ window (also counted, none).

## Proof-of-concept milestones

Every milestone is checked by numbers: GDB reads through `melonds_run.py`, files on
the DLDI image, and on hardware a boot log written to `sd:/tmc/boot.txt`. The
reference is the Mac headless build at the same game frame.

| # | Milestone | Verified by |
|---|---|---|
| 0 | **Done**: toolchain, DSi hello world (16,128 KB malloc-able, SD and DLDI), measurements, melonDS automation | README figures |
| 1 | **Load address and macro split.** (a) Hello world linked at 0x02040000 boots in melonDS DSi mode and on the DSi from TWiLight Menu++ and Unlaunch. (b) `PORT_HOSTED`/`PORT_LAYOUT_64` split. Status: (a) `make -C port/dsi/hello GBA_EWRAM=1` (`port/dsi/ld/`) boots in melonDS DSi direct boot and 0x02000000-0x0203FFFF tests clean (65,536/65,536 words, cached and uncached); hardware loaders still to do. (b) **done** (see Defines). | (a) `gHelloDone`/`hello.txt` from each loader. (b) `tmc.gba: OK`; Mac objects and binaries byte-identical before/after (replaces the frame-hash and room-smoke runs); `layout_diff.py` clean |
| 2 | **Link.** Done: `make -C port/dsi -j4` links `build/dsi/tmc_dsi.nds` (974 KB) with no undefined symbols: all game C, `port/src/{bios,interrupts,eeprom,data_glue}.c`, `port/src/asm`, the sound engine, the generated `rom_symbols.ld`/`ram_layout.ld`/`rom_relocs.s`, a stub presenter. In melonDS DSi mode it boots, logs its layout and stops before ROM loading | no undefined symbols; figures in [Measurements](#arm9-code-size) and [Build structure](#where-it-lives); 16,545 ROM symbols and 16,089 relocations generated; boot log read through GDB |
| 3 | **ROM load and relocation.** Done: boot reads `sd:/tmc/baserom.gba` (or `fat:/`), checks SHA-1, relocates with a check of every word, logs the copy's CRC32, checks the GBA slot faults, stops ([README](README.md#the-port)) | melonDS: 16,777,216 bytes, SD read 931 ms + SHA-1 4,678 ms (DLDI read 3,758 ms); SHA-1 `b4bd50e4…`; 11,547 add-base, 3,117 symbol, 1,425 kept, 0 unexpected, as generated; copy CRC32 equals `tools/rom_crc.py`'s; a one-byte-changed ROM shows its SHA-1 and stops; a stray read of 0x08000000 aborts into the logging handler |
| 4 | **Game runs blind.** Done: the game runs from `AgbMain`, a DS VBlank drives each game frame through `frame.c` into the shadows, nothing on screen. No input yet (all keys up). The first 1,024 frames record `HwShadowCrc` (`port/src/hw_crc.c`: display registers, palette, VRAM, OAM) and the task/state/substate/fade (`HeadlessGetState`) in `gDsiFrames`; the Mac headless runner writes the same with `TMC_HW_HASH` | melonDS: frames 0-1023 equal the Mac's on every frame, CRC and task (233 distinct CRCs; the title screen is fully faded in from frame 336; frame 1023 is task 0 state 1, CRC `3d91f6b1`), `port/dsi/scripts/compare_frames.py`. Frame time: game 14 µs min / 219 avg / 13,400 max, frame work 3.1 / 3.3 / 3.7 ms, shadow CRC 9.8 ms; 1,025 DS VBlanks for 1,024 game frames. The Mac's audio and hashes are byte-identical after the shared-file changes |
| 5 | **Picture.** Done: the presenter (`port/dsi/src/present.c`) shows the shadows on the top screen: banks A/B, DISPCNT/BG translation, OAM, palette, per-line HBlank DMA, centring with a border. The check is stronger than planned: the DS's own display capture (bank D) of every even frame 0-1022, against the Mac's picture of that frame | melonDS: the 512 captured 240x160 pictures equal the Mac's (`TMC_FRAME_HASH` with `TMC_PPU_DS_EFFECTS=1`; with GBA rounding 400 differ by one step in one channel), the border is clean on all of them; every 64th frame is shown three times and still matches; frames 0-1023 still equal the Mac's shadows. Present 1.6 ms, ended by line 217, 0 after VBlank. Frames 1024-2047 (no checks): game 0.21 ms, frame work 5.7 ms, 1,025 VBlanks; the checks (shadow CRC 9.0 ms, picture check 12.5 ms per 2 frames) are in `DSI_CHECKS=1` builds only, which run frames 0-1023 at about half speed. The default build runs 1,024 frames in 1,026 VBlanks and reads the buttons |
| 6 | **On the DSi.** Same build from TWiLight Menu++ and Unlaunch. Status: the boot log is written to `tmc/boot.txt` next to the ROM, rewritten whole at each stage (ROM found, SHA-1, relocation, copy CRC, game start, each frame-time report, stop), so a hang leaves the log to its last stage. A `DSI_CHECKS=1` build also logs the CRC32 of its 1,024 frame and 512 picture records. Checked in melonDS over DLDI; on the DSi: not yet ([README](README.md#on-the-dsi)) | `sd:/tmc/boot.txt` with SHA-1, load time, copy CRC and frame-time stats, the SHA-1, counts (11,547 add-base, 3,469 symbol, 1,442 kept, 0 unexpected) and CRC as melonDS's (CRC = `rom_crc.py`'s for the build); with the checks, records CRC32 frames `827985eb`, pictures `621b67ef` as melonDS's; **the user confirms the title screen and gameplay**. melonDS (DLDI): `fat:/tmc/boot.txt` equals `gDsiLog`; default build copy CRC `50fc1b78`, 1,024 frames in 1,026 VBlanks |

Milestone 6 is the proof of concept: it boots in DSi mode, loads and relocates the
ROM, and reaches the title screen with graphics and no sound.

## After the proof of concept

In this order, each step approved on its own:

1. **Input and saves:** keys (read since milestone 5) and input scripts get past the title screen, then the
   Mac checkpoint runs; EEPROM goes to `sd:/tmc/tmc.sav`.
2. **Extended view:** fullscreen 256x192 with no border (next section).
3. **ARM7 sound** (section above).
4. **Performance work**, as milestone 4's numbers demand. It may move earlier if
   the game is too slow to play.
5. **Bottom screen:** HUD and map on engine B.

### Extended view: fullscreen without a border

The DS can't scale its 2D output, and 240x160 to 256x192 isn't a whole-number scale
(1.07x across, 1.2x down). So "fullscreen" means the game draws **more of the world**:
256x192 of it, with sharp pixels.

**Approach:**

- The presenter's screen offset becomes (0, 0) and the border window goes away.
- The game's view size becomes a pair of constants (`PORT_SCREEN_W/H`, 240x160 on
  GBA and Mac, 256x192 on DSi extended).
- Every place the game assumes 240x160 is moved onto those constants:
  - camera clamping at room edges;
  - on-screen and off-screen checks for entities, so things don't pop in at the new
    edges;
  - tile streaming of the BG maps. The GBA map buffers are 256x256 pixels, so 16
    more columns and 32 more rows of tiles have to be valid.
  - sprite culling;
  - screen-space effects (iris and fade windows, HBlank tables of 160 lines become
    192).
- Menus, the HUD, text boxes and cutscenes keep their 240x160 layout, centred, or
  are re-anchored to the screen edges one by one (the HUD to the corners, for
  example).
- Rooms smaller than 256x192 are centred with backdrop around them.

**Finding the sites:**

- grep for 240/160/`DISPLAY_WIDTH`-style constants and `0xF0`/`0xA0`.
- The Mac build can run with a 256x192 software PPU: `port/src/ppu.c` with a larger
  frame and the same constants, as a quick visual and numeric check on the Mac.
  The room smoke test (576 rooms) then shows any crash or edge glitch per room.

**Verification:**

- The Mac headless run in extended mode against the DSi at the same frame
  (shadow-buffer hashes, as in milestone 4).
- Room smoke with extended view on.
- Visuals on the DSi checked by the user.
- The bordered mode stays as an option, a setting at boot.

**Scaling instead**, the other route: capture each frame and show it stretched
through the 3D engine or a scaled bitmap layer. It isn't planned. TMC uses all four
BG layers, so nothing is free to show the picture without trading frames. It adds a
frame of lag and needs two VRAM banks, and a 1.2x vertical stretch repeats every
fifth line. It may come back as an experiment later.

## Risks and unknowns

1. **Speed.** 632 KB of Thumb code runs from main RAM through an 8 KB instruction
   cache and 4 KB data cache, at 133 MHz against the GBA's 16.78 MHz. The extra
   clock should cover the cache misses, but that isn't proven.
   - Milestone 4 measures frame time first.
   - Fixes, in order: DSi 133 MHz mode checked on (`SCFG_CLK`); ITCM (32 KB) for
     the hottest functions (the GBA's IWRAM `ram_*` routines such as entity update
     and draw, plus collision); `-marm` for hot loops; `-O2` against `-Os`; DTCM
     for hot data.
2. **ARM9 load address other than 0x02000000.** It may not be honoured by every
   loader, or by BlocksDS's crt0. The fallback is EWRAM as a moved block plus
   offset mapping in `HwGbaPtr`, with a check that game code never passes a native
   binary pointer to `GBA_PTR`.
3. **melonDS DSi SD writes hang** (README). Saves and logs are tested through DLDI
   in melonDS, and `sd:/` writes on hardware only. Since the boot log is a file,
   the port stops at its first log write when melonDS has the ROM on the SD slot;
   melonDS runs put the ROM on DLDI with the SD slot off.
4. **The macro split** touches 106 files shared with the Mac build. Hash and
   room-smoke checks guard it; there is a risk of missing a 64-bit layout site,
   which the static asserts and the layout audit should catch.
5. **Unrelocated pointers in data.** Songs are known. Other blobs with embedded
   absolute addresses may exist beyond the sampled coincidences. The MPU guard on
   0x08-0x0A makes any such access fault loudly; the Mac port's
   `TMC_ROM_WRITES`/datagen hints list the known odd fields.
6. **NULL dereferences.** The GBA reads BIOS bytes silently; BlocksDS's MPU makes
   NULL fault. The Mac port fixed those it found (`PLATFORM_NATIVE` guards); more
   may appear. The exception handler logs PC and address.
7. **GCC versus clang code generation.** Code that is UB-sensitive under
   `NON_MATCHING` may behave differently from the Mac build. Milestone 4's hash
   comparison finds where.
8. **RAM headroom with the full ROM** (option A) is ~0.4 MB. Option B and NWRAM
   are the fallbacks.
9. **Border fidelity** when the game uses windows (side columns), and HBlank
   effects that rewrite registers mid-line.
10. **Boot time** of reading and hashing 16 MB from SD, unmeasured. The boot log
    reports it; reading only the needed ranges would save 2.2 MB of padding.
11. **Mid-frame register writes by the game** outside HBlank DMA (VCount IRQ
    tricks) need their own handling in the presenter if they appear.

## What the user supplies

- Their own USA `baserom.gba` on the SD card (`tmc/baserom.gba`). The build itself
  also needs it locally, as now.
- DSi BIOS, firmware and NAND dumps for melonDS DSi mode (already in
  `~/dsi/support/`).
- Tests on the real DSi from TWiLight Menu++ and Unlaunch, and the visual
  check of milestones 1 and 6.
