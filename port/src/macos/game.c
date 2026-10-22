/*
 * Game thread for the macOS app: the platform hooks (PlatformVBlank,
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

#include "frontend.h"
#include "port/hw.h"
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
static atomic_bool sStopped;
static atomic_int sStepFrames;

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

void FrontendSetPaused(bool paused) {
    atomic_store(&sPaused, paused);
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

bool FrontendCopyFrame(uint32_t* dest, uint64_t* sequence) {
    bool fresh;

    os_unfair_lock_lock(&sFrameLock);
    fresh = sFrameSequence != *sequence;
    if (fresh) {
        memcpy(dest, sFrame, sizeof(sFrame));
        *sequence = sFrameSequence;
    }
    os_unfair_lock_unlock(&sFrameLock);
    return fresh;
}

static void PublishFrame(const uint16_t* raw) {
    static uint32_t sConverted[PPU_WIDTH * PPU_HEIGHT];

    for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) {
        uint32_t r = raw[i] & 0x1F, g = (raw[i] >> 5) & 0x1F, b = (raw[i] >> 10) & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        sConverted[i] = 0xFF000000u | (r << 16) | (g << 8) | b; // BGRA8 in memory
    }
    os_unfair_lock_lock(&sFrameLock);
    memcpy(sFrame, sConverted, sizeof(sFrame));
    sFrameSequence++;
    os_unfair_lock_unlock(&sFrameLock);
}

static uint16_t sRaw[PPU_WIDTH * PPU_HEIGHT];
static uint64_t sFramePeriod; // mach ticks
static uint64_t sNextDeadline;

static void InitPacing(void) {
    mach_timebase_info_data_t timebase;

    mach_timebase_info(&timebase);
    sFramePeriod = (uint64_t)(1e9 / FRONTEND_FRAME_RATE * timebase.denom / timebase.numer);
    sNextDeadline = mach_absolute_time() + sFramePeriod;
}

// Sleep until this frame's slot. Fast-forward runs at up to 4x; falling far
// behind (a stall, the debugger) resynchronises instead of racing to catch up.
static void Pace(void) {
    uint64_t now = mach_absolute_time();
    uint64_t period = atomic_load(&sFastForward) ? sFramePeriod / 4 : sFramePeriod;

    if (now < sNextDeadline) {
        mach_wait_until(sNextDeadline);
    } else if (now - sNextDeadline > 4 * sFramePeriod) {
        sNextDeadline = now;
    }
    sNextDeadline += period;
}

static void PublishFrame(void);

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

    if (sStateRequest.save) {
        ok = StateSave(sStateRequest.path, gPlatformFrame, NULL, message, sizeof(message));
    } else {
        uint64_t frame;
        ok = StateLoad(sStateRequest.path, &frame, message, sizeof(message));
        if (ok) {
            gPlatformFrame = frame;
            PublishFrame(); // show the loaded picture, even while paused
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
    while (atomic_load(&sPaused) && !atomic_load(&sStopRequested)) {
        int steps = atomic_load(&sStepFrames);
        if (steps > 0 && atomic_compare_exchange_strong(&sStepFrames, &steps, steps - 1)) {
            break;
        }
        ServiceStateRequest();
        usleep(5000);
    }
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

void PlatformVBlank(void) {
    uint16_t keys;
    int resetFrames;

    StopIfRequested();
    if (PlatformAtLoopBoundary()) {
        ServiceStateRequest();
        WaitWhilePaused();
    }
    Pace();
    StopIfRequested();

    keys = atomic_load(&sHeldKeys);
    resetFrames = atomic_load(&sResetFrames);
    if (resetFrames > 0) {
        keys = RESET_KEYS;
        atomic_compare_exchange_strong(&sResetFrames, &resetFrames, resetFrames - 1);
    }
    HwSetKeys(keys);
    // Draw this frame line by line, running its HBlank DMA and interrupts.
    HwRunFrame(sRaw);
    PublishFrame(sRaw);
    gPlatformFrame++;
}

_Noreturn void PlatformSoftReset(void) {
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
}

void FrontendStopGame(void) {
    atomic_store(&sStopRequested, true);
    atomic_store(&sPaused, false);
    // A frame takes ~17 ms; give the game a few to reach its next VBlank.
    for (int i = 0; i < 20 && !atomic_load(&sStopped); i++) {
        usleep(5000);
    }
}
