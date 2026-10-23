#ifndef PORT_MACOS_LATENCY_H
#define PORT_MACOS_LATENCY_H

/*
 * Opt-in input latency and frame timing log (TMC_LATENCY_LOG=path).
 *
 * Every change of the held keys (keyboard or gamepad) is timed from the
 * hardware event to the moment the game thread latches the keys for a frame,
 * and on to the presentation of the first frame that saw them (the drawable's
 * presented time). One CSV line per change; a summary with frame timing
 * statistics (presented intervals, repeated and skipped game frames, game
 * thread frame intervals, display ticks) is appended when the app quits.
 * Everything here is a no-op while the variable is unset.
 */

#include <stdbool.h>
#include <stdint.h>

enum { LATENCY_KEY = 0, LATENCY_PAD = 1, LATENCY_SOURCES };

bool LatencyEnabled(void);
void LatencyStart(void);
void LatencyFinish(void);
// A free-form "# ..." line in the log (the pacing mode, the display rate).
void LatencyNote(const char* text);

// Nanoseconds on the clock of NSEvent timestamps and drawable presented times.
uint64_t LatencyNowNs(void);

// A key or gamepad change that altered the held keys (any thread).
void LatencyInputEvent(int source, uint64_t eventNs);

// Game thread: the keys are being latched; framesToGo is how many LatencyFrameDone
// calls from now (1: the next) publish the first frame that shows them. The game
// reads the latched keys after PlatformVBlank returns, so a latch before the
// frame is drawn is shown two frames on, one at its end by the next.
void LatencyFrameLatched(int framesToGo);
// A game frame has been published as sequence seq, its game-thread work having
// begun at startNs (LatencyNowNs()).
void LatencyFrameDone(uint64_t seq, uint64_t startNs);

// A game frame sequence went onto the screen (any thread). presentedSeconds is
// the drawable's presentedTime (0: the drawable was never shown).
void LatencyPresented(uint64_t seq, double presentedSeconds);

// A display tick reached the game thread, or was dropped because the game
// thread had not yet taken the one before.
void LatencyTick(bool dropped);

#endif // PORT_MACOS_LATENCY_H
