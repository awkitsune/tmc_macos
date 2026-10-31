# The Minish Cap for macOS 1.0

The first public build of the native macOS port of The Legend of Zelda: The Minish Cap. It is built from the
[zeldaret/tmc](https://github.com/zeldaret/tmc) decompilation: the game's own code compiled for the Mac, not an
emulator. It has been played from a new game to the end credits.

**You need your own ROM** of The Minish Cap (USA, SHA1 `b4bd50e4131b027c334547b4524e2dbbd4227130`). The app
contains no game data; on first launch it asks for the ROM and extracts what it needs (about 13 MB) into
`~/Library/Application Support/tmc_macos`.

## Download

`TheMinishCap-1.0-macos-universal.zip`: one app for Apple silicon and Intel Macs, macOS 12 Monterey or later.

The app is not signed by a registered developer. The first time, open it with right-click (or Control-click) →
Open, then Open in the alert; on recent macOS, try once, then System Settings → Privacy & Security → Open Anyway.
The zip's `Read me first.txt` has the steps.

## What's in it

- The whole game, with saves in the GBA's own format (`tmc.sav`, an 8 KB EEPROM image).
- Metal rendering, scaled by whole numbers or to the window, with filters: nearest neighbour, sharp and smooth
  bilinear, LCD grid and CRT scanlines, adjustable live in Settings.
- Keyboard and any extended gamepad (Xbox, PlayStation, Switch Pro, MFi), connected at any time.
- Save states: four quick slots (F5 / F7) and any number of files.
- Fast-forward (hold Tab or click the right stick).
- The game's m4a sound engine, optionally mixed at 48 kHz in floating point.
- Display sync and other input-latency options.
- Debug tools (off by default; Settings → Debug): frame stepping and viewers for tiles, sprites, layers, palettes,
  I/O registers and the map.

## Known differences from the GBA

- Every sprite on a scanline is drawn. The GBA drops sprites on very crowded lines, so the flicker in the Mazaal
  and Vaati fights is not there.
- The pause menu's Sleep option does nothing: the screen blanks for a moment and the game carries on.

## iPhone and iPad

The same game also runs on iPhone and iPad (iOS 16 or later) with a controller, a keyboard or on-screen touch
controls. There is no download for it: it is built from the source and installed with a free Apple ID; see
[iPhone and iPad](https://github.com/awkitsune/tmc_macos/tree/macos#iphone-and-ipad) in the README.

## Bugs

The decompiled code sometimes reads or writes memory in ways the GBA tolerates and a 64-bit Mac does not. Many
of these were found and fixed in playtesting, but not every story state has been tried. If the game crashes,
please open an issue with the crash report and, if you can, a save state from just before
(Game → Save State As…).
