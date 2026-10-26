/*
 * The hardware side of a frame, shared by the headless runner and the macOS front
 * end: latch the buttons, then play out the display timing of one frame. The
 * picture is drawn a scanline at a time with the interrupts and HBlank DMA the GBA
 * would run between lines, so per-line effects (HBlank DMA tables written to
 * scroll, affine, window or BG control registers) show as on hardware.
 *
 * Debugging (environment):
 *   TMC_PPU_LEGACY=1      draw the whole frame before any mid-frame change, as the
 *                         renderer did before scanline timing (for comparisons)
 *   TMC_PPU_COMPARE=1     draw every frame both ways from the same game state and
 *                         print the frames (and lines) where the pictures differ
 *   TMC_LINE_LOG=f,f,...  for those frames, print each line's display registers
 *                         that differ from line 0 (to stderr)
 *   TMC_LINE_STATS=1      for every frame whose display registers change between
 *                         lines, print which ones changed and on how many lines
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/hw.h"
#include "port/platform.h"
#include "port/ppu.h"
#include "port/ppucapture.h"

#define REG16(offset) (*(volatile uint16_t*)(gHwIo + (offset)))
#define REG_DISPSTAT REG16(0x004)
#define REG_VCOUNT REG16(0x006)
#define REG_KEYINPUT REG16(0x130)

#define DISPSTAT_VBLANK 0x0001
#define DISPSTAT_HBLANK 0x0002
#define DISPSTAT_VCOUNT 0x0004
#define DISPSTAT_VBLANK_INTR 0x0008
#define DISPSTAT_HBLANK_INTR 0x0010
#define DISPSTAT_VCOUNT_INTR 0x0020
#define INTR_VBLANK 0x0001
#define INTR_HBLANK 0x0002
#define INTR_VCOUNT 0x0004

#define LINES_PER_FRAME 228
#define DISPLAY_REGS HW_LINE_REGS // DISPCNT .. BLDY, the registers a line is drawn from

// DISPSTAT and VCOUNT change every line by design; the log and stats skip them.
static int IsStatusReg(int offset) {
    return offset == 0x04 || offset == 0x06;
}

void HwRaiseInterrupt(uint16_t flag);

uint8_t (*gHwLineCapture)[HW_LINE_REGS];

void HwSetKeys(uint16_t held) {
    REG_KEYINPUT = 0x03FF & ~held; // active low
}

static struct {
    int ready;
    int legacy;
    int compare;
    int stats;
    uint64_t logFrames[64];
    int logCount;
} sDebug;

static void ReadDebugEnv(void) {
    const char* log = getenv("TMC_LINE_LOG");

    sDebug.ready = 1;
    sDebug.legacy = getenv("TMC_PPU_LEGACY") != NULL;
    sDebug.compare = getenv("TMC_PPU_COMPARE") != NULL;
    sDebug.stats = getenv("TMC_LINE_STATS") != NULL;
    while (log != NULL && *log && sDebug.logCount < 64) {
        char* end;
        sDebug.logFrames[sDebug.logCount++] = strtoull(log, &end, 10);
        log = *end == ',' ? end + 1 : NULL;
    }
}

static int LogThisFrame(void) {
    for (int i = 0; i < sDebug.logCount; i++) {
        if (sDebug.logFrames[i] == gPlatformFrame) {
            return 1;
        }
    }
    return 0;
}

// Per-frame record of display registers changing between lines.
typedef struct {
    int log, stats;
    uint8_t first[DISPLAY_REGS]; // line 0's registers
    uint8_t prev[DISPLAY_REGS];
    int changedLines[DISPLAY_REGS / 2]; // lines whose value differs from the line before
} LineWatch;

static void WatchLine(LineWatch* w, int line) {
    if (line == 0) {
        memcpy(w->first, gHwIo, DISPLAY_REGS);
        memcpy(w->prev, gHwIo, DISPLAY_REGS);
        memset(w->changedLines, 0, sizeof(w->changedLines));
        if (w->log) {
            fprintf(stderr, "frame %llu line 0:", (unsigned long long)gPlatformFrame);
            for (int r = 0; r < DISPLAY_REGS; r += 2) {
                fprintf(stderr, " %02x=%04x", r, w->first[r] | (w->first[r + 1] << 8));
            }
            fprintf(stderr, "\n");
        }
        return;
    }
    if (w->log) {
        int shown = 0;
        for (int r = 0; r < DISPLAY_REGS; r += 2) {
            if (!IsStatusReg(r) && (gHwIo[r] != w->first[r] || gHwIo[r + 1] != w->first[r + 1])) {
                if (!shown++) {
                    fprintf(stderr, "frame %llu line %d:", (unsigned long long)gPlatformFrame, line);
                }
                fprintf(stderr, " %02x=%04x", r, gHwIo[r] | (gHwIo[r + 1] << 8));
            }
        }
        if (shown) {
            fprintf(stderr, "\n");
        }
    }
    for (int r = 0; r < DISPLAY_REGS; r += 2) {
        if (!IsStatusReg(r) && (gHwIo[r] != w->prev[r] || gHwIo[r + 1] != w->prev[r + 1])) {
            w->changedLines[r / 2]++;
        }
    }
    memcpy(w->prev, gHwIo, DISPLAY_REGS);
}

static void ReportFrame(const LineWatch* w) {
    int shown = 0;

    for (int r = 0; r < DISPLAY_REGS / 2; r++) {
        if (w->changedLines[r] == 0) {
            continue;
        }
        if (!shown++) {
            fprintf(stderr, "frame %llu mid-frame:", (unsigned long long)gPlatformFrame);
        }
        fprintf(stderr, " %02x/%d", r * 2, w->changedLines[r]);
    }
    if (shown) {
        fprintf(stderr, "\n");
    }
}

// TMC_PPU_COMPARE: report where the scanline picture differs from the whole frame.
static void CompareFrames(const uint16_t* scanline, const uint16_t* whole) {
    int first = -1, last = -1, count = 0;

    for (int line = 0; line < PPU_HEIGHT; line++) {
        if (memcmp(&scanline[line * PPU_WIDTH], &whole[line * PPU_WIDTH], PPU_WIDTH * sizeof(*whole)) != 0) {
            first = first < 0 ? line : first;
            last = line;
            count++;
        }
    }
    if (count != 0) {
        fprintf(stderr, "frame %llu differs from whole-frame drawing on %d lines (%d-%d)\n",
                (unsigned long long)gPlatformFrame, count, first, last);
    }
}

void HwRunLines(uint16_t* framebuffer) {
#ifndef PLATFORM_DSI
    static uint16_t sWhole[PPU_WIDTH * PPU_HEIGHT], sScratch[PPU_WIDTH * PPU_HEIGHT];
#else
    // No software PPU (and no environment) on the DSi: the comparisons are off.
    uint16_t *sWhole = NULL, *sScratch = NULL;
#endif
    int lyc = REG_DISPSTAT >> 8;
    LineWatch watch;
    uint16_t* draw = framebuffer;

    if (!sDebug.ready) {
        ReadDebugEnv();
    }
    if (sDebug.compare) {
        draw = framebuffer != NULL ? framebuffer : sScratch;
        PpuRenderFrame(sWhole);
    }
    watch.log = sDebug.logCount != 0 && LogThisFrame();
    watch.stats = sDebug.stats;
    if (sDebug.legacy && framebuffer != NULL) {
        PpuRenderFrame(framebuffer);
        draw = NULL;
    }

    // Drawing line by line costs no more than a whole frame: each line reads its
    // registers once, and the HBlank steps are a few compares when nothing is armed.
    PpuBeginFrame();
    bool compactCapture = PpuCompactCaptureActive();
    bool memoryUnchanged = false;
    for (int line = 0; line < PPU_HEIGHT; line++) {
        REG_VCOUNT = line;
        if (line == lyc) {
            REG_DISPSTAT |= DISPSTAT_VCOUNT;
            if (REG_DISPSTAT & DISPSTAT_VCOUNT_INTR) {
                HwRaiseInterrupt(INTR_VCOUNT);
                memoryUnchanged = false;
            }
        } else {
            REG_DISPSTAT &= ~DISPSTAT_VCOUNT;
        }
        if (watch.log || watch.stats) {
            WatchLine(&watch, line);
        }
        if (gHwLineCapture != NULL) {
            memcpy(gHwLineCapture[line], gHwIo, HW_LINE_REGS);
        }
        if (compactCapture && memoryUnchanged) PpuCompactMemoryUnchanged();
        PpuRenderLine(line, draw != NULL ? &draw[line * PPU_WIDTH] : NULL);

        REG_DISPSTAT |= DISPSTAT_HBLANK;
        bool hblankInterrupt = (REG_DISPSTAT & DISPSTAT_HBLANK_INTR) != 0;
        if (hblankInterrupt) {
            HwRaiseInterrupt(INTR_HBLANK);
        }
        bool dmaMayWriteMemory = false;
        HwDmaHBlank(compactCapture ? &dmaMayWriteMemory : NULL);
        // No game code runs between lines unless one of these occurs. On the
        // game thread VRAM, palette and OAM must therefore be unchanged; a
        // VCOUNT interrupt at the next line invalidates this proof before its
        // state is captured.
        memoryUnchanged = !hblankInterrupt && !dmaMayWriteMemory;
        REG_DISPSTAT &= ~DISPSTAT_HBLANK;
    }
    if (watch.stats) {
        ReportFrame(&watch);
    }
    if (sDebug.compare) {
        CompareFrames(draw, sWhole);
    }
}

void HwRunVBlank(void) {
    int lyc;

    REG_VCOUNT = PPU_HEIGHT;
    REG_DISPSTAT = (REG_DISPSTAT & ~DISPSTAT_VCOUNT) | DISPSTAT_VBLANK;
    HwDmaVBlank();
    if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR) {
        HwRaiseInterrupt(INTR_VBLANK);
    }
    // A VCOUNT match inside VBlank (none in this game) comes after the VBlank handler.
    lyc = REG_DISPSTAT >> 8;
    if (lyc >= PPU_HEIGHT && lyc < LINES_PER_FRAME && (REG_DISPSTAT & DISPSTAT_VCOUNT_INTR)) {
        REG_VCOUNT = lyc;
        REG_DISPSTAT |= DISPSTAT_VCOUNT;
        HwRaiseInterrupt(INTR_VCOUNT);
    }
    REG_DISPSTAT &= ~(DISPSTAT_VBLANK | DISPSTAT_VCOUNT);
    REG_VCOUNT = 0;
}

void HwRunFrame(uint16_t* framebuffer) {
    HwRunLines(framebuffer);
    HwRunVBlank();
}
