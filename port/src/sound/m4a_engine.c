/*
 * C version of asm/lib/m4a_asm.s: the m4a sequencer (MPlayMain), the song
 * command handlers (ply_*), note allocation and the channel chain helpers.
 * SoundMain, the per-frame entry point, is in m4a_mixer.c.
 *
 * The code follows the assembly instruction by instruction where behaviour
 * depends on it (8-bit wraparound, signed loads, the LFO arithmetic). Struct
 * fields replace the assembly's GBA byte offsets, and GBA addresses read from
 * song data go through M4A_PTR.
 *
 * DirectSound and CGB channels share one chain per track, as on the GBA; the
 * chain is walked through SoundChannel, so the fields both channel types use
 * are checked to sit at the same native offsets.
 */

#include "global.h"
#include "gba/m4a.h"
#include "gba/m4a_internal.h"
#include "port/sound.h"

extern void* gMPlayJumpTable[36];
extern void* const gMPlayJumpTableTemplate[];
extern const u8 gClockTable[];

void ClearChain(void* x);
void TrkVolPitSet(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track);
void FadeOutBody(MusicPlayerInfo* mplayInfo);
u32 MidiKeyToFreq(WaveData* wav, u8 key, u8 fineAdjust);
void ply_fine(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track);

#define CHECK_SAME_OFFSET(field) \
    _Static_assert(offsetof(SoundChannel, field) == offsetof(CgbChannel, field), "chain field " #field)
CHECK_SAME_OFFSET(statusFlags);
CHECK_SAME_OFFSET(type);
CHECK_SAME_OFFSET(rightVolume);
CHECK_SAME_OFFSET(leftVolume);
CHECK_SAME_OFFSET(attack);
CHECK_SAME_OFFSET(key);
CHECK_SAME_OFFSET(echoVolume);
CHECK_SAME_OFFSET(echoLength);
CHECK_SAME_OFFSET(gateTime);
CHECK_SAME_OFFSET(midiKey);
CHECK_SAME_OFFSET(velocity);
CHECK_SAME_OFFSET(priority);
CHECK_SAME_OFFSET(rhythmPan);
CHECK_SAME_OFFSET(frequency);
CHECK_SAME_OFFSET(track);
CHECK_SAME_OFFSET(prev);
CHECK_SAME_OFFSET(next);

#define CHAN_TRACK(chan) ((MusicPlayerTrack*)(chan)->track)
#define CHAN_PREV(chan) ((SoundChannel*)(chan)->prev)
#define CHAN_NEXT(chan) ((SoundChannel*)(chan)->next)

M4aGotoHook gM4aGotoHook;

u32 umul3232H32(u32 multiplier, u32 multiplicand) {
    return (u32)(((u64)multiplier * multiplicand) >> 32);
}

void SoundMainBTM(void* addr) {
    memset(addr, 0, 64);
}

void RealClearChain(void* x) {
    SoundChannel* chan = x;
    MusicPlayerTrack* track = CHAN_TRACK(chan);
    SoundChannel* next;
    SoundChannel* prev;

    if (track == NULL) {
        return;
    }
    next = CHAN_NEXT(chan);
    prev = CHAN_PREV(chan);
    if (prev != NULL) {
        prev->next = (PtrU32)next;
    } else {
        track->chan = next;
    }
    if (next != NULL) {
        next->prev = (PtrU32)prev;
    }
    chan->track = 0;
}

// The chains' "next" step: a channel linked to itself ends the chain.
static SoundChannel* ChainNext(SoundChannel* chan) {
    SoundChannel* next = CHAN_NEXT(chan);

    if (next == chan) {
        chan->next = 0;
        next = NULL;
    }
    return next;
}

void ply_fine(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    SoundChannel* chan;

    for (chan = track->chan; chan != NULL; chan = ChainNext(chan)) {
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON) {
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        }
        RealClearChain(chan);
    }
    track->flags = 0;
}

void MPlayJumpTableCopy(void** mplayJumpTable) {
    for (int i = 0; i < 36; i++) {
        mplayJumpTable[i] = gMPlayJumpTableTemplate[i];
    }
}

static u8 ReadByte(MusicPlayerTrack* track) {
    return *track->cmdPtr++;
}

static u32 ReadGbaAddress(const u8* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

// The jump shared by GOTO, PATT and REPT: the command pointer follows the
// 4-byte GBA address at the command pointer.
static void Jump(MusicPlayerTrack* track) {
    track->cmdPtr = M4A_PTR(u8*, ReadGbaAddress(track->cmdPtr));
}

void ply_goto(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    const u8* from = track->cmdPtr;

    Jump(track);
    if (gM4aGotoHook != NULL) {
        gM4aGotoHook(mplayInfo, track, from, track->cmdPtr);
    }
}

void ply_patt(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    if (track->patternLevel < 3) {
        track->patternStack[track->patternLevel] = track->cmdPtr + 4;
        track->patternLevel++;
        Jump(track);
    } else {
        ply_fine(mplayInfo, track);
    }
}

void ply_pend(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    if (track->patternLevel != 0) {
        track->patternLevel--;
        track->cmdPtr = track->patternStack[track->patternLevel];
    }
}

void ply_rept(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u8* p = track->cmdPtr;

    if (*p == 0) {
        // Repeat forever: a GOTO.
        track->cmdPtr = p + 1;
        ply_goto(mplayInfo, track);
        return;
    }
    track->repN++;
    track->cmdPtr = p + 1;
    if (track->repN < *p) {
        Jump(track);
    } else {
        track->repN = 0;
        track->cmdPtr = p + 5;
    }
}

void ply_prio(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->priority = ReadByte(track);
}

void ply_tempo(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u32 tempo = ReadByte(track) * 2;

    mplayInfo->tempoD = tempo;
    mplayInfo->tempoI = (tempo * mplayInfo->tempoU) >> 8;
}

void ply_keysh(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->keyShift = ReadByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_voice(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->tone = mplayInfo->tone[ReadByte(track)];
}

void ply_vol(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->vol = ReadByte(track);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->pan = ReadByte(track) - C_V;
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->bend = ReadByte(track) - C_V;
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->bendRange = ReadByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->lfoDelay = ReadByte(track);
}

void ply_modt(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u8 modT = ReadByte(track);

    if (track->modT != modT) {
        track->modT = modT;
        track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

void ply_tune(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->tune = ReadByte(track) - C_V;
    track->flags |= MPT_FLG_PITCHG;
}

void ply_port(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u8 reg = ReadByte(track);
    u8 value = ReadByte(track);

    *(vu8*)(REG_ADDR_NR10 + reg) = value;
}

static void clear_modM(MusicPlayerTrack* track) {
    track->modM = 0;
    track->lfoSpeedC = 0;
    track->flags |= track->modT == 0 ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
}

void ply_lfos(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->lfoSpeed = ReadByte(track);
    if (track->lfoSpeed == 0) {
        clear_modM(track);
    }
}

void ply_mod(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->mod = ReadByte(track);
    if (track->mod == 0) {
        clear_modM(track);
    }
}

void ply_endtie(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u8 key = *track->cmdPtr;
    SoundChannel* chan;

    if (key < 0x80) {
        track->key = key;
        track->cmdPtr++;
    } else {
        key = track->key;
    }
    for (chan = track->chan; chan != NULL; chan = ChainNext(chan)) {
        if ((chan->statusFlags & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV)) &&
            !(chan->statusFlags & SOUND_CHANNEL_SF_STOP) && chan->midiKey == key) {
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
            return;
        }
    }
}

void TrackStop(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    SoundChannel* chan;

    if (!(track->flags & MPT_FLG_EXIST)) {
        return;
    }
    for (chan = track->chan; chan != NULL; chan = ChainNext(chan)) {
        if (chan->statusFlags != 0) {
            u8 cgb = chan->type & TONEDATA_TYPE_CGB;
            if (cgb) {
                SOUND_INFO_PTR->CgbOscOff(cgb);
            }
            chan->statusFlags = 0;
        }
        chan->track = 0;
    }
    track->chan = NULL;
}

static void ChnVolSetAsm(SoundChannel* chan, MusicPlayerTrack* track) {
    s32 pan = (s8)chan->rhythmPan;
    u32 v;

    v = (track->volMR * ((0x80 + pan) * chan->velocity)) >> 14;
    chan->rightVolume = v > 0xFF ? 0xFF : v;
    v = (track->volML * ((0x7F - pan) * chan->velocity)) >> 14;
    chan->leftVolume = v > 0xFF ? 0xFF : v;
}

static u8 NoteKey(SoundChannel* chan, MusicPlayerTrack* track) {
    s32 key = chan->key + (s8)track->keyM;
    return key < 0 ? 0 : key;
}

void ply_note(u32 clock, MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    SoundInfo* soundInfo = SOUND_INFO_PTR;
    u8* p;
    ToneData* tone;
    SoundChannel* chan;
    u8 key;
    u32 priority;
    s32 rhythmPan = 0;
    u32 cgb;

    track->gateTime = gClockTable[clock];
    p = track->cmdPtr;
    if (*p < 0x80) {
        track->key = *p++;
        if (*p < 0x80) {
            track->velocity = *p++;
            if (*p < 0x80) {
                track->gateTime += *p++;
            }
        }
        track->cmdPtr = p;
    }

    tone = &track->tone;
    if (tone->type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL)) {
        // Rhythm and key-split voices pick a voice from a sub-voicegroup. TMC has
        // no key-split voices (whose split table the native ToneData cannot
        // hold), so a split uses the key directly.
        ToneData* sub = &((ToneData*)tone->wav)[track->key];
        if (sub->type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL)) {
            return;
        }
        key = track->key;
        if (tone->type & TONEDATA_TYPE_RHY) {
            if (sub->pan_sweep & 0x80) {
                rhythmPan = ((s32)sub->pan_sweep - TONEDATA_P_S_PAN) * 2;
            }
            key = sub->key;
        }
        tone = sub;
    } else {
        key = track->key;
    }

    priority = mplayInfo->priority + track->priority;
    if (priority > 0xFF) {
        priority = 0xFF;
    }

    cgb = tone->type & TONEDATA_TYPE_CGB;
    if (cgb) {
        CgbChannel* cgbChans = soundInfo->cgbChannels;
        if (cgbChans == NULL) {
            return;
        }
        chan = (SoundChannel*)&cgbChans[cgb - 1];
        if ((chan->statusFlags & SOUND_CHANNEL_SF_ON) && !(chan->statusFlags & SOUND_CHANNEL_SF_STOP)) {
            if (chan->priority > priority) {
                return;
            }
            if (chan->priority == priority && (PtrU32)CHAN_TRACK(chan) < (PtrU32)track) {
                return;
            }
        }
    } else {
        // The lowest-priority channel, preferring released ones; among equals the
        // one of the highest-addressed track. A free channel is taken at once.
        u32 bestPriority = priority;
        PtrU32 bestTrack = (PtrU32)track;
        bool32 releasing = FALSE;
        SoundChannel* best = NULL;
        s32 i;

        chan = NULL;
        for (i = 0; i < soundInfo->maxChannels; i++) {
            SoundChannel* c = &soundInfo->chans[i];
            if (!(c->statusFlags & SOUND_CHANNEL_SF_ON)) {
                chan = c;
                break;
            }
            if (c->statusFlags & SOUND_CHANNEL_SF_STOP) {
                if (!releasing) {
                    releasing = TRUE;
                    bestPriority = c->priority;
                    bestTrack = (PtrU32)c->track;
                    best = c;
                    continue;
                }
            } else if (releasing) {
                continue;
            }
            if (c->priority < bestPriority) {
                bestPriority = c->priority;
                bestTrack = (PtrU32)c->track;
                best = c;
            } else if (c->priority == bestPriority) {
                if ((PtrU32)c->track > bestTrack) {
                    bestTrack = (PtrU32)c->track;
                    best = c;
                } else if ((PtrU32)c->track == bestTrack) {
                    best = c;
                }
            }
        }
        if (chan == NULL) {
            chan = best;
        }
        if (chan == NULL) {
            return;
        }
    }

    ClearChain(chan);
    chan->prev = 0;
    chan->next = (PtrU32)track->chan;
    if (track->chan != NULL) {
        track->chan->prev = (PtrU32)chan;
    }
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0) {
        clear_modM(track);
    }
    TrkVolPitSet(mplayInfo, track);

    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = priority;
    chan->key = key;
    chan->rhythmPan = rhythmPan;
    chan->type = tone->type;
    chan->attack = tone->attack;
    chan->decay = tone->decay;
    chan->sustain = tone->sustain;
    chan->release = tone->release;
    chan->echoVolume = track->echoVolume;
    chan->echoLength = track->echoLength;
    if (cgb) {
        ((CgbChannel*)chan)->nextWave = (u32*)tone->wav;
    } else {
        chan->wav = tone->wav;
    }
    ChnVolSetAsm(chan, track);

    if (cgb) {
        CgbChannel* c = (CgbChannel*)chan;
        u8 sweep = tone->pan_sweep;

        c->length = tone->length;
        if ((sweep & 0x80) || !(sweep & 0x70)) {
            sweep = 8;
        }
        c->sweep = sweep;
        chan->frequency = soundInfo->MidiKeyToCgbFreq(cgb, NoteKey(chan, track), track->pitM);
    } else {
        chan->frequency = MidiKeyToFreq(chan->wav, NoteKey(chan, track), track->pitM);
    }
    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

typedef void (*PlyNote)(u32, MusicPlayerInfo*, MusicPlayerTrack*);

// One tick: count down note gate times, start tracks, read commands until the
// next wait, run the LFO. Returns the bits of the tracks still running.
static u32 Tick(MusicPlayerInfo* mplayInfo) {
    SoundInfo* soundInfo = SOUND_INFO_PTR;
    MusicPlayerTrack* track = mplayInfo->tracks;
    u32 bit = 1;
    u32 active = 0;
    s32 i;

    for (i = mplayInfo->trackCount; i > 0; i--, track++, bit <<= 1) {
        SoundChannel* chan;

        if (!(track->flags & MPT_FLG_EXIST)) {
            continue;
        }
        active |= bit;

        for (chan = track->chan; chan != NULL; chan = ChainNext(chan)) {
            if (chan->statusFlags & SOUND_CHANNEL_SF_ON) {
                if (chan->gateTime != 0 && --chan->gateTime == 0) {
                    chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
                }
            } else {
                ClearChain(chan);
            }
        }

        if (track->flags & MPT_FLG_START) {
            M4A_CLEAR_TRACK(track);
            track->flags = MPT_FLG_EXIST;
            track->bendRange = 2;
            track->volX = 64;
            track->lfoSpeed = 22;
            track->tone.type = 1;
        }

        while (track->wait == 0) {
            u8 cmd = *track->cmdPtr;

            if (cmd < 0x80) {
                cmd = track->runningStatus;
            } else {
                track->cmdPtr++;
                if (cmd >= 0xBD) {
                    track->runningStatus = cmd;
                }
            }
            if (cmd >= 0xCF) {
                ((PlyNote)soundInfo->plynote)(cmd - 0xCF, mplayInfo, track);
            } else if (cmd > 0xB0) {
                mplayInfo->cmd = cmd - 0xB1;
                ((XcmdFunc)((void**)soundInfo->MPlayJumpTable)[cmd - 0xB1])(mplayInfo, track);
                if (track->flags == 0) {
                    goto next_track;
                }
            } else {
                track->wait = gClockTable[cmd - 0x80];
            }
        }
        track->wait--;

        if (track->lfoSpeed != 0 && track->mod != 0) {
            if (track->lfoDelayC != 0) {
                track->lfoDelayC--;
            } else {
                // The sum is used untruncated, as in the assembly.
                u32 sum = track->lfoSpeedC + track->lfoSpeed;
                s32 wave;

                track->lfoSpeedC = sum;
                if ((s8)(sum - 0x40) < 0) {
                    wave = (s8)sum;
                } else {
                    wave = 0x80 - (s32)sum;
                }
                wave = (track->mod * wave) >> 6;
                if ((u8)(wave ^ track->modM) != 0) {
                    track->modM = wave;
                    track->flags |= track->modT == 0 ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
                }
            }
        }
    next_track:;
    }
    return active;
}

void MPlayMain(MusicPlayerInfo* mplayInfo) {
    SoundInfo* soundInfo;
    MusicPlayerTrack* track;
    u32 tempo;
    s32 i;

    if (mplayInfo->ident != ID_NUMBER) {
        return;
    }
    mplayInfo->ident++;

    // The players form a chain; the next one runs first.
    if (mplayInfo->func != NULL) {
        mplayInfo->func((MusicPlayerInfo*)mplayInfo->intp);
    }

    if ((s32)mplayInfo->status < 0) {
        goto done;
    }
    soundInfo = SOUND_INFO_PTR;
    FadeOutBody(mplayInfo);
    if ((s32)mplayInfo->status < 0) {
        goto done;
    }

    tempo = mplayInfo->tempoC + mplayInfo->tempoI;
    for (;;) {
        u32 active;

        mplayInfo->tempoC = tempo;
        if (tempo < 150) {
            break;
        }
        active = Tick(mplayInfo);
        mplayInfo->clock++;
        if (active == 0) {
            mplayInfo->status = MUSICPLAYER_STATUS_PAUSE;
            goto done;
        }
        mplayInfo->status = active;
        tempo = mplayInfo->tempoC - 150;
    }

    for (i = mplayInfo->trackCount, track = mplayInfo->tracks; i > 0; i--, track++) {
        SoundChannel* chan;

        if (!(track->flags & MPT_FLG_EXIST) || !(track->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG))) {
            continue;
        }
        TrkVolPitSet(mplayInfo, track);
        for (chan = track->chan; chan != NULL; chan = ChainNext(chan)) {
            u32 cgb;

            if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON)) {
                ClearChain(chan);
                continue;
            }
            cgb = chan->type & TONEDATA_TYPE_CGB;
            if (track->flags & MPT_FLG_VOLCHG) {
                ChnVolSetAsm(chan, track);
                if (cgb) {
                    ((CgbChannel*)chan)->modify |= CGB_CHANNEL_MO_VOL;
                }
            }
            if (track->flags & MPT_FLG_PITCHG) {
                if (cgb) {
                    chan->frequency = soundInfo->MidiKeyToCgbFreq(cgb, NoteKey(chan, track), track->pitM);
                    ((CgbChannel*)chan)->modify |= CGB_CHANNEL_MO_PIT;
                } else {
                    chan->frequency = MidiKeyToFreq(chan->wav, NoteKey(chan, track), track->pitM);
                }
            }
        }
        track->flags &= 0xF0;
    }

done:
    mplayInfo->ident = ID_NUMBER;
}
