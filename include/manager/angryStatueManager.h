#ifndef ANGRYSTATUEMANAGER_H
#define ANGRYSTATUEMANAGER_H

#include "manager.h"

typedef struct {
    Manager base;
#ifdef PORT_LAYOUT_64
    // The room data lands at the GBA offsets 0x30-0x3f (src/room.c RegisterRoomEntity), which 8-byte
    // pointers here would push 16 bytes along: field_0x20 is kept after it.
    u8 unk_20[0x10];
#else
    Entity* field_0x20[4];
#endif
    u8 field_0x30;
    u8 field_0x31;
    u8 field_0x32;
    u8 field_0x33;
    u8 field_0x34;
    u8 field_0x35;
    u8 field_0x36;
    u8 field_0x37;
    u8 field_0x38;
    u8 field_0x39;
    u8 field_0x3a;
    u8 field_0x3b;
    u8 field_0x3c;
    u8 field_0x3d;
    u16 field_0x3e;
#ifdef PORT_LAYOUT_64
    uintptr_t paramC; // the room data's paramC as a pointer
    Entity* field_0x20[4];
#endif
} AngryStatueManager;

#endif // ANGRYSTATUEMANAGER_H
