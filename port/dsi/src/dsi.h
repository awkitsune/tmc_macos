#ifndef DSI_H
#define DSI_H

/*
 * The DSi layer's own interface. Plain C types only: the files that include
 * libnds and the ones that include the game's headers both use it, and the two
 * must never meet in one file (port/dsi/PLAN.md, "ABI notes").
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Boot log: printed on the console and kept in gDsiLog (read through GDB). */
void DsiLog(const char* format, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void DsiFatal(const char* format, ...) __attribute__((format(printf, 1, 2)));
#define DSI_LOG_SIZE 4096
extern char gDsiLog[DSI_LOG_SIZE];
/* Set once the log is complete (after DSI_FRAME_RECORDS frames, twice that with DSI_CHECKS). */
extern volatile int gDsiLogDone;

/* The GBA IWRAM (32 KB): the game's IWRAM globals live here (ram_layout.ld). */
#define DSI_IWRAM_SIZE 0x8000
extern uint8_t gDsiIwram[DSI_IWRAM_SIZE];

/* The ROM copy, from the linker (rom_symbols.ld). */
extern uint8_t __rom_copy[];
extern uint8_t __rom_copy_end[];
#define DSI_ROM_BASE 0x08000000u

/* Zero the GBA EWRAM block and IWRAM, as the GBA's crt0 leaves them. */
void DsiClearGbaRam(void);

/*
 * Read the player's ROM into the copy and check it. False (with the reason
 * logged) if there is no usable ROM.
 */
bool DsiRomLoad(void);

/*
 * Patch the pointer words in the copy (rom_relocs.s). False if a count is off
 * or a word did not hold the GBA address the generator expects.
 */
bool DsiRomRelocate(void);

/* Make the GBA slot (0x08000000-0x0AFFFFFF) fault, so stray ROM pointers show. */
void DsiProtectGbaSlot(void);

/* SHA-1 (sha1.c). */
typedef struct {
    uint32_t state[5];
    uint64_t length;
    uint8_t buffer[64];
} DsiSha1;
void DsiSha1Init(DsiSha1* ctx);
void DsiSha1Update(DsiSha1* ctx, const void* data, size_t length);
void DsiSha1Final(DsiSha1* ctx, uint8_t digest[20]);


/* Run the game from AgbMain; a BIOS SoftReset comes back here. */
_Noreturn void DsiRunGame(void);

/*
 * Per game frame, for the first DSI_FRAME_RECORDS frames: the hardware
 * shadows' CRC (HwShadowCrc) and the game's task, as the Mac's TMC_HW_HASH
 * writes them (read through GDB, port/dsi/scripts/compare_frames.py).
 */
#ifndef DSI_FRAME_RECORDS
#define DSI_FRAME_RECORDS 1024
#endif
typedef struct {
    uint32_t crc;
    uint8_t task, state, substate, fade;
} DsiFrameRecord;
extern DsiFrameRecord gDsiFrames[DSI_FRAME_RECORDS];

/*
 * Scripted buttons for the recorded frames of a DSI_CHECKS build (input_dsi.c):
 * tmc/input.txt, frame steps of the Mac's TMC_INPUT syntax. No file, no keys.
 */
void DsiInputLoad(void);
uint16_t DsiInputKeys(uint32_t frame);

/*
 * Frame times in timer ticks (33.514 MHz), logged for frames 0-1023 (with the
 * checks above) and 1024-2047 (without).
 */
typedef struct {
    uint32_t first, frames;          // the window: frames first .. first + frames - 1
    uint32_t vblanks, late;          // DS VBlanks at its start; game frames that took over one
    uint32_t gameMin, gameMax;      // the game's own work between frames
    uint32_t workMin, workMax;      // frame.c: lines, DMA, interrupt handlers; the presenter's prepare
    uint32_t presentMin, presentMax; // DsiPresent, in the DS VBlank
    uint32_t presentLate;           // presents that ended after the VBlank
    uint32_t lastLine;              // the latest DS line a present ended on (192-262)
    uint64_t gameSum, workSum, crcSum, presentSum, pictureSum;
} DsiFrameStats;
extern DsiFrameStats gDsiFrameStats;

/*
 * The presenter (present.c): the game's shadows on the top screen. Init once;
 * per frame, Prepare after the frame's lines (HwRunLines) and Present in the
 * DS VBlank, before the frame's VBlank half (HwRunVBlank). PresentVBlank runs
 * in the VBlank interrupt of every DS frame, presented or not.
 */
void DsiPresentInit(void);
void DsiPresentPrepare(void);
void DsiPresent(void);
/* From the VBlank interrupt: re-arm the shown frame's per-line registers. */
void DsiPresentVBlank(void);

/* What the presenter could not show as the Mac does, and what it used. */
typedef struct {
    uint32_t twoWindowLines; // both game windows on one line: no side columns
    uint32_t wrapLines;      // a wrapped game window with the OBJ window on
    uint32_t wrappedSprites; // 128-line sprites the DS wraps differently
    uint32_t bitmapLines;    // GBA bitmap modes (not shown)
    uint32_t dispcntFrames;  // frames whose DISPCNT changes between DS lines
    uint32_t regFrames;      // frames with other registers changing between lines
    int32_t maxDmaWords;     // most register words written per HBlank
} DsiPresentCounts;
extern DsiPresentCounts gDsiPresentCounts;

/*
 * The picture check: display capture of a presented frame into bank D, then
 * its 240x160 picture's CRC (as the Mac's TMC_FRAME_HASH) and the number of
 * border pixels unlike the top-left one. Even frames of the first
 * DSI_FRAME_RECORDS (a capture takes the next DS frame) go to gDsiPictures;
 * with -DDSI_CAPTURE_DUMP=n, frame n's whole capture is kept in gDsiCaptureDump.
 */
typedef struct {
    uint32_t crc, border;
} DsiPicture;
#define DSI_PICTURES (DSI_FRAME_RECORDS / 2)
extern DsiPicture gDsiPictures[DSI_PICTURES];
void DsiCaptureStart(void);
bool DsiCaptureResult(uint32_t frame, DsiPicture* picture);

#endif // DSI_H
