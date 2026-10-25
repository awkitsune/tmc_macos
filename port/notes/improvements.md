# Improvements

Things that work today but could be better. Not bugs.

## Packaging

- Signing with a Developer ID, notarization and a `.dmg`. A shared build is ad-hoc signed today, so the
  recipient has to use Open Anyway.

## Late-game checks

Two audit fixes (`native-hazards.md`) have no in-game check yet: Zelda following Link (`npc5.c` ZeldaFollowAction)
and the Golden Octorok. They need save states from that far in.

## PPU: OBJ cycle limit per line

On hardware, sprites late in OAM are dropped on very crowded lines (1210 cycles per line). Here every sprite is
always drawn. Whether TMC ever goes over the limit hasn't been measured (`ppu-audit.md`).

## iOS app

- TestFlight or the App Store need the paid Apple Developer Program. With a free Apple ID the app is installed from
  Xcode and has to be installed again every 7 days (`port/scripts/ios_install.sh`).
- The touch controls are flat shapes drawn in code. A skin like the GBA's shell (the purple plastic, the speaker
  grille) drawn around them would be a possible later option.
- The touch controls' layout gives the picture a whole-number scale even with Integer scaling off: in landscape the
  middle column is only as wide as that picture.
- The touch controls cannot be moved, resized or made more transparent, and there is no fast-forward button.
- The settings sheet is driven by a controller through its own polling (`port/src/ios/settingsview.m`), not UIKit's
  focus system; a slider moves in 1/20 steps of its range.
- ☰ is both Start and (held) the settings, so holding it to open the settings first gives the game a Start press;
  controllers without a Home button only have that way (or a tap, or the touch controls' ☰).
- No in-app way to change the ROM or re-extract the data after the first launch (the Mac app has Change ROM…); a
  new build that needs other data re-extracts from the remembered ROM by itself.
