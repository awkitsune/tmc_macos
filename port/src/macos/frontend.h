#ifndef PORT_MACOS_FRONTEND_H
#define PORT_MACOS_FRONTEND_H

/*
 * Interface between the game thread (game.c) and the AppKit/Metal side.
 *
 * The game runs AgbMain on its own thread; every VBlankIntrWait it renders the
 * frame with the software PPU, publishes it, latches the keys and sleeps to keep
 * 59.73 Hz. The main thread presents the newest published frame at the display's
 * rate and feeds input.
 */

#include <stdbool.h>
#include <stdint.h>

#include "port/ppu.h"

/* The GBA's refresh rate: 16.78 MHz / 280896 cycles per frame. */
#define FRONTEND_FRAME_RATE (16777216.0 / 280896.0)

/* Start the game thread. Call once, after FrontendSetSavePath. */
void FrontendStartGame(void);

/* Where the emulated EEPROM is kept (unless TMC_SAVE is already set). */
void FrontendSetSavePath(const char* path);

/*
 * Copy the newest frame (PPU_WIDTH x PPU_HEIGHT, BGRA8) into dest if it is newer
 * than *sequence, updating *sequence. Returns whether a frame was copied. The
 * game frames since the last copy are the change in *sequence; *cut is set when
 * a state load or a reset came in between (the picture does not follow on).
 */
bool FrontendCopyFrame(uint32_t* dest, uint64_t* sequence, bool* cut);

/* Keys held by the player (HW_KEY_* bits), read at the next frame boundary. */
void FrontendSetKeys(uint16_t keys);

void FrontendSetPaused(bool paused);
bool FrontendIsPaused(void);
void FrontendSetFastForward(bool on);

/* Reset the game the way the game itself does (A+B+Select+Start). */
void FrontendRequestReset(void);

/* While paused: run to the next frame boundary (one frame), then pause again. */
void FrontendStepFrame(void);

/* The frame counter (frames since start, or as a loaded state set it). */
uint64_t FrontendFrame(void);

/*
 * Save states (port/include/port/state.h): saved or loaded by the game thread
 * at its next main-loop frame boundary, paused or not. done (may be NULL) is
 * called on the game thread with the outcome, a message (why it failed, or a
 * note on a load from another build) and the frame. Returns false if another
 * request is still pending.
 */
typedef void (*FrontendStateDone)(bool ok, const char* message, uint64_t frame, void* context);
bool FrontendRequestState(bool save, const char* path, FrontendStateDone done, void* context);

/* Stop the game thread at its next frame boundary (so no save write is cut short). */
void FrontendStopGame(void);

#endif // PORT_MACOS_FRONTEND_H
