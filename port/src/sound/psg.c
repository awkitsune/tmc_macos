/*
 * The GBA's four CGB (Game Boy) sound channels: two squares, wave and noise.
 *
 * The m4a engine's CgbSound drives them through the sound registers, which are
 * plain memory natively (gHwIo). Once per frame, after CgbSound, PsgRender picks
 * up what it wrote and synthesises the frame:
 *
 *  - a set NRx4 bit 7 is a trigger; it is consumed (cleared) here;
 *  - length bits in NRx1 are write-only on hardware, so a nonzero value means a
 *    fresh write: it loads the length counter and is cleared, which lets a
 *    retrigger without a new length keep counting as on hardware;
 *  - frequency, duty, panning, wave volume and the wave RAM are read live;
 *  - envelopes, lengths and channel 1's sweep run on the 512 Hz frame sequencer;
 *  - NR52's channel bits are kept up to date, since CgbSound reads them to see
 *    a channel whose length ran out.
 *
 * Each channel's 4-bit level is mixed by NR51/NR50 and the PSG ratio in
 * SOUNDCNT_H, added to the DirectSound output, clipped to the DAC's 10-bit range
 * around SOUNDBIAS, and box-filtered over a few sub-samples per output sample.
 * port/notes/audio-audit.md compares all of it with GBATEK.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "port/hw.h"
#include "port/state.h"
#include "sound_internal.h"

#define REG(offset) gHwIo[REG_OFFSET_##offset]

typedef struct PsgChannel {
    bool on;
    int length;
    int volume;
    int envelopeUp;
    int envelopePeriod;
    int envelopeTimer;
    double phase; // fraction of a duty cycle / wave period, or noise clock
    // Channel 1 sweep.
    int sweepTimer;
    int shadow;
    bool sweepOn;
    // Noise.
    uint16_t lfsr;
    bool noiseOut;
} PsgChannel;

static PsgChannel sChannels[4];
static double sSequencerTime; // seconds since the last 512 Hz step
static int sSequencerStep;

static const uint8_t sNrx1[4] = { REG_OFFSET_NR11, REG_OFFSET_NR21, REG_OFFSET_NR31, REG_OFFSET_NR41 };
static const uint8_t sNrx2[4] = { REG_OFFSET_NR12, REG_OFFSET_NR22, REG_OFFSET_NR32, REG_OFFSET_NR42 };
static const uint8_t sNrx3[4] = { REG_OFFSET_NR13, REG_OFFSET_NR23, REG_OFFSET_NR33, REG_OFFSET_NR43 };
static const uint8_t sNrx4[4] = { REG_OFFSET_NR14, REG_OFFSET_NR24, REG_OFFSET_NR34, REG_OFFSET_NR44 };

// ---- TMC_SOUND_TRACE ---------------------------------------------------------
// TMC_SOUND_TRACE=path writes, on exit, which register values and channel
// features a run used (the evidence in port/notes/audio-audit.md).

typedef struct TraceHist {
    const char* name;
    int count;
    uint32_t value[64];
    uint64_t hits[64];
} TraceHist;

enum {
    T_NR50, T_NR51, T_SOUNDCNT_H, T_SOUNDCNT_X, T_SOUNDBIAS,
    T_TRIGGER, T_DUTY, T_NR10, T_ENVELOPE, T_LENGTH, T_NR30, T_NR32, T_NR43, T_WAVE, T_NR3X_FRAME,
    T_EVENT, T_COUNT
};
static TraceHist sTrace[T_COUNT] STATE_EXCLUDE = {
    { "NR50" }, { "NR51" }, { "SOUNDCNT_H" }, { "SOUNDCNT_X" }, { "SOUNDBIAS" },
    { "trigger channel" }, { "ch1/2 duty (ch<<4|duty)" }, { "NR10 at ch1 trigger" },
    { "NRx2 at trigger (ch<<8|NRx2)" }, { "length at trigger (ch<<12|enable<<8|bits)" }, { "NR30 at ch3 trigger" },
    { "NR32 at ch3 trigger" }, { "NR43 at ch4 trigger" }, { "distinct wave RAM at ch3 trigger (hash)" },
    { "NR30<<8|NR32 in frames with ch3 on" }, { "event" },
};
static bool sTraceOn STATE_EXCLUDE;
static const char* sTracePath STATE_EXCLUDE;
// One hex digit per frame: the CGB channels audible at its start or end (bit 0 =
// channel 1).
static char* sTraceFrames STATE_EXCLUDE;
static size_t sTraceFrameCount STATE_EXCLUDE, sTraceFrameCapacity STATE_EXCLUDE;
static int sTraceFrameMask STATE_EXCLUDE;

enum {
    E_SWEEP_WRITE, E_SWEEP_OFF_TRIGGER, E_SWEEP_OFF_STEP, E_LENGTH_END_1, E_LENGTH_END_2, E_LENGTH_END_3,
    E_LENGTH_END_4, E_ENVELOPE_UP, E_ENVELOPE_DOWN, E_SUBSAMPLES, E_CLIPPED, E_NOISE_S14
};
static const char* const sEventNames[] = {
    "ch1 sweep frequency write", "ch1 off by sweep overflow at trigger", "ch1 off by sweep overflow in a step",
    "ch1 length ran out", "ch2 length ran out", "ch3 length ran out", "ch4 length ran out",
    "envelope step up", "envelope step down", "DAC sub-samples", "DAC sub-samples clipped",
    "ch4 output with s >= 14",
};

static void TraceCount(int hist, uint32_t value) {
    TraceHist* h = &sTrace[hist];

    if (!sTraceOn) {
        return;
    }
    for (int i = 0; i < h->count; i++) {
        if (h->value[i] == value) {
            h->hits[i]++;
            return;
        }
    }
    if (h->count < 64) {
        h->value[h->count] = value;
        h->hits[h->count++] = 1;
    }
}

static void TraceWrite(void) {
    FILE* f = fopen(sTracePath, "w");

    if (f == NULL) {
        return;
    }
    for (int t = 0; t < T_COUNT; t++) {
        TraceHist* h = &sTrace[t];
        fprintf(f, "%s:%s\n", h->name, h->count == 64 ? " (first 64 values)" : "");
        for (int i = 0; i < h->count; i++) {
            if (t == T_EVENT) {
                fprintf(f, "  %-44s %llu\n", sEventNames[h->value[i]], (unsigned long long)h->hits[i]);
            } else {
                fprintf(f, "  %08x %llu\n", h->value[i], (unsigned long long)h->hits[i]);
            }
        }
    }
    fprintf(f, "frames (CGB channels audible, one hex digit per frame):\n");
    fwrite(sTraceFrames, 1, sTraceFrameCount, f);
    fprintf(f, "\n");
    fclose(f);
}

static void TraceStart(void) {
    static bool checked STATE_EXCLUDE;

    if (!checked) {
        checked = true;
        sTracePath = getenv("TMC_SOUND_TRACE");
        if (sTracePath != NULL && sTracePath[0] != '\0') {
            sTraceOn = true;
            atexit(TraceWrite);
        }
    }
}

#define TRACE(hist, value) TraceCount(hist, value)
#define TRACE_EVENT(e) TraceCount(T_EVENT, e)

static void TraceTrigger(int ch) {
    if (!sTraceOn) {
        return;
    }
    TRACE(T_TRIGGER, ch + 1);
    TRACE(T_LENGTH, ((ch + 1) << 12) | (((gHwIo[sNrx4[ch]] >> 6) & 1) << 8) |
                        (ch == 2 ? gHwIo[sNrx1[ch]] : gHwIo[sNrx1[ch]] & 0x3F));
    if (ch < 2) {
        TRACE(T_DUTY, ((ch + 1) << 4) | (gHwIo[sNrx1[ch]] >> 6));
    }
    if (ch == 0) {
        TRACE(T_NR10, REG(NR10));
    }
    if (ch != 2) {
        TRACE(T_ENVELOPE, ((ch + 1) << 8) | gHwIo[sNrx2[ch]]);
    } else {
        uint32_t hash = 2166136261u;
        TRACE(T_NR30, REG(NR30));
        TRACE(T_NR32, REG(NR32));
        for (int i = 0; i < 16; i++) {
            hash = (hash ^ gHwIo[REG_OFFSET_WAVE_RAM0 + i]) * 16777619u;
        }
        TRACE(T_WAVE, hash);
    }
    if (ch == 3) {
        TRACE(T_NR43, REG(NR43));
    }
}

// The CGB channels that are on with a nonzero volume.
static int TraceAudible(void) {
    int mask = 0;

    for (int ch = 0; ch < 4; ch++) {
        bool loud = ch == 2 ? (REG(NR32) & 0xE0) != 0 : sChannels[ch].volume > 0;
        mask |= sChannels[ch].on && loud ? 1 << ch : 0;
    }
    return mask;
}

static void TraceFrame(void) {
    TraceStart();
    if (!sTraceOn) {
        return;
    }
    TRACE(T_NR50, REG(NR50));
    TRACE(T_NR51, REG(NR51));
    TRACE(T_SOUNDCNT_H, REG(SOUNDCNT_H) | (gHwIo[REG_OFFSET_SOUNDCNT_H + 1] << 8));
    TRACE(T_SOUNDCNT_X, REG(SOUNDCNT_X) & 0x80);
    TRACE(T_SOUNDBIAS, REG(SOUNDBIAS_L) | (REG(SOUNDBIAS_H) << 8));
    if (sChannels[2].on) {
        TRACE(T_NR3X_FRAME, (REG(NR30) << 8) | REG(NR32));
    }
    sTraceFrameMask = TraceAudible();
}

static void TraceFrameEnd(void) {
    int mask = sTraceFrameMask | TraceAudible();

    if (!sTraceOn) {
        return;
    }
    if (sTraceFrameCount == sTraceFrameCapacity) {
        sTraceFrameCapacity = sTraceFrameCapacity ? 2 * sTraceFrameCapacity : 65536;
        sTraceFrames = realloc(sTraceFrames, sTraceFrameCapacity);
    }
    sTraceFrames[sTraceFrameCount++] = "0123456789abcdef"[mask];
}

// The DAC sum's sub-samples, and those of them that leave the 10-bit range.
#define TRACE_FRAME() TraceFrame()
#define TRACE_SAMPLE(v, low, high)                     \
    do {                                               \
        if (sTraceOn) {                                \
            TRACE_EVENT(E_SUBSAMPLES);                 \
            if ((v) < (low) || (v) > (high)) {         \
                TRACE_EVENT(E_CLIPPED);                \
            }                                          \
        }                                              \
    } while (0)

void PsgReset(void) {
    memset(sChannels, 0, sizeof(sChannels));
    sSequencerTime = 0;
    sSequencerStep = 0;
}

bool PsgActive(void) {
    for (int i = 0; i < 4; i++) {
        PsgChannel* c = &sChannels[i];
        if (!c->on) {
            continue;
        }
        if (i == 2 ? (REG(NR32) & 0xE0) != 0 : c->volume > 0 || (c->envelopeUp && c->envelopePeriod != 0)) {
            return true;
        }
    }
    return false;
}

static int Frequency(int ch) {
    return gHwIo[sNrx3[ch]] | ((gHwIo[sNrx4[ch]] & 7) << 8);
}

static bool DacOn(int ch) {
    return ch == 2 ? (REG(NR30) & 0x80) != 0 : (gHwIo[sNrx2[ch]] & 0xF8) != 0;
}

static int SweepNext(PsgChannel* c) {
    int shift = REG(NR10) & 7;
    int delta = c->shadow >> shift;

    return (REG(NR10) & 8) ? c->shadow - delta : c->shadow + delta;
}

static void Trigger(int ch) {
    PsgChannel* c = &sChannels[ch];
    int lengthBits = ch == 2 ? gHwIo[sNrx1[ch]] : gHwIo[sNrx1[ch]] & 0x3F;
    int maxLength = ch == 2 ? 256 : 64;

    TraceTrigger(ch);

    if (lengthBits != 0) {
        c->length = maxLength - lengthBits;
        gHwIo[sNrx1[ch]] &= ch == 2 ? 0 : 0xC0;
    } else if (c->length == 0) {
        c->length = maxLength;
    }
    c->on = DacOn(ch);
    if (ch == 2) {
        c->phase = 0;
        return;
    }
    c->volume = gHwIo[sNrx2[ch]] >> 4;
    c->envelopeUp = (gHwIo[sNrx2[ch]] & 8) != 0;
    c->envelopePeriod = gHwIo[sNrx2[ch]] & 7;
    c->envelopeTimer = c->envelopePeriod ? c->envelopePeriod : 8;
    if (ch == 3) {
        c->lfsr = (REG(NR43) & 8) ? 0x40 : 0x4000;
        c->noiseOut = false;
        c->phase = 0;
    }
    if (ch == 0) {
        int period = (REG(NR10) >> 4) & 7;
        c->shadow = Frequency(0);
        c->sweepTimer = period ? period : 8;
        c->sweepOn = period != 0 || (REG(NR10) & 7) != 0;
        if ((REG(NR10) & 7) && SweepNext(c) > 2047) {
            c->on = false;
            TRACE_EVENT(E_SWEEP_OFF_TRIGGER);
        }
    }
}

static void SequencerStep(void) {
    int step = sSequencerStep;

    sSequencerStep = (sSequencerStep + 1) & 7;
    if ((step & 1) == 0) {
        // Length counters, 256 Hz.
        for (int ch = 0; ch < 4; ch++) {
            PsgChannel* c = &sChannels[ch];
            if ((gHwIo[sNrx4[ch]] & 0x40) && c->length > 0 && --c->length == 0) {
                if (c->on) {
                    TRACE_EVENT(E_LENGTH_END_1 + ch);
                }
                c->on = false;
            }
        }
    }
    if (step == 2 || step == 6) {
        // Channel 1 sweep, 128 Hz.
        PsgChannel* c = &sChannels[0];
        int period = (REG(NR10) >> 4) & 7;
        if (--c->sweepTimer <= 0) {
            c->sweepTimer = period ? period : 8;
            if (c->on && c->sweepOn && period) {
                int next = SweepNext(c);
                if (next > 2047) {
                    c->on = false;
                    TRACE_EVENT(E_SWEEP_OFF_STEP);
                } else if (REG(NR10) & 7) {
                    TRACE_EVENT(E_SWEEP_WRITE);
                    c->shadow = next;
                    REG(NR13) = next & 0xFF;
                    REG(NR14) = (REG(NR14) & ~7) | ((next >> 8) & 7);
                    if (SweepNext(c) > 2047) {
                        c->on = false;
                        TRACE_EVENT(E_SWEEP_OFF_STEP);
                    }
                }
            }
        }
    }
    if (step == 7) {
        // Volume envelopes, 64 Hz.
        for (int ch = 0; ch < 4; ch++) {
            PsgChannel* c = &sChannels[ch];
            if (ch == 2 || c->envelopePeriod == 0) {
                continue;
            }
            if (--c->envelopeTimer <= 0) {
                c->envelopeTimer = c->envelopePeriod;
                if (c->envelopeUp && c->volume < 15) {
                    c->volume++;
                    TRACE_EVENT(E_ENVELOPE_UP);
                } else if (!c->envelopeUp && c->volume > 0) {
                    c->volume--;
                    TRACE_EVENT(E_ENVELOPE_DOWN);
                }
            }
        }
    }
}

static const uint8_t sDuty[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
};

// Level 0-15 of a channel over the next dt seconds.
static float Level(int ch, double dt) {
    PsgChannel* c = &sChannels[ch];

    if (ch < 2) {
        int x = Frequency(ch);
        float level = sDuty[gHwIo[sNrx1[ch]] >> 6][(int)(c->phase * 8) & 7] ? c->volume : 0;
        c->phase += dt * 131072.0 / (2048 - x);
        c->phase -= floor(c->phase);
        return level;
    }
    if (ch == 2) {
        static const int shift[4] = { 4, 0, 1, 2 };
        int x = Frequency(2);
        int index = (int)(c->phase * 32) & 31;
        int byte = gHwIo[REG_OFFSET_WAVE_RAM0 + index / 2];
        int sample = (index & 1) ? byte & 0xF : byte >> 4;
        int vol = REG(NR32);
        c->phase += dt * 65536.0 / (2048 - x);
        c->phase -= floor(c->phase);
        if (vol & 0x80) {
            return (sample * 3) >> 2;
        }
        return sample >> shift[(vol >> 5) & 3];
    }
    {
        int nr43 = REG(NR43);
        int s = nr43 >> 4;
        double r = (nr43 & 7) ? (nr43 & 7) : 0.5;
        float sum = 0;
        int n = 0;

        if (s >= 14) {
            TRACE_EVENT(E_NOISE_S14);
            return c->noiseOut ? c->volume : 0;
        }
        c->phase += dt * 524288.0 / r / (2 << s);
        sum = c->noiseOut ? c->volume : 0;
        n = 1;
        while (c->phase >= 1) {
            // GBATEK: shift right; a 1 shifted out sets the output and XORs the
            // register with 6000h (60h for 7 bits).
            c->phase -= 1;
            c->noiseOut = c->lfsr & 1;
            c->lfsr >>= 1;
            if (c->noiseOut) {
                c->lfsr ^= (nr43 & 8) ? 0x60 : 0x6000;
            }
            sum += c->noiseOut ? c->volume : 0;
            n++;
        }
        return sum / n;
    }
}

void PsgRender(float* stereo, int frames, int sampleRate) {
    static const float ratio[4] = { 0.5f, 1.0f, 2.0f, 0.0f };
    int oversample = (96000 + sampleRate - 1) / sampleRate;
    double dt = 1.0 / ((double)sampleRate * oversample);
    int nr50, nr51, bias;
    float psgRatio, volL, volR, low, high;

    for (int ch = 0; ch < 4; ch++) {
        if (gHwIo[sNrx4[ch]] & 0x80) {
            Trigger(ch);
            gHwIo[sNrx4[ch]] &= 0x7F;
        }
        if (!DacOn(ch)) {
            sChannels[ch].on = false;
        }
    }

    nr50 = REG(NR50);
    nr51 = REG(NR51);
    // A channel at full volume spans a quarter of the 10-bit range (GBATEK):
    // level 0-15 times 2 * (NR50 volume + 1) at the 100% ratio.
    psgRatio = ratio[REG(SOUNDCNT_H) & 3];
    volR = (float)((nr50 & 7) + 1) * psgRatio;
    volL = (float)(((nr50 >> 4) & 7) + 1) * psgRatio;
    if (!(REG(NR52) & 0x80)) {
        volL = volR = 0;
    }
    // The sum plus the SOUNDBIAS level is clipped to 0-3FFh (GBATEK). The output
    // resolution (SOUNDBIAS bits 14-15) is not applied: the game's levels are all
    // multiples of 4, which its 8-bit setting keeps exactly.
    bias = (REG(SOUNDBIAS_L) | (REG(SOUNDBIAS_H) << 8)) & 0x3FE;
    low = (float)-bias;
    high = (float)(0x3FF - bias);
    TRACE_FRAME();

    for (int i = 0; i < frames; i++) {
        float ds[2] = { stereo[2 * i], stereo[2 * i + 1] };
        float sum[2] = { 0, 0 };

        for (int k = 0; k < oversample; k++) {
            float left = 0, right = 0;

            sSequencerTime += dt;
            if (sSequencerTime >= 1.0 / 512) {
                sSequencerTime -= 1.0 / 512;
                SequencerStep();
            }
            for (int ch = 0; ch < 4; ch++) {
                float level;
                if (!sChannels[ch].on) {
                    continue;
                }
                level = Level(ch, dt);
                if (nr51 & (0x10 << ch)) {
                    left += level;
                }
                if (nr51 & (0x01 << ch)) {
                    right += level;
                }
            }
            for (int c = 0; c < 2; c++) {
                float v = ds[c] + (c == 0 ? left * volL : right * volR);
                float out = v < low ? low : v > high ? high : v;
                TRACE_SAMPLE(v, low, high);
                sum[c] += out;
            }
        }
        stereo[2 * i] = sum[0] / oversample;
        stereo[2 * i + 1] = sum[1] / oversample;
    }
    TraceFrameEnd();

    REG(NR52) = (REG(NR52) & 0x80) | (sChannels[0].on ? 1 : 0) | (sChannels[1].on ? 2 : 0) | (sChannels[2].on ? 4 : 0) |
                (sChannels[3].on ? 8 : 0);
}
