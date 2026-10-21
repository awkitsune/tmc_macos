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
