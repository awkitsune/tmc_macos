/*
 * Interrupt dispatch, replacing IntrMain (asm/src/intr.s), which the game installs
 * as INTR_VECTOR and the BIOS calls on every IRQ.
 */

#include <stdio.h>
#include <stdlib.h>

#include "port/hw.h"

#define REG16(offset) (*(volatile uint16_t*)(gHwIo + (offset)))
#define REG_IE REG16(0x200)
#define REG_IF REG16(0x202)
#define REG_IME REG16(0x208)

#define INTR_VBLANK 0x0001
#define INTR_VCOUNT 0x0004
#define INTR_TIMER3 0x0040
#define INTR_SERIAL 0x0080
#define INTR_GAMEPAK 0x2000

// data/const/interrupts.s: handlers indexed like IntrMain's table offsets.
extern void (*const gUnk_080B2A70[])(void);

void ram_IntrMain(void) {
    uint16_t pending = REG_IE & REG_IF;
    int index;
    uint16_t flag;

    if (pending & INTR_GAMEPAK) {
        fprintf(stderr, "cartridge removed interrupt\n");
        abort();
    } else if (pending & INTR_SERIAL) {
        index = 1, flag = INTR_SERIAL;
    } else if (pending & INTR_VCOUNT) {
        index = 3, flag = INTR_VCOUNT;
    } else if (pending & INTR_VBLANK) {
        index = 4, flag = INTR_VBLANK;
    } else if (pending & INTR_TIMER3) {
        index = 5, flag = INTR_TIMER3;
    } else {
        index = 0, flag = 0;
    }
    REG_IF &= ~flag;
    gUnk_080B2A70[index]();
}

/* Raise an interrupt the way hardware would: flag it and run the vector if enabled. */
void HwRaiseInterrupt(uint16_t flag) {
    REG_IF |= flag;
    if (REG_IME & 1 && REG_IE & flag && gHwIntrVector != NULL) {
        ((void (*)(void))gHwIntrVector)();
    }
}
