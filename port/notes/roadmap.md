# Roadmap after 1.0

Features planned after the first public build, roughly in order. All of them are native only
(`#ifdef PLATFORM_NATIVE` or port code): `tmc.gba` keeps matching, and the GBA save format does not change. Each
one gets its own branch from `macos` and passes the usual checks (`make -j4` → `tmc.gba: OK`, the new-game
playthrough hash, the room smoke test).

## Groundwork

### A. Native side save

The GBA save has exactly two equipped items (`gSave.stats.equipped[2]`) and no room for anything else, and it
must stay the same 8 KB file the GBA uses. Port-only state goes into a small file next to `tmc.sav`
(per save slot): X/Y items, the speedrun timer and splits, in-game achievements, practice-mode markers. If it is
missing or unreadable, everything falls back to defaults, so old saves and saves from a GBA keep working.

### B. Options page in the pause menu

One more page in the pause menu (L/R already flips between the item, quest status and map pages, `src/menu/`),
drawn with the game's own font, window frames and cursor. Those come from the player's ROM at runtime like all
other graphics; only our own text and any new art (drawn by hand, not taken from the ROM) is in the repository.
The page title is a 128x32 banner like the other pages' (two 64x32 sprites at OBJ tiles 0x40 and 0x60, palette 5,
reloaded per page), drawn by hand; the other titles are in `port/design/hud/reference/` (not in git).
Holds the in-game settings: low-health beep, autosave, timer, and later photo mode and achievements. The Mac menu
and the iOS settings sheet keep the same settings in sync.

## Features

| # | Feature | Notes | Effort |
|---|---|---|---|
| 1 | Low-health beep off | `SFX_LOW_HEALTH` is queued in two places in `src/interrupts.c` (the life check). An option to never play it. A respawn with 3 of 16+ hearts is the case that annoys most | Tiny |
| 2 | Select (−) opens the map, Ezlo on left stick click | On a controller, Select/− opens the pause menu straight on the map page, and the left stick click does what Select does on the GBA (talk to Ezlo). Native input layer only (`port/src/apple/input.m`, Mac and iOS) plus a native way to open the menu on a given page; a setting keeps the GBA layout for anyone who wants it. Keyboard and touch keep Select as on the GBA unless the same option is on. Check every place the game reads Select (menus, the file screen, minigames) so none of them loses it. No kinstone button | Small |
| 3 | X and Y item buttons | Two more equip slots (in A), two more HUD buttons next to `UI_ELEMENT_BUTTON_A/B` (`src/ui.c`, positions in `gHUD.buttonX/Y`) with the item icon on them, item use that also reads X/Y. In the item menu, X or Y over an item equips it there. The X/Y button art is drawn by hand on the A/B sprites' palette (OBJ palette 1, 16x16; reference sprites in `port/design/hud/reference/`, not in git). The item code assumes two slots in many places | Medium-large |
| 4 | Practice menu | A Mac menu: warp to any area and room by name (the Map window's room list and `TMC_WARP` do most of it), set and clear story flags by name (from the comments in `include/flags.h`), give items, hearts, rupees. While it is on, the game never writes `tmc.sav` (saving shows a message instead) and the window title says PRACTICE, so a practice session cannot spoil a real save | Medium |
| 5 | Autosave (opt-in) | Off by default. Saves a state when Link enters a new area (an area change, not every room), near where the game itself puts you back after a game over, so it is a safety net and not a free retry of every room. Its own few rotating slots, separate from the player's save states | Small after B |
| 6 | Speedrun timer | Bottom-left in the game's font, off by default. Splits fire on game events (boss beaten, item received, dungeon entered and cleared), each with a short label. A pause-menu page lists the splits and the best times (stored in A) | Medium |
| 7 | Photo mode | Pause, hide the HUD, move the camera within the room, save a PNG of the frame | Medium |
| 8 | Kinstone helper | The game already knows who has a fusion for you (each fusion is a table entry with a flag, which is what the speech bubble uses). Read that and mark the people who still have one on the map. Research first: how the fusion tables tie to NPCs and areas | Medium, research first |
| 9 | In-game achievements | Fired by game events, kept in A, a pause-menu page and a small game-style message when one is earned. No Game Center (it needs a paid developer account and notarization) | Medium after B |
| 10 | Widescreen and other aspect ratios | Every layer drawn wider; sprites past 240 px are culled, the camera clamps to the room at 240 px, the HUD has fixed positions. Prototype in one room first | Hard, last |

Left out: palette filters (not wanted), a kinstone bag button.

## Order

1 and 2 together first (small, and they set up the native settings plumbing), then B, then 5, 6 and 9 on top of
it. 3 once the X/Y art is drawn. 4, 7, 8 in any order. 10 last.
