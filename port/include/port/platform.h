#ifndef PORT_PLATFORM_H
#define PORT_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

/*
 * What the game sees as the frame boundary. VBlankIntrWait() lands here: the
 * platform raises the interrupts the hardware would (VCOUNT mid-frame for sound,
 * then VBLANK), and later presents the frame, reads input and paces to 59.73 Hz.
 */
void PlatformVBlank(void);

/*
 * The main loop's VBlankIntrWait (WaitForNextFrame, src/interrupts.c). The game
 * also waits in pause loops; only here is nothing but globals live, so this is
 * where save states are taken and restored. PlatformAtLoopBoundary() is true
 * inside the PlatformVBlank this calls.
 */
void PlatformLoopVBlank(void);
bool PlatformAtLoopBoundary(void);

/* BIOS SoftReset: restart the game from AgbMain, keeping EWRAM-like state. */
_Noreturn void PlatformSoftReset(void);

/* Frames since start; the headless platform stops after TMC_FRAMES frames if set. */
extern uint64_t gPlatformFrame;

#endif // PORT_PLATFORM_H
