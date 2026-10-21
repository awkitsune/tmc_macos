/*
 * Silent stand-in for the m4a sound engine (src/gba/m4a.c and asm/lib/m4a_asm.s),
 * which is left out of the native build until phase 4: its song data holds raw
 * GBA addresses. Only the API the game calls is provided.
 */

#include "global.h"
#include "gba/m4a.h"

void m4aSoundInit(void) {
}

void m4aSoundMain(void) {
}

void m4aSoundVSync(void) {
}

void m4aSoundVSyncOn(void) {
}

void m4aSoundVSyncOff(void) {
}

void m4aSongNumStart(u16 n) {
}

void m4aSongNumStartOrContinue(u16 n) {
}

void m4aSongNumStop(u16 n) {
}

void m4aMPlayAllStop(void) {
}

void m4aMPlayImmInit(MusicPlayerInfo* mplayInfo) {
}

void m4aMPlayTempoControl(MusicPlayerInfo* mplayInfo, u16 tempo) {
}

void m4aMPlayVolumeControl(MusicPlayerInfo* mplayInfo, u16 trackBits, u16 volume) {
}
