#ifndef PORT_AREARENDER_H
#define PORT_AREARENDER_H

/*
 * Draws any room, a whole area or a dungeon floor from the game's data, for
 * maps and overviews (port/src/arearender.c). Each room's tile set, palettes,
 * metatiles and tile maps are decoded into the renderer's own memory, the way
 * the game's room loading does (LoadRoomTileSet, LoadRoomGfx), so nothing of
 * the running game is read or written but its constant data: it can run from
 * any thread at any time. What it draws is each room as it is when loaded,
 * bottom and top tile layers: no entities, no tiles changed at run time
 * (opened doors, chests, cut grass), no animated tiles past their first frame,
 * no BG3 backgrounds. port/notes/area-render.md has the coverage and the gaps.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define AREA_MAP_MAX_ROOMS 64

typedef struct {
    int area;
    int room;
    int x, y;          /* top left in the image, pixels */
    int width, height; /* pixels */
    int worldX, worldY; /* the room header's map position */
    const char* name;  /* "Area – Room" (RoomName), or NULL */
    bool atMapPosition; /* false: the room would cover more than half of an earlier one (or that one of it) at
                           its map position, so it is in a row under the others */
} AreaMapRoom;

typedef struct {
    int width, height;
    uint8_t* rgba;   /* width * height * 4: R, G, B, A; A = 0 outside the rooms */
    uint64_t checksum; /* FNV-1a of rgba */
    int roomCount;
    AreaMapRoom rooms[AREA_MAP_MAX_ROOMS];
    int moved;       /* rooms not at their map position (see AreaMapRoom.atMapPosition) */
    int overlaps;    /* pairs of rooms that overlap a little at their map positions (the later is drawn on top) */
    int skipped;     /* room headers that cannot be drawn (no tile set, no map, unused) */
} AreaMap;

/* Number of areas with room tables (0 to AREA_8F). */
int AreaMapAreaCount(void);
/* Number of room headers of an area, drawable or not. */
int AreaMapRoomCount(int area);
/* A room the renderer (and the game) can load: see AreaMapRenderArea. */
bool AreaMapRoomIsDrawable(int area, int room);

/* One room; out has one entry in rooms. False if the room cannot be drawn. */
bool AreaMapRenderRoom(int area, int room, AreaMap* out);
/* Every drawable room of an area at its map position (offset so the top left
   room touches 0,0), but for rooms that share a place with another (moved to
   rows below). False if the area has none. */
bool AreaMapRenderArea(int area, AreaMap* out);
/* Dungeons 1 (Deepwood Shrine) to 6 (Dark Hyrule Castle), as the pause menu's
   map lists them: floor 0 is the highest. A floor can take rooms from several
   areas (the boss room). */
int AreaMapDungeonCount(void);
int AreaMapFloorCount(int dungeon);
bool AreaMapRenderFloor(int dungeon, int floor, AreaMap* out);
/* The dungeon floor whose map has area:room (the first, if several do). */
bool AreaMapFloorOf(int area, int room, int* dungeon, int* floor);
void AreaMapFree(AreaMap* map);
/* Index into rooms of the room drawn at image pixel x, y, or -1. */
int AreaMapRoomAt(const AreaMap* map, int x, int y);

/* Headless checks (port/src/headless.c). TMC_AREADUMP: every area and dungeon
   floor into dir as area_AA.png and dungeon_D_floor_F.png, a line for each to
   out (and one for each room with listRooms); returns the number of areas drawn. */
int AreaMapDumpAll(const char* dir, bool listRooms, FILE* out);
/* TMC_AREACHECK: compare the renderer's picture of the current room with one
   drawn from the live tile maps, VRAM and palettes (as TMC_MAPDUMP draws its
   tile layers), and say what differs. Reads the game's state only. */
void AreaMapCheckLive(FILE* out);

#endif // PORT_AREARENDER_H
