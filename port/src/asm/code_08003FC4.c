/*
 * C versions of asm/src/code_08003FC4.s: gravity, pickup and rectangle checks,
 * drawing an entity (queueing it for DrawEntities), the animation state machine
 * and the splash/fall effects.
 */

#include <string.h>

#include "global.h"
#include "asm.h"
#include "effects.h"
#include "entity.h"
#include "map.h"
#include "object.h"
#include "physics.h"
#include "room.h"
#include "sound.h"
#include "structures.h"
#include "vram.h"

// ---- movement and checks --------------------------------------------------------

// Returns the new z while airborne, 0 once grounded.
u32 GravityUpdate(Entity* this, u32 gravity) {
    s32 z = this->z.WORD - this->zVelocity;

    if (z >= 0) {
        this->z.WORD = 0;
        this->zVelocity = 0;
        return 0;
    }
    this->zVelocity -= gravity;
    this->z.WORD = z;
    return z;
}

// Whether a is within (rx, ry) plus b's hitbox of b's hitbox center; 0 means no
// limit on that axis. Never true when both share layer 3.
static bool32 CheckEntityPickup(Entity* a, Entity* b, u32 rx, u32 ry) {
    Hitbox* hitbox = b->hitbox;
    s32 offsetX = hitbox->offset_x;
    u32 bx, by;

    rx += hitbox->width;
    ry += hitbox->height;
    if (((u8*)&b->spriteSettings)[0] & 4) {
        offsetX = -offsetX;
    }
    bx = b->x.HALF_U.HI + offsetX;
    by = b->y.HALF_U.HI + hitbox->offset_y;
    if ((a->collisionLayer & b->collisionLayer) == 3) {
        return FALSE;
    }
    if (rx != 0 && rx * 2 < (u32)(a->x.HALF_U.HI - bx + rx)) {
        return FALSE;
    }
    if (ry != 0 && ry * 2 < (u32)(a->y.HALF_U.HI - by + ry)) {
        return FALSE;
    }
    return TRUE;
}

u32 sub_08003FDE(Entity* a, Entity* b, u32 rx, u32 ry) {
    if (!CheckEntityPickup(a, b, rx, ry)) {
        return 0;
    }
    return sub_0806F58C(a, b);
}

u32 CheckOnScreen(Entity* this) {
    if ((u32)(this->x.HALF.HI - gRoomControls.scroll_x + 0x3F) >= 0x16E) {
        return FALSE;
    }
    if ((u32)(this->y.HALF.HI - gRoomControls.scroll_y + this->z.HALF.HI + 0x3F) >= 0x11E) {
        return FALSE;
    }
    return TRUE;
}

// Screen-space sprites (draw mode 2) always count as visible.
u32 sub_080040A2(Entity* this) {
    if (((u8*)&this->spriteSettings)[0] & 2) {
        return TRUE;
    }
    return CheckOnScreen(this);
}

// ---- drawing -----------------------------------------------------------------

typedef struct {
    u8 count;
    Entity* entities[80];
} DrawBucket;
extern DrawBucket gDrawBuckets[4]; // see intr.c

extern u8 gUnk_02024048;
extern u16 gUnk_02021F20[8];

// Queue the entity for DrawEntities by its priority, then play the sounds it
// enqueued with EnqueueSFX; they are dropped when it is not drawn.
void DrawEntity(Entity* this) {
    u32 draw = this->spriteSettings.draw;
    u32 count;

    if (draw == 0 || (draw != 3 && !sub_080040A2(this))) {
        gUnk_02024048 = 0;
        return;
    }
    {
        DrawBucket* bucket = &gDrawBuckets[(((u8*)&this->spriteRendering)[0] & 0xC0) >> 6];
        if (bucket->count < 0x40) {
            bucket->entities[bucket->count++] = this;
        }
    }
    count = gUnk_02024048;
    gUnk_02024048 = 0;
    for (u32 i = 0; i < count; i++) {
        SoundReq(gUnk_02021F20[i]);
    }
}

// ---- tile shape checks ---------------------------------------------------------

// Whether the 4x4 pixel cell at (x, y) is solid. Collision data below 0x10 is a
// 2x2 quadrant mask, 0xff fully solid, other values index 16-bit 4x4 masks.
static u32 IsSolidAt(Entity* this, const u16* masks, u32 x, u32 y) {
    u32 tilePos = (((x - gRoomControls.origin_x) & 0x3F0) >> 4) + (((y - gRoomControls.origin_y) & 0x3F0) << 2);
    MapLayer* map = this->collisionLayer == 2 ? &gMapTop : &gMapBottom;

    u32 data = map->collisionData[tilePos];
    if (data < 0x10) {
        if (!(y & 8)) {
            data >>= 2;
        }
        if (!(x & 8)) {
            data >>= 1;
        }
    } else if (data != 0xFF) {
        u32 sub;
        data = masks[data - 0x10];
        sub = x & 0xF;
        data >>= (sub >= 4) + (sub >= 8) + (sub >= 0xC);
        sub = y & 0xF;
        data >>= 4 * ((sub >= 4) + (sub >= 8) + (sub >= 0xC));
    }
    return data & 1;
}

u32 sub_080040D8(Entity* this, u8* masks, s32 x, s32 y) {
    return IsSolidAt(this, (const u16*)masks, x, y);
}

bool32 sub_080040E2(Entity* this, u8* masks) {
    return IsSolidAt(this, (const u16*)masks, this->x.HALF_U.HI, this->y.HALF_U.HI);
}

void SnapToTile(Entity* this) {
    this->x.WORD = (this->x.WORD & ~0xFFFFF) + 0x80000;
    this->y.WORD = (this->y.WORD & ~0xFFFFF) + 0x80000;
}

// Mirror the direction on the axes of the given collision flags.
void sub_0800417E(Entity* this, u32 collisions) {
    u32 direction = this->direction;

    if (collisions & 0xEE00) {
        direction = 0x20 - direction;
    }
    if (collisions & 0xEE) {
        direction = 0x10 - direction;
    }
    this->direction = direction & 0x1F;
}

static bool32 InRect(Entity* a, Entity* b, u32 rx, u32 ry) {
    if (rx != 0 && rx * 2 < (u32)(a->x.HALF_U.HI - b->x.HALF_U.HI + rx)) {
        return FALSE;
    }
    if (ry != 0 && ry * 2 < (u32)(a->y.HALF_U.HI - b->y.HALF_U.HI + ry)) {
        return FALSE;
    }
    return TRUE;
}

u32 sub_0800419C(Entity* a, Entity* b, u32 rx, u32 ry) {
    return InRect(a, b, rx, ry);
}

bool32 EntityInRectRadius(Entity* a, Entity* b, u32 rx, u32 ry) {
    if (!(a->collisionLayer & b->collisionLayer & 3)) {
        return FALSE;
    }
    return InRect(a, b, rx, ry);
}

u32 CalcDistance(s32 dx, s32 dy) {
    return Sqrt((u32)(dx * dx + dy * dy) << 8);
}

s32 sub_080041E8(s32 x1, s32 y1, s32 x2, s32 y2) {
    return CalcDistance(x1 - x2, y1 - y2);
}

u32 sub_080041DC(Entity* this, u32 x, u32 y) {
    return sub_080041E8(this->x.HALF.HI, this->y.HALF.HI, x, y);
}

// One tile on from a map byte offset in the direction of an animation state;
// returns the new offset and stores the tile type there in *outType.
u32 sub_08004202(Entity* this, u8* outType, u32 offset) {
    u32 state = this->animationState;
    MapLayer* map = this->collisionLayer == 2 ? &gMapTop : &gMapBottom;
    u32 tile;

    if (state & 3) {
        offset += (state & 4) ? -2 : 2;
    }
    if ((state & 3) != 2) {
        offset += ((state + 1) & 4) ? 0x80 : -0x80;
    }
    offset &= 0x1FFF;
    tile = map->mapData[offset / 2];
    if (!(tile & 0x4000)) {
        tile = map->tileTypes[tile & 0x7FFF];
    }
    memcpy(outType, &tile, 4);
    return offset;
}

// ---- animation -----------------------------------------------------------------

// Animation frames are 4 bytes: frame index, duration, sprite settings, flags.
// After a frame with flags bit 7 comes a byte: how many frames to loop back.
static void FrameZero(Entity* this) {
    const u8* p = this->animPtr;

    this->lastFrameIndex = this->frameIndex;
    this->frameIndex = p[0];
    this->frameDuration = p[1];
    this->frameSpriteSettings = p[2];
    this->frame = p[3];
    p += 4;
    if (this->frame & 0x80) {
        p -= p[0] * 4;
    }
    this->animPtr = (void*)p;
}

// Some scripted objects (e.g. the cutscene orchestrator) animate with sprite 0,
// which has no animation table. The GBA reads open bus there; natively use an
// inert looping animation: no frame gfx (0xFF), long duration, loop to itself.
static const u8 sNullAnimation[] = { 0xFF, 0xFF, 0, 0x80, 1, 0, 0, 0 };

void InitializeAnimation(Entity* this, u32 animIndex) {
    const void* const* anims = gSpritePtrs[(u16)this->spriteIndex].animations;

    this->animIndex = animIndex;
    this->animPtr = (anims != NULL && anims[animIndex] != NULL) ? (void*)anims[animIndex] : (void*)sNullAnimation;
    FrameZero(this);
}

void UpdateAnimationVariableFrames(Entity* this, u32 frames) {
    s32 time = this->frameDuration - (s32)frames;
    const u8* p;

    if (time == 0) {
        FrameZero(this);
        return;
    }
    if (time > 0) {
        this->frameDuration = time;
        return;
    }
    // Skip whole frames until the remaining time lands inside one.
    p = this->animPtr;
    while ((time += p[1]) <= 0) {
        u32 flags = p[3];
        p += 4;
        if (flags & 0x80) {
            p -= p[0] * 4;
        }
    }
    this->animPtr = (void*)p;
    FrameZero(this);
    this->frameDuration = time;
}

void GetNextFrame(Entity* this) {
    UpdateAnimationVariableFrames(this, 1);
}

// Point the entity's gfx slot at the tiles of a frame, marking it for upload if
// that changed.
void sub_080042D0(Entity* this, u32 frameIndex, u16 spriteIndex) {
    const SpritePtr* sprite;
    const SpriteFrame* frame;
    GfxSlot* slot;
    const u8* gfx;
    u32 oldTiles;
    const void* oldGfx;

    if (frameIndex == 0xFF) {
        return;
    }
    sprite = &gSpritePtrs[spriteIndex];
    if (sprite->frames == NULL) {
        return;
    }
    frame = &sprite->frames[frameIndex];
    if (frame->numTiles == 0) {
        return;
    }
    slot = &gGFXSlots.slots[this->spriteAnimation[0]];
    if (slot->status < 5) {
        return;
    }
    oldTiles = ((u8*)&slot->paletteIndex)[0];
    ((u8*)&slot->paletteIndex)[0] = frame->numTiles;
    gfx = (const u8*)sprite->ptr + frame->firstTileIndex * 32;
    oldGfx = slot->palettePointer;
    slot->palettePointer = gfx;
    if (oldTiles != frame->numTiles || oldGfx != gfx) {
        slot->vramStatus = 3;
    }
}

static void UpdateFrameGfx(Entity* this) {
    u32 frameIndex = this->frameIndex;
    u32 last = this->lastFrameIndex;

    this->lastFrameIndex = frameIndex;
    if (frameIndex != last) {
        sub_080042D0(this, frameIndex, this->spriteIndex);
    }
}

void sub_080042BA(Entity* this, u32 frames) {
    UpdateAnimationVariableFrames(this, frames);
    UpdateFrameGfx(this);
}

void UpdateAnimationSingleFrame(Entity* this) {
    sub_080042BA(this, 1);
}

void InitAnimationForceUpdate(Entity* this, u32 animIndex) {
    InitializeAnimation(this, animIndex);
    this->lastFrameIndex = 0xFF;
    UpdateFrameGfx(this);
}

// ---- effects ---------------------------------------------------------------------

// Replace the entity with a splash or fall effect at its position.
static void CreateFxAndDelete(Entity* this, u32 fx) {
    Entity* obj = CreateObject(SPECIAL_FX, fx, 0);

    if (obj != NULL) {
        obj->x.HALF.HI = this->x.HALF.HI;
        obj->y.HALF.HI = this->y.HALF.HI;
        obj->z.HALF.HI = this->z.HALF.HI;
        if (this->kind == ENEMY) {
            obj->type2 = 1;
        }
    }
    DeleteEntity(this);
}

void CreateDrownFx(Entity* this) {
    CreateFxAndDelete(this, FX_WATER_SPLASH);
}

void CreateLavaDrownFx(Entity* this) {
    CreateFxAndDelete(this, FX_LAVA_SPLASH);
}

void CreateSwampDrownFx(Entity* this) {
    CreateFxAndDelete(this, FX_GREEN_SPLASH);
}

void CreatePitFallFx(Entity* this) {
    CreateFxAndDelete(this, FX_FALL_DOWN);
}

// The values the asm uses directly.
_Static_assert(FX_WATER_SPLASH == 0xB && FX_LAVA_SPLASH == 0xC && FX_GREEN_SPLASH == 0x52 && FX_FALL_DOWN == 0,
               "effect ids");
_Static_assert(SPECIAL_FX == 0xF && ENEMY == 3, "object and kind ids");
