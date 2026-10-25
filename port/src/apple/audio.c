/*
 * Core Audio output for the app (see audio.h).
 *
 * Producer: SoundMain on the game thread hands over one frame of stereo
 * samples at the engine rate (15768 Hz, or 48 kHz in HQ). They go into a
 * single-producer/single-consumer ring. While the ring is fuller than a quarter
 * second (fast-forward), frames are dropped instead, which keeps latency bounded.
 *
 * Consumer: the output unit's render callback, on Core Audio's real-time
 * thread. It reads the ring with a Catmull-Rom resampler at
 * sourceRate / deviceRate, nudged by up to 0.5% towards a fill level of about
 * 50 ms. On an underrun (pause, a stall) it fades to silence and waits for the
 * ring to refill before playing again, so neither case clicks.
 *
 * On iOS the output unit is RemoteIO, at the audio session's rate
 * (ios/audiosession.m sets the session up and restarts the unit when the
 * route or the rate changes).
 *
 * TMC_AUDIO_WAV=path records what the callback produced (up to ten minutes) and
 * writes it on exit, with underrun and drop counts on stderr, for checking the
 * real output path without listening.
 */

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "port/sound.h"
#include "../sound/wav.h"

#define RING_FRAMES (1 << 15)
#define MAX_DRIFT 0.005
#define TARGET_SECONDS 0.05
#define DROP_SECONDS 0.25

static AudioUnit sUnit;
static double sDeviceRate;

static float sRing[2 * RING_FRAMES];
static _Atomic uint64_t sWritten; // frames written, ever
static _Atomic uint64_t sRead;    // frames consumed, ever
static _Atomic int sSourceRate;
static _Atomic uint32_t sGeneration; // bumped when the source rate changes
static atomic_bool sMuted;
static _Atomic double sRateScale = 1;
static _Atomic uint64_t sDropped;
static _Atomic uint64_t sUnderruns;

// Consumer state, touched only by the render callback.
static uint32_t sSeenGeneration;
static double sFrac;
static double sFillAverage;
static bool sPriming = true;
static float sLast[2];

// TMC_AUDIO_WAV.
static float* sRecord;
static size_t sRecordFrames, sRecordCapacity;
static double sFillMin = 1e9, sFillMax, sStepMin = 1e9, sStepMax;

static void Produce(const float* stereo, int frames, int rate, void* context) {
    uint64_t written = atomic_load_explicit(&sWritten, memory_order_relaxed);
    uint64_t read = atomic_load_explicit(&sRead, memory_order_acquire);
    uint64_t fill = written - read;

    if (rate != atomic_load(&sSourceRate)) {
        atomic_store(&sSourceRate, rate);
        atomic_fetch_add(&sGeneration, 1);
    }
    if (fill + frames > (uint64_t)(rate * DROP_SECONDS) || fill + frames > RING_FRAMES) {
        atomic_fetch_add(&sDropped, frames);
        return;
    }
    for (int i = 0; i < frames; i++) {
        size_t at = (size_t)((written + i) & (RING_FRAMES - 1));
        sRing[2 * at] = stereo[2 * i];
        sRing[2 * at + 1] = stereo[2 * i + 1];
    }
    atomic_store_explicit(&sWritten, written + frames, memory_order_release);
}

static inline float RingSample(uint64_t frame, int channel) {
    return sRing[2 * (frame & (RING_FRAMES - 1)) + channel];
}

static void FadeOut(float* out, UInt32 from, UInt32 frames) {
    for (UInt32 i = from; i < frames; i++) {
        sLast[0] *= 0.995f;
        sLast[1] *= 0.995f;
        out[2 * i] = sLast[0];
        out[2 * i + 1] = sLast[1];
    }
}

static OSStatus Render(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time, UInt32 bus,
                       UInt32 frames, AudioBufferList* buffers) {
    float* out = buffers->mBuffers[0].mData;
    uint32_t generation = atomic_load(&sGeneration);
    uint64_t written = atomic_load_explicit(&sWritten, memory_order_acquire);
    uint64_t read = atomic_load_explicit(&sRead, memory_order_relaxed);
    double rate = atomic_load(&sSourceRate);
    double target, step;
    UInt32 i;

    if (generation != sSeenGeneration) {
        // New source rate: what is buffered was made for the old one.
        sSeenGeneration = generation;
        read = written;
        sPriming = true;
        sFrac = 0;
    }
    if (rate <= 0) {
        FadeOut(out, 0, frames);
        return noErr;
    }
    target = fmax(rate * TARGET_SECONDS, 2.0 * frames * rate / sDeviceRate);
    if (sPriming) {
        if ((double)(written - read) < target) {
            FadeOut(out, 0, frames);
            return noErr;
        }
        sPriming = false;
        sFillAverage = target;
    }

    sFillAverage += ((double)(written - read) - sFillAverage) * 0.02;
    step = rate * atomic_load(&sRateScale) / sDeviceRate * (1 + MAX_DRIFT * fmax(-1, fmin(1, (sFillAverage - target) / target)));
    if (sRecord != NULL) {
        double ratio = step * sDeviceRate / (rate * atomic_load(&sRateScale));
        sFillMin = fmin(sFillMin, (written - read) / rate);
        sFillMax = fmax(sFillMax, (written - read) / rate);
        sStepMin = fmin(sStepMin, ratio);
        sStepMax = fmax(sStepMax, ratio);
    }

    for (i = 0; i < frames; i++) {
        float t = (float)sFrac;
        double advance;

        if (read + 2 >= written) {
            atomic_fetch_add(&sUnderruns, 1);
            sPriming = true;
            FadeOut(out, i, frames);
            break;
        }
        for (int c = 0; c < 2; c++) {
            float ym1 = RingSample(read - 1, c), y0 = RingSample(read, c);
            float y1 = RingSample(read + 1, c), y2 = RingSample(read + 2, c);
            float c1 = 0.5f * (y1 - ym1);
            float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
            float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
            sLast[c] = ((c3 * t + c2) * t + c1) * t + y0;
            out[2 * i + c] = sLast[c];
        }
        sFrac += step;
        advance = floor(sFrac);
        sFrac -= advance;
        read += (uint64_t)advance;
    }
    atomic_store_explicit(&sRead, read, memory_order_release);

    if (atomic_load(&sMuted)) {
        memset(out, 0, frames * 2 * sizeof(float));
    }
    if (sRecord != NULL) {
        UInt32 n = frames;
        if (sRecordFrames + n > sRecordCapacity) {
            n = (UInt32)(sRecordCapacity - sRecordFrames);
        }
        memcpy(sRecord + 2 * sRecordFrames, out, n * 2 * sizeof(float));
        sRecordFrames += n;
    }
    return noErr;
}

static bool Check(OSStatus status, const char* what) {
    if (status != noErr) {
        fprintf(stderr, "audio: %s failed (%d)\n", what, (int)status);
        return false;
    }
    return true;
}

// Create and initialize the output unit, fed at the device's rate.
static bool OpenUnit(void) {
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
#if TARGET_OS_IOS
        .componentSubType = kAudioUnitSubType_RemoteIO,
#else
        .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent component = AudioComponentFindNext(NULL, &desc);
    AudioStreamBasicDescription device = { 0 };
    UInt32 size = sizeof(device);
    AudioStreamBasicDescription format;
    AURenderCallbackStruct callback = { Render, NULL };

    if (component == NULL) {
        fprintf(stderr, "audio: no default output unit\n");
        return false;
    }
    if (!Check(AudioComponentInstanceNew(component, &sUnit), "AudioComponentInstanceNew")) {
        return false;
    }
    // Feed the unit at the device's own rate so there is only one resampler.
#if TARGET_OS_IOS
    (void)size;
    device.mSampleRate = AudioSessionSampleRate();
#else
    AudioUnitGetProperty(sUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &device, &size);
#endif
    sDeviceRate = device.mSampleRate >= 8000 ? device.mSampleRate : 48000;
    format = (AudioStreamBasicDescription){
        .mSampleRate = sDeviceRate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 8,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 8,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 32,
    };
    if (!Check(AudioUnitSetProperty(sUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                    sizeof(format)),
               "setting the stream format") ||
        !Check(AudioUnitSetProperty(sUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
                                    sizeof(callback)),
               "setting the render callback") ||
        !Check(AudioUnitInitialize(sUnit), "AudioUnitInitialize")) {
        AudioComponentInstanceDispose(sUnit);
        sUnit = NULL;
        return false;
    }
    return true;
}

bool AudioStart(void) {
    const char* record = getenv("TMC_AUDIO_WAV");

    if (!OpenUnit()) {
        return false;
    }
    if (record != NULL && record[0] != '\0') {
        sRecordCapacity = (size_t)(sDeviceRate * 600);
        sRecord = malloc(sRecordCapacity * 2 * sizeof(float));
    }
    SoundSetSink(Produce, NULL);
    if (!Check(AudioOutputUnitStart(sUnit), "AudioOutputUnitStart")) {
        SoundSetSink(NULL, NULL);
        return false;
    }
    return true;
}

void AudioStop(void) {
    const char* record = getenv("TMC_AUDIO_WAV");

    if (sUnit == NULL) {
        return;
    }
    AudioOutputUnitStop(sUnit);
    SoundSetSink(NULL, NULL);
    AudioUnitUninitialize(sUnit);
    AudioComponentInstanceDispose(sUnit);
    sUnit = NULL;
    if (sRecord != NULL) {
        WavWriter* wav = WavOpen(record, (int)sDeviceRate, NULL);
        if (wav != NULL) {
            WavWrite(wav, sRecord, (int)sRecordFrames);
            WavClose(wav);
        }
        fprintf(stderr, "audio: %.1f s at %.0f Hz recorded to %s; %llu underruns, %llu frames dropped\n",
                sRecordFrames / sDeviceRate, sDeviceRate, record, (unsigned long long)atomic_load(&sUnderruns),
                (unsigned long long)atomic_load(&sDropped));
        fprintf(stderr, "audio: buffered %.0f-%.0f ms, rate correction %+.3f%% to %+.3f%%\n", sFillMin * 1000,
                sFillMax * 1000, (sStepMin - 1) * 100, (sStepMax - 1) * 100);
        free(sRecord);
        sRecord = NULL;
    }
}

#if TARGET_OS_IOS
static void CloseUnit(void) {
    AudioOutputUnitStop(sUnit);
    AudioUnitUninitialize(sUnit);
    AudioComponentInstanceDispose(sUnit);
    sUnit = NULL;
}

bool AudioSetRunning(bool running) {
    if (sUnit == NULL) {
        return false;
    }
    if (!running) {
        AudioOutputUnitStop(sUnit);
        return true;
    }
    return Check(AudioOutputUnitStart(sUnit), "AudioOutputUnitStart");
}

bool AudioRestart(void) {
    if (sUnit == NULL) {
        return false;
    }
    CloseUnit();
    if (!OpenUnit()) {
        return false;
    }
    return Check(AudioOutputUnitStart(sUnit), "AudioOutputUnitStart");
}

double AudioDeviceRate(void) {
    return sUnit != NULL ? sDeviceRate : 0;
}
#endif

void AudioSetRateScale(double scale) {
    atomic_store(&sRateScale, fmax(0.98, fmin(1.02, scale)));
}

void AudioSetMuted(bool muted) {
    atomic_store(&sMuted, muted);
}

bool AudioIsMuted(void) {
    return atomic_load(&sMuted);
}

void AudioSetHighQuality(bool on) {
    SoundSetQuality(on ? SOUND_QUALITY_HQ : SOUND_QUALITY_AUTHENTIC, 48000);
}

bool AudioIsHighQuality(void) {
    return SoundGetQuality() == SOUND_QUALITY_HQ;
}
