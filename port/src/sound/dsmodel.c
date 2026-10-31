/*
 * A model of the DS's sound channels (GBATEK "DS Sound"), playing the
 * registers dsvoices.c writes, so the Mac can check the DSi's hardware sound
 * against the software mixer (TMC_DS_SOUND=1, m4a_mixer.c). Not on the DSi.
 *
 * Each channel steps through its source at DS_SOUND_CLOCK / (0x10000 - timer):
 * 8-bit samples with the loop (no interpolation, as the DS), a square of eight
 * steps with (duty + 1) high, or the 15-bit noise register. Volume, divider
 * and pan as the DS mixer applies them. The output has the authentic mixer's
 * scale: dsvoices.c sets levels at half the scale the DSi streams at, so a
 * 16-bit value y is y / 16384 here; and, as the GBA's and the DS's outputs
 * are AC-coupled, the same 5 Hz high-pass as the mixer's. TMC_DS_MODEL_MASK=hex plays only those
 * channels (bit n = channel n), to find a voice.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "port/dsvoices.h"

static struct {
    DsChannelRegs regs;
    bool on;
    double pos; // source steps since the start
    uint64_t steps;
    uint16_t lfsr;
    bool noiseLow;
} sChannels[DS_SOUND_CHANNELS];

void DsSoundFlush(const void* data, uint32_t size) {
    (void)data;
    (void)size;
}

void DsModelReset(void) {
    memset(sChannels, 0, sizeof(sChannels));
}

static int Value(int ch) {
    typeof(sChannels[0])* c = &sChannels[ch];
    uint32_t cnt = c->regs.cnt;
    uint64_t step = (uint64_t)c->pos;

    if ((cnt >> 29 & 3) == 3) {
        if (ch >= 14) {
            // A noise step per timer tick: X >> 1, on a carry X ^= 6000h and low.
            while (c->steps < step) {
                bool carry = c->lfsr & 1;
                c->lfsr >>= 1;
                if (carry) {
                    c->lfsr ^= 0x6000;
                }
                c->noiseLow = carry;
                c->steps++;
            }
            return c->noiseLow ? -0x7FFF : 0x7FFF;
        }
        if (ch >= 8) {
            int duty = cnt >> 24 & 7;
            return (int)(step % 8) >= 7 - duty ? 0x7FFF : -0x7FFF;
        }
        return 0;
    }
    {
        uint64_t start = (uint64_t)c->regs.loop * 4, end = start + (uint64_t)c->regs.length * 4;

        if (step >= end) {
            if ((cnt >> 27 & 3) != 1 || c->regs.length == 0) {
                c->on = false;
                return 0;
            }
            step = start + (step - start) % (end - start);
        }
        return ((const int8_t*)c->regs.src)[step] * 256;
    }
}

void DsModelRender(const DsSoundFrame* frame, float* stereo, int frames, int rate) {
    static const int shift[4] = { 0, 1, 2, 4 };
    static long sMask = -1;

    if (sMask < 0) {
        sMask = getenv("TMC_DS_MODEL_MASK") != NULL ? strtol(getenv("TMC_DS_MODEL_MASK"), NULL, 16) : 0xFFFF;
    }
    for (int ch = 0; ch < DS_SOUND_CHANNELS; ch++) {
        if (!(frame->write >> ch & 1)) {
            continue;
        }
        if (frame->start >> ch & 1) {
            sChannels[ch].pos = 0;
            sChannels[ch].steps = 0;
            sChannels[ch].lfsr = 0x7FFF;
            sChannels[ch].noiseLow = false;
        }
        sChannels[ch].regs = frame->ch[ch];
        sChannels[ch].on = (frame->ch[ch].cnt & DS_CNT_START) != 0;
    }
    for (int i = 0; i < frames; i++) {
        double left = 0, right = 0;

        for (int ch = 0; ch < DS_SOUND_CHANNELS; ch++) {
            typeof(sChannels[0])* c = &sChannels[ch];
            uint32_t cnt = c->regs.cnt;
            double x;

            if (!c->on) {
                continue;
            }
            x = (sMask >> ch & 1) ? Value(ch) : 0;
            x = x * (cnt & 127) / 128 / (1 << shift[cnt >> 8 & 3]);
            left += x * (128 - (cnt >> 16 & 127)) / 128;
            right += x * (cnt >> 16 & 127) / 128;
            c->pos += (double)DS_SOUND_CLOCK / (0x10000 - c->regs.timer) / rate;
        }
        {
            static double sIn[2], sOut[2];
            double pole = exp(-2 * 3.14159265358979 * 5 / rate), in[2] = { left / 16384, right / 16384 };

            for (int k = 0; k < 2; k++) {
                double out = in[k] - sIn[k] + pole * sOut[k];
                sIn[k] = in[k];
                sOut[k] = out;
            }
            left = sOut[0];
            right = sOut[1];
        }
        stereo[2 * i] = left > 1 ? 1 : left < -1 ? -1 : (float)left;
        stereo[2 * i + 1] = right > 1 ? 1 : right < -1 ? -1 : (float)right;
    }
}
