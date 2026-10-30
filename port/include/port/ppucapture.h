#ifndef PORT_PPUCAPTURE_H
#define PORT_PPUCAPTURE_H

#include "port/hw.h"
#include "port/ppu.h"
#include <stddef.h>
#include <stdbool.h>

// The GPU PPU's input: each line's state as the CPU saw it, which preserves
// writes by HBlank DMA and interrupts to VRAM, palettes and OAM. This full form
// (15 MiB a frame) is the capture file format for gpu_ppu_test and the tests;
// the app captures the compact form below.
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

// The compact form shares unchanged 1 KiB pages of memory between scanlines:
// VRAM's 96 pages, then palette RAM and OAM, one page each (in the order of
// PpuCaptureLine). The worst case is still bounded by a full copy for every
// scanline.
#define PPU_CAPTURE_PAGE_SIZE 1024
#define PPU_CAPTURE_VRAM_PAGES (HW_VRAM_SIZE / PPU_CAPTURE_PAGE_SIZE)
#define PPU_CAPTURE_PALETTE_PAGE PPU_CAPTURE_VRAM_PAGES
#define PPU_CAPTURE_OAM_PAGE (PPU_CAPTURE_VRAM_PAGES + 1)
#define PPU_CAPTURE_PAGES (PPU_CAPTURE_VRAM_PAGES + 2)
#define PPU_CAPTURE_MAX_PAGES (PPU_HEIGHT * PPU_CAPTURE_PAGES)
// A line's state other than memory (PpuCaptureLine from regs on).
typedef struct {
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
    // CPU-only scratch after the page pool, never uploaded to the GPU: the
    // memory as last captured, in page order.
    uint8_t previous[PPU_CAPTURE_PAGES * PPU_CAPTURE_PAGE_SIZE];
} PpuCompactCapture;

// Game thread only. NULL disables capturing. Starting a compact capture resets it.
void PpuCaptureTo(PpuCaptureLine* lines);
void PpuCompactCaptureTo(PpuCompactCapture* capture);
bool PpuCompactCaptureActive(void);
/* Frame loop only: VRAM, palette and OAM cannot have changed since the preceding line. */
void PpuCompactMemoryUnchanged(void);
void PpuCompactAppend(PpuCompactCapture* capture, int line, const uint8_t* vram, const uint8_t* palette,
                      const uint8_t* oam, const PpuCaptureMeta* meta);
void PpuCompactReuse(PpuCompactCapture* capture, int line, const PpuCaptureMeta* meta);
size_t PpuCompactSize(const PpuCompactCapture* capture);

#endif
