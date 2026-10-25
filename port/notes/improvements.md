# Improvements

Things that work today but could be better. Not bugs.

## `HwRomAddress` depends on the build's layout

`LoadRoomEntity` (`src/room.c`) keeps a cutscene script pointer as 32-bit data too, as the GBA did, through
`HwRomAddress`: 0x08000000 plus its offset in the program. That is the same at every load address, but not in another
build: a rebuild that moves the game's data gives entities created after a state load other values than the build
that saved it, so state checksums differ between builds from then on (the pictures do not; nothing seen reads the
value back as a pointer). Better: the script's real GBA address, from the GBA link (datagen knows it).

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
