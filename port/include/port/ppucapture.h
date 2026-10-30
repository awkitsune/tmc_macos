#ifndef PORT_PPUCAPTURE_H
#define PORT_PPUCAPTURE_H

#include "port/hw.h"
#include "port/ppu.h"
#include <stddef.h>
#include <stdbool.h>

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

// A compact snapshot shares unchanged 1 KiB VRAM pages between scanlines.
// The worst case is still bounded by a full copy for every scanline.
#define PPU_CAPTURE_PAGE_SIZE 1024
#define PPU_CAPTURE_PAGES (HW_VRAM_SIZE / PPU_CAPTURE_PAGE_SIZE)
#define PPU_CAPTURE_MAX_PAGES (PPU_HEIGHT * PPU_CAPTURE_PAGES)
typedef struct {
    uint8_t palette[HW_PLTT_SIZE];
    uint8_t oam[HW_OAM_SIZE];
    uint8_t regs[HW_LINE_REGS + 2];
    int32_t refX[2], refY[2], mosaicRefX[2], mosaicRefY[2];
    uint32_t mosaicBgY, mosaicObjY, layerMask;
} PpuCaptureMeta;
typedef struct {
    uint32_t page[PPU_CAPTURE_PAGES];
    PpuCaptureMeta meta;
} PpuCompactLine;
typedef struct {
    PpuCompactLine lines[PPU_HEIGHT];
    uint32_t pageCount;
    uint8_t pages[PPU_CAPTURE_MAX_PAGES * PPU_CAPTURE_PAGE_SIZE];
    // CPU-only scratch after the page pool; never uploaded to the GPU.
    uint8_t previousVram[HW_VRAM_SIZE];
} PpuCompactCapture;

// Game thread only. NULL disables capturing. Starting a compact capture resets it.
void PpuCaptureTo(PpuCaptureLine* lines);
void PpuCompactCaptureTo(PpuCompactCapture* capture);
/* Frame loop only: no interrupt or DMA ran since the preceding line. */
bool PpuCompactCaptureActive(void);
void PpuCompactVramUnchanged(void);
void PpuCompactAppend(PpuCompactCapture* capture, int line, const uint8_t* vram, const PpuCaptureMeta* meta);
void PpuCompactReuse(PpuCompactCapture* capture, int line, const PpuCaptureMeta* meta);
size_t PpuCompactSize(const PpuCompactCapture* capture);

#endif
