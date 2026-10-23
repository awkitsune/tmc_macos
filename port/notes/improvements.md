# Improvements

Things that work today but could be better. Not bugs.

## Keep ASLR on in the app

`StateDisableAslr` (`port/src/state.c`) re-executes the headless runner and the app with address randomization off, so
quick saves load byte for byte into the same build. It uses `_POSIX_SPAWN_DISABLE_ASLR`, a private, undocumented
spawn flag, and it turns off a security feature. The hardened runtime that notarization requires may also refuse it.
If the re-exec fails, the program carries on with ASLR on and states load by symbol.

Better: quick saves record which words are pointers (the exact pointer list that `TMC_STATE_REFERENCE` gives
checkpoints today), so they load exactly at any address. The app then never re-executes itself. Keep the re-exec
only for the test tooling (headless determinism and `checkpoints.sh`), or drop it there too.

## Packaging

- App icon, name and About box.
- Signing, notarization and a `.dmg`. Check the ASLR item above first.

## Input latency with a controller (on hold)

Only keyboard key-to-screen latency was measured (`TMC_LATENCY_LOG`). A Bluetooth pad feels the same as the
keyboard in play, so this waits until it feels worse. Then measure the GameController path.

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
