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

/* Write a framebuffer as an RGB PNG. Returns 0 on success. */
int PpuWritePng(const uint16_t* framebuffer, const char* path);
int PpuWritePngSize(const uint16_t* framebuffer, int width, int height, const char* path);

#endif // PORT_PPU_H
