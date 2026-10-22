/*
 * Software GBA picture processor (see port/ppu.h). Implements display modes 0-2:
 * text and affine backgrounds, regular and affine sprites, priorities, windows
 * (WIN0, WIN1, OBJ window) and colour effects (alpha blending, brighten,
 * darken). Frames are drawn one scanline at a time and each line reads the
 * registers as they are then, so changes made between lines (HBlank DMA, VCOUNT
 * interrupts; see frame.c) show up on the lines that follow, as on hardware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "port/hw.h"
#include "port/ppu.h"

static uint16_t Reg16(uint32_t offset) {
    return gHwIo[offset] | (gHwIo[offset + 1] << 8);
}

static int32_t Reg28(uint32_t offset) {
    uint32_t value = gHwIo[offset] | (gHwIo[offset + 1] << 8) | (gHwIo[offset + 2] << 16) | ((uint32_t)gHwIo[offset + 3] << 24);
    return (int32_t)(value << 4) >> 4; // 28-bit signed
}

static uint16_t Palette(uint32_t index) {
    return (gHwPltt[index * 2] | (gHwPltt[index * 2 + 1] << 8)) & 0x7FFF;
}

// ---- layers -------------------------------------------------------------------

enum { LAYER_BG0, LAYER_BG1, LAYER_BG2, LAYER_BG3, LAYER_OBJ, LAYER_BD };

#define PIXEL_EMPTY 0xFFFF

typedef struct {
    uint16_t color[PPU_WIDTH];      // PIXEL_EMPTY where transparent
    uint8_t priority;
} BgLine;

typedef struct {
    uint16_t color[PPU_WIDTH];
    uint8_t priority[PPU_WIDTH];
    uint8_t semiTransparent[PPU_WIDTH];
    uint8_t window[PPU_WIDTH]; // pixel belongs to the OBJ window
} ObjLine;

// A pixel of a 4bpp or 8bpp tile; returns the palette index or 0 (transparent).
static uint32_t TilePixel(uint32_t tileBase, uint32_t tile, int x, int y, int is8bpp) {
    if (is8bpp) {
        uint32_t addr = tileBase + tile * 64 + y * 8 + x;
        return addr < HW_VRAM_SIZE ? gHwVram[addr] : 0;
    } else {
        uint32_t addr = tileBase + tile * 32 + y * 4 + x / 2;
        if (addr >= HW_VRAM_SIZE) {
            return 0;
        }
        return (x & 1) ? gHwVram[addr] >> 4 : gHwVram[addr] & 0xF;
    }
}

static void RenderTextBg(int bg, int line, BgLine* out) {
    uint16_t cnt = Reg16(0x08 + bg * 2);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int is8bpp = (cnt >> 7) & 1;
    int size = cnt >> 14;
    int width = (size & 1) ? 512 : 256;
    int height = (size & 2) ? 512 : 256;
    int hofs = Reg16(0x10 + bg * 4) & 0x1FF;
    int vofs = Reg16(0x12 + bg * 4) & 0x1FF;
    int y = (line + vofs) & (height - 1);

    out->priority = cnt & 3;
    for (int sx = 0; sx < PPU_WIDTH; sx++) {
        int x = (sx + hofs) & (width - 1);
        // 32x32 screen blocks, laid out left-right then top-bottom.
        uint32_t block = (x >= 256 ? 1 : 0) + (y >= 256 ? (width == 512 ? 2 : 1) : 0);
        uint32_t entryAddr = screenBase + block * 0x800 + (((y & 255) >> 3) * 32 + ((x & 255) >> 3)) * 2;
        uint16_t entry = gHwVram[entryAddr] | (gHwVram[entryAddr + 1] << 8);
        int tx = x & 7, ty = y & 7;
        uint32_t index;

        if (entry & 0x400) {
            tx = 7 - tx;
        }
        if (entry & 0x800) {
            ty = 7 - ty;
        }
        index = TilePixel(charBase, entry & 0x3FF, tx, ty, is8bpp);
        if (index == 0) {
            out->color[sx] = PIXEL_EMPTY;
        } else {
            out->color[sx] = Palette(is8bpp ? index : ((entry >> 12) * 16 + index));
        }
    }
}

// Internal reference points of BG2 and BG3 (index bg - 2), 28-bit signed 8.8.
// Hardware copies BGxX/BGxY into these at VBlank and whenever they are written,
// and adds PB/PD after every line.
static int32_t sRefX[2], sRefY[2];

static void RenderAffineBg(int bg, BgLine* out) {
    uint16_t cnt = Reg16(0x08 + bg * 2);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int wrap = (cnt >> 13) & 1;
    int size = 128 << (cnt >> 14);
    uint32_t regs = bg == 2 ? 0x20 : 0x30;
    int16_t pa = Reg16(regs), pc = Reg16(regs + 4);
    int32_t refX = sRefX[bg - 2];
    int32_t refY = sRefY[bg - 2];

    out->priority = cnt & 3;
    for (int sx = 0; sx < PPU_WIDTH; sx++) {
        int32_t x = (refX + pa * sx) >> 8;
        int32_t y = (refY + pc * sx) >> 8;
        uint32_t tile, index;

        if (wrap) {
            x &= size - 1;
            y &= size - 1;
        } else if (x < 0 || y < 0 || x >= size || y >= size) {
            out->color[sx] = PIXEL_EMPTY;
            continue;
        }
        tile = gHwVram[(screenBase + (y >> 3) * (size >> 3) + (x >> 3)) % HW_VRAM_SIZE];
        index = TilePixel(charBase, tile, x & 7, y & 7, 1);
        out->color[sx] = index ? Palette(index) : PIXEL_EMPTY;
    }
}

static const uint8_t sObjSizes[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } }, // square
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } }, // horizontal
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } }, // vertical
};

static void RenderObjs(int line, int oneDimensional, ObjLine* out) {
    for (int i = 0; i < PPU_WIDTH; i++) {
        out->color[i] = PIXEL_EMPTY;
        out->priority[i] = 4;
        out->semiTransparent[i] = 0;
        out->window[i] = 0;
    }
    // Lower OAM index wins at equal priority, so draw from the back.
    for (int obj = 127; obj >= 0; obj--) {
        const uint8_t* oam = &gHwOam[obj * 8];
        uint16_t attr0 = oam[0] | (oam[1] << 8);
        uint16_t attr1 = oam[2] | (oam[3] << 8);
        uint16_t attr2 = oam[4] | (oam[5] << 8);
        int affine = (attr0 >> 8) & 1;
        int doubleOrHidden = (attr0 >> 9) & 1;
        int mode = (attr0 >> 10) & 3;
        int is8bpp = (attr0 >> 13) & 1;
        int shape = attr0 >> 14;
        int sizeIndex = attr1 >> 14;
        int width, height, boxWidth, boxHeight, top, left;
        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        uint32_t tileBase = attr2 & 0x3FF;
        int priority = (attr2 >> 10) & 3;

        if ((!affine && doubleOrHidden) || shape == 3 || mode == 3) {
            continue;
        }
        width = sObjSizes[shape][sizeIndex][0];
        height = sObjSizes[shape][sizeIndex][1];
        boxWidth = width << (affine && doubleOrHidden);
        boxHeight = height << (affine && doubleOrHidden);
        top = attr0 & 0xFF;
        if (top + boxHeight > 256) {
            top -= 256;
        }
        if (line < top || line >= top + boxHeight) {
            continue;
        }
        left = attr1 & 0x1FF;
        if (left >= 240) {
            left -= 512;
        }
        if (affine) {
            const uint8_t* p = &gHwOam[((attr1 >> 9) & 0x1F) * 32];
            pa = p[6] | (p[7] << 8);
            pb = p[14] | (p[15] << 8);
            pc = p[22] | (p[23] << 8);
            pd = p[30] | (p[31] << 8);
        }
        for (int bx = 0; bx < boxWidth; bx++) {
            int sx = left + bx;
            int tx, ty;
            uint32_t tile, index, color;

            if (sx < 0 || sx >= PPU_WIDTH) {
                continue;
            }
            if (affine) {
                int cx = bx - boxWidth / 2, cy = line - top - boxHeight / 2;
                tx = ((pa * cx + pb * cy) >> 8) + width / 2;
                ty = ((pc * cx + pd * cy) >> 8) + height / 2;
                if (tx < 0 || ty < 0 || tx >= width || ty >= height) {
                    continue;
                }
            } else {
                tx = bx;
                ty = line - top;
                if (attr1 & 0x1000) {
                    tx = width - 1 - tx;
                }
                if (attr1 & 0x2000) {
                    ty = height - 1 - ty;
                }
            }
            // Tile numbers count 32-byte units; 8bpp tiles take two.
            if (oneDimensional) {
                tile = tileBase + ((ty >> 3) * (width >> 3) + (tx >> 3)) * (is8bpp ? 2 : 1);
            } else {
                tile = tileBase + (ty >> 3) * 32 + (tx >> 3) * (is8bpp ? 2 : 1);
            }
            tile &= 0x3FF;
            index = is8bpp ? TilePixel(0x10000, tile / 2, tx & 7, ty & 7, 1)
                           : TilePixel(0x10000, tile, tx & 7, ty & 7, 0);
            if (index == 0) {
                continue;
            }
            if (mode == 2) {
                out->window[sx] = 1;
                continue;
            }
            color = Palette(0x100 + (is8bpp ? index : (attr2 >> 12) * 16 + index));
            if (priority <= out->priority[sx]) {
                out->color[sx] = color;
                out->priority[sx] = priority;
                out->semiTransparent[sx] = mode == 1;
            }
        }
    }
}

// ---- composition ----------------------------------------------------------------

static uint16_t Blend(uint16_t a, uint16_t b, int eva, int evb) {
    int r = ((a & 0x1F) * eva + (b & 0x1F) * evb) >> 4;
    int g = (((a >> 5) & 0x1F) * eva + ((b >> 5) & 0x1F) * evb) >> 4;
    int bl = (((a >> 10) & 0x1F) * eva + ((b >> 10) & 0x1F) * evb) >> 4;
    return (r > 31 ? 31 : r) | ((g > 31 ? 31 : g) << 5) | ((bl > 31 ? 31 : bl) << 10);
}

static uint16_t Brightness(uint16_t c, int evy, int up) {
    int ch[3] = { c & 0x1F, (c >> 5) & 0x1F, (c >> 10) & 0x1F };
    for (int i = 0; i < 3; i++) {
        ch[i] += up ? ((31 - ch[i]) * evy) >> 4 : -((ch[i] * evy) >> 4);
    }
    return ch[0] | (ch[1] << 5) | (ch[2] << 10);
}

void PpuBeginFrame(void) {
    for (int i = 0; i < 2; i++) {
        sRefX[i] = Reg28(0x28 + i * 0x10);
        sRefY[i] = Reg28(0x2C + i * 0x10);
    }
}

void PpuIoWritten(uint32_t offset, uint32_t size) {
    for (int i = 0; i < 2; i++) {
        uint32_t x = 0x28 + i * 0x10, y = x + 4;
        if (offset < x + 4 && offset + size > x) {
            sRefX[i] = Reg28(x);
        }
        if (offset < y + 4 && offset + size > y) {
            sRefY[i] = Reg28(y);
        }
    }
}

static void RenderLine(int line, uint16_t* out) {
    uint16_t dispcnt = Reg16(0x00);
    int mode = dispcnt & 7;
    uint16_t bldcnt = Reg16(0x50);
    uint16_t bldalpha = Reg16(0x52);
    int eva = bldalpha & 0x1F, evb = (bldalpha >> 8) & 0x1F;
    int evy = Reg16(0x54) & 0x1F;
    int effect = (bldcnt >> 6) & 3;
    int anyWindow = (dispcnt >> 13) & 7;
    uint16_t winin = Reg16(0x48), winout = Reg16(0x4A);
    static BgLine bgs[4];
    static ObjLine objs;
    int bgEnabled[4] = { 0 };

    if (eva > 16) {
        eva = 16;
    }
    if (evb > 16) {
        evb = 16;
    }
    if (evy > 16) {
        evy = 16;
    }

    if (dispcnt & 0x80) { // forced blank
        for (int x = 0; x < PPU_WIDTH; x++) {
            out[x] = 0x7FFF;
        }
        return;
    }
    for (int bg = 0; bg < 4; bg++) {
        int enabled = (dispcnt >> (8 + bg)) & 1;
        if (!enabled || (mode == 1 && bg == 3) || (mode == 2 && bg < 2) || mode > 2) {
            continue;
        }
        bgEnabled[bg] = 1;
        if ((mode == 1 && bg == 2) || mode == 2) {
            RenderAffineBg(bg, &bgs[bg]);
        } else {
            RenderTextBg(bg, line, &bgs[bg]);
        }
    }
    if (dispcnt & 0x1000) {
        RenderObjs(line, (dispcnt >> 6) & 1, &objs);
    } else {
        memset(&objs, 0xFF, sizeof(objs.color));
        memset(objs.window, 0, sizeof(objs.window));
    }

    for (int x = 0; x < PPU_WIDTH; x++) {
        // Which layers and effects this pixel's window allows (bits 0-5).
        int enable = 0x3F;
        int top = LAYER_BD, second = LAYER_BD;
        uint16_t topColor = Palette(0), secondColor = Palette(0);
        int topPriority = 4, secondPriority = 4;
        uint16_t color;

        if (anyWindow) {
            enable = winout & 0x3F;
            if ((dispcnt & 0x8000) && objs.window[x]) {
                enable = (winout >> 8) & 0x3F;
            }
            for (int w = 1; w >= 0; w--) {
                if (!(dispcnt & (0x2000 << w))) {
                    continue;
                }
                uint16_t h = Reg16(0x40 + w * 2), v = Reg16(0x44 + w * 2);
                int x1 = h >> 8, x2 = h & 0xFF, y1 = v >> 8, y2 = v & 0xFF;
                int inX = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
                int inY = y1 <= y2 ? (line >= y1 && line < y2) : (line >= y1 || line < y2);
                if (inX && inY) {
                    enable = (winin >> (w * 8)) & 0x3F;
                }
            }
        }

        // Find the two front-most layers: lower priority first, OBJ before
        // BGs of equal priority, then lower BG number.
        for (int prio = 0; prio < 4 && second == LAYER_BD; prio++) {
            if ((enable & 0x10) && objs.color[x] != PIXEL_EMPTY && objs.priority[x] == prio) {
                if (top == LAYER_BD) {
                    top = LAYER_OBJ, topColor = objs.color[x], topPriority = prio;
                } else {
                    second = LAYER_OBJ, secondColor = objs.color[x], secondPriority = prio;
                }
            }
            for (int bg = 0; bg < 4 && second == LAYER_BD; bg++) {
                if (!bgEnabled[bg] || !(enable & (1 << bg)) || bgs[bg].priority != prio ||
                    bgs[bg].color[x] == PIXEL_EMPTY) {
                    continue;
                }
                if (top == LAYER_BD) {
                    top = bg, topColor = bgs[bg].color[x], topPriority = prio;
                } else {
                    second = bg, secondColor = bgs[bg].color[x], secondPriority = prio;
                }
            }
        }
        (void)topPriority;
        (void)secondPriority;

        color = topColor;
        if (top == LAYER_OBJ && objs.semiTransparent[x] && (bldcnt & (0x100 << second))) {
            color = Blend(topColor, secondColor, eva, evb);
        } else if ((enable & 0x20) && (bldcnt & (1 << top))) {
            if (effect == 1 && (bldcnt & (0x100 << second))) {
                color = Blend(topColor, secondColor, eva, evb);
            } else if (effect == 2) {
                color = Brightness(topColor, evy, 1);
            } else if (effect == 3) {
                color = Brightness(topColor, evy, 0);
            }
        }
        out[x] = color;
    }
}

void PpuRenderLine(int line, uint16_t* out) {
    if (out != NULL) {
        RenderLine(line, out);
    }
    // Step the affine reference points to the next line.
    for (int i = 0; i < 2; i++) {
        sRefX[i] += (int16_t)Reg16(0x22 + i * 0x10);
        sRefY[i] += (int16_t)Reg16(0x26 + i * 0x10);
    }
}

void PpuRenderFrame(uint16_t* framebuffer) {
    PpuBeginFrame();
    for (int line = 0; line < PPU_HEIGHT; line++) {
        PpuRenderLine(line, &framebuffer[line * PPU_WIDTH]);
    }
}

// ---- PNG ------------------------------------------------------------------------

static void PutU32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

static int WriteChunk(FILE* f, const char* type, const uint8_t* data, uint32_t length) {
    uint8_t header[8];
    uint8_t crcBytes[4];
    uint32_t crc = crc32(0, (const Bytef*)type, 4);

    PutU32(header, length);
    memcpy(header + 4, type, 4);
    if (length) {
        crc = crc32(crc, data, length);
    }
    PutU32(crcBytes, crc);
    return fwrite(header, 1, 8, f) != 8 || (length && fwrite(data, 1, length, f) != length) ||
           fwrite(crcBytes, 1, 4, f) != 4;
}

int PpuWritePngSize(const uint16_t* framebuffer, int width, int height, const char* path) {
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    size_t rawSize = (size_t)height * (1 + width * 3);
    uint8_t* raw = malloc(rawSize);
    uLongf packedSize = compressBound(rawSize);
    uint8_t* packed = malloc(packedSize);
    uint8_t ihdr[13] = { 0 };
    FILE* f = NULL;
    int failed = -1;

    for (int y = 0; y < height; y++) {
        uint8_t* row = &raw[y * (1 + width * 3)];
        row[0] = 0; // no filter
        for (int x = 0; x < width; x++) {
            uint16_t c = framebuffer[y * width + x];
            // Expand 5-bit channels to 8 bits.
            for (int ch = 0; ch < 3; ch++) {
                uint8_t v = (c >> (5 * ch)) & 0x1F;
                row[1 + x * 3 + ch] = (v << 3) | (v >> 2);
            }
        }
    }
    if (compress2(packed, &packedSize, raw, rawSize, 6) == Z_OK && (f = fopen(path, "wb")) != NULL) {
        PutU32(ihdr, width);
        PutU32(ihdr + 4, height);
        ihdr[8] = 8; // bit depth
        ihdr[9] = 2; // RGB
        failed = fwrite(signature, 1, 8, f) != 8 || WriteChunk(f, "IHDR", ihdr, 13) ||
                 WriteChunk(f, "IDAT", packed, packedSize) || WriteChunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw);
    free(packed);
    return failed ? -1 : 0;
}

int PpuWritePng(const uint16_t* framebuffer, const char* path) {
    return PpuWritePngSize(framebuffer, PPU_WIDTH, PPU_HEIGHT, path);
}
