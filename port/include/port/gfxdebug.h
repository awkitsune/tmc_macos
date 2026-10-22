#ifndef PORT_GFXDEBUG_H
#define PORT_GFXDEBUG_H

/*
 * Graphics debugging: a copy of the emulated display hardware taken as a frame
 * is drawn (VRAM, OAM, palette, I/O registers, and the display registers each
 * scanline was drawn with), plus decoders that turn it into text and pictures.
 * Used by the macOS app's debug windows and the headless runner's TMC_DUMP_*
 * dumps. Nothing here runs unless a viewer or a dump asks for a snapshot.
 */

#include <stdint.h>
#include <stdio.h>

#include "port/hw.h"
#include "port/ppu.h"

typedef struct {
    uint32_t control;   // the latched DMAxCNT (count in the low half)
    uint32_t src, dest; // GBA addresses where known, else 0
} GfxDmaChannel;

typedef struct {
    uint64_t frame;
    uint8_t io[HW_IO_SIZE]; // as the frame started (line 0)
    uint8_t pltt[HW_PLTT_SIZE];
    uint8_t vram[HW_VRAM_SIZE];
    uint8_t oam[HW_OAM_SIZE];
    GfxDmaChannel dma[4];
    // Display registers (DISPCNT .. BLDY) as each line was drawn; valid when
    // hasLines is set (the snapshot covered a whole HwRunFrame).
    uint8_t lines[PPU_HEIGHT][HW_LINE_REGS];
    int hasLines;
} GfxSnapshot;

/*
 * Take a snapshot of the hardware as it is now and, until GfxSnapshotEnd, record
 * the display registers of every line HwRunFrame draws into it. Call Begin just
 * before HwRunFrame and End after it.
 */
void GfxSnapshotBegin(GfxSnapshot* snap, uint64_t frame);
void GfxSnapshotEnd(GfxSnapshot* snap);

uint16_t GfxReg16(const GfxSnapshot* snap, uint32_t offset);
// Palette entry (0-255 BG, 256-511 OBJ) as 15-bit BGR, and as 0xFFRRGGBB.
uint16_t GfxColor555(const GfxSnapshot* snap, int index);
uint32_t GfxRgb(uint16_t bgr555);

typedef struct {
    int index;
    uint16_t attr0, attr1, attr2;
    int x, y;              // signed screen position of the bounding box
    int width, height;     // sprite size in pixels
    int boxWidth, boxHeight; // on screen (twice the size with double-size)
    int shape, size;
    int tile, palette, priority;
    int mode;              // 0 normal, 1 semi-transparent, 2 OBJ window, 3 prohibited
    int affine, doubleSize, hidden, mosaic, is8bpp, hflip, vflip;
    int matrix;            // affine matrix index, -1 if not affine
    int16_t pa, pb, pc, pd; // 8.8 fixed point
    int visible;           // drawn at all (not hidden or invalid, box touches the screen)
} GfxObj;

void GfxDecodeObj(const GfxSnapshot* snap, int index, GfxObj* obj);

enum { GFX_BG_OFF, GFX_BG_TEXT, GFX_BG_AFFINE };

typedef struct {
    int bg;
    int kind;              // GFX_BG_*, from the display mode (GFX_BG_OFF if the mode has no such layer)
    int enabled;           // DISPCNT enables it
    uint16_t cnt;
    int priority, charBase, screenBase, is8bpp, mosaic, wrap, sizeIndex;
    int width, height;     // map size in pixels
    int hofs, vofs;        // text layers
    int16_t pa, pb, pc, pd; // affine layers, 8.8
    int32_t refX, refY;    // affine reference point, 20.8
} GfxBg;

void GfxDecodeBg(const GfxSnapshot* snap, int bg, GfxBg* out);

/*
 * Pictures, 0xFFRRGGBB per pixel (BGRA8 in memory).
 *
 * GfxRenderTiles draws count tiles starting at VRAM byte offset base, 32 tiles
 * per row (out is 256 x 8 * ceil(count / 32)). palette: 0-15 a BG palette, 16-31
 * an OBJ palette (4bpp); for 8bpp, 0-15 means the BG palette and 16-31 the OBJ
 * one; -1 is greyscale. Index 0 is drawn as is (not transparent).
 */
void GfxRenderTiles(const GfxSnapshot* snap, uint32_t base, int count, int is8bpp, int palette, uint32_t* out);

/*
 * The whole map of a layer (width x height of GfxDecodeBg, at most 1024 x 1024)
 * as the PPU would draw it with no scroll; transparent pixels get the backdrop
 * colour, or `transparent` if it is not 0. Returns 0 if the layer has no map
 * in this display mode.
 */
int GfxRenderBg(const GfxSnapshot* snap, int bg, uint32_t* out, uint32_t transparent);

/*
 * One sprite's pixels, unrotated (width x height of GfxDecodeObj, at most
 * 64 x 64); transparent pixels get `transparent`.
 */
void GfxRenderObj(const GfxSnapshot* snap, int index, uint32_t* out, uint32_t transparent);

/* Readable text dumps. */
void GfxDumpOam(const GfxSnapshot* snap, FILE* out, int all); // all = include hidden entries
void GfxDumpBg(const GfxSnapshot* snap, FILE* out);           // layer settings and map summaries
void GfxDumpPal(const GfxSnapshot* snap, FILE* out);
void GfxDumpIo(const GfxSnapshot* snap, FILE* out);           // DISPCNT, windows, blending, mosaic, DMA
void GfxDumpLines(const GfxSnapshot* snap, FILE* out);        // registers changed between lines

/* Hash of a buffer (FNV-1a, 64-bit), for comparing dumps. */
uint64_t GfxHash(const void* data, size_t size);

#endif // PORT_GFXDEBUG_H
