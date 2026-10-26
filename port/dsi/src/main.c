/*
 * DSi port entry point: boot log on the bottom screen, the GBA RAM cleared,
 * the ROM loaded and relocated, then the game.
 */

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dsi.h"

char gDsiLog[DSI_LOG_SIZE];
static size_t sLogLength;

// Set when main() stops: the log is complete (read through GDB).
volatile int gDsiLogDone;

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

static _Noreturn void Halt(void) {
    gDsiLogDone = 1;
    DC_FlushAll();
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

int main(void) {
    uint32_t reserved;

    consoleDemoInit();
    DsiLog("TMC DSi port\n");
    DsiLog("DSi mode: %s\n", isDSiMode() ? "yes" : "no");
    if (!isDSiMode()) {
        DsiFatal("needs DSi mode (16 MB)");
    }

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
    DsiRunGame();
}
