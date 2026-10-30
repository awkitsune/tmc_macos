#ifndef PORT_PPUCAPTURE_H
#define PORT_PPUCAPTURE_H

#include "port/hw.h"
#include "port/ppu.h"

// Experimental GPU reference input. One copy per line preserves writes by HBlank
// DMA and interrupts, including VRAM, palettes and OAM. These large captures are
// for correctness testing, not the eventual fast presentation path.
typedef struct {
    uint8_t vram[HW_VRAM_SIZE];
    uint8_t palette[HW_PLTT_SIZE];
    uint8_t oam[HW_OAM_SIZE];
    uint8_t regs[HW_LINE_REGS + 2]; // pad to a 4-byte boundary
    int32_t refX[2], refY[2], mosaicRefX[2], mosaicRefY[2];
    uint32_t mosaicBgY, mosaicObjY, layerMask;
} PpuCaptureLine;

typedef struct {
    PpuCaptureLine lines[PPU_HEIGHT];
    uint16_t reference[PPU_WIDTH * PPU_HEIGHT];
} PpuCapture;

// Game thread only. NULL disables capturing.
void PpuCaptureTo(PpuCaptureLine* lines);

#endif
