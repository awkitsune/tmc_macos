#ifndef AFFINE_H
#define AFFINE_H

#include "gba/types.h"

typedef struct {
    s16 x;
    s16 y;
    u16 _4;
    u16 _6;
    u16 _8;
} OAMCommand;
extern OAMCommand gOamCmd;

extern void FlushSprites(void);
extern void CopyOAM(void);
extern void DrawEntities(void);
extern void sub_080ADA04(OAMCommand*, void*);
extern void DrawDirect(u32 spriteIndex, u32 frameIndex);
#ifdef PORT_HOSTED
// The sprites drawn meanwhile are the HUD's: culled at the GBA screen's edges in the view (port/src/asm/intr.c).
void PortDrawHudSprites(bool32 on);
#endif

#endif // AFFINE_H
