/*
 * port/platform.h on the DSi: the game's frame boundary and soft reset.
 */

#include <nds.h>
#include <setjmp.h>
#include <string.h>

#include "port/headless_state.h"
#include "port/hw.h"
#include "port/platform.h"
#include "port/ppu.h"

#include "dsi.h"

void AgbMain(void);

uint64_t gPlatformFrame;

DsiFrameRecord gDsiFrames[DSI_FRAME_RECORDS];
DsiFrameStats gDsiFrameStats;

static jmp_buf sResetPoint;
static volatile uint32_t sVBlanks; // DS VBlanks, counted by the IRQ
static uint32_t sLastVBlank;
static uint32_t sLeft;             // timer ticks when the game got its frame back

static void OnVBlank(void) {
    sVBlanks++;
}

static void Track(uint32_t* min, uint32_t* max, uint64_t* sum, uint32_t value) {
    if (value < *min) {
        *min = value;
    }
    if (value > *max) {
        *max = value;
    }
    *sum += value;
}

static void Report(void) {
    const DsiFrameStats* s = &gDsiFrameStats;
    uint32_t n = s->frames;

    DsiLog("frames 1-%lu, game time per frame (us): min %lu avg %lu max %lu\n", (unsigned long)n,
           (unsigned long)timerTicks2usec(s->gameMin), (unsigned long)timerTicks2usec(s->gameSum / n),
           (unsigned long)timerTicks2usec(s->gameMax));
    DsiLog("frame work (lines, DMA, IRQs) (us): min %lu avg %lu max %lu\n",
           (unsigned long)timerTicks2usec(s->workMin), (unsigned long)timerTicks2usec(s->workSum / n),
           (unsigned long)timerTicks2usec(s->workMax));
    DsiLog("shadow CRC (us): avg %lu; DS VBlanks %lu (%lu game frames over one)\n",
           (unsigned long)timerTicks2usec(s->crcSum / n), (unsigned long)s->vblanks, (unsigned long)s->late);
}

/*
 * One frame: wait for the DS VBlank, then play out the GBA frame as the Mac
 * does (frame.c: per line VCOUNT and HBlank interrupts and HBlank DMA, then
 * VBlank DMA and interrupt), into the shadows. The presenter (milestone 5)
 * shows them. No input yet: all keys up, as the Mac headless run without a
 * script. The first DSI_FRAME_RECORDS frames keep the shadows' CRC and the
 * game's task for the comparison with the Mac's TMC_HW_HASH.
 */
void PlatformVBlank(void) {
    DsiFrameStats* s = &gDsiFrameStats;
    uint32_t start = cpuGetTiming(), work, crc;
    uint32_t vblanks;

    if (gPlatformFrame != 0) {
        Track(&s->gameMin, &s->gameMax, &s->gameSum, start - sLeft);
    }
    swiWaitForVBlank();
    vblanks = sVBlanks;
    if (gPlatformFrame != 0 && vblanks - sLastVBlank > 1) {
        s->late++;
    }
    sLastVBlank = vblanks;

    DsiPresent();
    start = cpuGetTiming();
    HwSetKeys(0);
    HwRunFrame(NULL);
    work = cpuGetTiming();
    Track(&s->workMin, &s->workMax, &s->workSum, work - start);

    if (gPlatformFrame < DSI_FRAME_RECORDS) {
        DsiFrameRecord* r = &gDsiFrames[gPlatformFrame];
        HeadlessState state;

        r->crc = HwShadowCrc();
        HeadlessGetState(&state);
        r->task = state.task;
        r->state = state.state;
        r->substate = state.substate;
        r->fade = state.fade;
        crc = cpuGetTiming();
        s->crcSum += crc - work;
    }
    gPlatformFrame++;
    s->frames = gPlatformFrame;
    s->vblanks = sVBlanks;
    if (gPlatformFrame == DSI_FRAME_RECORDS) {
        Report();
        gDsiLogDone = 1;
        DC_FlushAll();
    }
    sLeft = cpuGetTiming();
}

_Noreturn void PlatformSoftReset(void) {
    longjmp(sResetPoint, 1);
}

_Noreturn void DsiRunGame(void) {
    memset(&gDsiFrameStats, 0, sizeof(gDsiFrameStats));
    gDsiFrameStats.gameMin = gDsiFrameStats.workMin = UINT32_MAX;
    irqSet(IRQ_VBLANK, OnVBlank);
    irqEnable(IRQ_VBLANK);
    cpuStartTiming(0);
    DsiLog("running the game\n");
    HwSetKeys(0); // KEYINPUT is active low: all zero would be every key held
    setjmp(sResetPoint);
    AgbMain();
    DsiFatal("AgbMain returned");
}

void DsiPresent(void) {
    // Milestone 5: shadows to engine A (port/dsi/PLAN.md, "Hardware layer").
}

// frame.c drives the Mac's software PPU line by line. The DSi has none: the
// presenter translates the shadows instead, so these only keep frame.c's calls.
void PpuBeginFrame(void) {
}

void PpuRenderLine(int line, uint16_t* out) {
    (void)line;
    (void)out;
}

void PpuRenderFrame(uint16_t* framebuffer) {
    (void)framebuffer;
    DsiFatal("PpuRenderFrame: no software PPU on the DSi");
}

void PpuIoWritten(uint32_t offset, uint32_t size) {
    (void)offset;
    (void)size;
}
