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
