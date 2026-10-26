/*
 * port/platform.h on the DSi: the game's frame boundary and soft reset.
 */

#include <nds.h>
#include <setjmp.h>

#include "port/hw.h"
#include "port/platform.h"

#include "dsi.h"

#define INTR_VBLANK 0x0001

void AgbMain(void);
void HwRaiseInterrupt(uint16_t flag); // port/src/interrupts.c

uint64_t gPlatformFrame;

static jmp_buf sResetPoint;

/*
 * One frame: wait for the DS VBlank, show what the game drew, then do what the
 * GBA does at its VBlank (VBlank DMA, then the interrupt). The per-line
 * VCOUNT and HBlank work (sound timing, HBlank DMA) comes with the presenter.
 */
void PlatformVBlank(void) {
    swiWaitForVBlank();
    DsiPresent();
    HwDmaVBlank();
    HwRaiseInterrupt(INTR_VBLANK);
    gPlatformFrame++;
}

_Noreturn void PlatformSoftReset(void) {
    longjmp(sResetPoint, 1);
}

_Noreturn void DsiRunGame(void) {
    setjmp(sResetPoint);
    AgbMain();
    DsiFatal("AgbMain returned");
}

void DsiPresent(void) {
    // Milestone 5: shadows to engine A (port/dsi/PLAN.md, "Hardware layer").
}
