#ifndef PORT_APPLE_FRONTEND_H
#define PORT_APPLE_FRONTEND_H

/*
 * Interface between the game thread (game.c) and the AppKit or UIKit / Metal side.
 *
 * The game runs AgbMain on its own thread; every VBlankIntrWait it renders the
 * frame with the software PPU, publishes it, latches the keys and keeps the frame rate
 * (see FrontendPacing). The main thread presents the newest published frame at the display's
 * rate and feeds input.
 */

#include <stdbool.h>
#include <stdint.h>

#include "port/gfxdebug.h"
#include "port/mapview.h"
#include "port/ppu.h"
#include "port/ppucapture.h"

/* The GBA's refresh rate: 16.78 MHz / 280896 cycles per frame. */
#define FRONTEND_FRAME_RATE (16777216.0 / 280896.0)

/*
 * How the game keeps its frame rate. Accurate: its own timer at the GBA's
 * 59.73 Hz. Display sync: one game frame per display refresh (displaysync.h), 60 Hz
 * on a 60 Hz display, about 0.45% fast, with each frame finished just before the
 * refresh that shows it. The game falls back to the timer by itself while the
 * display cannot be followed (a rate that is not a multiple of 60 Hz, a hidden
 * window), and in fast-forward. Takes effect at the next frame.
 */
typedef enum {
    FRONTEND_PACING_ACCURATE = 0,
    FRONTEND_PACING_DISPLAY_SYNC = 1,
} FrontendPacing;
void FrontendSetPacing(FrontendPacing pacing);

/*
 * The game thread calls poll for the keys (HW_KEY_* bits) as the last thing of
 * each frame, right before the game reads them: the keys are as young as they
 * can be. poll runs on the game thread.
 */
void FrontendSetInputPoll(uint16_t (*poll)(void* context), void* context);

/*
 * Called on the game thread after it published a frame that is paced by the
 * display, so that the frame can be drawn at once.
 */
void FrontendSetFrameHandler(void (*handler)(void* context), void* context);

/* Start the game thread. Call once, after FrontendSetSavePath. */
void FrontendStartGame(void);

/* Where the emulated EEPROM is kept (unless TMC_SAVE is already set). */
void FrontendSetSavePath(const char* path);

/*
 * Copy the newest frame (PPU_WIDTH x PPU_HEIGHT, BGRA8) into dest if it is newer
 * than *sequence, updating *sequence. Returns whether a frame was copied. The
 * game frames since the last copy are the change in *sequence; *cut is set when
 * a state load or a reset came in between (the picture does not follow on).
 * If gpuLines is non-NULL, copy matching captured scanlines when available
 * and set *gpuAvailable. A GPU-only frame is not returned to a software-only
 * caller; it waits for the next software-rendered frame.
 */
bool FrontendCopyFrame(uint32_t* dest, uint64_t* sequence, bool* cut, PpuCaptureLine* gpuLines,
                       PpuCompactCapture* compact, bool* gpuAvailable, bool* cpuAvailable);
/* Experimental GPU rendering; the game thread keeps hardware timing on the CPU. */
bool FrontendSetGpuPpuEnabled(bool enabled);
/* Optional compact GPU capture; enabled only while GPU PPU is active. */
bool FrontendSetCompactGpuCapture(bool enabled);
/* Latest published capture size, for the Performance window. */
bool FrontendGpuCaptureStats(size_t* bytes, unsigned* pages, bool* compact);

/* Keys to hold in addition to the polled ones (HW_KEY_* bits), read at the next frame boundary. */
void FrontendSetKeys(uint16_t keys);

void FrontendSetPaused(bool paused);
bool FrontendIsPaused(void);
/*
 * After FrontendSetPaused(true): wait (up to timeoutMs) until the game thread
 * has stopped at its loop boundary, so it writes nothing more (the save) until
 * resumed. True once it has, or if the game has not started.
 */
bool FrontendWaitUntilPaused(int timeoutMs);
void FrontendSetFastForward(bool on);

/* Self-test: the next frame takes this much longer to compute, as a slow one does (an in-game save). */
void FrontendStallNextFrameForTesting(int ms);

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

/*
 * Graphics debug viewers (debugwindows.m). While at least one viewer holds a
 * retain, the game thread copies the hardware into a snapshot as it draws each
 * frame (port/gfxdebug.h); with none, nothing is copied. While paused, a retain
 * takes one snapshot of the hardware as it stands (without per-line registers).
 * FrontendCopyGfxSnapshot copies the newest snapshot into dest if it is newer
 * than *sequence, updating *sequence.
 */
void FrontendGfxRetain(void);
void FrontendGfxRelease(void);
bool FrontendCopyGfxSnapshot(GfxSnapshot* dest, uint64_t* sequence);

/*
 * The Map debug window (port/mapview.h). While it holds a retain, the game
 * thread takes the view of the room every frame (with the room's picture drawn
 * again only when the room or its tiles change); while paused, a retain or a
 * request takes one view as things stand; with none, nothing is taken.
 * FrontendCopyMapState copies the newest state into dest if it is newer than
 * *sequence, FrontendCopyMapImage the room's picture if it is not the one
 * numbered *sequence, updating *sequence.
 *
 * Requests are carried out by the game thread at its next loop boundary, paused
 * or not (a warp then goes on while the game runs, window open or not); what
 * became of them is in the state's status. Returns false if one is still pending.
 */
typedef enum {
    FRONTEND_MAP_REDRAW, // draw the room's picture again (animated tiles)
    FRONTEND_MAP_MOVE,   // move the player to room pixel a, b
    FRONTEND_MAP_WARP,   // warp to area a, room b
} FrontendMapRequest;
void FrontendMapRetain(void);
void FrontendMapRelease(void);
bool FrontendCopyMapState(MapViewState* dest, uint64_t* sequence);
bool FrontendCopyMapImage(MapViewImage* dest, uint64_t* sequence);
bool FrontendRequestMap(FrontendMapRequest request, int a, int b);

/* Stop the game thread at its next frame boundary (so no save write is cut short). */
void FrontendStopGame(void);

#endif // PORT_APPLE_FRONTEND_H
