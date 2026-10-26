# DSi port: plan for a proof of concept

Goal: a homebrew `.nds` that runs in DSi mode (TWiLight Menu++ or Unlaunch), reads
the player's own `baserom.gba` from the SD card, and runs the decompiled game
natively on the ARM9. This replaces GBARunner2 and its black borders. The proof of
concept ends at **"boots in DSi mode, loads and relocates the ROM, reaches the title
screen with graphics, no sound"**.

Status: plan only, awaiting approval. The toolchain and test setup are in
[README.md](README.md).

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
- If the binary grows past the estimate, option B (packing out the 0.83 MB of code
  holes) buys 0.8 MB. ROM-to-ROM fix-ups then become run-table lookups at boot.

## Build structure

### Where it lives

- `port/dsi/Makefile`: its own makefile, using BlocksDS's flags and specs (`ds_arm9.specs`) and its
  default ARM7 core. Outputs go to `build/dsi/` (`tmc_dsi.elf`, `tmc_dsi.nds`).
  Like the Mac build, it needs the GBA build first (`make -j4`).
- `port/dsi/src/`: DSi-only code: `main.c`, the ROM loader and relocation, the
  presenter (shadows to the 2D engine), interrupts, input, the save file and the
  boot log.
- `port/dsi/tools/gen_link.py`: generates three files into `build/dsi/gen/` from
  `build/USA/tmc.elf`, `tmc.map` and the `-q` relink:
  - `rom_symbols.ld`: `Sym = __rom_copy + 0x00123456;` for the 16,356 global
    symbols in data objects;
  - `ram_layout.ld`: EWRAM and IWRAM, from the preprocessed `linker.ld` with new
    origins;
  - `rom_relocs.s`: the relocation list.
- The game C: `src/**/*.c` minus `src/eeprom.c`, the same set as the Mac
  `tmc_game` target, plus `port/src/asm/*.c`.

### Defines

- Game files: `PLATFORM_DSI=1 NON_MATCHING=1 USA ENGLISH REVISION=0`.
- Game file flags: `-mthumb -mcpu=arm946e-s+nofp -O2 -fwrapv -fno-short-enums -ffunction-sections`.
- A few hot files can be compiled `-marm` or placed in ITCM once profiled.

`PLATFORM_NATIVE` currently means three things at once:

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

There are 235 `PLATFORM_NATIVE` sites in 106 files. The plan:

- Add `PORT_HOSTED`, set by both `PLATFORM_NATIVE` and `PLATFORM_DSI`, in one
  header included by `global.h`.
- Move kinds 1 and 2 to it. Leave kind 3 under `PLATFORM_NATIVE`, renamed or
  commented as `PORT_LAYOUT_64`.
- Triage the sites with a script plus reading. `port/scripts/entity_layout_audit.py`
  and the static asserts check the result. Most kind-2 sites are already written
  with `(s32)` casts or `sizeof(void*)` and behave the same on 32-bit.
- This touches shared headers, so it has to keep:
  - `tmc.gba: OK`;
  - the Mac headless frame hashes unchanged (`TMC_FRAME_HASH` over a fixed run,
    before and after);
  - the room smoke test unchanged.

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
| `port/src/sound/m4a_engine.c` | the sequencer runs on the ARM9 with output thrown away in the proof of concept, so game RAM evolves exactly as on the Mac; later it drives the ARM7 |
| `port/src/headless.c` input script (`TMC_INPUT` syntax) and frame-hash output | parser moved to a shared file, read from `sd:/tmc/input.txt`; hashes into a RAM ring buffer |
| `port/src/ppu.c` (software PPU) | not used at run time; host-side reference for the presenter's translation |
| `port/src/frame.c` (per-line timing) | the logic of when HBlank DMA and IRQs run matches the hardware; real IRQs are used instead |
| datagen, `romdata.c`, `state.c` (save states), `heap.c` type map, `src/apple`/`macos`/`ios` | not used |

## ROM loading and relocation at boot

1. **Find the ROM.** The default is `sd:/tmc/baserom.gba`, with a fallback to
   `fat:/tmc/` for DLDI and melonDS tests. A config file may name another path.
2. **Hash it** with SHA-1 over the whole 16,777,216-byte file, streamed. It must
   equal the USA ROM's `b4bd50e4131b027c334547b4524e2dbbd4227130`, the value
   `port/README.md` already uses. libnds has the DSi's SHA-1 hardware
   (`nds/sha1.h`). The bytes the game needs land straight in the copy while the
   hash runs: one pass, and the padding is hashed and dropped. On a mismatch the
   program shows the hash it found and stops.
3. **Copy layout.** `__rom_copy` is a NOLOAD section near the top of main RAM:
   10,227 KB in the proof of concept, 14,239 KB for the full game.
4. **Game code addresses ROM symbols** through `rom_symbols.ld`. Each global
   symbol defined in a data object becomes an absolute symbol at `__rom_copy` plus
   its GBA offset, so `extern const u8 gFoo[]` in the C resolves into the copy with
   no source changes. C-defined tables (`src/**` rodata) are compiled into the
   binary as usual.
5. **Relocate.**
   - For "add base" entries: `*(u32*)(copy + off) += (u32)__rom_copy - 0x08000000`.
   - For symbol entries: `*(u32*)(copy + off) = value`, where the linker has already
     put the ARM9 address in `value`. A Thumb function gets bit 0 set by the linker
     because the reference is to an `STT_FUNC` symbol, so a function compiled `-marm`
     later stays correct.
   - Then flush the data cache over the copy.
6. **Self-check.** Count the entries applied of each kind and compare with the
   counts the generator recorded.
7. **Catch stray ROM pointers.** Set the MPU to make 0x08000000-0x0AFFFFFF
   inaccessible. On the DSi the GBA-slot region reads back 0xFF without faulting, so
   without this an unrelocated pointer would fail silently. The BlocksDS exception
   handler shows PC and address, and the port also logs them.

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
  numbering. Modes 0-2 only: TMC uses mode 0 and mode 1 (title screen,
  rolling-barrel manager).
- Each VBlank: `DC_FlushRange` the shadow, then DMA 64 KB and 32 KB. About 24 K
  words, well inside the DS VBlank of 71 lines (~4.5 ms). Later, dirty ranges from
  `HwDmaSet`, LZ77 and CpuSet destinations cut this down.
- Banks C and D stay free (bottom screen and extended view later); banks E-I are
  unused.

**Display registers**: translated from `gHwIo` once per frame.

- DISPCNT: BG mode bits 0-2, forced blank bit 7 and layer/window enables bits 8-15
  are the same. GBA bit 6 (OBJ 1D) becomes DS bit 4 with boundary bits 20-21 = 0.
  Display mode bits 16-17 = 1.
- BGxCNT, WININ/WINOUT, MOSAIC, BLDCNT/BLDALPHA/BLDY: same offsets on engine A;
  copied with DS-only bits cleared.
- Scroll, affine reference points and window rectangles get the screen offset (see
  Screen).

**Interrupts**:

- A libnds `irqSet(IRQ_VBLANK)` handler does the presenter step, then calls the
  game's dispatch (`port/src/interrupts.c` → `gIntrTable`), as `IntrMain` does on
  the GBA. The main loop's `VBlankIntrWait` becomes `swiWaitForVBlank`.
- HBlank and VCount IRQs map to DS `IRQ_HBLANK`/`IRQ_VCOUNT`, with VCOUNT adjusted
  for the 16-line offset. The DS has 263 lines per frame, 0-191 visible.

**DMA**:

- `HwDmaSet` keeps the Mac semantics: immediate transfers are CPU copies into the
  shadows (cache-friendly, no coherency issue).
- VBlank transfers are latched and run at VBlank.
- **HBlank DMA** (per-line effects: scroll waves, window iris for transitions) is
  rebuilt each VBlank into a 192-entry table. Lines 0-15 and 176-191 get border
  values; lines 16-175 get the game's 160 values, translated (+8 for WINxH, the
  scroll offset for BGxHOFS).
- That table is played by a real DS DMA in HBlank start mode (DMAxCNT bits 27-29 = 2;
  the GBA's are bits 28-29), so the order of the source walk stays the same.

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
- Scripted input for tests comes from the input-script file.
- Not needed for the title screen; it comes in the milestone after.

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
- The presenter subtracts 8/16 from BGxHOFS/VOFS. For affine BGs it adjusts the
  reference point: X −= 8·PA + 16·PB, Y −= 8·PC + 16·PD. It adds 8/16 to OAM
  positions, wrapping x at 512 and y at 256.
- A sprite whose GBA y and height make it invisible on the GBA is moved off the DS
  screen (y = 192) rather than shown in the bottom border.
- The border uses WIN0 = (8,16)-(248,176):
  - When the game's windows are off, WININ shows the game's layers and effects and
    WINOUT shows nothing (backdrop colour, palette 0).
  - When the game uses its own windows, they are shifted, clamped to the box, and
    the border lines (0-15, 176-191) get DISPCNT with all layers off through the
    HBlank table.
  - The 8-pixel side columns may then show WINOUT content: a known gap for the
    proof of concept. A full fix needs a spare layer (the 3D BG0 is taken by the
    game's BG0) or display capture, and goes with the extended-view work.

## Proof-of-concept milestones

Every milestone is checked by numbers: GDB reads through `melonds_run.py`, files on
the DLDI image, and on hardware a boot log written to `sd:/tmc/boot.txt`. The
reference is the Mac headless build at the same game frame.

| # | Milestone | Verified by |
|---|---|---|
| 0 | **Done**: toolchain, DSi hello world (16,128 KB malloc-able, SD and DLDI), measurements, melonDS automation | README figures |
| 1 | **Load address and macro split.** (a) status: `make -C port/dsi/hello GBA_EWRAM=1` (`port/dsi/ld/`) boots in melonDS DSi direct boot, and 0x02000000-0x0203FFFF tests clean (65,536/65,536 words, cached and uncached); TWiLight Menu++ and Unlaunch on hardware still to do. (a) Hello world linked at 0x02040000 boots in melonDS DSi mode and on the DSi from TWiLight Menu++ and Unlaunch. (b) `PORT_HOSTED`/`PORT_LAYOUT_64` split. | (a) `gHelloDone`/`hello.txt` from each loader. (b) `tmc.gba: OK`; Mac `TMC_FRAME_HASH` for 3,000 frames identical before/after; room smoke 576/576 |
| 2 | **Link.** `tmc_dsi.nds` links: all game C, `port/src/{bios,interrupts,eeprom}.c`, `port/src/asm`, generated `rom_symbols.ld`/`ram_layout.ld`/`rom_relocs.s`, stub presenter | no undefined symbols; link-map figures recorded (.text, .rodata, .bss, NOLOAD blocks) against this plan's estimates; 16,356 ROM symbols and 15,849 relocations generated |
| 3 | **ROM load and relocation.** Boot reads `tmc/baserom.gba`, checks SHA-1, relocates, stops | boot log: bytes read, load ms, SHA-1 = `b4bd50e4…`, counts per relocation kind = generator's counts; CRC32 of the relocated copy equals the CRC a host tool computes from the same ROM, base address and list (run locally, nothing shipped); a wrong-ROM run shows the mismatch |
| 4 | **Game runs blind.** VBlank IRQ drives game frames into the shadows, nothing on screen | per frame, CRC32 of `gHwPltt`, `gHwOam`, `gHwVram` and the display registers in `gHwIo`, into a RAM ring buffer; Mac headless writes the same CRCs (a `TMC_HW_HASH` next to `TMC_FRAME_HASH`); identical for frames 1…N, where N is the frame the Mac run shows the title screen fully faded in; plus game state values (task/state bytes) at N; frame time (timer ticks) min/avg/max |
| 5 | **Picture.** Presenter: banks A/B, DISPCNT/BG translation, OAM, palette, HBlank table, centring | GDB reads back DS palette RAM, OAM and banks A/B at frame N; their CRCs equal a host tool's translation of the Mac shadows; the per-frame presenter time stays inside VBlank |
| 6 | **On the DSi.** Same build from TWiLight Menu++ and Unlaunch | `sd:/tmc/boot.txt` with SHA-1, load time, the frame-N CRCs (must equal melonDS's) and frame-time stats; **the user confirms the title screen visually** |

Milestone 6 is the proof of concept: it boots in DSi mode, loads and relocates the
ROM, and reaches the title screen with graphics and no sound.

## After the proof of concept

In this order, each step approved on its own:

1. **Input and saves:** keys and input scripts get past the title screen, then the
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
   in melonDS, and `sd:/` writes on hardware only.
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
