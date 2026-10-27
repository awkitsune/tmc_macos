/*
 * TMC_MAPDUMP: render the whole current room (bottom and top tile layers, from
 * the metatile map and the tile graphics in VRAM) as a PNG, with entity
 * markers and a grid, for planning headless input scripts. Room-relative
 * coordinates: thin grid lines every 16 pixels, brighter every 64 (with a mark
 * every 256). Markers: player white, NPCs blue, enemies red, objects yellow,
 * other entities grey. Fully solid tiles (bottom layer collision 0x0F) are
 * darkened.
 *
 * MapRenderRoom draws that picture into a buffer, and MapRenderLayer a single
 * layer, for the macOS app's Map window too (port/mapview.h).
 */

#include <stdio.h>

#include "global.h"
#include "entity.h"
#include "map.h"
#include "player.h"
#include "room.h"
#include "screen.h"

#include "port/hw.h"
#include "port/mapview.h"
#include "port/ppu.h"

// <stdlib.h> clashes with the game headers.
void* malloc(unsigned long size);
void free(void* p);

static u16 Bgr(int r, int g, int b) {
    return (u16)(r | (g << 5) | (b << 10));
}

static int TilePixel(u32 charBase, u32 tile, int x, int y) {
    u32 addr = charBase + tile * 32 + y * 4 + x / 2;
    if (addr >= 0x10000) {
        return 0;
    }
    return (x & 1) ? gHwVram[addr] >> 4 : gHwVram[addr] & 0xF;
}

static void DrawLayer(MapLayer* layer, u16* fb, int width, int height, int tilesWide, int tilesHigh) {
    u32 charBase;
    if (layer->bgSettings == NULL) {
        return;
    }
    charBase = ((layer->bgSettings->control >> 2) & 3) * 0x4000;
    for (int ty = 0; ty < tilesHigh; ty++) {
        for (int tx = 0; tx < tilesWide; tx++) {
            u32 index = layer->mapData[ty * MAX_MAP_SIZE + tx];
            if (index >= TILESET_SIZE) {
                continue; // special tiles
            }
            for (int k = 0; k < 4; k++) {
                u16 entry = layer->subTiles[index * 4 + k];
                int ox = tx * 16 + (k & 1) * 8, oy = ty * 16 + (k >> 1) * 8;
                for (int y = 0; y < 8; y++) {
                    for (int x = 0; x < 8; x++) {
                        int px = (entry & 0x400) ? 7 - x : x, py = (entry & 0x800) ? 7 - y : y;
                        int c = TilePixel(charBase, entry & 0x3FF, px, py);
                        if (c != 0 && ox + x < width && oy + y < height) {
                            const u8* p = &gHwPltt[((entry >> 12) * 16 + c) * 2];
                            fb[(oy + y) * width + ox + x] = (p[0] | (p[1] << 8)) & 0x7FFF;
                        }
                    }
                }
            }
        }
    }
}

static void Mark(u16* fb, int width, int height, int cx, int cy, int size, u16 color) {
    for (int y = cy - size; y <= cy + size; y++) {
        for (int x = cx - size; x <= cx + size; x++) {
            if (x >= 0 && y >= 0 && x < width && y < height &&
                (x == cx - size || x == cx + size || y == cy - size || y == cy + size)) {
                fb[y * width + x] = color;
            }
        }
    }
}

// Draws one tile layer's opaque pixels (BGR555, as the palette holds them) over fb, a picture of the
// room's top-left width x height pixels.
void MapRenderLayer(int top, uint16_t* fb, int width, int height) {
    int tilesWide = (width + 15) / 16, tilesHigh = (height + 15) / 16;

    DrawLayer(top ? &gMapTop : &gMapBottom, fb, width, height, tilesWide, tilesHigh);
}

int MapRenderRoom(uint16_t* fb, int width, int height) {
    int tilesWide = (width + 15) / 16, tilesHigh = (height + 15) / 16;
    const u8* backdrop = &gHwPltt[0];

    if (width <= 0 || height <= 0 || tilesWide > MAX_MAP_SIZE || tilesHigh > MAX_MAP_SIZE) {
        return 0;
    }
    for (int i = 0; i < width * height; i++) {
        fb[i] = backdrop[0] | (backdrop[1] << 8);
    }
    DrawLayer(&gMapBottom, fb, width, height, tilesWide, tilesHigh);
    DrawLayer(&gMapTop, fb, width, height, tilesWide, tilesHigh);

    // Darken fully blocked tiles (collision 0x0F).
    for (int ty = 0; ty < tilesHigh; ty++) {
        for (int tx = 0; tx < tilesWide; tx++) {
            if (gMapBottom.collisionData[ty * MAX_MAP_SIZE + tx] != 0x0F) {
                continue;
            }
            for (int y = ty * 16; y < ty * 16 + 16 && y < height; y++) {
                for (int x = tx * 16; x < tx * 16 + 16 && x < width; x++) {
                    u16 c = fb[y * width + x];
                    fb[y * width + x] = ((c >> 1) & 0x3DEF) & ~0x1F; // half brightness, red dropped
                }
            }
        }
    }
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (x % 64 == 0 || y % 64 == 0) {
                fb[y * width + x] = (x % 256 == 0 && y % 256 == 0) ? Bgr(31, 0, 31) : Bgr(31, 31, 31);
            } else if ((x % 16 == 0 || y % 16 == 0) && ((x + y) & 1)) {
                fb[y * width + x] = Bgr(16, 16, 16);
            }
        }
    }
    for (int list = 0; list < 9; list++) {
        for (Entity* e = gEntityLists[list].first; e && e != (Entity*)&gEntityLists[list]; e = e->next) {
            int x = e->x.HALF.HI - gRoomControls.origin_x, y = e->y.HALF.HI - gRoomControls.origin_y;
            u16 color = e->kind == PLAYER ? Bgr(31, 31, 31)
                        : e->kind == NPC  ? Bgr(0, 8, 31)
                        : e->kind == ENEMY ? Bgr(31, 0, 0)
                        : e->kind == OBJECT ? Bgr(31, 31, 0)
                                            : Bgr(20, 20, 20);
            if (e->kind == MANAGER) {
                continue;
            }
            Mark(fb, width, height, x, y, e->kind == PLAYER ? 6 : 4, color);
            Mark(fb, width, height, x, y, e->kind == PLAYER ? 5 : 3, color);
        }
    }
    return 1;
}

void DumpRoomMap(const char* path) {
    int width = gRoomControls.width, height = gRoomControls.height;
    u16* fb;

    if (width <= 0 || height <= 0) {
        return;
    }
    fb = malloc(sizeof(u16) * width * height);
    if (MapRenderRoom(fb, width, height) && PpuWritePngSize(fb, width, height, path) == 0) {
        printf("map: %s (%dx%d)\n", path, width, height);
    }
    free(fb);
}
