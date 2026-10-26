#ifndef PORT_SOUND_INTERNAL_H
#define PORT_SOUND_INTERNAL_H

#include <stdbool.h>

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
void PsgReset(void);
// True while a CGB channel is audible.
bool PsgActive(void);

// m4a_mixer.c: forget the mixer's history (HQ reverb line, filters, CGB state),
// for starting over after m4aSoundInit.
void SoundMixerReset(void);

#endif // PORT_SOUND_INTERNAL_H
