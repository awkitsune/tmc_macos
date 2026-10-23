#include "latency.h"

#include <mach/mach_time.h>
#include <math.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PENDING 64
#define MAX_SAMPLES (1 << 17)

typedef struct {
    int source;
    uint64_t eventNs, latchNs, seq;
} Entry;

static FILE* sLog;
static os_unfair_lock sLock = OS_UNFAIR_LOCK_INIT;

static _Atomic uint64_t sPendingEvent[LATENCY_SOURCES];

// Game thread only: latched events waiting for the frame that shows them.
typedef struct {
    uint64_t eventNs[LATENCY_SOURCES];
    uint64_t latchNs;
    int framesToGo; // published frames until the one that shows them
} Latched;
static Latched sLatched[4];
static int sLatchedCount;
static uint64_t sLastLatch;

// Guarded by sLock.
static Entry sEntries[MAX_PENDING];
static int sEntryCount;
static double sTotals[LATENCY_SOURCES][4096];
static int sTotalCount[LATENCY_SOURCES];
static uint64_t sLastSeq;
static double sLastPresent;
static bool sHavePresent;
static uint64_t sPresented, sUnpresented, sRepeated, sSkipped;
static double sPresentIntervals[MAX_SAMPLES];
static int sPresentIntervalCount;

// Game thread only, read at the end.
static double sLatchIntervals[MAX_SAMPLES];
static int sLatchIntervalCount;
static _Atomic uint64_t sTicks, sTicksDropped;

bool LatencyEnabled(void) {
    return sLog != NULL;
}

uint64_t LatencyNowNs(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void LatencyStart(void) {
    const char* path = getenv("TMC_LATENCY_LOG");

    if (path == NULL || path[0] == '\0') {
        return;
    }
    sLog = fopen(path, "w");
    if (sLog == NULL) {
        fprintf(stderr, "latency: cannot write %s\n", path);
        return;
    }
    fprintf(sLog, "# source,frame,event_to_latch_ms,latch_to_present_ms,total_ms\n");
    fflush(sLog);
}

void LatencyNote(const char* text) {
    if (sLog == NULL) {
        return;
    }
    os_unfair_lock_lock(&sLock);
    fprintf(sLog, "# %s\n", text);
    fflush(sLog);
    os_unfair_lock_unlock(&sLock);
}

void LatencyInputEvent(int source, uint64_t eventNs) {
    uint64_t none = 0;

    if (sLog != NULL) {
        atomic_compare_exchange_strong(&sPendingEvent[source], &none, eventNs); // keep the earliest
    }
}

void LatencyFrameLatched(int framesToGo) {
    Latched latched = { .framesToGo = framesToGo };
    bool any = false;

    if (sLog == NULL) {
        return;
    }
    for (int i = 0; i < LATENCY_SOURCES; i++) {
        latched.eventNs[i] = atomic_exchange(&sPendingEvent[i], 0);
        any |= latched.eventNs[i] != 0;
    }
    latched.latchNs = LatencyNowNs();
    if (any && sLatchedCount < 4) {
        sLatched[sLatchedCount++] = latched;
    }
}

void LatencyFrameDone(uint64_t seq, uint64_t startNs) {
    int kept = 0;

    if (sLog == NULL) {
        return;
    }
    if (sLastLatch != 0 && sLatchIntervalCount < MAX_SAMPLES) {
        sLatchIntervals[sLatchIntervalCount++] = (startNs - sLastLatch) / 1e6;
    }
    sLastLatch = startNs;
    os_unfair_lock_lock(&sLock);
    for (int n = 0; n < sLatchedCount; n++) {
        Latched* latched = &sLatched[n];
        if (--latched->framesToGo > 0) {
            sLatched[kept++] = *latched;
            continue;
        }
        for (int i = 0; i < LATENCY_SOURCES; i++) {
            if (latched->eventNs[i] != 0 && sEntryCount < MAX_PENDING) {
                sEntries[sEntryCount++] = (Entry){ i, latched->eventNs[i], latched->latchNs, seq };
            }
        }
    }
    sLatchedCount = kept;
    os_unfair_lock_unlock(&sLock);
}

void LatencyPresented(uint64_t seq, double presentedSeconds) {
    if (sLog == NULL) {
        return;
    }
    os_unfair_lock_lock(&sLock);
    if (presentedSeconds <= 0) {
        sUnpresented++;
        os_unfair_lock_unlock(&sLock);
        return;
    }
    sPresented++;
    if (sHavePresent) {
        if (sPresentIntervalCount < MAX_SAMPLES) {
            sPresentIntervals[sPresentIntervalCount++] = (presentedSeconds - sLastPresent) * 1000;
        }
        if (seq == sLastSeq) {
            sRepeated++;
        } else if (seq > sLastSeq + 1) {
            sSkipped += seq - sLastSeq - 1;
        }
    }
    sHavePresent = true;
    sLastPresent = presentedSeconds;
    sLastSeq = seq;

    uint64_t presentNs = (uint64_t)(presentedSeconds * 1e9);
    int kept = 0;
    for (int i = 0; i < sEntryCount; i++) {
        Entry e = sEntries[i];
        if (e.seq > seq) {
            sEntries[kept++] = e;
            continue;
        }
        double toLatch = ((double)e.latchNs - (double)e.eventNs) / 1e6;
        double toPresent = ((double)presentNs - (double)e.latchNs) / 1e6;
        fprintf(sLog, "%s,%llu,%.2f,%.2f,%.2f\n", e.source == LATENCY_KEY ? "key" : "pad", (unsigned long long)e.seq,
                toLatch, toPresent, toLatch + toPresent);
        if (sTotalCount[e.source] < 4096) {
            sTotals[e.source][sTotalCount[e.source]++] = toLatch + toPresent;
        }
    }
    sEntryCount = kept;
    fflush(sLog);
    os_unfair_lock_unlock(&sLock);
}

void LatencyTick(bool dropped) {
    if (sLog == NULL) {
        return;
    }
    atomic_fetch_add(&sTicks, 1);
    if (dropped) {
        atomic_fetch_add(&sTicksDropped, 1);
    }
}

static int Compare(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static void Describe(FILE* out, const char* name, double* values, int count) {
    double sum = 0, squares = 0;

    if (count == 0) {
        fprintf(out, "# %s: no samples\n", name);
        return;
    }
    qsort(values, count, sizeof(double), Compare);
    for (int i = 0; i < count; i++) {
        sum += values[i];
        squares += values[i] * values[i];
    }
    double mean = sum / count;
    fprintf(out, "# %s: n=%d mean %.2f ms, p50 %.2f, p95 %.2f, p99 %.2f, min %.2f, max %.2f, sd %.2f\n", name, count, mean,
            values[count / 2], values[(int)(count * 0.95)], values[(int)(count * 0.99)], values[0], values[count - 1],
            sqrt(fmax(squares / count - mean * mean, 0)));
}

void LatencyFinish(void) {
    if (sLog == NULL) {
        return;
    }
    os_unfair_lock_lock(&sLock);
    for (int i = 0; i < LATENCY_SOURCES; i++) {
        Describe(sLog, i == LATENCY_KEY ? "total latency, key" : "total latency, pad", sTotals[i], sTotalCount[i]);
    }
    fprintf(sLog, "# presented frames %llu (never shown %llu), game frames shown twice %llu, skipped %llu\n",
            (unsigned long long)sPresented, (unsigned long long)sUnpresented, (unsigned long long)sRepeated,
            (unsigned long long)sSkipped);
    if (sPresentIntervalCount > 0) {
        double* copy = malloc(sPresentIntervalCount * sizeof(double));
        memcpy(copy, sPresentIntervals, sPresentIntervalCount * sizeof(double));
        Describe(sLog, "present interval", copy, sPresentIntervalCount);
        double median = copy[sPresentIntervalCount / 2];
        int late = 0;
        for (int i = 0; i < sPresentIntervalCount; i++) {
            late += copy[i] > median * 1.5;
        }
        fprintf(sLog, "# present intervals above 1.5x the median (%.2f ms): %d\n", median, late);
        free(copy);
    }
    Describe(sLog, "game frame interval", sLatchIntervals, sLatchIntervalCount);
    if (sLatchIntervalCount > 0) {
        double median = sLatchIntervals[sLatchIntervalCount / 2];
        int late = 0;
        for (int i = 0; i < sLatchIntervalCount; i++) {
            late += sLatchIntervals[i] > median * 1.5;
        }
        fprintf(sLog, "# game frame intervals above 1.5x the median (%.2f ms): %d of %d\n", median, late, sLatchIntervalCount);
    }
    fprintf(sLog, "# display ticks %llu, dropped %llu\n", (unsigned long long)atomic_load(&sTicks),
            (unsigned long long)atomic_load(&sTicksDropped));
    fclose(sLog);
    sLog = NULL;
    os_unfair_lock_unlock(&sLock);
}
