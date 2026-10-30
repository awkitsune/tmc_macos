#ifndef LIGHTRAYMANAGER_H
#define LIGHTRAYMANAGER_H

#include "manager.h"

typedef struct {
    Manager base;
    u8 gfxGroup;
    u8 unk_21;
    u8 unk_22;
    u8 unk_23;
    u8 unk_24;
    u8 unk_25[15];
#ifdef PORT_LAYOUT_64
    // 4 bytes as on the GBA: the room data lands at the GBA offsets 0x30-0x3f (src/room.c RegisterRoomEntity),
    // and an 8-byte pointer would sit across it. Only compared with the BG3 tile map pointer.
    u32 unk_34;
#else
    void* unk_34;
#endif
} LightRayManager;

#endif // LIGHTRAYMANAGER_H
