/*
 * CRC-32 of the hardware shadows, the per-frame check the DSi port compares with
 * the Mac headless runner (TMC_HW_HASH, port/dsi/PLAN.md milestone 4). The same
 * file builds on both, so the two cannot drift apart. The CRC is zlib's.
 */

#include "port/hw.h"

static uint32_t sTable[256];

uint32_t HwCrc32(uint32_t crc, const void* data, size_t length) {
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

uint32_t HwShadowCrc(void) {
    uint32_t crc = HwCrc32(0, gHwIo, HW_LINE_REGS);

    crc = HwCrc32(crc, gHwPltt, HW_PLTT_SIZE);
    crc = HwCrc32(crc, gHwVram, HW_VRAM_SIZE);
    return HwCrc32(crc, gHwOam, HW_OAM_SIZE);
}
