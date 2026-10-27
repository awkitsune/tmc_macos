/*
 * The Map debug window's view of the current room (port/mapview.h), taken on
 * the game thread: the room's picture from the metatile map and the tile
 * graphics in VRAM (mapdump.c), the entities, the camera, and the requests the
 * window makes (move the player, warp; warp.c does the warping).
 */

#include <stdio.h>

#include "global.h"
#include "asm.h"
#include "entity.h"
#include "fade.h"
#include "game.h"
#include "main.h"
#include "map.h"
#include "player.h"
#include "room.h"

#include "port/headless_state.h"
#include "port/hw.h"
#include "port/mapview.h"

// warp.c
enum { WARP_OFF, WARP_WAITING, WARP_GOING, WARP_ARRIVED };
int WarpRequest(int area, int room, int x, int y);
int WarpUpdate(unsigned long long frame);

// The room picture's inputs; when they change, the picture is drawn again once they have stayed
// the same for SETTLE_FRAMES in plain gameplay (no fade, no room change under way: VRAM and the
// palette are the room's then), or for SETTLE_FALLBACK frames whatever happens (a cutscene).
#define SETTLE_FRAMES 8
#define SETTLE_FALLBACK 120
static uint64_t sKey, sDrawnKey;
static int sKeyFrames;
static uint64_t sImageSequence;
// The warp the window asked for, and how far along it was at the last update (warp.c's states).
static int sWarpArea, sWarpRoom, sWarpState;

static uint64_t Fnv(uint64_t h, const void* data, size_t size) {
    const uint8_t* p = data;
    for (size_t i = 0; i < size; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

static uint64_t RoomKey(int tilesWide, int tilesHigh) {
    MapLayer* layers[2] = { &gMapBottom, &gMapTop };
    int head[4] = { gRoomControls.area, gRoomControls.room, gRoomControls.width, gRoomControls.height };
    uint64_t h = Fnv(0xcbf29ce484222325ull, head, sizeof(head));

    for (int l = 0; l < 2; l++) {
        u16 control = layers[l]->bgSettings != NULL ? layers[l]->bgSettings->control : 0xFFFF;
        h = Fnv(h, &control, sizeof(control));
        for (int ty = 0; ty < tilesHigh; ty++) {
            h = Fnv(h, &layers[l]->mapData[ty * MAX_MAP_SIZE], tilesWide * sizeof(u16));
            h = Fnv(h, &layers[l]->collisionData[ty * MAX_MAP_SIZE], tilesWide);
        }
    }
    return h != 0 ? h : 1;
}

static void DrawImage(MapViewImage* image, uint64_t frame, int width, int height, int tilesWide, int tilesHigh) {
    MapLayer* layers[2] = { &gMapBottom, &gMapTop };

    image->frame = frame;
    image->area = gRoomControls.area;
    image->room = gRoomControls.room;
    image->width = width;
    image->height = height;
    image->tilesWide = tilesWide;
    image->tilesHigh = tilesHigh;
    image->backdrop = (gHwPltt[0] | (gHwPltt[1] << 8)) & 0x7FFF;
    for (int l = 0; l < 2; l++) {
        uint16_t* fb = image->layer[l];
        for (int i = 0; i < width * height; i++) {
            fb[i] = MAPVIEW_TRANSPARENT;
        }
        MapRenderLayer(l, fb, width, height);
        for (int i = 0; i < MAPVIEW_TILES * MAPVIEW_TILES; i++) {
            image->metatile[l][i] = layers[l]->mapData[i];
            image->collision[l][i] = layers[l]->collisionData[i];
        }
    }
    image->sequence = ++sImageSequence;
}

uint64_t MapViewImageHash(const MapViewImage* image) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int l = 0; l < 2; l++) {
        h = Fnv(h, image->layer[l], (size_t)image->width * image->height * sizeof(uint16_t));
    }
    return h;
}

void MapViewSummary(const MapViewState* state, const MapViewImage* image, char* out, int size) {
    if (!state->inGame) {
        snprintf(out, size, "map: frame %llu not in gameplay", (unsigned long long)state->frame);
        return;
    }
    snprintf(out, size,
             "map: frame %llu area %02x room %02x image %dx%d of %02x:%02x seq %llu hash %016llx entities %d "
             "camera %d,%d player %d,%d layer %d control %d",
             (unsigned long long)state->frame, state->area, state->room, image->width, image->height, image->area,
             image->room, (unsigned long long)image->sequence,
             (unsigned long long)(image->sequence != 0 ? MapViewImageHash(image) : 0), state->entityCount,
             state->cameraX, state->cameraY, state->playerX, state->playerY, state->playerLayer, state->control);
}

static void CaptureEntities(MapViewState* state) {
    state->entityCount = 0;
    for (int list = 0; list < 9; list++) {
        // A bound on the walk, should a list ever be broken.
        int steps = 0;
        for (Entity* e = gEntityLists[list].first; e && e != (Entity*)&gEntityLists[list] && steps < 1024;
             e = e->next, steps++) {
            MapViewEntity* out;
            if (e->kind == MANAGER || state->entityCount >= MAPVIEW_MAX_ENTITIES) {
                continue;
            }
            out = &state->entities[state->entityCount++];
            out->list = list;
            out->kind = e->kind;
            out->id = e->id;
            out->type = e->type;
            out->action = e->action;
            out->subAction = e->subAction;
            out->layer = e->collisionLayer;
            out->x = e->x.HALF.HI - gRoomControls.origin_x;
            out->y = e->y.HALF.HI - gRoomControls.origin_y;
            out->hasHitbox = e->hitbox != NULL;
            if (e->hitbox != NULL) {
                out->hitboxX = e->hitbox->offset_x;
                out->hitboxY = e->hitbox->offset_y;
                out->hitboxWidth = e->hitbox->width;
                out->hitboxHeight = e->hitbox->height;
            } else {
                out->hitboxX = out->hitboxY = out->hitboxWidth = out->hitboxHeight = 0;
            }
        }
    }
}

int MapViewCapture(MapViewState* state, MapViewImage* image, uint64_t frame, int settled, int redraw) {
    int width = gRoomControls.width, height = gRoomControls.height;
    int tilesWide = (width + 15) / 16, tilesHigh = (height + 15) / 16;
    Entity* p = &gPlayerEntity.base;
    HeadlessState s;
    uint64_t key;

    state->frame = frame;
    state->inGame = gMain.task == TASK_GAME && width > 0 && height > 0 && tilesWide <= MAX_MAP_SIZE &&
                    tilesHigh <= MAX_MAP_SIZE;
    state->imageSequence = image->sequence;
    if (!state->inGame) {
        state->entityCount = 0;
        sKeyFrames = 0;
        return 0;
    }
    HeadlessGetState(&s);
    state->area = gRoomControls.area;
    state->room = gRoomControls.room;
    state->originX = gRoomControls.origin_x;
    state->originY = gRoomControls.origin_y;
    state->width = width;
    state->height = height;
    state->cameraX = gRoomControls.scroll_x - gRoomControls.origin_x;
    state->cameraY = gRoomControls.scroll_y - gRoomControls.origin_y;
    state->playerX = p->x.HALF.HI - gRoomControls.origin_x;
    state->playerY = p->y.HALF.HI - gRoomControls.origin_y;
    state->playerLayer = p->collisionLayer;
    state->control = s.control;
    CaptureEntities(state);

    key = RoomKey(tilesWide, tilesHigh);
    if (key != sKey) {
        sKey = key;
        sKeyFrames = 0;
    } else if (sKeyFrames < SETTLE_FALLBACK) {
        sKeyFrames++;
    }
    if (!redraw && key == sDrawnKey && image->sequence != 0) {
        return 0;
    }
    if (!redraw && !settled) {
        int steady = gMain.state == GAMETASK_MAIN && gMain.substate == GAMEMAIN_UPDATE && !gFadeControl.active;
        if (gMain.substate == GAMEMAIN_SUBTASK || // a menu has the screen (and VRAM)
            !(sKeyFrames >= SETTLE_FALLBACK || (steady && sKeyFrames >= SETTLE_FRAMES))) {
            return 0;
        }
    }
    DrawImage(image, frame, width, height, tilesWide, tilesHigh);
    sDrawnKey = key;
    state->imageSequence = image->sequence;
    return 1;
}

int MapViewMovePlayer(int x, int y, char* message, int size) {
    Entity* p = &gPlayerEntity.base;
    int layer = p->collisionLayer != 0 ? p->collisionLayer : 1;
    int worldX = gRoomControls.origin_x + (x & ~15) + 8, worldY = gRoomControls.origin_y + (y & ~15) + 8;
    HeadlessState s;

    HeadlessGetState(&s);
    if (!s.control) {
        snprintf(message, size, "move: Link does not have control now");
        return 0;
    }
    if (x < 0 || y < 0 || x >= gRoomControls.width || y >= gRoomControls.height) {
        snprintf(message, size, "move: %d,%d is outside the room", x, y);
        return 0;
    }
    if (GetCollisionDataAtWorldCoords(worldX, worldY, layer) != 0) {
        snprintf(message, size, "move: tile %d,%d is not walkable on layer %d (collision %02x)", x / 16, y / 16,
                 layer, (unsigned)GetCollisionDataAtWorldCoords(worldX, worldY, layer));
        return 0;
    }
    p->x.HALF.HI = worldX;
    p->y.HALF.HI = worldY;
    snprintf(message, size, "moved Link to %d,%d (tile %d,%d)", worldX - gRoomControls.origin_x,
             worldY - gRoomControls.origin_y, x / 16, y / 16);
    return 1;
}

int MapViewWarp(int area, int room, char* message, int size) {
    if (!WarpRequest(area, room, -1, -1)) {
        snprintf(message, size, "warp: %02x:%02x is not a room the game can load", area, room);
        return 0;
    }
    sWarpArea = area;
    sWarpRoom = room;
    sWarpState = WARP_WAITING;
    snprintf(message, size, "warp to %02x:%02x: waiting for Link to have control", area, room);
    return 1;
}

int MapViewUpdate(uint64_t frame, char* message, int size) {
    int state = WarpUpdate(frame);

    if (state != sWarpState && state == WARP_GOING) {
        snprintf(message, size, "warp to %02x:%02x under way (frame %llu)", sWarpArea, sWarpRoom,
                 (unsigned long long)frame);
    } else if (state != sWarpState && state == WARP_ARRIVED) {
        snprintf(message, size, "warp arrived in %02x:%02x (frame %llu)", sWarpArea, sWarpRoom,
                 (unsigned long long)frame);
    }
    sWarpState = state;
    return state == WARP_WAITING || state == WARP_GOING;
}
