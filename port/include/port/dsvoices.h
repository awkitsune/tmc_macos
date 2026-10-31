#ifndef PORT_SOUND_DSVOICES_H
#define PORT_SOUND_DSVOICES_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The m4a engine's voices on the DS's own sound channels (dsvoices.c): the
 * registers of the 16 channels for one frame, which the DSi's ARM7 writes
 * (port/dsi/arm7) and dsmodel.c plays on the Mac to check them.
 */
#define DS_SOUND_CHANNELS 16
#define DS_SOUND_CLOCK 16756991u // the channels' timer clock: the bus clock halved

// SOUNDxCNT
#define DS_CNT_VOLUME(v) (v)          // 0-127
#define DS_CNT_PAN(p) ((uint32_t)(p) << 16) // 0-127, 64 centre
#define DS_CNT_DUTY(d) ((uint32_t)(d) << 24) // PSG: (d + 1) / 8 high
#define DS_CNT_LOOP (1u << 27)
#define DS_CNT_ONE_SHOT (2u << 27)
#define DS_CNT_PCM8 (0u << 29)
#define DS_CNT_PSG (3u << 29) // a square on channels 8-13, noise on 14-15
#define DS_CNT_START (1u << 31)

typedef struct {
    uint32_t cnt;    // SOUNDxCNT; without DS_CNT_START the channel is stopped
    uintptr_t src;   // SOUNDxSAD: the sample (word aligned)
    uint16_t timer;  // SOUNDxTMR: 0x10000 - DS_SOUND_CLOCK / rate
    uint16_t loop;   // SOUNDxPNT: the loop's start, in words
    uint32_t length; // SOUNDxLEN: the words after it
} DsChannelRegs;

typedef struct {
    uint16_t write; // channels whose registers changed this frame
    uint16_t start; // of those, the ones (re)started: stopped, then all written
    DsChannelRegs ch[DS_SOUND_CHANNELS];
} DsSoundFrame;

/*
 * One frame of the engine's sound (SoundMain's, after the players and
 * CgbSound) as channel registers, instead of mixing: samples is the frame's
 * length in engine samples, which the voices' bookkeeping advances by.
 */
void DsVoicesRun(int samples, DsSoundFrame* frame);
// Every channel stopped (frame->write says which were playing).
void DsVoicesStop(DsSoundFrame* frame);
// The word-aligned loop copies made so far, their bytes, loops played rounded
// to words for want of room, and the most reverb the engine was set to.
void DsVoicesStats(int* copies, uint32_t* poolBytes, int* rounded, int* reverb);
// A CRC32 of every frame's registers so far (not the samples' addresses) and
// the frames: the DSi (DSI_CHECKS) and the Mac (TMC_DS_SOUND) log it to compare.
void DsVoicesCrc(uint32_t* crc, uint32_t* runs);
// Forget the channels' state (after m4aSoundInit, or a save state).
void DsVoicesReset(void);

// Make memory the CPU wrote visible to the sound hardware (the DSi's data cache).
void DsSoundFlush(const void* data, uint32_t size);

// dsmodel.c (not the DSi): the DS mixer playing the frames, as stereo floats
// in [-1, 1] at the given rate, on the authentic mixer's scale.
void DsModelRender(const DsSoundFrame* frame, float* stereo, int frames, int rate);
void DsModelReset(void);

#endif // PORT_SOUND_DSVOICES_H
