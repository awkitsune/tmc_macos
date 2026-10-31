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
#include "subtask.h"
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

int gPortHudX, gPortHudY;

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
// Two sets, shown and next, so that a frame reads the last one's while it writes.
static u16 sMaps[2][2][HW_WIDE_MAP_SIZE];
static s32 sShownSet;
static s32 sShownX, sShownY;

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

// The wide map cells the view can show: the window starts up to 15 px left of
// the view and 23 px above it (ram_sub_080B197C's layout), and a screen shake
// moves it a few pixels more.
static s32 ViewCols(void) {
    return min((PORT_SCREEN_W + 14) / 8 + 2, HW_WIDE_MAP_COLS);
}

static s32 ViewRows(void) {
    return min((PORT_SCREEN_H + 22) / 8 + 2, HW_WIDE_MAP_ROWS);
}

// The window of the room's 8x8 tile maps (128 entries a row) that the view shows,
// into the next set for the layers in the mask, which becomes the shown one, laid out as ram_sub_080B197C lays out the
// 32x32 buffers: from the metatile-aligned scroll, one row above it. Outside the
// room (a room smaller than the view) the entries are 0. Only the cells the view
// shows are written.
static void CopyRoomWindow(int layers) {
    static u16* const sRoomMaps[2] = { gMapDataBottomSpecial, gMapDataTopSpecial };
    s32 col0 = ((gRoomControls.scroll_x - gRoomControls.origin_x) >> 4) * 2;
    s32 row0 = ((gRoomControls.scroll_y - gRoomControls.origin_y) >> 4) * 2 - 1;
    s32 cols = min(gRoomControls.width >> 3, 0x80), rows = min(gRoomControls.height >> 3, 0x80);
    s32 shownX = gRoomControls.origin_x + col0 * 8, shownY = gRoomControls.origin_y + row0 * 8;
    s32 viewCols = ViewCols(), viewRows = ViewRows();
    bool32 all = gRoomControls.scrollAction < 2 || gRoomControls.scrollAction > 5;

    for (int layer = 0; layer < 2; layer++) {
        const u16* shown = sMaps[sShownSet][layer];
        u16* next = sMaps[sShownSet ^ 1][layer];

        if (!(layers & (1 << layer))) {
            continue;
        }
        for (s32 r = 0; r < viewRows; r++) {
            s32 row = row0 + r;
            s32 oldRow = (shownY + r * 8 - sShownY) >> 3;
            const u16* src = &sRoomMaps[layer][row * 0x80];

            for (s32 c = 0; c < viewCols; c++) {
                s32 col = col0 + c;
                u32 inside = row >= 0 && row < rows && col >= 0 && col < cols;

                if (all || TransitionReached(col * 8 + 4, row * 8 + 4)) {
                    next[HwWideMapAt(c, r)] = inside ? src[col] : 0;
                } else {
                    s32 oldCol = (shownX + c * 8 - sShownX) >> 3;
                    u32 old = oldRow >= 0 && oldRow < viewRows && oldCol >= 0 && oldCol < viewCols;
                    next[HwWideMapAt(c, r)] = old ? shown[HwWideMapAt(oldCol, oldRow)] : 0;
                }
            }
        }
    }
    sShownSet ^= 1;
    sShownX = shownX;
    sShownY = shownY;
}

// Layers that managers stream from their own maps (Minish paths and rafters):
// the GBA copies a 32-column window of a wider source into a BG buffer; in the
// extended view the BG reads a 64-column window instead. Kept until the room
// changes.
static struct {
    const u16* src;
    s32 stride, col0, cols, rows;
    u8 area, room;
    u16 map[HW_WIDE_MAP_SIZE];
} sStreams[4] STATE_EXCLUDE;

void PortViewStreamBg(const void* bg, const u16* src, s32 stride, s32 col0, s32 cols, s32 rows) {
    int i = BgIndex(bg);

    sStreams[i].src = src;
    sStreams[i].stride = stride;
    sStreams[i].col0 = col0;
    sStreams[i].cols = cols;
    sStreams[i].rows = rows;
    sStreams[i].area = gRoomControls.area;
    sStreams[i].room = gRoomControls.room;
}

static void CopyStreams(void) {
    for (int bg = 0; bg < 4; bg++) {
        if (sStreams[bg].src == NULL) {
            continue;
        }
        if (sStreams[bg].area != gRoomControls.area || sStreams[bg].room != gRoomControls.room) {
            sStreams[bg].src = NULL;
            continue;
        }
        for (s32 r = 0; r < HW_WIDE_MAP_ROWS; r++) { // their own vertical scroll
            for (s32 c = 0; c < ViewCols(); c++) {
                s32 col = sStreams[bg].col0 + c;
                sStreams[bg].map[HwWideMapAt(c, r)] =
                    r < sStreams[bg].rows && col < sStreams[bg].cols ? sStreams[bg].src[r * sStreams[bg].stride + col] : 0;
            }
        }
        gHwWideMap[bg] = sStreams[bg].map;
        gHwWideMapMask |= 1 << bg;
    }
}

// The room's place in the view, from origin to origin + size; during transitions
// together with the room before (sClip, world coordinates, from the last frame
// outside one), so both show while the camera moves between them.
static s32 sClip[4];

static void SetViewClip(void) {
    s32 room[4] = { gRoomControls.origin_x, gRoomControls.origin_y, gRoomControls.origin_x + gRoomControls.width,
                    gRoomControls.origin_y + gRoomControls.height };
    s32 view[2] = { gRoomControls.scroll_x, gRoomControls.scroll_y };
    s32 size[2] = { PORT_SCREEN_W, PORT_SCREEN_H };

    if (gRoomControls.scrollAction >= 2 && gRoomControls.scrollAction <= 5) {
        room[0] = min(room[0], sClip[0]);
        room[1] = min(room[1], sClip[1]);
        room[2] = max(room[2], sClip[2]);
        room[3] = max(room[3], sClip[3]);
    } else {
        memcpy(sClip, room, sizeof(sClip));
    }
    for (int i = 0; i < 4; i++) {
        s32 v = room[i] - view[i & 1];
        gHwViewClip[i] = v < 0 ? 0 : v > size[i & 1] ? size[i & 1] : v;
    }
}

// Each frame where the GBA copies its BG buffers (WaitForNextFrame): in the world
// view the room layers come from the wide maps and the HUD keeps the GBA layout,
// centred; elsewhere (menus, title, file select) the whole picture is a centred
// GBA screen.
void PortViewUpdate(void) {
    // Subtasks are the menus (pause, maps, kinstones, figurines), laid out for
    // 240x160, and a few cutscenes in the world. The world is on screen while a
    // menu fades in (Subtask_FadeIn, 0) and after it (Subtask_Die, 4).
    u32 menu = gMain.substate == GAMEMAIN_SUBTASK && gUI.nextToLoad != 0 && gUI.nextToLoad != 4 &&
               gUI.lastState != SUBTASK_AUXCUTSCENE && gUI.lastState != SUBTASK_PORTALCUTSCENE &&
               gUI.lastState != SUBTASK_WORLDEVENT;
    u32 world = gMain.task == TASK_GAME && !menu && (gMapBottom.bgSettings != NULL || gMapTop.bgSettings != NULL);

    gHwWideMapMask = 0;
    gHwGbaLayerMask = 0;
    gHwGbaScreen = FALSE;
    gPortHudX = gPortHudY = 0;
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
        gHwWideMap[bg] = sMaps[sShownSet][0];
        gHwWideMapMask |= 1 << bg;
    }
    if (gMapTop.bgSettings != NULL) {
        int bg = BgIndex(gMapTop.bgSettings);
        gHwWideMap[bg] = sMaps[sShownSet][1];
        gHwWideMapMask |= 1 << bg;
    }
    CopyStreams();
    SetViewClip();
    // BG0 holds the HUD and text boxes. BG3, when on, is a background or overlay
    // that scrolls with the camera: drawn across the view, wrapping at 256.
    gHwGbaLayerMask = ~gHwWideMapMask & 1;
    gPortHudX = PortViewGbaLeft();
    gPortHudY = PortViewGbaTop();
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
