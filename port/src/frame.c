/*
 * The hardware side of a frame boundary, shared by the headless runner and the
 * macOS front end: latch the buttons and raise the interrupts the GBA would.
 */

#include "port/hw.h"

#define REG16(offset) (*(volatile uint16_t*)(gHwIo + (offset)))
#define REG_DISPSTAT REG16(0x004)
#define REG_VCOUNT REG16(0x006)
#define REG_KEYINPUT REG16(0x130)

#define DISPSTAT_VBLANK 0x0001
#define DISPSTAT_VCOUNT 0x0004
#define INTR_VBLANK 0x0001
#define INTR_VCOUNT 0x0004

void HwRaiseInterrupt(uint16_t flag);

void HwSetKeys(uint16_t held) {
    REG_KEYINPUT = 0x03FF & ~held; // active low
}

void HwFrameInterrupts(void) {
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
}
