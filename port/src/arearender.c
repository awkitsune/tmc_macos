/*
 * Rooms, areas and dungeon floors drawn from the game's data, without the game
 * (port/include/port/arearender.h). A room is loaded the way LoadRoomTileSet
 * and LoadRoomGfx load it, but every MapDataDefinition's destination (VRAM,
 * gMapBottom, gMapTop, gMapData*Special) and every palette group is redirected
 * into a Scratch of the renderer's own; then the layers are drawn as
 * TMC_MAPDUMP draws them (port/src/mapdump.c): backdrop, bottom layer, top
 * layer, colour 0 transparent.
 */

#include <string.h>

#include "area.h"
#include "assets/gfx_offsets.h"
#include "backgroundAnimations.h"
#include "common.h"
#include "global.h"
#include "map.h"
#include "room.h"
#include "tileMap.h"

#include "port/arearender.h"
#include "port/hw.h"
#include "port/ppu.h"
#include "port/state.h"

// <stdlib.h> clashes with the game headers.
void* calloc(unsigned long count, unsigned long size);
void* malloc(unsigned long size);
void free(void* p);

extern void** gAreaTileSets[];
extern void** gAreaRoomMaps[];
extern void* gAreaTiles[];
extern void** gAreaTable[];
extern u8 gMapData;
extern const DungeonLayout* const* const gDungeonLayouts[];

// common.c's, not in a header.
typedef struct {
    u16 paletteId;
    u8 destPaletteNum;
    u8 numPalettes;
} ArPaletteGroup;
extern const ArPaletteGroup* gPaletteGroups[];
extern const BgAnimationFrame* const gUnk_080B7278[];
extern const u8 gGlobalGfxAndPalettes[];

const char* RoomName(int area, int room); // the generated room_names.c
extern const int gAreaMapAreaCount;        // the generated area_rooms.c
extern const uint64_t gAreaMapLoadableRooms[];

// gAreaRoomHeaders (data/map/room_headers.s) has an entry for areas 0 to 0x8f only.
#define AREA_COUNT (AREA_8F + 1)
#define DUNGEON_COUNT 7
#define BG_VRAM_SIZE 0x10000
#define AR_MIN(a, b) ((a) < (b) ? (a) : (b))
#define AR_MAX(a, b) ((a) > (b) ? (a) : (b))

// Everything a room load writes, in the renderer's memory.
typedef struct {
    u8 vram[HW_VRAM_SIZE];
    MapLayer layer[2]; // bottom, top
    u16 special[2][0x4000];
    u16 pal[512];
    u16 copy[0x2000]; // for Arrange
    int unknownDest; // definitions whose destination is none of the above (skipped)
    int overflow;    // definitions that would write past their destination (skipped)
} Scratch;

// What DrawRoom draws from: the scratch, the live game or a mix (AreaMapCheckLive).
typedef struct {
    const u16* mapData[2];  // 64 metatiles a row
    const u16* subTiles[2]; // 4 entries a metatile
    const u16* special[2];  // bitmap rooms: 128 8x8 tiles a row
    const u8* vram;
    const u16* pal;
    u16 control[2];         // BGxCNT of the bottom (BG2) and top (BG1) layer
    bool bitmap;
    bool drawTop;
} Layers;

int AreaMapAreaCount(void) {
    return AREA_COUNT;
}

int AreaMapRoomCount(int area) {
    const RoomHeader* header;
    int count = 0;

    if (area < 0 || area >= AREA_COUNT || (header = gAreaRoomHeaders[area]) == NULL) {
        return 0;
    }
    while (count < MAX_ROOMS && header[count].map_x != 0xFFFF) {
        count++;
    }
    return count;
}

bool AreaMapRoomIsDrawable(int area, int room) {
    const RoomHeader* header;

    // A tile set, room properties (the unused rooms have none), a map and a tile set entry, as the room smoke
    // test's list: past the end entries of gAreaRoomMaps and gAreaTileSets look like real ones here.
    if (area < 0 || area >= AREA_COUNT || area >= gAreaMapAreaCount || room < 0 || room >= AreaMapRoomCount(area) ||
        !(gAreaMapLoadableRooms[area] >> room & 1)) {
        return false;
    }
    header = &gAreaRoomHeaders[area][room];
    return header->pixel_width > 0 && header->pixel_height > 0 && header->pixel_width <= MAX_MAP_SIZE * 16 &&
           header->pixel_height <= MAX_MAP_SIZE * 16;
}

// The destination in the scratch of size bytes at dest, or NULL.
static void* Redirect(Scratch* s, const void* dest, u32 size) {
    const struct {
        const void* live;
        size_t size;
        void* scratch;
    } regions[] = {
        { gHwVram, sizeof(s->vram), s->vram },
        { &gMapBottom, sizeof(MapLayer), &s->layer[0] },
        { &gMapTop, sizeof(MapLayer), &s->layer[1] },
        { gMapDataBottomSpecial, sizeof(s->special[0]), s->special[0] },
        { gMapDataTopSpecial, sizeof(s->special[1]), s->special[1] },
    };

    for (u32 i = 0; i < ARRAY_COUNT(regions); i++) {
        uintptr_t offset = (uintptr_t)dest - (uintptr_t)regions[i].live;
        if ((uintptr_t)dest >= (uintptr_t)regions[i].live && offset < regions[i].size) {
            if (offset + size > regions[i].size) {
                s->overflow++;
                return NULL;
            }
            return (u8*)regions[i].scratch + offset;
        }
    }
    s->unknownDest++;
    return NULL;
}

// The BIOS's LZ77 (port/src/bios.c), writing at most size bytes.
static void Lz77(const u8* src, u8* dest, u32 size) {
    u32 done = 0;

    src += 4;
    while (done < size) {
        u8 flags = *src++;
        for (int i = 0; i < 8 && done < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (src[0] >> 4) + 3;
                u32 disp = (((src[0] & 0xF) << 8) | src[1]) + 1;
                src += 2;
                for (; len > 0 && done < size; len--, done++) {
                    dest[done] = disp <= done ? dest[done - disp] : 0;
                }
            } else {
                dest[done++] = *src++;
            }
        }
    }
}

// LoadPaletteGroup, into pal.
static void PaletteGroup(u16* pal, u32 group) {
    const ArPaletteGroup* p = gPaletteGroups[group];

    while (p != NULL) {
        u32 count = p->numPalettes & 0xF ? p->numPalettes & 0xF : 16;
        u32 dest = p->destPaletteNum * 16;
        if (dest + count * 16 <= 512) {
            memcpy(&pal[dest], &gGlobalGfxAndPalettes[p->paletteId * 32], count * 32);
        }
        p = p->numPalettes & 0x80 ? p + 1 : NULL;
    }
}

// sub_080533CC, after each palette group of a map data list.
static void CopyObjColors(Scratch* s) {
    memcpy(&s->pal[32], &s->pal[288], 5 * sizeof(u16));
}

// LoadMapData into the scratch.
static void LoadDefinitions(Scratch* s, const MapDataDefinition* def) {
    if (def == NULL) {
        return;
    }
    do {
        if (def->dest != NULL) {
            const u8* src = &gMapData + (def->src & 0x7FFFFFFF);
            bool compressed = (def->size & MAP_COMPRESSED) != 0;
            u32 size = compressed ? (src[1] | (src[2] << 8) | (src[3] << 16)) : def->size;
            void* dest = Redirect(s, def->dest, size);
            if (dest != NULL) {
                if (compressed) {
                    Lz77(src, dest, size);
                } else {
                    memcpy(dest, src, size);
                }
            }
        } else {
            PaletteGroup(s->pal, *(const u16*)def);
            CopyObjColors(s);
        }
        def++;
    } while ((def - 1)->src & MAP_MULTIPLE);
}

// sub_0807C8B0: a map stored width x height to 64 tiles a row, the rest cleared.
static void Spread(u16* data, int width, int height) {
    for (int y = height - 1; y >= 0; y--) {
        for (int x = width - 1; x >= 0; x--) {
            data[y * MAX_MAP_SIZE + x] = data[y * width + x];
        }
    }
    for (int y = 0; y < MAX_MAP_SIZE; y++) {
        for (int x = y < height ? width : 0; x < MAX_MAP_SIZE; x++) {
            data[y * MAX_MAP_SIZE + x] = 0;
        }
    }
}

// sub_0807C5F4: a bitmap room's 32x32 screen blocks (two or four for a room over 255 pixels) to 128 tiles a row.
static void Arrange(u16* special, u16* copy, int width, int height) {
    const u16* src = copy;
    int blocks[4][2] = { { 0, 0 }, { 32, 0 }, { 0, 32 }, { 32, 32 } };
    bool use[4] = { true, width > 0xFF, height > 0xFF, width > 0xFF && height > 0xFF };

    memcpy(copy, special, 0x2000 * sizeof(u16));
    for (int b = 0; b < 4; b++) {
        if (!use[b]) {
            continue;
        }
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                special[(blocks[b][1] + y) * 128 + blocks[b][0] + x] = *src++;
            }
        }
    }
    memset(&special[0x2000], 0, 0x4000);
}

// The BG control bits, and whether BG1 is shown, as LoadRoomGfx sets them.
static void AreaControls(int area, u16 control[2], bool* drawTop) {
    control[0] = gUnk_080B77C0[0];
    control[1] = gUnk_080B77C0[1];
    *drawTop = true;
    switch (area) {
        case AREA_MINISH_HOUSE_INTERIORS:
        case AREA_TOWN_MINISH_HOLES:
            control[0] |= 0x80; // 256 colours
            *drawTop = false;
            break;
        case AREA_HOUSE_INTERIORS_1:
        case AREA_HOUSE_INTERIORS_2:
        case AREA_HOUSE_INTERIORS_3:
        case AREA_TREE_INTERIORS:
        case AREA_DOJOS:
        case AREA_MINISH_CRACKS:
        case AREA_HOUSE_INTERIORS_4:
        case AREA_WIND_TRIBE_TOWER:
        case AREA_EZLO_CUTSCENE:
            control[1] = gUnk_080B77C0[2];
            break;
    }
}

// The room's data into s, as entering it does. The palette starts as LoadGfxGroups leaves it.
static bool ScratchLoadRoom(Scratch* s, int area, int room, Layers* l) {
    const RoomHeader* header = &gAreaRoomHeaders[area][room];
    int width = header->pixel_width / 16, height = header->pixel_height / 16;

    if (!AreaMapRoomIsDrawable(area, room)) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    PaletteGroup(s->pal, 11);
    PaletteGroup(s->pal, 12);
    s->pal[0] = 0;
    LoadDefinitions(s, gAreaTileSets[area][header->tileSet_id]);
    LoadDefinitions(s, gAreaTiles[area]);
    LoadDefinitions(s, gAreaRoomMaps[area][room]);

    memset(l, 0, sizeof(*l));
    l->bitmap = s->layer[0].mapData[0] == 0xFFFF;
    if (!l->bitmap) {
        Spread(s->layer[0].mapData, width, height);
        Spread(s->layer[1].mapData, width, height);
    } else {
        Arrange(s->special[0], s->copy, header->pixel_width, header->pixel_height);
        Arrange(s->special[1], s->copy, header->pixel_width, header->pixel_height);
    }
    for (int i = 0; i < 2; i++) {
        l->mapData[i] = s->layer[i].mapData;
        l->subTiles[i] = s->layer[i].subTiles;
        l->special[i] = s->special[i];
    }
    l->vram = s->vram;
    l->pal = s->pal;
    AreaControls(area, l->control, &l->drawTop);
    return true;
}

// Hyrule Town, Festival Town and Minish Village swap tile graphics as the camera moves
// (hyruleTownTileSetManager.c, minishVillageTileSetManager.c): one to three slots of BG VRAM, each loaded with the
// group of the first of its regions on screen. Hyrule Town's tables are static there, so they are copied here.
typedef struct {
    u32 gfx1, dest1, gfx2, dest2;
} TownGfx;
static const u16 sTownRegions[2][3][11] = {
    {
        { 0, 0x000, 0x000, 0x3f0, 0x200, 1, 0x000, 0x280, 0x3f0, 0x140, 0xff },
        { 2, 0x000, 0x000, 0x180, 0x3c0, 3, 0x280, 0x000, 0x170, 0x3c0, 0xff },
        { 5, 0x130, 0x1b0, 0x190, 0x140, 4, 0x000, 0x000, 0x3f0, 0x3c0, 0xff },
    },
    {
        { 0, 0x000, 0x000, 0x190, 0x1d0, 1, 0x000, 0x2a0, 0x190, 0x120, 0xff },
        { 0xff },
        { 5, 0x000, 0x1b0, 0x190, 0x140, 4, 0x000, 0x000, 0x190, 0x3c0, 0xff },
    },
};
static const TownGfx sTownGfx[2][6] = {
    {
        { offset_gUnk_086D4460 + 0x8000, 0x0000, offset_gUnk_086D4460 + 0xE000, 0x8000 },
        { offset_gUnk_086D4460 + 0xB000, 0x0000, offset_gUnk_086D4460 + 0x11000, 0x8000 },
        { offset_gUnk_086D4460 + 0x9000, 0x1000, offset_gUnk_086D4460 + 0xF000, 0x9000 },
        { offset_gUnk_086D4460 + 0xC000, 0x1000, offset_gUnk_086D4460 + 0x12000, 0x9000 },
        { offset_gUnk_086D4460 + 0xA000, 0x2000, offset_gUnk_086D4460 + 0x10000, 0xA000 },
        { offset_gUnk_086D4460 + 0xD000, 0x2000, offset_gUnk_086D4460 + 0x13000, 0xA000 },
    },
    {
        { offset_gUnk_086E8460 + 0x800, 0x0000, offset_gUnk_086E8460 + 0x6800, 0x8000 },
        { offset_gUnk_086E8460 + 0x3800, 0x0000, offset_gUnk_086E8460 + 0x9800, 0x8000 },
        { offset_gUnk_086E8460 + 0x1800, 0x1000, offset_gUnk_086E8460 + 0x7800, 0x9000 },
        { offset_gUnk_086E8460 + 0x4800, 0x1000, offset_gUnk_086E8460 + 0xA800, 0x9000 },
        { offset_gUnk_086E8460 + 0x2800, 0x2000, offset_gUnk_086E8460 + 0x8800, 0xA000 },
        { offset_gUnk_086E8460 + 0x5800, 0x2000, offset_gUnk_086E8460 + 0xB800, 0xA000 },
    },
};

typedef struct {
    u32 gfx;
    void* dest;
} VillageGfx;
extern const u16 gMinishVillageTileSetManagerRegions[];
extern const VillageGfx gMinishVillageTileSetManagerGfxInfos[][8];
extern const u8 gMinishVillateTileSetManagerPaletteGroups[];
#define VILLAGE_GROUPS 5

// The region lists of an area's slots; 0 slots for an area without.
static int RegionSlots(int area, const u16* regions[3]) {
    int town = area == AREA_HYRULE_TOWN ? 0 : area == AREA_FESTIVAL_TOWN ? 1 : -1;

    if (town >= 0) {
        for (int slot = 0; slot < 3; slot++) {
            regions[slot] = sTownRegions[town][slot];
        }
        return 3;
    }
    if (area == AREA_MINISH_VILLAGE) {
        regions[0] = gMinishVillageTileSetManagerRegions;
        return 1;
    }
    return 0;
}

// CheckRegionsOnScreen for a screen centred on room pixel x, y.
static u32 RegionGroup(const u16* regions, int x, int y) {
    for (const u16* r = regions; *r != 0xff; r += 5) {
        u32 sx = (u32)(x - DISPLAY_WIDTH / 2 - r[1] + DISPLAY_WIDTH);
        u32 sy = (u32)(y - DISPLAY_HEIGHT / 2 - r[2] + DISPLAY_HEIGHT);
        if (sx < r[3] + DISPLAY_WIDTH && sy < r[4] + DISPLAY_HEIGHT) {
            return r[0];
        }
    }
    return 0xff;
}

// The managers' LoadGfxGroup for each slot's group (0xff: leave the slot alone).
static void LoadRegionGroups(int area, const u8* groups, u8* vram, u16* pal) {
    if (area == AREA_MINISH_VILLAGE) {
        if (groups[0] < VILLAGE_GROUPS) {
            PaletteGroup(pal, gMinishVillateTileSetManagerPaletteGroups[groups[0]]);
            for (int i = 0; i < 8; i++) {
                const VillageGfx* g = &gMinishVillageTileSetManagerGfxInfos[groups[0]][i];
                memcpy(&vram[(uintptr_t)g->dest - (uintptr_t)BG_VRAM], &gGlobalGfxAndPalettes[g->gfx],
                       BG_SCREEN_SIZE * 2);
            }
        }
        return;
    }
    for (int slot = 0; slot < 3; slot++) {
        int town = area == AREA_HYRULE_TOWN ? 0 : 1;
        if (groups[slot] < ARRAY_COUNT(sTownGfx[town])) {
            const TownGfx* g = &sTownGfx[town][groups[slot]];
            memcpy(&vram[g->dest1], &gGlobalGfxAndPalettes[g->gfx1], BG_SCREEN_SIZE * 2);
            memcpy(&vram[g->dest2], &gGlobalGfxAndPalettes[g->gfx2], BG_SCREEN_SIZE * 2);
        }
    }
}

static void DrawTile(const Layers* l, u16 entry, u32 charBase, bool bpp8, u16* fb, int width, int height, int ox,
                     int oy) {
    for (int y = 0; y < 8 && oy + y < height; y++) {
        for (int x = 0; x < 8 && ox + x < width; x++) {
            int px = (entry & 0x400) ? 7 - x : x, py = (entry & 0x800) ? 7 - y : y;
            u32 c;
            if (bpp8) {
                u32 addr = charBase + (entry & 0x3FF) * 64 + py * 8 + px;
                c = addr < BG_VRAM_SIZE ? l->vram[addr] : 0;
            } else {
                u32 addr = charBase + (entry & 0x3FF) * 32 + py * 4 + px / 2;
                c = addr < BG_VRAM_SIZE ? (px & 1 ? l->vram[addr] >> 4 : l->vram[addr] & 0xF) : 0;
                c = c != 0 ? (entry >> 12) * 16 + c : 0;
            }
            if (c != 0) {
                fb[(oy + y) * width + ox + x] = l->pal[c];
            }
        }
    }
}

static void DrawRoom(const Layers* l, u16* fb, int width, int height) {
    for (int i = 0; i < width * height; i++) {
        fb[i] = l->pal[0];
    }
    for (int layer = 0; layer < 2; layer++) {
        u32 charBase = ((l->control[layer] >> 2) & 3) * 0x4000;
        bool bpp8 = (l->control[layer] & 0x80) != 0;
        if (layer == 1 && !l->drawTop) {
            continue;
        }
        if (l->bitmap) {
            for (int ty = 0; ty * 8 < height && ty < 128; ty++) {
                for (int tx = 0; tx * 8 < width && tx < 128; tx++) {
                    DrawTile(l, l->special[layer][ty * 128 + tx], charBase, bpp8, fb, width, height, tx * 8, ty * 8);
                }
            }
            continue;
        }
        for (int ty = 0; ty * 16 < height && ty < MAX_MAP_SIZE; ty++) {
            for (int tx = 0; tx * 16 < width && tx < MAX_MAP_SIZE; tx++) {
                u32 index = l->mapData[layer][ty * MAX_MAP_SIZE + tx];
                if (index >= TILESET_SIZE) {
                    continue; // special tiles: set at run time
                }
                for (int k = 0; k < 4; k++) {
                    DrawTile(l, l->subTiles[layer][index * 4 + k], charBase, bpp8, fb, width, height,
                             tx * 16 + (k & 1) * 8, ty * 16 + (k >> 1) * 8);
                }
            }
        }
    }
}

// A room with region graphics: each pixel as it looks with the groups loaded for a screen centred on it.
static void DrawRegions(Scratch* s, const Layers* base, int area, u16* fb, int width, int height) {
    const u16* regions[3];
    int slots = RegionSlots(area, regions);
    u16* keys = malloc((size_t)width * height * sizeof(u16));
    u16* variant = malloc((size_t)width * height * sizeof(u16));
    u8* vram = malloc(HW_VRAM_SIZE);
    u16 pal[512];
    bool done[7 * 7 * 7] = { false };
    Layers l = *base;

    // A pixel's key: its groups, 0xff as 6, in base 7.
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int key = 0;
            for (int slot = 0; slot < slots; slot++) {
                u32 g = RegionGroup(regions[slot], x, y);
                key = key * 7 + (g < 6 ? g : 6);
            }
            keys[y * width + x] = key;
        }
    }
    l.vram = vram;
    l.pal = pal;
    for (int i = 0; i < width * height; i++) {
        int key = keys[i];
        u8 groups[3];
        if (done[key]) {
            continue;
        }
        done[key] = true;
        for (int slot = slots - 1, k = key; slot >= 0; slot--, k /= 7) {
            groups[slot] = k % 7 == 6 ? 0xff : k % 7;
        }
        memcpy(vram, s->vram, HW_VRAM_SIZE);
        memcpy(pal, s->pal, sizeof(pal));
        LoadRegionGroups(area, groups, vram, pal);
        DrawRoom(&l, variant, width, height);
        for (int j = i; j < width * height; j++) {
            if (keys[j] == key) {
                fb[j] = variant[j];
            }
        }
    }
    free(keys);
    free(variant);
    free(vram);
}

static void Store(AreaMap* map, int x0, int y0, const u16* fb, int width, int height) {
    for (int y = 0; y < height; y++) {
        u8* p = &map->rgba[((size_t)(y0 + y) * map->width + x0) * 4];
        for (int x = 0; x < width; x++, p += 4) {
            u16 c = fb[y * width + x];
            for (int ch = 0; ch < 3; ch++) {
                u8 v = (c >> (5 * ch)) & 0x1F;
                p[ch] = (v << 3) | (v >> 2); // as PpuWritePngSize
            }
            p[3] = 0xFF;
        }
    }
}

// Lays out map->rooms (x, y from the map positions), allocates and draws.
static bool Render(AreaMap* map) {
    int minX = 1 << 30, minY = 1 << 30, maxX = 0, maxY = 0;
    Scratch* s;
    u16* fb;
    Layers l;

    if (map->roomCount == 0) {
        return false;
    }
    // Rooms at the same place (Cloud Tops' three levels, Royal Crypt rooms left at 0,0) cannot share a picture: a room
    // overlapping an earlier one by more than half of the smaller of the two moves to rows under the others. A strip
    // of overlap (where two Fortress of Winds rooms meet) stays, the later room drawn over the earlier.
    for (int j = 0; j < map->roomCount; j++) {
        AreaMapRoom* b = &map->rooms[j];
        b->atMapPosition = true;
        for (int i = 0; i < j; i++) {
            const AreaMapRoom* a = &map->rooms[i];
            int w = AR_MIN(a->x + a->width, b->x + b->width) - AR_MAX(a->x, b->x);
            int h = AR_MIN(a->y + a->height, b->y + b->height) - AR_MAX(a->y, b->y);
            if (a->atMapPosition && w > 0 && h > 0) {
                if (w * h * 2 > AR_MIN(a->width * a->height, b->width * b->height)) {
                    b->atMapPosition = false;
                    break;
                }
                map->overlaps++;
            }
        }
        if (b->atMapPosition) {
            minX = AR_MIN(minX, b->x);
            maxX = AR_MAX(maxX, b->x + b->width);
            maxY = AR_MAX(maxY, b->y + b->height);
        } else {
            map->moved++;
        }
    }
    if (map->moved > 0) {
        // Rows in room order, 16 pixels apart, at most as wide as the placed rooms (or 1024 pixels).
        int rowWidth = AR_MAX(maxX - minX, 1024), x = minX, y = maxY + 16, rowHeight = 0;
        for (int i = 0; i < map->roomCount; i++) {
            AreaMapRoom* r = &map->rooms[i];
            if (r->atMapPosition) {
                continue;
            }
            if (x > minX && x + r->width > minX + rowWidth) {
                x = minX;
                y += rowHeight + 16;
                rowHeight = 0;
            }
            r->x = x;
            r->y = y;
            x += r->width + 16;
            rowHeight = AR_MAX(rowHeight, r->height);
        }
    }
    minX = minY = 1 << 30;
    maxX = maxY = 0;
    for (int i = 0; i < map->roomCount; i++) {
        AreaMapRoom* r = &map->rooms[i];
        minX = r->x < minX ? r->x : minX;
        minY = r->y < minY ? r->y : minY;
        maxX = r->x + r->width > maxX ? r->x + r->width : maxX;
        maxY = r->y + r->height > maxY ? r->y + r->height : maxY;
    }
    map->width = maxX - minX;
    map->height = maxY - minY;
    map->rgba = calloc((size_t)map->width * map->height, 4);
    s = malloc(sizeof(*s));
    fb = malloc(MAX_MAP_SIZE * 16 * MAX_MAP_SIZE * 16 * sizeof(u16));
    if (map->rgba == NULL || s == NULL || fb == NULL) {
        free(s);
        free(fb);
        AreaMapFree(map);
        return false;
    }
    for (int i = 0; i < map->roomCount; i++) {
        AreaMapRoom* r = &map->rooms[i];
        r->x -= minX;
        r->y -= minY;
        if (ScratchLoadRoom(s, r->area, r->room, &l)) {
            const u16* regions[3];
            if (RegionSlots(r->area, regions) > 0) {
                DrawRegions(s, &l, r->area, fb, r->width, r->height);
            } else {
                DrawRoom(&l, fb, r->width, r->height);
            }
            Store(map, r->x, r->y, fb, r->width, r->height);
        }
    }
    free(s);
    free(fb);
    map->checksum = StateHashBytes(map->rgba, (size_t)map->width * map->height * 4);
    return true;
}

// A room at x, y before Render lays out and offsets the rooms.
static void AddRoom(AreaMap* map, int area, int room, int x, int y) {
    const RoomHeader* header = &gAreaRoomHeaders[area][room];
    AreaMapRoom* r;

    if (map->roomCount >= AREA_MAP_MAX_ROOMS) {
        map->skipped++;
        return;
    }
    r = &map->rooms[map->roomCount++];
    r->area = area;
    r->room = room;
    r->worldX = header->map_x;
    r->worldY = header->map_y;
    r->x = x;
    r->y = y;
    r->width = header->pixel_width;
    r->height = header->pixel_height;
    r->name = RoomName(area, room);
}

bool AreaMapRenderRoom(int area, int room, AreaMap* out) {
    memset(out, 0, sizeof(*out));
    if (!AreaMapRoomIsDrawable(area, room)) {
        return false;
    }
    AddRoom(out, area, room, 0, 0);
    return Render(out);
}

bool AreaMapRenderArea(int area, AreaMap* out) {
    int count = AreaMapRoomCount(area);

    memset(out, 0, sizeof(*out));
    for (int room = 0; room < count; room++) {
        if (AreaMapRoomIsDrawable(area, room)) {
            AddRoom(out, area, room, gAreaRoomHeaders[area][room].map_x, gAreaRoomHeaders[area][room].map_y);
        } else {
            out->skipped++;
        }
    }
    return Render(out);
}

int AreaMapDungeonCount(void) {
    return DUNGEON_COUNT;
}

int AreaMapFloorCount(int dungeon) {
    return dungeon >= 1 && dungeon < DUNGEON_COUNT ? gDungeonFloorMetadatas[dungeon].numFloors : 0;
}

bool AreaMapRenderFloor(int dungeon, int floor, AreaMap* out) {
    memset(out, 0, sizeof(*out));
    if (floor < 0 || floor >= AreaMapFloorCount(dungeon)) {
        return false;
    }
    // As the pause menu's dungeon map places them (common.c): map positions modulo 0x800.
    for (const DungeonLayout* d = gDungeonLayouts[dungeon][floor]; d->area != 0; d++) {
        if (AreaMapRoomIsDrawable(d->area, d->room)) {
            const RoomHeader* header = &gAreaRoomHeaders[d->area][d->room];
            AddRoom(out, d->area, d->room, header->map_x % 0x800, header->map_y % 0x800);
        } else {
            out->skipped++;
        }
    }
    return Render(out);
}

bool AreaMapFloorOf(int area, int room, int* dungeon, int* floor) {
    for (int d = 1; d < DUNGEON_COUNT; d++) {
        for (int f = 0; f < AreaMapFloorCount(d); f++) {
            for (const DungeonLayout* l = gDungeonLayouts[d][f]; l->area != 0; l++) {
                if (l->area == area && l->room == room) {
                    *dungeon = d;
                    *floor = f;
                    return true;
                }
            }
        }
    }
    return false;
}

void AreaMapFree(AreaMap* map) {
    free(map->rgba);
    map->rgba = NULL;
}

int AreaMapRoomAt(const AreaMap* map, int x, int y) {
    for (int i = map->roomCount - 1; i >= 0; i--) { // the later of two overlapping rooms is drawn on top
        const AreaMapRoom* r = &map->rooms[i];
        if (x >= r->x && y >= r->y && x < r->x + r->width && y < r->y + r->height) {
            return i;
        }
    }
    return -1;
}

static int WritePng(const AreaMap* map, const char* path) {
    u16* fb = malloc((size_t)map->width * map->height * sizeof(u16));
    int result;

    for (size_t i = 0; i < (size_t)map->width * map->height; i++) {
        const u8* p = &map->rgba[i * 4];
        fb[i] = p[3] ? (p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) : 0x1084; // dark grey between rooms
    }
    result = PpuWritePngSize(fb, map->width, map->height, path);
    free(fb);
    return result;
}

static void PrintMap(FILE* out, const char* what, const AreaMap* map, const char* dir, const char* file,
                     bool listRooms) {
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    fprintf(out, "areadump %s rooms %d skipped %d size %dx%d checksum %016llx%s%s%s\n", what, map->roomCount,
            map->skipped, map->width, map->height, (unsigned long long)map->checksum, map->moved ? " moved" : "",
            map->overlaps ? " overlapping" : "",
            WritePng(map, path) == 0 ? "" : " (png failed)");
    for (int i = 0; listRooms && i < map->roomCount; i++) {
        const AreaMapRoom* r = &map->rooms[i];
        fprintf(out, "areadump   room %02x:%02x at %d,%d %dx%d map %d,%d%s %s\n", r->area, r->room, r->x, r->y,
                r->width, r->height, r->worldX, r->worldY, r->atMapPosition ? "" : " (moved)",
                r->name != NULL ? r->name : "-");
    }
}

int AreaMapDumpAll(const char* dir, bool listRooms, FILE* out) {
    int drawn = 0, rooms = 0;
    char what[64], file[64];

    for (int area = 0; area < AREA_COUNT; area++) {
        AreaMap map;
        if (!AreaMapRenderArea(area, &map)) {
            if (AreaMapRoomCount(area) > 0) {
                fprintf(out, "areadump area %02x: nothing drawable (%d room headers)\n", area, AreaMapRoomCount(area));
            }
            continue;
        }
        snprintf(what, sizeof(what), "area %02x", area);
        snprintf(file, sizeof(file), "area_%02x.png", area);
        PrintMap(out, what, &map, dir, file, listRooms);
        drawn++;
        rooms += map.roomCount;
        AreaMapFree(&map);
    }
    for (int dungeon = 1; dungeon < DUNGEON_COUNT; dungeon++) {
        for (int floor = 0; floor < AreaMapFloorCount(dungeon); floor++) {
            AreaMap map;
            if (AreaMapRenderFloor(dungeon, floor, &map)) {
                snprintf(what, sizeof(what), "dungeon %d floor %d", dungeon, floor);
                snprintf(file, sizeof(file), "dungeon_%d_floor_%d.png", dungeon, floor);
                PrintMap(out, what, &map, dir, file, listRooms);
                AreaMapFree(&map);
            }
        }
    }
    fprintf(out, "areadump: %d areas, %d rooms\n", drawn, rooms);
    return drawn;
}

// The BG VRAM bytes and palette colours an area's background animations (LoadBgAnimations) can write.
static void AnimationMask(int area, u8* vram, u8* pal) {
    const u16* list = gUnk_080B755C[area];

    for (; list != NULL && *list != 0xFFFF; list++) {
        for (const BgAnimationFrame* frame = gUnk_080B7278[*list]; frame->gfx != NULL; frame++) {
            for (const BgAnimationGfx* gfx = frame->gfx;; gfx++) {
                if (gfx->flags & BG_ANIM_PALETTE) {
                    u32 first = (gfx->vramOffset >> 5) * 16; // LoadPalettes(src, vramOffset >> 5, gfxSize)
                    for (u32 i = first; i < first + gfx->gfxSize * 16u && i < 512; i++) {
                        pal[i] = 1;
                    }
                } else {
                    for (u32 i = gfx->vramOffset; i < gfx->vramOffset + gfx->gfxSize * 32u && i < BG_VRAM_SIZE; i++) {
                        vram[i] = 1;
                    }
                }
                if (!(gfx->flags & BG_ANIM_MULTIPLE)) {
                    break;
                }
            }
        }
    }
}

static int CountDiff(const u16* a, const u16* b, int n) {
    int count = 0;

    for (int i = 0; i < n; i++) {
        count += a[i] != b[i];
    }
    return count;
}

void AreaMapCheckLive(FILE* out) {
    int area = gRoomControls.area, room = gRoomControls.room;
    int width = gRoomControls.width, height = gRoomControls.height, n = width * height;
    u16 livePal[512];
    u8* animVram;
    u8 animPal[512] = { 0 };
    u8* vram;
    Scratch* s;
    Layers side, live, mix;
    u16* fb[5];
    int swapped = 0, vramBytes = 0, animBytes = 0, palColors = 0, animColors = 0, palRows = 0;

    if (!AreaMapRoomIsDrawable(area, room) || width != gAreaRoomHeaders[area][room].pixel_width ||
        height != gAreaRoomHeaders[area][room].pixel_height || gMapBottom.bgSettings == NULL) {
        fprintf(out, "areacheck %02x:%02x: not checked (room %dx%d)\n", area, room, width, height);
        return;
    }
    s = malloc(sizeof(*s));
    animVram = calloc(BG_VRAM_SIZE, 1);
    vram = malloc(HW_VRAM_SIZE);
    for (int i = 0; i < 5; i++) {
        fb[i] = malloc(n * sizeof(u16));
    }
    ScratchLoadRoom(s, area, room, &side);
    {
        // The groups loaded now, not the per-pixel ones AreaMapRenderArea uses.
        const u16* regions[3];
        if (RegionSlots(area, regions) > 0) {
            LoadRegionGroups(area, gRoomVars.graphicsGroups, s->vram, s->pal);
        }
    }
    AnimationMask(area, animVram, animPal);
    for (int i = 0; i < 512; i++) {
        livePal[i] = gHwPltt[i * 2] | (gHwPltt[i * 2 + 1] << 8);
    }

    // 0: what TMC_MAPDUMP draws (for a bitmap room, which it cannot draw, the live 8x8 maps instead).
    live = side;
    live.mapData[0] = gMapBottom.mapData;
    live.mapData[1] = gMapTop.mapData;
    live.subTiles[0] = gMapBottom.subTiles;
    live.subTiles[1] = gMapTop.subTiles;
    live.special[0] = gMapDataBottomSpecial;
    live.special[1] = gMapDataTopSpecial;
    live.vram = gHwVram;
    live.pal = livePal;
    DrawRoom(&live, fb[0], width, height);
    // 1: the renderer's tile maps, the live graphics and palettes; 0 to 1 is tile changes.
    mix = side;
    mix.vram = gHwVram;
    mix.pal = livePal;
    DrawRoom(&mix, fb[1], width, height);
    // 2: the renderer's graphics but where animations write; 1 to 2 is other VRAM.
    memcpy(vram, s->vram, HW_VRAM_SIZE);
    for (int i = 0; i < BG_VRAM_SIZE; i++) {
        vram[i] = animVram[i] ? gHwVram[i] : vram[i];
    }
    mix.vram = vram;
    DrawRoom(&mix, fb[2], width, height);
    // 3: the renderer's graphics; 2 to 3 is animated tiles.
    mix.vram = s->vram;
    DrawRoom(&mix, fb[3], width, height);
    // 4: the renderer's picture; 3 to 4 is palettes.
    DrawRoom(&side, fb[4], width, height);

    for (int y = 0; y < height / 16; y++) {
        for (int x = 0; x < width / 16; x++) {
            for (int layer = 0; layer < 2; layer++) {
                swapped += live.mapData[layer][y * 64 + x] != side.mapData[layer][y * 64 + x];
            }
        }
    }
    for (int i = 0; i < BG_VRAM_SIZE; i++) {
        vramBytes += gHwVram[i] != s->vram[i];
        animBytes += gHwVram[i] != s->vram[i] && animVram[i];
    }
    for (int i = 0; i < 256; i++) {
        palColors += livePal[i] != s->pal[i];
        animColors += livePal[i] != s->pal[i] && animPal[i];
        palRows |= (livePal[i] != s->pal[i] && !animPal[i]) << (i / 16);
    }
    {
        int total = CountDiff(fb[0], fb[4], n);
        fprintf(out,
                "areacheck %02x:%02x %dx%d%s: %s, %d of %d pixels differ: %d by tile changes (%d metatiles), "
                "%d by animated tiles, %d by other VRAM (BG VRAM bytes differ: %d, %d of them animated), "
                "%d by palettes (BG colours differ: %d, %d of them animated; other BG palettes: %04x)%s%s\n",
                area, room, width, height, side.bitmap ? " bitmap" : "", total == 0 ? "match" : "differ", total, n,
                CountDiff(fb[0], fb[1], n), side.bitmap ? 0 : swapped, CountDiff(fb[2], fb[3], n),
                CountDiff(fb[1], fb[2], n), vramBytes, animBytes, CountDiff(fb[3], fb[4], n), palColors, animColors, palRows,
                gMapBottom.bgSettings->control != side.control[0] ? " (bottom BG control differs)" : "",
                s->unknownDest || s->overflow ? " (skipped data definitions)" : "");
    }
    free(s);
    free(animVram);
    free(vram);
    for (int i = 0; i < 5; i++) {
        free(fb[i]);
    }
}
