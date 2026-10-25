/*
 * TMC_WARP for the headless runner: move the player into any room the way the
 * game's own exits do, for the room smoke test (port/scripts/room_smoke.sh).
 * Separate from headless.c because the game headers clash with <stdlib.h>.
 *
 * Once the player has control, WarpUpdate fills a Transition and calls
 * DoExitTransition, as an exit or warp point does; the game then runs its usual
 * area/room change. The player arrives at an entrance into that room taken from
 * the game's exit lists (the first transition of any room that leads there), or
 * at the room's centre when no exit leads there. After arriving in a room
 * without an entrance, the player is moved to the walkable tile nearest the
 * centre.
 *
 * TMC_WARP_ITEMS=A,B (item ids, include/item.h) also gives the player those two items, equipped
 * on A and B, with bombs and arrows to use, and sets EZERO_1ST (Link has Ezlo, the cap), when the
 * warp starts; nothing else in the save changes. The warp itself needs no items or flags.
 */

#include <stdio.h>

#include "global.h"
#include "area.h"
#include "asm.h"
#include "entity.h"
#include "fade.h"
#include "flags.h"
#include "game.h"
#include "item.h"
#include "main.h"
#include "message.h"
#include "player.h"
#include "room.h"
#include "save.h"
#include "transitions.h"

#include "port/headless_state.h"

void DoExitTransition(const Transition* data);

// gAreaRoomHeaders (data/map/room_headers.s) has an entry for areas 0 to 0x8f only.
#define WARP_AREA_COUNT (AREA_8F + 1)

static int RoomCount(int area) {
    const RoomHeader* header = gAreaRoomHeaders[area];
    int count = 0;

    if (header == NULL) {
        return 0;
    }
    while (count < MAX_ROOMS && header[count].map_x != 0xFFFF) {
        count++;
    }
    return count;
}

extern void** gAreaTable[];

// A room the game can load: a header with a tile set, as InitAllRoomResInfo checks, and
// room properties (entity lists, callbacks; NULL for the unused rooms, whose properties the
// GBA would read from the BIOS). port/scripts/room_smoke_rooms.py also checks the room's
// map and tile set entries, which needs the tables' lengths.
static int RoomIsValid(int area, int room) {
    return area >= 0 && area < WARP_AREA_COUNT && room >= 0 && room < RoomCount(area) &&
           gAreaRoomHeaders[area][room].tileSet_id != 0xFFFF && gAreaTable[area] != NULL &&
           gAreaTable[area][room] != NULL;
}

// The first transition in the game's exit lists that leads into area:room.
static const Transition* FindEntrance(int area, int room) {
    for (int a = 0; a < WARP_AREA_COUNT; a++) {
        if (gExitLists[a] == NULL) {
            continue;
        }
        for (int r = 0; r < RoomCount(a); r++) {
            const Transition* t;
            if (!RoomIsValid(a, r) || (t = gExitLists[a][r]) == NULL) {
                continue;
            }
            for (; t->warp_type != WARP_TYPE_END_OF_LIST; t++) {
                if (t->area == area && t->room == room && t->endX <= 0x3ff && t->endY <= 0x3ff) {
                    return t;
                }
            }
        }
    }
    return NULL;
}

static enum { WARP_OFF, WARP_WAITING, WARP_GOING, WARP_ARRIVED } sWarpState;
static int sArea, sRoom, sX = -1, sY = -1, sNudge;
static int sItems[2];

void WarpSetItems(int a, int b) {
    sItems[0] = a;
    sItems[1] = b;
}

static void GiveItems(void) {
    for (int slot = 0; slot < 2; slot++) {
        if (sItems[slot] > ITEM_NONE && sItems[slot] < ITEM_BOTTLE1) {
            SetInventoryValue(sItems[slot], 1);
            ForceEquipItem(sItems[slot], slot == 0 ? EQUIP_SLOT_A : EQUIP_SLOT_B);
        }
    }
    if (sItems[0] != 0 || sItems[1] != 0) {
        // Every item comes after Ezlo in the game: without EZERO_1ST the player has no cap (PL_NO_CAP), and the
        // item animations (sub_08077D38) have no capless version, which leaves the player without a sprite.
        SetGlobalFlag(EZERO_1ST);
        gSave.stats.bombBagType = gSave.stats.bombBagType != 0 ? gSave.stats.bombBagType : 1;
        gSave.stats.bombCount = 10;
        gSave.stats.quiverType = gSave.stats.quiverType != 0 ? gSave.stats.quiverType : 1;
        gSave.stats.arrowCount = 30;
        printf("warp: items %d (A) and %d (B)\n", gSave.stats.equipped[SLOT_A], gSave.stats.equipped[SLOT_B]);
    }
}

// Returns 0 if the request is not a valid room.
int WarpRequest(int area, int room, int x, int y) {
    if (!RoomIsValid(area, room)) {
        return 0;
    }
    sArea = area;
    sRoom = room;
    sX = x;
    sY = y;
    sWarpState = WARP_WAITING;
    return 1;
}

static int HasControl(void) {
    HeadlessState s;
    HeadlessGetState(&s);
    return s.control;
}

// The walkable tile (collision 0) nearest the player, searched in growing squares.
static void MoveToWalkableTile(void) {
    Entity* p = &gPlayerEntity.base;
    int layer = p->collisionLayer != 0 ? p->collisionLayer : 1;
    int x0 = p->x.HALF.HI & ~15, y0 = p->y.HALF.HI & ~15;
    int left = gRoomControls.origin_x, top = gRoomControls.origin_y;
    int right = left + gRoomControls.width, bottom = top + gRoomControls.height;

    if (GetCollisionDataAtWorldCoords(p->x.HALF.HI, p->y.HALF.HI, layer) == 0) {
        return;
    }
    for (int d = 1; d < 64; d++) {
        for (int dy = -d; dy <= d; dy++) {
            for (int dx = -d; dx <= d; dx++) {
                int x = x0 + dx * 16 + 8, y = y0 + dy * 16 + 8;
                if ((dx != -d && dx != d && dy != -d && dy != d) || x < left || y < top || x >= right || y >= bottom) {
                    continue;
                }
                if (GetCollisionDataAtWorldCoords(x, y, layer) == 0) {
                    printf("warp: moved the player to the walkable tile at %d,%d\n", x - left, y - top);
                    p->x.HALF.HI = x;
                    p->y.HALF.HI = y;
                    return;
                }
            }
        }
    }
    printf("warp: no walkable tile near the player\n");
}

// Called once per frame by the runner, before input.
void WarpUpdate(unsigned long long frame) {
    if (sWarpState == WARP_WAITING && HasControl()) {
        Transition t = { 0 };
        const Transition* entrance = FindEntrance(sArea, sRoom);

        t.warp_type = WARP_TYPE_AREA;
        t.area = sArea;
        t.room = sRoom;
        t.layer = 1;
        t.transition_type = TRANSITION_TYPE_NORMAL;
        t.facing_direction = 4; // down
        if (sX >= 0) {
            t.endX = sX;
            t.endY = sY;
        } else if (entrance != NULL) {
            t.endX = entrance->endX;
            t.endY = entrance->endY;
            t.layer = entrance->layer;
            t.transition_type = entrance->transition_type;
            t.facing_direction = entrance->facing_direction;
        } else {
            t.endX = gAreaRoomHeaders[sArea][sRoom].pixel_width / 2;
            t.endY = gAreaRoomHeaders[sArea][sRoom].pixel_height / 2;
            sNudge = 1;
        }
        printf("frame %llu: warp to %02x:%02x at %d,%d layer %d (%s)\n", frame, sArea, sRoom, t.endX, t.endY, t.layer,
               sX >= 0 ? "given" : entrance != NULL ? "entrance" : "centre");
        GiveItems();
        DoExitTransition(&t);
        sWarpState = WARP_GOING;
    } else if (sWarpState == WARP_GOING && gRoomControls.area == sArea && gRoomControls.room == sRoom &&
               HasControl()) {
        printf("frame %llu: warp arrived in %02x:%02x\n", frame, sArea, sRoom);
        if (sNudge) {
            MoveToWalkableTile();
        }
        sWarpState = WARP_ARRIVED;
    }
}

// The line the smoke test reads at the end of a run.
void WarpReport(unsigned long long frame) {
    if (sWarpState != WARP_OFF) {
        printf("warp: end frame %llu area %02x room %02x task %d.%d.%d warp %s\n", frame, gRoomControls.area,
               gRoomControls.room, gMain.task, gMain.state, gMain.substate,
               sWarpState == WARP_ARRIVED ? "arrived" : sWarpState == WARP_GOING ? "started" : "not started");
    }
}
