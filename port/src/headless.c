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
#include <zlib.h>

#include "port/headless_state.h"
#include "port/hw.h"
#include "port/platform.h"
#include "port/ppu.h"
#include "port/romdata.h"
#include "port/sound.h"

void TraceGameTask(uint64_t frame); // trace.c (kept apart: game headers clash with libc)
void TraceEntities(uint64_t frame);
void DumpRoomMap(const char* path); // mapdump.c
void TracePlayer(uint64_t frame);

void AgbMain(void);

uint64_t gPlatformFrame;
static const char* sMapDir; // TMC_MAPDUMP
static uint64_t sFrameLimit;
static jmp_buf sResetPoint;

// TMC_INPUT="step,step,...": scripted buttons (a b select start right left up
// down r l, joined with +). TMC_INPUT=@path reads the steps from a file,
// separated by commas or whitespace; '#' starts a comment. Steps run in order:
//
//   F:keys:D          hold keys for D frames (default 1) from frame F
//   +N:keys:D         the same, N frames after the previous step started or
//                     the previous wait or walk finished
//   wait:COND[:T[:S]] wait until COND has held for S frames (default 20); fails
//                     after T frames (default 3000)
//   mash:COND[:T[:S]] the same, tapping A meanwhile (to get through dialogue)
//   gox:X  goy:Y      walk until the player's room-relative x (y) is X (Y),
//                     tapping A through any message on the way; fails when
//                     the player stops moving short of it
//
// COND is control (gameplay running, player free to move, no message or fade),
// room=AA.RR (hex area and room, with control), in=AA.RR (the same without
// control), msg (a message is shown) or nomsg. A failed step prints why and exits with status 3.
typedef enum { STEP_KEYS, STEP_WAIT, STEP_MASH, STEP_GOX, STEP_GOY } StepKind;
typedef enum { COND_CONTROL, COND_ROOM, COND_IN, COND_MSG, COND_NOMSG } StepCond;
typedef struct {
    StepKind kind;
    int relative;
    uint64_t frame, duration;
    uint16_t keys;
    StepCond cond;
    int area, room, target, stable;
    char text[48];
} ScriptStep;
#define MAX_STEPS 8192
static ScriptStep sSteps[MAX_STEPS];
static int sStepCount;

static char* ReadInputFile(const char* path) {
    FILE* file = fopen(path, "r");
    size_t size = 0, capacity = 4096;
    char* text = malloc(capacity);
    int c, comment = 0;

    if (file == NULL) {
        fprintf(stderr, "TMC_INPUT: cannot open %s\n", path);
        exit(1);
    }
    while ((c = fgetc(file)) != EOF) {
        if (c == '#') {
            comment = 1;
        } else if (c == '\n') {
            comment = 0;
        }
        if (comment || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            c = ',';
        }
        if (size + 2 > capacity) {
            text = realloc(text, capacity *= 2);
        }
        text[size++] = (char)c;
    }
    text[size] = 0;
    fclose(file);
    return text;
}

static uint16_t ParseKeys(const char* item, char* keys) {
    static const char* const names[] = { "a", "b", "select", "start", "right", "left", "up", "down", "r", "l" };
    uint16_t mask = 0;
    char* rest;

    for (char* name = strtok_r(keys, "+", &rest); name != NULL; name = strtok_r(NULL, "+", &rest)) {
        int found = 0;
        for (int i = 0; i < 10; i++) {
            if (strcmp(name, names[i]) == 0) {
                mask |= 1 << i;
                found = 1;
            }
        }
        if (!found) {
            fprintf(stderr, "TMC_INPUT: unknown button '%s' in '%s'\n", name, item);
            exit(1);
        }
    }
    return mask;
}

static void ParseStep(const char* item) {
    ScriptStep* step = &sSteps[sStepCount];
    char word[16] = { 0 }, arg[64] = { 0 };
    unsigned long long timeout = 3000;
    int stable = 20;

    if (sStepCount == MAX_STEPS) {
        fprintf(stderr, "TMC_INPUT: more than %d steps\n", MAX_STEPS);
        exit(1);
    }
    memset(step, 0, sizeof(*step));
    snprintf(step->text, sizeof(step->text), "%s", item);
    if (sscanf(item, "%15[a-z]:%63[^:]:%llu:%d", word, arg, &timeout, &stable) >= 2) {
        step->stable = stable;
        if (strcmp(word, "wait") == 0 || strcmp(word, "mash") == 0) {
            step->kind = word[0] == 'w' ? STEP_WAIT : STEP_MASH;
            step->duration = timeout;
            if (strcmp(arg, "control") == 0) {
                step->cond = COND_CONTROL;
            } else if (strcmp(arg, "msg") == 0) {
                step->cond = COND_MSG;
            } else if (strcmp(arg, "nomsg") == 0) {
                step->cond = COND_NOMSG;
            } else if (sscanf(arg, "room=%x.%x", &step->area, &step->room) == 2) {
                step->cond = COND_ROOM;
            } else if (sscanf(arg, "in=%x.%x", &step->area, &step->room) == 2) {
                step->cond = COND_IN;
            } else {
                fprintf(stderr, "TMC_INPUT: unknown condition in '%s'\n", item);
                exit(1);
            }
        } else if (strcmp(word, "gox") == 0 || strcmp(word, "goy") == 0) {
            step->kind = word[2] == 'x' ? STEP_GOX : STEP_GOY;
            step->target = atoi(arg);
        } else {
            fprintf(stderr, "TMC_INPUT: unknown step '%s'\n", item);
            exit(1);
        }
    } else {
        unsigned long long frame = 0, duration = 1;
        char keys[128] = { 0 };

        step->relative = item[0] == '+';
        if (sscanf(item + step->relative, "%llu:%127[^:]:%llu", &frame, keys, &duration) < 2) {
            fprintf(stderr, "TMC_INPUT: cannot parse '%s'\n", item);
            exit(1);
        }
        step->kind = STEP_KEYS;
        step->frame = frame;
        step->duration = duration;
        step->keys = ParseKeys(item, keys);
    }
    sStepCount++;
}

static void ParseInput(const char* text) {
    char* buffer = text[0] == '@' ? ReadInputFile(text + 1) : strdup(text);
    char* rest;

    for (char* item = strtok_r(buffer, ",", &rest); item != NULL; item = strtok_r(NULL, ",", &rest)) {
        ParseStep(item);
    }
    free(buffer);
}

// Key windows that have started; the steps run one after another.
typedef struct {
    uint64_t start, end;
    uint16_t keys;
} KeyWindow;
static KeyWindow sWindows[64];
static int sStep;
static uint64_t sBase;     // start of the previous step, or end of the previous wait/walk
static uint64_t sStepSince; // when the current wait/walk began
static int sLastPos, sStillFrames, sCondFrames;

static void StepFailed(const ScriptStep* step, const char* why) {
    HeadlessState s;
    HeadlessGetState(&s);
    fprintf(stderr,
            "TMC_INPUT: step %d '%s' %s at frame %llu (task %d.%d.%d area %02x room %02x pos %d,%d control %d)\n",
            sStep, step->text, why, (unsigned long long)gPlatformFrame, s.task, s.state, s.substate, s.area, s.room,
            s.x, s.y, s.control);
    if (sMapDir != NULL) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/map_failed.png", sMapDir);
        DumpRoomMap(path);
    }
    printf("headless: input script failed\n");
    exit(3);
}

static int CondHolds(const ScriptStep* step, const HeadlessState* s) {
    switch (step->cond) {
        case COND_CONTROL:
            return s->control;
        case COND_ROOM:
            return s->control && s->area == step->area && s->room == step->room;
        case COND_IN:
            return s->area == step->area && s->room == step->room;
        case COND_MSG:
            return s->message;
        case COND_NOMSG:
            return !s->message;
    }
    return 0;
}

static void AddWindow(uint64_t start, uint64_t duration, uint16_t keys) {
    for (int i = 0; i < 64; i++) {
        if (sWindows[i].end <= gPlatformFrame) {
            sWindows[i] = (KeyWindow){ start, start + duration, keys };
            return;
        }
    }
}

static uint16_t ScriptedKeys(void) {
    uint16_t held = 0;
    HeadlessState s;

    HeadlessGetState(&s);
    while (sStep < sStepCount) {
        const ScriptStep* step = &sSteps[sStep];
        if (step->kind == STEP_KEYS) {
            uint64_t start = step->relative ? sBase + step->frame : step->frame;
            if (start > gPlatformFrame) {
                break;
            }
            AddWindow(start, step->duration, step->keys);
            sBase = start;
            sStep++;
            continue;
        }
        if (sStepSince == 0) {
            sStepSince = gPlatformFrame;
            sLastPos = -100000;
            sStillFrames = 0;
            sCondFrames = 0;
        }
        if (step->kind == STEP_WAIT || step->kind == STEP_MASH) {
            sCondFrames = CondHolds(step, &s) ? sCondFrames + 1 : 0;
            if (sCondFrames >= step->stable) {
                printf("frame %llu: step %d '%s' done\n", (unsigned long long)gPlatformFrame, sStep, step->text);
            } else if (gPlatformFrame - sStepSince >= step->duration) {
                StepFailed(step, "timed out");
            } else {
                if (step->kind == STEP_MASH && (gPlatformFrame - sStepSince) % 8 < 2) {
                    held |= 1; // A
                }
                break;
            }
        } else {
            int pos = step->kind == STEP_GOX ? s.x : s.y;
            int delta = step->target - pos;
            if (delta >= -1 && delta <= 1 && s.control) {
                printf("frame %llu: step %d '%s' done\n", (unsigned long long)gPlatformFrame, sStep, step->text);
            } else {
                if (s.control) {
                    sStillFrames = pos == sLastPos ? sStillFrames + 1 : 0;
                    if (sStillFrames > 60) {
                        StepFailed(step, "is stuck");
                    }
                    held |= step->kind == STEP_GOX ? (delta > 0 ? 0x10 : 0x20) : (delta > 0 ? 0x80 : 0x40);
                } else if (gPlatformFrame - sStepSince > 3000) {
                    StepFailed(step, "lost control");
                } else if (s.message && (gPlatformFrame - sStepSince) % 8 < 2) {
                    held |= 1; // A: get through dialogue on the way
                }
                sLastPos = pos;
                break;
            }
        }
        sBase = gPlatformFrame;
        sStepSince = 0;
        sStep++;
    }
    for (int i = 0; i < 64; i++) {
        if (gPlatformFrame >= sWindows[i].start && gPlatformFrame < sWindows[i].end) {
            held |= sWindows[i].keys;
        }
    }
    return held;
}

static int sTraceTask;
static int sTraceEntities;

// TMC_MAPDUMP=dir: the first time the player has had control in a room for 30
// frames, save the whole room as dir/map_AA_RR.png (see mapdump.c).
static void MaybeDumpMap(void) {
    static uint8_t sDumped[256][64];
    static int sStable;
    HeadlessState s;
    char path[1024];

    HeadlessGetState(&s);
    sStable = s.control ? sStable + 1 : 0;
    if (sStable < 30 || s.area > 255 || s.room > 63 || sDumped[s.area][s.room]) {
        return;
    }
    sDumped[s.area][s.room] = 1;
    snprintf(path, sizeof(path), "%s/map_%02x_%02x.png", sMapDir, s.area, s.room);
    DumpRoomMap(path);
}
static uint64_t sTracePlayer;
static FILE* sHashFile; // TMC_FRAME_HASH

// TMC_FRAME_HASH=path writes "frame crc32" for every frame's picture, to compare
// renderer changes numerically between runs.
static void MaybeHashFrame(void) {
    static uint16_t sFrame[PPU_WIDTH * PPU_HEIGHT];

    if (sHashFile == NULL) {
        return;
    }
    PpuRenderFrame(sFrame);
    fprintf(sHashFile, "%llu %08lx\n", (unsigned long long)gPlatformFrame,
            crc32(0, (const Bytef*)sFrame, sizeof(sFrame)));
}

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
    if (sTracePlayer != 0 && gPlatformFrame % sTracePlayer == 0) {
        TracePlayer(gPlatformFrame);
    }
    if (sMapDir != NULL) {
        MaybeDumpMap();
    }
    // The picture shown during this frame: registers as the last VBlank left them.
    MaybeScreenshot();
    MaybeHashFrame();

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
    setvbuf(stdout, NULL, _IOLBF, 0);
    InstallDiagnostics();
    RomDataPrepareOrExit("tmc");
    if (getenv("TMC_INPUT") != NULL) {
        ParseInput(getenv("TMC_INPUT"));
    }
    sTraceTask = getenv("TMC_TRACE_TASK") != NULL;
    sTraceEntities = getenv("TMC_TRACE_ENTITIES") != NULL;
    if (getenv("TMC_TRACE_PLAYER") != NULL) {
        sTracePlayer = strtoull(getenv("TMC_TRACE_PLAYER"), NULL, 10);
        sTracePlayer = sTracePlayer != 0 ? sTracePlayer : 60;
    }
    if ((sMapDir = getenv("TMC_MAPDUMP")) != NULL) {
        mkdir(sMapDir, 0755);
    }
    if (getenv("TMC_SCREENSHOT") != NULL) {
        ParseScreenshots(getenv("TMC_SCREENSHOT"));
        if (getenv("TMC_SCREENSHOT_DIR") != NULL) {
            sShotDir = getenv("TMC_SCREENSHOT_DIR");
        }
        mkdir(sShotDir, 0755);
    }
    if (getenv("TMC_FRAME_HASH") != NULL && (sHashFile = fopen(getenv("TMC_FRAME_HASH"), "w")) == NULL) {
        fprintf(stderr, "TMC_FRAME_HASH: cannot write %s\n", getenv("TMC_FRAME_HASH"));
        exit(1);
    }
    SoundWavFromEnv();

    HwSetKeys(0);
    setjmp(sResetPoint);
    AgbMain();
    return 0;
}
