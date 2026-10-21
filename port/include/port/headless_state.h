// Game state the headless runner's scripted input waits on (port/src/trace.c).
#pragma once

typedef struct {
    int task, state, substate;
    int area, room;
    int x, y; // player position relative to the room
    int message, fade, control;
} HeadlessState;

void HeadlessGetState(HeadlessState* s);
