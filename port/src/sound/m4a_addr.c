/*
 * GBA addresses in song data to native pointers.
 *
 * Song headers and track bytecode keep their GBA form natively (they are copied
 * byte for byte), so the voicegroup and track pointers in song headers, GOTO /
 * PATT / REPT targets and XCMD wave pointers are GBA ROM addresses. datagen's
 * gSoundRomMap (build/native/data/sound_map.s) maps the sound data: linear
 * entries for data copied as is, strided ones for re-laid-out tables such as the
 * voicegroups' ToneData.
 *
 * Some song headers are placeholders whose fields are not addresses (the unused
 * BGM slots share a 4-byte header with no tracks); those translate to NULL with
 * a warning, which is harmless as long as nothing plays from them.
 */

#include <stdio.h>
#include <stdlib.h>

#include "global.h"
#include "port/hw.h"
#include "port/sound.h"

typedef struct SoundRomMapEntry {
    uint32_t address;
    uint32_t size;
    uint16_t gbaStride; // 0: linear
    uint16_t nativeStride;
    uint32_t pad;
    uint8_t* native;
} SoundRomMapEntry;

extern const SoundRomMapEntry gSoundRomMap[];
extern const uint32_t gSoundRomMapCount;

static uint32_t sLastUnmapped;

void* M4aGbaPtr(uint32_t address) {
    uint32_t lo = 0, hi = gSoundRomMapCount;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (gSoundRomMap[mid].address <= address) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo > 0) {
        const SoundRomMapEntry* e = &gSoundRomMap[lo - 1];
        uint32_t offset = address - e->address;
        if (offset < e->size) {
            if (e->gbaStride == 0) {
                return e->native + offset;
            }
            if (offset % e->gbaStride == 0) {
                return e->native + offset / e->gbaStride * e->nativeStride;
            }
            fprintf(stderr, "M4aGbaPtr: 0x%08x points inside a re-laid-out table element\n", address);
            abort();
        }
    }
    if ((address >> 24) == 0x02 || (address >> 24) == 0x03) {
        return HwGbaPtr(address);
    }
    if (address != sLastUnmapped) {
        fprintf(stderr, "M4aGbaPtr: no native sound data at GBA address 0x%08x\n", address);
        sLastUnmapped = address;
    }
    return NULL;
}
