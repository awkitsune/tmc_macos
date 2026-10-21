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
 * Each channel's 4-bit level is box-filtered over a few sub-samples per output
 * sample and mixed by NR51/NR50 and the PSG ratio in SOUNDCNT_H.
 */

#include <math.h>
#include <string.h>

#include "global.h"
#include "port/hw.h"
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
} PsgChannel;

static PsgChannel sChannels[4];
static double sSequencerTime; // seconds since the last 512 Hz step
static int sSequencerStep;

static const uint8_t sNrx1[4] = { REG_OFFSET_NR11, REG_OFFSET_NR21, REG_OFFSET_NR31, REG_OFFSET_NR41 };
static const uint8_t sNrx2[4] = { REG_OFFSET_NR12, REG_OFFSET_NR22, REG_OFFSET_NR32, REG_OFFSET_NR42 };
static const uint8_t sNrx3[4] = { REG_OFFSET_NR13, REG_OFFSET_NR23, REG_OFFSET_NR33, REG_OFFSET_NR43 };
static const uint8_t sNrx4[4] = { REG_OFFSET_NR14, REG_OFFSET_NR24, REG_OFFSET_NR34, REG_OFFSET_NR44 };

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
        c->lfsr = 0x7FFF;
        c->phase = 0;
    }
    if (ch == 0) {
        int period = (REG(NR10) >> 4) & 7;
        c->shadow = Frequency(0);
        c->sweepTimer = period ? period : 8;
        c->sweepOn = period != 0 || (REG(NR10) & 7) != 0;
        if ((REG(NR10) & 7) && SweepNext(c) > 2047) {
            c->on = false;
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
                } else if (REG(NR10) & 7) {
                    c->shadow = next;
                    REG(NR13) = next & 0xFF;
                    REG(NR14) = (REG(NR14) & ~7) | ((next >> 8) & 7);
                    if (SweepNext(c) > 2047) {
                        c->on = false;
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
                } else if (!c->envelopeUp && c->volume > 0) {
                    c->volume--;
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
            return (~c->lfsr & 1) ? c->volume : 0;
        }
        c->phase += dt * 524288.0 / r / (2 << s);
        sum = (~c->lfsr & 1) ? c->volume : 0;
        n = 1;
        while (c->phase >= 1) {
            int bit = (c->lfsr ^ (c->lfsr >> 1)) & 1;
            c->phase -= 1;
            c->lfsr = (c->lfsr >> 1) | (bit << 14);
            if (nr43 & 8) {
                c->lfsr = (c->lfsr & ~0x40) | (bit << 6);
            }
            sum += (~c->lfsr & 1) ? c->volume : 0;
            n++;
        }
        return sum / n;
    }
}

void PsgRender(float* stereo, int frames, int sampleRate) {
    static const float ratio[4] = { 0.25f, 0.5f, 1.0f, 0.0f };
    int oversample = (96000 + sampleRate - 1) / sampleRate;
    double dt = 1.0 / ((double)sampleRate * oversample);
    int nr50, nr51;
    float psgRatio, volL, volR;

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
    psgRatio = ratio[REG(SOUNDCNT_H) & 3];
    volR = (float)((nr50 & 7) + 1) * psgRatio;
    volL = (float)(((nr50 >> 4) & 7) + 1) * psgRatio;
    if (!(REG(NR52) & 0x80)) {
        volL = volR = 0;
    }

    for (int i = 0; i < frames; i++) {
        float left = 0, right = 0;

        for (int k = 0; k < oversample; k++) {
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
        }
        stereo[2 * i] += left * volL / oversample;
        stereo[2 * i + 1] += right * volR / oversample;
    }

    REG(NR52) = (REG(NR52) & 0x80) | (sChannels[0].on ? 1 : 0) | (sChannels[1].on ? 2 : 0) | (sChannels[2].on ? 4 : 0) |
                (sChannels[3].on ? 8 : 0);
}
