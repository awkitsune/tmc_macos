/*
 * C versions of asm/src/veneer.s: tile writes and the BG update dispatcher. The
 * long-branch veneers into IWRAM there need no counterpart; the routines they
 * reach are in intr.c under their own names.
 */

#include "global.h"
#include "map.h"
#include "tileMap.h"
#include "vram.h"

extern u8 gUpdateVisibleTiles;
extern const u8 gMapTileTypeToActTile[];
extern const u8 gMapSpecialTileToActTile[];
extern const u8 gMapTileTypeToCollisionData[];
extern const u8 gMapSpecialTileToCollisionData[];

void ram_sub_080B197C(void* src, void* dest);
void sub_0807D280(u16* mapSpecial, u16* bgBuffer);
void sub_0807D46C(u16* mapSpecial, u16* bgBuffer);
void sub_0807D6D8(u16* mapSpecial, u16* bgBuffer);
void RegisterInteractTile(u32 tileIndex, u32 tilePos, u32 layer);
void UnregisterInteractTile(u32 tilePos, s32 layer);

// Layers 0, 1 and 3 all use the bottom map.
static MapLayer* LayerMap(u32 layer) {
    return layer == 2 ? &gMapTop : &gMapBottom;
}

// Copies from the special tile maps into the BG buffers, by gUpdateVisibleTiles.
void UpdateScrollVram(void) {
    static void (*const sCopy[])(void*, void*) = {
        NULL,
        (void (*)(void*, void*))ram_sub_080B197C,
        (void (*)(void*, void*))sub_0807D280,
        (void (*)(void*, void*))sub_0807D46C,
        (void (*)(void*, void*))sub_0807D6D8,
    };
    void (*copy)(void*, void*);

    if (gUpdateVisibleTiles == 0) {
        return;
    }
    copy = sCopy[gUpdateVisibleTiles];
    if (gMapBottom.bgSettings != NULL) {
        copy(gMapDataBottomSpecial, gBG1Buffer + 0x20);
    }
    if (gMapTop.bgSettings != NULL) {
        copy(gMapDataTopSpecial, gBG2Buffer + 0x20);
    }
}

// Tile positions index signed, as the asm's 32-bit address adds wrap: a caller's
// tilePos - 0x40 on the room's top row lands just before the array, not 4 GB on.
void SetCollisionData(u32 collisionData, u32 tilePos, u32 layer) {
    LayerMap(layer)->collisionData[(s32)tilePos] = collisionData;
}

void SetActTileAtTilePos(u32 actTile, u32 tilePos, u32 layer) {
    LayerMap(layer)->actTiles[(s32)tilePos] = actTile;
}

u32 GetTileIndex(u32 tilePos, u32 layer) {
    return LayerMap(layer)->mapData[(s32)tilePos];
}

void SetTile(u32 tileIndex, u32 tilePos, u32 layer) {
    MapLayer* map = LayerMap(layer);
    u32 oldTile = map->mapData[(s32)tilePos];

    map->mapData[(s32)tilePos] = tileIndex;
    if (tileIndex >= 0x4000) {
        SetActTileAtTilePos(gMapSpecialTileToActTile[tileIndex - 0x4000], tilePos, layer);
        SetCollisionData(gMapSpecialTileToCollisionData[tileIndex - 0x4000], tilePos, layer);
        UnregisterInteractTile(tilePos, layer);
        RegisterInteractTile(oldTile, tilePos, layer);
    } else {
        u32 tileType = map->tileTypes[tileIndex];
        SetActTileAtTilePos(gMapTileTypeToActTile[tileType], tilePos, layer);
        SetCollisionData(gMapTileTypeToCollisionData[tileType], tilePos, layer);
        UnregisterInteractTile(tilePos, layer);
    }
}

// Places the tile that the layer's tileIndices table maps this tile type to.
void CloneTile(u32 tileType, u32 tilePos, u32 layer) {
    SetTile(LayerMap(layer)->tileIndices[tileType], tilePos, layer);
}
