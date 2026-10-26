/*
 * SoundMain and the DirectSound mixer (SoundMainRAM in asm/lib/m4a_asm.s).
 *
 * Each frame SoundMain runs the music players and CgbSound, then mixes the
 * DirectSound channels. The envelope code is shared; the sample loop comes in two
 * versions:
 *
 *  - authentic: the assembly's mixer, writing 8-bit mono samples into the
 *    engine's PCM ring (soundInfo->pcmBuffer) at the engine rate, with its
 *    reverb, linear interpolation and 8-bit wraparound;
 *  - HQ: floating point at the output rate, cubic interpolation, per-frame
 *    volume ramps, and the same reverb as a delay line of the same length.
 *
 * DirectSound A goes to the outputs SOUNDCNT_H selects. The CGB channels are
 * then synthesised from the sound registers and the sum is clipped as by the
 * DAC (psg.c), the DC is removed as by the GBA's output coupling capacitor (the 8-bit mixer's
 * rounding and the unipolar CGB levels both carry an offset), and the stereo
 * frame is handed to the sink.
 */

#include <math.h>
#include <stdatomic.h>

#include "global.h"
#include "gba/m4a.h"
#include "gba/m4a_internal.h"
#include "port/hw.h"
#include "port/sound.h"
#include "port/state.h"
#include "sound_internal.h"

#define MAX_FRAME 4096
#define GBA_CLOCK 16777216u
#define GBA_FRAME_CYCLES 280896u // 228 lines of 1232 cycles: 59.7275 Hz

// DirectSound A at 100% volume: an 8-bit sample is 4 units of the 10-bit DAC.
#define DS_SCALE 4.0f
#define DAC_HALF 512.0f

// Output settings, not game state: loading a save state keeps them.
static SoundSink sSink STATE_EXCLUDE;
static void* sSinkContext STATE_EXCLUDE;
// Set from the app's main thread, applied by the game thread at the next frame.
static _Atomic SoundQuality sQuality STATE_EXCLUDE = SOUND_QUALITY_AUTHENTIC;
static SoundQuality sAppliedQuality = SOUND_QUALITY_AUTHENTIC;
static _Atomic int sHqRate STATE_EXCLUDE = 48000;

static float sMono[MAX_FRAME];
static float sStereo[2 * MAX_FRAME];

// HQ state: frame length accumulator, reverb delay line, last frame's volumes.
static uint64_t sHqCycleAccum;
#define HQ_RING (1 << 15)
static float sHqRing[HQ_RING];
static uint32_t sHqRingPos;
static float sHqVolume[MAX_DIRECTSOUND_CHANNELS];

static float sDcIn[2], sDcOut[2];

void SoundSetSink(SoundSink sink, void* context) {
    sSink = sink;
    sSinkContext = context;
}

void SoundSetQuality(SoundQuality quality, int hqRate) {
    if (hqRate > 0) {
        sHqRate = hqRate;
    }
    sQuality = quality;
}

SoundQuality SoundGetQuality(void) {
    return sQuality;
}

int SoundOutputRate(void) {
    SoundInfo* soundInfo = SOUND_INFO_PTR;

    if (sQuality == SOUND_QUALITY_HQ) {
        return sHqRate;
    }
    return soundInfo != NULL && soundInfo->pcmFreqency > 0 ? soundInfo->pcmFreqency : 15768;
}

bool SoundChannelsActive(void) {
    SoundInfo* soundInfo = SOUND_INFO_PTR;

    if (soundInfo == NULL) {
        return false;
    }
    for (int i = 0; i < MAX_DIRECTSOUND_CHANNELS; i++) {
        if (soundInfo->chans[i].statusFlags & SOUND_CHANNEL_SF_ON) {
            return true;
        }
    }
    if (soundInfo->cgbChannels != NULL) {
        for (int i = 0; i < 4; i++) {
            if (soundInfo->cgbChannels[i].statusFlags & SOUND_CHANNEL_SF_ON) {
                return true;
            }
        }
    }
    return PsgActive();
}

/*
 * The per-frame envelope step of SoundMainRAM. Returns false if the channel is
 * silent this frame (stopped now or already). *started is set for a note that
 * starts this frame.
 */
static bool EnvelopeStep(SoundInfo* soundInfo, SoundChannel* chan, bool* started) {
    WaveData* wav = chan->wav;
    u32 flags = chan->statusFlags;
    s32 env;

    *started = false;
    if (!(flags & SOUND_CHANNEL_SF_ON)) {
        return false;
    }
    if (flags & SOUND_CHANNEL_SF_START) {
        if (flags & SOUND_CHANNEL_SF_STOP) {
            chan->statusFlags = 0;
            return false;
        }
        flags = SOUND_CHANNEL_SF_ENV_ATTACK;
        chan->currentPointer = (PtrU32)wav->data;
        chan->count = wav->size;
        env = 0;
        chan->fw = 0;
        if (wav->status & 0xC0) {
            flags |= SOUND_CHANNEL_SF_LOOP;
        }
        chan->statusFlags = flags;
        *started = true;
        goto attack;
    }

    env = chan->envelopeVolume;
    if (flags & SOUND_CHANNEL_SF_IEC) {
        // Pseudo-echo: hold the echo volume for echoLength frames.
        if (chan->echoLength-- <= 1) {
            chan->statusFlags = 0;
            return false;
        }
    } else if (flags & SOUND_CHANNEL_SF_STOP) {
        env = (env * chan->release) >> 8;
        if (env <= chan->echoVolume) {
        echo_start:
            env = chan->echoVolume;
            if (env == 0) {
                chan->statusFlags = 0;
                return false;
            }
            flags |= SOUND_CHANNEL_SF_IEC;
            chan->statusFlags = flags;
        }
    } else if ((flags & SOUND_CHANNEL_SF_ENV) == SOUND_CHANNEL_SF_ENV_DECAY) {
        env = (env * chan->decay) >> 8;
        if (env <= chan->sustain) {
            env = chan->sustain;
            if (env == 0) {
                goto echo_start;
            }
            flags--;
            chan->statusFlags = flags;
        }
    } else if ((flags & SOUND_CHANNEL_SF_ENV) == SOUND_CHANNEL_SF_ENV_ATTACK) {
    attack:
        env += chan->attack;
        if (env >= 0xFF) {
            env = 0xFF;
            flags--;
            chan->statusFlags = flags;
        }
    }

    chan->envelopeVolume = env;
    env = ((soundInfo->masterVolume + 1) * env) >> 4;
    chan->envelopeVolumeRight = ((chan->rightVolume + chan->leftVolume) * env) >> 9;
    return true;
}

// ---- authentic ------------------------------------------------------------

static void MixChannelAuthentic(SoundInfo* soundInfo, SoundChannel* chan, s8* out, s32 samples) {
    WaveData* wav = chan->wav;
    s32 vol = chan->envelopeVolumeRight;
    const s8* loopStart = NULL;
    s32 loopLength = 0;
    s32 count = chan->count;
    const s8* p = (const s8*)chan->currentPointer;
    s32 i = 0;

    if (chan->statusFlags & SOUND_CHANNEL_SF_LOOP) {
        loopStart = wav->data + wav->loopStart;
        loopLength = wav->size - wav->loopStart;
    }

    if (chan->type & TONEDATA_TYPE_FIX) {
        // One sample per output sample.
        for (; i < samples; i++) {
            out[i] = (s8)(out[i] + ((vol * *p++) >> 8));
            if (--count == 0) {
                if (loopLength == 0) {
                    chan->statusFlags = 0;
                    return;
                }
                count = loopLength;
                p = loopStart;
            }
        }
    } else {
        // Linear interpolation, 23-bit fraction, step divFreq * frequency.
        u32 fw = chan->fw;
        u32 step = (u32)soundInfo->divFreq * chan->frequency;
        s32 s0 = p[0];
        s32 delta = p[1] - s0;

        p++; // the assembly keeps the pointer on the next sample
        for (; i < samples; i++) {
            s32 sample = s0 + ((s32)(fw * delta) >> 23);
            u32 advance;

            out[i] = (s8)(out[i] + ((vol * sample) >> 8));
            fw += step;
            advance = fw >> 23;
            if (advance == 0) {
                continue;
            }
            fw &= 0x7FFFFF;
            count -= advance;
            if (count <= 0) {
                s32 over;

                if (loopLength == 0) {
                    chan->statusFlags = 0;
                    return;
                }
                over = -count;
                count += loopLength;
                while (count <= 0) {
                    over -= loopLength;
                    count += loopLength;
                }
                p = loopStart + over;
                s0 = p[0];
            } else if (advance == 1) {
                s0 += delta;
            } else {
                p += advance - 1;
                s0 = *p;
            }
            delta = p[1] - s0;
            p++;
        }
        p--;
        chan->fw = fw;
    }
    chan->count = count;
    chan->currentPointer = (PtrU32)p;
}

static void MixAuthentic(SoundInfo* soundInfo, s8* out, s32 samples) {
    s32 counter = soundInfo->pcmDmaCounter;
    s8* next;

    // The slot after this one in the PCM ring holds the oldest-but-one frame.
    next = counter == 2 ? soundInfo->pcmBuffer : out + samples;
    if (soundInfo->reverb != 0) {
        for (s32 i = 0; i < samples; i++) {
            s32 v = ((out[i] + next[i]) * soundInfo->reverb) >> 8;
            if (v & 0x80) {
                v++;
            }
            out[i] = v;
        }
    } else {
        memset(out, 0, samples);
    }

    for (s32 i = 0; i < soundInfo->maxChannels; i++) {
        SoundChannel* chan = &soundInfo->chans[i];
        bool started;

        if (EnvelopeStep(soundInfo, chan, &started)) {
            MixChannelAuthentic(soundInfo, chan, out, samples);
        }
    }
}

// ---- HQ -------------------------------------------------------------------

static inline float Fetch(const s8* data, s32 index, s32 size, s32 loopStart, bool loop) {
    if (index < 0) {
        index = 0;
    }
    if (index >= size) {
        if (!loop) {
            return 0;
        }
        index = loopStart + (index - size) % (size - loopStart);
    }
    return data[index];
}

static void MixChannelHq(SoundInfo* soundInfo, SoundChannel* chan, float* out, s32 samples, float v0, float v1) {
    WaveData* wav = chan->wav;
    const s8* data = wav->data;
    s32 size = wav->size;
    s32 loopStart = wav->loopStart;
    bool loop = (chan->statusFlags & SOUND_CHANNEL_SF_LOOP) && loopStart < size;
    s32 index = (s32)((const s8*)chan->currentPointer - data);
    s32 count = chan->count;
    uint64_t frac = chan->fw;
    double rate = (chan->type & TONEDATA_TYPE_FIX) ? soundInfo->pcmFreqency : chan->frequency;
    uint64_t step = (uint64_t)(rate / sHqRate * 4294967296.0);
    float dv = (v1 - v0) / samples;

    for (s32 i = 0; i < samples; i++) {
        float t = (float)(frac * (1.0 / 4294967296.0));
        float ym1 = Fetch(data, index - 1, size, loopStart, loop);
        float y0 = data[index];
        float y1 = Fetch(data, index + 1, size, loopStart, loop);
        float y2 = Fetch(data, index + 2, size, loopStart, loop);
        // Catmull-Rom spline through the four nearest samples.
        float c1 = 0.5f * (y1 - ym1);
        float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        float sample = ((c3 * t + c2) * t + c1) * t + y0;
        uint64_t advance;

        out[i] += sample * (v0 + dv * i) * (1.0f / 256);
        frac += step;
        advance = frac >> 32;
        frac &= 0xFFFFFFFFu;
        if (advance == 0) {
            continue;
        }
        index += (s32)advance;
        count -= (s32)advance;
        if (count <= 0) {
            if (!loop) {
                chan->statusFlags = 0;
                return;
            }
            s32 over = (-count) % (size - loopStart);
            index = loopStart + over;
            count = size - index;
        }
    }
    chan->fw = (u32)frac;
    chan->count = count;
    chan->currentPointer = (PtrU32)(data + index);
}

static s32 HqFrameLength(void) {
    s32 samples;

    sHqCycleAccum += (uint64_t)sHqRate * GBA_FRAME_CYCLES;
    samples = (s32)(sHqCycleAccum / GBA_CLOCK);
    sHqCycleAccum %= GBA_CLOCK;
    return samples > MAX_FRAME ? MAX_FRAME : samples;
}

static void MixHq(SoundInfo* soundInfo, float* out, s32 samples) {
    // The authentic reverb reads the ring slot pcmDmaPeriod frames back and the
    // one after it: delays of pcmDmaPeriod and pcmDmaPeriod - 1 frames.
    double frameSamples = (double)sHqRate * GBA_FRAME_CYCLES / GBA_CLOCK;
    s32 period = soundInfo->pcmDmaPeriod;
    uint32_t delay1 = (uint32_t)lround(period * frameSamples);
    uint32_t delay2 = (uint32_t)lround((period - 1) * frameSamples);
    float reverb = soundInfo->reverb * (1.0f / 256);

    memset(out, 0, samples * sizeof(float));
    for (s32 i = 0; i < soundInfo->maxChannels; i++) {
        SoundChannel* chan = &soundInfo->chans[i];
        bool started;

        if (!EnvelopeStep(soundInfo, chan, &started)) {
            sHqVolume[i] = 0;
            continue;
        }
        float vol = chan->envelopeVolumeRight;
        MixChannelHq(soundInfo, chan, out, samples, started ? vol : sHqVolume[i], vol);
        sHqVolume[i] = vol;
    }

    for (s32 i = 0; i < samples; i++) {
        float y = out[i];
        if (reverb != 0 && delay1 < HQ_RING) {
            y += (sHqRing[(sHqRingPos - delay1) & (HQ_RING - 1)] + sHqRing[(sHqRingPos - delay2) & (HQ_RING - 1)]) * reverb;
        }
        sHqRing[sHqRingPos] = y;
        sHqRingPos = (sHqRingPos + 1) & (HQ_RING - 1);
        out[i] = y;
    }
}

static void ApplyQuality(SoundInfo* soundInfo) {
    if (sAppliedQuality == sQuality) {
        return;
    }
    // The channels' fractional positions differ between the two mixers.
    for (int i = 0; i < MAX_DIRECTSOUND_CHANNELS; i++) {
        soundInfo->chans[i].fw = 0;
        sHqVolume[i] = soundInfo->chans[i].envelopeVolumeRight;
    }
    memset(sHqRing, 0, sizeof(sHqRing));
    memset(soundInfo->pcmBuffer, 0, sizeof(soundInfo->pcmBuffer));
    sHqCycleAccum = 0;
    PsgReset();
    sAppliedQuality = sQuality;
}

void SoundMixerReset(void) {
    memset(sHqRing, 0, sizeof(sHqRing));
    memset(sHqVolume, 0, sizeof(sHqVolume));
    sHqRingPos = 0;
    sHqCycleAccum = 0;
    memset(sDcIn, 0, sizeof(sDcIn));
    memset(sDcOut, 0, sizeof(sDcOut));
    PsgReset();
}

// ---- SoundMain --------------------------------------------------------------

void SoundMain(void) {
    SoundInfo* soundInfo = SOUND_INFO_PTR;
    s32 samples;
    int rate;

    if (soundInfo->ident != ID_NUMBER) {
        return;
    }
    soundInfo->ident++;

    if (soundInfo->MPlayMainHead != NULL) {
        soundInfo->MPlayMainHead(soundInfo->intp);
    }
    soundInfo->CgbSound();

    ApplyQuality(soundInfo);
    if (sAppliedQuality == SOUND_QUALITY_AUTHENTIC) {
        s32 counter = soundInfo->pcmDmaCounter;
        s8* slot = soundInfo->pcmBuffer;

        samples = soundInfo->pcmSamplesPerVBlank;
        if (counter > 1) {
            slot += samples * (soundInfo->pcmDmaPeriod - (counter - 1));
        }
        MixAuthentic(soundInfo, slot, samples);
        for (s32 i = 0; i < samples; i++) {
            sMono[i] = slot[i] * DS_SCALE;
        }
        rate = soundInfo->pcmFreqency;
    } else {
        samples = HqFrameLength();
        MixHq(soundInfo, sMono, samples);
        for (s32 i = 0; i < samples; i++) {
            sMono[i] *= DS_SCALE;
        }
        rate = sHqRate;
    }

    {
        // DirectSound A, the only FIFO m4a feeds: 50% or 100% (SOUNDCNT_H bit
        // 2), output right and left by bits 8 and 9, all of it off with the
        // master enable (SOUNDCNT_X bit 7).
        u16 cnt = gHwIo[REG_OFFSET_SOUNDCNT_H] | (gHwIo[REG_OFFSET_SOUNDCNT_H + 1] << 8);
        float scale = (gHwIo[REG_OFFSET_SOUNDCNT_X] & 0x80) ? ((cnt & 4) ? 1.0f : 0.5f) : 0;
        float right = (cnt & 0x100) ? scale : 0;
        float left = (cnt & 0x200) ? scale : 0;
        for (s32 i = 0; i < samples; i++) {
            sStereo[2 * i] = sMono[i] * left;
            sStereo[2 * i + 1] = sMono[i] * right;
        }
    }
    PsgRender(sStereo, samples, rate);
    {
        float pole = expf(-2.0f * 3.14159265f * 5.0f / rate);
        for (s32 i = 0; i < 2 * samples; i++) {
            float in = sStereo[i];
            float v = in - sDcIn[i & 1] + pole * sDcOut[i & 1];
            sDcIn[i & 1] = in;
            sDcOut[i & 1] = v;
            v *= 1.0f / DAC_HALF;
            sStereo[i] = v > 1 ? 1 : v < -1 ? -1 : v;
        }
    }
    if (sSink != NULL && samples > 0) {
        sSink(sStereo, samples, rate, sSinkContext);
    }

    soundInfo->ident = ID_NUMBER;
}
