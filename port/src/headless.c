/*
 * Headless platform: runs game logic with no window, picture or sound. Used until
 * the Metal front end exists, and for automated runs (TMC_FRAMES=n stops after n
 * frames).
 */

#include <execinfo.h>
#include <mach-o/dyld.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "port/hw.h"
#include "port/platform.h"

void TraceGameTask(uint64_t frame); // trace.c (kept apart: game headers clash with libc)

#define REG16(offset) (*(volatile uint16_t*)(gHwIo + (offset)))
#define REG_DISPSTAT REG16(0x004)
#define REG_VCOUNT REG16(0x006)
#define REG_KEYINPUT REG16(0x130)

#define DISPSTAT_VBLANK 0x0001
#define DISPSTAT_VCOUNT 0x0004
#define INTR_VBLANK 0x0001
#define INTR_VCOUNT 0x0004

void AgbMain(void);
void HwRaiseInterrupt(uint16_t flag);

uint64_t gPlatformFrame;
static uint64_t sFrameLimit;
static jmp_buf sResetPoint;

// TMC_INPUT="frame:keys:duration,...": hold buttons (a b select start right left
// up down r l, joined with +) for duration frames from the given frame.
typedef struct {
    uint64_t start, end;
    uint16_t keys;
} ScriptedInput;
static ScriptedInput sInputs[64];
static int sInputCount;

static void ParseInput(const char* text) {
    static const char* const names[] = { "a", "b", "select", "start", "right", "left", "up", "down", "r", "l" };
    char buffer[1024];
    char* item;
    char* rest;

    snprintf(buffer, sizeof(buffer), "%s", text);
    for (item = strtok_r(buffer, ",", &rest); item != NULL && sInputCount < 64; item = strtok_r(NULL, ",", &rest)) {
        unsigned long long frame = 0, duration = 1;
        char keys[128] = { 0 };
        uint16_t mask = 0;
        char* name;
        char* keysRest;

        if (sscanf(item, "%llu:%127[^:]:%llu", &frame, keys, &duration) < 2) {
            fprintf(stderr, "TMC_INPUT: cannot parse '%s'\n", item);
            exit(1);
        }
        for (name = strtok_r(keys, "+", &keysRest); name != NULL; name = strtok_r(NULL, "+", &keysRest)) {
            int found = 0;
            for (int i = 0; i < 10; i++) {
                if (strcmp(name, names[i]) == 0) {
                    mask |= 1 << i;
                    found = 1;
                }
            }
            if (!found) {
                fprintf(stderr, "TMC_INPUT: unknown button '%s'\n", name);
                exit(1);
            }
        }
        sInputs[sInputCount++] = (ScriptedInput){ frame, frame + duration, mask };
    }
}

static uint16_t ScriptedKeys(void) {
    uint16_t held = 0;
    for (int i = 0; i < sInputCount; i++) {
        if (gPlatformFrame >= sInputs[i].start && gPlatformFrame < sInputs[i].end) {
            held |= sInputs[i].keys;
        }
    }
    return held;
}

static int sTraceTask;

void PlatformVBlank(void) {
    REG_KEYINPUT = 0x03FF & ~ScriptedKeys(); // active low
    if (sTraceTask) {
        TraceGameTask(gPlatformFrame);
    }

    // Mid-frame VCOUNT match (line 80 is where the game asks for it): sound mixing.
    REG_VCOUNT = REG_DISPSTAT >> 8;
    REG_DISPSTAT |= DISPSTAT_VCOUNT;
    HwRaiseInterrupt(INTR_VCOUNT);
    REG_DISPSTAT &= ~DISPSTAT_VCOUNT;

    REG_VCOUNT = 160;
    REG_DISPSTAT |= DISPSTAT_VBLANK;
    HwRaiseInterrupt(INTR_VBLANK);
    REG_DISPSTAT &= ~DISPSTAT_VBLANK;
    REG_VCOUNT = 0;

    gPlatformFrame++;
    if (sFrameLimit != 0 && gPlatformFrame >= sFrameLimit) {
        printf("headless: stopped after %llu frames\n", (unsigned long long)gPlatformFrame);
        exit(0);
    }
}

// Print a backtrace for offline symbolizing (atos -o tmc -l <load address> <pc>...)
// and exit. Used on crashes, and by TMC_WATCHDOG=n after n seconds to find hangs,
// since attaching a debugger may need extra permissions.
static void ReportAndExit(const char* what, void* address) {
    void* frames[64];
    int count = backtrace(frames, 64);
    char line[160];
    int len = snprintf(line, sizeof(line), "%s at %p, frame %llu, load address 0x%lx\n", what, address,
                       (unsigned long long)gPlatformFrame,
                       0x100000000ul + (unsigned long)_dyld_get_image_vmaddr_slide(0));
    write(STDERR_FILENO, line, len);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    _exit(2);
}

static void OnCrash(int sig, siginfo_t* info, void* context) {
    ReportAndExit(sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "signal", info->si_addr);
}

static void OnWatchdog(int sig) {
    ReportAndExit("watchdog", NULL);
}

static void InstallDiagnostics(void) {
    // An alternate stack so stack overflows are reported too.
    static char sAltStack[64 * 1024];
    stack_t alt = { .ss_sp = sAltStack, .ss_size = sizeof(sAltStack) };
    struct sigaction action = { 0 };

    sigaltstack(&alt, NULL);
    action.sa_sigaction = OnCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    if (getenv("TMC_WATCHDOG") != NULL) {
        signal(SIGALRM, OnWatchdog);
        alarm(atoi(getenv("TMC_WATCHDOG")));
    }
}

_Noreturn void PlatformSoftReset(void) {
    longjmp(sResetPoint, 1);
}

int main(void) {
    const char* frames = getenv("TMC_FRAMES");
    sFrameLimit = frames != NULL ? strtoull(frames, NULL, 10) : 0;
    InstallDiagnostics();
    if (getenv("TMC_INPUT") != NULL) {
        ParseInput(getenv("TMC_INPUT"));
    }
    sTraceTask = getenv("TMC_TRACE_TASK") != NULL;

    REG_KEYINPUT = 0x03FF; // all buttons released (active low)
    setjmp(sResetPoint);
    AgbMain();
    return 0;
}
