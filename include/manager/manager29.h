#ifndef MANAGER29_H
#define MANAGER29_H

#include "manager.h"

typedef struct {
    Manager base;
    u8 unk_20[0x8];
#ifdef PORT_LAYOUT_64
    // The room data lands at the GBA offsets 0x30-0x3f (src/room.c RegisterRoomEntity), which 8-byte
    // pointers here would push 8 bytes along: unk_28 and unk_2c are kept after it.
    u32 unk_28_gba;
    u32 unk_2c_gba;
#else
    u16* unk_28;
    u16* unk_2c;
#endif
    u8 filler[0x4];
    u8 layer;
    u8 unk_35;
    u8 unk_36;
    u8 unk_37;
    s16 unk_38;
    u16 unk_3a;
    u16 unk_3c;
    u16 flag;
#ifdef PORT_LAYOUT_64
    uintptr_t paramC; // the room data's paramC as a pointer
    u16* unk_28;
    u16* unk_2c;
#endif
} Manager29;

#endif // MANAGER29_H
