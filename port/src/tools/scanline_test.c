/*
 * Checks of the scanline renderer (frame.c, ppu.c, hw.c) without the game: arm
 * HBlank DMA tables the way the game's VBlank handler does (PerformVBlankDMA in
 * src/interrupts.c) and check every line of the picture against a model.
 *
 *   scroll: per-line BG0HOFS, the heat-haze / light-ray wave (steamOverlayManager)
 *   affine: per-line BG2PA..BG2Y, the rolling barrel (rollingBarrelManager); each
 *           write of BG2X/BG2Y must reload the reference point
 *   window: per-line WIN0H, the iris / spotlight (sub_0801E1EC in src/common.c)
 *   palette: per-line palette RAM writes
 *   oam: per-line sprite attributes and positions
 *   vram: per-line tile graphics writes
 *
 * Prints one line per check and exits nonzero on a mismatch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/hw.h"
#include "port/platform.h"
#include "port/ppu.h"
#include "port/ppucapture.h"

// Stand-ins for what the game build provides.
uint64_t gPlatformFrame;
const struct {
    uint32_t address, size;
    uint8_t* native;
} gHwRamMap[1];
const uint32_t gHwRamMapCount = 0;

#define DMA_HBLANK_REPEAT 0xA2600000u // enable, HBlank, repeat, 16-bit, dest reload

static const uint16_t* sTable;
static uint32_t sTableDest, sTableCount;
static int sTablePalette, sTableOam, sTableVram;
static int sInterruptWrite, sInterruptLine;

// The game's VBlank handler: stop DMA 0, copy line 0's entry, re-arm the table.
static void ArmTable(void) {
    HwDmaStop(0);
    if (sTable != NULL) {
        uint8_t* dest = (sTableVram ? gHwVram : sTableOam ? gHwOam : sTablePalette ? gHwPltt : gHwIo) + sTableDest;
        memcpy(dest, sTable, sTableCount * 2);
        HwDmaSet(0, sTable, dest, DMA_HBLANK_REPEAT | sTableCount);
    }
}

void HwRaiseInterrupt(uint16_t flag) {
    if (flag == 0x0001) {
        ArmTable();
        if (sInterruptWrite) { gHwVram[32] = 0x11; sInterruptLine = 0; }
    } else if (flag == 0x0002 && sInterruptWrite == 1) {
        gHwVram[32] = (sInterruptLine++ % 15 + 1) * 0x11;
    } else if (flag == 0x0004 && sInterruptWrite == 2) {
        gHwVram[32] = 0x22;
    }
}

static void Reg16(uint32_t offset, uint16_t value) {
    gHwIo[offset] = value, gHwIo[offset + 1] = value >> 8;
}

static uint16_t Color(int index) {
    return (uint16_t)(index * 0x0421 + 7); // distinct, non-zero
}

static void ClearHardware(void) {
    sTablePalette = sTableOam = sTableVram = 0;
    sInterruptWrite = sInterruptLine = 0;
    memset(gHwIo, 0, sizeof(gHwIo));
    memset(gHwVram, 0, sizeof(gHwVram));
    memset(gHwPltt, 0, sizeof(gHwPltt));
    memset(gHwOam, 0, sizeof(gHwOam));
    for (int i = 0; i < 128; i++) {
        gHwOam[i * 8 + 1] = 0x02; // OBJ hidden
    }
    for (int i = 0; i < 256; i++) {
        gHwPltt[i * 2] = Color(i) & 0xFF, gHwPltt[i * 2 + 1] = Color(i) >> 8;
    }
    Reg16(0x04, 0x0008); // VBlank interrupt on, no VCOUNT match
}

static PpuCapture* sGpuCapture;
static int sCompactBad;

// Runs two frames (the first arms the table at its VBlank) and keeps the second.
static void RunFrames(uint16_t* frame) {
    ArmTable();
    HwRunFrame(frame);
    sGpuCapture = calloc(1, sizeof(*sGpuCapture));
    PpuCompactCapture* compact = malloc(sizeof(*compact));
    if (!sGpuCapture || !compact) exit(1);
    PpuCaptureTo(sGpuCapture->lines);
    PpuCompactCaptureTo(compact);
    HwRunFrame(getenv("TMC_GPU_CASE_DIR") && getenv("TMC_GPU_CASE_SKIP_CPU") ? NULL : frame);
    PpuCaptureTo(NULL);
    PpuCompactCaptureTo(NULL);
    memcpy(sGpuCapture->reference, frame, sizeof(sGpuCapture->reference));
    sCompactBad = 0;
    for (int line = 0; line < PPU_HEIGHT; line++) {
        PpuCompactLine* packed = &compact->lines[line];
        PpuCaptureLine* full = &sGpuCapture->lines[line];
        sCompactBad += memcmp(&packed->meta, (uint8_t*)full + offsetof(PpuCaptureLine, palette), sizeof(packed->meta)) != 0;
        for (int page = 0; page < PPU_CAPTURE_PAGES; page++) {
            uint32_t index = packed->page[page];
            if (index >= compact->pageCount ||
                memcmp(compact->pages + index * PPU_CAPTURE_PAGE_SIZE,
                       full->vram + page * PPU_CAPTURE_PAGE_SIZE, PPU_CAPTURE_PAGE_SIZE) != 0) sCompactBad++;
        }
    }
    free(compact);
}

// The entry a line is drawn with: line 0 gets the copy made at VBlank, and each
// HBlank DMA after line n-1 copies entry n-1 (the table starts over every frame).
static int EntryForLine(int line) {
    return line == 0 ? 0 : line - 1;
}

static int Check(const char* name, const uint16_t* frame, uint16_t (*expect)(int x, int line)) {
    int bad = 0, firstX = 0, firstLine = 0;
    if (sGpuCapture != NULL) {
        const char* dir = getenv("TMC_GPU_CASE_DIR");
        if (dir != NULL) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/scanline-%s.bin", dir, name);
            FILE* file = fopen(path, "wb");
            if (file == NULL || fwrite(sGpuCapture, sizeof(*sGpuCapture), 1, file) != 1 || fclose(file) != 0) {
                fprintf(stderr, "could not write GPU case %s\n", path);
                exit(1);
            }
        }
        free(sGpuCapture);
        sGpuCapture = NULL;
    }
    if (sCompactBad) fprintf(stderr, "%s: compact capture differs at %d pages or lines\n", name, sCompactBad);

    for (int line = 0; line < PPU_HEIGHT; line++) {
        for (int x = 0; x < PPU_WIDTH; x++) {
            if (frame[line * PPU_WIDTH + x] != expect(x, line) && bad++ == 0) {
                firstX = x, firstLine = line;
            }
        }
    }
    if (bad) {
        printf("%-7s FAIL: %d pixels, first at line %d x %d (got %04x, want %04x)\n", name, bad, firstLine, firstX,
               frame[firstLine * PPU_WIDTH + firstX], expect(firstX, firstLine));
    } else {
        printf("%-7s ok\n", name);
    }
    return bad != 0 || sCompactBad != 0;
}

// ---- scroll ---------------------------------------------------------------------

static uint16_t sScroll[PPU_HEIGHT];

// BG0: 4bpp, tile column c (of 32) shows colour c % 15 + 1.
static void SetupColumns(void) {
    for (int t = 1; t <= 15; t++) {
        memset(&gHwVram[t * 32], t * 0x11, 32);
    }
    for (int row = 0; row < 32; row++) {
        for (int col = 0; col < 32; col++) {
            uint32_t at = 0xF800 + (row * 32 + col) * 2;
            gHwVram[at] = col % 15 + 1;
        }
    }
    Reg16(0x08, 0x1F00); // BG0CNT: char base 0, screen base 31, 256x256
}

static uint16_t ExpectScroll(int x, int line) {
    int bgx = (x + sScroll[EntryForLine(line)]) & 255;
    return Color((bgx >> 3) % 15 + 1);
}

static int TestScroll(uint16_t* frame) {
    ClearHardware();
    SetupColumns();
    Reg16(0x00, 0x0100); // mode 0, BG0
    for (int i = 0; i < PPU_HEIGHT; i++) {
        sScroll[i] = (uint16_t)((i * 7 + (i * i) / 13) & 0x1FF);
    }
    sTable = sScroll, sTableDest = 0x10, sTableCount = 1;
    RunFrames(frame);
    return Check("scroll", frame, ExpectScroll);
}

// ---- affine ---------------------------------------------------------------------

typedef struct {
    int16_t pa, pb, pc, pd;
    int32_t x, y;
} AffineLine; // BG2PA..BG2Y in register order, 8 halfwords

static AffineLine sAffine[PPU_HEIGHT];

static uint16_t ExpectAffine(int x, int line) {
    const AffineLine* a = &sAffine[EntryForLine(line)];
    int bgx = ((a->x + a->pa * x) >> 8) & 127;
    return Color((bgx >> 3) + 1);
}

static int TestAffine(uint16_t* frame) {
    ClearHardware();
    // BG2 in mode 1: 8bpp tiles, 128x128 wrapping map; map column c uses tile c + 1.
    for (int t = 1; t <= 16; t++) {
        memset(&gHwVram[t * 64], t, 64);
    }
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            gHwVram[0xF800 + row * 16 + col] = col + 1;
        }
    }
    Reg16(0x0C, 0x3F00); // BG2CNT: char base 0, screen base 31, wrap, 128x128
    Reg16(0x00, 0x0401); // mode 1, BG2
    for (int i = 0; i < PPU_HEIGHT; i++) {
        // A horizontal squeeze that changes every line, and a PB that would drift
        // the reference point if the per-line BG2X writes did not reload it.
        sAffine[i] = (AffineLine){ (int16_t)(0x80 + i), 0x300, 0, 0x100, (i * 37) << 8, i << 8 };
    }
    sTable = (const uint16_t*)sAffine, sTableDest = 0x20, sTableCount = 8;
    RunFrames(frame);
    return Check("affine", frame, ExpectAffine);
}

// ---- window ---------------------------------------------------------------------

static uint16_t sWindow[PPU_HEIGHT];

static uint16_t ExpectWindow(int x, int line) {
    uint16_t h = sWindow[EntryForLine(line)];
    int inside = x >= (h >> 8) && x < (h & 0xFF);
    return inside ? Color(1) : Color(0); // BG0 inside the window, backdrop outside
}

static int TestWindow(uint16_t* frame) {
    ClearHardware();
    SetupColumns();
    for (int row = 0; row < 32; row++) {
        for (int col = 0; col < 32; col++) {
            gHwVram[0xF800 + (row * 32 + col) * 2] = 1;
        }
    }
    Reg16(0x00, 0x2100); // mode 0, BG0, WIN0
    Reg16(0x44, 0x00A0); // WIN0V: all lines
    Reg16(0x48, 0x0001); // inside: BG0
    Reg16(0x4A, 0x0000); // outside: nothing
    for (int i = 0; i < PPU_HEIGHT; i++) {
        int half = 10 + (i * 3) % 100; // a shape with a different span on every line
        sWindow[i] = (uint16_t)(((120 - half) << 8) | (120 + half));
    }
    sTable = sWindow, sTableDest = 0x40, sTableCount = 1;
    RunFrames(frame);
    return Check("window", frame, ExpectWindow);
}

// A DMA to palette RAM proves the captured memory, not just registers, changes per line.
static uint16_t sPalette[PPU_HEIGHT];
static uint16_t ExpectPalette(int x, int line) {
    (void)x;
    return sPalette[EntryForLine(line)];
}
static int TestPalette(uint16_t* frame) {
    ClearHardware();
    for (int i = 0; i < PPU_HEIGHT; i++) sPalette[i] = Color(i) & 0x7fff;
    sTable = sPalette;
    sTableDest = 0;
    sTableCount = 1;
    sTablePalette = 1;
    RunFrames(frame);
    return Check("palette", frame, ExpectPalette);
}

// Change OBJ0's attributes at each HBlank, including its X and Y positions.
typedef struct { uint16_t attr0, attr1, attr2; } ObjLine;
static ObjLine sObj[PPU_HEIGHT];
static uint16_t ExpectObj(int x, int line) {
    int left = sObj[EntryForLine(line)].attr1;
    return x >= left && x < left + 8 ? Color(1) : Color(0);
}
static int TestObj(uint16_t* frame) {
    ClearHardware();
    memset(&gHwVram[0x10000], 0x11, 32);
    uint16_t color = Color(1);
    gHwPltt[0x202] = color & 255;
    gHwPltt[0x203] = color >> 8;
    Reg16(0x00, 0x1040); // OBJ with 1D mapping
    for (int i = 0; i < PPU_HEIGHT; i++) sObj[i] = (ObjLine){ i, (i * 3) % 232, 0 };
    sTable = (const uint16_t*)sObj;
    sTableDest = 0;
    sTableCount = 3;
    sTableOam = 1;
    RunFrames(frame);
    return Check("oam", frame, ExpectObj);
}

// Repeated HBlank DMA writes into tile data, not just display registers.
static uint16_t sTileData[PPU_HEIGHT];
static uint16_t ExpectVram(int x, int line) {
    return (line % 8 == 0 && x % 8 < 4) ? Color(EntryForLine(line) % 15 + 1) : Color(0);
}
static int TestVram(uint16_t* frame) {
    ClearHardware();
    for (int i = 0; i < 32 * 32; i++) gHwVram[0xF800 + i * 2] = 1;
    Reg16(0x08, 0x1F00);
    Reg16(0x00, 0x0100);
    for (int i = 0; i < PPU_HEIGHT; i++) sTileData[i] = (i % 15 + 1) * 0x1111;
    sTable = sTileData;
    sTableDest = 32;
    sTableCount = 1;
    sTableVram = 1;
    RunFrames(frame);
    return Check("vram", frame, ExpectVram);
}

static uint16_t ExpectStable(int x, int line) { (void)x; (void)line; return Color(0); }
static int TestStable(uint16_t* frame) {
    ClearHardware();
    sTable = NULL;
    RunFrames(frame);
    return Check("stable", frame, ExpectStable);
}

static uint16_t ExpectInterrupt(int x, int line) {
    int index = sInterruptWrite == 1 ? EntryForLine(line) % 15 + 1 : (line >= 80 ? 2 : 1);
    return line % 8 == 0 && x % 8 < 2 ? Color(index) : Color(0);
}
static int TestInterrupt(uint16_t* frame, int mode) {
    ClearHardware();
    for (int i = 0; i < 32 * 32; i++) gHwVram[0xF800 + i * 2] = 1;
    gHwVram[32] = 0x11;
    Reg16(0x08, 0x1F00);
    Reg16(0x00, 0x0100);
    Reg16(0x04, mode == 1 ? 0x0018 : (80 << 8) | 0x0028);
    sTable = NULL;
    sInterruptWrite = mode;
    RunFrames(frame);
    int failed = Check(mode == 1 ? "hblank-irq" : "vcount-irq", frame, ExpectInterrupt);
    sInterruptWrite = 0;
    return failed;
}

int main(void) {
    static uint16_t frame[PPU_WIDTH * PPU_HEIGHT];
    int failed = 0;

    failed |= TestStable(frame);
    failed |= TestScroll(frame);
    failed |= TestAffine(frame);
    failed |= TestWindow(frame);
    failed |= TestPalette(frame);
    failed |= TestObj(frame);
    failed |= TestVram(frame);
    failed |= TestInterrupt(frame, 1);
    failed |= TestInterrupt(frame, 2);
    return failed;
}
