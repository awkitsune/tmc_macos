#ifndef PORT_ROMDATA_H
#define PORT_ROMDATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The game's data from the player's ROM.
 *
 * A build configured with TMC_BAKE_DATA=OFF contains none of the assets that
 * the decomp build extracts from baserom.gba (graphics, maps, sound, text...):
 * datagen leaves zeros where they go and a manifest of (native address, ROM
 * offset, length) runs. Data written in the repository's sources stays built
 * in. On first launch the ROM is checked and the bytes those runs need are
 * extracted into one cache file; every launch then fills the runs from the
 * cache before AgbMain. A baked build (the default) has the assets built in and
 * needs none of this.
 *
 * The cache is <dir>/gamedata.bin, where <dir> is $TMC_CACHE_DIR or
 * ~/Library/Application Support/tmc_macos. It is tied to the ROM's SHA1 and to
 * the manifest's version, so a build that extracts different data re-extracts.
 */

typedef enum {
    ROMDATA_OK,
    ROMDATA_NO_CACHE,        // no cache yet
    ROMDATA_STALE_CACHE,     // a cache from another build (different manifest)
    ROMDATA_BAD_CACHE,       // damaged or unreadable cache
    ROMDATA_UNREADABLE,      // the ROM file cannot be opened or read
    ROMDATA_NOT_A_ROM,       // not a GBA ROM, or another game
    ROMDATA_WRONG_VERSION,   // The Minish Cap, but not the USA release
    ROMDATA_HEADERED,        // has a 512-byte copier header
    ROMDATA_MODIFIED,        // USA header, but not the original bytes (hack, bad dump)
    ROMDATA_WRITE_FAILED,    // the cache could not be written
    ROMDATA_MISMATCH,        // baked data differs from the ROM (RomDataVerifyBaked)
} RomDataResult;

typedef enum {
    ROMDATA_STAGE_VERIFY,
    ROMDATA_STAGE_TABLES,
    ROMDATA_STAGE_GRAPHICS,
    ROMDATA_STAGE_MAPS,
    ROMDATA_STAGE_SOUND,
    ROMDATA_STAGE_WRITE,
    ROMDATA_STAGE_DONE,
} RomDataStage;

// Called from the extracting thread; fraction is overall progress, 0 to 1.
typedef void (*RomDataProgress)(void* context, RomDataStage stage, double fraction);

// True when the data is built into this program (no ROM needed).
bool RomDataIsBaked(void);

// "Verifying ROM", "Extracting graphics", ...
const char* RomDataStageName(RomDataStage stage);

// The cache file's path (creating its folder); false if the path is too long.
bool RomDataCachePath(char* path, size_t size);

// Fill the game data from the cache. On failure the data is left untouched.
RomDataResult RomDataLoad(const char* cachePath, char* message, size_t messageSize);

// Check the ROM at romPath and write the cache. message gets a sentence for the
// player on failure. progress may be NULL.
RomDataResult RomDataExtract(const char* romPath, const char* cachePath, RomDataProgress progress, void* context,
                             char* message, size_t messageSize);

// Baked builds: check the ROM and compare every manifest run with the built-in
// data (the manifest's self-test).
RomDataResult RomDataVerifyBaked(const char* romPath, char* message, size_t messageSize);

// Command-line programs: make the data ready or exit with a message. Baked:
// nothing to do (with TMC_ROM set, verifies the manifest against it). Otherwise
// loads the cache, extracting it from $TMC_ROM first if needed.
void RomDataPrepareOrExit(const char* program);

#endif // PORT_ROMDATA_H
