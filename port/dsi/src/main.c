/*
 * DSi port entry point: boot log on the bottom screen, the GBA RAM cleared,
 * the ROM loaded and relocated, then the game.
 */

#include <nds.h>
#include <nds/arm9/exceptions.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "port/hw.h"
#include "port/platform.h"

#include "dsi.h"

char gDsiLog[DSI_LOG_SIZE];
static size_t sLogLength;

// Set when main() stops: the log is complete (read through GDB).
volatile int gDsiLogDone;
volatile bool gDsiHalted;

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
    if (DSI_CHECKS) {
        fputs(line, stdout);
    }
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

// IRQs on in the CPU (ARM code: Thumb has no MSR) and in IME.
__attribute__((target("arm"), noinline)) static void EnableInterrupts(void) {
    uint32_t cpsr;

    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("msr cpsr_c, %0" : : "r"(cpsr & ~0x80u) : "memory");
    REG_IME = 1;
}

/*
 * An exception enters with interrupts off, and the SD card is the ARM7's: a
 * write waits for its reply through the FIFO interrupt. So the interrupts go
 * back on, with the VBlank's game work and the sound stopped first, and the
 * log is written. The console already shows it if that write never returns.
 */
static _Noreturn void Halt(void) {
    gDsiHalted = true;
    if (!DSI_CHECKS) {
        DsiShowLog();
    }
    DsiSoundStop();
    gDsiLogDone = 1;
    DC_FlushAll();
    EnableInterrupts();
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

// ---- stacks --------------------------------------------------------------------

/*
 * The DTCM stacks (BlocksDS's ARM9 layout): the game's and the port's below
 * __sp_usr down to the end of the DTCM data, then 256 bytes each for IRQ and
 * SVC mode. Filled at boot; the words still holding the pattern are what was
 * never used. Under the user stack is main RAM, the end of the ROM copy: an
 * overflow would not fault.
 */
#define STACK_FILL 0x5AC3A55Cu

extern uint32_t __dtcm_end[], __sp_usr[], __sp_irq[], __sp_svc[];

static uint32_t Unused(const uint32_t* bottom, const uint32_t* top) {
    const uint32_t* p = bottom;

    while (p < top && *p == STACK_FILL) {
        p++;
    }
    return (uint32_t)(p - bottom) * 4;
}

static void FillStacks(void) {
    int irq = enterCriticalSection();
    uint32_t* sp = (uint32_t*)__builtin_frame_address(0) - 64; // clear of this frame

    for (uint32_t* p = __dtcm_end; p < sp; p++) {
        *p = STACK_FILL;
    }
    for (uint32_t* p = __sp_usr; p < __sp_svc; p++) {
        *p = STACK_FILL;
    }
    leaveCriticalSection(irq);
}

void DsiStacksUnused(uint32_t* user, uint32_t* irq, uint32_t* svc) {
    *user = Unused(__dtcm_end, __sp_usr);
    *irq = Unused(__sp_usr, __sp_irq);
    *svc = Unused(__sp_irq, __sp_svc);
}

// ---- CPU exceptions --------------------------------------------------------------

/*
 * BlocksDS's handler calls DsiException in the exception's mode (abort or
 * undefined instruction), with the interrupts off, on a small stack at the
 * end of the ROM copy. The registers are kept there; the report then runs in
 * system mode on a stack of its own, as ordinary code, so that the log reaches
 * the card (Halt) whatever state the faulting code left its stack in. The
 * console shows the same lines first, at most 32 characters each.
 */
#define BIOS_EXCEPTION_CPSR 0x02FFFD90 // where the BIOS saved the faulting CPSR (BlocksDS reads it there)

static uint32_t sCrash[18];                                    // r0-r15, the faulting CPSR, the handler's CPSR
static uint32_t sCrashStack[2048] __attribute__((aligned(8))); // 8 KB

__attribute__((target("arm"), noinline)) static uint32_t Cpsr(void) {
    uint32_t cpsr;

    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    return cpsr;
}

// System mode, interrupts still off, sp = top; then fn (r0), which does not return.
__attribute__((target("arm"), naked, noreturn)) static void RunOnStack(void (*fn)(void), uint32_t* top) {
    __asm__ volatile("msr cpsr_c, #0xDF\n\t"
                     "mov sp, r1\n\t"
                     "bx r0\n\t");
}

static const char* ModeName(uint32_t cpsr) {
    switch (cpsr & 0x1F) {
        case 0x10: return "user";
        case 0x11: return "FIQ";
        case 0x12: return "IRQ";
        case 0x13: return "SVC";
        case 0x17: return "abort";
        case 0x1B: return "undef";
        case 0x1F: return "system";
        default: return "?";
    }
}

// Three words a line from address a, if it is in main RAM (with the DTCM).
static void LogWords(const char* what, uint32_t a, int count) {
    const uint32_t* p = (const uint32_t*)a;

    if ((a & 3) != 0 || a < 0x02000000 || a + count * 4 > 0x03000000) {
        DsiLog("%s %08lx: not RAM\n", what, (unsigned long)a);
        return;
    }
    DsiLog("%s %08lx:\n", what, (unsigned long)a);
    for (int i = 0; i < count; i += 3) {
        DsiLog(" %08lx %08lx %08lx\n", (unsigned long)p[i], (unsigned long)p[i + 1], (unsigned long)p[i + 2]);
    }
}

static _Noreturn void CrashReport(void) {
    const uint32_t* r = sCrash;
    uint32_t user, irq, svc;

    // PC as the BIOS saved it: 8 past the instruction for an abort, 4 (ARM)
    // or 2 (Thumb) past it for an undefined one.
    DsiLog("exception: %s, in %s %s\n", ModeName(r[17]), ModeName(r[16]), r[16] & 0x20 ? "thumb" : "arm");
    DsiLog("pc %08lx lr %08lx\n", (unsigned long)r[15], (unsigned long)r[14]);
    DsiLog("sp %08lx cpsr %08lx\n", (unsigned long)r[13], (unsigned long)r[16]);
    for (int i = 0; i < 12; i += 2) {
        DsiLog("r%-2d %08lx r%-2d %08lx\n", i, (unsigned long)r[i], i + 1, (unsigned long)r[i + 1]);
    }
    DsiLog("r12 %08lx frame %lu\n", (unsigned long)r[12], (unsigned long)gPlatformFrame);
    LogWords("at sp", r[13], 6);
    // The BIOS's IRQ entry pushes r0-r3, r12 and lr (the interrupted PC + 4) at
    // the top of the IRQ stack: what the last interrupt returned to.
    LogWords("irq frame", (uint32_t)__sp_irq - 24, 6);
    DsiStacksUnused(&user, &irq, &svc);
    DsiLog("unused stack: %lu %lu %lu\n", (unsigned long)user, (unsigned long)irq, (unsigned long)svc);
    DsiFatal("exception");
}

void DsiException(void) {
    memcpy(sCrash, (const void*)exceptionRegisters, 16 * sizeof(uint32_t));
    sCrash[16] = *(volatile uint32_t*)BIOS_EXCEPTION_CPSR;
    sCrash[17] = Cpsr();
    RunOnStack(CrashReport, sCrashStack + sizeof(sCrashStack) / sizeof(sCrashStack[0]));
}

extern uint8_t __end__[];

int main(int argc, char** argv) {
    uint32_t reserved, crc;

    FillStacks();
    consoleDemoInit();
    DsiScreenBoot();
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
    if (DSI_CHECKS) {
        // 1.5 s on the DSi; tools/rom_crc.py prints the same for the build.
        cpuStartTiming(0);
        crc = HwCrc32(0, __rom_copy, __rom_copy_end - __rom_copy);
        DsiLog("copy CRC32 %08lx (%lu ms)\n", (unsigned long)crc, (unsigned long)timerTicks2msec(cpuEndTiming()));
    }
    if (gDsiSettingsPath != NULL) {
        DsiSettingsLoad(gDsiSettingsPath);
    }
    DsiSettingsMenu(gDsiSettingsPath);
    DsiProtectGbaSlot();
    DsiLogFlush();
#ifdef DSI_TEST_STRAY_READ
    DsiLog("test: reading 0x08000000\n");
    DsiLog("read %08lx, no fault\n", (unsigned long)*(volatile uint32_t*)0x08000000);
#endif

    DsiRunGame();
}
