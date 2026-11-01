/*
 * The presenter (port/dsi/PLAN.md, "Hardware layer" and "Screen"): the GBA
 * shadows shown by DS engine A on the top screen, the 240x160 picture at
 * (8, 16) of the 256x192 screen.
 *
 * frame.c plays out the GBA frame's lines before the DS VBlank and copies each
 * line's display registers into sLines (gHwLineCapture); the PPU hooks below
 * follow the affine reference points as port/src/ppu.c does. DsiPresentPrepare
 * turns those into one row of DS registers per DS line, and DsiPresent, in the
 * DS VBlank, copies palette, OAM and VRAM, writes line 0's row and starts HBlank
 * DMA for the rows that change between lines. So the DS shows what the Mac's
 * software PPU draws for the same frame, mid-frame changes included.
 *
 * The border: on picture lines a wrapped WIN0 (x 248..7) covers the side
 * columns with no layers; the border lines have every layer and window off.
 * Both show the backdrop, with the game's colour effect. The game's own
 * window goes to WIN1, so the border holds as long as the game has one
 * window on a line (sLineCounts says when it did not).
 *
 * Fullscreen (the extended view, port/notes/widescreen.md): a world frame is
 * 256x192 game lines from (0, 0), and the border is what lies outside the
 * room (gHwViewClip). The room layers' 64x32 wide maps (gHwWideMap) need DS
 * VRAM the GBA layout leaves no room for, so a frame with them moves every BG
 * map to the second 64 KB of bank A (DISPCNT's map base): the GBA screen
 * blocks the frame uses, and the wide maps in blocks 0-7. Menus stay 240x160
 * at (8, 16), as the bordered mode.
 */

#include <nds.h>
#include <string.h>

#include "port/hw.h"
#include "port/ppu.h"
#include "port/ppucapture.h"

#include "dsi.h"

#include "port/view.h"

#define GBA_LINES 160
#define DS_LINES 192
#define TOP 16
#define LEFT 8
#define ROWS (DS_LINES + 1) // DMA writes row n + 1 at line n's HBlank; row 192 is never shown
#define MAP_WINDOW 0x06010000 // bank A's second half: the maps of a frame with wide maps
#define WIDE_BLOCKS 8         // the wide maps' screen blocks there: 64x32 entries, two each
#define ROW_WORDS 20        // DS registers 0x08-0x57

typedef struct {
    uint32_t dispcnt;
    union {
        uint32_t w[ROW_WORDS];
        uint16_t h[ROW_WORDS * 2];
    };
} Row;

#define DS_WIDTH 256
#define H(offset) (((offset) - 0x08) / 2) // halfword index in Row.h
#define W(offset) (((offset) - 0x08) / 4) // word index in Row.w
#define REF_MASK 0x0FFFFFFF               // BGxX/BGxY are 28-bit

static uint8_t sLines[DS_LINES][HW_LINE_REGS];
static Row sRows[ROWS];
static uint32_t sOam[HW_OAM_SIZE / 4] __attribute__((aligned(32)));

// HBlank DMA tables, double-buffered: one is built while the other plays.
static uint32_t sDispcntTable[2][ROWS] __attribute__((aligned(32)));
static uint32_t sRegTable[2][ROWS * ROW_WORDS] __attribute__((aligned(32)));
static struct {
    int buffer;
    int dispcnt;        // DISPCNT changes between lines
    int first, count;   // the register words that do
} sPlan;

DsiPresentCounts gDsiPresentCounts;

// ---- affine reference points, as port/src/ppu.c --------------------------------

static const uint8_t sRefRegs[4] = { 0x28, 0x2C, 0x38, 0x3C }; // BG2X, BG2Y, BG3X, BG3Y
static const uint8_t sRefSteps[4] = { 0x22, 0x26, 0x32, 0x36 }; // PB, PD of BG2, BG3
static int32_t sRef[4];
static int32_t sLineRef[DS_LINES][4];

// The frame being prepared: where its line 0 and column 0 go on the DS screen,
// its lines, the part of the screen it covers (the rest is border), its wide
// BGs, and the GBA screen blocks its maps use (a frame with wide maps copies
// them to MAP_WINDOW).
static struct {
    int left, top, lines;
    int clip[4]; // left, top, right, bottom in DS pixels
    bool world;  // a fullscreen world frame
    uint8_t wide, gbaLayers;
    int gbaLeft, gbaTop;
    uint32_t blocks;
} sGeo;
static bool sMapWindowDirty; // MAP_WINDOW holds maps (a non-wide frame needs it empty)

static uint16_t In16(const uint8_t* io, uint32_t offset) {
    return io[offset] | (io[offset + 1] << 8);
}

static int32_t Reg28(uint32_t offset) {
    uint32_t value = In16(gHwIo, offset) | ((uint32_t)In16(gHwIo, offset + 2) << 16);
    return (int32_t)(value << 4) >> 4;
}

void PpuBeginFrame(void) {
    for (int i = 0; i < 4; i++) {
        sRef[i] = Reg28(sRefRegs[i]);
    }
}

void PpuIoWritten(uint32_t offset, uint32_t size) {
    for (int i = 0; i < 4; i++) {
        if (offset < sRefRegs[i] + 4u && offset + size > sRefRegs[i]) {
            sRef[i] = Reg28(sRefRegs[i]);
        }
    }
}

void PpuRenderLine(int line, uint16_t* out) {
    (void)out;
    memcpy(sLineRef[line], sRef, sizeof(sRef));
    for (int i = 0; i < 4; i++) {
        sRef[i] += (int16_t)In16(gHwIo, sRefSteps[i]);
    }
}

void PpuRenderFrame(uint16_t* framebuffer) {
    (void)framebuffer;
    DsiFatal("PpuRenderFrame: no software PPU on the DSi");
}

// The Mac's GPU line capture; the DSi presents lines without it.
bool PpuCompactCaptureActive(void) {
    return false;
}

void PpuCompactMemoryUnchanged(void) {
}

// ---- one line's registers -------------------------------------------------------

// GBA DISPCNT to DS: the mode (GBA mode 1, two text BGs and affine BG2, is DS
// mode 2 with BG3 off), forced blank, the layer enables and OBJ 1D mapping
// (32-byte steps); windows are added by the caller.
static uint32_t Dispcnt(uint16_t gba) {
    uint32_t ds = 0x10000 | (gba & 0x1F80);

    if (gba & 0x40) {
        ds |= 0x10;
    }
    switch (gba & 7) {
        case 0:
            break;
        case 1:
            ds = (ds | 2) & ~0x800;
            break;
        case 2:
            ds = (ds | 2) & ~0x300;
            break;
        default: // bitmap modes: TMC has none
            ds &= ~0xF00;
            gDsiPresentCounts.bitmapLines++;
            break;
    }
    return ds;
}

typedef struct {
    uint8_t x1, x2; // DS coordinates
    bool wraps;     // x1 > x2: x1..255, then 0..x2 - 1 on the next line
} Span;

// Game window w's pixels on GBA line `line` as port/src/ppu.c finds them, as a
// DS span. False if it has none there.
static bool WindowSpan(const uint8_t* io, int w, int line, Span* span) {
    uint16_t h = In16(io, 0x40 + w * 2), v = In16(io, 0x44 + w * 2);
    int x1 = h >> 8, x2 = h & 0xFF, y1 = v >> 8, y2 = v & 0xFF;
    int width = sGeo.world ? DS_WIDTH : PPU_WIDTH;
    int a, b;
    bool wide = false;

    if (sGeo.world) { // the GBA's screen edge is the view's, as in port/src/ppu.c
        x2 = x2 == PPU_WIDTH ? DS_WIDTH : x2;
        y2 = y2 == PPU_HEIGHT ? DS_LINES : y2;
        if (w == 0 && (In16(io, HW_WIDE_WIN0 + 4) & 1)) { // the line's columns (port/view.h)
            x1 = (int16_t)In16(io, HW_WIDE_WIN0);
            x2 = (int16_t)In16(io, HW_WIDE_WIN0 + 2);
            wide = true;
        }
    }
    if (y1 <= y2 ? (line < y1 || line >= y2) : (line < y1 && line >= y2)) {
        return false;
    }
    span->wraps = false;
    if (wide) {
        a = x1 < 0 ? 0 : x1 > width ? width : x1;
        b = x2 < 0 ? 0 : x2 > width ? width : x2;
    } else if (x1 <= x2) {
        a = x1 < width ? x1 : width;
        b = x2 < width ? x2 : width;
    } else if (x2 >= width) {
        a = 0, b = width;
    } else if (x1 >= width) {
        a = 0, b = x2;
    } else if (x2 == 0) {
        a = x1, b = width;
    } else {
        span->x1 = x1 + sGeo.left;
        span->x2 = x2 + sGeo.left;
        span->wraps = true;
        return true;
    }
    if (a < b && b - a >= DS_WIDTH) {
        // All 256 columns: DS windows end at 255 (counted).
        gDsiPresentCounts.fullWidthLines++;
        b = DS_WIDTH - 1;
    }
    span->x1 = a + sGeo.left;
    span->x2 = b + sGeo.left;
    return a < b;
}

// WIN0 over the columns outside the picture (sGeo.clip): bordered, it wraps
// 248..255, then 0..7. 0 for none.
static uint16_t BorderColumns(void) {
    int left = sGeo.clip[0], right = sGeo.clip[2];

    if (left == 0 && right >= DS_WIDTH) {
        return 0;
    }
    if (right >= DS_WIDTH) {
        return left; // 0..left - 1
    }
    return right << 8 | left; // right..255, then 0..left - 1
}

static bool Contains(const Span* outer, const Span* inner) {
    return !outer->wraps && !inner->wraps && outer->x1 <= inner->x1 && inner->x2 <= outer->x2;
}

// DS windows for a picture line: WIN0 the side columns (no layers, effects on),
// WIN1 the game's window, the OBJ window and outside as the game's.
static uint32_t Windows(const uint8_t* io, int line, Row* row) {
    uint16_t dispcnt = In16(io, 0x00), winin = In16(io, 0x48), winout = In16(io, 0x4A);
    uint8_t outside = winout & 0x3F, inside[2] = { winin & 0x3F, (winin >> 8) & 0x3F };
    Span spans[2];
    bool on[2];
    int w;

    row->h[H(0x40)] = BorderColumns();
    row->h[H(0x44)] = DS_LINES; // lines 0-191
    row->h[H(0x46)] = DS_LINES;
    if ((dispcnt & 0xE000) == 0) {
        row->h[H(0x42)] = 0;
        row->h[H(0x48)] = 0x20;
        row->h[H(0x4A)] = 0x3F;
        return 0x2000;
    }
    for (w = 0; w < 2; w++) {
        on[w] = (dispcnt & (0x2000 << w)) && WindowSpan(io, w, line, &spans[w]);
    }
    if (on[0] && on[1] && Contains(&spans[0], &spans[1])) {
        on[1] = false;
    }
    row->h[H(0x4A)] = winout & 0x3F3F;
    if (on[0] && on[1]) {
        // Two windows and the border need three: the side columns go.
        row->h[H(0x40)] = spans[0].x1 << 8 | spans[0].x2;
        row->h[H(0x42)] = spans[1].x1 << 8 | spans[1].x2;
        row->h[H(0x48)] = inside[0] | inside[1] << 8;
        gDsiPresentCounts.twoWindowLines++;
    } else if (on[0] || on[1]) {
        w = on[1];
        if (!spans[w].wraps) {
            row->h[H(0x42)] = spans[w].x1 << 8 | spans[w].x2;
            row->h[H(0x48)] = 0x20 | inside[w] << 8;
        } else if (!(dispcnt & 0x8000)) {
            // Wrapped: WIN1 takes the gap between the two parts, with the
            // outside's layers, and outside takes the window's.
            row->h[H(0x42)] = spans[w].x2 << 8 | spans[w].x1;
            row->h[H(0x48)] = 0x20 | outside << 8;
            row->h[H(0x4A)] = (winout & 0x3F00) | inside[w];
        } else {
            row->h[H(0x42)] = spans[w].x1 << 8 | spans[w].x2;
            row->h[H(0x48)] = 0x20 | inside[w] << 8;
            gDsiPresentCounts.wrapLines++;
        }
    } else {
        row->h[H(0x42)] = 0; // empty
        row->h[H(0x48)] = 0x20;
    }
    return 0x6000 | (dispcnt & 0x8000);
}

// The GBA screen blocks a BG's map covers (text or affine, by its size).
static uint32_t MapBlocks(uint16_t cnt, bool affine) {
    int base = (cnt >> 8) & 0x1F, size = cnt >> 14;
    int count = affine ? (16 << (size * 2)) * 16 / 0x800 : (size == 0 ? 1 : size == 3 ? 4 : 2);

    count = count < 1 ? 1 : count;
    return (uint32_t)(((1ull << count) - 1) << base);
}

// DS registers for GBA line g, drawn on DS line g + sGeo.top.
static void Translate(int g, Row* row) {
    const uint8_t* io = sLines[g];
    uint16_t dispcnt = In16(io, 0x00);
    int mode = dispcnt & 7;

    for (int bg = 0; bg < 4; bg++) {
        uint16_t cnt = In16(io, 0x08 + bg * 2) & 0xFFCF; // DS char base bits 4-5
        int left = sGeo.left, top = sGeo.top;

        if (sGeo.wide & (1 << bg)) {
            cnt = (cnt & 0x20CF) | (bg * 2) << 8 | 0x4000; // its wide map, 512x256
        } else if (dispcnt & (0x100 << bg)) {
            sGeo.blocks |= MapBlocks(cnt, (mode == 1 && bg == 2) || (mode == 2 && bg >= 2));
        }
        if (sGeo.gbaLayers & (1 << bg)) {
            left += sGeo.gbaLeft, top += sGeo.gbaTop; // a centred GBA layer: the HUD
        }
        row->h[H(0x08) + bg] = cnt;
        row->h[H(0x10) + bg * 2] = (In16(io, 0x10 + bg * 4) - left) & 0x1FF;
        row->h[H(0x12) + bg * 2] = (In16(io, 0x12 + bg * 4) - top) & 0x1FF;
    }
    // Affine BGs: the parameters as they are; the reference point moves to
    // where DS pixel 0 of this line samples.
    for (int bg = 0; bg < 2; bg++) {
        uint32_t base = 0x20 + bg * 0x10;
        int16_t pa = In16(io, base), pc = In16(io, base + 4);

        row->w[W(base)] = In16(io, base) | (uint32_t)In16(io, base + 2) << 16;
        row->w[W(base + 4)] = In16(io, base + 4) | (uint32_t)In16(io, base + 6) << 16;
        row->w[W(base + 8)] = (sLineRef[g][bg * 2] - sGeo.left * pa) & REF_MASK;
        row->w[W(base + 12)] = (sLineRef[g][bg * 2 + 1] - sGeo.left * pc) & REF_MASK;
    }
    row->h[H(0x4C)] = In16(io, 0x4C);
    row->h[H(0x4E)] = 0;
    row->h[H(0x50)] = In16(io, 0x50) & 0x3FFF;
    row->h[H(0x52)] = In16(io, 0x52) & 0x1F1F;
    row->h[H(0x54)] = In16(io, 0x54) & 0x1F;
    row->h[H(0x56)] = 0;
    row->dispcnt = Dispcnt(dispcnt) | Windows(io, g, row) | (sGeo.wide ? 1u << 27 : 0); // maps at MAP_WINDOW
}

// A border line: the nearest picture line's registers, nothing drawn; the
// reference points continue as the hardware steps them.
static void Border(int d, int from) {
    Row* row = &sRows[d];

    *row = sRows[from];
    row->dispcnt &= ~0xFF00;
    for (int bg = 0; bg < 2; bg++) {
        uint32_t base = 0x20 + bg * 0x10;
        int16_t pb = row->h[H(base + 2)], pd = row->h[H(base + 6)];

        row->w[W(base + 8)] = (row->w[W(base + 8)] + (d - from) * pb) & REF_MASK;
        row->w[W(base + 12)] = (row->w[W(base + 12)] + (d - from) * pd) & REF_MASK;
    }
}

// Whether register word `word` must be written at the start of line d: it
// differs from line d - 1's, or, for a reference point, from what the
// hardware's own step after line d - 1 (PB or PD added) gives. Writing a
// reference point reloads it, so one written every line takes the row's value.
static bool Changes(int word, int d) {
    const Row* prev = &sRows[d - 1];
    uint32_t want = sRows[d].w[word];

    switch (word) {
        case W(0x28):
        case W(0x2C):
        case W(0x38):
        case W(0x3C): {
            uint32_t step = word == W(0x28) ? 0x22 : word == W(0x2C) ? 0x26 : word == W(0x38) ? 0x32 : 0x36;
            return ((prev->w[word] + (int16_t)prev->h[H(step)]) & REF_MASK) != want;
        }
        default:
            return prev->w[word] != want;
    }
}

// ---- OAM ------------------------------------------------------------------------

static void TranslateOam(void) {
    const uint16_t* in = (const uint16_t*)gHwOam;
    uint16_t* out = (uint16_t*)sOam;

    for (int i = 0; i < 128; i++, in += 4, out += 4) {
        uint16_t attr0 = in[0], attr1 = in[1];
        int affine = (attr0 >> 8) & 1, mode = (attr0 >> 10) & 3, shape = attr0 >> 14;

        out[2] = in[2];
        out[3] = in[3]; // affine parameters
        if ((!affine && (attr0 & 0x200)) || mode == 3 || shape == 3) {
            out[0] = (attr0 & ~0x300) | 0x200; // hidden (mode 3 is a DS bitmap sprite)
            out[1] = attr1;
            continue;
        }
        // 128-line boxes (double-size 64) that wrap past line 255 while
        // starting on screen show their top too on the DS, not under ppu.c's rule.
        if (affine && (attr0 & 0x200) && (shape == 0 || shape == 2) && (attr1 >> 14) == 3 &&
            (attr0 & 0xFF) < sGeo.lines && (attr0 & 0xFF) + 128 > 256) {
            gDsiPresentCounts.wrappedSprites++;
        }
        out[0] = (attr0 & 0xFF00) | ((attr0 + sGeo.top) & 0xFF);
        out[1] = (attr1 & 0xFE00) | ((attr1 + sGeo.left) & 0x1FF);
    }
}

// ---- per frame ------------------------------------------------------------------

void DsiPresentInit(void) {
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    vramSetBankB(VRAM_B_MAIN_SPRITE_0x06400000);
    vramSetBankD(VRAM_D_LCD); // display capture (DsiCaptureStart)
    // The parts the GBA cannot reach read as transparent there; keep them empty.
    dmaFillWords(0, (void*)0x06000000, 0x20000);
    dmaFillWords(0, (void*)0x06400000, 0x20000);
    REG_DISPCNT = 0x10080; // forced blank until the first frame
    lcdMainOnTop();
    gHwLineCapture = sLines;
}

/*
 * After the frame's lines, before the DS VBlank: the rows for every DS line,
 * the DMA tables for the registers that change between lines, and OAM.
 */
// Where this frame goes (the view it was drawn with, port/view.h).
static void FrameGeometry(void) {
    sGeo.world = PortViewExtended() && !gHwGbaScreen;
    sGeo.lines = PpuLines();
    sGeo.left = sGeo.world ? 0 : LEFT;
    sGeo.top = sGeo.world ? 0 : TOP;
    sGeo.wide = sGeo.world ? gHwWideMapMask : 0;
    sGeo.gbaLayers = sGeo.world ? gHwGbaLayerMask : 0;
    sGeo.gbaLeft = PortViewGbaLeft();
    sGeo.gbaTop = PortViewGbaTop();
    sGeo.blocks = 0;
    if (sGeo.world) {
        for (int i = 0; i < 4; i++) {
            sGeo.clip[i] = gHwViewClip[i];
        }
    } else {
        sGeo.clip[0] = LEFT, sGeo.clip[1] = TOP;
        sGeo.clip[2] = LEFT + PPU_WIDTH, sGeo.clip[3] = TOP + GBA_LINES;
    }
}

void DsiPresentPrepare(void) {
    uint32_t* dispcnt;
    uint32_t* regs;
    int first = ROW_WORDS, last = -1;
    int top, bottom;

    FrameGeometry();
    top = sGeo.clip[1];
    bottom = sGeo.clip[3];
    for (int g = 0; g < sGeo.lines; g++) {
        int d = g + sGeo.top;
        Translate(g, &sRows[d]);
        if (d < top || d >= bottom) {
            sRows[d].dispcnt &= ~0xFF00; // past the room: the backdrop
        }
    }
    if (top >= bottom) { // nothing of the room on screen
        top = sGeo.top, bottom = sGeo.top + 1;
    }
    for (int d = top - 1; d >= 0 && d < sGeo.top; d--) {
        Border(d, top);
    }
    for (int d = sGeo.top + sGeo.lines; d < ROWS; d++) {
        Border(d, sGeo.top + sGeo.lines - 1);
    }
    if (sGeo.wide && (sGeo.blocks & ((1u << WIDE_BLOCKS) - 1))) {
        gDsiPresentCounts.wideMapClashes++; // a GBA map where the wide maps go
    }

    sPlan.buffer ^= 1;
    sPlan.dispcnt = 0;
    for (int d = 1; d < ROWS; d++) {
        sPlan.dispcnt |= sRows[d].dispcnt != sRows[d - 1].dispcnt;
        for (int word = 0; word < ROW_WORDS; word++) {
            if ((word < first || word > last) && Changes(word, d)) {
                first = word < first ? word : first;
                last = word > last ? word : last;
            }
        }
    }
    // An affine BG's reference point is written on every line: the hardware
    // (melonDS at least) steps it only on lines that draw the BG, which the
    // border lines do not, so stepping alone would leave line 16 behind.
    for (int bg = 0; bg < 2; bg++) {
        for (int d = sGeo.top; d < sGeo.top + sGeo.lines; d++) {
            if ((sRows[d].dispcnt & 7) == 2 && (sRows[d].dispcnt & (0x400 << bg))) {
                int x = W(0x28 + bg * 0x10), y = W(0x2C + bg * 0x10);

                first = x < first ? x : first;
                last = y > last ? y : last;
                break;
            }
        }
    }
    sPlan.first = first;
    sPlan.count = last - first + 1;
    dispcnt = sDispcntTable[sPlan.buffer];
    regs = sRegTable[sPlan.buffer];
    for (int d = 0; d < ROWS; d++) {
        dispcnt[d] = sRows[d].dispcnt;
        for (int k = 0; k < sPlan.count; k++) {
            *regs++ = sRows[d].w[first + k];
        }
    }
    if (sPlan.count > gDsiPresentCounts.maxDmaWords) {
        gDsiPresentCounts.maxDmaWords = sPlan.count;
    }
    gDsiPresentCounts.dispcntFrames += sPlan.dispcnt;
    gDsiPresentCounts.regFrames += sPlan.count > 0;
    TranslateOam();
}

#define HBLANK_DMA (DMA_ENABLE | DMA_REPEAT | DMA_START_HBL | DMA_32_BIT | DMA_DST_RESET)

// What the screen shows: the last frame presented. A slow game frame spans
// several DS frames, and each needs line 0's registers and the HBlank DMA
// from row 1 again (the DMA repeats; left alone it would run on past the
// table), so DsiPresentVBlank re-arms them at every DS VBlank.
static struct {
    bool valid, world;
    int buffer, dispcnt, first, count;
    Row top;
} sShown;

static void ArmLines(void) {
    DMA_CR(1) = 0; // done since line 191
    DMA_CR(2) = 0;
    if (!sShown.valid) {
        return;
    }
    REG_DISPCNT = sShown.top.dispcnt;
    for (int word = 0; word < ROW_WORDS; word++) {
        ((vu32*)0x04000008)[word] = sShown.top.w[word];
    }
    if (sShown.dispcnt) {
        DMA_SRC(1) = (uint32_t)&sDispcntTable[sShown.buffer][1];
        DMA_DEST(1) = (uint32_t)&REG_DISPCNT;
        DMA_CR(1) = HBLANK_DMA | 1;
    }
    if (sShown.count > 0) {
        DMA_SRC(2) = (uint32_t)&sRegTable[sShown.buffer][sShown.count];
        DMA_DEST(2) = 0x04000008 + sShown.first * 4;
        DMA_CR(2) = HBLANK_DMA | sShown.count;
    }
}

/*
 * For the frame-time reports: what the shown frame's line 0 gave the DS, per
 * BG its control and scroll (a wide map is size 512x256, 0x4000).
 */
void DsiPresentReport(void) {
    const Row* r = &sShown.top;

    DsiLog("  shown: %s, wide %x, DISPCNT %08lx; BG cnt x,y:", sShown.world ? "world" : "GBA screen",
           (unsigned)gHwWideMapMask, (unsigned long)r->dispcnt);
    for (int bg = 0; bg < 4; bg++) {
        DsiLog(" %04x %u,%u", r->h[H(0x08) + bg], r->h[H(0x10) + bg * 2], r->h[H(0x12) + bg * 2]);
    }
    DsiLog("\n");
}

/* From the VBlank interrupt, every DS frame: the shown frame from line 0 again. */
void DsiPresentVBlank(void) {
    ArmLines();
}

/* In the DS VBlank, after the interrupt: the frame prepared, shown from line 0. */
void DsiPresent(void) {
    DMA_CR(1) = 0;
    DMA_CR(2) = 0;
    DC_FlushAll();
    dmaCopyWords(3, gHwPltt, (void*)0x05000000, HW_PLTT_SIZE);
    dmaCopyWords(3, sOam, (void*)0x07000000, HW_OAM_SIZE);
    dmaCopyWords(3, gHwVram, (void*)0x06000000, 0x10000);
    dmaCopyWords(3, gHwVram + 0x10000, (void*)0x06400000, HW_VRAM_SIZE - 0x10000);
    if (sGeo.wide) {
        for (int block = WIDE_BLOCKS; block < 32; block++) {
            if (sGeo.blocks & (1u << block)) {
                dmaCopyWords(3, gHwVram + block * 0x800, (void*)(MAP_WINDOW + block * 0x800), 0x800);
            }
        }
        for (int bg = 0; bg < 4; bg++) {
            if (sGeo.wide & (1 << bg)) {
                dmaCopyWords(3, gHwWideMap[bg], (void*)(MAP_WINDOW + bg * 0x1000), HW_WIDE_MAP_SIZE * 2);
            }
        }
        sMapWindowDirty = true;
        gDsiPresentCounts.wideFrames++;
    } else if (sMapWindowDirty) {
        // Back to the GBA's layout, where BG tiles past 64 KB read as transparent.
        dmaFillWords(0, (void*)MAP_WINDOW, 0x10000);
        sMapWindowDirty = false;
    }

    REG_IME = 0;
    sShown.valid = true;
    sShown.world = sGeo.world;
    sShown.buffer = sPlan.buffer;
    sShown.dispcnt = sPlan.dispcnt;
    sShown.first = sPlan.first;
    sShown.count = sPlan.count;
    sShown.top = sRows[0];
    ArmLines();
    REG_IME = 1;
}

// ---- picture check --------------------------------------------------------------

#define CAPTURE ((const vu32*)0x06860000) // bank D, LCDC

#ifdef DSI_CAPTURE_DUMP
uint16_t gDsiCaptureDump[256 * DS_LINES];
#endif

/* Capture the next DS frame (the one just presented) into bank D. */
void DsiCaptureStart(void) {
    REG_DISPCAPCNT = DCAP_ENABLE | DCAP_BANK(3) | DCAP_SIZE(3) | DCAP_MODE(DCAP_MODE_A) |
                     DCAP_SRC_A(DCAP_SRC_A_COMPOSITED);
}

/*
 * The captured frame: the CRC of its picture as the Mac's TMC_FRAME_HASH
 * (15-bit colours, little-endian): 240x160, or all 256x192 for a fullscreen
 * world frame; and how many border pixels differ from the top-left one. False
 * if the capture did not finish.
 */
bool DsiCaptureResult(uint32_t frame, DsiPicture* picture) {
    static uint32_t line[DS_WIDTH / 2];
    uint32_t corner = CAPTURE[0] & 0x7FFF, crc = 0, border = 0;
    int left = sShown.world ? 0 : LEFT, top = sShown.world ? 0 : TOP;
    int width = sShown.world ? DS_WIDTH : PPU_WIDTH, lines = sShown.world ? DS_LINES : GBA_LINES;

    if (REG_DISPCAPCNT & DCAP_ENABLE) {
        return false;
    }
    for (int y = 0; y < DS_LINES; y++) {
        const vu32* row = &CAPTURE[y * 128];
        bool picture_line = y >= top && y < top + lines;

        for (int x = 0; x < 128; x++) {
            uint32_t pair = row[x] & 0x7FFF7FFF;

            if (picture_line && x >= left / 2 && x < (left + width) / 2) {
                line[x - left / 2] = pair;
            } else {
                border += (pair & 0xFFFF) != corner;
                border += (pair >> 16) != corner;
            }
        }
        if (picture_line) {
            crc = HwCrc32(crc, line, width * 2);
        }
    }
#ifdef DSI_CAPTURE_DUMP
    if (frame == DSI_CAPTURE_DUMP) {
        for (int i = 0; i < 256 * DS_LINES / 2; i++) {
            ((uint32_t*)gDsiCaptureDump)[i] = CAPTURE[i] & 0x7FFF7FFF;
        }
    }
#else
    (void)frame;
#endif
    picture->crc = crc;
    picture->border = border;
    return true;
}
