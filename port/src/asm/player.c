/*
 * C versions of asm/src/player.s: player tile collision probes, tile
 * interactions (cutting, digging, picking up), and ice/velocity movement.
 */

#include "global.h"
#include "asm.h"
#include "effects.h"
#include "entity.h"
#include "item.h"
#include "map.h"
#include "object.h"
#include "physics.h"
#include "player.h"
#include "room.h"

void ResetPlayerVelocity(void);
void UpdateSpriteForCollisionLayer(Entity* entity);

// ---- tile collision ------------------------------------------------------------

extern u16* gUnk_0800823C[];

// Shape tables by player state: collision data >= 0x10 maps through them to one
// of the 16x16 masks in gUnk_0800823C.
extern const u8 gUnk_080082DC[];
extern const u8 gUnk_0800833C[];
extern const u8 gUnk_0800839C[];
extern const u8 gUnk_080083FC[];
extern const u8 gUnk_0800845C[];
extern const u8 gUnk_080084BC[];
extern const u8 gUnk_0800851C[];

// sub_080086D8 (r8 = entity, r11 = shape table): whether the pixel at room
// coordinates (x, y) is solid for the player.
static u32 IsSolidForPlayer(Entity* entity, const u8* shapes, u32 x, u32 y) {
    u32 tilePos = ((x & 0x3F0) >> 4) + ((y & 0x3F0) << 2);
    MapLayer* map = entity->collisionLayer == 2 ? &gMapTop : &gMapBottom;
    u32 data = map->collisionData[tilePos];

    if (data < 0x10) {
        // Swimming (except on floor type 0x18) treats partial tiles as solid.
        if (gPlayerState.swim_state != 0 && gPlayerState.floor_type != 0x18) {
            data = 0xF;
        }
        if (!(y & 8)) {
            data >>= 2;
        }
        if (!(x & 8)) {
            data >>= 1;
        }
    } else if (data != 0xFF) {
        const u16* mask = gUnk_0800823C[shapes[data - 0x10]];
        data = mask[y & 0xF] >> (0xF ^ (x & 0xF));
    }
    return data & 1;
}

// Probe the eight points around the player's hitbox into entity->collisions.
static void UpdatePlayerTileCollisions(Entity* entity) {
    const u8* shapes;
    Hitbox* hitbox = entity->hitbox;
    s32 cx, cy, x, y;
    u32 bits = 0;
    s32 dx = hitbox->unk2[0], dy = hitbox->unk2[1];

    if (gPlayerState.swim_state != 0) {
        shapes = (gPlayerState.flags & PL_MINISH) ? gUnk_0800839C : gUnk_080083FC;
    } else if (gPlayerState.jump_status != 0 || (gPlayerState.flags & 0x01000000)) {
        shapes = gUnk_0800845C;
    } else if (gPlayerState.flags & PL_MINISH) {
        shapes = gUnk_0800833C;
    } else if (gPlayerState.gustJarState != 0 || gPlayerState.heldObject != 0) {
        shapes = gUnk_080084BC;
    } else if (gPlayerState.attachedBeetleCount != 0) {
        shapes = gUnk_0800851C;
    } else {
        shapes = gUnk_080082DC;
    }

    cx = entity->x.HALF_U.HI - gRoomControls.origin_x + hitbox->offset_x;
    cy = entity->y.HALF_U.HI - gRoomControls.origin_y + hitbox->offset_y;

    // Left/right edges: each side probed above and below.
    x = cx + dx;
    for (int i = 0; i < 2; i++) {
        bits <<= 2;
        bits |= IsSolidForPlayer(entity, shapes, x, cy + dy);
        bits <<= 1;
        bits |= IsSolidForPlayer(entity, shapes, x, cy - dy);
        bits <<= 1;
        x -= 2 * dx;
    }
    // Top/bottom edges: each probed left and right.
    dx = hitbox->unk2[2];
    dy = hitbox->unk2[3];
    y = cy + dy;
    for (int i = 0; i < 2; i++) {
        bits <<= 2;
        bits |= IsSolidForPlayer(entity, shapes, cx + dx, y);
        bits <<= 1;
        bits |= IsSolidForPlayer(entity, shapes, cx - dx, y);
        bits <<= 1;
        y -= 2 * dy;
    }
    entity->collisions = bits;
}

void sub_0800857C(Entity* this) {
    if (!(this->type2 & 0x80) && !(gPlayerState.jump_status & 0x80)) {
        UpdatePlayerTileCollisions(this);
    }
    LinearMoveDirectionOLD(this, this->speed, this->direction);
}

void sub_080085B0(Entity* this) {
    UpdatePlayerTileCollisions(this);
}

u32 sub_080086B4(u32 x, u32 y, const u8* shapes) {
    return IsSolidForPlayer(&gPlayerEntity.base, shapes, x, y);
}

// ---- tile interactions ---------------------------------------------------------

extern const KeyValuePair gUnk_080046A4[]; // tile type -> interaction
extern const u8 gUnk_080047F6[];          // 8-byte interactions, see below
extern const s8 gUnk_08007DF4[];          // offsets by animation state

// Interaction entries: u16 mask of allowed kinds, object id (0xff none) and type,
// two bytes copied to item behaviors, u16 replacement tile (0x4xxx special tile,
// 0xffff restore the previous one, otherwise a tile type).
u16* DoTileInteraction(Entity* this, u32 kind, u32 x, u32 y) {
    u32 tileType, index;
    const u8* entry;
    u32 tilePos;
    u16 newTile;
    const KeyValuePair* pair;

    if (gRoomControls.reload_flags == 1) {
        return NULL;
    }
    tileType = GetTileTypeAtWorldCoords(x, y, this->collisionLayer);
    for (pair = gUnk_080046A4; pair->key != 0; pair++) {
        if (pair->key == tileType) {
            break;
        }
    }
    if (pair->key == 0) {
        return NULL;
    }
    index = pair->value;
    entry = &gUnk_080047F6[index * 8];
    if (!(((entry[0] | (entry[1] << 8)) >> kind) & 1)) {
        return NULL;
    }

    {
        u32 id = entry[2], type = entry[3];
        bool32 create = id != 0xFF && kind != 6 && kind != 0xE && kind != 0xA && kind != 0xB &&
                        (kind != 0xD || (id == SPECIAL_FX && type == FX_GRASS_CUT));
        if (create) {
            Entity* obj = CreateObject(id, type, id == SPECIAL_FX ? 0x80 : 0);
            if (obj != NULL) {
                if (id != 0) {
                    obj->x.HALF.HI = (x & ~0xF) + 8;
                    obj->y.HALF.HI = (y & ~0xF) + 8;
                } else {
                    obj->x.HALF.HI = this->x.HALF.HI;
                    obj->y.HALF.HI = this->y.HALF.HI;
                    // The asm then stores z and the parent through a coordinate
                    // instead of the object, so neither is ever set; skipped here.
                }
                obj->collisionLayer = this->collisionLayer;
                UpdateSpriteForCollisionLayer(obj);
            }
        }
    }

    tilePos = ((u32)(x - gRoomControls.origin_x) >> 4) + (((u32)(y - gRoomControls.origin_y) >> 4) << 6);
    newTile = entry[6] | (entry[7] << 8);
    if (newTile & 0x4000) {
        if (newTile == 0xFFFF) {
            RestorePrevTileEntity(tilePos, this->collisionLayer);
        } else {
            GetLayerByIndex(this->collisionLayer)->mapData[tilePos] = newTile;
        }
    } else {
        sub_0807B7D8(newTile, tilePos, this->collisionLayer);
    }
    return (u16*)entry;
}

// Callers declare the returned entry pointer under various types; it is the same
// pointer as DoTileInteraction's.
const u8* DoTileInteractionOffset(Entity* this, u32 kind, s32 dx, s32 dy) {
    return (const u8*)DoTileInteraction(this, kind, this->x.HALF_U.HI + dx, this->y.HALF_U.HI + dy);
}

u32* DoTileInteractionHere(Entity* this, u32 kind) {
    return (u32*)DoTileInteraction(this, kind, this->x.HALF_U.HI, this->y.HALF_U.HI);
}

const u8* DoItemTileInteraction(Entity* this, u32 kind, ItemBehavior* behavior) {
    const s8* offset = &gUnk_08007DF4[this->animationState & 6];
    const u8* entry = DoTileInteractionOffset(this, kind, offset[0], offset[1]);

    if (entry != NULL) {
        ((u8*)&behavior->field_0x2)[1] = entry[2];
        behavior->timer = entry[3];
        behavior->subtimer = entry[5];
    }
    return entry;
}

// ---- velocity ------------------------------------------------------------------

// vel_x/vel_y are u16 in PlayerState; the asm treats them as signed (ldrsh).
static void ClampPlayerVelocity(u16* field, s32 value) {
    if (value >= 0) {
        *field = (u32)value < 0x180 ? value : 0x180;
    } else {
        *field = (u32)value >= (u32)-0x180 ? value : -0x180;
    }
}

static void AddPlayerVelocity(s32 dx, s32 dy) {
    ClampPlayerVelocity(&gPlayerState.vel_x, (s16)gPlayerState.vel_x + dx);
    ClampPlayerVelocity(&gPlayerState.vel_y, (s16)gPlayerState.vel_y + dy);
}

// Friction: move towards zero by 3.
static void DecayVelocity(u16* field) {
    s32 v = (s16)*field;
    if (v >= 0) {
        v = v - 3 > 0 ? v - 3 : 0;
    } else {
        v = v + 3 < 0 ? v + 3 : 0;
    }
    *field = v;
}

static void MoveByVelocity(Entity* this) {
    s32 v = (s16)gPlayerState.vel_x;
    if (v != 0) {
        u32 direction = v < 0 ? 0x18 : 8;
        LinearMoveDirectionOLD(this, v < 0 ? -v : v, direction);
        sub_0807A5B8(direction);
    }
    v = (s16)gPlayerState.vel_y;
    if (v != 0) {
        u32 direction = v < 0 ? 0 : 0x10;
        LinearMoveDirectionOLD(this, v < 0 ? -v : v, direction);
        sub_0807A5B8(direction);
    }
    if (gPlayerState.jump_status == 0) {
        DecayVelocity(&gPlayerState.vel_x);
        DecayVelocity(&gPlayerState.vel_y);
    }
}

static const s8 sVelocities[][2] = { { 0, -3 }, { 3, -3 }, { 3, 0 }, { 3, 3 },
                                     { 0, 3 },  { -3, 3 }, { -3, 0 }, { -3, -3 } };
static const s8 sIceVelocities[][2] = { { 0, -10 }, { 10, -10 }, { 10, 0 }, { 10, 10 },
                                        { 0, 10 },  { -10, 10 }, { -10, 0 }, { -10, -10 } };
static const s8 sJumpBrake[][2] = { { 0, 6 }, { -6, 0 }, { 0, -6 }, { 6, 0 } };

// Accelerate along a direction (0x80 bit: none), then move and apply friction.
static void AcceleratePlayer(Entity* this, u32 direction) {
    if (!(direction & 0x80)) {
        const s8* v;

        if (gPlayerState.heldObject == 1 || gPlayerState.heldObject == 2) {
            ResetPlayerVelocity();
            return;
        }
        if (gPlayerState.jump_status == 0) {
            v = sIceVelocities[direction >> 2];
        } else {
            // In the air: steering against the facing direction brakes.
            u32 relative = ((direction >> 2) - ((this->animationState >> 1) << 1) + 2) & 7;
            if (relative > 4) {
                v = sJumpBrake[this->animationState >> 1];
            } else {
                v = sVelocities[direction >> 2];
            }
        }
        AddPlayerVelocity(v[0], v[1]);
    }
    MoveByVelocity(this);
}

void sub_08008926(Entity* this) {
    if (gPlayerState.field_0x7 | gPlayerState.field_0xa) {
        return;
    }
    this->direction = gPlayerState.direction;
    AcceleratePlayer(this, gPlayerState.direction);
}

// Like sub_08008926 but always accelerating in the facing direction, without
// updating the entity's direction.
void UpdateIcePlayerVelocity(Entity* this) {
    u32 direction = (this->animationState >> 1) << 3;

    if (gPlayerState.heldObject == 1 || gPlayerState.heldObject == 2) {
        ResetPlayerVelocity();
        return;
    }
    AcceleratePlayer(this, direction);
}

void sub_08008AA0(Entity* this) {
    if (gPlayerState.floor_type == 1 || gPlayerState.direction == 0xFF) {
        return;
    }
    gPlayerState.vel_x = gSineTable[gPlayerState.direction * 8];
    gPlayerState.vel_y = -gSineTable[gPlayerState.direction * 8 + 64];
}

// Respawn the player when stuck: collisions on all four sides.
void sub_08008AC6(Entity* this) {
    u32 collisions;

    if (gPlayerState.swim_state & 0xF) {
        return;
    }
    // The asm uses the address of gUnk_02000020 as this mask.
    if (gPlayerState.flags & 0x02000020) {
        return;
    }
    collisions = this->collisions;
    for (int side = 0; side < 4; side++, collisions >>= 4) {
        if (!(collisions & 0xE)) {
            return;
        }
    }
    this->iframes = (s8)0xE2;
    RespawnPlayer();
}

extern const KeyValuePair gMapActTileToSurfaceType[];

u32 PlayerCheckNEastTile(void) {
    u32 actTile = GetActTileRelativeToEntity(&gPlayerEntity.base, 0, 0);
    const KeyValuePair* pair;

    if (actTile & 0x4000) {
        return 0;
    }
    for (pair = gMapActTileToSurfaceType; pair->key != 0; pair++) {
        if (pair->key == actTile) {
            return pair->value == 1;
        }
    }
    return 0;
}

// The values the asm uses directly.
_Static_assert(PL_MINISH == 0x80, "player flag");
_Static_assert(SPECIAL_FX == 0xF && FX_GRASS_CUT == 0x17, "tile interaction objects");
