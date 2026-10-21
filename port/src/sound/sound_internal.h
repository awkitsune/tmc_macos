#ifndef PORT_SOUND_INTERNAL_H
#define PORT_SOUND_INTERNAL_H

#include <stdbool.h>

// psg.c: the four CGB channels, synthesised from the sound registers in gHwIo.
// Adds the frame's PSG output (in 10-bit DAC units) to interleaved stereo.
void PsgRender(float* stereo, int frames, int sampleRate);
void PsgReset(void);
// True while a CGB channel is audible.
bool PsgActive(void);

// m4a_mixer.c: forget the mixer's history (HQ reverb line, filters, CGB state),
// for starting over after m4aSoundInit.
void SoundMixerReset(void);

#endif // PORT_SOUND_INTERNAL_H
