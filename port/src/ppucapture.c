#include "port/ppucapture.h"

#include <assert.h>
#include <string.h>

_Static_assert(HW_PLTT_SIZE == PPU_CAPTURE_PAGE_SIZE && HW_OAM_SIZE == PPU_CAPTURE_PAGE_SIZE, "one page each");
_Static_assert(offsetof(PpuCaptureLine, palette) == PPU_CAPTURE_PALETTE_PAGE * PPU_CAPTURE_PAGE_SIZE &&
               offsetof(PpuCaptureLine, oam) == PPU_CAPTURE_OAM_PAGE * PPU_CAPTURE_PAGE_SIZE &&
               offsetof(PpuCaptureLine, regs) == PPU_CAPTURE_PAGES * PPU_CAPTURE_PAGE_SIZE, "capture memory in page order");
_Static_assert(sizeof(PpuCaptureMeta) == sizeof(PpuCaptureLine) - offsetof(PpuCaptureLine, regs), "capture metadata layout");
_Static_assert(offsetof(PpuCompactLine, meta) == PPU_CAPTURE_PAGES * sizeof(uint32_t), "compact line layout");
_Static_assert(PPU_CAPTURE_PAGES == 98 && sizeof(PpuCaptureMeta) == 33 * sizeof(uint32_t), "Metal compact layout");
_Static_assert(offsetof(PpuCompactCapture, pages) == PPU_HEIGHT * sizeof(PpuCompactLine) + sizeof(uint32_t), "Metal page pool offset");

// Captures count pages of one memory region starting at page first, sharing the
// previous line's copy of each page that is unchanged.
static void AppendRegion(PpuCompactCapture* capture, int line, const uint8_t* source, int first, int count) {
    uint32_t* page = capture->lines[line].page;
    uint8_t* previous = capture->previous + first * PPU_CAPTURE_PAGE_SIZE;
    if (line && memcmp(source, previous, (size_t)count * PPU_CAPTURE_PAGE_SIZE) == 0) {
        // The common case is no writes during visible lines. One contiguous
        // comparison avoids a small comparison per page.
        memcpy(&page[first], &capture->lines[line - 1].page[first], count * sizeof(*page));
        return;
    }
    for (int i = 0; i < count; i++, source += PPU_CAPTURE_PAGE_SIZE, previous += PPU_CAPTURE_PAGE_SIZE) {
        if (line && memcmp(source, previous, PPU_CAPTURE_PAGE_SIZE) == 0) {
            page[first + i] = capture->lines[line - 1].page[first + i];
        } else {
            assert(capture->pageCount < PPU_CAPTURE_MAX_PAGES);
            page[first + i] = capture->pageCount++;
            memcpy(capture->pages + page[first + i] * PPU_CAPTURE_PAGE_SIZE, source, PPU_CAPTURE_PAGE_SIZE);
            memcpy(previous, source, PPU_CAPTURE_PAGE_SIZE);
        }
    }
}

void PpuCompactAppend(PpuCompactCapture* capture, int line, const uint8_t* vram, const uint8_t* palette,
                      const uint8_t* oam, const PpuCaptureMeta* meta) {
    AppendRegion(capture, line, vram, 0, PPU_CAPTURE_VRAM_PAGES);
    AppendRegion(capture, line, palette, PPU_CAPTURE_PALETTE_PAGE, 1);
    AppendRegion(capture, line, oam, PPU_CAPTURE_OAM_PAGE, 1);
    memcpy(&capture->lines[line].meta, meta, sizeof(*meta));
}

void PpuCompactReuse(PpuCompactCapture* capture, int line, const PpuCaptureMeta* meta) {
    assert(line > 0);
    memcpy(capture->lines[line].page, capture->lines[line - 1].page, sizeof(capture->lines[line].page));
    memcpy(&capture->lines[line].meta, meta, sizeof(*meta));
}

size_t PpuCompactSize(const PpuCompactCapture* capture) {
    return offsetof(PpuCompactCapture, pages) + (size_t)capture->pageCount * PPU_CAPTURE_PAGE_SIZE;
}
