#ifndef PORT_SOUND_INTERNAL_H
#define PORT_SOUND_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

// psg.c: the four CGB channels, synthesised from the sound registers in gHwIo.
// stereo holds the frame's DirectSound output in 10-bit DAC units (signed, around
// the bias); this adds the PSG output and applies the DAC's clipping to 0-3FFh
// around SOUNDBIAS.
void PsgRender(float* stereo, int frames, int sampleRate);
/*
 * PsgRender's effect on the registers and channel state (triggers, lengths,
 * sweep, envelopes, NR52) without synthesising: the same clock, step for step.
 */
void PsgAdvance(int frames, int sampleRate);
/*
 * For the DS's own sound channels (dsvoices.c): PsgAdvance in integers, the
 * frame sequencer counted per output sample as PsgRenderDsi counts it; then the
 * channels' state and the mix settings.
 */
void PsgAdvanceFixed(int frames, int sampleRate);
typedef struct {
    bool on, triggered; // triggered since the last PsgVoices
    int volume;         // 0-15 (the envelope's; the wave channel's is in wave)
    int frequency;      // the 11-bit NRx3/NRx4 value
    int duty;           // squares: NRx1 bits 6-7
    uint8_t wave[32];   // wave: the 32 levels after NR32's volume
    uint8_t noise;      // noise: NR43
} PsgVoice;
typedef struct {
    int nr51;           // which channels go left (bits 4-7) and right (0-3)
    int volume[2];      // left, right: NR50 volume + 1 times the PSG ratio in halves (0 when off)
    int directSound[2]; // DirectSound A: half DAC units per 8-bit step (4 or 8; 0 when off)
} PsgMix;
void PsgVoices(PsgVoice voices[4], PsgMix* mix);
#ifdef PLATFORM_DSI
// PsgRender in integers, from the frame's 8-bit DirectSound samples to 16-bit
// stereo (psg.c).
void PsgRenderDsi(const int8_t* mono, int16_t* stereo, int frames, int sampleRate);
#endif
void PsgReset(void);
// True while a CGB channel is audible.
bool PsgActive(void);

/*
 * m4a_mixer.c: the per-frame envelope step of a DirectSound channel. False if
 * it is silent this frame; *started for a note that starts this frame.
 */
struct SoundInfo;
struct SoundChannel;
bool SoundEnvelopeStep(struct SoundInfo* soundInfo, struct SoundChannel* chan, bool* started);

// m4a_mixer.c: forget the mixer's history (HQ reverb line, filters, CGB state),
// for starting over after m4aSoundInit.
void SoundMixerReset(void);

#endif // PORT_SOUND_INTERNAL_H
