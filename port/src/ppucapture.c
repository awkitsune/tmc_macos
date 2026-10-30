#include "port/ppucapture.h"

#include <assert.h>
#include <string.h>

_Static_assert(sizeof(PpuCaptureMeta) == sizeof(PpuCaptureLine) - HW_VRAM_SIZE, "capture metadata layout");
_Static_assert(offsetof(PpuCaptureLine, palette) == HW_VRAM_SIZE, "capture metadata offset");
_Static_assert(offsetof(PpuCompactLine, meta) == PPU_CAPTURE_PAGES * sizeof(uint32_t), "compact line layout");
_Static_assert(PPU_CAPTURE_PAGES == 96 && sizeof(PpuCaptureMeta) == 545 * sizeof(uint32_t), "Metal compact layout");
_Static_assert(offsetof(PpuCompactCapture, pages) == PPU_HEIGHT * sizeof(PpuCompactLine) + sizeof(uint32_t), "Metal page pool offset");

void PpuCompactAppend(PpuCompactCapture* capture, int line, const uint8_t* vram, const PpuCaptureMeta* meta) {
    PpuCompactLine* dst = &capture->lines[line];
    if (line && memcmp(vram, capture->previousVram, HW_VRAM_SIZE) == 0) {
        // The common case is no VRAM writes during visible lines. One
        // contiguous comparison avoids 96 small comparisons per scanline.
        memcpy(dst->page, capture->lines[line - 1].page, sizeof(dst->page));
    } else {
        for (int page = 0; page < PPU_CAPTURE_PAGES; page++) {
            const uint8_t* source = vram + page * PPU_CAPTURE_PAGE_SIZE;
            uint8_t* previousBytes = capture->previousVram + page * PPU_CAPTURE_PAGE_SIZE;
            uint32_t previous = line ? capture->lines[line - 1].page[page] : 0;
            if (line && memcmp(source, previousBytes, PPU_CAPTURE_PAGE_SIZE) == 0) {
                dst->page[page] = previous;
            } else {
                assert(capture->pageCount < PPU_CAPTURE_MAX_PAGES);
                dst->page[page] = capture->pageCount++;
                memcpy(capture->pages + dst->page[page] * PPU_CAPTURE_PAGE_SIZE, source, PPU_CAPTURE_PAGE_SIZE);
                memcpy(previousBytes, source, PPU_CAPTURE_PAGE_SIZE);
            }
        }
    }
    memcpy(&dst->meta, meta, sizeof(*meta));
}

void PpuCompactReuse(PpuCompactCapture* capture, int line, const PpuCaptureMeta* meta) {
    assert(line > 0);
    memcpy(capture->lines[line].page, capture->lines[line - 1].page, sizeof(capture->lines[line].page));
    memcpy(&capture->lines[line].meta, meta, sizeof(*meta));
}

size_t PpuCompactSize(const PpuCompactCapture* capture) {
    return offsetof(PpuCompactCapture, pages) + (size_t)capture->pageCount * PPU_CAPTURE_PAGE_SIZE;
}
