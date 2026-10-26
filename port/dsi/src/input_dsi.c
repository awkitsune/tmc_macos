/*
 * Scripted buttons for DSI_CHECKS builds: tmc/input.txt on the SD card (or
 * DLDI), in the Mac's TMC_INPUT syntax, so a run past the title screen can be
 * compared with the Mac's frame for frame. Only the frame steps:
 *
 *   F:keys:D     hold keys (a b select start right left up down r l, joined
 *                with +) for D frames (default 1) from frame F
 *   +N:keys:D    the same, N frames after the previous step started
 *
 * Steps are separated by commas or whitespace; '#' starts a comment. The
 * Mac's wait, mash, gox/goy and save steps read the game's state and are not
 * supported here.
 */

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsi.h"

#define MAX_WINDOWS 4096

typedef struct {
    uint32_t start, end; // frames start .. end - 1
    uint16_t keys;
} Window;

static const char* const sPaths[] = { "sd:/tmc/input.txt", "fat:/tmc/input.txt" };
static const char* const sKeyNames[] = { "a", "b", "select", "start", "right", "left", "up", "down", "r", "l" };
static Window sWindows[MAX_WINDOWS];
static int sCount;

static uint16_t ParseKeys(char* keys, const char* step) {
    uint16_t mask = 0;
    char* rest;

    for (char* name = strtok_r(keys, "+", &rest); name != NULL; name = strtok_r(NULL, "+", &rest)) {
        int i = 0;
        while (i < 10 && strcmp(name, sKeyNames[i]) != 0) {
            i++;
        }
        if (i == 10) {
            DsiFatal("input.txt: unknown button '%s' in '%s'\n", name, step);
        }
        mask |= 1 << i;
    }
    return mask;
}

static void ParseStep(char* step, uint32_t* base) {
    char copy[64], *keys, *duration;
    int relative = step[0] == '+';
    uint32_t frame;

    snprintf(copy, sizeof(copy), "%s", step);
    keys = strchr(step, ':');
    if (keys == NULL || !(relative || (step[0] >= '0' && step[0] <= '9'))) {
        DsiFatal("input.txt: step '%s' not supported (frame steps only)\n", copy);
    }
    *keys++ = 0;
    duration = strchr(keys, ':');
    if (duration != NULL) {
        *duration++ = 0;
    }
    if (sCount == MAX_WINDOWS) {
        DsiFatal("input.txt: over %d steps\n", MAX_WINDOWS);
    }
    frame = strtoul(step + relative, NULL, 10) + (relative ? *base : 0);
    sWindows[sCount].start = frame;
    sWindows[sCount].end = frame + (duration != NULL ? strtoul(duration, NULL, 10) : 1);
    sWindows[sCount].keys = ParseKeys(keys, copy);
    sCount++;
    *base = frame;
}

void DsiInputLoad(void) {
    static char text[65536];
    FILE* file = NULL;
    size_t size;
    uint32_t base = 0;
    char *step, *rest;

    for (unsigned i = 0; i < sizeof(sPaths) / sizeof(*sPaths) && file == NULL; i++) {
        file = fopen(sPaths[i], "rb");
    }
    if (file == NULL) {
        return;
    }
    size = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[size] = 0;
    for (char *c = text, comment = 0; *c; c++) {
        comment = *c == '#' ? 1 : *c == '\n' ? 0 : comment;
        if (comment || *c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') {
            *c = ',';
        }
    }
    for (step = strtok_r(text, ",", &rest); step != NULL; step = strtok_r(NULL, ",", &rest)) {
        ParseStep(step, &base);
    }
    DsiLog("input script: %d steps\n", sCount);
}

uint16_t DsiInputKeys(uint32_t frame) {
    uint16_t keys = 0;

    for (int i = 0; i < sCount; i++) {
        if (frame >= sWindows[i].start && frame < sWindows[i].end) {
            keys |= sWindows[i].keys;
        }
    }
    return keys;
}
