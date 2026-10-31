/*
 * C versions of asm/src/veneer.s: tile writes and the BG update dispatcher. The
 * long-branch veneers into IWRAM there need no counterpart; the routines they
 * reach are in intr.c under their own names.
 */

#include "global.h"
#include "game.h"
#include "main.h"
#include "map.h"
#include "room.h"
#include "screen.h"
#include "tileMap.h"
#include "vram.h"

#include "port/state.h"
#include "port/view.h"

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

    // The extended view draws the room layers from the wide maps (PortViewUpdate).
    // TODO: the scroll transitions' incremental copies (2-4) in wide maps.
    if (gUpdateVisibleTiles == 0 || PortViewExtended()) {
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

// ---- extended view (port/notes/widescreen.md) ----------------------------------

int gPortHudX;

static int BgIndex(const void* bg) {
    if (bg == &gScreen.bg0) {
        return 0;
    }
    if (bg == &gScreen.bg1) {
        return 1;
    }
    return bg == &gScreen.bg2 ? 2 : 3;
}

// What each room layer's wide map showed last frame, and the world position of its
// first entry. During transitions the GBA keeps its BG buffers and writes only the
// new room's tiles that the transition has reached (sub_0807D280, sub_0807D46C,
// sub_0807D6D8); the rest is what was on screen. Part of save states, like the
// GBA's buffers.
static u16 sShown[2][HW_WIDE_MAP_COLS * HW_WIDE_MAP_ROWS];
static s32 sShownX, sShownY;
static u16 sNext[2][HW_WIDE_MAP_COLS * HW_WIDE_MAP_ROWS] STATE_EXCLUDE;

// Whether the transition in progress has put the new room's tile at room
// position (x, y) on screen (the tile's centre), as the GBA's copies decide it.
static bool32 TransitionReached(s32 x, s32 y) {
    RoomControls* rc = &gRoomControls;
    Entity* target = rc->camera_target;
    s32 dx, dy, rx, ry;

    switch (rc->scrollAction) {
        case 2: // slide: all of the new room; the old one stays outside it
            return x >= 0 && x < rc->width && y >= 0 && y < rc->height;
        case 4: // wipe outward from the camera target, scaled to the view
            if (target == NULL) {
                return TRUE;
            }
            if ((rc->scroll_direction & 1) == 0) {
                dx = x - (target->x.HALF.HI - rc->origin_x);
                return dx * DISPLAY_WIDTH <= (rc->unk_18 + 1) * 8 * PORT_SCREEN_W &&
                       -dx * DISPLAY_WIDTH <= (rc->unk_18 + 1) * 8 * PORT_SCREEN_W;
            }
            dy = y - (target->y.HALF.HI - rc->origin_y);
            return dy * DISPLAY_HEIGHT <= (rc->unk_18 + 1) * 8 * PORT_SCREEN_H &&
                   -dy * DISPLAY_HEIGHT <= (rc->unk_18 + 1) * 8 * PORT_SCREEN_H;
        case 5: // digging cave: an ellipse around the target, closing (2) then opening (5)
            if (target == NULL || (rc->scrollSubAction != 2 && rc->scrollSubAction != 5)) {
                return FALSE;
            }
            rx = rc->unk_18;
            ry = rc->unk_1a;
            dx = x - (target->x.HALF.HI - rc->origin_x);
            dy = y - (target->y.HALF.HI - rc->origin_y);
            if (rx == 0 || ry == 0) {
                return rc->scrollSubAction == 2;
            }
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry) {
                return rc->scrollSubAction == 5;
            }
            return rc->scrollSubAction == 2;
        default:
            return TRUE;
    }
}

// The window of the room's 8x8 tile maps (128 entries a row) that the view shows,
// into sShown for the layers in the mask, laid out as ram_sub_080B197C lays out the 32x32
// buffers: from the metatile-aligned scroll, one row above it. Outside the room (a
// room smaller than the view) the entries are 0.
static void CopyRoomWindow(int layers) {
    static u16* const sMaps[2] = { gMapDataBottomSpecial, gMapDataTopSpecial };
    s32 col0 = ((gRoomControls.scroll_x - gRoomControls.origin_x) >> 4) * 2;
    s32 row0 = ((gRoomControls.scroll_y - gRoomControls.origin_y) >> 4) * 2 - 1;
    s32 cols = gRoomControls.width >> 3, rows = gRoomControls.height >> 3;
    s32 shownX = gRoomControls.origin_x + col0 * 8, shownY = gRoomControls.origin_y + row0 * 8;
    bool32 all = gRoomControls.scrollAction < 2 || gRoomControls.scrollAction > 5;

    for (s32 r = 0; r < HW_WIDE_MAP_ROWS; r++) {
        s32 row = row0 + r;
        s32 oldRow = (shownY + r * 8 - sShownY) >> 3;
        for (s32 c = 0; c < HW_WIDE_MAP_COLS; c++) {
            s32 col = col0 + c;
            s32 oldCol = (shownX + c * 8 - sShownX) >> 3;
            u32 inside = row >= 0 && row < rows && row < 0x80 && col >= 0 && col < cols && col < 0x80;
            u32 old = oldRow >= 0 && oldRow < HW_WIDE_MAP_ROWS && oldCol >= 0 && oldCol < HW_WIDE_MAP_COLS;
            u32 fresh = all || TransitionReached(col * 8 + 4, row * 8 + 4);

            for (int layer = 0; layer < 2; layer++) {
                u16* out = &sNext[layer][r * HW_WIDE_MAP_COLS + c];
                if (!(layers & (1 << layer))) {
                    continue;
                }
                if (fresh) {
                    *out = inside ? sMaps[layer][row * 0x80 + col] : 0;
                } else {
                    *out = old ? sShown[layer][oldRow * HW_WIDE_MAP_COLS + oldCol] : 0;
                }
            }
        }
    }
    for (int layer = 0; layer < 2; layer++) {
        if (layers & (1 << layer)) {
            memcpy(sShown[layer], sNext[layer], sizeof(sShown[layer]));
        }
    }
    sShownX = shownX;
    sShownY = shownY;
}

// Each frame where the GBA copies its BG buffers (WaitForNextFrame): in the world
// view the room layers come from the wide maps and the HUD keeps the GBA layout,
// centred; elsewhere (menus, title, file select) the whole picture is a centred
// GBA screen.
void PortViewUpdate(void) {
    // Subtasks are the menus (pause, maps, kinstones, figurines) and a few
    // cutscenes, all laid out for 240x160; the world is on screen while the first
    // one fades in (Subtask_FadeIn, 0) and after the last (Subtask_Die, 4).
    u32 world = gMain.task == TASK_GAME &&
                (gMain.substate != GAMEMAIN_SUBTASK || gUI.nextToLoad == 0 || gUI.nextToLoad == 4) &&
                (gMapBottom.bgSettings != NULL || gMapTop.bgSettings != NULL);

    gHwWideMapMask = 0;
    gHwGbaLayerMask = 0;
    gHwGbaScreen = FALSE;
    gPortHudX = 0;
    if (!PortViewExtended()) {
        return;
    }
    if (!world) {
        gHwGbaScreen = TRUE;
        return;
    }
    CopyRoomWindow((gMapBottom.bgSettings != NULL) | ((gMapTop.bgSettings != NULL) << 1));
    if (gMapBottom.bgSettings != NULL) {
        int bg = BgIndex(gMapBottom.bgSettings);
        memcpy(gHwWideMap[bg], sShown[0], sizeof(gHwWideMap[bg]));
        gHwWideMapMask |= 1 << bg;
    }
    if (gMapTop.bgSettings != NULL) {
        int bg = BgIndex(gMapTop.bgSettings);
        memcpy(gHwWideMap[bg], sShown[1], sizeof(gHwWideMap[bg]));
        gHwWideMapMask |= 1 << bg;
    }
    // BG0 holds the HUD and text boxes. BG3, when on, is a background or overlay
    // that scrolls with the camera: drawn across the view, wrapping at 256.
    gHwGbaLayerMask = ~gHwWideMapMask & 1;
    gPortHudX = PortViewGbaLeft();
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
