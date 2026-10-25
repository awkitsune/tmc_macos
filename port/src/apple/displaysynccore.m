/*
 * Display sync timing shared by both apps (displaysync.h, displaysynccore.h).
 */

#import "displaysync.h"
#import "displaysynccore.h"

#include <mach/mach_time.h>
#include <stdatomic.h>
#include <string.h>

#include <math.h>
#include <stdlib.h>

#include "latency.h"

#define PERIOD_SAMPLES 32
#define TICK_TIMEOUT_MS 100

static _Atomic bool sUsable;
static _Atomic uint64_t sGrid;     // ticks per game frame
static _Atomic uint64_t sRefresh;  // ticks per display refresh
static _Atomic uint64_t sAnchor;   // a refresh a game frame was (or will be) shown at
static atomic_bool sAnchorFromPresent;
static _Atomic uint64_t sLastTick; // when the display link last fired
static _Atomic int sDivisor;
static mach_timebase_info_data_t sTimebase;

static atomic_bool sResetRequested; // from the main thread: measure the period afresh

// Display link thread only.
static uint64_t sLastOutput;
static uint64_t sPeriods[PERIOD_SAMPLES];
static int sPeriodCount, sPeriodNext;
static void (^sUsableChanged)(void);

static uint64_t NsToTicks(double ns) {
    return (uint64_t)(ns * sTimebase.denom / sTimebase.numer);
}

static double TicksToNs(uint64_t ticks) {
    return (double)ticks * sTimebase.numer / sTimebase.denom;
}

bool DisplaySyncUsable(void) {
    return atomic_load(&sUsable);
}

bool DisplaySyncActive(void) {
    return atomic_load(&sUsable) && mach_absolute_time() - atomic_load(&sLastTick) < NsToTicks(TICK_TIMEOUT_MS * 1e6);
}

uint64_t DisplaySyncFramePeriod(void) {
    return atomic_load(&sUsable) ? atomic_load(&sGrid) : 0;
}

bool DisplaySyncPredictVsync(uint64_t earliest, uint64_t* vsync) {
    uint64_t grid = atomic_load(&sGrid), anchor = atomic_load(&sAnchor);

    if (!atomic_load(&sUsable) || grid == 0 || anchor == 0) {
        return false;
    }
    if (earliest <= anchor) {
        *vsync = anchor;
    } else {
        *vsync = anchor + (earliest - anchor + grid - 1) / grid * grid;
    }
    return true;
}

void DisplaySyncNotePresented(double seconds) {
    uint64_t ticks = NsToTicks(seconds * 1e9);

    if (seconds > 0 && atomic_load(&sUsable)) {
        atomic_store(&sAnchor, ticks);
        atomic_store(&sAnchorFromPresent, true);
    }
}

static int CompareTicks(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

// Display link thread: what the refresh period is, and so whether and how the
// game can follow it.
void DisplaySyncTick(uint64_t outputTime) {
    if (atomic_exchange(&sResetRequested, false)) {
        sPeriodCount = 0;
        sPeriodNext = 0;
        sLastOutput = 0;
    }
    atomic_store(&sLastTick, mach_absolute_time());
    LatencyTick(false);
    if (sLastOutput != 0 && outputTime > sLastOutput) {
        sPeriods[sPeriodNext] = outputTime - sLastOutput;
        sPeriodNext = (sPeriodNext + 1) % PERIOD_SAMPLES;
        sPeriodCount = sPeriodCount < PERIOD_SAMPLES ? sPeriodCount + 1 : PERIOD_SAMPLES;
    }
    sLastOutput = outputTime;
    if (sPeriodCount < PERIOD_SAMPLES / 2) {
        return;
    }
    uint64_t sorted[PERIOD_SAMPLES];
    memcpy(sorted, sPeriods, sizeof(sorted));
    qsort(sorted, sPeriodCount, sizeof(uint64_t), CompareTicks);
    uint64_t period = sorted[sPeriodCount / 2];
    double hz = 1e9 / TicksToNs(period);
    int divisor = (int)lround(hz / 60);
    bool usable = divisor >= 1 && divisor <= 4 && fabs(hz / divisor - 60) < 1.5;

    atomic_store(&sRefresh, period);
    atomic_store(&sDivisor, divisor);
    atomic_store(&sGrid, period * (uint64_t)MAX(divisor, 1));
    if (!atomic_load(&sAnchorFromPresent)) {
        atomic_store(&sAnchor, outputTime);
    }
    if (usable != atomic_load(&sUsable)) {
        atomic_store(&sUsable, usable);
        void (^changed)(void) = sUsableChanged;
        if (changed != nil) {
            dispatch_async(dispatch_get_main_queue(), changed);
        }
    }
}

void DisplaySyncCoreInit(void) {
    mach_timebase_info(&sTimebase);
}

void DisplaySyncSetUsableChanged(void (^changed)(void)) {
    sUsableChanged = changed;
}

void DisplaySyncForget(void) {
    atomic_store(&sAnchorFromPresent, false);
    atomic_store(&sUsable, false);
    atomic_store(&sResetRequested, true);
}

void DisplaySyncStopped(void) {
    atomic_store(&sUsable, false);
    atomic_store(&sAnchorFromPresent, false);
}

NSString* DisplaySyncStatus(bool enabled, bool haveLink) {
    int divisor = atomic_load(&sDivisor);
    uint64_t refresh = atomic_load(&sRefresh);

    if (!enabled) {
        return @"";
    }
    if (!haveLink) {
        return @"No display link: the accurate timer is used.";
    }
    if (refresh == 0) {
        return @"Measuring the display…";
    }
    double hz = 1e9 / TicksToNs(refresh);
    if (!atomic_load(&sUsable)) {
        return [NSString stringWithFormat:@"The display runs at %.0f Hz, not a multiple of 60: the accurate timer is used.", hz];
    }
    if (divisor == 1) {
        return [NSString stringWithFormat:@"The display runs at %.0f Hz: one game frame per refresh.", hz];
    }
    return [NSString stringWithFormat:@"The display runs at %.0f Hz: one game frame per %d refreshes.", hz, divisor];
}
