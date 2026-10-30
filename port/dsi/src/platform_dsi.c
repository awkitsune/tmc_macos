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
DsiPicture gDsiPictures[DSI_PICTURES];

#define REPEAT_EVERY 64

static jmp_buf sResetPoint;
static volatile uint32_t sVBlanks; // DS VBlanks, counted by the IRQ
static uint32_t sLastVBlank;
static uint32_t sLeft;             // timer ticks when the game got its frame back

static void OnVBlank(void) {
    sVBlanks++;
    DsiPresentVBlank();
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

static unsigned long Us(uint64_t ticks) {
    return (unsigned long)timerTicks2usec((uint32_t)ticks);
}

static void ResetStats(void) {
    memset(&gDsiFrameStats, 0, sizeof(gDsiFrameStats));
    gDsiFrameStats.gameMin = gDsiFrameStats.workMin = gDsiFrameStats.presentMin = UINT32_MAX;
    gDsiFrameStats.first = gPlatformFrame;
    gDsiFrameStats.vblanks = sVBlanks;
}

static void Report(const char* what) {
    const DsiFrameStats* s = &gDsiFrameStats;
    const DsiPresentCounts* p = &gDsiPresentCounts;
    uint32_t n = s->frames;

    DsiLog("frames %lu-%lu (%s), game time per frame (us): min %lu avg %lu max %lu\n",
           (unsigned long)s->first, (unsigned long)(s->first + n - 1), what, Us(s->gameMin), Us(s->gameSum / n),
           Us(s->gameMax));
    DsiLog("frame work (lines, DMA, IRQs, prepare) (us): min %lu avg %lu max %lu\n", Us(s->workMin),
           Us(s->workSum / n), Us(s->workMax));
    DsiLog("present (us): min %lu avg %lu max %lu, ended by line %lu, %lu after VBlank\n", Us(s->presentMin),
           Us(s->presentSum / n), Us(s->presentMax), (unsigned long)s->lastLine, (unsigned long)s->presentLate);
    if (s->crcSum != 0) {
        DsiLog("shadow CRC (us): avg %lu; picture check avg %lu per 2 frames\n", Us(s->crcSum / n),
               Us(s->pictureSum / (n / 2)));
    }
    DsiLog("DS VBlanks %lu (%lu game frames over one)\n", (unsigned long)(sVBlanks - s->vblanks),
           (unsigned long)s->late);
    DsiLog("presenter so far: two-window lines %lu, wrap+OBJ-window lines %lu, wrapped sprites %lu, bitmap lines %lu\n",
           (unsigned long)p->twoWindowLines, (unsigned long)p->wrapLines, (unsigned long)p->wrappedSprites,
           (unsigned long)p->bitmapLines);
    DsiLog("presenter so far: per-line DISPCNT in %lu frames, other registers in %lu, up to %ld words\n",
           (unsigned long)p->dispcntFrames, (unsigned long)p->regFrames, (long)p->maxDmaWords);
    DsiLogFlush();
}

/*
 * One frame. The GBA frame's lines play out as on the Mac (frame.c: per line
 * VCOUNT and HBlank interrupts and HBlank DMA) into the shadows, and the
 * presenter prepares what they show; at the DS VBlank it shows them, then the
 * frame's VBlank half (VBlank DMA and interrupt) runs and the game goes on.
 * The buttons are latched at the start of the frame; A, B, Select, Start, the
 * directions, R and L have the GBA's bits.
 *
 * With DSI_CHECKS the first DSI_FRAME_RECORDS frames keep the shadows' CRC and
 * the game's task for the comparison with the Mac's TMC_HW_HASH, and the even
 * ones a capture of the picture for TMC_FRAME_HASH. Those frames take their
 * keys from tmc/input.txt (all up without one), as the Mac's from TMC_INPUT.
 */
void PlatformVBlank(void) {
    DsiFrameStats* s = &gDsiFrameStats;
    uint32_t frame = gPlatformFrame;
    uint32_t start = cpuGetTiming(), work, t, line, vblanks;

    if (frame != s->first) {
        Track(&s->gameMin, &s->gameMax, &s->gameSum, start - sLeft);
    }
    scanKeys();
    HwSetKeys(DSI_CHECKS && frame < DSI_FRAME_RECORDS ? DsiInputKeys(frame) : keysHeld() & 0x3FF);
    HwRunLines(NULL);
    DsiPresentPrepare();
    work = cpuGetTiming() - start;

    swiWaitForVBlank();
    vblanks = sVBlanks;
    if (frame != s->first && vblanks - sLastVBlank > 1) {
        s->late++;
    }
    sLastVBlank = vblanks;
    t = cpuGetTiming();
    DsiPresent();
    line = REG_VCOUNT;
    Track(&s->presentMin, &s->presentMax, &s->presentSum, cpuGetTiming() - t);
    if (line < 192) {
        s->presentLate++;
    } else if (line > s->lastLine) {
        s->lastLine = line;
    }
    if (DSI_CHECKS && frame < DSI_FRAME_RECORDS && frame % 2 == 0) {
        // Every 64th frame stays on screen two more DS frames, as a slow game
        // frame would, and the capture takes the last: the per-line registers
        // must hold over DS frames that present nothing new.
        if (frame % REPEAT_EVERY == 0) {
            swiWaitForVBlank();
            swiWaitForVBlank();
        }
        DsiCaptureStart();
    }

    t = cpuGetTiming();
    HwRunVBlank();
    work += cpuGetTiming() - t;
    Track(&s->workMin, &s->workMax, &s->workSum, work);
    if (EepromFlush() < 0) {
        DsiLog("save: writing %s failed\n", DsiSavePath());
        DsiLogFlush();
    }

    if (DSI_CHECKS && frame < DSI_FRAME_RECORDS) {
        DsiFrameRecord* r = &gDsiFrames[frame];
        HeadlessState state;

        t = cpuGetTiming();
        r->crc = HwShadowCrc();
        HeadlessGetState(&state);
        r->task = state.task;
        r->state = state.state;
        r->substate = state.substate;
        r->fade = state.fade;
        s->crcSum += cpuGetTiming() - t;
        if (frame % 2 == 1) {
            DsiPicture* picture = &gDsiPictures[frame / 2];

            t = cpuGetTiming();
            if (!DsiCaptureResult(frame - 1, picture)) {
                picture->crc = picture->border = UINT32_MAX;
            }
            s->pictureSum += cpuGetTiming() - t;
        }
    }
    gPlatformFrame++;
    s->frames++;
    if (DSI_CHECKS && gPlatformFrame == DSI_FRAME_RECORDS) {
        // The records in two numbers, to compare a DSi's log with melonDS's.
        DsiLog("records CRC32: frames %08lx, pictures %08lx\n",
               (unsigned long)HwCrc32(0, gDsiFrames, sizeof(gDsiFrames)),
               (unsigned long)HwCrc32(0, gDsiPictures, sizeof(gDsiPictures)));
        Report("with the checks");
        ResetStats();
    } else if (gPlatformFrame == (DSI_CHECKS ? 2 : 1) * DSI_FRAME_RECORDS) {
        Report(DSI_CHECKS ? "no checks, keys" : "keys");
        gDsiLogDone = 1;
        DC_FlushAll();
    }
    sLeft = cpuGetTiming();
}

_Noreturn void PlatformSoftReset(void) {
    longjmp(sResetPoint, 1);
}

_Noreturn void DsiRunGame(void) {
    ResetStats();
    irqSet(IRQ_VBLANK, OnVBlank);
    irqEnable(IRQ_VBLANK);
    cpuStartTiming(0);
    DsiPresentInit();
    if (DSI_CHECKS) {
        DsiInputLoad();
    }
    DsiLog("running the game\n");
    DsiLogFlush();
    HwSetKeys(0); // KEYINPUT is active low: all zero would be every key held
    setjmp(sResetPoint);
    AgbMain();
    DsiFatal("AgbMain returned");
}
