/*
 * C versions of asm/src/enemy.s: the enemy update entry point and shared enemy
 * behaviour helpers.
 */

#include "global.h"
#include "asm.h"
#include "effects.h"
#include "enemy.h"
#include "entity.h"

void DrawEntity(Entity*);
void Knockback1(Entity*);
void Knockback2(Entity*);
extern void (*const gEnemyFunctions[])(Entity*);

void EnemyUpdate(Entity* this) {
    if (this->action == 0) {
        if (!EnemyInit((Enemy*)this)) {
            DeleteThisEntity();
        }
    } else {
        if (EntityDisabled(this)) {
            DrawEntity(this);
            return;
        }
        sub_080028E0(this);
    }
    if (!(((Enemy*)this)->enemyFlags & 0x10)) {
        gEnemyFunctions[this->id](this);
        this->contactFlags &= 0x7F;
    }
    DrawEntity(this);
}

// Falling into a pit: wait out the timer while animating, then the fall effect.
static void sub_08001214(Entity* this) {
    if (!(this->gustJarState & 1)) {
        this->gustJarState = 1; // quirk: stores 1, not gustJarState | 1
        this->timer = (this->frame & 0x40) ? 0x20 : 1;
    }
    if (--this->timer != 0) {
        UpdateAnimationVariableFrames(this, 4);
    } else {
        CreatePitFallFx(this);
    }
}

// Hazard handlers indexed by GetTileHazardType's result.
void (*const gUnk_080012C8[])(Entity*) = {
    NULL, sub_08001214, CreateDrownFx, CreateLavaDrownFx, CreateSwampDrownFx,
};

void GenericConfused(Entity* this) {
    static const s8 sShake[] = { 0, 1, 0, -1 };
    u32 time = --this->confusedTime;

    if (time < 0x3C) {
        this->spriteOffsetX = sShake[time & 3];
        if (time == 0) {
            Entity* fx = ((Enemy*)this)->child;
            if (fx != NULL && fx->kind == 6 && fx->id == 0xF && fx->type == 0x1C) {
                EnemyDetachFX(this);
            }
        }
    }
    GravityUpdate(this, Q_8_8(24.0));
}

void sub_08001290(Entity* this, u32 hazard) {
    if (hazard != 0) {
        gUnk_080012C8[hazard](this);
    }
}

// Returns the hazard the enemy is on (1-3), or 0 when it acts normally.
s32 sub_080012DC(Entity* this) {
    u32 hazard;

    if (this->gustJarState & 4) {
        return 0;
    }
    hazard = GetTileHazardType(this);
    if (hazard == 4) {
        return 0;
    }
    if (hazard == 0) {
        if (this->gustJarState & 1) {
            this->flags |= ENT_COLLIDE;
        }
        return 0;
    }
    if (hazard != 1) {
        this->timer = 1;
        this->gustJarState |= 1;
    }
    return hazard;
}

void EnemyFunctionHandler(Entity* this, EntityActionArray functions) {
    u32 hazard = sub_080012DC(this);

    if (hazard != 0) {
        gUnk_080012C8[hazard](this);
    } else {
        functions[GetNextFunction(this)](this);
    }
}

void sub_08001318(Entity* this) {
    if (this->z.HALF.HI < 0) {
        this->direction = 0xFF;
    }
    Knockback1(this);
}

void GenericKnockback(Entity* this) {
    Knockback1(this);
}

void GenericKnockback2(Entity* this) {
    Knockback2(this);
}

u32 CalcCollisionDirection(s32 x1, s32 y1, s32 x2, s32 y2);

// Facing direction from a to b when they share a layer and are not within 8px
// of each other on both axes, else 0xFF.
u32 sub_0800132C(Entity* a, Entity* b) {
    if (b == NULL) {
        // Leever_Move passes gEnemyTarget unchecked, NULL while Link cannot be targeted. The GBA
        // then reads BIOS open bus (0xe55ec002 after an interrupt returns): layer byte 0x02,
        // x and y halves 0xe55e, a point far off any room, so the direction is toward it.
        if (!(a->collisionLayer & 0x02)) {
            return 0xFF;
        }
        if ((u32)(a->x.HALF_U.HI - 0xe55e + 8) < 0x11 && (u32)(a->y.HALF_U.HI - 0xe55e + 8) < 0x11) {
            return 0xFF;
        }
        return CalcCollisionDirection(a->x.HALF.HI, a->y.HALF.HI, (s16)0xe55e, (s16)0xe55e);
    }
    if (!(a->collisionLayer & b->collisionLayer)) {
        return 0xFF;
    }
    if ((u32)(a->x.HALF_U.HI - b->x.HALF_U.HI + 8) < 0x11 && (u32)(a->y.HALF_U.HI - b->y.HALF_U.HI + 8) < 0x11) {
        return 0xFF;
    }
    return GetFacingDirection(a, b);
}
