#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/hw.h"
#include "port/ppu.h"

uint8_t gHwIo[HW_IO_SIZE];
uint8_t gHwPltt[HW_PLTT_SIZE];
uint8_t gHwVram[HW_VRAM_SIZE];
uint8_t gHwOam[HW_OAM_SIZE];

struct SoundInfo* gHwSoundInfoPtr;
uint16_t gHwIntrCheck;
void* gHwIntrVector;

// DMAxCNT_H bits (upper half of the 32-bit control word)
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
    uint8_t* destStart; // reloaded after each repeat with CNT_DEST_RELOAD
    uint32_t control;
} HwDmaChannel;

// Timed (non-immediate) transfers, latched for the frame loop to service.
static HwDmaChannel sDmaChannels[4];

// Advance a transfer's addresses past count units the way the DMA unit does.
static intptr_t Step(uint16_t cnt, uint32_t mask, uint32_t dec, uint32_t fixed) {
    int unit = (cnt & CNT_32BIT) ? 4 : 2;
    uint32_t mode = cnt & mask;
    return mode == dec ? -unit : mode == fixed ? 0 : unit;
}

static void RunTransfer(const void* src, void* dest, uint32_t control) {
    uint16_t cnt = control >> 16;
    uint32_t count = control & 0xFFFF;
    int unit = (cnt & CNT_32BIT) ? 4 : 2;
    intptr_t srcStep = unit, destStep = unit;
    const uint8_t* s = src;
    uint8_t* d = dest;

    if (count == 0) {
        count = 0x10000;
    }
    if ((cnt & CNT_SRC_MASK) == CNT_SRC_DEC) {
        srcStep = -unit;
    } else if ((cnt & CNT_SRC_MASK) == CNT_SRC_FIXED) {
        srcStep = 0;
    }
    if ((cnt & CNT_DEST_MASK) == CNT_DEST_DEC) {
        destStep = -unit;
    } else if ((cnt & CNT_DEST_MASK) == CNT_DEST_FIXED) {
        destStep = 0;
    }

    for (; count != 0; count--, s += srcStep, d += destStep) {
        memcpy(d, s, unit);
    }
}

void HwDmaSet(int channel, const void* src, void* dest, uint32_t control) {
    uint16_t cnt = control >> 16;
    static int sTrace = -1;

    if (sTrace < 0) {
        sTrace = getenv("TMC_TRACE_DMA") != NULL;
    }
    if (sTrace) {
        fprintf(stderr, "dma%d %p -> %p ctrl %08x\n", channel, src, dest, control);
    }

    if (!(cnt & CNT_ENABLE)) {
        return;
    }
    if ((cnt & CNT_START_MASK) == 0) {
        RunTransfer(src, dest, control);
        return;
    }
    sDmaChannels[channel] = (HwDmaChannel){ src, dest, dest, control };
}

// Run the channels waiting for one start timing (HBlank or VBlank).
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
        if (ch->dest >= gHwIo && ch->dest < gHwIo + HW_IO_SIZE) {
            // Assumes an incrementing or reloading destination, the only kind used.
            PpuIoWritten(ch->dest - gHwIo, count * ((cnt & CNT_32BIT) ? 4 : 2));
        }
        ch->src += Step(cnt, CNT_SRC_MASK, CNT_SRC_DEC, CNT_SRC_FIXED) * (intptr_t)count;
        if ((cnt & CNT_DEST_MASK) == CNT_DEST_RELOAD) {
            ch->dest = ch->destStart;
        } else {
            ch->dest += Step(cnt, CNT_DEST_MASK, CNT_DEST_DEC, CNT_DEST_FIXED) * (intptr_t)count;
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

void HwDmaStop(int channel) {
    sDmaChannels[channel].control &= ~((uint32_t)(CNT_ENABLE | CNT_START_MASK | CNT_REPEAT) << 16);
}

void HwSystemCall(int number) {
    // Only Stop (3) reaches here today; there is nothing to halt on the host.
    if (number != 3) {
        fprintf(stderr, "HwSystemCall: unhandled BIOS call 0x%02x\n", number);
        abort();
    }
}

typedef struct {
    uint32_t address;
    uint32_t size;
    uint8_t* native;
} HwRamMapEntry;

extern const HwRamMapEntry gHwRamMap[];
extern const uint32_t gHwRamMapCount;

extern const struct mach_header_64 _mh_execute_header;

uint32_t HwRomAddress(uintptr_t pointer) {
    return 0x08000000u + (uint32_t)(pointer - (uintptr_t)&_mh_execute_header);
}

void* HwGbaPtr(uint32_t address) {
    static const struct {
        uint32_t base;
        uint32_t size;
        uint8_t* native;
    } sRegions[] = {
        { 0x04000000, HW_IO_SIZE, gHwIo },
        { 0x05000000, HW_PLTT_SIZE, gHwPltt },
        { 0x06000000, HW_VRAM_SIZE, gHwVram },
        { 0x07000000, HW_OAM_SIZE, gHwOam },
    };
    uint32_t lo = 0, hi = gHwRamMapCount;

    for (size_t i = 0; i < sizeof(sRegions) / sizeof(sRegions[0]); i++) {
        if (address - sRegions[i].base < sRegions[i].size) {
            return sRegions[i].native + (address - sRegions[i].base);
        }
    }
    // Last RAM symbol at or below the address.
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (gHwRamMap[mid].address <= address) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo > 0) {
        const HwRamMapEntry* e = &gHwRamMap[lo - 1];
        if (address - e->address < e->size && e->native != NULL) {
            return e->native + (address - e->address);
        }
    }
    fprintf(stderr, "HwGbaPtr: no native memory for GBA address 0x%08x\n", address);
    abort();
}
