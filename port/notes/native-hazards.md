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
