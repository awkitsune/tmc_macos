#ifndef GUARD_M4A_INTERNAL_PRIVATE_H
#define GUARD_M4A_INTERNAL_PRIVATE_H

// Engine-private m4a types and constants, shared by src/gba/m4a.c and the native
// C version of asm/lib/m4a_asm.s (port/src/sound).

#include "gba/m4a.h"

// ASCII encoding of 'Smsh' in reverse
// This is presumably short for SMASH, the developer of MKS4AGB.
#define ID_NUMBER 'hsmS'

#define C_V 0x40 // center value for PAN, BEND, and TUNE

#define SOUND_MODE_REVERB_VAL 0x0000007F
#define SOUND_MODE_REVERB_SET 0x00000080
#define SOUND_MODE_MAXCHN 0x00000F00
#define SOUND_MODE_MAXCHN_SHIFT 8
#define SOUND_MODE_MASVOL 0x0000F000
#define SOUND_MODE_MASVOL_SHIFT 12
#define SOUND_MODE_FREQ_05734 0x00010000
#define SOUND_MODE_FREQ_07884 0x00020000
#define SOUND_MODE_FREQ_10512 0x00030000
#define SOUND_MODE_FREQ_13379 0x00040000
#define SOUND_MODE_FREQ_15768 0x00050000
#define SOUND_MODE_FREQ_18157 0x00060000
#define SOUND_MODE_FREQ_21024 0x00070000
#define SOUND_MODE_FREQ_26758 0x00080000
#define SOUND_MODE_FREQ_31536 0x00090000
#define SOUND_MODE_FREQ_36314 0x000A0000
#define SOUND_MODE_FREQ_40137 0x000B0000
#define SOUND_MODE_FREQ_42048 0x000C0000
#define SOUND_MODE_FREQ 0x000F0000
#define SOUND_MODE_FREQ_SHIFT 16
#define SOUND_MODE_DA_BIT_9 0x00800000
#define SOUND_MODE_DA_BIT_8 0x00900000
#define SOUND_MODE_DA_BIT_7 0x00A00000
#define SOUND_MODE_DA_BIT_6 0x00B00000
#define SOUND_MODE_DA_BIT 0x00B00000
#define SOUND_MODE_DA_BIT_SHIFT 20

typedef void (*MPlayFunc)();
typedef void (*PlyNoteFunc)(u32, MusicPlayerInfo*, MusicPlayerTrack*);
typedef void (*CgbSoundFunc)(void);
typedef void (*CgbOscOffFunc)(u8);
typedef u32 (*MidiKeyToCgbFreqFunc)(u8, u8, u8);
typedef void (*ExtVolPitFunc)(void);
typedef void (*XcmdFunc)(MusicPlayerInfo*, MusicPlayerTrack*);

typedef struct CgbChannel {
    u8 statusFlags;
    u8 type;
    u8 rightVolume;
    u8 leftVolume;
    u8 attack;
    u8 decay;
    u8 sustain;
    u8 release;
    u8 key;
    u8 envelopeVolume;
    u8 envelopeGoal;
    u8 envelopeCounter;
    u8 echoVolume;
    u8 echoLength;
    u8 unk0;
    u8 unk1;
    u8 gateTime;
    u8 midiKey;
    u8 velocity;
    u8 priority;
    u8 rhythmPan;
    u8 unk2[3];
    u8 unk3;
    u8 sustainGoal;
    u8 n4; // NR[1-4]4 register (initial, length bit)
    u8 pan;
    u8 panMask;
    u8 modify;
    u8 length;
    u8 sweep;
    u32 frequency;
    u32* nextWave;
    u32* currentWave;
    PtrU32 track;
    PtrU32 prev;
    PtrU32 next;
    u8 unk4[8];
} CgbChannel;

#define MAX_DIRECTSOUND_CHANNELS 12

#define PCM_DMA_BUF_SIZE 1584 // size of Direct Sound buffer

typedef struct SoundInfo {
    // This field is normally equal to ID_NUMBER but it is set to other
    // values during sensitive operations for locking purposes.
    // This field should be volatile but isn't. This could potentially cause
    // race conditions.
    u32 ident;

    vu8 pcmDmaCounter;

    // Direct Sound
    u8 reverb;
    u8 maxChannels;
    u8 masterVolume;
    u8 frequency;

    u8 mode;
    u8 c15;          // periodically counts from 14 down to 0 (15 states)
    u8 pcmDmaPeriod; // number of V-blanks per PCM DMA
    u8 maxLines;
    u8 gap[3];
    s32 pcmSamplesPerVBlank;
    s32 pcmFreqency;
    s32 divFreq;
    CgbChannel* cgbChannels;
    MPlayMainFunc MPlayMainHead;
    void* intp;
    void (*CgbSound)(void);
    void (*CgbOscOff)(u8);
    u32 (*MidiKeyToCgbFreq)(u8, u8, u8);
    void* MPlayJumpTable;
    void* plynote;
    void (*ExtVolPit)(void);
    u8 gap2[16];
    SoundChannel chans[MAX_DIRECTSOUND_CHANNELS];
    s8 pcmBuffer[PCM_DMA_BUF_SIZE];
} SoundInfo;

#define MUSICPLAYER_STATUS_TRACK 0x0000ffff
#define MUSICPLAYER_STATUS_PAUSE 0x80000000

#define MAX_MUSICPLAYER_TRACKS 16

#define TEMPORARY_FADE 0x0001
#define FADE_IN 0x0002
#define FADE_VOL_MAX 64
#define FADE_VOL_SHIFT 2

#define SOUND_CHANNEL_SF_START 0x80
#define SOUND_CHANNEL_SF_STOP 0x40
#define SOUND_CHANNEL_SF_LOOP 0x10
#define SOUND_CHANNEL_SF_IEC 0x04
#define SOUND_CHANNEL_SF_ENV 0x03
#define SOUND_CHANNEL_SF_ENV_ATTACK 0x03
#define SOUND_CHANNEL_SF_ENV_DECAY 0x02
#define SOUND_CHANNEL_SF_ENV_SUSTAIN 0x01
#define SOUND_CHANNEL_SF_ENV_RELEASE 0x00
#define SOUND_CHANNEL_SF_ON \
    (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_STOP | SOUND_CHANNEL_SF_IEC | SOUND_CHANNEL_SF_ENV)

#define CGB_CHANNEL_MO_PIT 0x02
#define CGB_CHANNEL_MO_VOL 0x01

#define CGB_NRx2_ENV_DIR_DEC 0x00
#define CGB_NRx2_ENV_DIR_INC 0x08

// Song data holds GBA addresses natively (see SongHeader); M4A_PTR turns one into
// a native pointer. Tracks are cleared up to their command pointer, which is
// what the GBA engine's 64-byte clear covers.
#ifdef PORT_HOSTED
#include <stddef.h>
#include <string.h>
void* M4aGbaPtr(u32 address);
#define M4A_PTR(type, value) ((type)M4aGbaPtr((u32)(PtrU32)(value)))
#define M4A_CLEAR_TRACK(track) memset((track), 0, offsetof(MusicPlayerTrack, cmdPtr))
#else
#define M4A_PTR(type, value) ((type)(value))
#define M4A_CLEAR_TRACK(track) Clear64byte(track)
#endif

#endif // GUARD_M4A_INTERNAL_PRIVATE_H
