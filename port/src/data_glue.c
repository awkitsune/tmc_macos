/*
 * Small pieces of code and data that live inside GBA data objects or are only
 * defined for other game versions.
 */

#include "global.h"
#include "effects.h"

// Embedded as Thumb code in data/scripts/CreateDustFromScript.inc and called
// from scripts; the script context in r1 is passed through and ignored.
void CreateDustFromScript(Entity* entity) {
    CreateDeathFx(entity);
}

// Demo-ROM graphics (data/gfx/sprite_table.s, DEMO_USA only). src/demo.c refers to
// them in every version but only runs in demo builds.
const u8 gUnk_089FD1B4[1];
const u8 gUnk_089FD2F4[1];
const u8 gUnk_08A05751[1];
const u8 gUnk_08A068BF[1];

// The save header the GBA keeps at the start of EWRAM (gSaveHeader). The DSi
// keeps it there too (port/dsi/tools/gen_link.py).
#ifndef PLATFORM_DSI
#include "save.h"
SaveHeader gNativeSaveHeader;
#endif
