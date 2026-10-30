#include "port/performance.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static atomic_bool sEnabled;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static PerformanceSnapshot sSnapshot;
static int sNext;
static double sLastPresented;
// Only the game thread touches the current frame's accumulators.
static uint64_t sPpu[PERF_STAGES];

bool PerformanceEnabled(void) { return atomic_load_explicit(&sEnabled, memory_order_relaxed); }
uint64_t PerformanceNow(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
void PerformanceSetEnabled(bool enabled) { atomic_store(&sEnabled, enabled); }
void PerformanceReset(void) {
    pthread_mutex_lock(&sLock);
    memset(&sSnapshot, 0, sizeof(sSnapshot));
    sNext = 0;
    sLastPresented = 0;
    pthread_mutex_unlock(&sLock);
}
uint64_t PerformancePpuTotal(void) {
    return sPpu[PERF_BG] + sPpu[PERF_OBJ] + sPpu[PERF_COMPOSE] + sPpu[PERF_PPU_OTHER];
}
void PerformancePpuLine(uint64_t start, uint64_t before) {
    uint64_t elapsed = PerformanceNow() - start, accounted = PerformancePpuTotal() - before;
    if (elapsed > accounted) sPpu[PERF_PPU_OTHER] += elapsed - accounted;
}
void PerformanceBeginFrame(void) { memset(sPpu, 0, sizeof(sPpu)); }
void PerformanceAddPpu(int stage, uint64_t start) { sPpu[stage] += PerformanceNow() - start; }
void PerformanceEndFrame(uint64_t frame, uint64_t gameNs, uint64_t hardwareNs, uint64_t publishNs, double budgetMs) {
    PerformanceFrame sample = { .frame = frame, .timeNs = PerformanceNow(), .budgetMs = budgetMs };
    uint64_t ppu = 0;
    for (int i = PERF_BG; i <= PERF_PPU_OTHER; i++) {
        sample.stage[i] = sPpu[i] / 1e6;
        ppu += sPpu[i];
    }
    sample.stage[PERF_GAME] = gameNs / 1e6;
    sample.stage[PERF_TIMING] = (hardwareNs > ppu ? hardwareNs - ppu : 0) / 1e6;
    sample.stage[PERF_PUBLISH] = publishNs / 1e6;
    for (int i = 0; i < PERF_STAGES; i++) sample.total += sample.stage[i];
    pthread_mutex_lock(&sLock);
    sSnapshot.frames[sNext] = sample;
    sNext = (sNext + 1) % PERF_SAMPLES;
    if (sSnapshot.count < PERF_SAMPLES) sSnapshot.count++;
    pthread_mutex_unlock(&sLock);
}
void PerformancePresentation(double cpuMs, double gpuMs) {
    if (!PerformanceEnabled()) return;
    pthread_mutex_lock(&sLock);
    if (cpuMs >= 0) {
        sSnapshot.cpuMs += (cpuMs - sSnapshot.cpuMs) / (sSnapshot.cpuCount < PERF_SAMPLES ? ++sSnapshot.cpuCount : PERF_SAMPLES);
    }
    if (gpuMs >= 0) {
        sSnapshot.gpuMs += (gpuMs - sSnapshot.gpuMs) / (sSnapshot.gpuCount < PERF_SAMPLES ? ++sSnapshot.gpuCount : PERF_SAMPLES);
    }
    pthread_mutex_unlock(&sLock);
}
void PerformancePresented(double seconds) {
    if (!PerformanceEnabled() || seconds <= 0) return;
    pthread_mutex_lock(&sLock);
    double interval = seconds - sLastPresented;
    if (sLastPresented > 0 && interval > 0 && interval < 1) {
        double fps = 1.0 / interval;
        sSnapshot.displayFps = sSnapshot.displayFps ? sSnapshot.displayFps * 0.9 + fps * 0.1 : fps;
    }
    sLastPresented = seconds;
    pthread_mutex_unlock(&sLock);
}
void PerformanceCopy(PerformanceSnapshot* out) {
    pthread_mutex_lock(&sLock);
    *out = sSnapshot;
    pthread_mutex_unlock(&sLock);
}
