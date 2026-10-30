# Improvements

Things that work today but could be better. Not bugs.

## Packaging

- Signing with a Developer ID, notarization and a `.dmg`. A shared build is ad-hoc signed today, so the
  recipient has to use Open Anyway.

## PPU: OBJ cycle limit per line

On hardware, sprites late in OAM are dropped on very crowded lines (1210 cycles per line). Here every sprite is
always drawn. On a GBA the limit shows as flicker (with heavy slowdown) in the Mazaal and transformed Vaati fights;
drawing every sprite there is kept on purpose (`ppu-audit.md`).

## iOS app

- TestFlight or the App Store need the paid Apple Developer Program. With a free Apple ID the app is installed from
  Xcode and has to be installed again every 7 days (`port/scripts/ios_install.sh`).
- The touch controls' Button style draws only the buttons (Plain, Indigo, Outline). The shell around them in the
  prototypes (`port/design/skins/`: the plastic, the speaker grille) is not drawn in the app.
- The settings sheet is driven by a controller through its own polling (`port/src/ios/settingsview.m`), not UIKit's
  focus system (kept: the rows are custom and the polling is small).
- ☰ gives Start on release (so a held ☰ can open the settings without it): a quick press reaches the game a little
  late, and Start cannot be held.
