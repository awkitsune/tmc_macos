#ifndef PORT_SOUND_WAV_H
#define PORT_SOUND_WAV_H

#include <stdint.h>

// 16-bit stereo PCM WAV files, optionally with a LIST/INFO chunk.
typedef struct WavInfo {
    const char* title;   // INAM
    const char* artist;  // IART
    const char* album;   // IPRD
    const char* comment; // ICMT
    const char* genre;   // IGNR
    int track;           // ITRK, if > 0
} WavInfo;

typedef struct WavWriter WavWriter;

WavWriter* WavOpen(const char* path, int sampleRate, const WavInfo* info);
// Interleaved stereo in [-1, 1].
void WavWrite(WavWriter* wav, const float* stereo, int frames);
uint64_t WavFrames(const WavWriter* wav);
void WavClose(WavWriter* wav);

#endif // PORT_SOUND_WAV_H
