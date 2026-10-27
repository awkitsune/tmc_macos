#ifndef PORT_MAPOVERVIEW_H
#define PORT_MAPOVERVIEW_H

/*
 * The Map window's overview of a whole area or dungeon floor (port/src/mapoverview.c):
 * the area renderer's picture (port/arearender.h), scaled down by a whole factor until
 * it fits MAP_OVERVIEW_MAX_PIXELS, as 0xFFRRGGBB pixels; the full-size picture is freed
 * as soon as it is scaled. Hit tests and warp targets for the window's hover, double
 * click and menu. Any thread, like the renderer.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "port/arearender.h"

#define MAP_OVERVIEW_MAX_PIXELS (4 << 20) // 16 MB of 32-bit pixels; Palace of Winds (1920x9808) is scaled by 3
#define MAP_OVERVIEW_BACKGROUND 0xFF1F1F1Fu // between rooms (the window's background)

typedef enum {
    MAP_OVERVIEW_AREA = 1,
    MAP_OVERVIEW_FLOOR,
} MapOverviewKind;

typedef struct {
    MapOverviewKind kind;
    int area;           // MAP_OVERVIEW_AREA
    int dungeon, floor; // MAP_OVERVIEW_FLOOR
    int scale;          // a pixel here is scale x scale pixels of the full picture
    int width, height;
    uint32_t* pixels;   // width * height, 0xFFRRGGBB (BGRA in memory)
    uint64_t checksum;  // FNV-1a of pixels
    AreaMap map;        // the rooms, in full-size pixels; map.rgba is NULL, map.checksum the full picture's
} MapOverview;

// The smallest factor that brings width x height within MAP_OVERVIEW_MAX_PIXELS.
int MapOverviewScaleFor(int width, int height);

// Draw area (kind AREA) or dungeon a, floor b (kind FLOOR). False if there is nothing to draw.
bool MapOverviewRender(MapOverviewKind kind, int a, int b, MapOverview* out);
void MapOverviewFree(MapOverview* overview);

// The room under overview pixel x, y (the centre of the full-size pixels it covers): index into map.rooms, or -1.
int MapOverviewRoomAt(const MapOverview* overview, int x, int y);
// The room to warp to for overview pixel x, y: false if there is none or the game cannot load it. The room
// can be in another area than the overview's (a dungeon floor's boss room, say).
bool MapOverviewWarpTarget(const MapOverview* overview, int x, int y, int* area, int* room);
// Index of area:room in map.rooms, or -1.
int MapOverviewFindRoom(const MapOverview* overview, int area, int room);

// "Hyrule Field" (from the room names; "" if the area has none), "Deepwood Shrine".
void MapOverviewAreaName(int area, char* out, int size);
const char* MapOverviewDungeonName(int dungeon);
// "area 03 Hyrule Field", "dungeon 1 Deepwood Shrine floor 0 of 3".
void MapOverviewTitle(MapOverviewKind kind, int a, int b, char* out, int size);

// A line of numbers for the window's summary and the headless runner: what is drawn, the scale and sizes, and
// what is at overview pixel x, y.
void MapOverviewSummary(const MapOverview* overview, int x, int y, char* out, int size);

// TMC_MAPOVERVIEW (port/src/headless.c): draw every area and floor, check each room's hit test at its centre and
// its warp target, a line for each to out. Returns the number of problems.
int MapOverviewCheckAll(FILE* out);

#endif // PORT_MAPOVERVIEW_H
