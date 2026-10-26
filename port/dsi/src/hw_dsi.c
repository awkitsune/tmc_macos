/*
 * port/hw.h on the DSi: the GBA hardware as shadow buffers in main RAM, which
 * the presenter turns into DS display state at VBlank (port/dsi/PLAN.md,
 * "Hardware layer"). The data keeps its GBA layout here, so GBA addresses map
 * by offset: EWRAM is at its own address, IWRAM is gDsiIwram, and the ROM is
 * the copy at __rom_copy.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/hw.h"
#include "port/platform.h"
#include "port/sound.h"

#include "dsi.h"

#define ALIGNED32 __attribute__((aligned(32)))

uint8_t gHwIo[HW_IO_SIZE] ALIGNED32;
uint8_t gHwPltt[HW_PLTT_SIZE] ALIGNED32;
uint8_t gHwVram[HW_VRAM_SIZE] ALIGNED32;
uint8_t gHwOam[HW_OAM_SIZE] ALIGNED32;
uint8_t gDsiIwram[DSI_IWRAM_SIZE] ALIGNED32;

struct SoundInfo* gHwSoundInfoPtr;
uint16_t gHwIntrCheck;
void* gHwIntrVector;


// ---- DMA ----------------------------------------------------------------------
// The same semantics as the Mac's port/src/hw.c: immediate transfers run now,
// timed ones are latched for the presenter.

#define CNT_DEST_MASK 0x0060
#define CNT_DEST_DEC 0x0020
#define CNT_DEST_FIXED 0x0040
#define CNT_DEST_RELOAD 0x0060
#define CNT_SRC_MASK 0x0180
#define CNT_SRC_DEC 0x0080
#define CNT_SRC_FIXED 0x0100
#define CNT_REPEAT 0x0200
#define CNT_32BIT 0x0400
#define CNT_START_MASK 0x3000
#define CNT_START_VBLANK 0x1000
#define CNT_START_HBLANK 0x2000
#define CNT_ENABLE 0x8000

typedef struct {
    const uint8_t* src;
    uint8_t* dest;
    uint8_t* destStart;
    uint32_t control;
} HwDmaChannel;

static HwDmaChannel sDmaChannels[4];

static int Step(uint16_t cnt, uint32_t mask, uint32_t dec, uint32_t fixed) {
    int unit = (cnt & CNT_32BIT) ? 4 : 2;
    uint32_t mode = cnt & mask;
    return mode == dec ? -unit : mode == fixed ? 0 : unit;
}

static void RunTransfer(const void* src, void* dest, uint32_t control) {
    uint16_t cnt = control >> 16;
    uint32_t count = control & 0xFFFF;
    int srcStep = Step(cnt, CNT_SRC_MASK, CNT_SRC_DEC, CNT_SRC_FIXED);
    int destStep = Step(cnt, CNT_DEST_MASK, CNT_DEST_DEC, CNT_DEST_FIXED);
    const uint8_t* s = src;
    uint8_t* d = dest;

    int unit = (cnt & CNT_32BIT) ? 4 : 2;

    if (count == 0) {
        count = 0x10000;
    }
    for (; count != 0; count--, s += srcStep, d += destStep) {
        memcpy(d, s, unit);
    }
}

void HwDmaSet(int channel, const void* src, void* dest, uint32_t control) {
    uint16_t cnt = control >> 16;

    if (!(cnt & CNT_ENABLE)) {
        return;
    }
    if ((cnt & CNT_START_MASK) == 0) {
        RunTransfer(src, dest, control);
        return;
    }
    sDmaChannels[channel] = (HwDmaChannel){ src, dest, dest, control };
}

static int RunTimed(uint16_t start) {
    int ran = 0;

    for (int i = 0; i < 4; i++) {
        HwDmaChannel* ch = &sDmaChannels[i];
        uint16_t cnt = ch->control >> 16;
        uint32_t count = ch->control & 0xFFFF;

        if (!(cnt & CNT_ENABLE) || (cnt & CNT_START_MASK) != start) {
            continue;
        }
        if (count == 0) {
            count = i == 3 ? 0x10000 : 0x4000;
        }
        RunTransfer(ch->src, ch->dest, (ch->control & 0xFFFF0000u) | (count & 0xFFFF));
        ch->src += Step(cnt, CNT_SRC_MASK, CNT_SRC_DEC, CNT_SRC_FIXED) * (int)count;
        if ((cnt & CNT_DEST_MASK) == CNT_DEST_RELOAD) {
            ch->dest = ch->destStart;
        } else {
            ch->dest += Step(cnt, CNT_DEST_MASK, CNT_DEST_DEC, CNT_DEST_FIXED) * (int)count;
        }
        if (!(cnt & CNT_REPEAT)) {
            ch->control &= ~((uint32_t)CNT_ENABLE << 16);
        }
        ran = 1;
    }
    return ran;
}

int HwDmaHBlank(void) {
    return RunTimed(CNT_START_HBLANK);
}

int HwDmaVBlank(void) {
    return RunTimed(CNT_START_VBLANK);
}

void HwDmaInfo(int channel, const void** src, void** dest, uint32_t* control) {
    const HwDmaChannel* ch = &sDmaChannels[channel];

    *src = ch->src;
    *dest = ch->dest;
    *control = (ch->control >> 16) & CNT_ENABLE ? ch->control : 0;
}

void HwDmaStop(int channel) {
    sDmaChannels[channel].control &= ~((uint32_t)(CNT_ENABLE | CNT_START_MASK | CNT_REPEAT) << 16);
}

// ---- addresses ----------------------------------------------------------------

#define EWRAM_START 0x02000000u
#define EWRAM_SIZE 0x40000u
#define IWRAM_START 0x03000000u

static const struct {
    uint32_t base;
    uint32_t size;
    uint8_t* native;
} sRegions[] = {
    { IWRAM_START, DSI_IWRAM_SIZE, gDsiIwram }, { 0x04000000, HW_IO_SIZE, gHwIo },
    { 0x05000000, HW_PLTT_SIZE, gHwPltt },      { 0x06000000, HW_VRAM_SIZE, gHwVram },
    { 0x07000000, HW_OAM_SIZE, gHwOam },
};

void* HwGbaPtr(uint32_t address) {
    if (address - EWRAM_START < EWRAM_SIZE) {
        return (void*)address;
    }
    for (size_t i = 0; i < sizeof(sRegions) / sizeof(sRegions[0]); i++) {
        if (address - sRegions[i].base < sRegions[i].size) {
            return sRegions[i].native + (address - sRegions[i].base);
        }
    }
    if (address - DSI_ROM_BASE < (uint32_t)(__rom_copy_end - __rom_copy)) {
        return __rom_copy + (address - DSI_ROM_BASE);
    }
    DsiFatal("HwGbaPtr: no DSi memory for GBA address 0x%08lx", (unsigned long)address);
}

uint32_t HwGbaAddress(const void* pointer) {
    const uint8_t* p = pointer;

    if ((uintptr_t)p - EWRAM_START < EWRAM_SIZE) {
        return (uintptr_t)p;
    }
    for (size_t i = 0; i < sizeof(sRegions) / sizeof(sRegions[0]); i++) {
        if (p >= sRegions[i].native && p < sRegions[i].native + sRegions[i].size) {
            return sRegions[i].base + (uint32_t)(p - sRegions[i].native);
        }
    }
    if (p >= __rom_copy && p < __rom_copy_end) {
        return DSI_ROM_BASE + (uint32_t)(p - __rom_copy);
    }
    return 0;
}

uint32_t HwRomAddress(uintptr_t pointer) {
    uint32_t address = HwGbaAddress((const void*)pointer);
    return address != 0 ? address : (uint32_t)pointer;
}

/*
 * Song headers and tracks hold raw GBA ROM addresses (they are .incbin'd, so the
 * relocation list cannot reach them); voicegroups are relocated in place, so
 * their pointers are DSi addresses already and pass through.
 */
void* M4aGbaPtr(uint32_t address) {
    if (address - DSI_ROM_BASE < 0x02000000u) {
        if (address - DSI_ROM_BASE < (uint32_t)(__rom_copy_end - __rom_copy)) {
            return __rom_copy + (address - DSI_ROM_BASE);
        }
        return NULL;
    }
    if (address - IWRAM_START < DSI_IWRAM_SIZE) {
        return gDsiIwram + (address - IWRAM_START);
    }
    return (void*)address;
}

void HwSystemCall(int number) {
    // Only Stop (3) reaches here, as on the Mac.
    if (number != 3) {
        DsiFatal("HwSystemCall: unhandled BIOS call 0x%02x", number);
    }
}

int PlatformEntityLimit(void) {
    return -1;
}

void DsiClearGbaRam(void) {
    memset((void*)EWRAM_START, 0, EWRAM_SIZE);
    memset(gDsiIwram, 0, sizeof(gDsiIwram));
}
