#ifndef DSI_SOUND_IMAGE_H
#define DSI_SOUND_IMAGE_H

#include <stdint.h>

/*
 * The DS sound channels' registers for one frame, from the ARM9
 * (sound_dsi.c, from port/src/sound/dsvoices.c) to the ARM7 (port/dsi/arm7),
 * which writes them: a channel is SOUNDxCNT, SAD, TMR, PNT, LEN as the
 * hardware lays them out at 0x04000400 + 16 * x. The ARM9 sends the image's
 * address on DSI_SOUND_FIFO after flushing it from its data cache.
 */
#define DSI_SOUND_FIFO FIFO_USER_01

/*
 * The ARM7's main loop ending (port/dsi/arm7: Start+Select+L+R held, or the
 * power button), which powers the DSi off: the ARM7 sends the reason
 * (DSI_QUIT_*) and waits up to two seconds for the ARM9's answer on the same
 * channel, so the save and the log reach the card first (platform_dsi.c).
 */
#define DSI_QUIT_FIFO FIFO_USER_02
#define DSI_QUIT_KEYS 1
#define DSI_QUIT_POWER 2

typedef struct {
    uint32_t cnt;
    uint32_t src;
    uint16_t timer;
    uint16_t loop;
    uint32_t length;
} DsiSoundChannel;

typedef struct {
    uint16_t write; // channels to write
    uint16_t start; // of those, the ones to stop and start from the beginning
    uint32_t pad[7];
    DsiSoundChannel ch[16];
} DsiSoundImage; // a multiple of 32 bytes, the cache line

#endif // DSI_SOUND_IMAGE_H
