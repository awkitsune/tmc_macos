/*
 * Headless platform: runs game logic with no window or sound, as fast as it can.
 * Used for automated runs (TMC_FRAMES=n stops after n frames, TMC_INPUT scripts
 * buttons, TMC_SCREENSHOT saves frames); the app is in port/src/macos.
 *
 * Save states (port/include/port/state.h):
 *   TMC_LOAD_STATE=path      start from a state (at the first frame boundary)
 *   TMC_SAVE_STATES=F,F,...  save at these frames, or every:K, into TMC_STATE_DIR
 *                            (default states) as frame_NNNNNN.tmcs
 *   TMC_HASH=F,F,... | every:K   print "hash F state S video V": checksums of the
 *                            state and of the picture, to compare runs
 *   TMC_FRAMES=+n            stop n frames after the start (or the loaded state)
 *   TMC_RECORD_INPUT=path    write the keys each frame held as a TMC_INPUT
 *                            script (absolute frames), to replay a run exactly
 *   TMC_ROM_WRITES=1         log stores into the game's read-only data (ROM on
 *                            the GBA) as "rom write: symbol from function" and
 *                            skip them, instead of crashing with SIGBUS
 *   TMC_WARP=AA:RR[:X:Y]     once the player has control, go to that room (hex
 *                            area and room; X,Y room-relative) through the game's
 *                            own exit transition, see port/src/warp.c; prints
 *                            "warp: end ..." with the room it is in when the run
 *                            ends. TMC_WARP_ITEMS=A,B also equips those items
 *                            (ids from include/item.h) when the warp starts
 *   TMC_MAPVIEW=K            every K frames, the macOS Map window's view of the
 *                            room as its summary line (port/src/mapview.c);
 *                            TMC_MAPVIEW_REQUEST=F:move:X,Y or F:warp:AA:RR makes
 *                            one of the window's requests from frame F on
 *
 * Graphics dumps (port/gfxdebug.h), each a frame list (F,F,... or every:K), of
 * the hardware as that frame was drawn:
 *   TMC_DUMP_OAM             decoded sprite table (TMC_DUMP_OAM_ALL=1: hidden too)
 *   TMC_DUMP_BG              layer settings and map summaries
 *   TMC_DUMP_PAL             the 512 palette colours
 *   TMC_DUMP_REGS            decoded display registers and the per-line changes
 *   TMC_DUMP_FILE=path       write them there instead of stdout
 *   TMC_DUMP_PNG=dir         with TMC_DUMP_BG, also each layer's whole map and all
 *                            VRAM tiles (greyscale) as PNGs
 *   TMC_LAYER_MASK=hex       draw only these layers (bits 0-3 BG0-3, 4 OBJ; 1f = all)
 *
 * Area maps (port/arearender.h), drawn from the game's data without the game:
 *   TMC_AREADUMP=dir         every area and dungeon floor as a PNG in dir, once:
 *                            at TMC_AREADUMP_FRAME, or when the player first
 *                            has had control for 30 frames; checks that the
 *                            game's state (the whole save state segment: RAM,
 *                            VRAM, palettes, registers) is byte for byte the
 *                            same afterwards; TMC_AREADUMP_ROOMS=1 also lists
 *                            each room's place and name
 *   TMC_AREACHECK=1          for each room the player has had control in for 30
 *                            frames, compare the renderer's picture of it with
 *                            the live tile layers, and say what differs
 * States are taken at the main loop's frame boundary: a frame that falls in one
 * of the game's pause loops saves at the next boundary instead.
 */

#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <zlib.h>

#include "port/arearender.h"
#include "port/gfxdebug.h"
#include "port/mapview.h"
#include "port/headless_state.h"
#include "port/hw.h"
#include "port/platform.h"
#include "port/ppu.h"
#include "port/romdata.h"
#include "port/sound.h"
#include "port/state.h"

void TraceGameTask(uint64_t frame); // trace.c (kept apart: game headers clash with libc)
void TraceEntities(uint64_t frame);
void DumpRoomMap(const char* path); // mapdump.c
void CheckEntityLists(const char* path); // entitylistcheck.c
void TracePlayer(uint64_t frame);
int WarpRequest(int area, int room, int x, int y);
void WarpSetItems(int a, int b);
int WarpUpdate(unsigned long long frame);
void WarpReport(unsigned long long frame);

void AgbMain(void);

uint64_t gPlatformFrame;
static const char* sMapDir; // TMC_MAPDUMP
static uint64_t sFrameLimit;
static int sFrameLimitRelative; // TMC_FRAMES=+n
static jmp_buf sResetPoint;
static int sWarping;          // TMC_WARP
static int sLastArea = -1, sLastRoom = -1; // for crash reports

static void ReportWarp(void) {
    WarpReport(gPlatformFrame);
}

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
//
//   save:NAME         save a state as TMC_STATE_DIR/NAME.tmcs at the next frame
//                     boundary (only when TMC_STATE_DIR is set; the step takes
//                     the same frames either way, so runs stay alike).
//
// TMC_INPUT_FROM=NAME starts the script after its save:NAME step, to carry on
// from that state (TMC_LOAD_STATE=.../NAME.tmcs) exactly as the full run did.
typedef enum { STEP_KEYS, STEP_WAIT, STEP_MASH, STEP_GOX, STEP_GOY, STEP_SAVE } StepKind;
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
static ScriptStep* sSteps;
static const char* const sKeyNames[] = { "a", "b", "select", "start", "right", "left", "up", "down", "r", "l" };
static int sStepCount, sStepCapacity;

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
    uint16_t mask = 0;
    char* rest;

    for (char* name = strtok_r(keys, "+", &rest); name != NULL; name = strtok_r(NULL, "+", &rest)) {
        int found = 0;
        for (int i = 0; i < 10; i++) {
            if (strcmp(name, sKeyNames[i]) == 0) {
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
    ScriptStep* step;
    char word[16] = { 0 }, arg[64] = { 0 };
    unsigned long long timeout = 3000;
    int stable = 20;

    if (sStepCount == sStepCapacity) {
        sStepCapacity = sStepCapacity != 0 ? sStepCapacity * 2 : 1024;
        sSteps = realloc(sSteps, sizeof(ScriptStep) * sStepCapacity);
    }
    step = &sSteps[sStepCount];
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
        } else if (strcmp(word, "save") == 0) {
            step->kind = STEP_SAVE;
            snprintf(step->text, sizeof(step->text), "%s", arg);
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
static const char* sStateDir; // TMC_STATE_DIR
static int sScriptSaveDone;   // the current save: step's state was written at this boundary

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
        if (step->kind == STEP_SAVE) {
            if (!sScriptSaveDone) {
                break; // saved at the start of the next loop boundary (HeadlessStates)
            }
            sScriptSaveDone = 0;
        } else if (step->kind == STEP_WAIT || step->kind == STEP_MASH) {
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

// TMC_MAPVIEW=K: every K frames, take the macOS Map window's view of the room (mapview.c) and
// print its summary line. TMC_MAPVIEW_REQUEST=F:move:X,Y (room pixels) or F:warp:AA:RR (hex): from
// frame F on, make one of the window's requests, carried out as the app does (at a loop boundary;
// a warp then goes on every frame), printing what became of it.
static uint64_t sMapViewEvery;
static const char* sMapViewRequest;

static void MapViewFrame(void) {
    static MapViewState sState;
    static MapViewImage sImage;
    static int sWarping;
    char message[160] = "";
    unsigned long long from;
    int a, b;

    if (sMapViewRequest != NULL && PlatformAtLoopBoundary() && sscanf(sMapViewRequest, "%llu:", &from) == 1 &&
        gPlatformFrame >= from) {
        const char* what = strchr(sMapViewRequest, ':') + 1;
        if (sscanf(what, "move:%d,%d", &a, &b) == 2) {
            MapViewMovePlayer(a, b, message, sizeof(message));
        } else if (sscanf(what, "warp:%x:%x", &a, &b) == 2) {
            sWarping = MapViewWarp(a, b, message, sizeof(message));
        } else {
            snprintf(message, sizeof(message), "TMC_MAPVIEW_REQUEST: cannot read '%s'", sMapViewRequest);
        }
        sMapViewRequest = NULL;
    } else if (sWarping && PlatformAtLoopBoundary()) {
        sWarping = MapViewUpdate(gPlatformFrame, message, sizeof(message));
    }
    if (message[0] != 0) {
        printf("frame %llu: mapview: %s\n", (unsigned long long)gPlatformFrame, message);
    }
    if (sMapViewEvery != 0) {
        MapViewCapture(&sState, &sImage, gPlatformFrame, 0, 0);
        if (gPlatformFrame % sMapViewEvery == 0) {
            MapViewSummary(&sState, &sImage, message, sizeof(message));
            printf("%s\n", message);
        }
    }
}

static const char* sAreaDir; // TMC_AREADUMP
static uint64_t sAreaFrame;  // TMC_AREADUMP_FRAME
static int sAreaCheck;       // TMC_AREACHECK

// TMC_AREADUMP: draw every area, with the save state segment (everything the game
// can change) copied before and compared after, to show the renderer leaves it alone.
static void MaybeDumpAreas(void) {
    static int sStable, sDone;
    HeadlessState s;
    unsigned long size;
    uint8_t* segment = getsegmentdata(&_mh_execute_header, "__TMCSTATE", &size);
    uint8_t* copy;
    uint64_t before, after;
    size_t differ = 0, first = 0;

    HeadlessGetState(&s);
    sStable = s.control ? sStable + 1 : 0;
    if (sDone || (sAreaFrame != 0 ? gPlatformFrame != sAreaFrame : sStable < 30)) {
        return;
    }
    sDone = 1;
    copy = malloc(size);
    memcpy(copy, segment, size);
    before = StateChecksum();
    printf("areadump: frame %llu area %02x room %02x, state segment %lu bytes, checksum %016llx\n",
           (unsigned long long)gPlatformFrame, s.area, s.room, size, (unsigned long long)before);
    AreaMapDumpAll(sAreaDir, getenv("TMC_AREADUMP_ROOMS") != NULL, stdout);
    after = StateChecksum();
    for (size_t i = 0; i < size; i++) {
        if (copy[i] != segment[i] && differ++ == 0) {
            first = i;
        }
    }
    printf("areadump: state segment after: checksum %016llx, %zu bytes differ%s", (unsigned long long)after, differ,
           differ == 0 && before == after ? " (unchanged)\n" : "");
    if (differ != 0 || before != after) {
        printf(" (CHANGED, first at offset 0x%zx)\n", first);
    }
    free(copy);
}

// TMC_AREACHECK: once per room, after 30 frames of control.
static void MaybeCheckArea(void) {
    static uint8_t sChecked[256][64];
    static int sStable;
    HeadlessState s;

    HeadlessGetState(&s);
    sStable = s.control ? sStable + 1 : 0;
    if (sStable < 30 || s.area > 255 || s.room > 63 || sChecked[s.area][s.room]) {
        return;
    }
    sChecked[s.area][s.room] = 1;
    printf("frame %llu: ", (unsigned long long)gPlatformFrame);
    AreaMapCheckLive(stdout);
}

static uint64_t sTracePlayer;
static FILE* sHashFile; // TMC_FRAME_HASH

// TMC_FRAME_HASH=path writes "frame crc32" for every frame's picture, to compare
// renderer changes numerically between runs.
static void HashFrame(const uint16_t* frame) {
    fprintf(sHashFile, "%llu %08lx\n", (unsigned long long)gPlatformFrame,
            crc32(0, (const Bytef*)frame, PPU_WIDTH * PPU_HEIGHT * sizeof(*frame)));
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

static int ScreenshotThisFrame(void) {
    int take = sShotEvery != 0 && gPlatformFrame % sShotEvery == 0;

    for (int i = 0; i < sShotCount && !take; i++) {
        take = sShotFrames[i] == gPlatformFrame;
    }
    return take;
}

static void DumpIo(void) {
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
}

static void Screenshot(const uint16_t* frame) {
    char path[1024];

    snprintf(path, sizeof(path), "%s/frame_%06llu.png", sShotDir, (unsigned long long)gPlatformFrame);
    if (PpuWritePng(frame, path) != 0) {
        fprintf(stderr, "screenshot: cannot write %s\n", path);
    }
}

// Frame lists: "F,F,..." (up to 256) or "every:K".
typedef struct {
    uint64_t frames[256];
    int count;
    uint64_t every;
} FrameList;

static void ParseFrameList(const char* text, FrameList* list) {
    if (strncmp(text, "every:", 6) == 0) {
        list->every = strtoull(text + 6, NULL, 10);
        return;
    }
    while (*text && list->count < 256) {
        char* end;
        list->frames[list->count++] = strtoull(text, &end, 10);
        if (end == text) {
            break;
        }
        text = *end == ',' ? end + 1 : end;
    }
}

static int InFrameList(const FrameList* list, uint64_t frame) {
    if (list->every != 0 && frame % list->every == 0) {
        return 1;
    }
    for (int i = 0; i < list->count; i++) {
        if (list->frames[i] == frame) {
            return 1;
        }
    }
    return 0;
}

// TMC_DUMP_*: graphics dumps.
static FrameList sDumpOam, sDumpBg, sDumpPal, sDumpRegs;
static int sDumping, sDumpOamAll;
static FILE* sDumpOut;
static const char* sDumpPngDir;

static int DumpThisFrame(void) {
    return sDumping && (InFrameList(&sDumpOam, gPlatformFrame) || InFrameList(&sDumpBg, gPlatformFrame) ||
                        InFrameList(&sDumpPal, gPlatformFrame) || InFrameList(&sDumpRegs, gPlatformFrame));
}

static void DumpPng(const uint32_t* rgb, int width, int height, const char* name) {
    static uint16_t sPixels[1024 * 1024];
    char path[1024];

    for (int i = 0; i < width * height; i++) {
        uint32_t c = rgb[i];
        sPixels[i] = ((c >> 19) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 3) & 0x1F) << 10);
    }
    snprintf(path, sizeof(path), "%s/frame_%06llu_%s.png", sDumpPngDir, (unsigned long long)gPlatformFrame, name);
    if (PpuWritePngSize(sPixels, width, height, path) != 0) {
        fprintf(stderr, "TMC_DUMP_PNG: cannot write %s\n", path);
    }
}

static void WriteDumps(const GfxSnapshot* snap) {
    uint64_t f = gPlatformFrame;

    if (InFrameList(&sDumpRegs, f)) {
        GfxDumpIo(snap, sDumpOut);
        GfxDumpLines(snap, sDumpOut);
    }
    if (InFrameList(&sDumpBg, f)) {
        GfxDumpBg(snap, sDumpOut);
        if (sDumpPngDir != NULL) {
            static uint32_t sImage[1024 * 1024];
            char name[16];
            for (int bg = 0; bg < 4; bg++) {
                GfxBg info;
                GfxDecodeBg(snap, bg, &info);
                if (GfxRenderBg(snap, bg, sImage, 0)) {
                    snprintf(name, sizeof(name), "bg%d", bg);
                    DumpPng(sImage, info.width, info.height, name);
                }
            }
            GfxRenderTiles(snap, 0, HW_VRAM_SIZE / 32, 0, -1, sImage);
            DumpPng(sImage, 256, HW_VRAM_SIZE / 32 / 32 * 8, "vram");
        }
    }
    if (InFrameList(&sDumpOam, f)) {
        GfxDumpOam(snap, sDumpOut, sDumpOamAll);
    }
    if (InFrameList(&sDumpPal, f)) {
        GfxDumpPal(snap, sDumpOut);
    }
    fflush(sDumpOut);
}

static const char* sLoadState; // TMC_LOAD_STATE, until loaded
static FrameList sSaveFrames;  // TMC_SAVE_STATES
static uint64_t sSaveDue = UINT64_MAX; // a listed frame that fell in a pause loop
static FrameList sHashFrames; // TMC_HASH
static int sHashing;

static void SaveStateAs(const char* name) {
    char path[1024], message[256];

    mkdir(sStateDir, 0755);
    snprintf(path, sizeof(path), "%s/%s.%s", sStateDir, name, STATE_EXTENSION);
    if (!StateSave(path, gPlatformFrame, message, sizeof(message))) {
        fprintf(stderr, "save state: %s\n", message);
        exit(1);
    }
    printf("frame %llu: saved state %s (checksum %016llx)\n", (unsigned long long)gPlatformFrame, path,
           (unsigned long long)StateChecksum());
}

// Loads, saves and checksums, at the start of a frame (before its input).
static void HeadlessStates(void) {
    int boundary = PlatformAtLoopBoundary();

    if (sLoadState != NULL && boundary) {
        char message[256];
        uint64_t frame;

        if (!StateLoad(sLoadState, &frame, message, sizeof(message))) {
            fprintf(stderr, "TMC_LOAD_STATE: %s\n", message);
            exit(1);
        }
        if (message[0] != 0) {
            fprintf(stderr, "TMC_LOAD_STATE: %s\n", message);
        }
        printf("frame %llu: loaded state %s (checksum %016llx)\n", (unsigned long long)frame, sLoadState,
               (unsigned long long)StateChecksum());
        sLoadState = NULL;
        gPlatformFrame = frame;
        if (sFrameLimitRelative) {
            sFrameLimit += frame;
            sFrameLimitRelative = 0;
        }
        // The input script starts here: relative steps count from this frame.
        sBase = gPlatformFrame;
        memset(sWindows, 0, sizeof(sWindows));
    }
    if (sHashing && InFrameList(&sHashFrames, gPlatformFrame)) {
        static uint16_t sPicture[PPU_WIDTH * PPU_HEIGHT];
        PpuRenderFrame(sPicture);
        printf("hash %llu state %016llx video %016llx%s\n", (unsigned long long)gPlatformFrame,
               (unsigned long long)StateChecksum(), (unsigned long long)StateHashBytes(sPicture, sizeof(sPicture)),
               boundary ? "" : " (pause loop)");
    }
    if (InFrameList(&sSaveFrames, gPlatformFrame) && sSaveDue == UINT64_MAX) {
        sSaveDue = gPlatformFrame;
    }
    if (!boundary) {
        return;
    }
    if (sSaveDue != UINT64_MAX) {
        char name[32];
        snprintf(name, sizeof(name), "frame_%06llu", (unsigned long long)gPlatformFrame);
        SaveStateAs(name);
        sSaveDue = UINT64_MAX;
    }
    if (sStep < sStepCount && sSteps[sStep].kind == STEP_SAVE && !sScriptSaveDone) {
        for (int i = 0; i < 64; i++) {
            if (sWindows[i].end > gPlatformFrame) {
                fprintf(stderr, "TMC_INPUT: keys still held at save:%s; a state does not carry them\n",
                        sSteps[sStep].text);
                break;
            }
        }
        if (sStateDir != NULL) {
            SaveStateAs(sSteps[sStep].text);
        }
        sScriptSaveDone = 1;
    }
}

// TMC_RECORD_INPUT: one "F:keys:D" step per run of the same keys.
static FILE* sRecord;
static uint64_t sRecordStart;
static uint16_t sRecordKeys;

static void RecordRun(void) {
    if (sRecordKeys == 0) {
        return;
    }
    fprintf(sRecord, "%llu:", (unsigned long long)sRecordStart);
    for (int i = 0, first = 1; i < 10; i++) {
        if (sRecordKeys & (1 << i)) {
            fprintf(sRecord, "%s%s", first ? "" : "+", sKeyNames[i]);
            first = 0;
        }
    }
    fprintf(sRecord, ":%llu\n", (unsigned long long)(gPlatformFrame - sRecordStart));
}

static void RecordKeys(uint16_t keys) {
    if (keys != sRecordKeys) {
        RecordRun();
        sRecordStart = gPlatformFrame;
        sRecordKeys = keys;
    }
}

static void FinishRecording(void) {
    RecordRun();
    fclose(sRecord);
}

void PlatformVBlank(void) {
    uint16_t keys;

    HeadlessStates();
    if (sWarping && sLoadState == NULL) {
        HeadlessState s;
        WarpUpdate(gPlatformFrame);
        HeadlessGetState(&s);
        sLastArea = s.area;
        sLastRoom = s.room;
    }
    keys = ScriptedKeys();
    if (sRecord != NULL) {
        RecordKeys(keys);
    }
    HwSetKeys(keys);
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
    if (sMapViewEvery != 0 || sMapViewRequest != NULL) {
        MapViewFrame();
    }
    if (sAreaDir != NULL) {
        MaybeDumpAreas();
    }
    if (sAreaCheck) {
        MaybeCheckArea();
    }
    // The picture shown during this frame, drawn only when something needs it.
    {
        static uint16_t sFrame[PPU_WIDTH * PPU_HEIGHT];
        int shot = ScreenshotThisFrame();

        if (shot) {
            DumpIo();
        }
        static GfxSnapshot sSnap;
        int dump = DumpThisFrame();

        if (dump) {
            GfxSnapshotBegin(&sSnap, gPlatformFrame);
        }
        HwRunFrame(shot || sHashFile != NULL ? sFrame : NULL);
        if (dump) {
            GfxSnapshotEnd(&sSnap);
            WriteDumps(&sSnap);
        }
        if (shot) {
            Screenshot(sFrame);
        }
        if (sHashFile != NULL) {
            HashFrame(sFrame);
        }
    }

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
    if (sWarping) {
        len = snprintf(line, sizeof(line), "warp: crashed in area %02x room %02x\n", sLastArea, sLastRoom);
        write(STDERR_FILENO, line, len);
    }
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    _exit(2);
}

// TMC_ROM_WRITES=log: a store into the game's read-only data (__DATA_CONST, __TEXT,__const) is
// logged (address and pc, as symbol+offset) and skipped, which is what the GBA does with a write
// to ROM. Without it such a store crashes, as the game's data is mapped read-only.
// arm64 only, as skipping a store means decoding it: on x86_64 the option is turned off
// (InstallDiagnostics).
static int sRomWrites;

#if defined(__aarch64__)
static int InRomData(uintptr_t address) {
    static const char* const kSections[][2] = { { "__DATA_CONST", "__const" }, { "__TEXT", "__const" } };
    for (size_t i = 0; i < sizeof(kSections) / sizeof(kSections[0]); i++) {
        unsigned long size;
        uintptr_t start = (uintptr_t)getsectiondata(&_mh_execute_header, kSections[i][0], kSections[i][1], &size);
        if (address >= start && address - start < size) {
            return 1;
        }
    }
    return 0;
}

static void PrintSymbol(char* out, size_t len, uintptr_t address) {
    Dl_info dl;
    if (dladdr((void*)address, &dl) && dl.dli_sname != NULL) {
        snprintf(out, len, "%s+0x%lx", dl.dli_sname, (unsigned long)(address - (uintptr_t)dl.dli_saddr));
    } else {
        snprintf(out, len, "%p", (void*)address);
    }
}

// Skips the faulting store, applying its base register writeback. Returns 0 if pc is not a
// store this knows (the crash is then reported as usual).
static int SkipStore(ucontext_t* uc) {
    uint32_t op = *(const uint32_t*)uc->uc_mcontext->__ss.__pc;
    uint64_t* regs = uc->uc_mcontext->__ss.__x;
    uint32_t rn = (op >> 5) & 31;
    int64_t offset = 0;
    int writeback = 0;

    // Single register stores: opc (bits 23:22) is 00, or x0 for SIMD registers (STR Q is 10).
    int store = (op & (1u << 26)) ? !((op >> 22) & 1) : ((op >> 22) & 3) == 0;

    if ((op & 0x3b000000) == 0x38000000 && store) {
        // STR/STRB/STRH/STUR (immediate pre/post-index, unscaled) and register offset
        if (!(op & (1u << 21)) && (((op >> 10) & 3) == 1 || ((op >> 10) & 3) == 3)) {
            writeback = 1;
            offset = (int32_t)(op << 11) >> 23;
        }
    } else if ((op & 0x3b000000) == 0x39000000 && store) {
        // STR (immediate, unsigned offset): no writeback
    } else if ((op & 0x38400000) == 0x28000000) {
        // STP/STNP: bits 25:23 = 001 post-index, 011 pre-index
        uint32_t mode = (op >> 23) & 7;
        uint32_t opc = op >> 30;
        int scale = (op & (1u << 26)) ? 2 + opc : 2 + (opc >> 1);
        if (mode == 1 || mode == 3) {
            writeback = 1;
            offset = (int64_t)((int32_t)(op << 10) >> 25) * (1 << scale);
        }
    } else {
        return 0;
    }
    if (writeback) {
        uint64_t* base = rn == 31 ? &uc->uc_mcontext->__ss.__sp : &regs[rn];
        *base += offset;
    }
    uc->uc_mcontext->__ss.__pc += 4;
    return 1;
}
#endif

static void OnCrash(int sig, siginfo_t* info, void* context) {
#if defined(__aarch64__)
    if (sRomWrites && (sig == SIGBUS || sig == SIGSEGV) && InRomData((uintptr_t)info->si_addr)) {
        ucontext_t* uc = context;
        static uintptr_t sSeen[256];
        static int sSeenCount;
        uintptr_t pc = uc->uc_mcontext->__ss.__pc;
        int seen = 0;

        for (int i = 0; i < sSeenCount; i++) {
            seen |= sSeen[i] == pc;
        }
        if (!seen) {
            char target[128], where[128], line[320];
            PrintSymbol(target, sizeof(target), (uintptr_t)info->si_addr);
            PrintSymbol(where, sizeof(where), pc);
            int len = snprintf(line, sizeof(line), "rom write: %s from %s, frame %llu\n", target, where,
                               (unsigned long long)gPlatformFrame);
            write(STDERR_FILENO, line, len);
            if (sSeenCount < 256) {
                sSeen[sSeenCount++] = pc;
            }
        }
        if (SkipStore(uc)) {
            return;
        }
    }
#endif
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
    sRomWrites = getenv("TMC_ROM_WRITES") != NULL;
#if !defined(__aarch64__)
    if (sRomWrites) {
        fprintf(stderr, "headless: TMC_ROM_WRITES needs the arm64 build (it decodes arm64 stores); ignored\n");
        sRomWrites = 0;
    }
#endif
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

int main(int argc, char** argv) {
    const char* frames = getenv("TMC_FRAMES");

    // The build's check that the save state pointer map covers the segment.
    if (getenv("TMC_STATE_MAP_CHECK") != NULL) {
        char message[256];
        if (!StateCheckMap(message, sizeof(message))) {
            fprintf(stderr, "TMC_STATE_MAP_CHECK: %s\n", message);
            return 1;
        }
        return 0;
    }
    sFrameLimitRelative = frames != NULL && frames[0] == '+';
    sFrameLimit = frames != NULL ? strtoull(frames + sFrameLimitRelative, NULL, 10) : 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    InstallDiagnostics();
    RomDataPrepareOrExit("tmc");
    if (getenv("TMC_WARP") != NULL) {
        const char* warp = getenv("TMC_WARP");
        unsigned area, room;
        int x = -1, y = -1;

        if (sscanf(warp, "%x:%x:%d:%d", &area, &room, &x, &y) < 2 || !WarpRequest(area, room, x, y)) {
            fprintf(stderr, "TMC_WARP: '%s' is not a valid area:room\n", warp);
            exit(1);
        }
        if (getenv("TMC_WARP_ITEMS") != NULL) {
            int a = 0, b = 0;
            sscanf(getenv("TMC_WARP_ITEMS"), "%i,%i", &a, &b);
            WarpSetItems(a, b);
        }
        sWarping = 1;
        atexit(ReportWarp);
    }
    if (getenv("TMC_INPUT") != NULL) {
        ParseInput(getenv("TMC_INPUT"));
    }
    if (getenv("TMC_INPUT_FROM") != NULL) {
        const char* from = getenv("TMC_INPUT_FROM");
        while (sStep < sStepCount && !(sSteps[sStep].kind == STEP_SAVE && strcmp(sSteps[sStep].text, from) == 0)) {
            sStep++;
        }
        if (sStep == sStepCount) {
            fprintf(stderr, "TMC_INPUT_FROM: the script has no save:%s step\n", from);
            exit(1);
        }
        sStep++;
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
    if (getenv("TMC_MAPVIEW") != NULL) {
        sMapViewEvery = strtoull(getenv("TMC_MAPVIEW"), NULL, 10);
    }
    sMapViewRequest = getenv("TMC_MAPVIEW_REQUEST");
    if ((sAreaDir = getenv("TMC_AREADUMP")) != NULL) {
        mkdir(sAreaDir, 0755);
        if (getenv("TMC_AREADUMP_FRAME") != NULL) {
            sAreaFrame = strtoull(getenv("TMC_AREADUMP_FRAME"), NULL, 10);
        }
    }
    sAreaCheck = getenv("TMC_AREACHECK") != NULL;
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
    if (getenv("TMC_RECORD_INPUT") != NULL) {
        if ((sRecord = fopen(getenv("TMC_RECORD_INPUT"), "w")) == NULL) {
            fprintf(stderr, "TMC_RECORD_INPUT: cannot write %s\n", getenv("TMC_RECORD_INPUT"));
            exit(1);
        }
        fprintf(sRecord, "# Keys recorded by the headless runner (TMC_RECORD_INPUT)\n");
        atexit(FinishRecording);
    }
    sLoadState = getenv("TMC_LOAD_STATE");
    sStateDir = getenv("TMC_STATE_DIR");
    if (getenv("TMC_SAVE_STATES") != NULL) {
        ParseFrameList(getenv("TMC_SAVE_STATES"), &sSaveFrames);
        sStateDir = sStateDir != NULL ? sStateDir : "states";
    }
    if (getenv("TMC_HASH") != NULL) {
        ParseFrameList(getenv("TMC_HASH"), &sHashFrames);
        sHashing = 1;
    }
    {
        static const struct {
            const char* name;
            FrameList* list;
        } dumps[] = {
            { "TMC_DUMP_OAM", &sDumpOam },
            { "TMC_DUMP_BG", &sDumpBg },
            { "TMC_DUMP_PAL", &sDumpPal },
            { "TMC_DUMP_REGS", &sDumpRegs },
        };
        for (size_t i = 0; i < sizeof(dumps) / sizeof(dumps[0]); i++) {
            if (getenv(dumps[i].name) != NULL) {
                ParseFrameList(getenv(dumps[i].name), dumps[i].list);
                sDumping = 1;
            }
        }
        sDumpOamAll = getenv("TMC_DUMP_OAM_ALL") != NULL;
        if (getenv("TMC_LAYER_MASK") != NULL) {
            PpuSetLayerMask((unsigned)strtoul(getenv("TMC_LAYER_MASK"), NULL, 16));
        }
        sDumpOut = stdout;
        if (getenv("TMC_DUMP_FILE") != NULL && (sDumpOut = fopen(getenv("TMC_DUMP_FILE"), "w")) == NULL) {
            fprintf(stderr, "TMC_DUMP_FILE: cannot write %s\n", getenv("TMC_DUMP_FILE"));
            exit(1);
        }
        if ((sDumpPngDir = getenv("TMC_DUMP_PNG")) != NULL) {
            mkdir(sDumpPngDir, 0755);
        }
    }

    if (getenv("TMC_CHECK_ENTITY_LISTS") != NULL) {
        CheckEntityLists(getenv("TMC_CHECK_ENTITY_LISTS"));
    }

    HwSetKeys(0);
    setjmp(sResetPoint);
    AgbMain();
    return 0;
}
