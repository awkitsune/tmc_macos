#ifndef PORT_PROFILE_H
#define PORT_PROFILE_H

#include <stdint.h>

/*
 * Where a frame's hardware work goes, on the DSi (its boot log,
 * port/dsi/src/platform_dsi.c): clock ticks summed per part. Elsewhere the
 * marks compile to nothing.
 */
typedef enum {
    PROFILE_IRQ_HBLANK,  // the game's HBlank interrupt handler
    PROFILE_IRQ_VCOUNT,  // its VCount handler: the sound (m4aSoundMain), parts below
    PROFILE_IRQ_VBLANK,  // its VBlank handler
    PROFILE_IRQ_OTHER,   // other interrupts
    PROFILE_DMA_HBLANK,  // HBlank DMA transfers
    PROFILE_DMA_VBLANK,  // VBlank DMA transfers
    PROFILE_SOUND_SEQ,   // the m4a sequencer (MPlayMain)
    PROFILE_SOUND_CGB,   // CgbSound: the PSG channels' registers
    PROFILE_SOUND_MIX,   // the DirectSound mixer
    PROFILE_SOUND_PSG,   // the PSG channels rendered and the output stage (PsgRenderDsi)
    PROFILE_SOUND_OUT,   // the samples into the DS's ring (DsiSoundFrame)
    PROFILE_PARTS
} ProfilePart;

#ifdef PLATFORM_DSI
uint32_t ProfileClock(void);
extern uint32_t gProfileTicks[PROFILE_PARTS], gProfileCount[PROFILE_PARTS];
#define PROFILE_START() ProfileClock()
#define PROFILE_ADD(part, start) (gProfileTicks[part] += ProfileClock() - (start), gProfileCount[part]++)
#else
#define PROFILE_START() 0u
#define PROFILE_ADD(part, start) ((void)(start))
#endif

#endif // PORT_PROFILE_H
