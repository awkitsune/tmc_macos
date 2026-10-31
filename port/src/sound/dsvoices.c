/*
 * The m4a engine's voices on the DS's own sound channels, instead of the
 * software mixer (the DSi's default, port/dsi/README.md "Sound").
 *
 * Each frame, after the players and CgbSound:
 *
 *  - DirectSound: each engine channel has a DS channel (0-7). The envelope
 *    runs as in the mixer (SoundEnvelopeStep) and the channel's position is
 *    advanced by the frame without mixing, so the engine sees its channels
 *    start, loop and end as with the mixer. The DS plays the game's 8-bit
 *    sample from memory at the rate the mixer steps through it, with the loop;
 *    a loop whose start or length is not a whole number of words plays from a
 *    copy that makes it one (the DS loops in words).
 *  - CGB: psg.c keeps the registers' effects (triggers, envelopes, lengths,
 *    sweep) without synthesising, and each frame its state sets the squares
 *    (PSG channels 8 and 9, the duty), the wave channel (channel 10, a looped
 *    32-sample copy of wave RAM at NR32's volume) and noise (15-bit: the
 *    noise channel 14, the same shift register; 7-bit: channel 11, a looped
 *    copy of its 127-step sequence).
 *  - Levels: each voice's left and right level from the GBA's (envelope and
 *    master volume, DirectSound A's 50/100% and outputs, NR50, NR51, the PSG
 *    ratio), as a DS volume (with the divider for quiet ones) and pan, at half
 *    the scale the software path streams at: a centred DS channel reaches only
 *    half of full scale on each side.
 *
 * Volume, pan and pitch change once a frame, as the mixer's do.
 */

#include <string.h>

#include "global.h"
#include "gba/m4a.h"
#include "gba/m4a_internal.h"
#include "port/dsvoices.h"
#include "port/hw.h"
#include "sound_internal.h"

#define DIRECT_CHANNELS 8
#define SQUARE_CHANNEL 8 // and 9
#define WAVE_CHANNEL 10
#define NOISE7_CHANNEL 11
#define NOISE_CHANNEL 14
#define FULL 65536 // a level of 1: the voice at full scale on that side

static DsChannelRegs sRegs[DS_SOUND_CHANNELS]; // as last written

// Loops made whole words: copies of the game's samples.
#define COPY_POOL (128 * 1024)
#define COPIES 64
static int8_t sPool[COPY_POOL] __attribute__((aligned(32)));
static uint32_t sPoolUsed;
static struct {
    const WaveData* wav;
    const int8_t* data;
    uint32_t loop, length; // words
} sCopies[COPIES];
static int sCopyCount;

static int sRounded; // loops played rounded to words (no room for a copy)
static uint32_t sCrc, sRuns; // DsVoicesCrc
static int sReverb;          // the most reverb the engine was set to (not played: see the top)

void DsVoicesCrc(uint32_t* crc, uint32_t* runs) {
    *crc = sCrc;
    *runs = sRuns;
}

void DsVoicesStats(int* copies, uint32_t* poolBytes, int* rounded, int* reverb) {
    *reverb = sReverb;
    *copies = sCopyCount;
    *poolBytes = sPoolUsed;
    *rounded = sRounded;
}

static int8_t sWaveTable[32] __attribute__((aligned(32)));
static int8_t sNoise7Table[127 * 4] __attribute__((aligned(32)));

void DsVoicesReset(void) {
    memset(sRegs, 0, sizeof(sRegs));
}

// The timer value for a rate of DS_SOUND_CLOCK / ticks.
static uint16_t Timer(uint64_t ticks) {
    return (uint16_t)(0x10000 - (ticks < 1 ? 1 : ticks > 0xFFFF ? 0xFFFF : ticks));
}

/*
 * Volume (with the divider) and pan for levels left and right (FULL = full
 * scale on that side): the DS gives a channel at volume v and pan p
 * v * (128 - p) / 128 on the left and v * p / 128 on the right.
 */
static uint32_t Level(uint32_t left, uint32_t right) {
    static const int shift[4] = { 0, 1, 2, 4 };
    uint32_t sum = left + right, volume = 0, pan;
    int div;

    if (sum == 0) {
        return DS_CNT_PAN(64);
    }
    for (div = 3; div > 0; div--) {
        if (((uint64_t)sum << shift[div]) / 512 <= 127) {
            break;
        }
    }
    volume = (uint32_t)(((uint64_t)sum << shift[div]) / 512);
    volume = volume > 127 ? 127 : volume;
    pan = (uint32_t)((uint64_t)right * 128 / sum);
    pan = pan > 127 ? 127 : pan;
    return DS_CNT_VOLUME(volume) | (uint32_t)div << 8 | DS_CNT_PAN(pan);
}

static void Set(DsSoundFrame* frame, int ch, const DsChannelRegs* regs, bool start) {
    DsChannelRegs* last = &sRegs[ch];

    if (!(regs->cnt & DS_CNT_START)) {
        if (last->cnt & DS_CNT_START) {
            memset(last, 0, sizeof(*last));
            frame->write |= 1 << ch;
        }
        frame->ch[ch] = *last;
        return;
    }
    if (start || !(last->cnt & DS_CNT_START) || regs->src != last->src || regs->loop != last->loop ||
        regs->length != last->length) {
        frame->start |= 1 << ch;
        frame->write |= 1 << ch;
    } else if (regs->cnt != last->cnt || regs->timer != last->timer) {
        frame->write |= 1 << ch;
    }
    *last = *regs;
    frame->ch[ch] = *regs;
}

// ---- DirectSound -----------------------------------------------------------------

// A copy of wav whose loop starts on a word and is whole words long, or NULL.
static int Copy(const WaveData* wav) {
    uint32_t loopStart = wav->loopStart, loopLength = wav->size - wav->loopStart;
    uint32_t pad = (4 - (loopStart & 3)) & 3, repeat = (loopLength & 1) ? 4 : (loopLength & 2) ? 2 : 1;
    uint32_t size = pad + loopStart + loopLength * repeat;
    int8_t* out;

    for (int i = 0; i < sCopyCount; i++) {
        if (sCopies[i].wav == wav) {
            return i;
        }
    }
    if (sCopyCount == COPIES || sPoolUsed + ((size + 31) & ~31u) > COPY_POOL) {
        return -1;
    }
    out = &sPool[sPoolUsed];
    memset(out, 0, pad);
    memcpy(out + pad, wav->data, loopStart);
    for (uint32_t r = 0; r < repeat; r++) {
        memcpy(out + pad + loopStart + r * loopLength, wav->data + loopStart, loopLength);
    }
    DsSoundFlush(out, size);
    sCopies[sCopyCount].wav = wav;
    sCopies[sCopyCount].data = out;
    sCopies[sCopyCount].loop = (pad + loopStart) / 4;
    sCopies[sCopyCount].length = loopLength * repeat / 4;
    sPoolUsed += (size + 31) & ~31u;
    return sCopyCount++;
}

// The DS's view of the channel's sample: where it is, the loop, one-shot or not.
static void Sample(const SoundChannel* chan, DsChannelRegs* regs) {
    const WaveData* wav = chan->wav;
    bool loop = (chan->statusFlags & SOUND_CHANNEL_SF_LOOP) != 0 && wav->loopStart < wav->size;

    regs->src = (uintptr_t)wav->data;
    if (!loop) {
        regs->cnt |= DS_CNT_ONE_SHOT;
        regs->loop = 0;
        regs->length = wav->size / 4;
        return;
    }
    regs->cnt |= DS_CNT_LOOP;
    if (((wav->loopStart | wav->size) & 3) != 0 || (regs->src & 3) != 0) {
        int copy = Copy(wav);
        if (copy >= 0) {
            regs->src = (uintptr_t)sCopies[copy].data;
            regs->loop = sCopies[copy].loop;
            regs->length = sCopies[copy].length;
            return;
        }
        // No room: the loop rounded to words.
        sRounded++;
    }
    regs->loop = wav->loopStart / 4;
    regs->length = wav->size / 4 - regs->loop;
}

/*
 * MixChannelAuthentic's effect on the channel without the mixing: the position
 * after samples engine samples, the loop taken, or the end (statusFlags 0).
 */
static void Advance(const SoundInfo* soundInfo, SoundChannel* chan, s32 samples) {
    const WaveData* wav = chan->wav;
    s32 loopLength = (chan->statusFlags & SOUND_CHANNEL_SF_LOOP) ? wav->size - wav->loopStart : 0;
    s32 count = chan->count;
    const s8* p = (const s8*)chan->currentPointer;
    s32 advance;

    if (chan->type & TONEDATA_TYPE_FIX) {
        advance = samples;
    } else {
        uint64_t total = chan->fw + (uint64_t)(u32)(soundInfo->divFreq * chan->frequency) * samples;
        advance = (s32)(total >> 23);
        chan->fw = (u32)total & 0x7FFFFF;
    }
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
        p = wav->data + wav->loopStart + over;
    } else {
        p += advance;
    }
    chan->count = count;
    chan->currentPointer = (PtrU32)p;
}

static void DirectSound(SoundInfo* soundInfo, const PsgMix* mix, int samples, DsSoundFrame* frame) {
    // The engine's channels past the DS's eight (TMC sets eight) keep their
    // bookkeeping but are not heard.
    for (int i = 0; i < MAX_DIRECTSOUND_CHANNELS; i++) {
        SoundChannel* chan = &soundInfo->chans[i];
        DsChannelRegs regs = { 0 };
        bool started = false;

        if (i < soundInfo->maxChannels && SoundEnvelopeStep(soundInfo, chan, &started)) {
            uint32_t e = chan->envelopeVolumeRight;
            uint64_t ticks;

            // The rate the mixer steps through the sample at.
            if (chan->type & TONEDATA_TYPE_FIX) {
                ticks = (DS_SOUND_CLOCK + soundInfo->pcmFreqency / 2) / soundInfo->pcmFreqency;
            } else {
                uint64_t step = (u32)(soundInfo->divFreq * chan->frequency);
                uint64_t rate = step * (uint64_t)soundInfo->pcmFreqency; // in 2^-23 Hz
                ticks = rate == 0 ? 0xFFFF : (((uint64_t)DS_SOUND_CLOCK << 23) + rate / 2) / rate;
            }
            regs.cnt = DS_CNT_START | DS_CNT_PCM8 | Level(e * mix->directSound[0] * 16, e * mix->directSound[1] * 16);
            regs.timer = Timer(ticks);
            Sample(chan, &regs);
            Advance(soundInfo, chan, samples);
        }
        if (i < DIRECT_CHANNELS) {
            Set(frame, i, &regs, started);
        }
    }
}

// ---- CGB ---------------------------------------------------------------------

// Left and right levels of a CGB channel with an amplitude of `unit` per step of
// level (FULL-based) at the mix's volume, routed by NR51.
static uint32_t CgbLevel(const PsgMix* mix, int ch, uint32_t unit) {
    uint32_t left = (mix->nr51 >> (4 + ch)) & 1 ? unit * mix->volume[0] : 0;
    uint32_t right = (mix->nr51 >> ch) & 1 ? unit * mix->volume[1] : 0;

    return Level(left, right);
}

static void Cgb(int samples, int rate, const PsgMix* mix, const PsgVoice* voices, DsSoundFrame* frame) {
    static const uint8_t duty[4] = { 0, 1, 3, 5 }; // 12.5, 25, 50, 75%
    static uint8_t sWaveLevels[32];
    static uint8_t sNoise7Made;
    DsChannelRegs regs;

    (void)samples;
    (void)rate;
    // The squares: a DS PSG step is an eighth of the period, 131072 / (2048 - x) Hz.
    for (int ch = 0; ch < 2; ch++) {
        const PsgVoice* v = &voices[ch];

        memset(&regs, 0, sizeof(regs));
        if (v->on) {
            regs.cnt = DS_CNT_START | DS_CNT_PSG | DS_CNT_DUTY(duty[v->duty]) | CgbLevel(mix, ch, v->volume * 16);
            regs.timer = Timer(((uint64_t)DS_SOUND_CLOCK * (2048 - v->frequency) + 524288) / 1048576);
        }
        Set(frame, SQUARE_CHANNEL + ch, &regs, v->triggered);
    }

    // The wave channel: 32 samples a period of 65536 / (2048 - x) Hz.
    memset(&regs, 0, sizeof(regs));
    if (voices[2].on) {
        if (memcmp(sWaveLevels, voices[2].wave, 32) != 0) {
            int sum = 0;

            // Centred on its mean, as the GBA's output capacitor leaves it
            // (the steps' DC would thump at every start and stop).
            memcpy(sWaveLevels, voices[2].wave, 32);
            for (int k = 0; k < 32; k++) {
                sum += sWaveLevels[k] * 17;
            }
            for (int k = 0; k < 32; k++) {
                int s = sWaveLevels[k] * 17 - sum / 32;
                sWaveTable[k] = (int8_t)(s > 127 ? 127 : s < -128 ? -128 : s);
            }
            DsSoundFlush(sWaveTable, sizeof(sWaveTable));
        }
        regs.cnt = DS_CNT_START | DS_CNT_PCM8 | DS_CNT_LOOP | CgbLevel(mix, 2, FULL / 272);
        regs.src = (uintptr_t)sWaveTable;
        regs.length = sizeof(sWaveTable) / 4;
        regs.timer = Timer(((uint64_t)DS_SOUND_CLOCK * (2048 - voices[2].frequency) + 1048576) / 2097152);
    }
    Set(frame, WAVE_CHANNEL, &regs, voices[2].triggered);

    // Noise: clocked at 524288 / r / 2^(s + 1) Hz (r = 0 counts as 0.5); none
    // at s >= 14.
    {
        const PsgVoice* v = &voices[3];
        int nr43 = v->noise, s = nr43 >> 4, r = nr43 & 7;
        bool seven = (nr43 & 8) != 0;
        DsChannelRegs off = { 0 };
        uint64_t ticks = ((uint64_t)DS_SOUND_CLOCK * (r ? 2 * r : 1) * (2u << s) + 524288) / 1048576;

        memset(&regs, 0, sizeof(regs));
        if (v->on && s < 14) {
            regs.cnt = DS_CNT_START | CgbLevel(mix, 3, v->volume * 16);
            regs.timer = Timer(ticks);
            if (seven) {
                // The 7-bit sequence from the trigger's state, four times (whole words).
                if (!sNoise7Made) {
                    uint16_t lfsr = 0x40;
                    for (int k = 0; k < 127; k++) {
                        int out = lfsr & 1;
                        lfsr >>= 1;
                        if (out) {
                            lfsr ^= 0x60;
                        }
                        for (int r4 = 0; r4 < 4; r4++) {
                            sNoise7Table[r4 * 127 + k] = out ? 127 : -128;
                        }
                    }
                    DsSoundFlush(sNoise7Table, sizeof(sNoise7Table));
                    sNoise7Made = 1;
                }
                regs.cnt |= DS_CNT_PCM8 | DS_CNT_LOOP;
                regs.src = (uintptr_t)sNoise7Table;
                regs.length = sizeof(sNoise7Table) / 4;
            } else {
                regs.cnt |= DS_CNT_PSG;
            }
        }
        Set(frame, NOISE7_CHANNEL, seven ? &regs : &off, v->triggered);
        Set(frame, NOISE_CHANNEL, seven ? &off : &regs, v->triggered);
    }
}

void DsVoicesRun(int samples, DsSoundFrame* frame) {
    SoundInfo* soundInfo = SOUND_INFO_PTR;
    PsgVoice voices[4];
    PsgMix mix;

    frame->write = frame->start = 0;
    sReverb = soundInfo->reverb > sReverb ? soundInfo->reverb : sReverb;
    PsgAdvanceFixed(samples, soundInfo->pcmFreqency);
    PsgVoices(voices, &mix);
    DirectSound(soundInfo, &mix, samples, frame);
    Cgb(samples, soundInfo->pcmFreqency, &mix, voices, frame);

    // What was written, but where the samples are, which differs by platform.
    sCrc = HwCrc32(sCrc, &frame->write, sizeof(frame->write));
    sCrc = HwCrc32(sCrc, &frame->start, sizeof(frame->start));
    for (int ch = 0; ch < DS_SOUND_CHANNELS; ch++) {
        if (frame->write >> ch & 1) {
            const DsChannelRegs* r = &frame->ch[ch];
            sCrc = HwCrc32(sCrc, &r->cnt, 4);
            sCrc = HwCrc32(sCrc, &r->timer, 2);
            sCrc = HwCrc32(sCrc, &r->loop, 2);
            sCrc = HwCrc32(sCrc, &r->length, 4);
        }
    }
    sRuns++;
}

void DsVoicesStop(DsSoundFrame* frame) {
    static const DsChannelRegs sOff;

    frame->write = frame->start = 0;
    for (int ch = 0; ch < DS_SOUND_CHANNELS; ch++) {
        Set(frame, ch, &sOff, false);
    }
}
