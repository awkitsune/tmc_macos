/*
 * Checks of the picture processor's mosaic (ppu.c) without the game. Each check
 * draws a scene twice, without and with mosaic, and expects every pixel of the
 * mosaic picture to be the plain picture's pixel at the start of its block:
 *
 *   BGs:  the block is aligned to the screen, (x - x % H, y - y % V).
 *   OBJs: the same screen-aligned block, clamped to the sprite's own box (a
 *         block that starts left of or above the sprite takes its first column
 *         or row), so a sprite never shows pixels from outside itself.
 *
 *   text      BG0 text, scrolled, 4bpp
 *   affine    BG2 rotated in mode 1 (the reference point steps per line)
 *   size1     mosaic bits set with REG_MOSAIC = 0 changes nothing
 *   obj       a regular sprite at an unaligned position, flipped
 *   objaff    an affine, double-size sprite
 *   window    BG0 with mosaic inside WIN0 only: the window edge stays sharp
 *   blend     mosaic BG0 alpha-blended over plain BG1
 *   objwin    an OBJ window sprite with mosaic shapes the window in blocks
 *   obj8bpp   an 8bpp sprite on an odd tile number reads from tile * 32
 *   bgobjvram BG tiles past 0x10000 (OBJ VRAM) are transparent
 *
 * Prints one line per check and exits nonzero on a mismatch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/hw.h"
#include "port/platform.h"
#include "port/ppu.h"
#include "port/performance.h"
#include "port/ppucapture.h"

// Stand-ins for what the game build provides.
uint64_t gPlatformFrame;
const struct {
    uint32_t address, size;
    uint8_t* native;
} gHwRamMap[1];
const uint32_t gHwRamMapCount = 0;

void HwRaiseInterrupt(uint16_t flag) {
    (void)flag;
}

static uint16_t sPlain[PPU_WIDTH * PPU_HEIGHT], sMosaic[PPU_WIDTH * PPU_HEIGHT];

static void Reg16(uint32_t offset, uint16_t value) {
    gHwIo[offset] = value, gHwIo[offset + 1] = value >> 8;
}

static uint16_t GetReg16(uint32_t offset) {
    return gHwIo[offset] | (gHwIo[offset + 1] << 8);
}

static uint16_t Color(int index) {
    return (uint16_t)((index * 0x1C47 + 0x0421) & 0x7FFF); // distinct for 0-511
}

static void ClearHardware(void) {
    memset(gHwIo, 0, sizeof(gHwIo));
    memset(gHwVram, 0, sizeof(gHwVram));
    memset(gHwPltt, 0, sizeof(gHwPltt));
    memset(gHwOam, 0, sizeof(gHwOam));
    for (int i = 0; i < 128; i++) {
        gHwOam[i * 8 + 1] = 0x02; // OBJ hidden
    }
    for (int i = 0; i < 512; i++) {
        gHwPltt[i * 2] = Color(i) & 0xFF, gHwPltt[i * 2 + 1] = Color(i) >> 8;
    }
}

// 4bpp tiles 1..63 at charBase whose pixels all differ within a tile and between
// neighbouring tiles; a map at screenBase (32x32) using them with flips.
static void SetupTextBg(uint32_t charBase, uint32_t screenBase, int salt) {
    for (int t = 1; t < 64; t++) {
        for (int i = 0; i < 32; i++) {
            int a = (t * 7 + i * 2 + salt) % 15 + 1, b = (t * 3 + i * 2 + 1 + salt) % 15 + 1;
            gHwVram[charBase + t * 32 + i] = a | (b << 4);
        }
    }
    for (int i = 0; i < 32 * 32; i++) {
        uint16_t entry = (i * 5 + salt) % 63 + 1;
        entry |= (i % 3 == 0) ? 0x400 : 0;
        entry |= (i % 7 == 0) ? 0x800 : 0;
        entry |= ((i + salt) % 16) << 12;
        gHwVram[screenBase + i * 2] = entry & 0xFF, gHwVram[screenBase + i * 2 + 1] = entry >> 8;
    }
}

// A 32x32 4bpp sprite (1D mapping) at OBJ tile 0 with every pixel set.
static void SetupObjTiles(void) {
    for (int i = 0; i < 16 * 32; i++) {
        int a = (i * 3) % 15 + 1, b = (i * 5 + 2) % 15 + 1;
        gHwVram[0x10000 + i] = a | (b << 4);
    }
}

static void SetObj(int index, uint16_t attr0, uint16_t attr1, uint16_t attr2) {
    uint8_t* oam = &gHwOam[index * 8];
    oam[0] = attr0, oam[1] = attr0 >> 8, oam[2] = attr1, oam[3] = attr1 >> 8, oam[4] = attr2, oam[5] = attr2 >> 8;
}

static void SetAffine(int group, int16_t pa, int16_t pb, int16_t pc, int16_t pd) {
    int16_t p[4] = { pa, pb, pc, pd };
    for (int i = 0; i < 4; i++) {
        gHwOam[group * 32 + i * 8 + 6] = p[i] & 0xFF, gHwOam[group * 32 + i * 8 + 7] = (uint16_t)p[i] >> 8;
    }
}

static int Report(const char* name, int bad, int x, int y, uint16_t got, uint16_t want) {
    const char* dir = getenv("TMC_GPU_CASE_DIR");
    if (dir != NULL) {
        PpuCapture* capture = calloc(1, sizeof(*capture));
        if (capture == NULL) exit(1);
        if (getenv("TMC_GPU_CASE_SKIP_CPU")) {
            PpuRenderFrame(capture->reference);
            PpuCaptureTo(capture->lines);
            PpuBeginFrame();
            for (int line = 0; line < PPU_HEIGHT; line++) PpuRenderLine(line, NULL);
        } else {
            PpuCaptureTo(capture->lines);
            PpuRenderFrame(capture->reference);
        }
        PpuCaptureTo(NULL);
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.bin", dir, name);
        FILE* file = fopen(path, "wb");
        if (file == NULL || fwrite(capture, sizeof(*capture), 1, file) != 1 || fclose(file) != 0) {
            fprintf(stderr, "could not write GPU case %s\n", path);
            exit(1);
        }
        free(capture);
    }
    if (bad) {
        printf("%-9s FAIL: %d pixels, first at line %d x %d (got %04x, want %04x)\n", name, bad, y, x, got, want);
    } else {
        printf("%-9s ok\n", name);
    }
    return bad != 0;
}

// Expected: the plain picture at the block start, clamped to [left, right) x [top, bottom).
typedef struct {
    int h, v;
    int left, top, right, bottom; // the mosaic layer's extent (the whole screen for BGs)
} Blocks;

static int CheckBlocks(const char* name, const Blocks* b) {
    int bad = 0, fx = 0, fy = 0;
    uint16_t got = 0, want = 0;

    for (int y = 0; y < PPU_HEIGHT; y++) {
        for (int x = 0; x < PPU_WIDTH; x++) {
            int inside = x >= b->left && x < b->right && y >= b->top && y < b->bottom;
            int sx = x - x % b->h, sy = y - y % b->v;
            uint16_t expect;

            sx = sx < b->left ? b->left : sx;
            sy = sy < b->top ? b->top : sy;
            expect = inside ? sPlain[sy * PPU_WIDTH + sx] : sPlain[y * PPU_WIDTH + x];
            if (sMosaic[y * PPU_WIDTH + x] != expect && bad++ == 0) {
                fx = x, fy = y, got = sMosaic[y * PPU_WIDTH + x], want = expect;
            }
        }
    }
    return Report(name, bad, fx, fy, got, want);
}

// Renders plain, then with the given BGxCNT bits / OAM attr0 bit set and REG_MOSAIC.
static void RenderBoth(uint16_t mosaic, unsigned bgMask, unsigned objMask) {
    PpuRenderFrame(sPlain);
    for (int bg = 0; bg < 4; bg++) {
        if (bgMask & (1 << bg)) {
            Reg16(0x08 + bg * 2, GetReg16(0x08 + bg * 2) | 0x40);
        }
    }
    for (int i = 0; i < 128; i++) {
        if (objMask & (1u << (i & 31))) {
            gHwOam[i * 8 + 1] |= 0x10;
        }
    }
    Reg16(0x4C, mosaic);
    PpuRenderFrame(sMosaic);
}

static const Blocks kScreen = { 0, 0, 0, 0, PPU_WIDTH, PPU_HEIGHT };

static int TestText(void) {
    Blocks b = kScreen;
    ClearHardware();
    SetupTextBg(0x0000, 0xF800, 0);
    Reg16(0x00, 0x0100); // mode 0, BG0
    Reg16(0x08, 0x1F00); // BG0CNT: char 0, screen 31, 4bpp 256x256
    Reg16(0x10, 13), Reg16(0x12, 250);
    RenderBoth(0x0053, 1, 0); // BG 4x6
    b.h = 4, b.v = 6;
    return CheckBlocks("text", &b);
}

static int TestAffine(void) {
    Blocks b = kScreen;
    ClearHardware();
    // 8bpp tiles 1..64 with distinct pixels, a 256x256 wrapping map.
    for (int t = 1; t <= 64; t++) {
        for (int i = 0; i < 64; i++) {
            gHwVram[t * 64 + i] = (t * 11 + i * 3) % 255 + 1;
        }
    }
    for (int i = 0; i < 32 * 32; i++) {
        gHwVram[0xE000 + i] = i % 64 + 1;
    }
    Reg16(0x00, 0x0401);             // mode 1, BG2
    Reg16(0x0C, 0x7C00);             // BG2CNT: char 0, screen 28, wrap, 256x256
    Reg16(0x20, 0x00E0), Reg16(0x22, 0x0050); // PA, PB
    Reg16(0x24, (uint16_t)-0x0050), Reg16(0x26, 0x00E0); // PC, PD
    Reg16(0x28, 0x1234), Reg16(0x2A, 0); // BG2X
    Reg16(0x2C, 0x5678), Reg16(0x2E, 0); // BG2Y
    RenderBoth(0x0035, 4, 0); // BG 6x4
    b.h = 6, b.v = 4;
    return CheckBlocks("affine", &b);
}

static int TestSize1(void) {
    int bad = 0;
    ClearHardware();
    SetupTextBg(0x0000, 0xF800, 0);
    SetupObjTiles();
    SetObj(0, 0x0021, 0x8033, 0x0000); // 32x32 at (51, 33)
    Reg16(0x00, 0x1140);               // mode 0, BG0, OBJ, 1D
    Reg16(0x08, 0x1F00);
    RenderBoth(0x0000, 0xF, ~0u);
    for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) {
        bad += sPlain[i] != sMosaic[i];
    }
    return Report("size1", bad, 0, 0, 0, 0);
}

static int TestObj(void) {
    Blocks b = { 5, 3, 51, 33, 51 + 32, 33 + 32 };
    ClearHardware();
    SetupObjTiles();
    SetObj(0, 0x0021, 0xB033, 0x3000); // 32x32 at (51, 33), H+V flip, palette 3
    Reg16(0x00, 0x1040);               // mode 0, OBJ, 1D
    RenderBoth(0x2400, 0, 1);          // OBJ 5x3
    return CheckBlocks("obj", &b);
}

static int TestObjAffine(void) {
    // 32x32 affine double size: a 64x64 box at (70, 50), rotated and scaled.
    Blocks b = { 3, 7, 70, 50, 70 + 64, 50 + 64 };
    ClearHardware();
    SetupObjTiles();
    SetAffine(2, 0x00C0, 0x0060, -0x0060, 0x00C0);
    SetObj(0, 0x0332, 0x8446, 0x1000); // affine + double size, group 2, (70, 50)
    Reg16(0x00, 0x1040);
    RenderBoth(0x6200, 0, 1); // OBJ 3x7
    return CheckBlocks("objaff", &b);
}

static int TestWindow(void) {
    static uint16_t windowed[PPU_WIDTH * PPU_HEIGHT];
    Blocks b = kScreen;
    int bad = 0, fx = 0, fy = 0;
    uint16_t got = 0, want = 0;

    // BG0 with mosaic only inside WIN0 (x 37..149, lines 21..116), the backdrop
    // outside: inside it is the unwindowed mosaic picture, so the blocks stay
    // aligned to the screen and are cut by the window edge.
    ClearHardware();
    SetupTextBg(0x0000, 0xF800, 0);
    Reg16(0x00, 0x2100);
    Reg16(0x08, 0x1F40);
    Reg16(0x40, (37 << 8) | 150), Reg16(0x44, (21 << 8) | 117);
    Reg16(0x48, 0x0001), Reg16(0x4A, 0x0000);
    Reg16(0x4C, 0x0077); // BG 8x8
    PpuRenderFrame(windowed);

    Reg16(0x00, 0x0100);
    Reg16(0x08, 0x1F00);
    RenderBoth(0x0077, 1, 0);
    b.h = 8, b.v = 8;
    if (CheckBlocks("window-b", &b)) {
        return 1;
    }
    for (int y = 0; y < PPU_HEIGHT; y++) {
        for (int x = 0; x < PPU_WIDTH; x++) {
            int inside = x >= 37 && x < 150 && y >= 21 && y < 117;
            uint16_t expect = inside ? sMosaic[y * PPU_WIDTH + x] : Color(0);
            if (windowed[y * PPU_WIDTH + x] != expect && bad++ == 0) {
                fx = x, fy = y, got = windowed[y * PPU_WIDTH + x], want = expect;
            }
        }
    }
    return Report("window", bad, fx, fy, got, want);
}

static uint16_t Blend(uint16_t a, uint16_t b, int eva, int evb) {
    uint16_t out = 0;
    for (int ch = 0; ch < 15; ch += 5) {
        int v = (((a >> ch) & 0x1F) * eva + ((b >> ch) & 0x1F) * evb) >> 4;
        out |= (v > 31 ? 31 : v) << ch;
    }
    return out;
}

static int TestBlend(void) {
    static uint16_t top[PPU_WIDTH * PPU_HEIGHT], below[PPU_WIDTH * PPU_HEIGHT];
    int bad = 0, fx = 0, fy = 0;
    uint16_t got = 0, want = 0;

    ClearHardware();
    SetupTextBg(0x0000, 0xF800, 0);
    SetupTextBg(0x4000, 0xF000, 5);
    Reg16(0x08, 0x1F40); // BG0: mosaic, priority 0
    Reg16(0x0A, 0x1E05); // BG1: char 1, screen 30, priority 1
    Reg16(0x4C, 0x0034); // BG 5x4

    Reg16(0x00, 0x0100); // BG0 alone (mosaic)
    PpuRenderFrame(top);
    Reg16(0x00, 0x0200); // BG1 alone
    PpuRenderFrame(below);
    Reg16(0x00, 0x0300);
    Reg16(0x50, 0x0241), Reg16(0x52, 0x0709); // BG0 over BG1, 9/16 + 7/16
    PpuRenderFrame(sMosaic);
    for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) {
        uint16_t expect = Blend(top[i], below[i], 9, 7);
        if (sMosaic[i] != expect && bad++ == 0) {
            fx = i % PPU_WIDTH, fy = i / PPU_WIDTH, got = sMosaic[i], want = expect;
        }
    }
    return Report("blend", bad, fx, fy, got, want);
}

static int TestObjWindow(void) {
    // A window sprite with mosaic over a solid BG0: BG0 outside the OBJ window,
    // nothing (backdrop) inside. Its tiles are a checkerboard of opaque and
    // transparent pixels, which mosaic turns into blocks; the window must take
    // the same blocky shape as a normal sprite with the same tiles and mosaic.
    static uint16_t shape[PPU_WIDTH * PPU_HEIGHT];
    Blocks b = { 4, 4, 40, 40, 40 + 32, 40 + 32 };
    int bad = 0, fx = 0, fy = 0;
    uint16_t got = 0, want = 0;

    ClearHardware();
    for (int i = 0; i < 16 * 32; i++) {
        int row = (i % 32) / 4, tile = i / 32;
        gHwVram[0x10000 + i] = ((row + tile + (i & 1)) & 1) ? 0x11 : 0x00;
    }
    memset(&gHwVram[32], 0x11, 32); // BG tile 1: colour 1
    for (int i = 0; i < 32 * 32; i++) {
        gHwVram[0xF800 + i * 2] = 1;
    }
    Reg16(0x08, 0x1F00);
    Reg16(0x4C, 0x3300); // OBJ 4x4

    // The shape: a normal sprite with mosaic over BG0.
    SetObj(0, 0x1028, 0x8028, 0x0000); // mosaic, 32x32 at (40, 40)
    Reg16(0x00, 0x1140);
    PpuRenderFrame(shape);

    SetObj(0, 0x1828, 0x8028, 0x0000); // the same as an OBJ window sprite
    Reg16(0x00, 0x9140);               // BG0, OBJ, OBJ window
    Reg16(0x4A, 0x0001);               // outside: BG0; OBJ window: nothing
    PpuRenderFrame(sMosaic);
    for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) {
        uint16_t expect = shape[i] == Color(1) ? Color(1) : Color(0);
        if (sMosaic[i] != expect && bad++ == 0) {
            fx = i % PPU_WIDTH, fy = i / PPU_WIDTH, got = sMosaic[i], want = expect;
        }
    }
    if (Report("objwin", bad, fx, fy, got, want)) {
        return 1;
    }
    // And that shape is in blocks: the normal sprite, mosaic vs plain.
    SetObj(0, 0x0028, 0x8028, 0x0000);
    Reg16(0x00, 0x1140);
    RenderBoth(0x3300, 0, 1);
    return CheckBlocks("objwin-b", &b);
}

static int TestObj8bpp(void) {
    int bad = 0;
    ClearHardware();
    // An 8x8 8bpp sprite on tile 3: its pixels are the 64 bytes at 0x10000 + 3 * 32.
    for (int i = 0; i < 64; i++) {
        gHwVram[0x10000 + 3 * 32 + i] = i + 1;
    }
    SetObj(0, 0x2000 | 10, 20, 3); // 8bpp, 8x8 at (20, 10), tile 3
    Reg16(0x00, 0x1040);
    PpuRenderFrame(sMosaic);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            bad += sMosaic[(10 + y) * PPU_WIDTH + 20 + x] != Color(0x100 + y * 8 + x + 1);
        }
    }
    return Report("obj8bpp", bad, 0, 0, 0, 0);
}

static int TestBgObjVram(void) {
    int bad = 0;
    ClearHardware();
    // Char base 3 (0xC000): tile 512 is at 0x10000, in OBJ VRAM. Fill it and use it.
    memset(&gHwVram[0x10000], 0x11, 32);
    memset(&gHwVram[0xC000 + 32], 0x22, 32); // tile 1, in BG VRAM
    for (int i = 0; i < 32 * 32; i++) {
        uint16_t entry = i & 1 ? 512 : 1;
        gHwVram[0xF800 + i * 2] = entry & 0xFF, gHwVram[0xF800 + i * 2 + 1] = entry >> 8;
    }
    Reg16(0x00, 0x0100);
    Reg16(0x08, 0x1F0C); // BG0: char base 3, screen 31
    PpuRenderFrame(sMosaic);
    for (int y = 0; y < PPU_HEIGHT; y++) {
        for (int x = 0; x < PPU_WIDTH; x++) {
            bad += sMosaic[y * PPU_WIDTH + x] != ((x >> 3) & 1 ? Color(0) : Color(2));
        }
    }
    return Report("bgobjvram", bad, 0, 0, 0, 0);
}

// Overlapping objects exercise priority, OAM tie-breaking, transparency and
// the GBA's forced alpha for semi-transparent objects over a background.
static int TestMaskedObjWindow(void) {
    int bad = 0;
    ClearHardware();
    memset(&gHwVram[0x10000], 0x11, 32);
    memset(&gHwVram[32], 0x11, 32);
    for (int i = 0; i < 32 * 32; i++) gHwVram[0xF800 + i * 2] = 1;
    Reg16(0x08, 0x1F00);
    Reg16(0x00, 0x9140); // BG0, OBJ, OBJ window, 1D mapping
    Reg16(0x4A, 0x0001); // BG0 outside, backdrop inside
    SetObj(0, 0x0800 | 20, 20, 0);
    PpuSetLayerMask(0x0F); // hide sprite colors without disabling OBJ windows
    PpuRenderFrame(sMosaic);
    for (int y = 0; y < PPU_HEIGHT; y++) {
        for (int x = 0; x < PPU_WIDTH; x++) {
            uint16_t want = x >= 20 && x < 28 && y >= 20 && y < 28 ? Color(0) : Color(1);
            bad += sMosaic[y * PPU_WIDTH + x] != want;
        }
    }
    int failed = Report("objwin-hidden", bad, 0, 0, 0, 0);
    PpuSetLayerMask(PPU_LAYERS_ALL);
    return failed;
}

static int TestObjPriority(void) {
    int failed = 0;
    const int pixel = 20 * PPU_WIDTH + 20;
    ClearHardware();
    memset(&gHwVram[0x10000], 0x11, 32);
    memset(&gHwVram[0x10020], 0x22, 32);
    memset(&gHwVram[0x10040], 0x33, 32);
    SetObj(0, 20, 20, 2 << 10);
    SetObj(1, 20, 20, (1 << 10) | 1);
    SetObj(2, 20, 20, (1 << 10) | 2);
    Reg16(0x00, 0x1040);
    PpuRenderFrame(sMosaic);
    failed |= Report("obj-prio", sMosaic[pixel] != Color(0x102), 20, 20, sMosaic[pixel], Color(0x102));
    SetObj(0, 20, 20, 1 << 10);
    PpuRenderFrame(sMosaic);
    failed |= Report("obj-tie", sMosaic[pixel] != Color(0x101), 20, 20, sMosaic[pixel], Color(0x101));
    memset(&gHwVram[0x10000], 0, 32);
    PpuRenderFrame(sMosaic);
    failed |= Report("obj-transparent", sMosaic[pixel] != Color(0x102), 20, 20, sMosaic[pixel], Color(0x102));
    memset(&gHwVram[0x10000], 0x11, 32);
    memset(&gHwVram[32], 0x44, 32);
    for (int i = 0; i < 32 * 32; i++) gHwVram[0xF800 + i * 2] = 1;
    Reg16(0x08, 0x1F01); // BG0 priority 1
    Reg16(0x00, 0x1140); // BG0 and OBJ
    SetObj(0, 0x0400 | 20, 20, 0); // semi-transparent OBJ, priority 0
    Reg16(0x50, 0x0100); // BG0 is a second target
    Reg16(0x52, 0x0808);
    PpuRenderFrame(sMosaic);
    uint16_t want = Blend(Color(0x101), Color(4), 8, 8);
    failed |= Report("obj-alpha", sMosaic[pixel] != want, 20, 20, sMosaic[pixel], want);
    return failed;
}

static int TestMode2Layers(void) {
    int failed = 0;
    ClearHardware();
    for (int i = 0; i < 64; i++) {
        gHwVram[64 + i] = i % 8 < 4 ? 0 : 1;
        gHwVram[128 + i] = 2;
    }
    memset(&gHwVram[0xF800], 1, 16 * 16);
    memset(&gHwVram[0xF000], 2, 16 * 16);
    Reg16(0x0C, 0x1F00); // BG2 priority 0
    Reg16(0x0E, 0x1E01); // BG3 priority 1
    Reg16(0x20, 0x0100); Reg16(0x26, 0x0100);
    Reg16(0x30, 0x0100); Reg16(0x36, 0x0100);
    Reg16(0x00, 0x0C02); // mode 2, BG2 and BG3
    PpuRenderFrame(sMosaic);
    int bad = sMosaic[0] != Color(2) || sMosaic[4] != Color(1);
    failed |= Report("mode2-layers", bad, 4, 0, sMosaic[4], Color(1));
    PpuSetLayerMask(PPU_LAYERS_ALL & ~(1u << 2));
    PpuRenderFrame(sMosaic);
    failed |= Report("mode2-hidden", sMosaic[4] != Color(2), 4, 0, sMosaic[4], Color(2));
    PpuSetLayerMask(PPU_LAYERS_ALL);
    return failed;
}

static int TestEffects(void) {
    int failed = 0, bad;
    ClearHardware();
    Reg16(0x50, 0x00a0); // backdrop as first target, brighten
    Reg16(0x54, 16);
    PpuRenderFrame(sMosaic);
    bad = 0;
    for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) bad += sMosaic[i] != 0x7fff;
    failed |= Report("brighten", bad, 0, 0, 0, 0);
    Reg16(0x50, 0x00e0); // darken
    PpuRenderFrame(sMosaic);
    bad = 0;
    for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) bad += sMosaic[i] != 0;
    failed |= Report("darken", bad, 0, 0, 0, 0);
    Reg16(0x00, 0x0080); // forced blank
    PpuRenderFrame(sMosaic);
    bad = 0;
    for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) bad += sMosaic[i] != 0x7fff;
    failed |= Report("blank", bad, 0, 0, 0, 0);
    return failed;
}

static int TestPerformance(void) {
    PerformanceSnapshot snap;
    PerformanceReset();
    PerformanceSetEnabled(false);
    PpuRenderFrame(sPlain);
    PerformanceSetEnabled(true);
    PerformanceBeginFrame();
    PpuRenderFrame(sMosaic);
    uint64_t elapsed = PerformancePpuTotal();
    PerformanceEndFrame(123, 1000000, elapsed, 100000, 16.74);
    PerformanceCopy(&snap);
    int bad = memcmp(sPlain, sMosaic, sizeof(sPlain)) != 0 || snap.count != 1 ||
              snap.frames[0].frame != 123 || snap.frames[0].stage[PERF_GAME] != 1.0 ||
              snap.frames[0].total <= 1.0 || elapsed == 0;
    for (int i = 0; i < PERF_SAMPLES + 1; i++) {
        PerformanceEndFrame(i, 1000000, elapsed, 0, 16.74);
    }
    PerformanceCopy(&snap);
    bad |= snap.count != PERF_SAMPLES;
    PerformanceReset();
    PerformanceCopy(&snap);
    bad |= snap.count != 0;
    PerformanceSetEnabled(false);
    return Report("perf", bad, 0, 0, 0, 0);
}

static int TestCompactCapture(void) {
    PpuCapture* full = calloc(1, sizeof(*full));
    PpuCompactCapture* compact = malloc(sizeof(*compact));
    if (!full || !compact) exit(1);
    ClearHardware();
    memset(&gHwVram[32], 0x11, 32);
    for (int i = 0; i < 32 * 32; i++) gHwVram[0xF800 + i * 2] = 1;
    Reg16(0x08, 0x1F00);
    Reg16(0x00, 0x0100);
    PpuCaptureTo(full->lines);
    PpuCompactCaptureTo(compact);
    PpuBeginFrame();
    for (int line = 0; line < PPU_HEIGHT; line++) {
        // Direct writes are deliberately not routed through DMA or helpers.
        gHwVram[(line * 997) % HW_VRAM_SIZE] ^= line + 1;
        gHwVram[32 + line % 32] ^= line + 1;
        gHwPltt[2] ^= line + 3;
        gHwPltt[(line * 7) % HW_PLTT_SIZE] ^= line + 3;
        gHwOam[(line * 11) % HW_OAM_SIZE] ^= line + 5;
        Reg16(0x10, line * 3);
        PpuRenderLine(line, &full->reference[line * PPU_WIDTH]);
    }
    PpuCaptureTo(NULL);
    PpuCompactCaptureTo(NULL);
    int bad = compact->pageCount < PPU_CAPTURE_PAGES || compact->pageCount > PPU_CAPTURE_MAX_PAGES;
    for (int line = 0; line < PPU_HEIGHT; line++) {
        PpuCompactLine* packed = &compact->lines[line];
        PpuCaptureLine* reference = &full->lines[line];
        bad |= memcmp(&packed->meta, (const uint8_t*)reference + offsetof(PpuCaptureLine, palette), sizeof(packed->meta)) != 0;
        for (int page = 0; page < PPU_CAPTURE_PAGES; page++) {
            uint32_t index = packed->page[page];
            if (index >= compact->pageCount ||
                memcmp(compact->pages + index * PPU_CAPTURE_PAGE_SIZE,
                       reference->vram + page * PPU_CAPTURE_PAGE_SIZE, PPU_CAPTURE_PAGE_SIZE) != 0) bad++;
        }
    }
    const char* dir = getenv("TMC_GPU_CASE_DIR");
    if (dir != NULL) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/compact-changing.bin", dir);
        FILE* file = fopen(path, "wb");
        if (!file || fwrite(full, sizeof(*full), 1, file) != 1 || fclose(file) != 0) {
            fprintf(stderr, "could not write GPU case %s\n", path);
            exit(1);
        }
    }
    // Worst case: every page changes on every line and uses the full capacity.
    PpuCompactCaptureTo(compact);
    PpuBeginFrame();
    for (int line = 0; line < PPU_HEIGHT; line++) {
        memset(gHwVram, line, sizeof(gHwVram));
        PpuRenderLine(line, NULL);
    }
    PpuCompactCaptureTo(NULL);
    bad |= compact->pageCount != PPU_CAPTURE_MAX_PAGES || PpuCompactSize(compact) != offsetof(PpuCompactCapture, previousVram);
    free(compact);
    free(full);
    return Report("compact", bad, 0, 0, 0, 0);
}

int main(void) {
    int failed = 0;

    failed |= TestText();
    failed |= TestAffine();
    failed |= TestSize1();
    failed |= TestObj();
    failed |= TestObjAffine();
    failed |= TestWindow();
    failed |= TestBlend();
    failed |= TestObjWindow();
    failed |= TestMaskedObjWindow();
    failed |= TestObj8bpp();
    failed |= TestBgObjVram();
    failed |= TestObjPriority();
    failed |= TestMode2Layers();
    failed |= TestEffects();
    failed |= TestCompactCapture();
    failed |= TestPerformance();
    return failed;
}
