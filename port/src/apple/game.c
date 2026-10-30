/*
 * Game thread for the macOS and iOS apps: the platform hooks (PlatformVBlank,
 * PlatformSoftReset) plus frame pacing and the hand-off of finished frames.
 */

#include <mach/mach_time.h>
#include <os/lock.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"
#include "displaysync.h"
#include "frontend.h"
#include "latency.h"
#include "port/hw.h"
#include "port/performance.h"
#include "port/platform.h"
#include "port/state.h"

void AgbMain(void);

uint64_t gPlatformFrame;
static jmp_buf sResetPoint;

static _Atomic uint16_t sHeldKeys;
static atomic_bool sPaused;
static atomic_bool sFastForward;
static atomic_int sResetFrames;
static atomic_bool sStopRequested;
static atomic_bool sWaiting; // paused at a loop boundary
static atomic_bool sStarted;
static atomic_bool sStopped;
static atomic_int sStepFrames;
static atomic_int sPacing; // FrontendPacing

// Read by the game thread right before it hands the keys to the game.
static uint16_t (*sInputPoll)(void*);
static void* sInputPollContext;
// Called by the game thread after it published a frame that a display refresh is waiting for.
static void (*sFrameHandler)(void*);
static void* sFrameHandlerContext;

// A save or load for the game thread's next loop boundary, guarded by sStateLock.
static os_unfair_lock sStateLock = OS_UNFAIR_LOCK_INIT;
static struct {
    bool pending, save;
    char path[1024];
    FrontendStateDone done;
    void* context;
} sStateRequest;

// The newest finished frame, guarded by sFrameLock.
static os_unfair_lock sFrameLock = OS_UNFAIR_LOCK_INIT;
static uint32_t sFrame[PPU_WIDTH * PPU_HEIGHT];
static uint64_t sFrameSequence;
// The sequence of the newest frame that does not follow on from the one before
// (after a state load or a reset), so the renderer can drop its LCD history.
static uint64_t sCutSequence;
static atomic_bool sCutNext;
static atomic_bool sGpuPpuEnabled;
static atomic_bool sGpuPpuRedraw;
// Both pointers are swapped under sFrameLock; only the game thread writes sGpuWork.
static PpuCaptureLine *sGpuWork, *sGpuShared;
static bool sGpuFrameValid, sCpuFrameValid;


// Graphics debug snapshots: taken by the game thread into sSnapWork while
// viewers are open, then published into sSnapShared (guarded by sSnapLock).
static atomic_int sGfxViewers;
static atomic_bool sGfxSnapNow; // take one now (a viewer opened while paused)
static os_unfair_lock sSnapLock = OS_UNFAIR_LOCK_INIT;
static GfxSnapshot sSnapWork, sSnapShared;
static uint64_t sSnapSequence;

// The Map window's view (port/mapview.h): taken by the game thread into sMapWork
// and sMapImageWork while the window is open, then published into the shared
// copies (guarded by sMapLock, as is sMapRequest).
static atomic_int sMapViewers;
static atomic_bool sMapNow; // take one now (opened while paused, a request carried out)
static atomic_bool sMapRequestPending;
static os_unfair_lock sMapLock = OS_UNFAIR_LOCK_INIT;
static MapViewState sMapWork, sMapShared;
static MapViewImage sMapImageWork, sMapImageShared;
static uint64_t sMapSequence;
static struct {
    FrontendMapRequest request;
    int a, b;
} sMapRequest;
// Game thread only.
static char sMapStatus[sizeof(sMapWork.status)];
static bool sMapRedraw, sMapWarping;

// Holding all four reset keys for a few frames goes through the game's own
// DoSoftReset, which reinitialises things the way the cartridge would.
#define RESET_KEYS (HW_KEY_A | HW_KEY_B | HW_KEY_SELECT | HW_KEY_START)
#define RESET_HOLD_FRAMES 4

void FrontendSetSavePath(const char* path) {
    setenv("TMC_SAVE", path, 0); // eeprom.c reads it; an explicit TMC_SAVE wins
}

void FrontendSetKeys(uint16_t keys) {
    atomic_store(&sHeldKeys, keys);
}

void FrontendSetPacing(FrontendPacing pacing) {
    atomic_store(&sPacing, pacing);
}

void FrontendSetInputPoll(uint16_t (*poll)(void* context), void* context) {
    sInputPollContext = context;
    sInputPoll = poll;
}

void FrontendSetFrameHandler(void (*handler)(void* context), void* context) {
    sFrameHandlerContext = context;
    sFrameHandler = handler;
}

void FrontendSetPaused(bool paused) {
    atomic_store(&sPaused, paused);
}

bool FrontendWaitUntilPaused(int timeoutMs) {
    for (int waited = 0; !atomic_load(&sWaiting); waited++) {
        if (!atomic_load(&sStarted) || !atomic_load(&sPaused) || waited >= timeoutMs) {
            return atomic_load(&sWaiting) || !atomic_load(&sStarted);
        }
        usleep(1000);
    }
    return true;
}

bool FrontendIsPaused(void) {
    return atomic_load(&sPaused);
}

void FrontendSetFastForward(bool on) {
    atomic_store(&sFastForward, on);
}

void FrontendRequestReset(void) {
    atomic_store(&sResetFrames, RESET_HOLD_FRAMES);
}

void FrontendStepFrame(void) {
    atomic_fetch_add(&sStepFrames, 1);
}

uint64_t FrontendFrame(void) {
    return gPlatformFrame;
}

bool FrontendRequestState(bool save, const char* path, FrontendStateDone done, void* context) {
    bool accepted;

    os_unfair_lock_lock(&sStateLock);
    accepted = !sStateRequest.pending && strlen(path) < sizeof(sStateRequest.path);
    if (accepted) {
        sStateRequest.pending = true;
        sStateRequest.save = save;
        strcpy(sStateRequest.path, path);
        sStateRequest.done = done;
        sStateRequest.context = context;
    }
    os_unfair_lock_unlock(&sStateLock);
    return accepted;
}

bool FrontendSetGpuPpuEnabled(bool enabled) {
    os_unfair_lock_lock(&sFrameLock);
    if (enabled && sGpuWork == NULL) {
        sGpuWork = malloc(sizeof(PpuCaptureLine) * PPU_HEIGHT);
        sGpuShared = malloc(sizeof(PpuCaptureLine) * PPU_HEIGHT);
        if (sGpuWork == NULL || sGpuShared == NULL) {
            free(sGpuWork); free(sGpuShared);
            sGpuWork = sGpuShared = NULL;
            os_unfair_lock_unlock(&sFrameLock);
            return false;
        }
    }
    atomic_store(&sGpuPpuEnabled, enabled);
    if (!enabled) atomic_store(&sGpuPpuRedraw, true);
    os_unfair_lock_unlock(&sFrameLock);
    return true;
}

bool FrontendCopyFrame(uint32_t* dest, uint64_t* sequence, bool* cut, PpuCaptureLine* gpuLines, bool* gpuAvailable,
                       bool* cpuAvailable) {
    bool fresh;

    os_unfair_lock_lock(&sFrameLock);
    fresh = sFrameSequence != *sequence && (sCpuFrameValid || (gpuLines != NULL && sGpuFrameValid));
    if (fresh) {
        *cpuAvailable = sCpuFrameValid;
        if (sCpuFrameValid) memcpy(dest, sFrame, sizeof(sFrame));
        *gpuAvailable = gpuLines != NULL && sGpuFrameValid;
        if (*gpuAvailable) memcpy(gpuLines, sGpuShared, sizeof(PpuCaptureLine) * PPU_HEIGHT);
        *cut = sCutSequence > *sequence;
        *sequence = sFrameSequence;
    }
    os_unfair_lock_unlock(&sFrameLock);
    return fresh;
}

static uint64_t PublishFrame(const uint16_t* raw, bool gpuCaptured) {
    static uint32_t sConverted[PPU_WIDTH * PPU_HEIGHT];

    if (raw != NULL) {
        for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) {
            uint32_t r = raw[i] & 0x1F, g = (raw[i] >> 5) & 0x1F, b = (raw[i] >> 10) & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 3) | (g >> 2);
            b = (b << 3) | (b >> 2);
            sConverted[i] = 0xFF000000u | (r << 16) | (g << 8) | b; // BGRA8 in memory
        }
    }
    os_unfair_lock_lock(&sFrameLock);
    sCpuFrameValid = raw != NULL;
    if (sCpuFrameValid) memcpy(sFrame, sConverted, sizeof(sFrame));
    if (gpuCaptured) {
        PpuCaptureLine* old = sGpuShared;
        sGpuShared = sGpuWork;
        sGpuWork = old;
    }
    sGpuFrameValid = gpuCaptured;
    sFrameSequence++;
    if (atomic_exchange(&sCutNext, false)) {
        sCutSequence = sFrameSequence;
    }
    uint64_t sequence = sFrameSequence;
    os_unfair_lock_unlock(&sFrameLock);
    return sequence;
}

void FrontendGfxRetain(void) {
    if (atomic_fetch_add(&sGfxViewers, 1) == 0) {
        atomic_store(&sGfxSnapNow, true);
    }
}

void FrontendGfxRelease(void) {
    atomic_fetch_sub(&sGfxViewers, 1);
}

bool FrontendCopyGfxSnapshot(GfxSnapshot* dest, uint64_t* sequence) {
    bool fresh;

    os_unfair_lock_lock(&sSnapLock);
    fresh = sSnapSequence != *sequence;
    if (fresh) {
        memcpy(dest, &sSnapShared, sizeof(*dest));
        *sequence = sSnapSequence;
    }
    os_unfair_lock_unlock(&sSnapLock);
    return fresh;
}

static void PublishSnapshot(void) {
    os_unfair_lock_lock(&sSnapLock);
    memcpy(&sSnapShared, &sSnapWork, sizeof(sSnapShared));
    sSnapSequence++;
    os_unfair_lock_unlock(&sSnapLock);
}

// The hardware as it stands, outside a frame (paused, or just loaded).
static void SnapshotNow(void) {
    atomic_store(&sGfxSnapNow, false);
    GfxSnapshotBegin(&sSnapWork, gPlatformFrame);
    GfxSnapshotEnd(&sSnapWork);
    sSnapWork.hasLines = 0;
    PublishSnapshot();
}

void FrontendMapRetain(void) {
    if (atomic_fetch_add(&sMapViewers, 1) == 0) {
        // A picture of the room as it is now, not one kept from an earlier opening.
        FrontendRequestMap(FRONTEND_MAP_REDRAW, 0, 0);
    }
}

void FrontendMapRelease(void) {
    atomic_fetch_sub(&sMapViewers, 1);
}

bool FrontendCopyMapState(MapViewState* dest, uint64_t* sequence) {
    bool fresh;

    os_unfair_lock_lock(&sMapLock);
    fresh = sMapSequence != *sequence;
    if (fresh) {
        memcpy(dest, &sMapShared, sizeof(*dest));
        *sequence = sMapSequence;
    }
    os_unfair_lock_unlock(&sMapLock);
    return fresh;
}

bool FrontendCopyMapImage(MapViewImage* dest, uint64_t* sequence) {
    bool fresh;

    os_unfair_lock_lock(&sMapLock);
    fresh = sMapImageShared.sequence != *sequence;
    if (fresh) {
        memcpy(dest, &sMapImageShared, sizeof(*dest));
        *sequence = sMapImageShared.sequence;
    }
    os_unfair_lock_unlock(&sMapLock);
    return fresh;
}

bool FrontendRequestMap(FrontendMapRequest request, int a, int b) {
    bool accepted;

    os_unfair_lock_lock(&sMapLock);
    accepted = !atomic_load(&sMapRequestPending);
    if (accepted) {
        sMapRequest.request = request;
        sMapRequest.a = a;
        sMapRequest.b = b;
        atomic_store(&sMapRequestPending, true);
    }
    os_unfair_lock_unlock(&sMapLock);
    return accepted;
}

// Take the Map window's view (settled: paused or just loaded, nothing will change).
static void CaptureMap(bool settled) {
    bool drawn;

    atomic_store(&sMapNow, false);
    drawn = MapViewCapture(&sMapWork, &sMapImageWork, gPlatformFrame, settled, sMapRedraw);
    sMapRedraw = sMapRedraw && !drawn && sMapWork.inGame;
    memcpy(sMapWork.status, sMapStatus, sizeof(sMapStatus));
    os_unfair_lock_lock(&sMapLock);
    memcpy(&sMapShared, &sMapWork, sizeof(sMapShared));
    if (drawn) {
        memcpy(&sMapImageShared, &sMapImageWork, sizeof(sMapImageShared));
    }
    sMapSequence++;
    os_unfair_lock_unlock(&sMapLock);
}

// Carry out the Map window's request (at a loop boundary only).
static void ServiceMapRequest(void) {
    FrontendMapRequest request;
    int a, b;

    if (!atomic_load(&sMapRequestPending)) {
        return;
    }
    os_unfair_lock_lock(&sMapLock);
    request = sMapRequest.request;
    a = sMapRequest.a;
    b = sMapRequest.b;
    atomic_store(&sMapRequestPending, false);
    os_unfair_lock_unlock(&sMapLock);
    switch (request) {
        case FRONTEND_MAP_REDRAW:
            sMapRedraw = true;
            break;
        case FRONTEND_MAP_MOVE:
            MapViewMovePlayer(a, b, sMapStatus, sizeof(sMapStatus));
            break;
        case FRONTEND_MAP_WARP:
            sMapWarping = MapViewWarp(a, b, sMapStatus, sizeof(sMapStatus)) || sMapWarping;
            break;
    }
    atomic_store(&sMapNow, true);
}

static uint16_t sRaw[PPU_WIDTH * PPU_HEIGHT];
static uint64_t sFramePeriod; // mach ticks
static double sTicksToNs;
static double sMarginMs = 7; // TMC_SYNC_MARGIN_MS: room after the frame's work for the draw and the GPU
static uint64_t sNextDeadline;

static void InitPacing(void) {
    mach_timebase_info_data_t timebase;

    mach_timebase_info(&timebase);
    if (getenv("TMC_SYNC_MARGIN_MS") != NULL) {
        sMarginMs = atof(getenv("TMC_SYNC_MARGIN_MS"));
    }
    sTicksToNs = (double)timebase.numer / timebase.denom;
    sFramePeriod = (uint64_t)(1e9 / FRONTEND_FRAME_RATE * timebase.denom / timebase.numer);
    sNextDeadline = mach_absolute_time() + sFramePeriod;
}

// Whether a display refresh drives this frame (display sync pacing, with the
// display link ticking, outside fast-forward).
static bool DisplayDriven(void) {
    return atomic_load(&sPacing) == FRONTEND_PACING_DISPLAY_SYNC && !atomic_load(&sFastForward) && DisplaySyncActive();
}

// Frames per second the game runs at now.
static double GameFrameRate(bool driven) {
    uint64_t period = driven ? DisplaySyncFramePeriod() : 0;

    return period != 0 ? 1e9 / ((double)period * sTicksToNs) : FRONTEND_FRAME_RATE;
}

// Sleep until this frame's slot. Fast-forward runs at up to 4x; falling far
// behind (a stall, the debugger) resynchronises instead of racing to catch up.
// Driven by the display, the frame is not held back here: the wait happened in
// LatchInput, so that it finishes just in time for its refresh.
static void Pace(bool driven) {
    uint64_t now = mach_absolute_time();
    uint64_t period = atomic_load(&sFastForward) ? sFramePeriod / 4 : sFramePeriod;

    if (driven) {
        sNextDeadline = now + period; // the timer takes over from here if the display stops
        return;
    }
    if (now < sNextDeadline) {
        mach_wait_until(sNextDeadline);
    } else if (now - sNextDeadline > 4 * sFramePeriod) {
        sNextDeadline = now;
    }
    sNextDeadline += period;
}

// How long the game needs from reading the keys to having a frame ready for the
// display: the longest of the last few frames, and room for the main thread to
// draw it and the GPU to finish. Frames that included a pause, a save or a load
// do not count.
#define WORK_SAMPLES 32
#define WORK_MARGIN_NS (sMarginMs * 1e6)
static uint64_t sWork[WORK_SAMPLES];
static int sWorkCount, sWorkNext;
static uint64_t sLatchTime;     // when the keys were last latched
static bool sSkipWork;          // this frame is not a typical one
static uint64_t sPublished;     // sequence of the last frame published
static uint64_t sLastVsync;     // the refresh the last display-driven frame was aimed at
static atomic_int sStallMs;     // self-test: make the next frame this long (as an in-game save can)

static void NoteWork(uint64_t ticks) {
    if (sSkipWork) {
        sSkipWork = false;
        return;
    }
    sWork[sWorkNext] = ticks;
    sWorkNext = (sWorkNext + 1) % WORK_SAMPLES;
    sWorkCount = sWorkCount < WORK_SAMPLES ? sWorkCount + 1 : WORK_SAMPLES;
}

static uint64_t WorkBudget(void) {
    mach_timebase_info_data_t timebase;
    uint64_t longest = 0;

    if (sWorkCount < WORK_SAMPLES / 2) {
        return 0; // no idea yet: do not hold the frame back
    }
    for (int i = 0; i < sWorkCount; i++) {
        longest = sWork[i] > longest ? sWork[i] : longest;
    }
    mach_timebase_info(&timebase);
    return longest + (uint64_t)(WORK_MARGIN_NS * timebase.denom / timebase.numer);
}

// Called as the last thing of a frame, which is as late as it can be before the
// game reads the keys (ReadKeyInput, straight after PlatformVBlank returns): wait
// until the frame the game is about to compute will just make its refresh (or,
// with the timer, its slot), then latch the keys. The wait is what takes the
// age out of the input: without it the keys would be sampled when the previous
// frame was drawn, a whole frame before the next one is.
static void LatchInput(bool driven) {
    uint64_t budget = WorkBudget();
    uint64_t now = mach_absolute_time();
    uint16_t keys = 0;
    int resetFrames;

    if (budget != 0 && !atomic_load(&sPaused) && !atomic_load(&sFastForward) && !atomic_load(&sStopRequested)) {
        uint64_t target = 0;
        uint64_t vsync;

        if (driven) {
            if (DisplaySyncPredictVsync(now + budget, &vsync)) {
                uint64_t grid = DisplaySyncFramePeriod();
                // One frame per refresh. A refresh before the last one aimed at means
                // that aim was too far ahead (planned with a slow frame's budget,
                // which has since dropped out): start again from the display.
                if (sLastVsync != 0 && vsync >= sLastVsync && vsync < sLastVsync + grid) {
                    vsync = sLastVsync + grid;
                }
                sLastVsync = vsync;
                target = vsync - budget;
                // The frame after the one the game is about to compute is the one that shows its effect... but this is the next one published.
                LatencyFrameTarget(sPublished + 1, (uint64_t)(vsync * sTicksToNs));
            }
        } else {
            target = sNextDeadline - budget; // the next Pace is the slot's end
        }
        if (target > now && target - now < 2 * sFramePeriod) {
            mach_wait_until(target);
        }
    }
    sLatchTime = mach_absolute_time();

    LatencyFrameLatched(2);
    if (sInputPoll != NULL) {
        keys = sInputPoll(sInputPollContext);
    }
    keys |= atomic_load(&sHeldKeys);
    resetFrames = atomic_load(&sResetFrames);
    if (resetFrames > 0) {
        keys = RESET_KEYS;
        atomic_compare_exchange_strong(&sResetFrames, &resetFrames, resetFrames - 1);
    }
    HwSetKeys(keys);
}

// Carry out a pending save or load (at a loop boundary only).
static void ServiceStateRequest(void) {
    char message[512];
    bool ok;

    os_unfair_lock_lock(&sStateLock);
    if (!sStateRequest.pending) {
        os_unfair_lock_unlock(&sStateLock);
        return;
    }
    os_unfair_lock_unlock(&sStateLock);

    sSkipWork = true;
    if (sStateRequest.save) {
        ok = StateSave(sStateRequest.path, gPlatformFrame, message, sizeof(message));
    } else {
        uint64_t frame;
        ok = StateLoad(sStateRequest.path, &frame, message, sizeof(message));
        if (ok) {
            gPlatformFrame = frame;
            atomic_store(&sCutNext, true);
            // Show the loaded picture, even while paused (a plain redraw of
            // the loaded registers, without running the game).
            PpuRenderFrame(sRaw);
            PublishFrame(sRaw, false);
            if (atomic_load(&sGfxViewers) > 0) {
                SnapshotNow();
            }
            if (atomic_load(&sMapViewers) > 0) {
                CaptureMap(true);
            }
        }
    }
    if (sStateRequest.done != NULL) {
        sStateRequest.done(ok, message, gPlatformFrame, sStateRequest.context);
    }
    os_unfair_lock_lock(&sStateLock);
    sStateRequest.pending = false;
    os_unfair_lock_unlock(&sStateLock);
}

// Pausing (and stepping) happens at loop boundaries, so a paused game can
// always save or load.
static void WaitWhilePaused(void) {
    if (!atomic_load(&sPaused)) {
        return;
    }
    sSkipWork = true;
    atomic_store(&sWaiting, true);
    while (atomic_load(&sPaused) && !atomic_load(&sStopRequested)) {
        int steps = atomic_load(&sStepFrames);
        if (steps > 0 && atomic_compare_exchange_strong(&sStepFrames, &steps, steps - 1)) {
            break;
        }
        ServiceStateRequest();
        ServiceMapRequest();
        if (atomic_exchange(&sGpuPpuRedraw, false) && !atomic_load(&sGpuPpuEnabled)) {
            PpuRenderFrame(sRaw);
            PublishFrame(sRaw, false);
        }
        if (atomic_load(&sGfxSnapNow) && atomic_load(&sGfxViewers) > 0) {
            SnapshotNow();
        }
        if (atomic_load(&sMapNow) && atomic_load(&sMapViewers) > 0) {
            CaptureMap(true);
        }
        usleep(5000);
    }
    atomic_store(&sWaiting, false);
    sNextDeadline = mach_absolute_time();
}

static void StopIfRequested(void) {
    if (atomic_load(&sStopRequested)) {
        atomic_store(&sStopped, true);
        for (;;) {
            pause();
        }
    }
}

static uint64_t sPerfLogicStart;

void PlatformVBlank(void) {
    uint64_t perfEntry = PerformanceEnabled() ? PerformanceNow() : 0;
    uint64_t gameNs = sPerfLogicStart && perfEntry >= sPerfLogicStart ? perfEntry - sPerfLogicStart : 0;
    uint64_t frameStart;
    bool driven;

    StopIfRequested();
    if (PlatformAtLoopBoundary()) {
        ServiceStateRequest();
        WaitWhilePaused();
        ServiceMapRequest();
        if (sMapWarping) {
            sMapWarping = MapViewUpdate(gPlatformFrame, sMapStatus, sizeof(sMapStatus));
        }
    }
    driven = DisplayDriven();
    Pace(driven);
    StopIfRequested();

    bool perf = PerformanceEnabled() && gameNs != 0 && !atomic_load(&sPaused);
    uint64_t perfStart = perf ? PerformanceNow() : 0;
    if (perf) PerformanceBeginFrame();
    frameStart = LatencyNowNs();
    bool captureGpu = atomic_load(&sGpuPpuEnabled);
    static unsigned gpuSelfTestFrames;
    bool reference = captureGpu && getenv("TMC_GPU_PPU_SELFTEST") && gpuSelfTestFrames < 60;
    if (captureGpu && getenv("TMC_GPU_PPU_SELFTEST") && gpuSelfTestFrames <= 60) {
        if (gpuSelfTestFrames++ == 60) fprintf(stderr, "GPU PPU self-test: GPU-only frames started\n");
    }
    uint16_t* draw = !captureGpu || reference ? sRaw : NULL;
    if (!captureGpu) {
        atomic_store(&sGpuPpuRedraw, false);
        os_unfair_lock_lock(&sFrameLock);
        if (!atomic_load(&sGpuPpuEnabled) && sGpuWork != NULL) {
            free(sGpuWork); free(sGpuShared);
            sGpuWork = sGpuShared = NULL;
            sGpuFrameValid = false;
        }
        os_unfair_lock_unlock(&sFrameLock);
    } else {
        PpuCaptureTo(sGpuWork);
    }
    if (atomic_load_explicit(&sMapViewers, memory_order_relaxed) > 0) {
        CaptureMap(false);
    }
    // Draw this frame line by line, running its HBlank DMA and interrupts.
    // With debug viewers open, record the hardware it was drawn from.
    if (atomic_load_explicit(&sGfxViewers, memory_order_relaxed) > 0) {
        atomic_store(&sGfxSnapNow, false); // this frame's snapshot will do
        GfxSnapshotBegin(&sSnapWork, gPlatformFrame);
        HwRunFrame(draw);
        GfxSnapshotEnd(&sSnapWork);
        PublishSnapshot();
    } else {
        HwRunFrame(draw);
    }
    if (captureGpu) PpuCaptureTo(NULL);
    uint64_t hardwareNs = perf ? PerformanceNow() - perfStart : 0;
    uint64_t publishStart = perf ? PerformanceNow() : 0;
    int stall = atomic_exchange(&sStallMs, 0);
    if (stall > 0) {
        usleep((useconds_t)stall * 1000);
    }
    sPublished = PublishFrame(draw, captureGpu);
    if (perf && stall == 0) PerformanceEndFrame(gPlatformFrame, gameNs, hardwareNs, PerformanceNow() - publishStart, 1000.0 / GameFrameRate(driven) / (atomic_load(&sFastForward) ? 4.0 : 1.0));
    LatencyFrameDone(sPublished, frameStart);
    gPlatformFrame++;
    if (sLatchTime != 0) {
        NoteWork(mach_absolute_time() - sLatchTime);
    }
    if (driven && sFrameHandler != NULL) {
        sFrameHandler(sFrameHandlerContext);
    }
    // The game plays at the display's rate here, a little above 59.73 Hz: tell
    // the audio, whose clock-drift correction then has only the rest to do.
    AudioSetRateScale(GameFrameRate(driven) / FRONTEND_FRAME_RATE);
    LatchInput(driven);
    sPerfLogicStart = PerformanceEnabled() && !atomic_load(&sPaused) ? PerformanceNow() : 0;
}

void FrontendStallNextFrameForTesting(int ms) {
    atomic_store(&sStallMs, ms);
}

_Noreturn void PlatformSoftReset(void) {
    atomic_store(&sCutNext, true);
    longjmp(sResetPoint, 1);
}

static void* GameThread(void* arg) {
    (void)arg;
    pthread_setname_np("game");
    InitPacing();
    HwSetKeys(0);
    setjmp(sResetPoint);
    AgbMain();
    return NULL;
}

void FrontendStartGame(void) {
    pthread_attr_t attr;
    pthread_t thread;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 << 20); // the game was written for the main stack
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    if (pthread_create(&thread, &attr, GameThread, NULL) != 0) {
        fprintf(stderr, "cannot start the game thread\n");
        abort();
    }
    pthread_attr_destroy(&attr);
    atomic_store(&sStarted, true);
}

void FrontendStopGame(void) {
    atomic_store(&sStopRequested, true);
    atomic_store(&sPaused, false);
    // A frame takes ~17 ms; give the game a few to reach its next VBlank.
    for (int i = 0; i < 20 && !atomic_load(&sStopped); i++) {
        usleep(5000);
    }
}
