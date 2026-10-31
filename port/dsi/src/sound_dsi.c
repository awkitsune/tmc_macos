/*
 * Sound. By default (DSI_SOUND=hw) the m4a engine's voices play on the DS's
 * own channels: each frame SoundMain turns them into the channels' registers
 * (port/src/sound/dsvoices.c) and DsiSoundChannels hands them to the ARM7
 * (port/dsi/arm7), which writes them. When no frame comes for a few DS frames
 * (the engine off for a room load, a stall) the channels stop, as the GBA's
 * DMA does, and start over with the next frame.
 *
 * With DSI_SOUND=mix: the game's mix, streamed to two DS sound channels.
 *
 * The ARM9 mixes each frame as the GBA would (port/src/sound/m4a_mixer.c and
 * psg.c, PsgRenderDsi) and hands the 16-bit stereo samples here. They go into
 * a ring the two channels loop over, left and right, with the default ARM7
 * program starting them (libnds soundPlaySampleChannel). The channels play
 * the ring at the frame's sample count per DS frame, which is where the game's
 * frames come from, so writing keeps pace with playing.
 *
 * Timers 2 and 3 count the samples played: timer 2 overflows once per sample
 * period of the channels (the sound clock is the bus clock halved, so the same
 * crystal), and timer 3 counts its overflows. Writing stays a few frames ahead
 * of that; a sample is dropped or repeated when the lead drifts, and after a
 * stall (a save) the channels play silence (each frame clears the ring for
 * a few frames past its samples) and writing starts over ahead of them. A long game frame (a room load) does not stall the sound:
 * DsiSoundVBlank runs the game's sound from the DS VBlank interrupt, as the
 * GBA's interrupt does (platform_dsi.c).
 */

#include <nds.h>
#include <string.h>

#include "port/dsvoices.h"

#include "dsi.h"
#include "sound_image.h"

static bool sPlaying;
static volatile bool sFrameSince; // a frame came since the last DS VBlank

void m4aSoundMain(void);

#if DSI_SOUND_HW
#define QUIET_VBLANKS 4 // DS frames without a sound frame before the channels stop

static DsiSoundImage sImages[2] __attribute__((aligned(32)));
static int sNextImage;
static int sQuiet;
uint32_t gDsiSoundSends;

void DsSoundFlush(const void* data, uint32_t size) {
    DC_FlushRange(data, size);
}

static void Send(const DsSoundFrame* frame) {
    DsiSoundImage* image = &sImages[sNextImage];

    if (frame->write == 0) {
        return;
    }
    sNextImage ^= 1;
    image->write = frame->write;
    image->start = frame->start;
    for (int ch = 0; ch < 16; ch++) {
        if (frame->write >> ch & 1) {
            const DsChannelRegs* r = &frame->ch[ch];

            image->ch[ch] = (DsiSoundChannel){ r->cnt, (uint32_t)r->src, r->timer, r->loop, r->length };
        }
    }
    DC_FlushRange(image, sizeof(*image));
    fifoSendValue32(DSI_SOUND_FIFO, (u32)image);
    gDsiSoundSends++;
}

void DsiSoundChannels(const DsSoundFrame* frame) {
    sFrameSince = true;
    sQuiet = 0;
    if (!sPlaying) {
        static bool sLogged;

        if (!sLogged) {
            DsiLog("sound: the m4a voices on the DS's channels\n");
            sLogged = true;
        }
        soundEnable();
        sPlaying = true;
    }
    Send(frame);
}

void DsiSoundStop(void) {
    int irq = enterCriticalSection();

    if (sPlaying) {
        DsSoundFrame frame;

        DsVoicesStop(&frame);
        Send(&frame);
        sPlaying = false;
    }
    leaveCriticalSection(irq);
}

void DsiSoundVBlank(bool mayRun) {
    if (mayRun && sPlaying && !sFrameSince) {
        m4aSoundMain();
    }
    if (sPlaying && !sFrameSince && ++sQuiet >= QUIET_VBLANKS) {
        DsiSoundStop();
    }
    sFrameSince = false;
}

// The stream path is not used.
void DsiSoundFrame(const int16_t* stereo, int frames) {
    (void)stereo;
    (void)frames;
}
#else
#define CHANNEL_LEFT 0
#define CHANNEL_RIGHT 1
#define RING 4096 // samples per channel; divides timer 3's 65536
#define LEAD_FRAMES 3
#define DRIFT 32  // samples of lead either way before correcting
#define SILENCE_FRAMES 4 // cleared past the written samples, which a stall plays
// A DS frame: 263 lines of 2,130 bus cycles (355 dots of 6), in sound clock
// cycles.
#define SOUND_CYCLES_PER_FRAME (263 * 2130 / 2)

static int16_t sRing[2][RING] __attribute__((aligned(32)));
static int16_t sRing[2][RING] __attribute__((aligned(32)));
static int sFrameLength;
static uint16_t sWrite; // samples written, mod 65536, as timer 3 counts played ones


static uint16_t Played(void) {
    return TIMER_DATA(3);
}

static void Flush(int channel, uint16_t from, int count) {
    uint32_t start = from & (RING - 1);
    uint32_t first = count < (int)(RING - start) ? (uint32_t)count : RING - start;

    DC_FlushRange(&sRing[channel][start], first * 2);
    if ((uint32_t)count > first) {
        DC_FlushRange(&sRing[channel][0], (count - first) * 2);
    }
}

// Silence the ring ahead of the channels and write from LEAD_FRAMES on.
static void Restart(void) {
    sWrite = Played() + LEAD_FRAMES * sFrameLength;
    memset(sRing, 0, sizeof(sRing));
    DC_FlushRange(sRing, sizeof(sRing));
}

static void Start(int frames) {
    // The sample period in sound clock cycles, and the rate libnds turns back
    // into it (SOUND_FREQ rounds 16,756,991 / rate).
    uint32_t period = (SOUND_CYCLES_PER_FRAME + frames / 2) / frames;
    uint16_t rate = (BUS_CLOCK / 2 + period / 2) / period;

    DsiSoundStop();
    sFrameLength = frames;
    memset(sRing, 0, sizeof(sRing));
    DC_FlushRange(sRing, sizeof(sRing));
    soundEnable();
    TIMER_DATA(2) = (uint16_t)(0x10000 - 2 * period);
    TIMER_DATA(3) = 0;
    TIMER_CR(3) = TIMER_ENABLE | TIMER_CASCADE;
    soundPlaySampleChannel(CHANNEL_LEFT, sRing[0], SoundFormat_16Bit, sizeof(sRing[0]), rate, 127, 0, true, 0);
    soundPlaySampleChannel(CHANNEL_RIGHT, sRing[1], SoundFormat_16Bit, sizeof(sRing[1]), rate, 127, 127, true, 0);
    TIMER_CR(2) = TIMER_ENABLE | TIMER_DIV_1;
    sWrite = LEAD_FRAMES * frames;
    sPlaying = true;
    DsiLog("sound: %d samples per frame at %u Hz (period %lu)\n", frames, (unsigned)rate, (unsigned long)period);
}

void DsiSoundStop(void) {
    if (!sPlaying) {
        return;
    }
    soundKill(CHANNEL_LEFT);
    soundKill(CHANNEL_RIGHT);
    TIMER_CR(2) = 0;
    TIMER_CR(3) = 0;
    sPlaying = false;
}

void DsiSoundVBlank(bool mayRun) {
    if (mayRun && sPlaying && !sFrameSince) {
        m4aSoundMain();
    }
    // Still none: the engine is off (m4aSoundVSyncOff, as for every room load)
    // or the game is in a stall. Silence, as the GBA plays, while the lead is
    // short of its mark; the main code is not in DsiSoundFrame (it sets
    // sFrameSince first).
    if (sPlaying && !sFrameSince && (int16_t)(sWrite - Played()) < LEAD_FRAMES * sFrameLength) {
        for (int i = 0; i < sFrameLength; i++) {
            uint32_t at = (uint16_t)(sWrite + i) & (RING - 1);

            sRing[0][at] = 0;
            sRing[1][at] = 0;
        }
        Flush(0, sWrite, sFrameLength);
        Flush(1, sWrite, sFrameLength);
        sWrite += sFrameLength;
    }
    sFrameSince = false;
}

void DsiSoundFrame(const int16_t* stereo, int frames) {
    int16_t lead;
    int count;

    sFrameSince = true;
    if (frames <= 0 || frames > RING / 4) {
        return;
    }
    if (!sPlaying || frames != sFrameLength) {
        Start(frames);
    }
    lead = (int16_t)(sWrite - Played());
    if (lead < frames / 2 || lead > (LEAD_FRAMES + 3) * frames) {
        Restart();
        lead = LEAD_FRAMES * frames;
    }
    // One sample less or one more brings the lead back by a sample a frame.
    count = frames;
    if (lead > LEAD_FRAMES * frames + DRIFT) {
        count--;
    } else if (lead < LEAD_FRAMES * frames - DRIFT) {
        count++;
    }
    for (int i = 0; i < count; i++) {
        int from = i < frames ? i : frames - 1;
        uint32_t at = (uint16_t)(sWrite + i) & (RING - 1);

        sRing[0][at] = stereo[2 * from];
        sRing[1][at] = stereo[2 * from + 1];
    }
    // Past them, silence rather than the ring's old samples.
    for (int i = count; i < count + SILENCE_FRAMES * frames; i++) {
        uint32_t at = (uint16_t)(sWrite + i) & (RING - 1);

        sRing[0][at] = 0;
        sRing[1][at] = 0;
    }
    Flush(0, sWrite, count + SILENCE_FRAMES * frames);
    Flush(1, sWrite, count + SILENCE_FRAMES * frames);
    sWrite += count;
}
#endif // DSI_SOUND_HW
