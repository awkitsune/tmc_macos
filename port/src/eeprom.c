/*
 * Emulated 8 KB cartridge EEPROM, replacing src/eeprom.c (which drives the chip
 * through DMA). Blocks are 8 bytes, addressed by index, as the game sees them.
 *
 * The contents live in memory; with TMC_SAVE=path they are loaded from and
 * written through to that file. The app sets TMC_SAVE to its save in
 * ~/Library/Application Support/tmc_macos unless it is already set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "gba/eeprom.h"

#define EEPROM_BLOCKS 0x400
#define EEPROM_BLOCK_SIZE 8

static u8 sEeprom[EEPROM_BLOCKS * EEPROM_BLOCK_SIZE];
static bool32 sLoaded;

static const char* SavePath(void) {
    return getenv("TMC_SAVE");
}

static void LoadEeprom(void) {
    const char* path = SavePath();
    FILE* f;

    if (sLoaded) {
        return;
    }
    sLoaded = TRUE;
    memset(sEeprom, 0xFF, sizeof(sEeprom)); // erased EEPROM reads as all ones
    if (path != NULL && (f = fopen(path, "rb")) != NULL) {
        fread(sEeprom, 1, sizeof(sEeprom), f);
        fclose(f);
    }
}

static void StoreBlock(u16 address) {
    const char* path = SavePath();
    FILE* f;

    if (path == NULL) {
        return;
    }
    // Write the whole image: small, and keeps the file valid if it did not exist.
    if ((f = fopen(path, "wb")) != NULL) {
        fwrite(sEeprom, 1, sizeof(sEeprom), f);
        fclose(f);
    }
}

u16 EEPROMConfigure(u16 type) {
    LoadEeprom();
    return type == 0x40 ? 0 : 1; // only the 8 KB (64 kbit) chip is emulated
}

u16 EEPROMRead(u16 address, u16* data) {
    if (address >= EEPROM_BLOCKS) {
        return EEPROM_OUT_OF_RANGE;
    }
    LoadEeprom();
    memcpy(data, &sEeprom[address * EEPROM_BLOCK_SIZE], EEPROM_BLOCK_SIZE);
    return 0;
}

u16 EEPROMCompare(u16 address, const u16* data) {
    if (address >= EEPROM_BLOCKS) {
        return EEPROM_OUT_OF_RANGE;
    }
    LoadEeprom();
    return memcmp(data, &sEeprom[address * EEPROM_BLOCK_SIZE], EEPROM_BLOCK_SIZE) ? EEPROM_COMPARE_FAILED : 0;
}

u16 EEPROMWrite0_8k_Check(u16 address, const u16* data) {
    if (address >= EEPROM_BLOCKS) {
        return EEPROM_OUT_OF_RANGE;
    }
    LoadEeprom();
    memcpy(&sEeprom[address * EEPROM_BLOCK_SIZE], data, EEPROM_BLOCK_SIZE);
    StoreBlock(address);
    return 0;
}
