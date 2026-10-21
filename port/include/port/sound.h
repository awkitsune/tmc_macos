#ifndef PORT_SOUND_H
#define PORT_SOUND_H

/*
 * Native sound output. The game's m4a engine runs unchanged (src/gba/m4a.c plus
 * the C version of its assembly in port/src/sound); once per frame SoundMain
 * mixes the DirectSound channels, the CGB channels are synthesised from the
 * sound registers, and the frame's stereo samples go to the sink set here.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum SoundQuality {
    // The GBA mixer as written: 8-bit mono DirectSound at the engine rate
    // (15768 Hz in TMC), linear interpolation, 8-bit wraparound.
    SOUND_QUALITY_AUTHENTIC,
    // Same engine and envelopes, mixed in floating point at the output rate with
    // cubic interpolation and per-frame volume ramps.
    SOUND_QUALITY_HQ,
} SoundQuality;

// Called once per mixed frame with interleaved stereo samples in [-1, 1].
typedef void (*SoundSink)(const float* stereo, int frames, int sampleRate, void* context);

void SoundSetSink(SoundSink sink, void* context);

// Takes effect from the next frame. hqRate is the HQ output rate (e.g. 48000).
void SoundSetQuality(SoundQuality quality, int hqRate);
SoundQuality SoundGetQuality(void);

// The rate frames are delivered at: the engine's rate (authentic) or hqRate.
int SoundOutputRate(void);

// Song-data GBA address (ROM sound data) to native pointer; NULL (with a
// warning) if unmapped.
void* M4aGbaPtr(uint32_t address);

// Called on every GOTO a track executes (loop detection for the song exporter).
struct MusicPlayerInfo;
struct MusicPlayerTrack;
typedef void (*M4aGotoHook)(struct MusicPlayerInfo* player, struct MusicPlayerTrack* track, const uint8_t* from,
                            const uint8_t* to);
extern M4aGotoHook gM4aGotoHook;

// True while any DirectSound or CGB channel is sounding.
bool SoundChannelsActive(void);

// TMC_WAV=path: record everything the headless runner plays (sound_wav.c).
void SoundWavFromEnv(void);
void SoundWavClose(void);

#endif // PORT_SOUND_H
