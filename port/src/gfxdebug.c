/*
 * Graphics debugging snapshots and decoders (see port/gfxdebug.h). The decoding
 * follows ppu.c; keep the two in step.
 */

#include <string.h>

#include "port/gfxdebug.h"

void GfxSnapshotBegin(GfxSnapshot* snap, uint64_t frame) {
    snap->frame = frame;
    memcpy(snap->io, gHwIo, sizeof(snap->io));
    memcpy(snap->pltt, gHwPltt, sizeof(snap->pltt));
    memcpy(snap->vram, gHwVram, sizeof(snap->vram));
    memcpy(snap->oam, gHwOam, sizeof(snap->oam));
    for (int i = 0; i < 4; i++) {
        const void* src;
        void* dest;
        HwDmaInfo(i, &src, &dest, &snap->dma[i].control);
        snap->dma[i].src = snap->dma[i].control ? HwGbaAddress(src) : 0;
        snap->dma[i].dest = snap->dma[i].control ? HwGbaAddress(dest) : 0;
    }
    // Until the frame is drawn, every line looks like line 0.
    for (int line = 0; line < PPU_HEIGHT; line++) {
        memcpy(snap->lines[line], gHwIo, HW_LINE_REGS);
    }
    snap->hasLines = 0;
    gHwLineCapture = snap->lines;
}

void GfxSnapshotEnd(GfxSnapshot* snap) {
    if (gHwLineCapture == snap->lines) {
        gHwLineCapture = NULL;
        snap->hasLines = 1;
    }
}

uint16_t GfxReg16(const GfxSnapshot* snap, uint32_t offset) {
    return snap->io[offset] | (snap->io[offset + 1] << 8);
}

static uint32_t Reg32(const GfxSnapshot* snap, uint32_t offset) {
    return GfxReg16(snap, offset) | ((uint32_t)GfxReg16(snap, offset + 2) << 16);
}

uint16_t GfxColor555(const GfxSnapshot* snap, int index) {
    return (snap->pltt[index * 2] | (snap->pltt[index * 2 + 1] << 8)) & 0x7FFF;
}

uint32_t GfxRgb(uint16_t c) {
    uint32_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

uint64_t GfxHash(const void* data, size_t size) {
    const uint8_t* p = data;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

// A pixel of a 4bpp or 8bpp tile (palette index), as ppu.c TilePixel.
static uint32_t TilePixel(const GfxSnapshot* snap, uint32_t tileBase, uint32_t tile, int x, int y, int is8bpp) {
    if (is8bpp) {
        uint32_t addr = tileBase + tile * 64 + y * 8 + x;
        return addr < HW_VRAM_SIZE ? snap->vram[addr] : 0;
    } else {
        uint32_t addr = tileBase + tile * 32 + y * 4 + x / 2;
        if (addr >= HW_VRAM_SIZE) {
            return 0;
        }
        return (x & 1) ? snap->vram[addr] >> 4 : snap->vram[addr] & 0xF;
    }
}

// ---- sprites ----------------------------------------------------------------------

static const uint8_t sObjSizes[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

void GfxDecodeObj(const GfxSnapshot* snap, int index, GfxObj* obj) {
    const uint8_t* oam = &snap->oam[index * 8];

    memset(obj, 0, sizeof(*obj));
    obj->index = index;
    obj->attr0 = oam[0] | (oam[1] << 8);
    obj->attr1 = oam[2] | (oam[3] << 8);
    obj->attr2 = oam[4] | (oam[5] << 8);
    obj->affine = (obj->attr0 >> 8) & 1;
    obj->doubleSize = obj->affine && ((obj->attr0 >> 9) & 1);
    obj->hidden = !obj->affine && ((obj->attr0 >> 9) & 1);
    obj->mode = (obj->attr0 >> 10) & 3;
    obj->mosaic = (obj->attr0 >> 12) & 1;
    obj->is8bpp = (obj->attr0 >> 13) & 1;
    obj->shape = obj->attr0 >> 14;
    obj->size = obj->attr1 >> 14;
    obj->hflip = !obj->affine && ((obj->attr1 >> 12) & 1);
    obj->vflip = !obj->affine && ((obj->attr1 >> 13) & 1);
    obj->tile = obj->attr2 & 0x3FF;
    obj->priority = (obj->attr2 >> 10) & 3;
    obj->palette = obj->attr2 >> 12;
    obj->matrix = obj->affine ? (obj->attr1 >> 9) & 0x1F : -1;
    obj->pa = obj->pd = 0x100;
    if (obj->affine) {
        const uint8_t* p = &snap->oam[obj->matrix * 32];
        obj->pa = p[6] | (p[7] << 8);
        obj->pb = p[14] | (p[15] << 8);
        obj->pc = p[22] | (p[23] << 8);
        obj->pd = p[30] | (p[31] << 8);
    }
    if (obj->shape == 3) {
        obj->width = obj->height = 8; // prohibited shape: never drawn
    } else {
        obj->width = sObjSizes[obj->shape][obj->size][0];
        obj->height = sObjSizes[obj->shape][obj->size][1];
    }
    obj->boxWidth = obj->width << obj->doubleSize;
    obj->boxHeight = obj->height << obj->doubleSize;
    obj->y = obj->attr0 & 0xFF;
    if (obj->y + obj->boxHeight > 256) {
        obj->y -= 256;
    }
    obj->x = obj->attr1 & 0x1FF;
    if (obj->x >= 240) {
        obj->x -= 512;
    }
    obj->visible = !obj->hidden && obj->shape != 3 && obj->mode != 3 && obj->x + obj->boxWidth > 0 &&
                   obj->x < PPU_WIDTH && obj->y + obj->boxHeight > 0 && obj->y < PPU_HEIGHT;
}

void GfxRenderObj(const GfxSnapshot* snap, int index, uint32_t* out, uint32_t transparent) {
    GfxObj obj;
    int oneDimensional = (GfxReg16(snap, 0) >> 6) & 1;

    GfxDecodeObj(snap, index, &obj);
    for (int ty = 0; ty < obj.height; ty++) {
        for (int tx = 0; tx < obj.width; tx++) {
            uint32_t tile, value;
            if (oneDimensional) {
                tile = obj.tile + ((ty >> 3) * (obj.width >> 3) + (tx >> 3)) * (obj.is8bpp ? 2 : 1);
            } else {
                tile = obj.tile + (ty >> 3) * 32 + (tx >> 3) * (obj.is8bpp ? 2 : 1);
            }
            tile &= 0x3FF;
            value = obj.is8bpp ? TilePixel(snap, 0x10000, tile / 2, tx & 7, ty & 7, 1)
                               : TilePixel(snap, 0x10000, tile, tx & 7, ty & 7, 0);
            out[ty * obj.width + tx] =
                value == 0 ? transparent : GfxRgb(GfxColor555(snap, 0x100 + (obj.is8bpp ? value : obj.palette * 16 + value)));
        }
    }
}

// ---- backgrounds --------------------------------------------------------------------

void GfxDecodeBg(const GfxSnapshot* snap, int bg, GfxBg* out) {
    uint16_t dispcnt = GfxReg16(snap, 0);
    int mode = dispcnt & 7;
    uint16_t cnt = GfxReg16(snap, 0x08 + bg * 2);

    memset(out, 0, sizeof(*out));
    out->bg = bg;
    out->cnt = cnt;
    out->enabled = (dispcnt >> (8 + bg)) & 1;
    if (mode == 0 || (mode == 1 && bg < 2)) {
        out->kind = GFX_BG_TEXT;
    } else if ((mode == 1 && bg == 2) || (mode == 2 && bg >= 2)) {
        out->kind = GFX_BG_AFFINE;
    } else {
        out->kind = GFX_BG_OFF;
    }
    out->priority = cnt & 3;
    out->charBase = ((cnt >> 2) & 3) * 0x4000;
    out->mosaic = (cnt >> 6) & 1;
    out->is8bpp = (cnt >> 7) & 1;
    out->screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    out->wrap = (cnt >> 13) & 1;
    out->sizeIndex = cnt >> 14;
    out->pa = out->pd = 0x100;
    if (out->kind == GFX_BG_AFFINE) {
        uint32_t regs = bg == 2 ? 0x20 : 0x30;
        out->width = out->height = 128 << out->sizeIndex;
        out->is8bpp = 1;
        out->pa = GfxReg16(snap, regs);
        out->pb = GfxReg16(snap, regs + 2);
        out->pc = GfxReg16(snap, regs + 4);
        out->pd = GfxReg16(snap, regs + 6);
        out->refX = (int32_t)(Reg32(snap, regs + 8) << 4) >> 4;
        out->refY = (int32_t)(Reg32(snap, regs + 12) << 4) >> 4;
    } else {
        out->width = (out->sizeIndex & 1) ? 512 : 256;
        out->height = (out->sizeIndex & 2) ? 512 : 256;
        out->hofs = GfxReg16(snap, 0x10 + bg * 4) & 0x1FF;
        out->vofs = GfxReg16(snap, 0x12 + bg * 4) & 0x1FF;
        out->wrap = 1;
    }
}

// Map entry of a text layer at map pixel (x, y).
static uint16_t TextEntry(const GfxSnapshot* snap, const GfxBg* bg, int x, int y) {
    uint32_t block = (x >= 256 ? 1 : 0) + (y >= 256 ? (bg->width == 512 ? 2 : 1) : 0);
    uint32_t addr = (bg->screenBase + block * 0x800 + (((y & 255) >> 3) * 32 + ((x & 255) >> 3)) * 2) % HW_VRAM_SIZE;
    return snap->vram[addr] | (snap->vram[addr + 1] << 8);
}

int GfxRenderBg(const GfxSnapshot* snap, int bgIndex, uint32_t* out, uint32_t transparent) {
    GfxBg bg;
    uint32_t backdrop = transparent ? transparent : GfxRgb(GfxColor555(snap, 0));

    GfxDecodeBg(snap, bgIndex, &bg);
    if (bg.kind == GFX_BG_OFF) {
        return 0;
    }
    for (int y = 0; y < bg.height; y++) {
        for (int x = 0; x < bg.width; x++) {
            uint32_t index, color;
            if (bg.kind == GFX_BG_TEXT) {
                uint16_t entry = TextEntry(snap, &bg, x, y);
                int tx = x & 7, ty = y & 7;
                if (entry & 0x400) {
                    tx = 7 - tx;
                }
                if (entry & 0x800) {
                    ty = 7 - ty;
                }
                index = TilePixel(snap, bg.charBase, entry & 0x3FF, tx, ty, bg.is8bpp);
                color = bg.is8bpp ? index : (entry >> 12) * 16 + index;
            } else {
                uint32_t tile = snap->vram[(bg.screenBase + (y >> 3) * (bg.width >> 3) + (x >> 3)) % HW_VRAM_SIZE];
                index = TilePixel(snap, bg.charBase, tile, x & 7, y & 7, 1);
                color = index;
            }
            out[y * bg.width + x] = index == 0 ? backdrop : GfxRgb(GfxColor555(snap, color));
        }
    }
    return 1;
}

void GfxRenderTiles(const GfxSnapshot* snap, uint32_t base, int count, int is8bpp, int palette, uint32_t* out) {
    int rows = (count + 31) / 32;

    for (int i = 0; i < rows * 32 * 64; i++) {
        out[i] = 0xFF000000u;
    }
    for (int t = 0; t < count; t++) {
        int ox = (t % 32) * 8, oy = (t / 32) * 8;
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint32_t value = TilePixel(snap, base, t, x, y, is8bpp), rgb;
                if (palette < 0) {
                    uint32_t v = is8bpp ? value : value * 17;
                    rgb = 0xFF000000u | (v << 16) | (v << 8) | v;
                } else {
                    int bank = palette >= 16 ? 0x100 : 0;
                    rgb = GfxRgb(GfxColor555(snap, bank + (is8bpp ? value : (palette & 15) * 16 + value)));
                }
                out[(oy + y) * 256 + ox + x] = rgb;
            }
        }
    }
}

// ---- text dumps -----------------------------------------------------------------------

static const char* const sModeNames[4] = { "normal", "semi", "objwin", "invalid" };
static const char* const sShapeNames[4] = { "square", "wide", "tall", "invalid" };

void GfxDumpOam(const GfxSnapshot* snap, FILE* out, int all) {
    int visible = 0;
    GfxObj obj;

    for (int i = 0; i < 128; i++) {
        GfxDecodeObj(snap, i, &obj);
        visible += obj.visible;
    }
    fprintf(out, "frame %llu oam: %s mapping, %d of 128 entries on screen (hash %016llx)\n",
            (unsigned long long)snap->frame, (GfxReg16(snap, 0) & 0x40) ? "1D" : "2D", visible,
            (unsigned long long)GfxHash(snap->oam, sizeof(snap->oam)));
    fprintf(out, "  obj attr0 attr1 attr2     x    y  size  shape   tile pal pri mode    flags  affine\n");
    for (int i = 0; i < 128; i++) {
        GfxDecodeObj(snap, i, &obj);
        if (!all && (obj.hidden || !obj.visible)) {
            continue;
        }
        fprintf(out, "  %3d  %04x  %04x  %04x  %4d %4d %2dx%-2d %-6s 0x%03x %3d %3d %-7s %c%c%c%c%c%c ", i, obj.attr0,
                obj.attr1, obj.attr2, obj.x, obj.y, obj.width, obj.height, sShapeNames[obj.shape], obj.tile,
                obj.is8bpp ? -1 : obj.palette, obj.priority, sModeNames[obj.mode], obj.hflip ? 'H' : '.',
                obj.vflip ? 'V' : '.', obj.is8bpp ? '8' : '.', obj.mosaic ? 'M' : '.', obj.doubleSize ? 'D' : '.',
                obj.hidden ? 'X' : obj.visible ? '.' : 'o');
        if (obj.affine) {
            fprintf(out, " m%-2d %6.3f %6.3f %6.3f %6.3f\n", obj.matrix, obj.pa / 256.0, obj.pb / 256.0, obj.pc / 256.0,
                    obj.pd / 256.0);
        } else {
            fprintf(out, " -\n");
        }
    }
    fprintf(out, "  flags: H/V flip, 8 = 8bpp (pal -1), M mosaic, D double size, X hidden, o off screen\n");
}

static int LayerLinesChanged(const GfxSnapshot* snap, uint32_t offset, uint32_t size) {
    int changed = 0;
    if (!snap->hasLines) {
        return 0;
    }
    for (int line = 1; line < PPU_HEIGHT; line++) {
        if (memcmp(&snap->lines[line][offset], &snap->lines[line - 1][offset], size) != 0) {
            changed++;
        }
    }
    return changed;
}

void GfxDumpBg(const GfxSnapshot* snap, FILE* out) {
    uint16_t dispcnt = GfxReg16(snap, 0);

    fprintf(out, "frame %llu bg: mode %d, DISPCNT %04x\n", (unsigned long long)snap->frame, dispcnt & 7, dispcnt);
    for (int i = 0; i < 4; i++) {
        GfxBg bg;
        GfxDecodeBg(snap, i, &bg);
        fprintf(out, "  BG%d %-3s %-6s cnt %04x prio %d char 0x%05x (block %d) screen 0x%05x (block %2d) %dbpp %dx%d%s%s",
                i, bg.enabled ? "on" : "off",
                bg.kind == GFX_BG_TEXT ? "text" : bg.kind == GFX_BG_AFFINE ? "affine" : "none", bg.cnt, bg.priority,
                bg.charBase, bg.charBase / 0x4000, bg.screenBase, bg.screenBase / 0x800, bg.is8bpp ? 8 : 4, bg.width,
                bg.height, bg.mosaic ? " mosaic" : "", bg.kind == GFX_BG_AFFINE && bg.wrap ? " wrap" : "");
        if (bg.kind == GFX_BG_TEXT) {
            fprintf(out, " scroll %d,%d\n", bg.hofs, bg.vofs);
        } else if (bg.kind == GFX_BG_AFFINE) {
            fprintf(out, " pa %.3f pb %.3f pc %.3f pd %.3f ref %.2f,%.2f\n", bg.pa / 256.0, bg.pb / 256.0,
                    bg.pc / 256.0, bg.pd / 256.0, bg.refX / 256.0, bg.refY / 256.0);
        } else {
            fprintf(out, "\n");
            continue;
        }
        // What the map uses: distinct tiles, palettes, flipped entries.
        {
            static uint8_t tileUsed[1024];
            int tiles = 0, flipped = 0, entries = 0;
            uint16_t palettes = 0;
            memset(tileUsed, 0, sizeof(tileUsed));
            for (int y = 0; y < bg.height; y += 8) {
                for (int x = 0; x < bg.width; x += 8) {
                    int tile;
                    if (bg.kind == GFX_BG_TEXT) {
                        uint16_t entry = TextEntry(snap, &bg, x, y);
                        tile = entry & 0x3FF;
                        palettes |= 1 << (entry >> 12);
                        flipped += (entry & 0xC00) != 0;
                    } else {
                        tile = snap->vram[(bg.screenBase + (y >> 3) * (bg.width >> 3) + (x >> 3)) % HW_VRAM_SIZE];
                    }
                    tiles += !tileUsed[tile];
                    tileUsed[tile] = 1;
                    entries++;
                }
            }
            fprintf(out, "      map: %d entries, %d distinct tiles", entries, tiles);
            if (bg.kind == GFX_BG_TEXT) {
                fprintf(out, ", %d flipped, palettes", flipped);
                for (int p = 0; p < 16; p++) {
                    if (palettes & (1 << p)) {
                        fprintf(out, " %d", p);
                    }
                }
            }
            fprintf(out, "; lines with new scroll/affine: %d\n",
                    bg.kind == GFX_BG_TEXT ? LayerLinesChanged(snap, 0x10 + i * 4, 4)
                                           : LayerLinesChanged(snap, i == 2 ? 0x20 : 0x30, 16));
        }
    }
}

void GfxDumpPal(const GfxSnapshot* snap, FILE* out) {
    fprintf(out, "frame %llu palettes (BGR555; hash BG %016llx OBJ %016llx)\n", (unsigned long long)snap->frame,
            (unsigned long long)GfxHash(snap->pltt, 0x200), (unsigned long long)GfxHash(snap->pltt + 0x200, 0x200));
    for (int p = 0; p < 32; p++) {
        fprintf(out, "  %-3s %2d:", p < 16 ? "BG" : "OBJ", p & 15);
        for (int i = 0; i < 16; i++) {
            fprintf(out, " %04x", GfxColor555(snap, p * 16 + i));
        }
        fprintf(out, "\n");
    }
}

static void LayerList(FILE* out, unsigned bits) {
    static const char* const names[6] = { "BG0", "BG1", "BG2", "BG3", "OBJ", "BD" };
    int any = 0;
    for (int i = 0; i < 6; i++) {
        if (bits & (1 << i)) {
            fprintf(out, "%s%s", any++ ? "+" : "", names[i]);
        }
    }
    if (!any) {
        fprintf(out, "-");
    }
}

static void WindowLayers(FILE* out, const char* name, unsigned bits) {
    fprintf(out, " %s ", name);
    LayerList(out, bits & 0x1F);
    fprintf(out, "%s", (bits & 0x20) ? " fx" : "");
}

void GfxDumpIo(const GfxSnapshot* snap, FILE* out) {
    uint16_t dispcnt = GfxReg16(snap, 0x00), dispstat = GfxReg16(snap, 0x04);
    uint16_t winin = GfxReg16(snap, 0x48), winout = GfxReg16(snap, 0x4A);
    uint16_t bldcnt = GfxReg16(snap, 0x50), alpha = GfxReg16(snap, 0x52), mosaic = GfxReg16(snap, 0x4C);
    static const char* const effects[4] = { "none", "alpha blend", "brighten", "darken" };

    fprintf(out, "frame %llu io (as line 0 was drawn)\n", (unsigned long long)snap->frame);
    fprintf(out, "DISPCNT  %04x  mode %d, OBJ %s mapping%s%s, layers ", dispcnt, dispcnt & 7,
            (dispcnt & 0x40) ? "1D" : "2D", (dispcnt & 0x80) ? ", FORCED BLANK" : "",
            (dispcnt & 0x20) ? ", HBlank interval free" : "");
    LayerList(out, (dispcnt >> 8) & 0x1F);
    fprintf(out, ", windows%s%s%s%s\n", (dispcnt & 0x2000) ? " WIN0" : "", (dispcnt & 0x4000) ? " WIN1" : "",
            (dispcnt & 0x8000) ? " OBJWIN" : "", (dispcnt & 0xE000) ? "" : " -");
    fprintf(out, "DISPSTAT %04x  VCOUNT match line %d, IRQ on%s%s%s\n", dispstat, dispstat >> 8,
            (dispstat & 0x08) ? " VBlank" : "", (dispstat & 0x10) ? " HBlank" : "", (dispstat & 0x20) ? " VCount" : "");
    for (int bg = 0; bg < 4; bg++) {
        GfxBg b;
        GfxDecodeBg(snap, bg, &b);
        fprintf(out, "BG%dCNT   %04x  prio %d char block %d screen block %d %dbpp size %d%s%s; HOFS %d VOFS %d\n", bg,
                b.cnt, b.priority, b.charBase / 0x4000, b.screenBase / 0x800, ((b.cnt >> 7) & 1) ? 8 : 4, b.sizeIndex,
                b.mosaic ? " mosaic" : "", ((b.cnt >> 13) & 1) ? " wrap" : "", GfxReg16(snap, 0x10 + bg * 4) & 0x1FF,
                GfxReg16(snap, 0x12 + bg * 4) & 0x1FF);
    }
    for (int bg = 2; bg < 4; bg++) {
        uint32_t r = bg == 2 ? 0x20 : 0x30;
        fprintf(out, "BG%d affine pa %04x pb %04x pc %04x pd %04x X %08x Y %08x (%.3f %.3f %.3f %.3f ref %.2f,%.2f)\n", bg,
                GfxReg16(snap, r), GfxReg16(snap, r + 2), GfxReg16(snap, r + 4), GfxReg16(snap, r + 6), Reg32(snap, r + 8),
                Reg32(snap, r + 12), (int16_t)GfxReg16(snap, r) / 256.0, (int16_t)GfxReg16(snap, r + 2) / 256.0,
                (int16_t)GfxReg16(snap, r + 4) / 256.0, (int16_t)GfxReg16(snap, r + 6) / 256.0,
                ((int32_t)(Reg32(snap, r + 8) << 4) >> 4) / 256.0, ((int32_t)(Reg32(snap, r + 12) << 4) >> 4) / 256.0);
    }
    for (int w = 0; w < 2; w++) {
        uint16_t h = GfxReg16(snap, 0x40 + w * 2), v = GfxReg16(snap, 0x44 + w * 2);
        fprintf(out, "WIN%dH    %04x  x %d-%d   WIN%dV %04x  y %d-%d%s\n", w, h, h >> 8, h & 0xFF, w, v, v >> 8, v & 0xFF,
                (dispcnt & (0x2000 << w)) ? "" : " (off)");
    }
    fprintf(out, "WININ    %04x ", winin);
    WindowLayers(out, "win0:", winin & 0x3F);
    WindowLayers(out, " win1:", winin >> 8);
    fprintf(out, "\nWINOUT   %04x ", winout);
    WindowLayers(out, "outside:", winout & 0x3F);
    WindowLayers(out, " objwin:", winout >> 8);
    fprintf(out, "\nBLDCNT   %04x  %s, 1st ", bldcnt, effects[(bldcnt >> 6) & 3]);
    LayerList(out, bldcnt & 0x3F);
    fprintf(out, ", 2nd ");
    LayerList(out, (bldcnt >> 8) & 0x3F);
    fprintf(out, "\nBLDALPHA %04x  EVA %d/16 EVB %d/16   BLDY %04x  EVY %d/16\n", alpha, alpha & 0x1F, (alpha >> 8) & 0x1F,
            GfxReg16(snap, 0x54), GfxReg16(snap, 0x54) & 0x1F);
    fprintf(out, "MOSAIC   %04x  BG %dx%d OBJ %dx%d\n", mosaic, (mosaic & 0xF) + 1, ((mosaic >> 4) & 0xF) + 1,
            ((mosaic >> 8) & 0xF) + 1, ((mosaic >> 12) & 0xF) + 1);
    fprintf(out, "IE %04x  IF %04x  IME %04x\n", GfxReg16(snap, 0x200), GfxReg16(snap, 0x202), GfxReg16(snap, 0x208));
    for (int i = 0; i < 4; i++) {
        const GfxDmaChannel* d = &snap->dma[i];
        uint16_t cnt = d->control >> 16;
        static const char* const timing[4] = { "immediate", "VBlank", "HBlank", "special" };
        static const char* const step[4] = { "inc", "dec", "fixed", "reload" };
        if (d->control == 0) {
            fprintf(out, "DMA%d     idle\n", i);
            continue;
        }
        fprintf(out, "DMA%d     %08x  %s%s, %u x %d-bit, src %08x (%s) dest %08x (%s)\n", i, d->control,
                timing[(cnt >> 12) & 3], (cnt & 0x200) ? " repeat" : "", d->control & 0xFFFF, (cnt & 0x400) ? 32 : 16,
                d->src, step[(cnt >> 7) & 3], d->dest, step[(cnt >> 5) & 3]);
    }
}

static const char* RegName(int offset) {
    static const char* const names[HW_LINE_REGS / 2] = {
        "DISPCNT", "GREENSWP", "DISPSTAT", "VCOUNT", "BG0CNT", "BG1CNT", "BG2CNT", "BG3CNT",
        "BG0HOFS", "BG0VOFS", "BG1HOFS", "BG1VOFS", "BG2HOFS", "BG2VOFS", "BG3HOFS", "BG3VOFS",
        "BG2PA", "BG2PB", "BG2PC", "BG2PD", "BG2X_L", "BG2X_H", "BG2Y_L", "BG2Y_H",
        "BG3PA", "BG3PB", "BG3PC", "BG3PD", "BG3X_L", "BG3X_H", "BG3Y_L", "BG3Y_H",
        "WIN0H", "WIN1H", "WIN0V", "WIN1V", "WININ", "WINOUT", "MOSAIC", "4E",
        "BLDCNT", "BLDALPHA", "BLDY",
    };
    return names[offset / 2];
}

void GfxDumpLines(const GfxSnapshot* snap, FILE* out) {
    int changes = 0;

    if (!snap->hasLines) {
        fprintf(out, "frame %llu lines: not recorded\n", (unsigned long long)snap->frame);
        return;
    }
    fprintf(out, "frame %llu per-line registers (changes from the line before; DISPSTAT/VCOUNT left out)\n",
            (unsigned long long)snap->frame);
    for (int line = 1; line < PPU_HEIGHT; line++) {
        int shown = 0;
        for (int r = 0; r < HW_LINE_REGS; r += 2) {
            const uint8_t* now = &snap->lines[line][r];
            const uint8_t* before = &snap->lines[line - 1][r];
            if (r == 0x04 || r == 0x06 || (now[0] == before[0] && now[1] == before[1])) {
                continue;
            }
            if (!shown++) {
                fprintf(out, "  line %3d:", line);
            }
            fprintf(out, " %s=%04x", RegName(r), now[0] | (now[1] << 8));
        }
        if (shown) {
            fprintf(out, "\n");
            changes++;
        }
    }
    if (changes == 0) {
        fprintf(out, "  (no changes: every line used line 0's registers)\n");
    }
}
