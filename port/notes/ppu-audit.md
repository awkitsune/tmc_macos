# PPU audit: mosaic and the rest of `port/src/ppu.c` against GBATEK

## Mosaic (added)

- REG_MOSAIC is read on every line, like the other display registers. BG mosaic
  (BGxCNT bit 6) works on text and affine BGs; OBJ mosaic (attr0 bit 12) works on
  regular, affine and double-size sprites, OBJ window sprites included.
- Blocks are aligned to the screen. Horizontally they start at x = 0.
  Vertically there are two counters, BG and OBJ. Each is reset at line 0, counts
  lines, and restarts after V lines, so a size changed mid-frame takes effect when
  the counter next wraps.
- Text BGs are drawn from the block's first line (with the current scroll), then
  each pixel copies the pixel at the block's left edge. Affine BGs are drawn from
  the reference point latched on the block's first line, so PB/PD stay frozen
  within a block.
- Sprites sample the texel at the screen block's start (column and row). A block
  that starts left of or above the sprite's box is clamped to the box's first
  column or row, as mGBA does. For affine sprites the box is sampled before the
  transform.
- Mosaic is applied to each layer before composition. Windows, priority and
  blending then run per real pixel, so window edges stay sharp and blending mixes
  the blocky layer with whatever is below it.

### Where the game uses it

- `SetFade(... FADE_MOSAIC ...)` (`src/fade.c`) sets mosaic on BG1-BG3. It also
  sets `gOAMControls.spritesOffset = 1`, which is attr0 bit 12 on every sprite the
  game draws (`port/src/asm/intr.c`). Then `sub_08050230` steps REG_MOSAIC
  through 1x1 to 16x16 (out) or back (in). The VBlank handler copies it from
  `gScreen.controls.mosaicSize` (`src/interrupts.c`). BG0 (HUD/text) is never
  mosaicked.
- `TRANSITION_CUT` / `TRANSITION_CUT_FAST` room transitions: the mosaic-out on
  leaving (`CheckRoomExit`) and the mosaic-in on the new room
  (`InitRoomTransition`), both in `src/gameUtils.c`.
  - **Falling into a hole or pit to the floor below** (`DoHoleTransition`,
    `src/manager/holeManager.c`). The `gHoleTransitions` destinations are in
    Deepwood Shrine, Cave of Flames boss, Fortress of Winds, the Hyrule Town minish
    caves, Temple of Droplets, Cloud Tops, Veil Falls top and Palace of Winds.
  - Warps of type BORDER2/AREA2 (`src/scroll.c`). No transition data in `src/data`
    uses those types.
  - npc4E transition 6 (`gNpc4ETransitionTypes`, CUT_FAST) to the Dark Hyrule
    Castle garden.
- **Simon's Simulation** in Hyrule Town (`sub_0806C2A0`, `src/npc/simon.c`, called
  from `script_CutsceneOrchestratorSimonsSimulation.inc`): a mosaic fade-out
  (speed 4) when the simulation ends, and a mosaic fade-in when it starts.
- `verticalMinishPathBackgroundManager.c` turns on mosaic for its parallax BG1/BG3
  (vertical Minish paths). It is only visible while a mosaic fade runs there,
  because REG_MOSAIC is 0 otherwise.

### Checks

- `build/native/ppu_test` (new): text BG, rotated affine BG, regular and affine
  double-size sprites, mosaic inside WIN0 only, mosaic BG alpha-blended over a
  plain BG, an OBJ window sprite with mosaic, and size 1x1 (no change). Each check
  compares every pixel with the plain picture's pixel at the block start. All of
  them pass. Built against the old `ppu.c`, 8 of the 12 fail.
- The 24000-frame playthrough never shows a mosaic fade. All 24000 frame CRCs
  (`TMC_FRAME_HASH`) are identical before and after this change, and the last one
  is still 89eb5912. The Ezlo replay exits 0 and all its frame CRCs are identical
  too.
- In-game check, with a temporary hack that is not committed: `SetFade` ORs in
  FADE_MOSAIC for every non-iris fade.
  - Over 9000 playthrough frames, 1325 pictures differ from the old renderer. All
    of them are in frames where an enabled BG has mosaic set and the size is above
    1x1.
  - The 176 frames where mosaic was on but the picture didn't change: the mosaic
    BGs there are empty, and BG1-3 alone render as a single colour.
  - 250 of those frames were rendered with `TMC_LAYER_MASK=0e` (BG1-3 only, in the
    same run as the register dump). Every HxV block is a single colour: 150
    frames without windows, and 100 WIN0 frames when blocks are split at the
    window edge.
  - `TMC_DUMP_OAM` shows the sprites carrying the M flag during the fade.

## The rest of `ppu.c`

Fixed (no visible change in the playthrough; all its frame CRCs are identical):

- BG tile fetches past 0x10000 (OBJ VRAM) are now transparent, as on hardware.
  Before, char base 3 with tile numbers ≥ 512 (4bpp) or ≥ 256 (8bpp) read sprite
  tiles. Affine BG map reads wrap at 64 KB too.
- 8bpp sprites address `0x10000 + tile * 32` (GBATEK). Before, they used
  `(tile / 2) * 64`, which is wrong for odd tile numbers. The game has no
  ST_OAM_8BPP use, so this has no visible effect.

Checked and correct:

- Modes 0-2: which BGs are text or affine per mode. The game uses only mode 0
  and mode 1: mode 1 for the title screen and the rolling barrel
  (`DISPCNT_MODE_1`). No mode 2-5 constants appear anywhere in `src/`, and the
  register dumps only show modes 0 and 1.
- Text BG 256/512 sizes and screen-block order, flips, 4bpp/8bpp, 9-bit scroll.
- Affine BG: 28-bit reference points latched at VBlank and on write, stepped by
  PB/PD each line, wrap (BGCNT bit 13) or transparent outside, 128-1024 sizes.
- Sprites: Y wrap at 256, X as 9 bits (≥ 240 is off the left edge), affine
  maths around the centre, the double-size box, 1D/2D mapping, hidden flag,
  prohibited shape/mode skipped, flips only on non-affine sprites.
- OBJ priority: the lowest priority value wins, and on equal priority the lower
  OAM index wins (drawn from the back with `<=`). Equal priority with a BG: OBJ
  in front, then the lower BG number.
- Windows: WIN0 > WIN1 > OBJ window > outside. x1 > x2 or y1 > y2 wraps around
  (as mGBA does, not GBATEK's "R=240"). x1 == x2 gives an empty window. The OBJ
  window needs the OBJ layer enabled.
- Semi-transparent OBJ: always 1st target and always alpha, when the layer below
  is a 2nd target. Otherwise BLDCNT's own effect applies if OBJ is a 1st target
  (mGBA behaviour; the window's effect bit doesn't stop the forced alpha).
  The game uses it (`spriteRendering.alphaBlend`, e.g. the Great Fairy, Mazaal).
- Blending: EVA/EVB/EVY clamp at 16, the sum clamps at 31, and brighten/darken
  use I + (31 - I) * EVY / 16 and I - I * EVY / 16. Forced blank is white.
- Per-scanline effects: every register is read per line, and HBlank DMA and
  VCOUNT IRQs run between lines (`frame.c`, `scanline_test`).

Not implemented, and not used by the game:

- Bitmap modes 3-5, and with them the sprite tiles < 512 limit in those modes.
- The OBJ rendering-cycle limit per line (1210 cycles, or 954 with
  DISPCNT bit 5 "HBlank interval free"). On hardware, sprites late in OAM are
  dropped on very crowded lines; here they are always drawn. TMC never sets
  DISPCNT_HBLANK_INTERVAL. Whether a line ever exceeds the limit hasn't been
  measured.
- Green swap (undocumented register 0x02).
- The 2-3 line delay after enabling a BG mid-frame.
- Mosaic edge cases where hardware and emulators differ (a size change mid-block,
  and OBJ mosaic across neighbouring sprites, which hardware latches per pixel of
  the OBJ line buffer). The game only changes REG_MOSAIC at VBlank.
