/*
 * CRC-32 (the zlib/PNG polynomial, as Python's zlib.crc32): the checks the
 * milestones compare with host tools (port/dsi/PLAN.md, "Milestones").
 */

#include "dsi.h"

static uint32_t sTable[256];

uint32_t DsiCrc32(uint32_t crc, const void* data, size_t length) {
    const uint8_t* p = data;

    if (sTable[1] == 0) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            sTable[i] = c;
        }
    }
    crc = ~crc;
    while (length--) {
        crc = sTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}
