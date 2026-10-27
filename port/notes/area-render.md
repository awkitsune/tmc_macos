# Area renderer

[arearender.c](../src/arearender.c) ([arearender.h](../include/port/arearender.h)) draws any room, a whole area or
a dungeon floor into an RGBA buffer from the game's data, without touching the running game. It is the core for a
whole-area overview in the app's Map window; the UI is not wired up yet.

`TMC_MAPDUMP` ([mapdump.c](../src/mapdump.c)) can only draw the room the player is in, because only that room's
tiles are in VRAM. The area renderer decodes every room's tile set, palettes, metatiles and tile maps itself.

## API

```c
AreaMap map;
if (AreaMapRenderArea(area, &map)) {        // or AreaMapRenderRoom(area, room, &map),
    // map.rgba: map.width x map.height x 4  // AreaMapRenderFloor(dungeon, floor, &map)
    // map.rooms[i]: area, room, x, y, width, height (in the image), worldX/worldY, name, atMapPosition
    int i = AreaMapRoomAt(&map, x, y);       // hit test (topmost room)
    AreaMapFree(&map);
}
```

- Rooms are placed at their room header's map position, offset so that the top left room touches 0,0. Alpha is 0
  between rooms. `name` comes from the generated room name table (`RoomName`, port/scripts/room_names.py).
- Dungeon floors (dungeons 1 to 6, floors from `gDungeonFloorMetadatas`, floor 0 the highest) take their rooms
  from `gDungeonLayouts`, as the pause menu's map does, at map position modulo 0x800. A floor can take rooms from more
  than one area: Deepwood Shrine's top floor has the boss door room (area 0x48) and the boss room (0x49).
- It reads only constant data and allocates its own memory (about 2.5 MB while it draws, plus the image), so any
  thread can call it at any time. The largest image is Palace of Winds (area 0x70), 1920x9808 (75 MB of RGBA). The UI may
  want to scale it down or draw one floor at a time.

## How it works

Loading a room works the way `LoadRoomTileSet` and `LoadRoomGfx` do. The renderer walks the room's three
`MapDataDefinition` lists: tile set, area tiles and room map. It redirects every destination into a `Scratch`: VRAM,
`gMapBottom`, `gMapTop` and `gMapDataBottomSpecial`/`gMapDataTopSpecial`. Palette entries go to a scratch palette:
it starts as `LoadGfxGroups` leaves it (palette groups 11 and 12, colour 0 black), and each group is followed by
`sub_080533CC`. Then the renderer:

- spreads the maps to 64 tiles a row (`sub_0807C8B0`);
- for a "bitmap" room, whose bottom map starts with 0xFFFF (Minish house interiors, town Minish holes), arranges the
  8x8 maps' screen blocks (`sub_0807C5F4`);
- uses the BG control bits and BG1 on or off that `LoadRoomGfx` sets per area: 256 colours in the two Minish areas,
  and the other top layer control in house interiors;
- draws as mapdump does: backdrop colour, bottom layer, then top layer, colour 0 transparent, special tiles
  (0x4000 and up) left out.

Which rooms can be loaded comes from a table generated at build time ([area_rooms.py](../scripts/area_rooms.py),
the same list as the room smoke test's [room_smoke_rooms.py](../scripts/room_smoke_rooms.py)). That is 576 rooms in
88 areas. The lists in `gAreaRoomMaps` and `gAreaTileSets` have no end marker, so at run time a past-the-end entry
(02:01, 58:05, 58:06, 47:02) looks like a real pointer. Rendering it crashed in area 0x63 before the table existed.
Areas 0x40, 0x46 and 0x61 have room headers but no loadable room.

### Hyrule Town, Festival Town and Minish Village

These rooms swap tile graphics (and, in Minish Village, a palette group) as the camera moves
(`hyruleTownTileSetManager.c`, `minishVillageTileSetManager.c`). Each slot of BG VRAM is loaded with the group of the
first region in its list that is on screen. No single VRAM content draws the whole room correctly. So for each pixel,
the renderer works out the groups the game would load for a screen centred on that pixel. It draws the room once
for each combination of groups and keeps each pixel from its own combination. Hyrule Town's tables are `static` in
the manager, so they are copied into arearender.c. Minish Village's are global and are used directly.

### Rooms that share a position

Some rooms have no usable map position. Cloud Tops' three rooms are all at 2976,0, and three unnamed Royal Crypt
rooms are at 0,0. A room that overlaps an earlier room by more than half of the smaller of the two moves to a row
under the other rooms, and its `atMapPosition` is false. A thin strip of overlap is kept, with the later room drawn on
top. This happens where two Fortress of Winds rooms meet: 58:01 and 58:02 overlap by 48 pixels at their map
positions. Across all areas and floors, 2 areas have moved rooms (Cloud Tops, Royal Crypt: 5 rooms), and Fortress of
Winds has overlaps.

Dungeon areas cover all their floors in one map position space, so `AreaMapRenderArea` of a dungeon shows the floors
side by side as their map positions put them. Dark Hyrule Castle (0x88) is 1712x9312, for example. Use
`AreaMapRenderFloor` for one floor.

## Headless hooks

- `TMC_AREADUMP=dir` writes every area as `area_AA.png` and every dungeon floor as `dungeon_D_floor_F.png`
  (111 PNGs), each with a line `areadump area AA rooms N skipped N size WxH checksum C [moved] [overlapping]`.
  `TMC_AREADUMP_ROOMS=1` also lists each room's place, map position and name. The dump runs once, at
  `TMC_AREADUMP_FRAME=F` or else when the player first has had control for 30 frames. Before the dump, the runner
  copies the whole save state segment (`__TMCSTATE`: every game global, EWRAM and IWRAM, the emulated VRAM, palette
  RAM, OAM and I/O registers, the sound engine and EEPROM). After the dump it compares the copy byte for byte with
  the segment and prints the result. Write the PNGs to a scratch directory: they are ROM-derived and must not be
  committed.
- `TMC_AREACHECK=1` cross-checks each room the player has had control in for 30 frames. It draws the room with the
  renderer and draws it again from the live tile maps, VRAM and palettes (mapdump's tile layers, without the markers
  and grid). Mixed versions of the two pictures split the differing pixels into causes:
  `areacheck AA:RR WxH: match|differ, N of M pixels differ: a by tile changes (k metatiles), b by animated tiles,
  c by other VRAM (...), d by palettes (... other BG palettes: mask)`.
  - Tile changes means the live metatile maps differ (doors opened, chests, pots, cut grass, puzzle state).
  - Animated tiles means VRAM bytes that the area's background animations (`gUnk_080B755C`, `LoadBgAnimations`)
    write.
  - Other VRAM is anything else in VRAM.
  - Palettes means palette differences, with the palettes outside the area's palette animations as a bit mask.

## Verification

All of these ran headless; nothing was checked by looking at images.

- **No side effects.** The dump ran mid-game in the 01_new_game playthrough, at frame 8396 in area 0x22. The state
  segment was 688,128 bytes and its checksum was 1cef20ab71a8f177 both before and after: 0 bytes differed. The same
  result came from a dump at frame 23060 from `hyrule_field_north.tmcs`, where the checksum was 3d5e4403ff14b0e5 before
  and after.
- **Playthrough hash.** `TMC_SAVE=x TMC_FRAMES=24000 TMC_INPUT=@port/scripts/play/01_new_game.txt TMC_FRAME_HASH=out`
  ends `23999 89eb5912` with the dump on, with the cross-check on, with both, and with neither. All 24,000 frame hashes
  are identical to the run without them. Use a fresh `TMC_SAVE` path each time: a save file left over from an earlier
  run changes the playthrough.
- **Deterministic output.** The 111 area and floor checksums are the same from both dumps above, which ran from
  different game states.
- **`port/scripts/room_smoke.sh`**: ok 576.

### Cross-check against the live picture

The playthrough reaches 6 rooms with control. One matches exactly. The differences in the other five are all tile
changes and animated tiles. Festival Town (15:00) also matches on its tile graphics once the town's region groups
are applied (before that, 32,757 of its pixels differed).

To cover every room, each of the 576 rooms was warped into from `hyrule_field_north.tmcs`
(`TMC_WARP=AA:RR TMC_FRAMES=+240 TMC_AREACHECK=1`). Of those, 529 rooms had control for 30 frames and were checked.
The other 47 ended elsewhere or never gave control (cutscenes, rooms that lead on; see room-smoke.md).

| Result | Rooms |
|---|---|
| exact match | 123 |
| differ only by tile changes and/or animated tiles | 353 (336 with tile changes, 168 with animated tiles) |
| also palette differences, all in palette animations | 51 |
| palette differences outside palette animations | 2 |
| differences from other VRAM | 0 |

Of 59.7 M pixels compared, 2.10 M differ. 1.27 M are tile changes, 0.15 M animated tiles, 0.71 M palettes, 0 other
VRAM.

- All 30 bitmap rooms that were checked match exactly. mapdump cannot draw these rooms, because it draws metatiles,
  so the live side of the check uses their live 8x8 maps instead.
- 06:00 (Mt. Crenel summit) has 163 colours in palettes 2, 3 and 5 to 14 that differ. This is the storm palette
  (group 0x5B), which the weather manager loads (`sub_08059994`) depending on story flags.
- 60:08 (Temple of Droplets element room) has palette 5 cycling through 4 palettes. The Temple of Droplets manager
  does this (`sub_0805AA58`, light rays), not the area's background animations.
- 44:00 (Simon's Simulation) has a bottom BG control that differs from the default in bits that do not change the
  tile layers. It matches exactly.

## Known gaps

The renderer draws each room as it is when it loads. These things are not drawn:

- **Tiles changed at run time**: doors, chests, pots, bushes, switches, puzzle state, and story-dependent tiles such
  as Hyrule Town's second Oracle house (`HyruleTownTileSetManager_BuildSecondOracleHouse`). Special tiles (0x4000 and
  up) in a map are left out, as mapdump leaves them out.
- **Animations**: background tile animations stay on the tile set's own frame, and palette animations on the tile
  set's palette.
- **Story or weather palettes and graphics** that room code or managers load: Mt. Crenel's storm, the Temple of
  Droplets light ray palette, and probably others in rooms the warp check could not reach (47 rooms) or reached in
  only one story state.
- **Other layers**: entities, BG3 backgrounds (sky, clouds, water, parallax), the darkness of rooms with a light
  level, window effects, blending and mosaic.
- **Rooms not in the table**: the 162 room headers the room smoke test leaves out (no tile set, no room properties,
  or a past-the-end table entry). The game never loads these rooms.
- **Region graphics are approximate at region edges**: a pixel gets the groups for a screen centred on it. When the
  player stands somewhere else, the game can show a different group near a region boundary.
