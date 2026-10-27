# Native hazards: patterns that work on the GBA and can crash or misbehave natively

Static audit of `src/` and `port/src/` against the seven patterns in the task, with a
UBSan run as runtime evidence. Nothing here is fixed; each hit has a suggested fix.
Branch base: `macos` at 29154401. Line numbers refer to that tree.

**Status:** every hit below is fixed except the ones listed as left alone. H1, H2, M1-M5 are in the first fix commit and
the Low items in the second, all under `#ifdef PLATFORM_NATIVE` (the preprocessed GBA source of each touched game file is
unchanged) except `-fwrapv` in `port/CMakeLists.txt`. The playthrough trace (new game to frame 24000, five checkpoints x 3000
frames) is identical to before the fixes. None of the new guards fires in those runs.

Left alone on purpose:
- `demo.c:178`: demo-only pointer arithmetic that is deliberate.
- The bitfield and `s8` constant-truncation warnings (takkuri.c:162, npc4E.c:56, floatingBlock.c:13, mazaalBossObject.c:145,
  player.c:354/827/1588/1741): the C truncation is the same on the GBA, so behaviour already matches.
- `lakituCloud.c:85`: only copies the property pointer.
- `movement.c` gSineTable[64], `gleerok.c:385`, `vaatiArm.c:1388` from `crash-patterns.md`: no longer reported by `-Warray-bounds`.
- Extra finds fixed with the Low items: four functions that fall off the end without a value (`sub_080A7CFC`, `sub_080A4418`,
  `AdvanceIntroSequence`, `sub_08059F9C`; callers ignore the value) and three unsequenced expressions (armos.c:431, mazaalHead.c:811,
  townsperson.c:92). For mazaalHead the ROM disassembly confirms ptr[0] then ptr[1], which is what the native fix does.

How the evidence was gathered (all reproducible):

- `clang -fsyntax-only` over every game source with the native flags
  (`build/native/CMakeFiles/tmc_game.dir/flags.make`) plus `-Warray-bounds
  -Wsometimes-uninitialized -Wuninitialized -Wint-to-pointer-cast -Wpointer-to-int-cast
  -Wshorten-64-to-32 -Wreturn-type -Wshift-count-overflow`.
- A UBSan native build: `cmake -S port -B build/native-ubsan -DCMAKE_C_FLAGS="-fsanitize=bounds,local-bounds,shift,signed-integer-overflow,null,pointer-overflow,nonnull-attribute,returns-nonnull-attribute,return,unreachable,vla-bound,float-cast-overflow,integer-divide-by-zero -g -fno-omit-frame-pointer"`.
  Run with `TMC_BIN=build/native-ubsan/tmc port/scripts/play.sh port/scripts/play/01_new_game.txt 24000 OUT 6000`
  (new game to Hyrule Field north, 24000 frames) and `play.sh -s <checkpoint> ... 3000` for bedroom,
  outside_house, hyrule_town, town_shield, hyrule_field_north. Findings are in "UBSan results".
  Caveat: the build type is empty (-O0), so this tests what the code does, not what clang would do
  to it at -O2. Coverage is the first hours only; everything late-game is static only.
- struct sizes from `build/native/data/types.json` (`TypeDB.record_layout(key, "gba"|"native")`).
- every function-pointer table in the native Mach-O/ELF scanned for NULL entries; all room entity
  placements in the ROM (4500 entries, 265 distinct kind/id pairs) checked against table sizes.

Ranking: **High** = deterministic crash or wrong behaviour in a state the game can reach;
**Medium** = wrong behaviour on common paths, or crash only with bad data; **Low** = works by
accident, theoretical, or only reachable with data/ids that do not occur in the ROM.

## High

### H1. `ZeldaFollowAction` dereferences an uninitialized `Entity*`
[src/npc/npc5.c:230](../../src/npc/npc5.c) declares `Entity* r5;` (comment `//! @bug: r5 is uninitialized`) and reads
it at [npc5.c:254](../../src/npc/npc5.c) (`r5->direction`), [:260](../../src/npc/npc5.c) and [:265](../../src/npc/npc5.c)
(`r5->x.HALF.HI`, `r5->y.HALF.HI`). On the GBA this reads whatever address the register held (no fault on
GBA memory); natively it is a stale stack word, so a wild-pointer read (crash or garbage walk target).
- Reachable: only in the `FLAG_GOTO_JUMPED` branch, i.e. Zelda following Link after Link jumped
  (late game; I did not identify which sequence). Not reachable from any current checkpoint.
- Repro: needs a late-game save with Zelda following. clang reports it as `-Wsometimes-uninitialized`.
- Fix: initialise `r5` natively. The branch's intent is the player's jump position, so
  `#ifdef PLATFORM_NATIVE r5 = &gPlayerEntity.base; #endif` is the closest match; the other
  uses of `HEAP->playerJumpedX/Y` in the same branch show the position it should walk to.
  The GBA build must stay unchanged (keep the declaration as is under `#else`).

### H2. `OctorokGolden` calls through an uninitialized table index
[src/enemy/octorokGolden.c:27-34](../../src/enemy/octorokGolden.c): `u32 index; if (sub_080012DC(this)) { gUnk_080012C8[index](this); }`.
`gUnk_080012C8` ([port/src/asm/enemy.c:50](../../port/src/asm/enemy.c), `{0, sub_08001214, CreateDrownFx, CreateLavaDrownFx, CreateSwampDrownFx}`)
is indexed by the hazard type that `sub_080012DC` returns; on the GBA `index` happened to be r4 = that result.
Natively `index` is stack garbage, so the call jumps to an arbitrary slot or to entry 0 (NULL), which crashes.
- Reachable: a Golden Octorok standing on a pit/water/lava tile (it is knocked in or pushed there).
  Not hit by the first-hours playthrough or any checkpoint.
- Repro: needs a Golden Octorok over a hazard tile (late-game save, or place one with a test harness);
  `TMC_ENTITY_LIMIT` does not help (not a spawn failure).
- Fix: `u32 index = sub_080012DC(this); if (index) { gUnk_080012C8[index](this); }` (valid for both builds if the
  GBA code generation is allowed to change; otherwise `#ifdef PLATFORM_NATIVE`).

## Medium

### M1. `HasDungeonSmallKey` / `HasDungeonBigKey` read an uninitialized local
[src/gameUtils.c:246-262](../../src/gameUtils.c): `tmp` is only assigned when `AreaHasKeys()`; otherwise `return tmp ? 1 : 0` returns garbage.
These are called every frame by the interaction scan, [src/playerUtils.c:1226-1228](../../src/playerUtils.c)
(`r7 = HasDungeonSmallKey() ? PL_BUSY : 0; if (HasDungeonBigKey()) r7 |= PL_FLAGS2;`), so outside dungeons `r7` is
random: wrong interactable matching (flag bits compared against objects) and the HUD big-key icon
([src/menu/pauseMenu.c:1166](../../src/menu/pauseMenu.c)). Also [src/script.c:738,741](../../src/script.c) and
[src/beanstalkSubtask.c:344](../../src/beanstalkSubtask.c).
- Reachable: every area without keys, i.e. everywhere outside dungeons; the stack word is usually stable so it
  may look fine for long stretches. Static only: not observed.
- Fix: `u32 tmp = 0;` (correct on the GBA too, where the compiler's value was 0 in practice).

### M2. `ManagerUpdate` indexes `gMiscManagerunctions` by `this->id` without a range or NULL check
[src/manager.c:65-68](../../src/manager.c): `gMiscManagerunctions[58]` ([manager.c:4](../../src/manager.c)), entry 0 is NULL; the GBA jumps to 0 for id 0
or reads past the table for id >= 58 without faulting. Natively the first is a NULL call, the second a wild call.
This is the same class as the `ram_UpdateEntities` crash. Every manager placed in ROM room data has id <= 31 (checked on
all 4500 placements), so it needs a bad id from code or data.
- Related: the other kinds are in range for room data (enemy max id 47 of 103 entries, object 79 of 194, NPC 79 of 192,
  projectile 15 of 38) but the tables have no checks either: [src/enemy.c:434](../../src/enemy.c), [src/object.c:207](../../src/object.c),
  [src/npc.c:24-27](../../src/npc.c) (`[1] != NULL` is checked, `[0]` is not).
- Repro: `TMC_ENTITY_LIMIT` does not help; a `LoadRoomEntity` with an id past the end does.
- Fix (native only): `if (this->id >= ARRAY_COUNT(gMiscManagerunctions) || gMiscManagerunctions[this->id] == NULL) { DeleteThisEntity(); }`
  under `#ifdef PLATFORM_NATIVE`, with a log line to find the source of the id.

### M3. `gSurfaceActions` has NULL entries
[src/playerUtils.c:742-790](../../src/playerUtils.c), called at [:2599](../../src/playerUtils.c) with `gPlayerState.floor_type`
(46 entries; NULL at 2, 3, 0x1d, 0x1f). `src/data/mapActTileToSurfaceType.c` never yields these, so they are
unreachable with the shipped map data. Same class as M2; add the same guard if the surface-type table gets
re-derived.

### M4. `SurfaceAction_CloneTile`: `n` uninitialized in `default`
[src/player.c:3470-3495](../../src/player.c) (marked `@bug`). `n` is the bound of the `for (i = 0; i < n; ++i)` loop over `gPlayerClones`, so an item
other than 1, 2, 3, 4, 6 reads past the array. Only those items call it today.
Fix: `u32 n = 0;`.

### M5. `removableDust` builds a 24-byte EntityData over a pointer field
[src/projectile/removableDust.c:154-164](../../src/projectile/removableDust.c): `entityData = (EntityData*)&this->unk_68;`
then `MemCopy(&gUnk_0812A004, entityData, 0x10)` and `LoadRoomEntity(entityData)`.
Natively `unk_68` is an `EntityData*` at 0xb8 (entity 0xc0), so the native EntityData occupies 0xb8..0xd0 inside the 0x100 slot:
it works only because the slot has slack and `GenericEntity` zeroes it, and the 0x10 copy leaves `spritePtr` as zero.
Reachable in normal play (removable dust), no crash seen.
- Fix: a local `EntityData data;` copied with `sizeof` natively (the source `gUnk_0812A004` is typed by datagen if it
  has pointers; if not, copy 0x10 bytes into a zeroed local).

## Low

### L1. Uninitialized locals that are harmless or need an allocation failure
- [src/npc/stockwell.c:81,104-105](../../src/npc/stockwell.c): `itemPrice` is written to `gMessage.rupees` when `shopItemType == 0`; the message does not print it.
- [src/npc/npc5.c:616,663-664](../../src/npc/npc5.c): `bVar4` in `ZeldaDoLostAnim`, a timer value.
- [src/menu/kinstoneMenu.c:634-655](../../src/menu/kinstoneMenu.c): `ret` is uninitialized if `sub_0805F2C8()` returns NULL
  (all `WStruct` slots in use). Line 649's `(u8*)multiVal.HALF_U.HI` is a u16 text id, which `InitToken` accepts below 0x10000.
- Fix: initialise all three to 0.

### L2. Pointers kept in fields that hold something else
- [src/object/lilypadLarge.c:188-189,235,245](../../src/object/lilypadLarge.c) stores `x.WORD`/`y.WORD` into `child`/`parent`
  as `(Entity*)` to detect "did not move". Round-trips on 64-bit, but if anything walks `child`/`parent` of a lilypad
  (cleanup, `DeleteEntity`) it dereferences garbage. No such walk was found.
- [src/script.c](../../src/script.c) `intVariable` is `PtrU32` and holds pointers: every pointer use found ([script.c:1656,1838,1868](../../src/script.c),
  [cuccoMinigame.c:336](../../src/object/cuccoMinigame.c), [castleMaid.c:112](../../src/npc/castleMaid.c)) is full width. The ~30 truncating
  `-Wshorten-64-to-32` sites in script.c, carpenter.c:85, drLeft.c:58, farmers.c:55, forestMinish.c:717, stockwell.c:311, talon.c:147,
  townMinish.c:355, sittingPerson.c:186, repeatedSoundManager.c:45, greatFairy.c:573, roomInit.c:1009, text.c:431/485 narrow a script word,
  an index or a pointer-sized zero/one into a 32-bit field. None truncates a pointer.
- [src/common.c:487](../../src/common.c) `zFree`: `(intptr_t)ptr - (intptr_t)gzHeap` truncated to `u32` is
  fine unless a pointer 4 GB away is passed (then it is looked up in the wrong place and ignored).

### L3. Table reads relying on ROM layout and other out-of-bounds shapes
- [src/common.c:494](../../src/common.c) `(u16*)(gzHeap - 2)` and [src/playerUtils.c:3777](../../src/playerUtils.c) `gArea.filler3[-1]`
  are formally out of bounds (UBSan reports both) but stay inside the object (`gzHeap` table, the bitfield byte before `filler3`).
- Other clang `-Warray-bounds` hits all point at pointer arithmetic into the same object:
  [src/beanstalkSubtask.c:790,892](../../src/beanstalkSubtask.c), [src/common.c:494](../../src/common.c), [src/demo.c:178](../../src/demo.c) (demo only).
- NULL entries in small tables, each indexed by a state that code sets: [src/scroll.c:70](../../src/scroll.c) `gUnk_0811E768`,
  TempleOfDropletsManager `*_Actions[0]`, [src/projectile/dirtParticle.c](../../src/projectile/dirtParticle.c) `DirtParticle_Actions[2]`,
  [src/playerItem/linkEmptyingBottle.c](../../src/playerItem/linkEmptyingBottle.c) `gUnk_08122A20[0]`, [src/playerItem/itemBomb.c](../../src/playerItem/itemBomb.c) `stateFuncs[1]`.
  Index 0 is the "uninitialised" action and each of these is only reached after the init state ran, as on the GBA.
- Open items from `port/notes/crash-patterns.md` that I did not re-check: `movement.c` gSineTable[64], `gleerok.c:385`, `vaatiArm.c:1388`.

### L4. Room-property consumers that dereference a NULL result
`GetCurrentRoomProperty` returns NULL when the room has no such property (it never reads garbage natively;
the GBA read ROM address 0). These use the result without a NULL check, so a room placed without its property data crashes:
[lightRayManager.c:85](../../src/manager/lightRayManager.c), [angryStatueManager.c:39](../../src/manager/angryStatueManager.c),
[minishSizedEntranceManager.c:31](../../src/manager/minishSizedEntranceManager.c), [lavaPlatform.c:237](../../src/object/lavaPlatform.c),
[houseDoorExterior.c:69](../../src/object/houseDoorExterior.c), [lakituCloud.c:85](../../src/enemy/lakituCloud.c) (only copies it).
All ship with their property in the ROM, so this is data-driven. Checked and fine: `LoadRoomEntityList` NULL check, delayedEntityLoad,
destructibleTileObserve, pushableFurniture, enemyInteraction, chestSpawner, fightManager, playerItemUtils:81, postman.
- Fix, if wanted: `if (prop == NULL) { DeleteThisEntity(); }` for each, natively.

### L5. UB clang does not currently exploit
Signed shifts into the sign bit or of negative values (see UBSan results). Compiled as on the GBA at -O0; `-fwrapv`-style
behaviour is what the game assumes. Worth a `-fno-strict-overflow`/`-fwrapv` flag in `port/CMakeLists.txt` before any move to -O2.
Bitfield truncation warnings (takkuri.c:162, npc4E.c:56, floatingBlock.c:13, mazaalBossObject.c:145, player.c:354) and unsequenced
modifications (armos.c:431, mazaalHead.c:811, townsperson.c:92) were not triaged one by one; they compile to what the GBA code did at -O0.

## UBSan results

New game to frame 24000 plus 5 checkpoints x 3000 frames: no crash, no `null`, no `pointer-overflow`, no `vla-bound`, no
`signed-integer-overflow` (arithmetic), no float-cast or divide-by-zero reports. The only reports:

| What | Where | Count |
|---|---|---|
| index out of bounds | [common.c:494](../../src/common.c) (index -2, `gzHeap`) | 2 |
| index out of bounds | [playerUtils.c:3777](../../src/playerUtils.c) (index -1, `filler3`) | 2 |
| left shift of a value into the sign bit, `<< 16` of a u16-sized colour/pos value | vram.c:124,133; main.c:46; interrupts.c:51,84,97,171,228,230; common.c:184,188,214,270,276,860; ui.c:102,317,323,327; entity.c:429; fileselect.c:805; color.c:246,368 | 80+ |
| left shift of a negative value | [physics.c:159-166](../../src/physics.c), [objectUtils.c:273](../../src/objectUtils.c), fade.c:212, fileScreenObjects.c:538,549, `port/src/asm/{code_08001A7C.c:200,206, code_080043E8.c:193, intr.c:759,760}` | ~30 |
| left shift overflow | [script.c:1770](../../src/script.c) (`(direction & 0x80) << 24`), [playerUtils.c:2946](../../src/playerUtils.c) (`<< 0x1e` of 3) | 4 |

## Patterns checked and found clean

1. **Literal byte sizes** (49 Mem*/Dma*/zMalloc sites, listed with GBA vs native sizes): only the structs that differ matter and only
   `gHUD.elements` (already fixed) and M5 hit. [enterPortalSubtask.c:142](../../src/enterPortalSubtask.c) `MemClear(&gUnk_02018EB0, 0x28)` matches the
   native size (GBA 0x1c); [entity.c:259-272](../../src/entity.c) `EraseAllEntities` has a native branch; `LoadResources` ([interrupts.c:157-205](../../src/interrupts.c))
   uses `ResourceRequest` natively; [dust.c:128-165](../../src/enemy/dust.c) packs a pointer-free heap; `zMalloc` budgets are in bytes of pointer-free data.
2. **Function-pointer tables**: only three typed tables have NULL entries (`gMiscManagerunctions[0]`, `gSurfaceActions` four entries, and the
   small action tables in L3). Room-placed ids are in range for every kind (M2).
3. **ROM-layout dependent reads**: the previously reported cases are fixed; the remaining out-of-range shapes are L3.
4. **Pointer/int casts**: ~92 `(u32)` casts reviewed through `-Wpointer-to-int-cast`/`-Wshorten-64-to-32`; none truncates a pointer (L2).
   `GetFuserIdPair` ([code_08001A7C.c](../../port/src/asm/code_08001A7C.c)) returns the text id in the high word; `KinstoneMenu` handles it.
5. **NULL derefs**: `animPtr`, `myHeap` and the Chuchu/Octorok child cases are fixed already; room-property consumers are L4.
6. **UB**: uninitialized locals are H1, H2, M1, M4, L1 (every other `-Wsometimes-uninitialized` hit is the `DeleteThisEntity()` else-branch,
   which is a longjmp and so a false positive). Shifts are L5. No shift count >= 32 was found.
7. **Raw blobs**: `datagen/emit.py` types data by relocation, C declaration or hint. The struct types whose native size differs and that
   are reached through a pointer cast (not an `extern` declaration) are: `gMenu.field_0xc` as `struct_080FCF04`/`struct_080FCFB8`
   ([cutscene.c:632,758](../../src/cutscene.c), a pointer value, fine), `GetCurrentRoomProperty` results as `unk_DoorProperty`,
   `LavaPlatformEntry`, `TileEntity`, `EntityData` (all hinted in `hints.txt` or derived in `entitylists.py`), and `Dialog`
   ([script.c:1868](../../src/script.c), pointer passed through `intVariable`). The risk class is pointer-bearing structs in
   `.incbin` blobs whose pointers are all NULL (no relocations, e.g. the Festival Town doors, the lava platform tables): hints
   were needed for those, and any further instance is invisible to a static pass (no relocation, no C type). `build/native/data/notes.txt` lists 12 notes (8 distinct labels) for C
   declarations typed as integer arrays that hold pointers (gSpriteAnimations_*, gUnk_080B44B8, gUnk_080C8F7C, Area_HyruleTown); they work
   because datagen widens them as pointer tables, but they should be given pointer types in C.

## Suggested order

1. H1, H2: native-only initialisation (small, certain); M1: `u32 tmp = 0` (removes a nondeterminism that exists every frame outside dungeons).
2. M2: guard `ManagerUpdate` (and add a log of the id), because the failure is a silent NULL call.
3. M4, M5, L1: initialise `n`/`ret`/`itemPrice`; replace the removableDust overlay.
4. L4, L5: defensive checks and `-fwrapv` only if the port moves to optimised builds.

## Writes into ROM data

The GBA ignores a store to ROM; natively the game's data is in read-only `__DATA_CONST,__const` / `__TEXT,__const`, so the same
store is a SIGBUS (KERN_PROTECTION_FAILURE). The fix is to skip the store natively (`#ifndef PLATFORM_NATIVE` around it, with a
comment), which is exactly what the hardware does.

| Site | Target (ROM) | Triggered by | Fix |
|---|---|---|---|
| [playerItemGustJar.c](../../src/playerItem/playerItemGustJar.c) `PlayerItemGustJar_Init` | `gUnk_08132B28` (`hitbox->unk2[0..3]`) | using the Gust Jar | c0eb6fbc |
| [spearMoblin.c](../../src/enemy/spearMoblin.c) `sub_08028858` (three of its states call it every frame) and `sub_08028528` (when it spots Link) | `gUnk_080FD258`, the hitbox from `gEnemyDefinition_15` (offset, width, height) | any Spear Moblin, as soon as it updates: Trilby Highlands and Western Woods (Hyrule Field), Lake Hylia, Palace of Winds, Dark Hyrule Castle | writes skipped |

Spear Moblin: on the GBA its hitbox never follows the spear direction table `gUnk_080CC944` (the writes are lost), so every moblin
keeps `gUnk_080FD258` as is. Skipping the writes keeps that. Verified with a Spear Moblin spawned next to Link from
`hyrule_field_north.tmcs` (a scratch hack, not committed): before the fix SIGBUS in `sub_08028858` two frames after the spawn, and
with `TMC_ROM_WRITES=1` both sites are logged; after the fix 600 frames and no ROM write.

**How it was found** (static, all game TUs under the native flags):
1. ROM symbols: every symbol `nm -m` puts in `__DATA_CONST,__const` or `__TEXT,__const` (21k).
2. libclang: every declaration of such a symbol (file scope and block scope) whose type is not const, including pointer tables
   whose pointees are not const (575 declarations in 97 files).
3. Scratch copy of `src/`, `include/`, `port/`: make those declarations const (also the pointer level), then `clang -fsyntax-only
   -Wcast-qual -Wincompatible-pointer-types-discards-qualifiers`. A direct store is an error; every place a ROM address goes into a
   non-const pointer (assignment, initialiser, argument, return value, cast) is a warning.
4. For each warning, libclang finds the destination (local, field, parameter, return type) and that is made const too. Repeat
   until nothing changes (15 rounds), with pointer-to-pointer destinations (`RoomInfo.properties`, `gCurrentRoomProperties`)
   made `T* const*` by hand. Also constified by hand: the pointer fields of every struct type that ROM data contains (from
   `types.json`: `SpritePtr`, `Figurine`, `struct_080D8E54`, the m4a `ToneData`/`WaveData`/`cmdPtr`), the `ColSettings` argument
   of every collision handler, and the results of `GetCurrentRoomProperty`/`GetRoomProperty`.
5. The 74 remaining errors are stores through a pointer that can hold a ROM address. All are `->hitbox` writes plus two others:
   - `entity->hitbox`: enemies and projectiles get it from their definition (ROM), objects and NPCs from `gObjectHitboxes`/
     `gNPCHitboxes` (ROM), and many entities point it at a ROM `Hitbox`. Writes are safe only after `AllocMutableHitbox` (zMalloc).
     Safe: PlayerItemSword, PlayerItemLantern, PlayerItemBottle (empty bottle only, the only branch that writes), MoblinSpear,
     MineralWaterSource, GleerokParticle (type 1 allocates, type 0 has a NULL definition hitbox and checks for NULL), ChuchuBoss
     (types 0-3 allocate, type 4 shares a part's), PlayerClone (writes its sword's, which allocates), Dust (type 0 heap hitbox).
     **Not safe: SpearMoblin** (above) and GustJar (fixed before).
   - `dust.c:245` writes into the type-0 heap hitbox; `kinstoneMenu.c:632` writes `currentObject->kinstoneId` after a fusion,
     when `currentObject` is the fuser's candidate (RAM), not `gNoInteraction` (ROM).

Checked and only read: the room-property consumers (`super->child = GetCurrentRoomProperty()` rails in BladeTrap, Bombarossa,
LightableSwitch, LilypadSmall, LakituCloud via `UpdateRailMovement`), NPC paths set by `sub_0806EE04`, the Flame whose `child`
points at tile data, script pointers (every `scriptInstructionPointer` use), and `zFree(ROM hitbox)` (returns early, the pointer
is outside the heap). No game data is in a writable native section (`__DATA,__data`/`__TMCSTATE,__data` hold only port
variables), so there is no case of a ROM write silently taking effect natively.

Blind spots: a ROM address that reaches a pointer through an integer (script words, `u32` fields) and is then written is
invisible to the const pass; the script commands only read through such pointers.

**Dynamic check:** `TMC_ROM_WRITES=1` (headless runner) logs a store into read-only game data as `rom write: <symbol+off> from
<function+off>, frame N` and skips it (applying the base-register writeback of pre/post-indexed `STR`/`STP`) instead of crashing.
No ROM write in the 24000-frame playthrough, the Ezlo replay or the gust jar repro.

## Unsigned 32-bit "negative" offsets

On the GBA a pointer plus a u32 wraps at 32 bits, so a u32 that holds a negative number (`(u32)n * -3`, `-innerIndex`, `pos - 0x80`
near 0) still lands just before the base. Natively the u32 is zero-extended and lands 4-16 GB away. Fixed before this sweep:
9ed69348 (`UpdateRailMovement`, lily pads), 8689d25b (`sub_0807C69C`, Minish house interiors), b7608064 (Big Goron BG copy),
b22fcf35 (iris window `sub_0801E290`). `-fsanitize=pointer-overflow` cannot see this class: base + 4 GB does not overflow 64 bits.

**Static pass** (libclang over all 658 C files in `src/` and `port/src/` with the native compile commands; scripts in the session
notes folder, `u32-offsets-tools/`):
1. Every array subscript, pointer `+`/`-`/`+=`/`-=` and `(uintptr_t)p + x` whose integer operand has type `unsigned int` after
   the usual conversions (u8/u16 promote to int, so only u32 and unsigned enums qualify): 1535 sites (1375 subscripts, 158 pointer
   adds, 2 uintptr, 13 enum-typed).
2. 223 of them are provably below 0x80000000 (constants, masks, `u32 >> n`, narrow unsigned values).
3. For the rest, every definition of each u32 local, parameter (all call sites) and field the operand reads, three levels deep,
   flagged when it contains a negation, a subtraction, `~`, a decrement, or a conversion from a signed value not provably >= 0.
   That leaves 218 candidate sites in game and port-asm code (106 functions), each read by hand.

**Dynamic pass:** a scratch copy of the tree with every one of those operands (1519; the 13 enum indices left out, since wrapping
them in a `u32` function changes their type) wrapped in `u32chk_(e, id)`, which returns `e` and logs `U32CHK id value` when
`e >= 0x80000000`. The wrapping keeps the game identical: the 24000-frame playthrough's video hashes match the plain build.
Runs: the 24000-frame playthrough, the Ezlo replay, and a warp scan (a scratch `TMC_WARP_SCAN` hook, not committed) that sends
Link to each of the 661 rooms from the `hyrule_town` checkpoint, at full health, for 90 frames with control. **No report in any
run.** The warp scan reached control in 499 rooms; 72 timed out and 91 crashed with other bugs already known from the earlier
warp scan (80 in `sub_0804AF90`, NULL room properties; `LoadRoomTileSet`, `LoadRoomGfx`, `sub_0802D650`, OctorokBoss,
FrozenOctorok, GyorgBossObject; most likely warp artefacts). The gust jar state (`frame_001329`) did not load into any build
whose code layout differs, so it was not run instrumented. (The cause was the version 1 state format guessing pointers from
values: four entities' kind/id/type words, e.g. `0x1005f0506`, looked like pointers into `gGfx_94_0` and were re-pointed, so
the entities' kinds changed and `ram_UpdateEntities` called a NULL update. States now list pointers exactly from the C types,
and old states load through that map; see README.md, save states.) the lily pad state was not found on disk. Both run clean on the fixed build.

**Fixed** (all GBA-faithful: the index wraps backwards as on the GBA instead of jumping 4 GB):

| Site | Value | Reached when | Fix |
|---|---|---|---|
| [veneer.c](../../port/src/asm/veneer.c) `SetCollisionData`, `SetActTileAtTilePos`, `GetTileIndex`, `SetTile`; [intr.c](../../port/src/asm/intr.c) `TileTypeAt` (`GetTileTypeAtTilePos`), `GetCollisionDataAtTilePos` | `tilePos` from callers that pass a neighbour: `tilePos - 0x40`, `- 0x41`, `- 1`, `+ TILE_POS(-1, -1)` (bossDoor, metalDoor, bench, furniture, pushableGrave, pushableFurniture, bakerOven, castorWildsStatue, bigIceBlock, madderpillar, vaatiEyesMacro, ...) | an object on the room's top row (or tile 0) looks at the tile above it. The asm (`lsls r0, #1; ldrh r0, [r1, r0]`) wraps | index `(s32)tilePos` (port-asm files, native only) |
| [vaatiBall.c](../../src/enemy/vaatiBall.c) `sub_08044DEC` | `off = parent->unk_80 - 1`, `unk_80` the u8 count of remaining balls | the Vaati Reborn fight, if a ball takes this path with the count at 0 (`sub_080449F8` sets the parent's state 1 exactly when it is 0); not proven reachable | `gUnk_080D1628[(s32)off]` |
| [beanstalkSubtask.c](../../src/beanstalkSubtask.c) `sub_0801A370` | `collisionData[pos - 0x80]`, two rows above the target tile | pushing a push tile (`SPECIAL_TILE_43`-`45`) sideways in the top two tile rows of a room | `[(s32)pos - 0x80]` |

None of these is on the playthrough's path or in the warp scan (no report), so none is a crash a player is known to hit; they are
the only sites where a "negative" value is possible from the code and data, as opposed to the many below.

**Checked and safe** (main groups):
- Guarded before use: `hazard - 1` (`!= 0`), `objPaletteId - 0x16` (`>= 0x16`), `item - ITEM_BOTTLE1`, `bottle_no - 1`,
  `collision - 0x10` / `data - 0x10` (`>= 0x10`), `tileIndex - 0x4000`, `param_1 - 0x80` (text, `> 0x7f`), Mazaal bracelet
  `direction - 10` (clamped 10..0x16), smith `global_progress - 2` (int compare), file select hearts (capped at 10), name entry
  `idx--` (u32 clamp), the `sub_080012DC` indices of chaser/cuccoAggr/flyingSkull/octorokGolden/ropeGolden/sluggula (`!= 0`).
- Loops counting down that stop at 0: OctorokBoss tails (`tailCount - 1` .. 1), gyorgFemale, `UnregisterInteractTile`, MemCopy.
- Random(): returns `r >> 1`, so `(s32)Random() % 3` (gyorgMale) is never negative.
- InitializeAnimation callers that subtract (`animIndex - 7` in Enemy50/Ghini, `type2 - 1` in Beanstalk) are guarded. The asm
  indexes with the full argument, so a negative one would differ natively; none was found.
- `u32` shifted right (`dx >> 4`) is always below 2^31; where the source was a negative int (scroll before the room origin,
  `horizontalMinishPathBackgroundManager` with a room wider than 0x400) the GBA reads 0.5-1 GB away too, so it is not this class.
- `port/src/` outside `asm/` (PPU, sound, state, hw): native code with range checks; reviewed, nothing.

**Blind spots:**
- Functions declared to return u32 that return a negative value: their result is not followed into callers.
- Values that come from data (`u8`/`s8` table entries added to a u32 position, script operands) are only covered dynamically.
- The game-side tile writers `SetTileType`, `SetTileByIndex`, `RestorePrevTileEntity` (playerUtils.c) index with the caller's u32
  `tilePos` too (bombable walls, graves, ice blocks pass `- 1`/`- 0x40`/`- 0x41`); left as is: a negative value needs one of
  those objects on a room's top row, and no call in the runs above (Init-time calls in 499 rooms included) produced one.
- `sub_0801A570` (big pushed blocks) walks `position - 0x40` up to three rows and indexes `mapData` with it; already partly
  native-fixed for `collisionData` in 2b987919, the `mapData` reads stay u32 (needs a block within three rows of the top).
- Coverage of the dynamic pass is the playthrough (first hours), the Ezlo replay, and 90 idle frames per room: interaction-time
  code (pushing, bombing, boss phases) in late-game rooms is static only.

## Struct punning: Manager fields (and other structs) read through `Entity` members

On the GBA a Manager's own fields start at 0x20, and several objects reach them through their `parent` typed `Entity*`, using
whichever Entity member sits at the same GBA offset (`parent->zVelocity` for a manager's 0x20 field). Natively `Entity` and
`Manager` only agree up to `flags` (GBA 0x10): `Manager.parent`/`child` are 8-byte pointers at native 0x20/0x28 and a manager's
own fields start at 0x38, while `Entity.zVelocity` is at 0x28. So the same expression natively reads or writes the manager's
`child` pointer, a statue pointer, or padding. Fixed before this sweep: 5a2c3adc (cannonball, AngryStatueManager.field_0x20),
41d18989 (Deepwood barrel spiderweb, RollingBarrelManager.unk_20).

**Method** (static, all 618 game TUs, tools in the side-session notes folder `manager-fields-tools/`):
1. Layouts: libclang parses every TU twice, once with the native flags and once as `arm-none-eabi -mcpu=arm7tdmi
   -fno-short-enums` without `PLATFORM_NATIVE` and the port's forced include (the decomp's GBA layout). Every record is flattened
   to leaves (path, bit offset, bit size) in both, keyed by declaration location. Checked against the `/*0x..*/` comments
   (Entity 0x68/0x90, Manager 0x20/0x38).
2. Facts (native parse): every member access with the pointer root it goes through and that root's provenance (casts, locals and
   their assignments, `&x->base`, field loads, calls with their arguments, params), every store of a pointer into a field, every
   call's pointer arguments, every record-pointer cast.
3. Checks. A "mismatch" is an access whose leaf (GBA g, native n) lands, in the record the object really is, on a leaf whose
   native placement of byte g is not n.
   - Managers stored into another struct's field (`object->parent = (Entity*)this`: 9 sites). The stored-into object's ID
     (`CreateObject(ID)`) maps through `gObjectFunctions`/`gEnemyFunctions` to its file; every read of that field there, and
     every function it is passed to (per-param access summaries, transitive), is checked against the manager's layout.
   - Every Manager pointer cast to a non-Manager type and used directly or passed to a function (`SetEntityPriority`,
     `RequestPriorityDuration`, `AppendEntityToList`, `EntityHasDuplicateID`, `UnlinkEntity`; all 30 call sites): only header
     fields, or guarded by `kind != MANAGER`.
   - Code walking `gEntityLists` (casts from `LinkedList`, `->next`/`->prev` chains) that touches fields past 0x14 without a
     `kind` filter: 8 functions, one real (`sub_0805ACC0`), the rest filter on kind or walk lists without managers.
   - General: every access through a cast between two record types, compared against the source type's layout; objects reached
     through several views (the `gMenu` views, `void*` fields such as `myHeap` per file); and entity-to-entity links: a
     `parent`/`child` (or other field) store whose holder and value are resolved to their files (Create calls, `this`), and every
     read of that field through a subtype cast in the holder's file compared with the value's own views of itself (131 links).
     Menu views are consistent with each other; no entity subtype link disagrees.

**Fixed** (all `#ifdef PLATFORM_NATIVE`, the field read by its own name; GBA build `tmc.gba: OK`):

| Site | GBA meaning | Natively hit before the fix | Reached | Effect natively before the fix |
|---|---|---|---|---|
| [houseSign.c](../../src/object/houseSign.c) `HouseSign` | clear the sign's bit in HouseSignManager.bitfield (0x20) via `zVelocity` | low half of the manager's `child` (NULL) | Festival Town / Hyrule Town (`Entities_HyruleTown_0_1`, `_1_1`), every time a house sign scrolls off screen | the bit stays set, so a sign that scrolled off screen never comes back. **In the playthrough**: 7 signs created before, 12 after (5 respawns) |
| [minishSizedEntrance.c](../../src/object/minishSizedEntrance.c) `MinishSizedEntrance_Action1` | test the entrance's bit in MinishSizedEntranceManager.field_0x20 via `zVelocity` | the manager's `child` (normally NULL): bit clear | Hyrule Town (`Entities_HyruleTown_0_1`, manager type 0xe) | the type-1 entrance deletes itself on its first frame and the manager's bit stays set: the Minish-sized entrance never appears, so Minish Link cannot use it |
| [angryStatue.c](../../src/object/angryStatue.c) `AngryStatue_Action3` | set the statue's hit bit in AngryStatueManager.field_0x36 via `z.BYTES.byte2` | byte 6 of `field_0x20[0]`, the first statue's pointer | Dark Hyrule Castle (outside NE/SE, 1F loop left, B1 cannons), when a reflected cannonball hits a statue (`sub_080AB634` sets action 3 for 30 frames) | the puzzle never sees all four bits (never solved), and the first statue pointer becomes non-canonical: the next cannonball collision check (`sub_080AB634`) or the solve (`sub_0805D11C`) dereferences it: crash |
| [itemForSale.c](../../src/object/itemForSale.c) `sub_080819B4` | mark the kinstone sold in GoronMerchantShopManager.itemActive (0x20) | a byte of the manager's `parent` pointer | the Goron merchant's kinstone shop (GoronMerchantShopManager, created by `goronMerchant.c`), buying a kinstone | the purchase is not recorded (`GORON_KAKERA_*` never set), so bought kinstones come back and the stock never advances; the manager's `parent` gets 0xff in one byte |
| [pushableFurniture.c](../../src/object/pushableFurniture.c) `sub_0808FC5C` | set the piece's bit in PushableFurnitureManager.unk_28 via `spriteAnimation[2]` | `Manager.unused2` | only managers with a flag (type2 != 0xff); every USA room has 0xff (house interiors, Cafe, Carpenter, ranch houses) | none with USA data (latent: the "all in place" flag would never be set) |
| [delayedEntityLoadManager.c](../../src/manager/delayedEntityLoadManager.c) `sub_0805ACC0` | DelayedEntityLoadManager.spawnedCount (0x21) as the second byte of `zVelocity` | a byte of `child` (0) | townsperson, cat, dog, cow, guard, kid, castle maid spawned by the loader (53 calls in the playthrough) | always returned 0 (the probe saw `spawned=26`, old byte 0), so NPCs took their current position as home instead of the spawn point from the room data; they are equal at the call, so nothing visible in the runs |
| [gleerok.c](../../src/enemy/gleerok.c) `sub_0802D170`, `sub_0802D218` | `((Entity*)heap)->parent`: Gleerok_HeapStruct.ent2 (0x50) | `entities[4]` (native 0x60; `ent2` is at 0x68) | Gleerok (Cave of Flames boss) | sets the timer of the wrong body segment; `ent2` (the last segment created in the init loop) has its `timer` compared with 12 by `sub_0802DCE0` and others, so the fight's phase handshake goes wrong |

Ambiguous / checked and fine: `itemForSale.c` parent is only ever the Goron shop manager or NULL (other shop items come from room
data without a parent). `vaatiAppearingManager.c` `sub_0805D9D8` reads `super->parent->x`: its parent is the Vaati NPC
(`CreateVaatiApparateManager` is only called from `vaati.c` with the NPC), not a manager. FightManagerHelper reads its parent
as `FightManager*` (right type). FloatingPlatform and LightRay read only `timer`/`subAction` of their manager (header, same
offsets). `cutsceneMiscObject.c` type 31 views itself as `Type1F` (`anim` at 0x84) and never uses the script context there,
so its own view is consistent. `gyorgMale.c` reads `((Entity*)myHeap)->next`: `GyorgHeap` starts with pointers, so `next` is
`female` in both layouts.

**Runtime**: the playthrough hash still ends on `23999 89eb5912`, but frames 15336-15521, 15826-15996 and 17257-22132 (Festival
Town) differ, all from the house sign fix (reverting only that file gives the old hashes exactly; reverting the minish entrance
or the delayed loader fix changes nothing): the signs that scroll back on screen are drawn again. Step timings in the play log
are the same frame for frame. The Ezlo and gust jar states exit 0, the self-test prints `0 failed checks`. The minish entrance,
angry statues, Goron shop, furniture flag and Gleerok sites are not on any recorded path (lldb hit count 0), so they rest on the
static analysis.

**Blind spots**: provenance does not cross globals or struct fields other than the stored-into one (a manager pointer saved in a
global and read elsewhere would be missed; none was seen by grep); objects spawned by `LoadRoomEntity` (room data) are not
resolved to a file (FloatingPlatformManager was checked by hand); entity links whose holder or value is found at run time
(`FindEntity*`, list walks, `gPlayerEntity.carriedEntity`) are not typed; accesses through integer arithmetic on addresses (e.g.
`(u8*)&e->field + k`) count as the field they start from; and bitfields are judged by their containing unit.

## Room smoke test: first-update order and detached parents

Found by warping into every room ([room-smoke.md](room-smoke.md)); all fixed under `#ifdef PLATFORM_NATIVE`.

- **A part updates before the part it points at has initialised.** A multi-part boss creates its parts in one
  init; an earlier-created part updates first and reads a heap pointer a later part's init sets (or a part's
  hitbox before that part's init), which the GBA reads as BIOS. octorokBoss / frozenOctorok legs
  (`heap->mouthObject`), gleerok `sub_0802E7E4` (`heap->entity1`), gyorgBossObject `sub_080A1DCC`
  (`heap->mouth`, `heap->tail` and its segments), chuchuBoss `sub_080279AC` (`hitbox`).
- **A child detached before its first update.** darkNutSwordSlash: the Dark Nut sets `child->parent = NULL`
  (`sub_08021588`), possibly in the frame it creates the slash, whose init reads `parent->type` before checking
  for NULL.
- **An entity's heap used by someone else before the entity's own first update allocates it.** zeldaFollower
  `sub_08068578`, called through `SetZeldaFollowTarget` when the player arrives by stairs with Zelda following.

## Calls missing an argument the callee reads

A function declared without a prototype (`void f();`), or with fewer parameters than its definition, is called
with fewer arguments than it uses. On the GBA the missing one is whatever the caller left in that register,
which happens to be the right value; natively it is garbage. Found from a crash after the Big Green Chuchu's
defeat (the element's background); swept with libclang over every call in the game sources (call argument
count against the definition's parameter count). Fixed under `#ifdef PLATFORM_NATIVE` by passing the value:

- elementsBackground `sub_080A04E8()`: its entity (`this`), from both callers. Crashed when an element appears.
- vaati `CreateRepeatedSoundManager(super)` / `DeleteRepeatedSoundManager(super)`: the script context, which
  they read (`context->intVariable`).
- room.c `sub_080186EC()` takes a parameter it never reads: left alone.

## Arrays indexed past their declared count, with elements that grow natively

`gNPCData` is declared `NPCStruct[50]`, but the game indexes it far past 50 into the rest of its GBA region
(4096 bytes: 256 entries of 16). `NPCStruct` holds a script pointer, so natively an entry is 24 bytes, and
the block, sized to the GBA region's bytes, held only 170. Hyrule Town after the Deepwood Shrine
(DelayedEntityLoadManager filling townspeople at `type2 + gArea.filler[1]`) wrote past it into the manager
slots (`gUnk_02033290`): the first slot's links became `NULL` / `0x03ff0000` and the next walk of the manager
list (a townsperson's `sub_0805ACC0`) crashed. Found with a watchpoint on the slot from the player's state.
Fixed in the RAM layout (`port/tools/datagen/ram.py`): a fixed-address array whose element is bigger natively
gets room for as many elements as its GBA region holds. Only `gNPCData` changed size. The room smoke test did
not see it: its checkpoint is from before the town fills up.

## DMA started by writing the registers directly

The port performs DMA in `HwDmaSet` (through `DmaSet` and the `DmaCopy*` macros). Code that writes the DMA
registers itself (`DMA3->sourceAddress = ...; DMA3->control.word = ...`) only fills `gHwIo` natively, and no
transfer happens. kinstoneMenu `sub_080A4418` loaded uncompressed kinstone piece graphics this way, so the
fusion screen showed whatever was in that VRAM. Fixed under `#ifdef PLATFORM_NATIVE` with `DmaSet`. The only
other direct use, `src/eeprom.c`, is the GBA EEPROM driver the port replaces (`port/src/eeprom.c`).

Also on the fusion screen, a ROM write: `KinstoneMenu_080A4468` clears `currentObject->kinstoneId`, and with
nothing to interact with `currentObject` is the const `gNoInteraction`. Skipped natively for that object.

## Lists of structs walked with a GBA stride

`sub_0805ACC0` (DelayedEntityLoadManager) finds a townsperson's home in the room's NPC list as a `u16*` stepped
by 8 per entry: 16 bytes, the GBA size of `NPCStruct`. Natively the struct is 24 bytes (its script pointer),
so every townsperson after the first read its home from the middle of another entry, walked toward it (the
room's corner, for example), left the screen, and its spawn bit was cleared, so it deleted itself a second or
so after the player arrived or came close. Fixed under `#ifdef PLATFORM_NATIVE` by indexing the list as
`NPCStruct*`. The other `u16*` walk of a room property (smallIceBlock `sub_0809963C`) reads one struct with no
pointer in it.

## Fixed-address variables that the GBA overlays inside a bigger one

On GBA, `gDungeonMap` (0x0201AEE0) lies inside `gMapDataBottomSpecial` (0x02019EE0, 0x8000 bytes), and
`DrawDungeonFeatures` relies on that: its `MemClear(gMapDataBottomSpecial, 0x8000)` also erases the pause menu's
dungeon map before the floor is drawn. Natively every fixed symbol gets its own block (`ram.py`), so the clear
missed it and each floor drew over the floors shown before. Fixed under `#ifdef PLATFORM_NATIVE` with a second
`MemClear(gDungeonMap, ...)`. Checked with a save state on the map screen: the floor's frame checksums change and
are the same on every visit.

Sweep (blocks in `build/native/data/ram.s` larger than their GBA region): the others are larger because of
native pointer growth. Inside `gMapDataBottomSpecial`'s 0x8000 GBA bytes the only other symbol is `gDungeonMap`;
the remaining clears and copies of it (`playerUtils.c`, `fileselect.c`) use it as a tile map or the file select
data, not for the dungeon map.

## Pointers read at a GBA byte offset

Gleerok's death (`sub_0802D258`) picks a body part for each explosion as `*(Entity**)((u8*)heap + 0x3c + n * 4)`:
the GBA offset of `entities[]` and a 4-byte step. Natively the pointers are 8 bytes and start at another
offset, so it read half of one pointer and half of the next and crashed in `CreateFx` a few seconds into the
death animation (the second lava boss, Cave of Flames). Fixed under `#ifdef PLATFORM_NATIVE` by indexing the
pointers (`n` is 0, 1, 4 or 5; 5 is `ent2`, right after `entities[4]`). The room smoke test could not reach it:
it needs the boss beaten. Sweep for `*(T**)(` casts through byte pointers: `src/interrupts.c` (GBA-only branch)
and vaatiArm (already native-specific).

## Structs written as GBA byte arrays

The figurine device (shell trading, `sub_080882A8`) passed two `u8[24]` arrays to `ShowTextBox` as `Font*`
("TODO convert data"). The bytes hold GBA addresses (the BG0 buffer, VRAM, `gTextGfxBuffer`) at 4 bytes each;
natively `Font` has 8-byte pointers, so its `dest` came out as `0x0600d000_02034fb6` and `DispMessageFrame`
crashed writing through it as soon as the trade text box opened. Fixed under `#ifdef PLATFORM_NATIVE` with two
`Font` initializers (the native pointers, as phonograph.c does); a check program confirmed that the same field
values give back the original 24 bytes on GBA. Sweep for `(Font*)` / `(const Font*)` casts: none left.

## Division and modulo by zero

agbcc compiles signed `/` and `%` to calls to `__divsi3` / `__modsi3`. This ROM does not link agbcc's
`tools/agbcc/lib/libgcc.a` for them but its own `asm/lib/libgcc.s` (linker.ld): `cmp r1, #0; beq` to a stub that
returns r0 = r1 = r2 = 0, otherwise `svc #6` (BIOS Div), returning r0 (quotient) or r1 (remainder). There is no
`__udivsi3` / `__umodsi3` in the ROM, so no unsigned non-power-of-two division exists in the GBA code. (agbcc's
libgcc.a agrees anyway: all four of its routines branch to `Ldiv0`, `bl __div0` (`mov pc, lr`), `movs r0, #0`.)
So on the GBA `x / 0 == 0` and `x % 0 == 0`, and through the BIOS `INT_MIN / -1 == INT_MIN`, `INT_MIN % -1 == 0`.
Natively, arm64 `sdiv` gives 0 for `/` but `%` then gives the dividend; x86_64 (the universal build's other slice)
traps with SIGFPE on both, and on `INT_MIN / -1`.
`TextDispEnquiry` hits it on every choice: after A it clears `gMessageChoices` and then computes
`choiceIdx % choiceCount` with a count of 0. With the second option chosen the native result was 1, so it
"moved" the cursor to the cleared position 0 and drew an arrow over the first letters of the text box. Fixed
under `#ifdef PLATFORM_NATIVE` (count 0 gives 0). Found with a save state at a Business Scrub ("No, thanks").
### Sweep of every division (all of `src/` and `port/src/asm/`)

**Helpers.** `include/global.h` has `GBA_DIV(num, denom)` / `GBA_MOD(num, denom)`. On the GBA build they expand to
the plain `((num) / (denom))` / `((num) % (denom))`, which is the same tree for agbcc (`tmc.gba: OK`). Natively they
call inline `GbaDiv` / `GbaMod`, which give the ROM's results: 0 for a zero divisor, and `INT_MIN / -1` as INT_MIN
rem 0 (`(-7, 2)` gives -3 rem -1, as `sdiv` / the BIOS do). They are applied only at sites that can see a zero
divisor; provably non-zero sites stay as they are. There's no global switch: no clang flag changes the result of
`%` by 0, and `-fsanitize=integer-divide-by-zero` only reports (on x86_64 the division still runs and traps).
Wrapping all 79 sites would edit matching source for nothing, while missing a zero site is a correctness bug; so
the rule is: anything not provably non-zero gets the helper. Both helpers take and return s32; all 35 wrapped
sites are int / int after promotion (checked from the AST), so the result is unchanged for every non-zero divisor.

**Method.** A libclang pass over `build/native/compile_commands.json` (627 translation units: every file under
`src/` and `port/src/asm/`, with the native flags, so `NON_MATCHING` bodies and native-only branches are what's
seen) lists every integer `/`, `%`, `/=`, `%=` with its divisor and evaluates constant divisors. 451 sites: 372
constant divisors (none 0), 79 non-constant. Each non-constant one was read and classified below. Runtime check: a
build with `-fsanitize=integer-divide-by-zero` (before the fixes) ran the 24000-frame playthrough and the
576-room smoke test. Neither reported anything (the message.c site was already fixed), so the fixes below come
from the static reading, not from a runtime hit. The script is `divscan.py` in the session notes folder
(`side-session-notes/<group>/div0-sweep-tools/`); run it from the repo root as
`.venv/bin/python -I divscan.py > hits.json`.

**Provably non-zero (43, unchanged):**
- explicit guard: `CalculateDirectionFromOffsets` (port asm, `dx != 0`); `FixedDiv` (`r1 == 0` returns);
  `sub_0807D6D8` x4 (returns if `unk_18` or `unk_1a` is 0; squares of u16 can't wrap to 0).
- constants in locals: gleerok `x = 3`, ropeGolden `tmp = 10`, carlovObject `tmp = 3`, greatFairy x2 `temp = 11`,
  fileselect `sub_080610B8` x2 (4/5, 2/3), kinstoneMenu `0xb21`, figurineMenu 130/136, fileselect `sub_08050940`
  (`num_rows` 3/4), `EncodeBCD` x2 (10000000..10).
- only reached with a positive count: fireballGuy x2, slime x2 (inside `for (i = 0; i < count; i++)`);
  `ShuffleEzloHints` (callers pass 5 and 8, so `uVar4 + 1 >= 2`); physics `sub_0806FB38` x2 (`byte3 > byte2 >= 0`).
- bounded expressions: minishEzlo `sub_080630A4` and player `PortalShrinkUpdate` (`(x >> 6 ^ 3) + 1` is 1..4);
  elementsBackground (`|z| + 0x100`); gleerokProjectile and v1FireProjectile (`iVar2` clamped to >= 0x20);
  scroll `sub_080803D0` x4 (`dd_x >= 66`, `dd_y = dd_x * 2 / 3`); gleerokParticle Action4 x2 (the scale steps one
  away from 0 before the divide).
- data that is all non-zero: goronMerchantShopManager `numTypes` (3, 2, 3); m4a `SampleFreqSet` x2 (table 96..704,
  only called with a non-zero frequency); `GetRandomInt` (script args 4, 9, 9); `CreateRippleFxRandom` x2 (only
  caller passes 4) and `CreateLargeRippleFxRandom` (only callers pass 8, 10).

**Possibly zero or unknown (36: 35 now `GBA_DIV` / `GBA_MOD`, plus message.c):**
- boss scale factors that are moved every frame: chuchuBoss `0x10000 / unk_74|78.HALF_U.HI` x6, `unk_81 << n / speed`
  x3, `Random() % (hitbox->width|height << 1)` x2 (hitbox size is a truncated u8 of the scale); octorokBoss x3,
  frozenOctorok x2, octorokBossObject x1 (`0x10000 / unk_74|76`); chuchuBossStartParticle x2 (parent hitbox).
- shrinking through 0: gleerokParticle Action2 x2 (scale moves by `speed` toward and past 0), and physics
  `sub_0806FCF4` (`0x10000 / |param_2|`; gleerokParticle passes that same scale).
- counts that can be 0: figurineDevice `sub_0808826C` (`unk_80` counts available figurines from 0); itemUtils
  `CreateRandomItemDrop` (`summOdds` is a sum of drop weights clamped to >= 0).
- room or script data: wizzrobeWind x2 (`unk_6e/6f` from the room entity); minishRaftersBackground x2 and
  staticBackground x1 (`room height/width - screen size`, 0 for a screen-sized room); postman x3 (counts from
  `gUnk_0810A918`); objectA2 (`unk_1` from `gUnk_08124708`); script `ScriptCommand_0807ED24` x2 (`unk_12` is the
  third arg of `_0807ED24`: 0x3c..0xb4 in the used scripts, but 0 in `data/scripts/unused/script_08016482.inc`).
- message.c `TextDispEnquiry` keeps its own `#ifdef` (above).

**BIOS Div / Mod.** The game also calls BIOS `Div` and `Mod` (SWI 6) directly; `DivArm` (SWI 7) is unused. Unlike
`__divsi3`, these don't check for 0 first. GBATEK: "The function usually gets caught in an endless loop upon
division by zero." Stepping through the BIOS loop for denom 0: it returns quotient 1 (or -1 for a negative num),
remainder num and r3 1 when |num| <= 1, and spins forever otherwise. mGBA's HLE returns the same. This is reasoned
from the algorithm and mGBA, not run on a real BIOS. `port/src/bios.c` returned 0 / 0. The one caller that can
pass 0 is `sub_0801E31C` (iris/circle window, `src/common.c`): with a 0 radius it calls `Div(0, 0)` and stores the
quotient in `gUnk_02018EE0[0]`, so the GBA writes 1 there. `Div` now returns `num < 0 ? -1 : 1` rem `num` for denom
0 (the |num| > 1 hang is not emulated), `INT_MIN / -1` as INT_MIN rem 0, and `Mod` is `Div`'s remainder like the
ROM's `svc #6; adds r0, r1, #0`. Other direct callers divide by constants or are guarded (`sub_0801E64C` slope:
`y1 == y2` returns).

## Unused copy slots (Four Sword clones)

`gPlayerClones[3]` holds the copies; with fewer than three (the first Crystal Sanctuaries give one or two) the
rest stay NULL while `PL_CLONING` is set. `sub_080171F0` (every player update) cleared `spriteOffsetY` on all
three, and pressurePlate `get_standing_count` passes each to `IsColliding`; the GBA reads and writes BIOS
through the NULL slots. Natively the first one crashed as soon as a copy was made. Fixed under
`#ifdef PLATFORM_NATIVE` by skipping NULL slots: pressure plates and floor buttons (button.c `sub_08081D74`,
`EntityInRectRadius` per slot) treat them as not standing, and the push tile `ACT_TILE_114` (beanstalkSubtask.c)
reads copy 0's per-copy bits only when copy 0 exists. Swept every `gPlayerClones` use: the rest check the slot
(`sub_0801A8D0`, ghini, enemy50, macroBook, button.c's push) or are a copy reaching its own slot (playerClone.c).

## Build warnings (universal Release build)

A clean `arm64;x86_64` Release build prints 77 warnings per architecture. Checked by kind:

- `-Wcast-function-type-mismatch`: the two-value returns (`DivAndMod`, `GetFuserIdAndFuserTextId`) have native
  s64/u64 implementations (`port/src/bios.c`, `port/src/asm/code_08001A7C.c`); the m4a `nullsub_544` stubs take
  no arguments. townMinish casts five `(Entity*, ScriptExecutionContext*)` functions to the one-argument dialog
  callback: four never read the context, but `sub_0806B004` writes `context->condition` (on GBA through its own
  ROM address, so lost; natively through a stale register). Its dialog entries now call a native wrapper with a
  scratch context.
- `-Wint-to-pointer-cast`: `sub_0805F6A4`'s result is only tested for zero (text.c); kinstoneMenu passes a text
  id (< 0x10000) where a text index or pointer is taken.
- The rest (`-Wswitch`, bit-field and `s8` constant truncation, unused values, extra parentheses, a const
  qualifier, an array address in a condition) are the decompiled code as it matches the ROM and behave the same
  natively.

## Function pointers passed as entities

`CreateVaatiApparateManager` (decomp `@bug`) passes `gArea.onEnter`, a
thumb function address, to `DeleteManager`. On the GBA the unaligned loads
read code bytes as `prev`/`next`; for every `*_OnEnterRoom` function the
writes go to BIOS, open bus, or at worst 4 bytes of `gTextGfxBuffer`, and
`MemClear` writes to ROM. Natively those are wild pointers, so the native
build only does the `gManagerCount--` that the GBA does.

## `child` / `parent` that nothing sets

An entity reads its `child` or `parent` on a path where neither its own code nor its creator stored one. The
pointer is NULL (slots are cleared), and the GBA reads BIOS open bus through it: the last BIOS opcode fetched,
0xe55ec002 after an interrupt returns (0xe3a02004 after a SWI). Stores go nowhere. Template: the Great Fairy's
form 9 (81cf59a5). Fixed under `#ifdef PLATFORM_NATIVE`, keeping what the GBA does with those bytes:

| Site | Missing pointer | Reached | Fix |
|---|---|---|---|
| [object63.c](../../src/object/object63.c) `Object63_Init`, `Object63_Action1` | `child`: `CreateItemDrop` sets it to the dropped ground item, but only for a ground item. With no drop (often: drop table 4) or a beetle, it stays NULL | pushable furniture type 2/4 with `type2` 1 (`Entities_HouseInteriors2_EastOracle_1`) when the player bounces into it with the Pegasus Boots (`sub_0808FECC`, `sub_0808FF20`). Natively a crash on every empty drop: `TMC_WARP=22:09:108:110 TMC_WARP_ITEMS=1,21` from `hyrule_field_north`, input `+60:up:2,+10:up+b:70` gives an empty drop on the first dash and, unfixed, SIGSEGV at 0x28 in `Object63_Init` | skip the `zVelocity` store and the 32 frames of `y++`, which go to BIOS on the GBA |
| [stalfosProjectile.c](../../src/projectile/stalfosProjectile.c) `StalfosProjectile_Action2` | `parent`: `Stalfos_SubAction2` (the gust jar pulling the head off a Stalfos that carries nothing) creates the head, type 1 / type2 1, without one | the head's Init runs in the same frame (projectiles update after enemies) and the gust collision usually grabs it, so OnGrabbed runs instead. Action2 runs if it is not caught (the gust jar button released that frame, the head outside the gust cone). Not reproduced headless | the GBA sees `next` set and `action` 2 (not 9), so `sub_080A9C50` copies the BIOS bytes as the position every frame (every candidate is a large negative x/y: far up-left, out of sight). Natively the head gets those values and skips the parent read. The ROM byte the GBA adds to `spriteOffsetY` (read past `gUnk_08129B50`) is left out |

**Method.** `child`/`parent` reads (`super`/`this`, through casts and locals) in `src/object`, `npc`, `enemy`,
`projectile`, `manager`, `playerItem` (853 reads, 136 files), matched against: stores in the same file; stores by
every creator found through `CreateObject`/`CreateObjectWithParent`/`CreateFx`/`CreateEnemy`/`CreateNPC`/
`CreateProjectile`/`EnemyCreateProjectile`/`CreateManager` (ids mapped to files through `gObjectFunctions`,
`gEnemyFunctions`, `gNPCFunctions`, `projectile_functions.s`); and the forms that come from room data
(`entity_raw` family in `data/`, so no creator). A second pass listed, per function, unguarded dereferences of
a pointer the file sets only in another function (197), checked by hand where the setter is conditional. The
scripts are in the side-session notes folder (`null-entity-sweep/`).

**Checked and fine.** Set by the creator for every form that reads it, or guarded:
Enemy50 (`(enemy->base).parent`), ScissorsBeetle (`sub_08038C2C` runs on the mandibles), BusinessScrubPrologue,
MazaalHead/Macro/Object, BigGoron (types 1/4 find their parent, type 3 is made by type 2), Rem, VaatiReborn,
Bird, ChuchuBossParticle (`CreateObjectWithParent`), DoubleBookshelf, Flame (types 2/3 set by every creator; room
data has only type 4), LavaPlatform (child set with the timer it is gated on), PullableLever, FloatingPlatform
(room-data platforms have `timer` 0, so the parent is not read), LinkAnimation/LinkHoldingItem, GreatFairy
MiniAffine, LakituCloudProjectile, Projectile5, Fire/Ice/WindProjectile (NULL-checked), GleerokProjectile (type 3
reads neither), OctorokBossProjectile (type 3 reads none), BallAndChain (type 4 reads none), Thunderbolt (only
compared), Vaati3Arm (type 2 reads none), WhiteTriangleEffect, SpecialFx (`FX_27`/`FX_32..34`, the only users of
`child->next`, all come from `collision.c` with the child set), Vaati3InsideArmManager, PlayerItemGust/GustBig/
FireRodProjectile, MandiblesProjectile, Keaton, SpearMoblin, ItemOnGround, Fairy, CameraTarget, PlayerClone,
NPC23, Vaati3StartManager, RupeeLike, Pesto, CloudPiranha, GyorgMale. Rupee, Lakitu, MazaalHead and others
dereference a child they just failed to create: that is allocation failure, not this class
(`TMC_ENTITY_LIMIT`, a3010676, is the tool for that class).

Not swept: heap entity pointers (`myHeap` part tables of the multi-part bosses; the room smoke test found the
first-update cases), and the 2nd-pass sites in ChuchuBoss, MazaalBracelet, Moldworm, Madderpillar, Moldorm,
which set every link in one init.
