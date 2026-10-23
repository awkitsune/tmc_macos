/*
 * TMC_CHECK_ENTITY_LISTS=path: walk every room's entity lists the way the
 * game's loader sees them natively and write one line per list:
 *
 *   list AREA ROOM PROPERTY ENTRIES        the 0xFF terminator after ENTRIES entries
 *   bad AREA ROOM PROPERTY INDEX KIND      an entry whose kind is not 1-9
 *   unterminated AREA ROOM PROPERTY        no terminator within MAX_ENTRIES entries
 *
 * The lists are properties 0, 1 and 2 of each room (LoadRoom) and the
 * properties that managers and objects in them load as entity lists (the same
 * consumers as port/tools/datagen/entitylists.py). port/scripts/
 * entity_list_check.py compares the counts with the GBA lists in tmc.gba.
 */

#include <stdio.h>

#include "global.h"
#include "area.h"
#include "room.h"

#define AREA_TABLE_SIZE 0x90
#define PROPERTY_SLOTS 0x80 // GetCurrentRoomProperty indices below the rail range
#define MAX_ENTRIES 256

extern void*** gAreaTable[];

// Room property indices that this entity loads as entity lists; returns how many.
static int ConsumedProperties(const EntityData* e, u32 out[2]) {
    u32 kind = e->kind & 0xF;
    u32 type2 = (e->flags & 0xF0) == 0x10 ? 0 : *(const u8*)&e->type2;

    if (kind == 9) {
        switch (e->id) {
            case 0x0B: // FightManager
            case 0x28: // EnemyInteractionManager
                out[0] = type2;
                return 1;
            case 0x0D: // TileChangeObserveManager, 0: none
                out[0] = e->type;
                return e->type != 0;
            case 0x0E: // EntitySpawnManager
            case 0x38: // FloatingPlatformManager
                out[0] = e->type;
                return 1;
        }
    } else if (kind == 6 && e->id == 0x89) { // MazaalBossObject
        out[0] = e->type;
        out[1] = type2;
        return 2;
    }
    return 0;
}

static void CheckRoom(FILE* f, u32 area, u32 room, void** props) {
    u8 queued[PROPERTY_SLOTS] = { 0 };
    u32 work[PROPERTY_SLOTS];
    int n = 0;
    u32 p;

    for (p = 0; p < 3; p++) {
        work[n++] = p;
        queued[p] = 1;
    }
    while (n > 0) {
        const EntityData* list;
        int i;

        p = work[--n];
        list = props[p];
        if (list == NULL) {
            continue;
        }
        for (i = 0; i < MAX_ENTRIES && list[i].kind != 0xFF; i++) {
            u32 kind = list[i].kind & 0xF;
            u32 next[2];
            int k, count;

            if (kind < 1 || kind > 9) {
                fprintf(f, "bad %u %u %u %d %u\n", area, room, p, i, list[i].kind);
            }
            count = ConsumedProperties(&list[i], next);
            for (k = 0; k < count; k++) {
                if (next[k] < PROPERTY_SLOTS && !queued[next[k]]) {
                    queued[next[k]] = 1;
                    work[n++] = next[k];
                }
            }
        }
        if (i == MAX_ENTRIES) {
            fprintf(f, "unterminated %u %u %u\n", area, room, p);
        } else {
            fprintf(f, "list %u %u %u %d\n", area, room, p, i);
        }
    }
}

void CheckEntityLists(const char* path) {
    FILE* f = fopen(path, "w");
    u32 area, room;

    if (f == NULL) {
        fprintf(stderr, "TMC_CHECK_ENTITY_LISTS: cannot write %s\n", path);
        return;
    }
    for (area = 0; area < AREA_TABLE_SIZE; area++) {
        const RoomHeader* hdr = gAreaRoomHeaders[area];

        if (gAreaTable[area] == NULL || hdr == NULL) {
            continue;
        }
        for (room = 0; room < MAX_ROOMS && *(const u16*)&hdr[room] != 0xFFFF; room++) {
            if (gAreaTable[area][room] != NULL) {
                CheckRoom(f, area, room, gAreaTable[area][room]);
            }
        }
    }
    fclose(f);
}
