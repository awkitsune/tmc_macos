# Room smoke test

Puts the native game into every room it has, plays a few hundred frames there, and reports what breaks:
crashes, writes into ROM data, hangs. Before it, only the scripted playthrough route (new game to Hyrule
Field) and hand play had run natively, so most rooms, and every late-game one, had never been loaded.

## How to run it

```sh
port/scripts/room_smoke.sh                  # every valid room, into build/room_smoke
port/scripts/room_smoke.sh OUT 48:00 51:00  # just these rooms (hex area:room)
```

It takes about a minute for all 576 rooms on an M-series Mac (4 runs at a time; each run is ~0.1 s).
Outputs in `OUT`:

- `rooms.txt` / `skipped.txt`: the rooms tried, and the room headers left out with the reason;
- `logs/AA_RR.log`: the runner's output for each room;
- `results.tsv`: one row per room: outcome, failing function, whether the run ended in the warped-to room
  (`reached`), whether the warp arrived with the player in control (`arrived`), the room it ended in,
  frames run, and the crash or ROM-write detail;
- `summary.txt`: counts, and the failures grouped by distinct function.

Settings (environment): `STATE` (default `port/states/hyrule_field_north.tmcs`), `FRAMES` (default 720,
counted from the warp; the warp itself takes ~35), `ITEMS=A,B` (item ids from `include/item.h` to equip,
see below), `JOBS` (default 4), `WATCHDOG` (seconds, default 30), `TMC_BIN`.

Each run is `TMC_LOAD_STATE=$STATE TMC_WARP=AA:RR TMC_FRAMES=+$FRAMES TMC_ROM_WRITES=1 TMC_WATCHDOG=30` with
a fixed input script: from frame +60, walk a square (24 frames per side) with A (talk, lift, read) and B
(sword or the B item) between legs, R (the action button) and a held B, repeated. Outcomes: `ok`, `crash`
(signal, and the first game function on the stack), `rom_write` (logged and skipped by `TMC_ROM_WRITES`),
`hang` (watchdog), `script`/`error` (the runner failed).

### The warp: `TMC_WARP=AA:RR[:X:Y]`

[port/src/warp.c](../src/warp.c), called by the headless runner each frame. Once the player has control
after the state loads, it fills a `Transition` and calls the game's own `DoExitTransition`, as an exit or
warp point does; the game then runs its usual area change (`CheckRoomExit`, `GameMain_ChangeArea` /
`GameTask_Init`, room init). No room globals are written. The player arrives at:

1. X,Y if given;
2. otherwise an entrance: the first transition in the game's exit lists (`gExitLists`, every room) that leads
   into the room, with its position, layer, spawn type and facing;
3. otherwise the room's centre, and once there the player is moved to the nearest tile with no collision.

It prints `warp to ...`, `warp arrived in AA:RR` (the room was reached with the player in control) and, when
the run ends, `warp: end frame F area AA room RR ...` (or `warp: crashed in area AA room RR` from the crash
handler).

No items or flags are needed for the warp. The checkpoint used is the last one the playthrough writes
(`hyrule_field_north`: sword and shield, Zelda following during the festival); `town_shield` and `bedroom` were
also run. `TMC_WARP_ITEMS=A,B` (`ITEMS=` in the script) equips two items on A and B at the warp with 10 bombs and
30 arrows, and sets the one flag they need, `EZERO_1ST` (Link has Ezlo). Without that flag the player has no
cap (`PL_NO_CAP`), and the item animations have no capless form (`sub_08077D38`): using the gust jar then left the
player with sprite 0 and crashed in `sub_08078E84`. That state cannot happen in the game (every item comes after
Ezlo), so it is not a port bug.

### Which rooms

[room_smoke_rooms.py](../scripts/room_smoke_rooms.py) reads the four room tables in `data/map/*.s`
(`gAreaRoomHeaders`, `gAreaTable`, `gAreaRoomMaps`, `gAreaTileSets`). 738 room headers exist; 576 are valid
rooms. The 162 left out are leftovers no exit leads to:

| Reason | Rooms |
|---|---|
| header without a tile set (`tileSet_id` 0xFFFF) | 76 |
| room properties NULL in `gAreaTable` (no entity lists or callbacks; the GBA would read them from the BIOS) | 82 |
| no map: `gAreaRoomMaps` entry NULL or past the end of the area's list (02:01, 58:05, 58:06) | 3 |
| tile set index past the end of `gAreaTileSets` (47:02) | 1 |

Warping into the 80 rooms with a NULL properties entry crashed in `sub_0804AF90` (NULL properties), and the
past-the-end ones in `LoadRoomTileSet` / `LoadRoomGfx`; on the GBA they would load garbage. They are not port bugs.

## Results

| Run | Rooms | ok | crash | ROM write | hang |
|---|---|---|---|---|---|
| first run (all 662 headers with a tile set) | 662 | 562 | 100 | 0 | 0 |
| the 576 valid rooms, before the fixes | 576 | 561 | 15 | 0 | 0 |
| after the fixes, 720 frames | 576 | 576 | 0 | 0 | 0 |
| after the fixes, 2400 frames | 576 | 576 | 0 | 0 | 0 |
| `STATE=town_shield`, `STATE=bedroom` | 576 each | 576 | 0 | 0 | 0 |
| `ITEMS=` gust jar + bombs, cane + boomerang, bow + Roc's cape, mole mitts + lantern | 576 each | 576 | 0 | 0 | 0 |

In the final 720-frame run, 562 rooms were reached with the player in control. 530 ended in the room warped to
and 46 elsewhere: the activity walked through an exit, the entrance was a stair or hole that leads on, or the
game redirects (Hyrule Town to Festival Town during the festival, 02:00 to 15:00). The 14 rooms where the player
never got control are cutscene or boss rooms (Vaati 2/3, Gleerok, Deepwood barrel, castle 80:04, Ruins entrances).

## Bugs found and fixed

All under `#ifdef PLATFORM_NATIVE`; `tmc.gba: OK` after each. Every one is a native-only crash: the GBA ignores the
same access (BIOS reads, writes to address 0). Each was confirmed by the room that crashed passing afterwards.

### 1. A part's first update runs before the part it depends on has initialised

A boss is built from entities created in one init; a part created earlier updates first, in the same frame,
and dereferences a pointer that a later-created part's init sets. The GBA reads BIOS through it.

| Site | Missing pointer | Rooms (crash) |
|---|---|---|
| [octorokBoss.c](../../src/enemy/octorokBoss.c) legs (two reads), via `OCTOROK_MOUTH_HEALTH` in [octorokBoss.h](../../include/enemy/octorokBoss.h) | `heap->mouthObject` (the mouth is created after the 4 legs) | 60:0e Big Octo: every Octorok boss fight |
| [frozenOctorok.c](../../src/object/frozenOctorok.c) `FrozenOctorok_Action1` legs | same | 60:08 Temple of Droplets element room |
| [gleerok.c](../../src/enemy/gleerok.c) `sub_0802E7E4` | `heap->entity1`, only set when the fight starts (`sub_0802D714`); written to during the intro | 50:12, 51:00: every Gleerok fight |
| [gyorgBossObject.c](../../src/object/gyorgBossObject.c) `sub_080A1DCC` | `heap->mouth`, `heap->tail` and the tail's segments, created by the female's first update; the heap itself while `SetupStart` waits for entity slots | 71:00: every Gyorg fight |
| [chuchuBoss.c](../../src/enemy/chuchuBoss.c) `sub_080279AC` | a part's `hitbox` (NULL until that part's own init) | 49:00 Big Green Chuchu, after ~800 frames (seen only with `FRAMES=2400`) |

The values the GBA reads instead are BIOS bytes. The fixes skip the stores, as the GBA's are lost, and take the
health read through the missing mouth as "not 1, at least 2", which is what any BIOS opcode byte at that offset
gives.

Sweep: every heap field a part sets to itself (`heap->x = this/super`): octorokBoss, frozenOctorok (fixed), vaatiWrath
(`type0`, set before the parts are created: fine), gyorgBossObject `boss` (set first: fine). Gleerok's other heap
pointers are set in the init loop before any part updates.

### 2. A detached child reads its parent in its first update

[darkNutSwordSlash.c](../../src/projectile/darkNutSwordSlash.c): the Dark Nut detaches its sword slash
(`sub_08021588`: `child->parent = NULL`) and can do so in the frame it creates it, before the slash's first
update. The slash's init reads `parent->type` before its own `parent == NULL` check. Natively that's a NULL read.
Rooms: 2b:00, 70:0c, 88:0a, 88:1f (all Dark Nut rooms).

Sweep: the other `child->parent = NULL` sites (keaton, pesto, scissorsBeetle, keyStealingTakkuri, rupee,
`EnemyDetachFX`, mazaalMacro, dirtBallProjectile) and a scan for functions that use `parent->` before a NULL
check on it (ballAndChain `sub_080AB074`, wizzrobeWind `Action3`; both keep their parent): their children check
`parent` first or are never detached early. Not settled: `DirtBallProjectile_OnCollision` writes
`parent->child` for type 0 balls; pesto detaches its child in `sub_08024AD8`, so a ball detached and then hit in
the same frame would write through NULL. Not seen in any run.

### 3. A NULL heap of an entity that has not run its init

[zeldaFollower.c](../../src/npc/zeldaFollower.c) `sub_08068578`: with Zelda following (`ZELDA_CHASE`, the
festival), arriving by stairs calls `SetZeldaFollowTarget` from the player's update. The follower is created on
room entry, so its first update (which allocates its 20-entry position heap) has not run yet, and the store goes
through a NULL `myHeap`. Rooms: 33:05, 48:02, 58:1c, 70:0d, 88:07, 88:2b (any stairs arrival). In the game this
needs Zelda following and a stairs entrance, e.g. a house with stairs in Hyrule Town during the festival.

## What the smoke test cannot see

- **Story-gated behaviour.** A warp skips the story: bosses start without their flags, cutscene rooms play their
  scripts out of order or wait forever (14 rooms never gave control), and rooms that the story changes (Hyrule
  Town during the festival) are only seen in the state the checkpoint is in. Code that only runs after a story
  event (a puzzle solved, a boss beaten, Zelda in stone, the Vaati fights proper) is not exercised.
- **Most player items.** Covered with `ITEMS=` pairs only as listed above; no bottles, no Four Sword clones, no
  Minish Link (portals were not used).
- **Rooms not reached.** The 46 runs that ended elsewhere still loaded the room, but spent less time in it.
  Rooms only reachable by a transition the warp does not reproduce (scrolling between rooms, falling) are entered
  by area change instead, so their scroll-in code did not run.
- **Long sessions.** 720 frames (and one 2400-frame run) per room; slow timers, heap exhaustion and save/load
  paths are out of reach.
- **x86_64.** No failure looked architecture-specific (all were NULL dereferences), so no x86_64 rerun was
  needed; `TMC_ROM_WRITES` is arm64 only.

Nothing in these runs needed a rerun on x86_64, and none of the failures were ROM writes: the earlier sweep's
fixes (GustJar, SpearMoblin) hold in every room.
