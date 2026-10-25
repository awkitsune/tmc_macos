# Improvements

Things that work today but could be better. Not bugs.

## Packaging

- About box.
- Signing with a Developer ID, notarization and a `.dmg`. A shared build is ad-hoc signed today, so the
  recipient has to use Open Anyway.

## Late-game checks

Two audit fixes (`native-hazards.md`) have no in-game check yet: Zelda following Link (`npc5.c` ZeldaFollowAction)
and the Golden Octorok. They need save states from that far in.

## PPU: OBJ cycle limit per line

On hardware, sprites late in OAM are dropped on very crowded lines (1210 cycles per line). Here every sprite is
always drawn. Whether TMC ever goes over the limit hasn't been measured (`ppu-audit.md`).

## iPad build

The game core, PPU, sound and Metal shaders carry over. It needs a UIKit shell (fullscreen Metal view, settings),
touch controls, an `AVAudioSession`, `CADisplayLink` pacing and saves in Documents. The iPad mini 7 fits the
picture at 9x (2160x1440 in 2266x1488).
