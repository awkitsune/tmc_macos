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

/*
 * Host pointer for a fixed GBA address: hardware regions map to the buffers above,
 * EWRAM/IWRAM to the variable placed there (by symbol, from the GBA link).
 * Aborts on an address with no native counterpart.
 */
void* HwGbaPtr(uint32_t address);

/* BIOS calls issued through SystemCall(n) rather than a named wrapper. */
void HwSystemCall(int number);

/* GBA key bits (KEYINPUT order), as held by the player. */
#define HW_KEY_A 0x0001
#define HW_KEY_B 0x0002
#define HW_KEY_SELECT 0x0004
#define HW_KEY_START 0x0008
#define HW_KEY_RIGHT 0x0010
#define HW_KEY_LEFT 0x0020
#define HW_KEY_UP 0x0040
#define HW_KEY_DOWN 0x0080
#define HW_KEY_R 0x0100
#define HW_KEY_L 0x0200

/*
 * Frame boundary helpers for platform layers (frame.c): latch the held keys into
 * KEYINPUT, then raise the mid-frame VCOUNT and the VBLANK interrupts.
 */
void HwSetKeys(uint16_t held);
void HwFrameInterrupts(void);

#endif // PORT_HW_H
