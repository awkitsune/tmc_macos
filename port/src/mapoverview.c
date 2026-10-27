/* The Map window's area and floor overview (port/mapoverview.h). */

#include "port/mapoverview.h"

#include <stdlib.h>
#include <string.h>

#include "port/mapview.h"

const char* RoomName(int area, int room); // the generated room_names.c

int MapOverviewScaleFor(int width, int height) {
    int scale = 1;

    while ((int64_t)((width + scale - 1) / scale) * ((height + scale - 1) / scale) > MAP_OVERVIEW_MAX_PIXELS) {
        scale++;
    }
    return scale;
}

static uint64_t Fnv(const void* data, size_t size) {
    const uint8_t* p = data;
    uint64_t hash = 0xcbf29ce484222325ull;

    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ p[i]) * 0x100000001b3ull;
    }
    return hash;
}

// Each pixel the average of the opaque pixels of its scale x scale block, or the background
// when fewer than half of them are opaque (a block on a room's edge).
static void Shrink(const AreaMap* map, int scale, uint32_t* out, int width, int height) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t r = 0, g = 0, b = 0, opaque = 0, total = 0;
            for (int sy = y * scale; sy < y * scale + scale && sy < map->height; sy++) {
                for (int sx = x * scale; sx < x * scale + scale && sx < map->width; sx++) {
                    const uint8_t* p = &map->rgba[((size_t)sy * map->width + sx) * 4];
                    total++;
                    if (p[3] != 0) {
                        r += p[0];
                        g += p[1];
                        b += p[2];
                        opaque++;
                    }
                }
            }
            out[(size_t)y * width + x] =
                opaque * 2 < total
                    ? MAP_OVERVIEW_BACKGROUND
                    : 0xFF000000u | (r + opaque / 2) / opaque << 16 | (g + opaque / 2) / opaque << 8 |
                          (b + opaque / 2) / opaque;
        }
    }
}

bool MapOverviewRender(MapOverviewKind kind, int a, int b, MapOverview* out) {
    bool drawn;

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == MAP_OVERVIEW_AREA) {
        out->area = a;
        drawn = AreaMapRenderArea(a, &out->map);
    } else {
        out->dungeon = a;
        out->floor = b;
        drawn = AreaMapRenderFloor(a, b, &out->map);
    }
    if (!drawn || out->map.rgba == NULL) {
        AreaMapFree(&out->map);
        return false;
    }
    out->scale = MapOverviewScaleFor(out->map.width, out->map.height);
    out->width = (out->map.width + out->scale - 1) / out->scale;
    out->height = (out->map.height + out->scale - 1) / out->scale;
    out->pixels = malloc((size_t)out->width * out->height * sizeof(uint32_t));
    if (out->pixels != NULL) {
        Shrink(&out->map, out->scale, out->pixels, out->width, out->height);
        out->checksum = Fnv(out->pixels, (size_t)out->width * out->height * sizeof(uint32_t));
    }
    AreaMapFree(&out->map);
    return out->pixels != NULL;
}

void MapOverviewFree(MapOverview* overview) {
    free(overview->pixels);
    overview->pixels = NULL;
}

int MapOverviewRoomAt(const MapOverview* overview, int x, int y) {
    if (overview->scale <= 0 || x < 0 || y < 0 || x >= overview->width || y >= overview->height) {
        return -1;
    }
    return AreaMapRoomAt(&overview->map, x * overview->scale + overview->scale / 2,
                         y * overview->scale + overview->scale / 2);
}

bool MapOverviewWarpTarget(const MapOverview* overview, int x, int y, int* area, int* room) {
    int i = MapOverviewRoomAt(overview, x, y);

    if (i < 0 || !WarpRoomIsValid(overview->map.rooms[i].area, overview->map.rooms[i].room)) {
        return false;
    }
    *area = overview->map.rooms[i].area;
    *room = overview->map.rooms[i].room;
    return true;
}

int MapOverviewFindRoom(const MapOverview* overview, int area, int room) {
    for (int i = 0; i < overview->map.roomCount; i++) {
        if (overview->map.rooms[i].area == area && overview->map.rooms[i].room == room) {
            return i;
        }
    }
    return -1;
}

void MapOverviewAreaName(int area, char* out, int size) {
    out[0] = 0;
    for (int room = 0; room < AreaMapRoomCount(area); room++) {
        const char* name = RoomName(area, room);
        if (name != NULL) {
            const char* dash = strstr(name, " \xe2\x80\x93 "); // "Area – Room"
            snprintf(out, size, "%.*s", dash != NULL ? (int)(dash - name) : (int)strlen(name), name);
            return;
        }
    }
}

const char* MapOverviewDungeonName(int dungeon) {
    static const char* const names[] = {
        NULL,          "Deepwood Shrine", "Cave of Flames",    "Fortress of Winds",
        "Temple of Droplets", "Palace of Winds", "Dark Hyrule Castle",
    };
    return dungeon >= 1 && dungeon < (int)(sizeof(names) / sizeof(names[0])) ? names[dungeon] : "";
}

void MapOverviewTitle(MapOverviewKind kind, int a, int b, char* out, int size) {
    if (kind == MAP_OVERVIEW_AREA) {
        char name[96];
        MapOverviewAreaName(a, name, sizeof(name));
        snprintf(out, size, "area %02x%s%s", a, name[0] ? " " : "", name);
    } else {
        snprintf(out, size, "dungeon %d %s floor %d of %d", a, MapOverviewDungeonName(a), b, AreaMapFloorCount(a));
    }
}

void MapOverviewSummary(const MapOverview* overview, int x, int y, char* out, int size) {
    char title[160];
    int n, i, area, room;

    MapOverviewTitle(overview->kind, overview->kind == MAP_OVERVIEW_AREA ? overview->area : overview->dungeon,
                     overview->floor, title, sizeof(title));
    n = snprintf(out, size, "overview %s: scale 1/%d %dx%d (full %dx%d) rooms %d checksum %016llx; at %d,%d: ",
                 title, overview->scale, overview->width, overview->height, overview->map.width,
                 overview->map.height, overview->map.roomCount, (unsigned long long)overview->checksum, x, y);
    if (n < 0 || n >= size) {
        return;
    }
    i = MapOverviewRoomAt(overview, x, y);
    if (i < 0) {
        snprintf(out + n, size - n, "no room");
        return;
    }
    snprintf(out + n, size - n, "room %02x:%02x %s, warp %s", overview->map.rooms[i].area, overview->map.rooms[i].room,
             overview->map.rooms[i].name ?: "(unnamed)",
             MapOverviewWarpTarget(overview, x, y, &area, &room) ? "ok" : "not possible");
}

// Each room's centre hits that room (or one drawn over it there), and it is a warp target.
static int CheckOne(MapOverview* o, FILE* out, int* crossArea) {
    int problems = 0, covered = 0, home = o->map.roomCount > 0 ? o->map.rooms[0].area : -1;
    char line[512];

    *crossArea = 0;
    if ((int64_t)o->width * o->height > MAP_OVERVIEW_MAX_PIXELS || o->scale != MapOverviewScaleFor(o->map.width, o->map.height)) {
        fprintf(out, "mapoverview: PROBLEM: %dx%d at scale %d is over the budget\n", o->width, o->height, o->scale);
        problems++;
    }
    for (int i = 0; i < o->map.roomCount; i++) {
        const AreaMapRoom* r = &o->map.rooms[i];
        int x = (r->x + r->width / 2) / o->scale, y = (r->y + r->height / 2) / o->scale, area, room;
        int hit = MapOverviewRoomAt(o, x, y);
        const AreaMapRoom* h = hit >= 0 ? &o->map.rooms[hit] : NULL;
        int fx = x * o->scale + o->scale / 2, fy = y * o->scale + o->scale / 2;
        if (hit > i && fx >= h->x && fy >= h->y && fx < h->x + h->width && fy < h->y + h->height &&
            fx >= r->x && fy >= r->y && fx < r->x + r->width && fy < r->y + r->height) {
            covered++; // a later room drawn over this one's centre (Fortress of Winds' thin overlaps)
        } else if (hit != i) {
            fprintf(out, "mapoverview: PROBLEM: room %02x:%02x centre %d,%d hits %d\n", r->area, r->room, x, y, hit);
            problems++;
        }
        if (hit == i && (!MapOverviewWarpTarget(o, x, y, &area, &room) || area != r->area || room != r->room)) {
            fprintf(out, "mapoverview: PROBLEM: room %02x:%02x is no warp target\n", r->area, r->room);
            problems++;
        }
        *crossArea += r->area != home;
    }
    MapOverviewSummary(o, -1, -1, line, sizeof(line));
    fprintf(out, "mapoverview: %.*s covered %d other-area %d\n", (int)(strstr(line, "; at") - line), line, covered,
            *crossArea);
    return problems;
}

int MapOverviewCheckAll(FILE* out) {
    MapOverview o;
    int problems = 0, maps = 0, rooms = 0, scaled = 0, crossArea = 0, cross;
    int64_t largest = 0;

    for (int area = 0; area < AreaMapAreaCount(); area++) {
        if (MapOverviewRender(MAP_OVERVIEW_AREA, area, 0, &o)) {
            problems += CheckOne(&o, out, &cross);
            maps++;
            rooms += o.map.roomCount;
            scaled += o.scale > 1;
            largest = (int64_t)o.width * o.height > largest ? (int64_t)o.width * o.height : largest;
            MapOverviewFree(&o);
        }
    }
    for (int dungeon = 1; dungeon < AreaMapDungeonCount(); dungeon++) {
        for (int floor = 0; floor < AreaMapFloorCount(dungeon); floor++) {
            if (MapOverviewRender(MAP_OVERVIEW_FLOOR, dungeon, floor, &o)) {
                problems += CheckOne(&o, out, &cross);
                maps++;
                crossArea += cross;
                scaled += o.scale > 1;
                largest = (int64_t)o.width * o.height > largest ? (int64_t)o.width * o.height : largest;
                MapOverviewFree(&o);
            }
        }
    }
    fprintf(out,
            "mapoverview: %d maps, %d area rooms, %d scaled down, largest %lld pixels (%lld KB), %d floor rooms "
            "from another area, %d problems\n",
            maps, rooms, scaled, (long long)largest, (long long)largest * 4 / 1024, crossArea, problems);
    return problems;
}
