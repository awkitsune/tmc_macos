#ifndef TEMPLEOFDROPLETSMANAGER_H
#define TEMPLEOFDROPLETSMANAGER_H

#include "manager.h"

typedef struct {
    Manager base;
    u8 unk_20;
    u8 unk_21;
    u8 unk_22;
    u8 unk_23; // used
    u16 unk_24;
    u16 unk_26;
#ifdef PLATFORM_NATIVE
    // 4 bytes as on the GBA: the room data lands at the GBA offsets 0x30-0x3f (unk_34 .. localFlag),
    // which an 8-byte pointer here would push 4 bytes along (src/room.c RegisterRoomEntity).
    u32 unk_28;
#else
    void* unk_28;
#endif
    u16 unk_2c;
    u8 unk_2e[0x6];
    s16 unk_34;
    s16 unk_36;
    s16 unk_38;
    s16 unk_3a;
    u16 flag;
    u16 localFlag; // used
} TempleOfDropletsManager;

#endif // TEMPLEOFDROPLETSMANAGER_H
