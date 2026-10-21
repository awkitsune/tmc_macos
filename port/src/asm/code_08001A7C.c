/*
 * C versions of the code in asm/src/code_08001A7C.s (its data tables are
 * converted by datagen): fuser lookup, palette nibble blits, the enemy action
 * selector, old-style linear movement and on-screen checks.
 */

#include "global.h"
#include "asm.h"
#include "entity.h"
#include "physics.h"
#include "player.h"
#include "room.h"

// ---- fusers --------------------------------------------------------------------

// 6-byte entries: id, type (0xff any), type2 (0xff any), fuser id, u16 text id.
// The scan starts at the second entry of each table.
extern const u8 gUnk_0800232E[]; // enemies
extern const u8 gUnk_08002342[]; // NPCs

// Returns the fuser id in the low word and the fuser name text id in the high
// word (r0/r1 on GBA): GetFuserIdAndFuserTextId calls this through a u64 cast,
// while the u32 prototype sees only the id. Hence the separate C name.
u64 GetFuserIdPair(Entity* entity) __asm__("_GetFuserId");
u64 GetFuserIdPair(Entity* entity) {
    static const u32 sMatchMasks[] = { 0xFFFFFF, 0xFFFF00, 0xFF00FF, 0xFF0000 };
    const u8* e;
    u32 key;

    if (entity->kind == ENEMY) {
        e = gUnk_0800232E;
    } else if (entity->kind == NPC) {
        e = gUnk_08002342;
    } else {
        return 0;
    }
    key = (entity->id << 16) | (entity->type << 8) | entity->type2;
    for (e += 6; e[0] != 0; e += 6) {
        u32 entry = (e[0] << 16) | (e[1] << 8) | e[2];
        u32 mask = sMatchMasks[(e[1] == 0xFF ? 2 : 0) + (e[2] == 0xFF ? 1 : 0)];
        if ((key & mask) == (entry & mask)) {
            return e[3] | ((u64)(e[4] | (e[5] << 8)) << 32);
        }
    }
    return 0;
}

// ---- palette nibbles -------------------------------------------------------------

// Write 16 looked-up values into one nibble of every 4th byte of dst, reading every
// 8th byte of src. The low bit of index picks the nibble (and the lut's 2nd half).
static void BlitNibbles(const u8* src, u8* dst, const u8* lut, u32 index, bool32 skipZero) {
    u8 keep = 0xF0;

    dst += (index >> 3) << 6;
    if (index & 1) {
        keep = 0x0F;
        lut += 0x10;
    }
    dst += (index >> 1) & 3;
    for (int i = 0; i < 16; i++, dst += 4, src += 8) {
        u8 value = lut[*src];
        if (!skipZero || value != 0) {
            *dst = (*dst & keep) | value;
        }
    }
}

void sub_080026C4(u8* src, u8* dst, u8* lut, u32 index) {
    BlitNibbles(src, dst, lut, index, FALSE);
}

void sub_080026F2(u8* src, void* dst, u8* lut, u32 index) {
    BlitNibbles(src, dst, lut, index, TRUE);
}

void UnpackTextNibbles(void* src, u8* dst) {
    const u8* s = src;
    for (int i = 0; i < 64; i++) {
        dst[2 * i] = s[i] & 0xF;
        dst[2 * i + 1] = s[i] >> 4;
    }
}

// ---- enemies ---------------------------------------------------------------------

// Which entry of an enemy's function table to run: 0 normal, 1 hit, 2 knockback,
// 3 dying, 4 confused, 5 held by the gust jar.
u32 GetNextFunction(Entity* this) {
    u32 gustJarState = this->gustJarState;

    if (!(gustJarState & 4) && (this->contactFlags & 0x80)) {
        return 1;
    }
    if (this->knockbackDuration != 0) {
        return 2;
    }
    if (this->health == 0) {
        return (this->action | this->subAction) ? 3 : 0;
    }
    if (gustJarState & 4) {
        return 5;
    }
    if (this->confusedTime != 0) {
        return 4;
    }
    return 0;
}

void sub_080028E0(Entity* this) {
    if (this->iframes > 0) {
        this->iframes--;
    } else if (this->iframes < 0) {
        this->iframes++;
    }
}

u32 GetRandomByWeight(const u8* weights) {
    s32 roll = Random() & 0xFF;
    u32 i = 0;

    do {
        roll -= weights[i++];
    } while (roll >= 0);
    return i - 1;
}

// ---- movement --------------------------------------------------------------------

// Slide along a wall: the direction to take instead, given the collision flags.
static u32 CalcCollisionDirectionOLD(u32 direction, u32 collisions) {
    switch (direction >> 3) {
        case 0:
            if (!(collisions & 0xE)) {
                return direction;
            }
            if (!(collisions & 0xE004)) {
                return 8;
            }
            if (!(collisions & 0xE02)) {
                return 0x18;
            }
            return direction;
        case 1:
            if (!(collisions & 0xE000)) {
                return direction;
            }
            if (!(collisions & 0x200E)) {
                return 0;
            }
            if (!(collisions & 0x40E0)) {
                return 0x10;
            }
            return direction;
        case 2:
            if (!(collisions & 0xE0)) {
                return direction;
            }
            if (!(collisions & 0xE040)) {
                return 8;
            }
            if (!(collisions & 0xE20)) {
                return 0x18;
            }
            return direction;
        default:
            if (!(collisions & 0xE00)) {
                return direction;
            }
            if (!(collisions & 0x20E)) {
                return 0;
            }
            if (!(collisions & 0x4E0)) {
                return 0x10;
            }
            return direction;
    }
}

extern const u16 gUnk_0800275C[]; // per direction: collision flags that block it

void LinearMoveDirectionOLD(Entity* this, u32 speed, u32 direction) {
    u32 collisions;

    if (direction & 0x80) {
        return;
    }
    collisions = this->collisions;
    if (!(direction & 7)) {
        u32 slide = CalcCollisionDirectionOLD(direction, collisions);
        if (slide != direction) {
            direction = slide;
            speed = 0x100;
        }
    }
    collisions &= gUnk_0800275C[direction];
    if (!(collisions & 0xEE00)) {
        s16 sine = ((const s16*)gSineTable)[direction * 8]; // the table runs past its declared 64
        if (sine != 0) {
            this->x.WORD += (s32)FixedMul(sine, speed) << 8;
        }
    }
    if (!(collisions & 0xEE)) {
        s16 cosine = ((const s16*)gSineTable)[direction * 8 + 64];
        if (cosine != 0) {
            this->y.WORD -= (s32)FixedMul(cosine, speed) << 8;
        }
    }
}

// ---- screen checks ---------------------------------------------------------------

u32 CheckRectOnScreen(s32 roomX, s32 roomY, u32 width, u32 height) {
    s32 left = gRoomControls.scroll_x - gRoomControls.origin_x - width;
    s32 top = gRoomControls.scroll_y - gRoomControls.origin_y - height;

    if ((u32)(roomX - left) >= width * 2 + DISPLAY_WIDTH) {
        return FALSE;
    }
    if ((u32)(roomY - top) >= height * 2 + DISPLAY_HEIGHT) {
        return FALSE;
    }
    return TRUE;
}

u32 CheckPlayerInRegion(u32 centerX, u32 centerY, u32 radiusX, u32 radiusY) {
    s32 left = gPlayerEntity.base.x.HALF_U.HI - gRoomControls.origin_x - radiusX;
    s32 top = gPlayerEntity.base.y.HALF_U.HI - gRoomControls.origin_y - radiusY;

    if ((u32)(centerX - left) >= radiusX * 2) {
        return FALSE;
    }
    if ((u32)(centerY - top) >= radiusY * 2) {
        return FALSE;
    }
    return TRUE;
}

_Static_assert(ENEMY == 3 && NPC == 7, "entity kinds used by GetFuserId");
