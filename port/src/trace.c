/*
 * Game-state tracing for the headless runner (TMC_TRACE_TASK). Separate from
 * headless.c because the game headers clash with macros from <stdlib.h>.
 */

#include <stdio.h>

#include "global.h"
#include "main.h"

void TraceGameTask(u64 frame) {
    static int sLast = -1;
    int now = (gMain.task << 16) | (gMain.state << 8) | gMain.substate;

    if (now != sLast) {
        printf("frame %llu: task %d state %d substate %d\n", (unsigned long long)frame, gMain.task, gMain.state,
               gMain.substate);
        sLast = now;
    }
}
