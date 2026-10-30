/*
 * C versions of asm/src/code_080043E8.s: tile hazards, collision layer resets,
 * bouncing, sound helpers and direction calculations.
 */

#include "global.h"
#include "asm.h"
#include "collision.h"
#include "effects.h"
#include "entity.h"
#include "player.h"
#include "sound.h"

u32 ram_sub_080B227C(Entity* tgt, Entity* org);
u32 UpdateCollisionLayer(Entity* entity);

typedef struct {
    u16 actTile;
    u16 hazard;
} HazardTile;

// Hazard by act tile: 1 pit, 2 water, 3 lava, 4 swamp.
static const HazardTile sHazardTiles[] = {
    { 0x0D, 1 }, { 0x10, 2 }, { 0x11, 2 }, { 0x5A, 3 }, { 0x13, 4 }, { 0 },
};

u32 GetTileHazardType(Entity* this) {
    u32 actTile;
    const HazardTile* h;

    if (this->action == 0) {
        return 0;
    }
    actTile = UpdateCollisionLayer(this);
    if (this->z.HALF.HI < 0 || actTile == 0) {
        return 0;
    }
    for (h = sHazardTiles; h->actTile != 0; h++) {
        if (h->actTile == actTile) {
            return h->hazard;
        }
    }
    return 0;
}

u32 sub_0800442E(Entity* this) {
    static void (*const sHazardFx[])(Entity*) = { CreatePitFallFx, CreateDrownFx, CreateLavaDrownFx,
                                                  CreateSwampDrownFx };
    u32 hazard = GetTileHazardType(this);

    if (hazard == 0) {
        return 0;
    }
    sHazardFx[hazard - 1](this);
    return 1;
}

u32 sub_0800445C(Entity* this) {
    if (!PlayerCanBeMoved()) {
        return 0;
    }
    if (!ram_sub_080B227C(this, &gPlayerEntity.base)) { // CalcCollisionStaticEntity, with its result
        return 0;
    }
    if (gPlayerEntity.base.action == 2) {
        gPlayerEntity.base.subAction = 3;
    }
    return 1;
}

void CalcCollisionStaticEntity(Entity* tgt, Entity* org) {
    ram_sub_080B227C(tgt, org);
}

extern u8 gUnk_02024048;
extern u16 gUnk_02021F20[8];

void EnqueueSFX(u32 sfx) {
    if (gUnk_02024048 < 8) {
        gUnk_02021F20[gUnk_02024048++] = sfx;
    }
}

void SoundReqClipped(Entity* this, u32 sfx) {
    if (CheckOnScreen(this)) {
        SoundReq(sfx);
    }
}

void sub_080044AE(Entity* this, u32 speed, u32 direction) {
    if (this == &gPlayerEntity.base) {
        sub_08079E58(speed, direction);
    } else {
        CalculateEntityTileCollisions(this, direction, 2);
        ProcessMovementInternal(this, speed, direction, 2);
    }
}

// Assumes an initial zVelocity. Returns BOUNCE_AIRBORNE while in the air,
// BOUNCE_INIT_NEXT on landing with another bounce to come, else BOUNCE_DONE_ALL.
u32 BounceUpdate(Entity* this, u32 acceleration) {
    s32 z = this->z.WORD - this->zVelocity;
    u32 velocity;

    if (z < 0) {
        this->z.WORD = z;
        this->zVelocity -= acceleration;
        return BOUNCE_AIRBORNE;
    }
    this->z.WORD = 1;
    velocity = (u32)-(this->zVelocity - (s32)acceleration) >> 1;
    velocity += velocity >> 2;
    if ((velocity >> 12) < 0xC) {
        this->zVelocity = 0;
        return BOUNCE_DONE_ALL;
    }
    this->zVelocity = velocity;
    return BOUNCE_INIT_NEXT;
}

static void SetCollisionLayerAndSprite(Entity* this, u32 layer, u32 spriteBits) {
    u8* orientation = (u8*)&this->spriteOrientation;
    u8* rendering = (u8*)&this->spriteRendering;

    this->collisionLayer = layer;
    *orientation = (*orientation & ~0xC0) + spriteBits;
    *rendering = (*rendering & ~0xC0) + spriteBits;
}

void sub_08004542(Entity* this) {
    SetCollisionLayerAndSprite(this, 2, 0x40);
}

void ResetCollisionLayer(Entity* this) {
    SetCollisionLayerAndSprite(this, 1, 0x80);
}

void sub_0800451C(Entity* this) {
    switch (GetActTileAtEntity(this)) {
        case 0x0C:
        case 0x0B:
            sub_08004542(this);
            break;
        case 0x0A:
        case 0x09:
            ResetCollisionLayer(this);
            break;
        case 0x26:
        case 0x27:
            SetCollisionLayerAndSprite(this, 3, 0x40);
            break;
    }
}

// Turn one step (of 32) towards a direction; invalid directions snap to it.
void sub_08004596(Entity* this, u32 target) {
    u32 direction = this->direction;
    u32 diff;

    if (direction >= 0x20) {
        this->direction = target & 0x1F;
        return;
    }
    diff = target - direction;
    if (diff == 0) {
        return;
    }
    this->direction = (direction + ((diff & 0x1F) < 0x10 ? 1 : -1)) & 0x1F;
}

u32 CalcCollisionDirection(s32 x1, s32 y1, s32 x2, s32 y2);

u32 sub_080045B4(Entity* this, u32 x, u32 y) {
    return CalcCollisionDirection(this->x.HALF.HI, this->y.HALF.HI, x, y);
}

u32 GetFacingDirection(Entity* origin, Entity* target) {
    if (target == NULL) {
        // Octorok_Turn (type 2, off its home area) passes gEnemyTarget unchecked, NULL while Link
        // cannot be targeted. The GBA's ldrsh at 0x2e and 0x32 read BIOS open bus (0xe55ec002 after
        // an interrupt returns): x and y halves 0xe55e, signed, a point far up-left of any room.
        return CalcCollisionDirection(origin->x.HALF.HI, origin->y.HALF.HI, (s16)0xe55e, (s16)0xe55e);
    }
    return CalcCollisionDirection(origin->x.HALF.HI, origin->y.HALF.HI, target->x.HALF.HI, target->y.HALF.HI);
}

u32 CalculateDirectionTo(u32 x1, u32 y1, u32 x2, u32 y2) {
    return CalcCollisionDirection(x1, y1, x2, y2);
}

extern const u16 gUnk_080C93E0[];

// Direction (0-0xff) of an offset, from a table of slope boundaries.
u32 CalculateDirectionFromOffsets(s32 dx, s32 dy) {
    u32 angle = 0x40;

    if (dx != 0) {
        // The BIOS Div also returns |quotient| in r3, which is what gets compared.
        s32 quotient = (dy << 8) / dx;
        u32 slope = quotient < 0 ? -quotient : quotient;
        u32 i, end;

        if (slope < 0x106) {
            if (slope < 0x6E) {
                angle = 0;
                i = 0, end = 0x20;
            } else {
                i = 0x20, end = 0x40;
            }
        } else if (slope < 0x280) {
            i = 0x40, end = 0x60;
        } else {
            i = 0x60, end = 0x7E;
        }
        for (; i < end; i += 2) {
            if (slope >= gUnk_080C93E0[i / 2] && slope < gUnk_080C93E0[i / 2 + 1]) {
                angle = (i >> 1) + 1;
                break;
            }
        }
    }
    if (dx >= 0) {
        return dy < 0 ? 0x40 - angle : 0x40 + angle;
    }
    return dy < 0 ? 0xC0 + angle : 0xC0 - angle;
}

// Direction by CalcCollisionDirection's octant index (see intr.c).
const u8 gUnk_0800464E[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04, 0x00, 0x00, 0x00,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x00, 0x00, 0x00, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x00, 0x00, 0x00,
    0x00, 0x1F, 0x1E, 0x1D, 0x1C, 0x00, 0x00, 0x00, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x00, 0x00, 0x00,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x00, 0x00, 0x00, 0x18, 0x17, 0x16, 0x15, 0x14, 0x00, 0x00, 0x00,
    0x00, 0x00,
};
