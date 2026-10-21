/*
 * Headless platform: runs game logic with no window, picture or sound. Used until
 * the Metal front end exists, and for automated runs (TMC_FRAMES=n stops after n
 * frames).
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "port/hw.h"
#include "port/platform.h"

#define REG16(offset) (*(volatile uint16_t*)(gHwIo + (offset)))
#define REG_DISPSTAT REG16(0x004)
#define REG_VCOUNT REG16(0x006)
#define REG_KEYINPUT REG16(0x130)

#define DISPSTAT_VBLANK 0x0001
#define DISPSTAT_VCOUNT 0x0004
#define INTR_VBLANK 0x0001
#define INTR_VCOUNT 0x0004

void AgbMain(void);
void HwRaiseInterrupt(uint16_t flag);

uint64_t gPlatformFrame;
static uint64_t sFrameLimit;
static jmp_buf sResetPoint;

void PlatformVBlank(void) {
    // Mid-frame VCOUNT match (line 80 is where the game asks for it): sound mixing.
    REG_VCOUNT = REG_DISPSTAT >> 8;
    REG_DISPSTAT |= DISPSTAT_VCOUNT;
    HwRaiseInterrupt(INTR_VCOUNT);
    REG_DISPSTAT &= ~DISPSTAT_VCOUNT;

    REG_VCOUNT = 160;
    REG_DISPSTAT |= DISPSTAT_VBLANK;
    HwRaiseInterrupt(INTR_VBLANK);
    REG_DISPSTAT &= ~DISPSTAT_VBLANK;
    REG_VCOUNT = 0;

    gPlatformFrame++;
    if (sFrameLimit != 0 && gPlatformFrame >= sFrameLimit) {
        printf("headless: stopped after %llu frames\n", (unsigned long long)gPlatformFrame);
        exit(0);
    }
}

_Noreturn void PlatformSoftReset(void) {
    longjmp(sResetPoint, 1);
}

int main(void) {
    const char* frames = getenv("TMC_FRAMES");
    sFrameLimit = frames != NULL ? strtoull(frames, NULL, 10) : 0;

    REG_KEYINPUT = 0x03FF; // all buttons released (active low)
    setjmp(sResetPoint);
    AgbMain();
    return 0;
}
