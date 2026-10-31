# Extended view (widescreen)

Roadmap feature 9 and port/dsi/PLAN.md "Extended view". The game draws more of the world at a larger
view size; nothing is stretched. The view is `PORT_SCREEN_W` x `PORT_SCREEN_H`: 240x160 on the GBA build
(constants) and by default on the native builds (run-time variables). Everything below is a no-op at
240x160, so the playthrough hash and the GBA match hold.

## Inventory: where the game assumes 240x160

GBA asm routines have C versions in `port/src/asm/*.c` on native builds; only those need changing, the
GBA build keeps its asm.

### Camera

| Site | What |
|------|------|
| `src/scroll.c` `Scroll1` | follow the camera target: target - 0x78 / 0x50, clamp to origin .. origin + size - 0xf0 / `DISPLAY_HEIGHT` |
| `src/scroll.c` `sub_080809D4`, `sub_08080974` | place the camera at once (120 / 80 halves) |
| `src/playerUtils.c` ~4228 | camera at room load (0x78 / 0x50 halves) |
| `src/script.c` ~1818 | script camera move, clamped with `DISPLAY_WIDTH/HEIGHT` |
| `src/scroll.c` `UpdateScreenShake` | BG offsets = (scroll - origin) & 15 (+8 down); fine at any size |

Rooms smaller than the view: the GBA has none smaller than 240x160, and the code puts such a room at the
left/top edge. In the extended view a room narrower or shorter than the view is centred (scroll goes
negative relative to the origin) and the BG copy fills the outside with blank entries.

### BG map streaming

- Room layers: the whole room is pre-rendered as 8x8 tiles in `gMapDataBottomSpecial` / `gMapDataTopSpecial`
  (128 entries a row). `UpdateScrollVram` (`port/src/asm/veneer.c`) copies a window of it into
  `gBG1Buffer` / `gBG2Buffer` (32x32), `WaitForNextFrame` DMAs 0x5C0 bytes (23 rows) to the layers' screen
  blocks (28, 29). The window starts at the metatile-aligned scroll, one 8-px row above it; the BG
  offset is `(scroll & 15)` and `(scroll & 15) + 8`.
  - Full copy: `ram_sub_080B197C` (`port/src/asm/intr.c`): 32 columns x 23 rows.
  - Incremental copies during room-to-room scroll transitions: `sub_0807D280`, `sub_0807D46C`, `sub_0807D6D8`
    (`src/screenTileMap.c`). They shift the buffer and add one row/column of the new room; the old room is
    only in the buffer by then, so these can't be replaced with a full copy.
- Width needed: W + 15 px from the aligned left edge (38 columns at 288, 34 at 256). Height: H + 23 px
  (27 rows at 192). So W > 241 needs a 64-column map. GBA BG VRAM is full (screen blocks 28-31 are the
  four layers, char blocks 0-3 cover the rest), so a 512-wide map cannot go in GBA VRAM.
- Decision taken: a port-only wide map per BG (`gHwWideMap`, 64x32 entries, laid out linearly), selected
  by a mask (`gHwWideMapMask`). The software PPU reads it instead of the screen block; the DS presenter
  will put it in DS BG VRAM (64 KB map window via DISPCNT's screen offset, 512x256 size).
- Other BG users that scroll with the camera: BG3 backgrounds by managers (static/animated backgrounds,
  cloud and steam overlays, Minish path backgrounds, light rays, Temple of Droplets, Vaati, Gyorg, POW),
  all with their own 32x32/32x64 buffers. Each needs checking one by one (parallax layers wrap at 256).

### On/off-screen checks

| Site | Size baked in |
|------|---------------|
| `CheckOnScreen` (`port/src/asm/code_08003FC4.c`) | 0x16E = 240 + 2*63, 0x11E = 160 + 2*63 (27 callers: AI wakes, sounds, scripts) |
| `CheckRectOnScreen` (`port/src/asm/code_08001A7C.c`) | `DISPLAY_WIDTH/HEIGHT` |
| `CheckRegionOnScreen` (`src/main.c`) | `DISPLAY_WIDTH/HEIGHT` (used by `CheckRegionsOnScreen`, room tile-set swaps in Hyrule Town / Minish Village) |
| sprite culling `EmitFrameObjs` (`port/src/asm/intr.c`) | y >= 0xA0, x >= 0xF0 |
| `src/npc/guardWithSpear.c`, `src/npc/dampe.c` | own screen tests |
| `src/manager/hyruleTownTileSetManager.c`, `minishVillageTileSetManager.c` | regions vs the screen (through `CheckRegionsOnScreen`) |

No room-entity streaming: room entities are all created at room load, so wider checks only change who wakes
or makes sounds earlier.

### Screen-space effects (windows, HBlank tables)

- GBA window registers are 8-bit: at W > 255 a window can't reach the right edge. Full-screen windows are
  written as 0..0xF0 / 0..0xA0 (`src/scroll.c` Scroll5, `src/cutscene.c`, `src/common.c` 850/870,
  `lightRay.c`, `templeOfDropletsManager.c`, `figurineMenu.c`, `bigGoron.c`).
- Iris / circle windows built per line: `src/common.c` ~930-1030 (160-line loop), `vaatiAppearingManager`,
  `steamOverlayManager`, `lightRayManager`, `pauseMenuScreen6` (0xa0-line loops).
- 160-line HBlank tables only matter for a taller view (DSi 192): Mac/iOS views keep 160 lines.

### Screen-space layout (HUD, text, menus)

- HUD: OAM sprites in GBA screen space (draw mode 2 entities, `ram_DrawDirect`/`gOamCmd`), BG0 for text
  boxes (32x32 map, offset 0, wraps at 256).
- Menus, file select, title, staff roll, map screens: full 240x160 layouts on all four BGs.
- `src/script.c` 1056 / 2124, `rainfallManager`, `cutsceneMiscObject`, `mazaalMacro`, `bombPeahat`,
  `bird.c`, `octorokBossObject`, `gyorgBossObject`: positions relative to the 240x160 screen; fine to leave
  (they place things inside the GBA-sized centre) unless a case looks wrong.

### Native port (ours)

- `port/src/ppu.c`: line buffers and loops at `PPU_WIDTH`; OBJ x wraps at 240; windows 8-bit.
- `port/src/frame.c`: visible lines = `PPU_HEIGHT`, VBlank at that line, 228 lines a frame.
- `port/src/headless.c`: frame buffers, hashes and PNGs at 240x160.
- Metal GPU PPU (`port/src/apple/gpuppu.*`), presenter (`renderer.m`, `game.c`), iOS touch layout
  (`touchlayout.m`), gfx debug windows: 240x160 textures and checks.
- DSi `port/dsi/src/present.c`: 240x160 at (8, 16) with a border window.

## Prototype (Mac headless)

`TMC_VIEW=WxH` (headless) sets the view, 240x160 to 432x224 (`PORT_VIEW_MAX_W/H`).

### How it works

- `gPortScreenW/H` (`port/src/view.c`), `PORT_SCREEN_W/H` in game code (`include/gba/defines.h`; the GBA
  constants on the GBA build). `PORT_VIEW_EXTENDED` is false at 240x160 and every change below is then a
  no-op.
- Camera: `CAMERA_MIN/MAX` and `CameraClamp` (`include/room.h`) at the sites in the table. A room smaller
  than the view is centred (only in the extended view).
- Display extension (`port/include/port/view.h`), set each frame by `PortViewUpdate`
  (`port/src/asm/veneer.c`, called from `WaitForNextFrame` where the GBA DMAs its BG buffers):
  - world view (`TASK_GAME` with room layers, not in a subtask except while the world fades out to it or
    back in): the room layers read `gHwWideMap` (64x32), filled from the
    room's 8x8 maps for the current scroll, 0 outside the room; BG0 (HUD, text) is a centred GBA layer;
    BG3 is drawn across the view with its own scroll; screen-space sprites (UI elements, draw-mode-2
    entities) move by `PORT_HUD_X` = (W - 240) / 2.
  - anything else (title, file select, the subtask menus: pause, maps, kinstones, figurines):
    `gHwGbaScreen`, the 240x160 picture centred with black around it.
  - windows: an edge at 240/160 means the view's edge.
- Culling: `CheckOnScreen`, `CheckRectOnScreen`, `CheckRegionOnScreen`, OBJ emit, PPU OBJ wrap.
- The GBA's own 32x32 copies (`UpdateScrollVram`) are skipped in the extended view.

### Checks

- 240x160: playthrough `23999 89eb5912`, all 24,000 frame CRCs identical to before; room smoke ok 576;
  `tmc.gba: OK`; `make -C port/dsi` links.
- 256x160, 288x160, 432x160: room smoke ok 576 (same reached/arrived counts as 240x160).
- 288x160 playthrough: same route and end (player trace identical). Screenshots every 1000 frames: the
  288 frame's columns 24..263 equal the 240 frame pixel for pixel in 21 of 24 shots (the other: frame 0,
  black, and two where the camera is clamped at a room edge; there the room layers alone match at the
  clamped offset, 0 and 32, and BG0 alone at 24).
- Bedroom (a 240x160 room) at 288: centred, both 24-px side strips backdrop, centre identical.
- Pause menu in and out at 288 (every second frame for 260 frames): centred, black sides, identical to
  240x160 except the 3 frames after the state load (the camera moving from the 240 scroll) and one
  frame (192 px) as the world fades back in.

### Not done yet

- Room-to-room scroll transitions: the incremental copies (`sub_0807D280`, `sub_0807D46C`, `sub_0807D6D8`)
  keep the old room in the BG buffer; with the wide map refilled from the new room each frame the old
  room's side shows backdrop during the slide. They need wide-map versions.
- BG3 users one by one (static pictures wrap at 256 px; overlays are fine).
- Windows narrower than full screen past x 255 (8-bit registers), iris effects.
- Subtasks that are cutscenes in the world (`subtaskAuxCutscene`, portals) are drawn as a GBA screen for now.
- Tile 0 outside the room is assumed blank (true in the rooms checked); a dedicated blank entry may be needed.
- Metal GPU PPU, presenter, iOS touch layout, settings UI; the DSi presenter (wide maps in DS VRAM,
  192 lines, HBlank tables).
