/*
 * DSi port entry point: boot log on the bottom screen, the GBA RAM cleared,
 * the ROM loaded and relocated, then the game.
 */

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "port/hw.h"

#include "dsi.h"

char gDsiLog[DSI_LOG_SIZE];
static size_t sLogLength;

// Set when main() stops: the log is complete (read through GDB).
volatile int gDsiLogDone;

// The log's file (DsiLogToFile); NULL before there is one or after a failed write.
static const char* sLogFile;


static void LogV(const char* format, va_list args) {
    char line[256];
    int length = vsnprintf(line, sizeof(line), format, args);

    if (length < 0) {
        return;
    }
    if ((size_t)length >= sizeof(line)) {
        length = sizeof(line) - 1;
    }
    fputs(line, stdout);
    if (sLogLength + length < sizeof(gDsiLog)) {
        memcpy(gDsiLog + sLogLength, line, length + 1);
        sLogLength += length;
    }
}

void DsiLog(const char* format, ...) {
    va_list args;

    va_start(args, format);
    LogV(format, args);
    va_end(args);
}

void DsiLogToFile(const char* path) {
    sLogFile = path;
    DsiLogFlush();
}

/*
 * The whole log, rewritten and closed each time, so the card holds all of it
 * up to the last flush whatever happens after.
 */
void DsiLogFlush(void) {
    const char* path = sLogFile;
    FILE* file;
    bool ok;

    if (path == NULL) {
        return;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        sLogFile = NULL;
        DsiLog("cannot open %s\n", path);
        return;
    }
    ok = fwrite(gDsiLog, 1, sLogLength, file) == sLogLength;
    if (fclose(file) != 0 || !ok) {
        sLogFile = NULL;
        DsiLog("cannot write %s\n", path);
    }
}

/*
 * With no ROM: the first tmc/ there is, on the SD card or on DLDI; with none,
 * one made on the first card that takes it.
 */
void DsiLogToDefaultFile(void) {
    static const char* const dirs[] = { "sd:/tmc", "fat:/tmc" };
    static const char* const files[] = { "sd:/tmc/boot.txt", "fat:/tmc/boot.txt" };

    for (int make = 0; make < 2; make++) {
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
            struct stat st;

            if (make) {
                mkdir(dirs[i], 0777);
            }
            if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode)) {
                DsiLogToFile(files[i]);
                return;
            }
        }
    }
}

static _Noreturn void Halt(void) {
    gDsiLogDone = 1;
    DC_FlushAll();
    // Last, as it may not return (from an exception, interrupts are off).
    DsiLogFlush();
    while (1) {
        swiWaitForVBlank();
    }
}

_Noreturn void DsiFatal(const char* format, ...) {
    va_list args;

    DsiLog("fatal: ");
    va_start(args, format);
    LogV(format, args);
    va_end(args);
    DsiLog("\n");
    Halt();
}

extern uint8_t __end__[];

int main(int argc, char** argv) {
    uint32_t reserved, crc;

    consoleDemoInit();
    DsiLog("TMC DSi port\n");
    DsiLog("started as %s\n", argc > 0 && argv[0] != NULL ? argv[0] : "(no argv)");
    DsiLog("DSi mode: %s\n", isDSiMode() ? "yes" : "no");
    if (!isDSiMode()) {
        DsiFatal("needs DSi mode (16 MB)");
    }
    DsiLog("ARM9 at 133 MHz (was %s)\n", setCpuClock(true) ? "133" : "67");

    // The ROM copy takes the top of main RAM; keep the heap below it.
    reserved = (uint32_t)getHeapLimit() - (uint32_t)__rom_copy;
    if ((uint32_t)getHeapLimit() > (uint32_t)__rom_copy) {
        reduceHeapSize(reserved);
    }
    DsiLog("binary end %p, ROM copy %p-%p\n", (void*)__end__, (void*)__rom_copy, (void*)__rom_copy_end);
    DsiLog("heap %p-%p\n", getHeapStart(), getHeapLimit());
    if ((uint8_t*)getHeapLimit() > __rom_copy) {
        DsiFatal("heap overlaps the ROM copy");
    }

    DsiClearGbaRam();
    if (!DsiRomLoad() || !DsiRomRelocate()) {
        Halt();
    }
    cpuStartTiming(0);
    crc = HwCrc32(0, __rom_copy, __rom_copy_end - __rom_copy);
    DsiLog("copy CRC32 %08lx (%lu ms)\n", (unsigned long)crc, (unsigned long)timerTicks2msec(cpuEndTiming()));
    DsiProtectGbaSlot();
    DsiLogFlush();
#ifdef DSI_TEST_STRAY_READ
    DsiLog("test: reading 0x08000000\n");
    DsiLog("read %08lx, no fault\n", (unsigned long)*(volatile uint32_t*)0x08000000);
#endif

    DsiRunGame();
}
