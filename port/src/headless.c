/*
 * Headless platform: runs game logic with no window or sound, as fast as it can.
 * Used for automated runs (TMC_FRAMES=n stops after n frames, TMC_INPUT scripts
 * buttons, TMC_SCREENSHOT saves frames); the app is in port/src/macos.
 */

#include <execinfo.h>
#include <mach-o/dyld.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "port/hw.h"
#include "port/platform.h"
#include "port/ppu.h"

void TraceGameTask(uint64_t frame); // trace.c (kept apart: game headers clash with libc)
void TraceEntities(uint64_t frame);

void AgbMain(void);

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
static int sTraceEntities;

// TMC_SCREENSHOT="frame,frame,..." or "every:n" saves PNGs to TMC_SCREENSHOT_DIR
// (default: screenshots).
static uint64_t sShotFrames[256];
static int sShotCount;
static uint64_t sShotEvery;
static const char* sShotDir = "screenshots";

static void ParseScreenshots(const char* text) {
    if (strncmp(text, "every:", 6) == 0) {
        sShotEvery = strtoull(text + 6, NULL, 10);
        return;
    }
    while (*text && sShotCount < 256) {
        char* end;
        sShotFrames[sShotCount++] = strtoull(text, &end, 10);
        text = *end == ',' ? end + 1 : end;
        if (end == text && *text) {
            break;
        }
    }
}

static void MaybeScreenshot(void) {
    static uint16_t sFrame[PPU_WIDTH * PPU_HEIGHT];
    int take = sShotEvery != 0 && gPlatformFrame % sShotEvery == 0;
    char path[1024];

    for (int i = 0; i < sShotCount && !take; i++) {
        take = sShotFrames[i] == gPlatformFrame;
    }
    if (!take) {
        return;
    }
    if (getenv("TMC_DUMP_IO") != NULL) {
        fprintf(stderr, "frame %llu io:", (unsigned long long)gPlatformFrame);
        for (int i = 0; i < 0x56; i += 2) {
            fprintf(stderr, " %02x=%04x", i, gHwIo[i] | (gHwIo[i + 1] << 8));
        }
        fprintf(stderr, " IE=%04x IF=%04x IME=%04x vector=%p", gHwIo[0x200] | (gHwIo[0x201] << 8),
                gHwIo[0x202] | (gHwIo[0x203] << 8), gHwIo[0x208] | (gHwIo[0x209] << 8), gHwIntrVector);
        {
            int shown = 0;
            for (int i = 0; i < 128; i++) {
                uint16_t attr0 = gHwOam[i * 8] | (gHwOam[i * 8 + 1] << 8);
                if ((attr0 & 0x300) != 0x200) {
                    shown++;
                }
            }
            fprintf(stderr, " visible OBJs=%d\n", shown);
            for (int i = 0; i < 12; i++) {
                const uint8_t* o = &gHwOam[i * 8];
                fprintf(stderr, "  obj%-3d %04x %04x %04x\n", i, o[0] | (o[1] << 8), o[2] | (o[3] << 8), o[4] | (o[5] << 8));
            }
        }
        fprintf(stderr, "\n");
    }
    PpuRenderFrame(sFrame);
    snprintf(path, sizeof(path), "%s/frame_%06llu.png", sShotDir, (unsigned long long)gPlatformFrame);
    if (PpuWritePng(sFrame, path) != 0) {
        fprintf(stderr, "screenshot: cannot write %s\n", path);
    }
}

void PlatformVBlank(void) {
    HwSetKeys(ScriptedKeys());
    if (sTraceTask) {
        TraceGameTask(gPlatformFrame);
    }
    if (sTraceEntities && gPlatformFrame % 100 == 0) {
        TraceEntities(gPlatformFrame);
    }
    // The picture shown during this frame: registers as the last VBlank left them.
    MaybeScreenshot();

    HwFrameInterrupts();

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
    sTraceEntities = getenv("TMC_TRACE_ENTITIES") != NULL;
    if (getenv("TMC_SCREENSHOT") != NULL) {
        ParseScreenshots(getenv("TMC_SCREENSHOT"));
        if (getenv("TMC_SCREENSHOT_DIR") != NULL) {
            sShotDir = getenv("TMC_SCREENSHOT_DIR");
        }
        mkdir(sShotDir, 0755);
    }

    HwSetKeys(0);
    setjmp(sResetPoint);
    AgbMain();
    return 0;
}
