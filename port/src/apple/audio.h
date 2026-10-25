#ifndef PORT_APPLE_AUDIO_H
#define PORT_APPLE_AUDIO_H

#include <stdbool.h>

/*
 * Sound output for the app: the game thread's mixed frames go through a
 * lock-free ring to a Core Audio output unit, whose render callback resamples
 * them to the device rate (RemoteIO on iOS). The resampling ratio follows the ring's fill level
 * (by at most 0.5%), so the game's own 59.73 Hz pacing and the audio device's
 * clock never drift apart: no growing latency, no underruns.
 */

// Start the output unit and route the game's sound to it. Returns false (and
// logs why) if there is no audio output; the game then runs silently.
bool AudioStart(void);
void AudioStop(void);

void AudioSetMuted(bool muted);
bool AudioIsMuted(void);

// The game thread's frame rate relative to the 59.73 Hz the sound engine mixes
// for (display sync pacing runs it at the display's rate): the resampler plays
// the samples that much faster. The drift correction does the rest.
void AudioSetRateScale(double scale);

#include <TargetConditionals.h>
#if TARGET_OS_IOS
// Stop or restart the started output unit (an audio session interruption, the
// app in the background).
bool AudioSetRunning(bool running);
// Make the unit again at the session's current rate (a route change).
bool AudioRestart(void);
// The rate the unit is fed at; 0 when not started.
double AudioDeviceRate(void);
// The audio session's output rate (ios/audiosession.m).
double AudioSessionSampleRate(void);
#endif

// High-quality mixing (48 kHz float) instead of the GBA's 15.7 kHz 8-bit mix.
void AudioSetHighQuality(bool on);
bool AudioIsHighQuality(void);

#endif // PORT_APPLE_AUDIO_H
