/*
 * Game-state tracing for the headless runner (TMC_TRACE_TASK). Separate from
 * headless.c because the game headers clash with macros from <stdlib.h>.
 */

#include <stdio.h>

#include "global.h"
#include "main.h"
#include "fade.h"

void TraceGameTask(u64 frame) {
    static int sLast = -1;
    int now = (gMain.task << 16) | (gMain.state << 8) | gMain.substate;

    static int sLastFade = -1;
    int fade = (gFadeControl.active << 16) | (u16)gFadeControl.progress;

    if (now != sLast) {
        printf("frame %llu: task %d state %d substate %d\n", (unsigned long long)frame, gMain.task, gMain.state,
               gMain.substate);
        sLast = now;
    }
    if (fade != sLastFade && (frame % 8 == 0 || !gFadeControl.active)) {
        printf("frame %llu: fade active %d type %x progress %d sustain %d speed %d\n", (unsigned long long)frame,
               gFadeControl.active, gFadeControl.type, (s16)gFadeControl.progress, (s16)gFadeControl.sustain,
               gFadeControl.speed);
        sLastFade = fade;
    }
}

#include "entity.h"
#include "vram.h"

typedef struct {
    u8 count;
    Entity* entities[80];
} DrawBucket;
extern DrawBucket gDrawBuckets[4];

// TMC_TRACE_ENTITIES: live entities per list and what was queued for drawing.
void TraceEntities(u64 frame) {
    printf("frame %llu entities:", (unsigned long long)frame);
    for (int i = 0; i < 9; i++) {
        int n = 0;
        for (Entity* e = gEntityLists[i].first; e && e != (Entity*)&gEntityLists[i] && n < 200; e = e->next) {
            n++;
        }
        printf(" %d", n);
    }
    printf(" | buckets %d %d %d %d | oam updated %d\n", gDrawBuckets[0].count, gDrawBuckets[1].count,
           gDrawBuckets[2].count, gDrawBuckets[3].count, gOAMControls.updated);
}

#include "player.h"
#include "room.h"
#include "save.h"
#include "message.h"

// TMC_TRACE_PLAYER=n: every n frames, where the player is and what it is doing.
void TracePlayer(u64 frame) {
    Entity* p = &gPlayerEntity.base;
    printf("frame %llu: task %d.%d.%d area %02x room %02x pos %d,%d (room %d,%d) z %d dir %d action %d/%d "
           "player state %x/%x hp %d msg %d\n",
           (unsigned long long)frame, gMain.task, gMain.state, gMain.substate, gRoomControls.area,
           gRoomControls.room, p->x.HALF.HI, p->y.HALF.HI, p->x.HALF.HI - gRoomControls.origin_x,
           p->y.HALF.HI - gRoomControls.origin_y, p->z.HALF.HI, p->direction, p->action, p->subAction,
           gPlayerState.framestate, gPlayerState.flags, gSave.stats.health, gMessage.state);
}
