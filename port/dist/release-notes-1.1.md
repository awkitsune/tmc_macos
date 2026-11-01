# The Minish Cap for macOS 1.1

This release adds widescreen. The game can now show more of the world than the GBA's 240x160 screen, so it fills a
wide window or a full-screen Mac display without black bars at the sides.

**You need your own ROM** of The Minish Cap (USA, SHA1 `b4bd50e4131b027c334547b4524e2dbbd4227130`), as with 1.0.
Saves, save states and settings from 1.0 carry over.

## Download

`TheMinishCap-1.1-macos-universal.zip`: one app for Apple silicon and Intel Macs, macOS 12 Monterey or later.
The app is not signed by a registered developer; the first time, open it with right-click → Open (the zip's
`Read me first.txt` has the steps).

## What's new

- **Widescreen.** Settings → Display → View: "GBA (original)" or "Fit to display". Fit to display sizes the view
  from the window: as wide as the window asks, up to 432x160, or taller on screens narrower than 3:2 (up to 224
  lines). It follows the window as you resize it.
  - The game itself draws the wider view: the camera, the room layers, the sprites past the GBA's edge and
    room-to-room scrolling all work at the wider size, and the camera still stops at the room's edges.
  - Screen effects cover the whole view: iris fades, the lantern light in dark rooms, the portal circles.
  - The HUD keeps the GBA's layout in the middle of the screen. Menus and the file screen stay at 240x160 with
    black around them.
  - The GPU renderer ("Draw on the GPU") and the CPU one draw the same picture at every size.
- **iPhone and iPad** get the same setting (Settings → Display). In landscape the view takes the width between
  the touch controls; on iPad it can also grow taller.

## Known differences

- In a few rooms a background picture still repeats at 256 px when the view is wider (some fixed backgrounds, the
  Vaati and Gyorg fights).
- When the game slides the A/B buttons off the top of the screen, a sliver can show for 2–3 frames in views taller
  than 160 lines.
- As in 1.0: every sprite on a scanline is drawn (no sprite flicker in the Mazaal and Vaati fights), and the pause
  menu's Sleep does nothing on the Mac.

## Also in the source: a DSi port

The repository's `native` branch now also builds the game for the Nintendo DSi (DSi mode, from TWiLight Menu++),
with fullscreen 256x192 using the same wider view, sound on the DS's own channels, lid sleep and saves on the SD
card. It is early and built from the source; see `port/dsi/README.md`. It reads your own ROM from the SD card.

## Bugs

If the game crashes or something looks wrong at a wider view, please open an issue with the crash report, the
view size, and if you can a save state from just before (Game → Save State As…).
