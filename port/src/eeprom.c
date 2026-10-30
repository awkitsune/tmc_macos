/*
 * Emulated 8 KB cartridge EEPROM, replacing src/eeprom.c (which drives the chip
 * through DMA). Blocks are 8 bytes, addressed by index, as the game sees them.
 *
 * The contents live in memory; with TMC_SAVE=path they are loaded from and
 * written through to that file. The app sets TMC_SAVE to its save in
 * ~/Library/Application Support/tmc_macos unless it is already set.
 *
 * The DSi has no environment: the save is tmc/tmc.sav on the card the ROM came
 * from (port/dsi/src/rom.c). Rewriting the file on the SD card for each 8-byte
 * block would stall a save for seconds, so writes only mark the image dirty and
 * EepromFlush writes it once a frame has passed without one.
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

#ifdef PLATFORM_DSI
const char* DsiSavePath(void);
int EepromFlush(void);

static bool32 sDirty;
static bool32 sWrittenThisFrame;

static const char* SavePath(void) {
    return DsiSavePath();
}
#else
static const char* SavePath(void) {
    return getenv("TMC_SAVE");
}
#endif

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

#ifdef PLATFORM_DSI
static void StoreBlock(u16 address) {
    sDirty = TRUE;
    sWrittenThisFrame = TRUE;
}

/*
 * Once a frame: write the image if it changed and the game wrote nothing since
 * the last call. In place ("r+b") when the file exists, so it is never left
 * truncated. 0 if nothing was written, 1 if it was, -1 if the write failed.
 */
int EepromFlush(void) {
    const char* path = SavePath();
    FILE* f;
    size_t written;

    if (!sDirty || path == NULL) {
        return 0;
    }
    if (sWrittenThisFrame) {
        sWrittenThisFrame = FALSE;
        return 0;
    }
    sDirty = FALSE;
    if ((f = fopen(path, "r+b")) == NULL && (f = fopen(path, "wb")) == NULL) {
        return -1;
    }
    written = fwrite(sEeprom, 1, sizeof(sEeprom), f);
    return fclose(f) == 0 && written == sizeof(sEeprom) ? 1 : -1;
}
#else
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
#endif

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
