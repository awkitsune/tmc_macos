/*
 * Game data from the player's ROM (see port/romdata.h).
 *
 * datagen's manifest has two parts. The runs (__DATA,__tmcruns, one table per
 * data object, concatenated by the linker) say where each stretch of ROM bytes
 * goes natively. The extents (rom_manifest.s) are those ROM stretches merged,
 * sorted and numbered into the cache: the cache's data is the bytes of every
 * extent back to back. Extracting copies extents out of the ROM; loading copies
 * each run out of the cache.
 */

#include "port/romdata.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint8_t* dest;
    uint32_t rom;
    uint32_t length;
} RomRun;

typedef struct {
    uint32_t rom;
    uint32_t length;
    uint32_t cache;
    uint32_t category; // datagen's: 0 tables, 1 graphics, 2 maps, 3 sound
} RomExtent;

extern const uint64_t gRomManifestVersion;
extern const uint32_t gRomDataBaked;
extern const uint32_t gRomCacheSize;
extern const uint32_t gRomExtentCount;
extern const RomExtent gRomExtents[];
extern const RomRun sRunsStart __asm("section$start$__DATA$__tmcruns");
extern const RomRun sRunsEnd __asm("section$end$__DATA$__tmcruns");

#define ROM_SIZE 0x1000000
#define ROM_MAX_READ (64 << 20) // anything larger is not a GBA ROM
#define COPIER_HEADER 512
#define CACHE_NAME "gamedata.bin"
#define CACHE_MAGIC "TMCDATA"
#define CACHE_FORMAT 1
#define CACHE_HEADER_SIZE 4096 // the data starts page-aligned

typedef struct {
    char magic[8];
    uint32_t format;
    uint32_t headerSize;
    uint64_t manifestVersion;
    uint64_t dataSize;
    uint8_t romSha1[CC_SHA1_DIGEST_LENGTH];
} CacheHeader;

typedef struct {
    const char* sha1;
    const char* name;
} KnownRom;

// The repository's tmc*.sha1 files.
static const char kUsaSha1[] = "b4bd50e4131b027c334547b4524e2dbbd4227130";
static const KnownRom kOtherRoms[] = {
    { "cff199b36ff173fb6faf152653d1bccf87c26fb7", "the European release" },
    { "6c5404a1effb17f481f352181d0f1c61a2765c5d", "the Japanese release" },
    { "63fcad218f9047b6a9edbb68c98bd0dec322d7a1", "the USA demo" },
    { "9cdb56fa79bba13158b81925c1f3641251326412", "the Japanese demo" },
};

static const RomDataStage kCategoryStage[] = {
    ROMDATA_STAGE_TABLES, ROMDATA_STAGE_GRAPHICS, ROMDATA_STAGE_MAPS, ROMDATA_STAGE_SOUND,
};

// Share of the progress bar: hashing, copying, writing.
#define PROGRESS_VERIFY 0.35
#define PROGRESS_COPY 0.25

bool RomDataIsBaked(void) {
    return gRomDataBaked != 0;
}

const char* RomDataStageName(RomDataStage stage) {
    switch (stage) {
        case ROMDATA_STAGE_VERIFY:
            return "Verifying ROM";
        case ROMDATA_STAGE_TABLES:
            return "Extracting game tables";
        case ROMDATA_STAGE_GRAPHICS:
            return "Extracting graphics";
        case ROMDATA_STAGE_MAPS:
            return "Extracting maps";
        case ROMDATA_STAGE_SOUND:
            return "Extracting sound";
        case ROMDATA_STAGE_WRITE:
            return "Saving game data";
        case ROMDATA_STAGE_DONE:
            return "Done";
    }
    return "";
}

static RomDataResult Fail(RomDataResult result, char* message, size_t size, const char* format, ...) {
    va_list args;

    if (message != NULL && size != 0) {
        va_start(args, format);
        vsnprintf(message, size, format, args);
        va_end(args);
    }
    return result;
}

static void Report(RomDataProgress progress, void* context, RomDataStage stage, double fraction) {
    if (progress != NULL) {
        progress(context, stage, fraction);
    }
}

static double Milliseconds(uint64_t start) {
    mach_timebase_info_data_t timebase;

    mach_timebase_info(&timebase);
    return (double)(mach_absolute_time() - start) * timebase.numer / timebase.denom / 1e6;
}

static bool MakeDirectories(char* path) {
    for (char* p = path + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            bool ok = mkdir(path, 0755) == 0 || errno == EEXIST;
            *p = '/';
            if (!ok) {
                return false;
            }
        }
    }
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

bool RomDataCachePath(char* path, size_t size) {
    const char* dir = getenv("TMC_CACHE_DIR");
    const char* home = getenv("HOME");
    char folder[1024];
    int n;

    if (dir != NULL && dir[0] != '\0') {
        n = snprintf(folder, sizeof(folder), "%s", dir);
    } else {
        n = snprintf(folder, sizeof(folder), "%s/Library/Application Support/tmc_macos", home != NULL ? home : "");
    }
    if (n < 0 || (size_t)n >= sizeof(folder)) {
        return false;
    }
    MakeDirectories(folder); // a failure shows up when the cache is written
    n = snprintf(path, size, "%s/%s", folder, CACHE_NAME);
    return n > 0 && (size_t)n < size;
}

static const RomExtent* FindExtent(uint32_t rom) {
    uint32_t lo = 0, hi = gRomExtentCount;

    while (lo < hi) { // first extent starting after rom
        uint32_t mid = (lo + hi) / 2;
        if (gRomExtents[mid].rom <= rom) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return NULL;
    }
    const RomExtent* e = &gRomExtents[lo - 1];
    return rom - e->rom < e->length ? e : NULL;
}

static void HexDigest(const uint8_t* digest, char* hex) {
    for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" // SHA1 is what ROM checksums use
static void Sha1(const uint8_t* data, size_t size, uint8_t* digest, RomDataProgress progress, void* context) {
    CC_SHA1_CTX sha;
    const size_t chunk = 1 << 20;

    CC_SHA1_Init(&sha);
    for (size_t done = 0; done < size; done += chunk) {
        size_t n = size - done < chunk ? size - done : chunk;
        CC_SHA1_Update(&sha, data + done, (CC_LONG)n);
        Report(progress, context, ROMDATA_STAGE_VERIFY, PROGRESS_VERIFY * (done + n) / size);
    }
    CC_SHA1_Final(digest, &sha);
}
#pragma clang diagnostic pop

static RomDataResult ReadFile(const char* path, uint8_t** data, size_t* size, char* message, size_t messageSize) {
    const char* name = strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path;
    struct stat st;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return Fail(ROMDATA_UNREADABLE, message, messageSize, "“%s” could not be opened: %s.", name, strerror(errno));
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return Fail(ROMDATA_UNREADABLE, message, messageSize, "“%s” is not a file that can be read.", name);
    }
    if (st.st_size < 0xC0 || st.st_size > ROM_MAX_READ) {
        close(fd);
        return Fail(ROMDATA_NOT_A_ROM, message, messageSize,
                    "“%s” is not a Game Boy Advance ROM (it is %lld bytes; the game's ROM is 16 MB).", name,
                    (long long)st.st_size);
    }
    *size = (size_t)st.st_size;
    *data = malloc(*size);
    size_t done = 0;
    while (*data != NULL && done < *size) {
        ssize_t n = read(fd, *data + done, *size - done);
        if (n <= 0) {
            int error = n < 0 ? errno : EIO;
            close(fd);
            free(*data);
            *data = NULL;
            return Fail(ROMDATA_UNREADABLE, message, messageSize, "“%s” could not be read: %s.", name,
                        strerror(error));
        }
        done += (size_t)n;
    }
    close(fd);
    if (*data == NULL) {
        return Fail(ROMDATA_UNREADABLE, message, messageSize, "Out of memory reading “%s”.", name);
    }
    return ROMDATA_OK;
}

static bool IsMinishCapHeader(const uint8_t* header) {
    return memcmp(header + 0xA0, "GBAZELDA MC", 11) == 0 && memcmp(header + 0xAC, "BZM", 3) == 0;
}

// Read the ROM and make sure it is the original USA release.
static RomDataResult ReadRom(const char* path, uint8_t** rom, uint8_t* sha1, RomDataProgress progress,
                             void* context, char* message, size_t messageSize) {
    const char* name = strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path;
    uint8_t* data;
    size_t size;
    char hex[2 * CC_SHA1_DIGEST_LENGTH + 1];
    RomDataResult result;

    Report(progress, context, ROMDATA_STAGE_VERIFY, 0);
    if ((result = ReadFile(path, &data, &size, message, messageSize)) != ROMDATA_OK) {
        return result;
    }
    Sha1(data, size, sha1, progress, context);
    HexDigest(sha1, hex);
    if (strcmp(hex, kUsaSha1) == 0) {
        *rom = data;
        return ROMDATA_OK;
    }
    const char* known = NULL;
    for (size_t i = 0; i < sizeof(kOtherRoms) / sizeof(kOtherRoms[0]); i++) {
        if (strcmp(hex, kOtherRoms[i].sha1) == 0) {
            known = kOtherRoms[i].name;
        }
    }
    if (known != NULL) {
        result = Fail(ROMDATA_WRONG_VERSION, message, messageSize,
                      "“%s” is %s of The Minish Cap. This port is built from the USA release and needs that ROM.",
                      name, known);
    } else if (size % 0x8000 == COPIER_HEADER && size > COPIER_HEADER + 0xC0 && IsMinishCapHeader(data + COPIER_HEADER)) {
        result = Fail(ROMDATA_HEADERED, message, messageSize,
                      "“%s” starts with a 512-byte copier header. Use the plain 16 MB ROM, or remove the first "
                      "512 bytes of the file.",
                      name);
    } else if (!IsMinishCapHeader(data)) {
        result = Fail(ROMDATA_NOT_A_ROM, message, messageSize,
                      "“%s” is not a ROM of The Legend of Zelda: The Minish Cap.", name);
    } else if (data[0xAF] != 'E') {
        const char* region = data[0xAF] == 'P' ? "the European release" : data[0xAF] == 'J' ? "the Japanese release" : "another release";
        result = Fail(ROMDATA_WRONG_VERSION, message, messageSize,
                      "“%s” is %s of The Minish Cap. This port is built from the USA release and needs that ROM.",
                      name, region);
    } else if (size != ROM_SIZE) {
        result = Fail(ROMDATA_MODIFIED, message, messageSize,
                      "“%s” has the USA game's header but is %zu bytes instead of 16 MB, so it is a trimmed, "
                      "patched or damaged copy.",
                      name, size);
    } else {
        result = Fail(ROMDATA_MODIFIED, message, messageSize,
                      "“%s” has the USA game's header but different contents (SHA1 %s, expected %s). Patched "
                      "ROMs, translations and bad dumps are not supported.",
                      name, hex, kUsaSha1);
    }
    free(data);
    return result;
}

static bool WriteAll(int fd, const uint8_t* data, size_t size, RomDataProgress progress, void* context,
                     double from, double to) {
    const size_t chunk = 1 << 20;

    for (size_t done = 0; done < size;) {
        size_t n = size - done < chunk ? size - done : chunk;
        ssize_t written = write(fd, data + done, n);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            if (written == 0) {
                errno = EIO;
            }
            return false;
        }
        done += (size_t)written;
        Report(progress, context, ROMDATA_STAGE_WRITE, from + (to - from) * done / size);
    }
    return true;
}

RomDataResult RomDataExtract(const char* romPath, const char* cachePath, RomDataProgress progress, void* context,
                             char* message, size_t messageSize) {
    uint8_t sha1[CC_SHA1_DIGEST_LENGTH];
    uint8_t* rom;
    uint8_t* page;
    char temp[1100];
    RomDataResult result;

    if ((result = ReadRom(romPath, &rom, sha1, progress, context, message, messageSize)) != ROMDATA_OK) {
        return result;
    }

    // The data part of the cache: every extent, stage by stage.
    page = calloc(1, CACHE_HEADER_SIZE + (size_t)gRomCacheSize);
    if (page == NULL) {
        free(rom);
        return Fail(ROMDATA_WRITE_FAILED, message, messageSize, "Out of memory extracting the game data.");
    }
    uint8_t* data = page + CACHE_HEADER_SIZE;
    size_t copied = 0;
    for (uint32_t category = 0; category < sizeof(kCategoryStage) / sizeof(kCategoryStage[0]); category++) {
        for (uint32_t i = 0; i < gRomExtentCount; i++) {
            const RomExtent* e = &gRomExtents[i];
            if (e->category != category) {
                continue;
            }
            memcpy(data + e->cache, rom + e->rom, e->length);
            copied += e->length;
            Report(progress, context, kCategoryStage[category],
                   PROGRESS_VERIFY + PROGRESS_COPY * copied / (gRomCacheSize ? gRomCacheSize : 1));
        }
    }
    free(rom);

    CacheHeader* header = (CacheHeader*)page;
    memcpy(header->magic, CACHE_MAGIC, sizeof(CACHE_MAGIC));
    header->format = CACHE_FORMAT;
    header->headerSize = CACHE_HEADER_SIZE;
    header->manifestVersion = gRomManifestVersion;
    header->dataSize = gRomCacheSize;
    memcpy(header->romSha1, sha1, sizeof(sha1));

    // Write next to the cache and rename over it, so a failed write never
    // leaves a damaged cache behind.
    snprintf(temp, sizeof(temp), "%s.%d.tmp", cachePath, (int)getpid());
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0 &&
              WriteAll(fd, page, CACHE_HEADER_SIZE + (size_t)gRomCacheSize, progress, context,
                       PROGRESS_VERIFY + PROGRESS_COPY, 1.0) &&
              fsync(fd) == 0;
    int error = errno;
    if (fd >= 0 && close(fd) != 0 && ok) {
        ok = false;
        error = errno;
    }
    if (ok && rename(temp, cachePath) != 0) {
        ok = false;
        error = errno;
    }
    free(page);
    if (!ok) {
        unlink(temp);
        return Fail(ROMDATA_WRITE_FAILED, message, messageSize, "The game data could not be saved to “%s”: %s.",
                    cachePath, error == ENOSPC ? "the disk is full" : strerror(error));
    }
    Report(progress, context, ROMDATA_STAGE_DONE, 1.0);
    return Fail(ROMDATA_OK, message, messageSize, "Extracted %u bytes of game data.", gRomCacheSize);
}

RomDataResult RomDataLoad(const char* cachePath, char* message, size_t messageSize) {
    CacheHeader header;
    struct stat st;
    uint64_t start = mach_absolute_time();
    int fd = open(cachePath, O_RDONLY);

    if (fd < 0) {
        if (errno == ENOENT) {
            return Fail(ROMDATA_NO_CACHE, message, messageSize, "No game data has been extracted yet.");
        }
        return Fail(ROMDATA_BAD_CACHE, message, messageSize, "The game data at “%s” could not be opened: %s.",
                    cachePath, strerror(errno));
    }
    if (fstat(fd, &st) != 0 || pread(fd, &header, sizeof(header), 0) != sizeof(header) ||
        memcmp(header.magic, CACHE_MAGIC, sizeof(CACHE_MAGIC)) != 0 || header.format != CACHE_FORMAT ||
        header.headerSize < sizeof(header)) {
        close(fd);
        return Fail(ROMDATA_BAD_CACHE, message, messageSize, "The game data at “%s” is damaged.", cachePath);
    }
    if (header.manifestVersion != gRomManifestVersion || header.dataSize != gRomCacheSize) {
        close(fd);
        return Fail(ROMDATA_STALE_CACHE, message, messageSize,
                    "The game data was extracted by a different build of the game and has to be extracted again.");
    }
    if ((uint64_t)st.st_size < header.headerSize + header.dataSize) {
        close(fd);
        return Fail(ROMDATA_BAD_CACHE, message, messageSize, "The game data at “%s” is incomplete.", cachePath);
    }
    uint8_t* file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) {
        return Fail(ROMDATA_BAD_CACHE, message, messageSize, "The game data at “%s” could not be read: %s.",
                    cachePath, strerror(errno));
    }
    const uint8_t* data = file + header.headerSize;
    const RomExtent* e = NULL;
    size_t runs = 0;
    for (const RomRun* run = &sRunsStart; run < &sRunsEnd; run++, runs++) {
        // Runs come in ROM order within each object, so the last extent usually fits.
        if (e == NULL || run->rom < e->rom || run->rom + run->length > e->rom + e->length) {
            e = FindExtent(run->rom);
        }
        if (e == NULL || run->rom + run->length > e->rom + e->length) {
            munmap(file, (size_t)st.st_size);
            return Fail(ROMDATA_BAD_CACHE, message, messageSize, "ROM run %#x+%#x is outside the extracted data.",
                        run->rom, run->length);
        }
        memcpy(run->dest, data + e->cache + (run->rom - e->rom), run->length);
    }
    munmap(file, (size_t)st.st_size);
    return Fail(ROMDATA_OK, message, messageSize, "Loaded %zu runs (%u bytes of extracted data) in %.1f ms.", runs,
                gRomCacheSize, Milliseconds(start));
}

RomDataResult RomDataVerifyBaked(const char* romPath, char* message, size_t messageSize) {
    uint8_t sha1[CC_SHA1_DIGEST_LENGTH];
    uint8_t* rom;
    size_t runs = 0, bytes = 0, bad = 0;
    const RomRun* first = NULL;
    RomDataResult result;

    if ((result = ReadRom(romPath, &rom, sha1, NULL, NULL, message, messageSize)) != ROMDATA_OK) {
        return result;
    }
    for (const RomRun* run = &sRunsStart; run < &sRunsEnd; run++, runs++) {
        bytes += run->length;
        if (FindExtent(run->rom) == NULL || memcmp(run->dest, rom + run->rom, run->length) != 0) {
            first = first != NULL ? first : run;
            bad++;
        }
    }
    free(rom);
    if (bad != 0) {
        return Fail(ROMDATA_MISMATCH, message, messageSize,
                    "%zu of %zu manifest runs differ from the ROM; the first is ROM %#x+%#x.", bad, runs, first->rom,
                    first->length);
    }
    return Fail(ROMDATA_OK, message, messageSize, "all %zu manifest runs (%zu bytes) match the ROM.", runs, bytes);
}

void RomDataPrepareOrExit(const char* program) {
    const char* rom = getenv("TMC_ROM");
    char path[1024], message[512];
    RomDataResult result;

    if (RomDataIsBaked()) {
        if (rom != NULL && rom[0] != '\0') {
            result = RomDataVerifyBaked(rom, message, sizeof(message));
            fprintf(stderr, "%s: built-in data: %s\n", program, message);
            if (result != ROMDATA_OK) {
                exit(2);
            }
        }
        return;
    }
    if (!RomDataCachePath(path, sizeof(path))) {
        fprintf(stderr, "%s: the game data path is too long\n", program);
        exit(2);
    }
    result = RomDataLoad(path, message, sizeof(message));
    if (result != ROMDATA_OK && rom != NULL && rom[0] != '\0') {
        fprintf(stderr, "%s: %s Extracting it from %s.\n", program, message, rom);
        result = RomDataExtract(rom, path, NULL, NULL, message, sizeof(message));
        if (result == ROMDATA_OK) {
            result = RomDataLoad(path, message, sizeof(message));
        }
    } else if (result != ROMDATA_OK) {
        fprintf(stderr,
                "%s: %s\n%s: this build has no game data built in. Set TMC_ROM=path/to/rom.gba (The Minish Cap, "
                "USA) or open TMC.app once to extract it into %s.\n",
                program, message, program, path);
        exit(2);
    }
    if (result != ROMDATA_OK) {
        fprintf(stderr, "%s: %s\n", program, message);
        exit(2);
    }
    if (getenv("TMC_ROMDATA_VERBOSE") != NULL) {
        fprintf(stderr, "%s: %s: %s\n", program, path, message);
    }
}
