#ifndef PORT_PERFORMANCE_H
#define PORT_PERFORMANCE_H

#include <stdbool.h>
#include <stdint.h>

enum { PERF_GAME, PERF_BG, PERF_OBJ, PERF_COMPOSE, PERF_PPU_OTHER, PERF_TIMING, PERF_PUBLISH, PERF_STAGES };
#define PERF_SAMPLES 120

typedef struct {
    double stage[PERF_STAGES]; // milliseconds per frame
    double total, budgetMs;
    uint64_t frame, timeNs;
} PerformanceFrame;

typedef struct {
    PerformanceFrame frames[PERF_SAMPLES];
    int count;
    double cpuMs, gpuMs, displayFps;
    int cpuCount, gpuCount;
} PerformanceSnapshot;

bool PerformanceEnabled(void);
uint64_t PerformanceNow(void);
void PerformanceSetEnabled(bool enabled);
void PerformanceReset(void);
void PerformanceAddPpu(int stage, uint64_t start);
uint64_t PerformancePpuTotal(void);
void PerformancePpuLine(uint64_t start, uint64_t before);
void PerformanceBeginFrame(void);
void PerformanceEndFrame(uint64_t frame, uint64_t gameNs, uint64_t hardwareNs, uint64_t publishNs, double budgetMs);
void PerformancePresentation(double cpuMs, double gpuMs);
void PerformancePresented(double seconds);
void PerformanceCopy(PerformanceSnapshot* out);

#endif
