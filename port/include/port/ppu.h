#ifndef PORT_PPU_H
#define PORT_PPU_H

#include <stdint.h>

#define PPU_WIDTH 240
#define PPU_HEIGHT 160

/*
 * Software GBA picture processor: renders the frame described by the hardware
 * buffers (gHwIo, gHwPltt, gHwVram, gHwOam) as 15-bit BGR colours, one u16 per
 * pixel. This is the reference renderer; the Metal front end displays its output.
 */
void PpuRenderFrame(uint16_t* framebuffer);

/*
 * Scanline interface, for frames whose registers change between lines (frame.c):
 * PpuBeginFrame latches the affine reference points at the start of a frame,
 * PpuRenderLine draws one line (out may be NULL to only advance the line state),
 * and PpuIoWritten tells the PPU that registers in [offset, offset + size) were
 * written mid-frame, which reloads the affine reference points they cover.
 * PpuRenderFrame is all 160 lines with no changes in between.
 */
void PpuBeginFrame(void);
void PpuRenderLine(int line, uint16_t* out);
void PpuIoWritten(uint32_t offset, uint32_t size);

/*
 * Debugging: which layers are drawn (bits 0-3 BG0-BG3, bit 4 OBJ; default all,
 * 0x1F). A hidden layer is left out as if transparent; the OBJ window still
 * applies. Changes nothing while all bits are set.
 */
#define PPU_LAYERS_ALL 0x1F
void PpuSetLayerMask(unsigned mask);
unsigned PpuLayerMask(void);

/* Write a framebuffer as an RGB PNG. Returns 0 on success. */
int PpuWritePng(const uint16_t* framebuffer, const char* path);
int PpuWritePngSize(const uint16_t* framebuffer, int width, int height, const char* path);

#endif // PORT_PPU_H
