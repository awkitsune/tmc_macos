// DSi-mode toolchain check for the TMC port: reports free main RAM, lists the
// SD card root and writes hello.txt there, then reads it back. Built with
// TMC_GBA_EWRAM (make GBA_EWRAM=1) it is linked at 0x02040000 and also checks
// that 0x02000000-0x0203FFFF is RAM nobody else uses. The report goes
// to the screen and to gHelloOut, which a debugger reads once gHelloDone is 1
// (port/dsi/scripts/melonds_run.py).
#include <dirent.h>
#include <fat.h>
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char gHelloOut[4096];
volatile int gHelloDone;
static size_t sOutLen;

#define OUT(...)                                                                               \
    do {                                                                                       \
        printf(__VA_ARGS__);                                                                   \
        if (sOutLen < sizeof(gHelloOut))                                                       \
            sOutLen += snprintf(gHelloOut + sOutLen, sizeof(gHelloOut) - sOutLen, __VA_ARGS__); \
    } while (0)

// How much malloc hands out in total, in 64 KB blocks (all freed again).
static size_t MallocTotal(void) {
    enum { BLOCK = 64 << 10, MAX_BLOCKS = 512 };
    static void* blocks[MAX_BLOCKS];
    size_t count = 0;
    while (count < MAX_BLOCKS && (blocks[count] = malloc(BLOCK)) != NULL)
        count++;
    for (size_t i = 0; i < count; i++)
        free(blocks[i]);
    return count * BLOCK;
}

#ifdef TMC_GBA_EWRAM
// Fills the GBA EWRAM range through the cached mapping, flushes, and checks the
// pattern through the uncached mirror at 0x0C000000 (DSi) and back through the cache.
static void CheckGbaEwram(void) {
    enum { BASE = 0x02000000, SIZE = 0x40000 };
    volatile u32* cached = (volatile u32*)BASE;
    for (u32 i = 0; i < SIZE / 4; i++)
        cached[i] = 0xA5000000u ^ (i * 2654435761u);
    DC_FlushRange((void*)BASE, SIZE);
    volatile u32* uncached = (volatile u32*)(isDSiMode() ? BASE - 0x02000000 + 0x0C000000 : BASE + 0x400000);
    u32 bad = 0, badCached = 0;
    for (u32 i = 0; i < SIZE / 4; i++) {
        u32 want = 0xA5000000u ^ (i * 2654435761u);
        bad += uncached[i] != want;
        badCached += cached[i] != want;
    }
    OUT("gba ewram 0x%08x-0x%08x: %u/%u words wrong (uncached), %u (cached)\n", BASE, BASE + SIZE - 1,
        (unsigned)bad, SIZE / 4, (unsigned)badCached);
}
#endif

static void ListRoot(const char* root) {
    DIR* dir = opendir(root);
    int count = 0;
    if (dir != NULL) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            OUT("  %s%s\n", entry->d_name, entry->d_type == DT_DIR ? "/" : "");
            count++;
        }
        closedir(dir);
    }
    OUT("%s: %d entries%s\n", root, count, dir == NULL ? " (opendir failed)" : "");
}

// Writes the report so far to path, then checks the file's size by stat and by reading it.
static void WriteTest(const char* path) {
    size_t len = sOutLen;
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        OUT("fopen %s: FAILED\n", path);
        return;
    }
    size_t written = fwrite(gHelloOut, 1, len, f);
    int closed = fclose(f);
    OUT("write %s: %u of %u bytes, fclose %d\n", path, (unsigned)written, (unsigned)len, closed);

    struct stat st;
    OUT("stat size: %ld\n", stat(path, &st) == 0 ? (long)st.st_size : -1L);
    f = fopen(path, "rb");
    if (f != NULL) {
        static char back[sizeof(gHelloOut)];
        size_t got = fread(back, 1, sizeof(back), f);
        fclose(f);
        OUT("read back: %u bytes, %s\n", (unsigned)got,
            got == len && memcmp(back, gHelloOut, len) == 0 ? "match" : "MISMATCH");
    }
}

int main(int argc, char** argv) {
    consoleDemoInit();

    extern char __end__[];
    OUT("mode: %s\n", isDSiMode() ? "DSi" : "DS");
    OUT("argv0: %s\n", argc > 0 && argv[0] ? argv[0] : "(none)");
    OUT("main at: %p\n", (void*)main);
    OUT("binary end: %p\n", (void*)__end__);
    OUT("heap start: %p\n", (void*)getHeapStart());
    OUT("heap limit: %p\n", (void*)getHeapLimit());
    OUT("heap span: %u KB\n", (unsigned)((getHeapLimit() - getHeapStart()) >> 10));
    OUT("malloc total: %u KB\n", (unsigned)(MallocTotal() >> 10));
#ifdef TMC_GBA_EWRAM
    CheckGbaEwram();
#endif

    bool fat = fatInitDefault();
    OUT("fatInitDefault: %s\n", fat ? "ok" : "FAILED");
    // DLDI (fat:) first: in DSi mode an emulator may offer both, and the internal
    // SD slot (sd:) is the one TWiLight Menu++ and Unlaunch use on hardware.
    static const char* const roots[] = { "fat:/", "sd:/" };
    for (int i = 0; fat && i < 2; i++) {
        DIR* probe = opendir(roots[i]);
        if (probe == NULL) {
            OUT("%s not mounted\n", roots[i]);
            continue;
        }
        closedir(probe);
        char path[32];
        ListRoot(roots[i]);
        snprintf(path, sizeof(path), "%shello.txt", roots[i]);
        WriteTest(path);
        DC_FlushAll();
    }
    OUT("done\n");
    gHelloDone = 1;
    DC_FlushAll();

    while (1) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START)
            break;
    }
    return 0;
}
