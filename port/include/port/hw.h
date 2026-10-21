#ifndef PORT_HW_H
#define PORT_HW_H

/*
 * Host-side stand-ins for GBA hardware, used when building with PLATFORM_NATIVE.
 *
 * The game addresses I/O registers, palette RAM, VRAM and OAM at fixed GBA
 * addresses. Natively those regions are plain host buffers; the gba/ headers
 * redirect their base addresses here, and the renderer reads them back.
 */

#include <stdint.h>

#define HW_IO_SIZE 0x400
#define HW_PLTT_SIZE 0x400
#define HW_VRAM_SIZE 0x18000
#define HW_OAM_SIZE 0x400

extern uint8_t gHwIo[HW_IO_SIZE];
extern uint8_t gHwPltt[HW_PLTT_SIZE];
extern uint8_t gHwVram[HW_VRAM_SIZE];
extern uint8_t gHwOam[HW_OAM_SIZE];

/* BIOS-reserved IWRAM words (0x3007FF0 / 0x3007FF8 / 0x3007FFC on hardware). */
extern struct SoundInfo* gHwSoundInfoPtr;
extern uint16_t gHwIntrCheck;
extern void* gHwIntrVector;

/*
 * DMA cannot be triggered by register writes on the host, so the DmaSet family
 * calls into the port directly. Immediate transfers complete before returning;
 * HBlank/VBlank/special transfers are latched for the frame loop and renderer.
 */
void HwDmaSet(int channel, const void* src, void* dest, uint32_t control);
void HwDmaStop(int channel);

/* BIOS calls issued through SystemCall(n) rather than a named wrapper. */
void HwSystemCall(int number);

#endif // PORT_HW_H
