/*
 * port/platform.h on the DSi: the game's frame boundary and soft reset.
 */

#include <nds.h>
#include <setjmp.h>
#include <string.h>

#include "port/headless_state.h"
#include "port/hw.h"
#include "port/platform.h"
#include "port/dsvoices.h"
#include "port/profile.h"
#include "port/ppu.h"
#include "port/view.h"

#include "dsi.h"

void AgbMain(void);

uint64_t gPlatformFrame;

DsiFrameRecord gDsiFrames[DSI_FRAME_RECORDS];
DsiFrameStats gDsiFrameStats;
DsiPicture gDsiPictures[DSI_PICTURES];
uint32_t gProfileTicks[PROFILE_PARTS], gProfileCount[PROFILE_PARTS];

uint32_t ProfileClock(void) {
    return cpuGetTiming();
}

#define REPEAT_EVERY 64

static jmp_buf sResetPoint;
static volatile uint32_t sVBlanks; // DS VBlanks, counted by the IRQ
static uint32_t sLastVBlank;
static uint32_t sLeft;             // timer ticks when the game got its frame back
static volatile bool sFrameWork;   // in PlatformVBlank: the frame's lines, VBlank, present
static volatile uint32_t sFrameVBlank; // sVBlanks at the last game frame (the stall check)
static volatile bool sStopped;         // in the game's Sleep or the lid's: no frames are due

/*
 * TMC runs m4aSoundMain from its VCount interrupt, so on the GBA the music
 * goes on through a frame that takes several (a room load). Here interrupts
 * come at the frame boundary; a DS VBlank that finds the game still in its
 * frame, with no sound frame since the last one, runs the sound as the GBA's
 * interrupt would, if the game has that interrupt on (IME, IE, DISPSTAT).
 * The m4a engine guards itself against this, as on the GBA (its ident locks).
 * Not with DSI_CHECKS, whose records follow the Mac frame by frame.
 */
static bool SoundInterruptOn(void) {
    return !DSI_CHECKS && !sFrameWork && (gHwIo[0x208] & 1) && (gHwIo[0x200] & 4) && (gHwIo[0x004] & 0x20);
}

/*
 * No game frame for STALL_VBLANKS DS frames outside a sleep: the game or the
 * port is stuck, where no exception says so. The card cannot be written from
 * the interrupt (its replies come through the FIFO interrupt, and the code it
 * stopped may be in the middle of a write), so the log goes to the bottom
 * screen, with where the interrupt came in: the BIOS's IRQ entry pushed r0-r3,
 * r12 and lr (the interrupted PC + 4) at the top of the IRQ stack.
 */
#define STALL_VBLANKS (10 * 60)

extern uint8_t __sp_irq[];

static void Stall(void) {
    const volatile uint32_t* frame = (const volatile uint32_t*)((uintptr_t)__sp_irq - 24);

    DsiLog("stall: no frame for %d s\nafter frame %lu\n", STALL_VBLANKS / 60, (unsigned long)gPlatformFrame);
    DsiLog("interrupted at %08lx\n", (unsigned long)(frame[5] - 4));
    DsiLog("r0 %08lx r1 %08lx\nr2 %08lx r3 %08lx\n", (unsigned long)frame[0], (unsigned long)frame[1],
           (unsigned long)frame[2], (unsigned long)frame[3]);
    DsiShowLog();
}

static void OnVBlank(void) {
    sVBlanks++;
    if (gDsiHalted) {
        return;
    }
    if (!sStopped && sVBlanks - sFrameVBlank == STALL_VBLANKS) {
        Stall();
    }
    DsiPresentVBlank();
    DsiSoundVBlank(SoundInterruptOn());
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

static void ReportStacks(void) {
    uint32_t user, irq, svc;

    DsiStacksUnused(&user, &irq, &svc);
    DsiLog("  unused stack (bytes): user %lu, irq %lu, svc %lu\n", (unsigned long)user, (unsigned long)irq,
           (unsigned long)svc);
}

static void FlushSave(bool now) {
    if (EepromFlush(now) < 0) {
        DsiLog("save: writing %s failed\n", DsiSavePath());
        DsiLogFlush();
    }
}

/*
 * The lid closed. The ARM7's own sleep on the lid is off (DsiRunGame), as it
 * would come at any point, in the middle of a frame or of a save; here the save
 * is on the card first and the sound is stopped (it starts again with the
 * next frame). Sleep is allowed just for the request, and the DSi wakes when
 * the lid opens.
 */
static void SleepUntilLidOpens(void) {
    bool stopped = sStopped;

    FlushSave(true);
    DsiSoundStop();
    sStopped = true;
    enableSleep();
    systemSleep();
    disableSleep();
    sFrameVBlank = sVBlanks;
    sStopped = stopped;
}

void DsiStop(void) {
    uint16_t keycnt = gHwIo[0x132] | (gHwIo[0x133] << 8); // REG_KEYCNT, as SetSleepMode left it
    uint16_t wake = keycnt & 0x3FF;

    uint32_t start = sVBlanks;

    FlushSave(true);
    DsiSoundStop(); // the channels would loop the last frames
    sStopped = true;
    // In the log at once, so a crash after the wake shows where it came from.
    DsiLog("sleep (the game's Stop) after frame %lu\n", (unsigned long)gPlatformFrame);
    DsiLogFlush();
    powerOff(PM_BACKLIGHT_TOP | PM_BACKLIGHT_BOTTOM);
    for (;;) {
        uint16_t held;

        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_LID) {
            SleepUntilLidOpens();
        }
        held = keysHeld() & 0x3FF;
        // Bit 15: all of the keys (the game's L+R+Select), else any of them.
        if (wake != 0 && ((keycnt & 0x8000) ? (held & wake) == wake : (held & wake) != 0)) {
            break;
        }
    }
    powerOn(PM_BACKLIGHT_TOP | PM_BACKLIGHT_BOTTOM);
    DsiLog("wake after %lu DS frames\n", (unsigned long)(sVBlanks - start));
    ReportStacks();
    DsiLogFlush();
    sFrameVBlank = sVBlanks;
    sStopped = false;
}

static unsigned long Us(uint64_t ticks) {
    return (unsigned long)timerTicks2usec((uint32_t)ticks);
}

static void ResetStats(void) {
    memset(&gDsiFrameStats, 0, sizeof(gDsiFrameStats));
    gDsiFrameStats.gameMin = gDsiFrameStats.workMin = gDsiFrameStats.presentMin = UINT32_MAX;
    gDsiFrameStats.first = gPlatformFrame;
    gDsiFrameStats.vblanks = sVBlanks;
    memset(gProfileTicks, 0, sizeof(gProfileTicks));
    memset(gProfileCount, 0, sizeof(gProfileCount));
}

static void ReportRooms(void);
static void ReportParts(void);

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
    DsiLog("present: wide-map frames %lu, map clashes %lu, full-width window lines %lu\n",
           (unsigned long)p->wideFrames, (unsigned long)p->wideMapClashes, (unsigned long)p->fullWidthLines);
    ReportParts();
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

    sFrameWork = true;
    if (frame != s->first) {
        Track(&s->gameMin, &s->gameMax, &s->gameSum, start - sLeft);
    }
    scanKeys();
    if (keysDown() & KEY_LID) {
        SleepUntilLidOpens();
    }
    HwSetKeys(DSI_CHECKS && frame < DSI_FRAME_RECORDS ? DsiInputKeys(frame) : keysHeld() & 0x3FF);
    HwRunLines(NULL);
    t = cpuGetTiming();
    s->linesSum += t - start;
    DsiPresentPrepare();
    s->prepareSum += cpuGetTiming() - t;
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
    s->vblankSum += cpuGetTiming() - t;
    work += cpuGetTiming() - t;
    Track(&s->workMin, &s->workMax, &s->workSum, work);
    FlushSave(false);

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
#if DSI_SOUND_HW
        {
            uint32_t crc, runs;

            DsVoicesCrc(&crc, &runs);
            DsiLog("sound registers CRC32 %08lx after %lu frames\n", (unsigned long)crc, (unsigned long)runs);
        }
#endif
        Report("with the checks");
        ResetStats();
    } else if (gPlatformFrame == (DSI_CHECKS ? 2 : 1) * DSI_FRAME_RECORDS) {
        Report(DSI_CHECKS ? "no checks, keys" : "keys");
        gDsiLogDone = 1;
        DC_FlushAll();
    } else {
        ReportRooms();
    }
    sLeft = cpuGetTiming();
    sFrameVBlank = sVBlanks;
    sFrameWork = false;
}

// Microseconds per frame of a part.
static unsigned long Part(uint64_t ticks) {
    return (unsigned long)timerTicks2usec((uint32_t)(ticks / gDsiFrameStats.frames));
}

/*
 * The parts of work, per frame (us): the lines (the loop, the interrupt handlers
 * and HBlank DMA during them), the presenter's prepare and the VBlank half;
 * within them the game's interrupt handlers, the timed DMA (transfers per frame
 * in brackets), and the sound by stage (which the DS VBlank also runs when a
 * frame is late).
 */
static void ReportParts(void) {
    const DsiFrameStats* s = &gDsiFrameStats;
    const uint32_t* t = gProfileTicks;
    uint32_t n = s->frames;

    DsiLog("  work: lines %lu, prepare %lu, vblank %lu; irq hblank %lu (%lu), vcount %lu, vblank %lu, other %lu; "
           "dma hblank %lu (%lu), vblank %lu\n",
           Part(s->linesSum), Part(s->prepareSum), Part(s->vblankSum), Part(t[PROFILE_IRQ_HBLANK]),
           (unsigned long)(gProfileCount[PROFILE_IRQ_HBLANK] / n), Part(t[PROFILE_IRQ_VCOUNT]),
           Part(t[PROFILE_IRQ_VBLANK]), Part(t[PROFILE_IRQ_OTHER]), Part(t[PROFILE_DMA_HBLANK]),
           (unsigned long)(gProfileCount[PROFILE_DMA_HBLANK] / n), Part(t[PROFILE_DMA_VBLANK]));
#if DSI_SOUND_HW
    DsiLog("  sound (DS channels): sequencer %lu, cgb %lu, voices %lu, send %lu (%lu runs; %lu sends in all)\n",
           Part(t[PROFILE_SOUND_SEQ]), Part(t[PROFILE_SOUND_CGB]), Part(t[PROFILE_SOUND_MIX]),
           Part(t[PROFILE_SOUND_OUT]), (unsigned long)gProfileCount[PROFILE_SOUND_MIX], (unsigned long)gDsiSoundSends);
#else
    DsiLog("  sound (mixed): sequencer %lu, cgb %lu, mixer %lu, psg %lu, out %lu (%lu runs)\n", Part(t[PROFILE_SOUND_SEQ]),
           Part(t[PROFILE_SOUND_CGB]), Part(t[PROFILE_SOUND_MIX]), Part(t[PROFILE_SOUND_PSG]),
           Part(t[PROFILE_SOUND_OUT]), (unsigned long)gProfileCount[PROFILE_SOUND_MIX]);
#endif
    ReportStacks();
}

/*
 * After the start-up reports, one line per room stretch (at least 2 s) or
 * 30 s, for comparing rooms and the screen modes: the times in us, and the
 * frames that took more than one DS frame.
 */
static void ReportRooms(void) {
    static int sArea = -1, sRoom = -1;
    const DsiFrameStats* s = &gDsiFrameStats;
    HeadlessState state;
    uint32_t n = s->frames;

    if (gPlatformFrame < (DSI_CHECKS ? 2 : 1) * DSI_FRAME_RECORDS) {
        return;
    }
    HeadlessGetState(&state);
    if (n > 0 && ((state.area != sArea || state.room != sRoom) ? n >= 120 : n >= 1800)) {
        DsiLog("%02x:%02x %s %lu frames: game %lu/%lu, work %lu/%lu, present %lu/%lu (avg/max us), %lu late\n",
               sArea, sRoom, gPortScreenW == 256 ? "full" : "bordered", (unsigned long)n, Us(s->gameSum / n),
               Us(s->gameMax), Us(s->workSum / n), Us(s->workMax), Us(s->presentSum / n), Us(s->presentMax),
               (unsigned long)s->late);
        ReportParts();
        DsiLogFlush();
        ResetStats();
    } else if (state.area != sArea || state.room != sRoom) {
        ResetStats();
    }
    sArea = state.area;
    sRoom = state.room;
}

_Noreturn void PlatformSoftReset(void) {
    // The game starts its sound over; nothing may run it from the interrupt
    // until its first frame.
    DsiSoundStop();
    longjmp(sResetPoint, 1);
}

_Noreturn void DsiRunGame(void) {
    ResetStats();
    disableSleep(); // the lid is handled at the frame boundary (SleepUntilLidOpens)
    irqSet(IRQ_VBLANK, OnVBlank);
    irqEnable(IRQ_VBLANK);
    cpuStartTiming(0);
    DsiPresentInit();
    DsiSettingsApply();
    if (DSI_CHECKS) {
        DsiInputLoad();
    }
    sFrameVBlank = sVBlanks;
    DsiLog("running the game\n");
    DsiLogFlush();
    HwSetKeys(0); // KEYINPUT is active low: all zero would be every key held
    setjmp(sResetPoint);
    AgbMain();
    DsiFatal("AgbMain returned");
}
