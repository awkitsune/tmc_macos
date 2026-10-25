/*
 * zMalloc's call-site wrapper (include/common.h): each heap block records the
 * type it was allocated for, so a save state knows which of its bytes are
 * pointers (port/include/port/state_map.h). In the state segment.
 */

#define ZMALLOC_DEFINITION
#include "global.h"
#include "common.h"

#include <string.h>

#include "port/state_map.h"

extern u8 gzHeap[0x1000];

uint32_t gzHeapTypes[0x1000 / 4];

void* PortZMalloc(u32 size, const char* file, u32 line) {
    u8* block = zMalloc(size);

    if (block != NULL) {
        // A call the map does not know (it is out of date): saving refuses.
        uint32_t hash = UINT32_MAX;
        for (uint32_t i = 0; i < gStateMapHeapSiteCount; i++) {
            if (gStateMapHeapSites[i].line == line && strcmp(gStateMapHeapSites[i].file, file) == 0) {
                hash = gStateMapHeapSites[i].hash;
                break;
            }
        }
        gzHeapTypes[(block - gzHeap) / 4] = hash;
    }
    return block;
}
