#ifndef VERTICALMINISHPATHBACKGROUNDMANAGER_H
#define VERTICALMINISHPATHBACKGROUNDMANAGER_H

#include "manager.h"

typedef struct {
    Manager base;
    u32 field_0x20[0x6];
#ifdef PORT_LAYOUT_64
    // 4 bytes as on the GBA: the room data lands at the GBA offsets 0x30-0x3f (src/room.c RegisterRoomEntity),
    // which 8-byte pointers would push along. Only compared with the BG3 / BG1 tile map pointers.
    u32 field_0x38;
    u32 field_0x3c;
#else
    void* field_0x38;
    void* field_0x3c;
#endif
} VerticalMinishPathBackgroundManager;

extern void sub_080575C8(u32);
extern void sub_08057688(void);

#endif // VERTICALMINISHPATHBACKGROUNDMANAGER_H
