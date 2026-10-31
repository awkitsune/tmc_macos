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
  - world view (`TASK_GAME` with room layers; of the subtasks only the world cutscenes, `SUBTASK_AUXCUTSCENE`,
    `SUBTASK_PORTALCUTSCENE` and `SUBTASK_WORLDEVENT`, and the frames while a menu fades in or out): the
    room layers read `gHwWideMap` (64x32), filled from the
    room's 8x8 maps for the current scroll, 0 outside the room; BG0 (HUD, text) is a centred GBA layer;
    BG3 is drawn across the view with its own scroll; the picture is clipped to the room (`gHwViewClip`,
    backdrop outside, as past the GBA's screen edge; during transitions the union with the room before);
    screen-space sprites (UI elements, draw-mode-2
    entities) move by `PORT_HUD_X` = (W - 240) / 2.
  - anything else (title, file select, the subtask menus: pause, maps, kinstones, figurines):
    `gHwGbaScreen`, the 240x160 picture centred with black around it.
  - windows: an edge at 240/160 means the view's edge.
- Culling: `CheckOnScreen`, `CheckRectOnScreen`, `CheckRegionOnScreen`, OBJ emit, PPU OBJ wrap.
- The GBA's own 32x32 copies (`UpdateScrollVram`) are skipped in the extended view.

### Room transitions

The GBA keeps its BG buffers through a transition and writes only the parts of the new room the
transition has reached: a column or row at a time in the slide (`sub_0807D280`), outward from the
player in the wipe (`sub_0807D46C`), an ellipse around the player for digging caves (`sub_0807D6D8`).
`CopyRoomWindow` does the same in one place: it keeps what each layer showed last frame (`sShown`) with
the world position of its first entry, and each frame takes the new room's tile where
`TransitionReached` says the GBA's copy would have written it, else the tile shown there before (in
world coordinates, which run on across the room change), else 0. Outside transitions every tile is
fresh.

The slide moves the camera 240 or 160 px (60 or 40 frames of 4 px). In a wider view the old room's
camera and the new room's are not always a screen apart, so in the extended view the slide goes from
the one to the other (`StartSlide`/`StepSlide` in `src/scroll.c`), in the same number of frames; the
other axis moves into the new room's range on the way.

### Other layers

- BG3 (and BG1 where a manager takes it over) is drawn across the view with its own scroll. Layers that
  scroll through all 256 px on the GBA (cloud overlay, animated backgrounds) are seamless there already.
- Managers that stream a 32-column window of a wider map into a BG buffer call `PortViewStreamBg`
  (`include/screen.h`) with the source, and the BG reads a 64-column window of it: the horizontal Minish
  paths (12:02, 12:03; BG1 and BG3) and the Minish rafters (2e:00-03; BG1). Found by logging the
  registrations through the room smoke test.
- Pictures narrower than the view in rooms narrower than the view are covered by the room clip.

### Taller views

- HUD and text: the GBA layout is a 240x160 block centred both ways (`PORT_HUD_X/Y`; BG0 as a GBA
  layer is offset and clipped in both directions).
- The game's HBlank tables are 160 lines long. In a taller frame the HBlank DMA stops after line 159, so
  the lines below keep line 159's registers (an iris or a wave effect does not reach them).
- A camera outside the room's range (the view changed size, or a state saved at another size) moves
  back into it at the scroll speed (`Scroll1`, extended view only).

### Menus in taller views

A GBA screen (menus) in a view taller than 160 runs the GBA's 160 lines, as on the GBA (VCOUNT, HBlank
effects and VBlank at line 160), drawn into rows `PpuLineRow()` on; the rows above and below are black
(`PpuLines`, `PpuLineRow`, `PpuClearRows` in `port/include/port/ppu.h`).

### Mac and iOS app

- Setting "View" (`kViewDefault`): "GBA (original)" (240x160) or "Fit to display". macOS: Settings,
  Display, Picture. iOS: Settings, Display, Picture.
- Fit to display (`PortViewFit`): from the picture area's aspect, 3:2 or wider: 160 lines and the width
  to the nearest 16; taller (iPad 4:3, portrait): 240 wide and the height to the nearest 8; within
  240x160..432x224. The renderer asks for it from its picture area on every draw (`FrontendSetView`);
  the game thread takes it right after publishing a frame, so the next frame's logic and picture agree.
- Frames carry their size (`FrontendCopyFrame`); the renderer's textures and LCD response targets
  follow it; the picture is fitted by the frame's aspect (`RendererFitPictureSize`).
- Metal PPU: the capture carries the frame's view (`PpuCaptureView`: size, lines, masks, clip, wide
  maps), bound as buffer 3; layers and sprites are drawn in line coordinates in max-size textures, the
  output texture is the frame's size.
- iOS touch layout: in landscape a fitted view takes all the width between the control columns
  (`TouchLayoutOptions.viewFit`); portrait already gives the picture the top half.

### DSi

`port/dsi/src/present.c`. The Screen option (boot options menu: hold Start at boot, `port/dsi/README.md`;
saved in `tmc/settings.txt`) is Fullscreen by default, the view 256x192; Bordered keeps 240x160 with the
border, drawn as before.

- A world frame is 192 game lines from (0, 0). Menus (GBA screen) are 240x160 at (8, 16) as in the bordered
  mode: the backdrop around them, not black as on the Mac (the DS has no layer left to draw black with).
- The border is what lies outside the room (`gHwViewClip`): rows outside it have every layer and window
  off, columns outside it are WIN0 (no layers, effects on), as the bordered mode's side columns.
- Wide maps: DS BG maps share one 64 KB window (DISPCNT's map base), and the GBA's is full. A frame with
  wide maps moves the window to bank A's second half (0x06010000): the GBA screen blocks the frame uses are
  copied there, and the wide maps go to blocks 0-7 (`wideMapClashes` counts frames with a GBA map there).
  BG tiles past 64 KB, transparent on the GBA, would read those maps in such a frame; the half is cleared
  again when a frame without wide maps follows.
- BG0 (HUD, text) is offset by (8, 16) with no clipping: its map's unused rows and columns show around the
  centred block (empty in the rooms seen on the Mac).
- Game windows: an edge at 240/160 means 256/192; a window over all 256 columns stops at 255 (the DS cannot
  hold 256 in a window register; `fullWidthLines` counts them).
- Verified only by building: melonDS is a GUI app, so the DSi is checked by hand.

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

- Slides at 288x160: in the playthrough's two (house, between 240x160 rooms) all 48 sampled frames equal
  the 240 run at offset 24. 26 slides found by walking off every dungeon room left and right; in 50:13
  (into a 336-wide room, the camera travels 288 px in 4-5 px steps) each sampled mid-slide frame (BG1
  and BG2) equals the last old-room frame and the settled new-room frame put side by side at the room
  boundary, at the expected camera position (0 to 36 of 11,520 sampled pixels differ, as many as between
  the settled frames: animated tiles).

- Streamed layers at 288: each alone (`TMC_LAYER_MASK`) contains the 240 run's picture with 0 differing
  pixels, at its parallax offset (12:03, 2e:01). In 2e:01 (256 wide) the 16-px margins are backdrop.

- 256x192 and 288x160 room smoke ok 576. Hyrule Town at 288x192 (camera at the room's bottom): BG0
  alone equals the 240 run at (24, 16), the room layers alone at (24, 32), with 0 differing pixels.
- iOS app builds (`xcodebuild`, no signing).
- Metal PPU against the software PPU (`gpu_ppu_test`), 0 differing pixels: also Hyrule Town at 288x192
  and 256x192, the bedroom, the intro's iris and the pause menu at 256x192; and the 34 synthetic cases at
  240x160, and game captures: Hyrule Town at 288x160 and 432x224, the bedroom (clipped) at 288x192, the
  pause menu at 288x192 (GBA screen, centred both ways), the Minish path (12:03) and rafters (2e:01) at
  288, a mid-slide frame, the clouds (area 03) and the intro's iris at 288x160 and 240x160.

### Not done yet

- The digging-cave ellipse (`scrollAction` 5) and the wipe (`scrollAction` 4, no setter found in the C
  code) are implemented but not seen running yet.
- BG3 pictures in rooms at least as wide as the view still wrap at 256 px (`staticBackgroundManager`,
  `holeManager`, `lightRayManager`, `templeOfDropletsManager`, Vaati and Gyorg fights). Overlays that
  scroll across all 256 px on the GBA (clouds, animated backgrounds) wrap there too and are seamless.
- Windows narrower than full screen past x 255 (8-bit registers), iris effects.
- Tile 0 outside the room is assumed blank (true in the rooms checked); a dedicated blank entry may be needed.
- The app at other sizes is checked by building only: no GUI runs here. The debug windows (BG viewer's
  screen outline, Map window's camera box) still draw a 240x160 screen.
- DSi: speed (the game draws and copies more), the DSI_CHECKS picture comparison for fullscreen menus
  (the Mac draws black around them, the DSi backdrop).
