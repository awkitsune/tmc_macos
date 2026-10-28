#ifndef TILECHANGEOBSERVEMANAGER_H
#define TILECHANGEOBSERVEMANAGER_H

#include "manager.h"

typedef struct {
    Manager base;
    u8 field_0x20;
    u8 field_0x21;
    u8 field_0x22;
    u8 field_0x23;
    u8 field_0x24;
    u8 field_0x25;
    u8 field_0x26;
    u8 field_0x27;
    u16 initialTile;
    u8 field_0x2a;
    u8 field_0x2b;
    u8 field_0x2c;
    u8 field_0x2d;
    u8 field_0x2e;
    u8 field_0x2f;
#ifdef PLATFORM_NATIVE
    // The room data lands at the GBA offsets 0x30-0x3f (src/room.c RegisterRoomEntity), which an 8-byte
    // pointer here would push 4 bytes along: observedTile is kept after it.
    u32 field_0x30;
#else
    u16* observedTile;
#endif
    u8 field_0x34;
    u8 field_0x35;
    u8 field_0x36;
    u8 field_0x37;
    u16 tilePos;
    u16 field_0x3a;
    u8 field_0x3c;
    u8 field_0x3d;
    u16 flag;
#ifdef PLATFORM_NATIVE
    uintptr_t paramC; // the room data's paramC as a pointer
    u16* observedTile;
#endif
} TileChangeObserveManager;

#endif // TILECHANGEOBSERVEMANAGER_H
