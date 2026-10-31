/*
 * Sound: the game's mix, streamed to two DS sound channels.
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
 * stall (a long frame, a save) the ring is silenced and writing starts over
 * ahead of the channels.
 */

#include <nds.h>
#include <string.h>

#include "dsi.h"

#define CHANNEL_LEFT 0
#define CHANNEL_RIGHT 1
#define RING 4096 // samples per channel; divides timer 3's 65536
#define LEAD_FRAMES 3
#define DRIFT 32  // samples of lead either way before correcting
// A DS frame: 263 lines of 2,130 bus cycles (355 dots of 6), in sound clock
// cycles.
#define SOUND_CYCLES_PER_FRAME (263 * 2130 / 2)

static int16_t sRing[2][RING] __attribute__((aligned(32)));
static bool sPlaying;
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

void DsiSoundFrame(const int16_t* stereo, int frames) {
    int16_t lead;
    int count;

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
    Flush(0, sWrite, count);
    Flush(1, sWrite, count);
    sWrite += count;
}
