/*
 * C versions of the ARM routines in asm/src/intr.s, which the GBA build copies to
 * IWRAM and calls through the ram_* names: tile lookups, the collision pass, the
 * entity update loop, palette fading and sprite (OAM) generation.
 *
 * Register-passing between the original routines is made explicit: DrawState
 * mirrors r6-r8, sb and sl of the sprite code. Quirks of the original are kept and
 * marked as such.
 */

#include <setjmp.h>
#include <string.h>

#include "global.h"
#include "affine.h"
#include "asm.h"
#include "color.h"
#include "collision.h"
#include "entity.h"
#include "map.h"
#include "room.h"
#include "save.h"
#include "vram.h"

// ---- tables that lived in asm/src/veneer.s ---------------------------------

// Indexed by layer (0-3); layers 0, 1 and 3 all use the bottom map.
#define LAYER_MAP(layer) ((layer) == 2 ? &gMapTop : &gMapBottom)

static u16* MapDataFor(u32 layer) {
    return LAYER_MAP(layer)->mapData;
}
static u16* MapDataOriginalFor(u32 layer) {
    return LAYER_MAP(layer)->mapDataOriginal;
}
static u16* TileTypesFor(u32 layer) {
    return LAYER_MAP(layer)->tileTypes;
}
static u8* CollisionDataFor(u32 layer) {
    return LAYER_MAP(layer)->collisionData;
}
static u8* ActTilesFor(u32 layer) {
    return LAYER_MAP(layer)->actTiles;
}

extern const u8 gMapTileTypeToActTile[];
extern const u8 gMapSpecialTileToActTile[];
extern const u16 gUnk_08000360[];
extern const u16 gUnk_080B7A3E[];

// ---- tile lookups -----------------------------------------------------------

// Room pixel coordinates to a tile position (64 tiles per row), as the
// "lsl #0x16; lsr #0x1a" pairs do: bits 4-9 of each coordinate.
static u32 RoomCoordsToTilePos(u32 roomX, u32 roomY) {
    return ((roomX >> 4) & 0x3F) + (((roomY >> 4) & 0x3F) << 6);
}

static u32 TileTypeAt(u16* const map, u16* const types, u32 tilePos) {
    u32 tile = map[tilePos];
    if (tile >= 0x4000) {
        return tile; // special tiles are their own type
    }
    return types[tile];
}

u32 GetTileTypeAtTilePos(u32 tilePos, u32 layer) {
    return TileTypeAt(MapDataFor(layer), TileTypesFor(layer), tilePos);
}

u32 GetTileTypeAtRoomTile(u32 roomTileX, u32 roomTileY, u32 layer) {
    return GetTileTypeAtTilePos(roomTileX + (roomTileY << 6), layer);
}

u32 GetTileTypeAtRoomCoords(u32 roomX, u32 roomY, u32 layer) {
    return GetTileTypeAtTilePos(RoomCoordsToTilePos(roomX, roomY), layer);
}

u32 GetTileTypeAtWorldCoords(s32 worldX, s32 worldY, u32 layer) {
    return GetTileTypeAtRoomCoords(worldX - gRoomControls.origin_x, worldY - gRoomControls.origin_y, layer);
}

u32 GetTileTypeAtEntity(Entity* entity) {
    return GetTileTypeAtWorldCoords(entity->x.HALF_U.HI, entity->y.HALF_U.HI, entity->collisionLayer);
}

u32 GetTileTypeRelativeToEntity(Entity* entity, s32 xOffset, s32 yOffset) {
    return GetTileTypeAtWorldCoords(entity->x.HALF_U.HI + xOffset, entity->y.HALF_U.HI + yOffset,
                                    entity->collisionLayer);
}

// Like GetTileTypeAtEntity, but resolving through the unmodified map data.
u32 GetTileAtEntityPos(Entity* entity) {
    u32 tilePos = RoomCoordsToTilePos(entity->x.HALF_U.HI - gRoomControls.origin_x,
                                      entity->y.HALF_U.HI - gRoomControls.origin_y);
    return TileTypeAt(MapDataOriginalFor(entity->collisionLayer), TileTypesFor(entity->collisionLayer), tilePos);
}

u32 GetActTileAtTilePos(u16 tilePos, u8 layer) {
    return ActTilesFor(layer)[tilePos];
}

u32 GetActTileAtRoomTile(u32 roomTileX, u32 roomTileY, u32 layer) {
    return ActTilesFor(layer)[roomTileX + (roomTileY << 6)];
}

u32 GetActTileAtRoomCoords(u32 roomX, u32 roomY, u32 layer) {
    return ActTilesFor(layer)[RoomCoordsToTilePos(roomX, roomY)];
}

u32 GetActTileAtWorldCoords(u32 worldX, u32 worldY, u32 layer) {
    return GetActTileAtRoomCoords(worldX - gRoomControls.origin_x, worldY - gRoomControls.origin_y, layer);
}

u32 GetActTileAtEntity(Entity* entity) {
    return GetActTileAtWorldCoords(entity->x.HALF_U.HI, entity->y.HALF_U.HI, entity->collisionLayer);
}

u32 GetActTileRelativeToEntity(Entity* entity, s32 xOffset, s32 yOffset) {
    return GetActTileAtWorldCoords(entity->x.HALF_U.HI + xOffset, entity->y.HALF_U.HI + yOffset,
                                   entity->collisionLayer);
}

u32 GetCollisionDataAtTilePos(u32 tilePos, u32 layer) {
    return CollisionDataFor(layer)[tilePos];
}

u32 GetCollisionDataAtRoomTile(u32 roomTileX, u32 roomTileY, u32 layer) {
    return CollisionDataFor(layer)[roomTileX + (roomTileY << 6)];
}

u32 GetCollisionDataAtRoomCoords(u32 roomX, u32 roomY, u32 layer) {
    return CollisionDataFor(layer)[RoomCoordsToTilePos(roomX, roomY)];
}

u32 GetCollisionDataAtWorldCoords(u32 worldX, u32 worldY, u32 layer) {
    return GetCollisionDataAtRoomCoords(worldX - gRoomControls.origin_x, worldY - gRoomControls.origin_y, layer);
}

u32 GetCollisionDataAtEntity(Entity* entity) {
    return GetCollisionDataAtWorldCoords(entity->x.HALF_U.HI, entity->y.HALF_U.HI, entity->collisionLayer);
}

u32 GetCollisionDataRelativeTo(Entity* entity, s32 xOffset, s32 yOffset) {
    return GetCollisionDataAtWorldCoords(entity->x.HALF_U.HI + xOffset, entity->y.HALF_U.HI + yOffset,
                                         entity->collisionLayer);
}

u32 GetActTileForTileType(u32 tileType) {
    // "lsls #0x12" leaves the carry set for types >= 0x4000.
    if (tileType & 0x4000) {
        return gMapSpecialTileToActTile[tileType & 0x3FFF];
    }
    return gMapTileTypeToActTile[tileType & 0x3FFF];
}

u32 sub_080B1B68(u32 tileIndex, u32 layer) {
    if (tileIndex >> 14) {
        return tileIndex;
    }
    return TileTypesFor(layer)[tileIndex];
}

static u32 TileTypeFlags(u32 tileType) {
    if (tileType & 0x4000) {
        return gUnk_080B7A3E[tileType & 0x3FFF];
    }
    return gUnk_08000360[tileType & 0x3FFF];
}

u32 sub_080B1B84(u32 tilePos, u32 layer) {
    return TileTypeFlags(GetTileTypeAtTilePos(tilePos, layer));
}

u32 sub_080B1BA4(u32 tilePos, u32 layer, u32 mask) {
    return TileTypeFlags(GetTileTypeAtTilePos(tilePos, layer)) & mask;
}

// Act tile under an entity plus an offset (used by the shadow code).
static u32 ActTileUnderEntity(Entity* entity, s32 xOffset, s32 yOffset) {
    u32 x = (xOffset + entity->x.HALF_U.HI - gRoomControls.origin_x) & 0x3F0;
    u32 y = (yOffset + entity->y.HALF_U.HI - gRoomControls.origin_y) & 0x3F0;
    return ActTilesFor(entity->collisionLayer)[(x + (y << 6)) >> 4];
}

// ---- collision --------------------------------------------------------------

typedef struct {
    void* last;
    void* first;
    void* node;
    u8 flags;
} LinkedList2;

extern LinkedList2* gUnk_02018EA0;
extern Entity* gCollidableList[];
extern u8 gCollidableCount;
extern u8 gCollisionMtx[][12]; // ColSettings, see src/collision.c
typedef CollisionResult (*CollisionHandler)(Entity*, Entity*, u32, void*);
extern const CollisionHandler gCollisionHandlers[];
extern const u8 gUnk_0800464E[];

void UpdateCollision(Entity* this) {
    if (this->flags & ENT_COLLIDE) {
        gCollidableList[gCollidableCount++] = this;
    }
}

u32 CalcCollisionDirection(s32 x1, s32 y1, s32 x2, s32 y2) {
    u32 index;
    u32 dx, dy, step;

    dx = x2 - x1;
    index = (s32)dx < 0 ? 0x20 : 0;
    if ((s32)dx < 0) {
        dx = -dx;
    }
    dy = y1 - y2;
    if ((s32)dy < 0) {
        dy = -dy;
        index += 0x10;
    }
    if (dx >= dy) {
        u32 tmp = dy;
        dy = dx;
        dx = tmp;
        index += 8;
    }
    dx <<= 3;
    step = dy << 1;
    for (int i = 0; i < 4; i++) {
        if (dx < dy) {
            break;
        }
        index++;
        dy += step;
    }
    return gUnk_0800464E[index];
}

// BBOX_REFLECT: mirror the knockback direction against a facing direction.
static u32 ReflectDirection(u32 direction, u32 facing) {
    if (((direction - ((facing - 8) & 0x1F)) & 0x1F) < 0x11) {
        return direction;
    }
    return (direction - (((direction - facing - 8) & 0x1F) << 1)) & 0x1F;
}

static bool32 CalcCollision(Entity* this, Entity* other) {
    Hitbox* bbThis = this->hitbox;
    Hitbox* bbOther = other->hitbox;
    s32 dx, dy, size;
    u32 direction;
    u8* settings;
    u32 handler;
    CollisionResult result;

    dx = this->x.HALF_U.HI - other->x.HALF_U.HI + bbThis->offset_x - bbOther->offset_x;
    size = bbThis->width + bbOther->width;
    if ((u32)(dx + size) > (u32)(size * 2)) {
        return FALSE;
    }
    dy = this->y.HALF_U.HI - other->y.HALF_U.HI + bbThis->offset_y - bbOther->offset_y;
    size = bbThis->height + bbOther->height;
    if ((u32)(dy + size) > (u32)(size * 2)) {
        return FALSE;
    }
    {
        // Entities with a 3D hitbox (collisionFlags 0x10) use its depth, others 5.
        s32 depth = ((this->collisionFlags & 0x10) ? ((Hitbox3D*)bbThis)->depth : 5) +
                    ((other->collisionFlags & 0x10) ? ((Hitbox3D*)bbOther)->depth : 5);
        s32 dz = this->z.HALF.HI - other->z.HALF.HI;
        if ((u32)(depth + dz) > (u32)(depth * 2)) {
            return FALSE;
        }
    }

    // BBOX_SOLID: measure from the solid entity's position rather than its hitbox.
    switch (((this->collisionFlags & 0x20) >> 3) + ((other->collisionFlags & 0x20) >> 2)) {
        case 4:
            dx = this->x.HALF_U.HI - other->x.HALF_U.HI - bbOther->offset_x;
            dy = this->y.HALF_U.HI - other->y.HALF_U.HI - bbOther->offset_y;
            break;
        case 8:
            dx = this->x.HALF_U.HI - other->x.HALF_U.HI + bbThis->offset_x;
            dy = this->y.HALF_U.HI - other->y.HALF_U.HI + bbThis->offset_y;
            break;
        case 12:
            dx = this->x.HALF_U.HI - other->x.HALF_U.HI;
            dy = this->y.HALF_U.HI - other->y.HALF_U.HI;
            break;
    }

    direction = CalcCollisionDirection(dx, dy, 0, 0);
    settings = gCollisionMtx[other->hitType * 34 + this->hurtType];
    handler = settings[0] == 0xFF ? *(u16*)&settings[2] : 0;
    result = gCollisionHandlers[handler](this, other, direction, settings);
    if (result == RESULT_NO_COLLISION) {
        return FALSE;
    }
    if (result == RESULT_COLLISION_WITHOUT_SET) {
        return TRUE;
    }

    switch (((this->collisionFlags & 0x80) >> 5) + ((other->collisionFlags & 0x80) >> 4)) {
        case 4:
            direction = ReflectDirection(direction, this->direction);
            break;
        case 8:
            direction = ReflectDirection(direction, other->direction ^ 0x10);
            break;
        case 12:
            direction = this->direction;
            break;
    }
    other->knockbackDirection = direction;
    this->knockbackDirection = direction ^ 0x10;
    return TRUE;
}

void ram_CollideAll(void) {
    LinkedList2* start;
    LinkedList2* link;
    u32 count = gCollidableCount;

    if (count == 0) {
        return;
    }
    start = gUnk_02018EA0;
    link = start;
    do {
        Entity* this = link->node;
        if (this->flags & ENT_COLLIDE) {
            u32 classBit = 1 << (this->collisionFlags & 7);
            u32 thisFlags = this->collisionFlags;
            // Quirk: after a hit this also includes confusedTime (the asm reuses r2).
            u32 thisIframes = (u8)this->iframes;
            s32 i = count;
            while (--i >= 0) {
                Entity* other = gCollidableList[i];
                if (this == other || !(other->collisionMask & classBit) || !(other->flags & ENT_COLLIDE) ||
                    !(this->collisionLayer & other->collisionLayer)) {
                    continue;
                }
                // "lsrs #7" then "bhs": the carry is bit 6, so 0x40 skips the iframe check.
                if (!((other->collisionFlags | thisFlags) & 0x40) && ((u8)other->iframes | thisIframes)) {
                    continue;
                }
                if (!CalcCollision(this, other)) {
                    continue;
                }
                thisFlags = this->collisionFlags;
                thisIframes = (u8)this->iframes | this->confusedTime;
                if (!(other->contactFlags & 0x80)) {
                    other->contactedEntity = this;
                    other->contactFlags = this->hurtType | 0x80;
                }
                if (!(this->contactFlags & 0x80)) {
                    this->contactedEntity = other;
                    this->contactFlags = other->hurtType | 0x80;
                }
            }
        }
        link = link->first;
    } while (link != start);
}

// Push-apart used for solid entities (target, origin).
u32 ram_sub_080B227C(Entity* tgt, Entity* org) {
    Hitbox* bbTgt = tgt->hitbox;
    Hitbox* bbOrg = org->hitbox;
    s32 d, size, overlapX, overlapY;
    u32 dirX, direction;

    if (bbTgt == NULL || bbOrg == NULL) {
        return FALSE;
    }
    d = org->x.HALF_U.HI + bbOrg->offset_x - tgt->x.HALF_U.HI - bbTgt->offset_x;
    size = bbTgt->width + bbOrg->width;
    if ((u32)(size * 2) < (u32)(d + size)) {
        return FALSE;
    }
    dirX = d >= 0 ? 8 : 0x18;
    overlapX = size - (d >= 0 ? d : -d);

    d = org->y.HALF_U.HI + bbOrg->offset_y - tgt->y.HALF_U.HI - bbTgt->offset_y;
    size = bbTgt->height + bbOrg->height;
    if ((u32)(size * 2) < (u32)(d + size)) {
        return FALSE;
    }
    direction = d >= 0 ? 0x10 : 0;
    overlapY = size - (d >= 0 ? d : -d);

    if ((u32)overlapY >= (u32)overlapX) {
        overlapY = overlapX;
        direction = dirX;
    }
    if (overlapY == 0) {
        return FALSE;
    }
    if ((u32)overlapY >= 5) {
        overlapY = 4;
    }
    sub_080044AE(org, overlapY << 8, direction);
    return TRUE;
}

// ---- entity update loop -----------------------------------------------------

typedef struct {
    void* table;
    void* list_top;
    Entity* current_entity;
    void* restore_sp;
} UpdateContext;

extern UpdateContext gUpdateContext;
extern Entity* gEnemyTarget;

void DeleteThisEntity(void);
void PlayerUpdate(Entity*);
void EnemyUpdate(Entity*);
void ProjectileUpdate(Entity*);
void ObjectUpdate(Entity*);
void NPCUpdate(Entity*);
void ItemUpdate(Entity*);
void ManagerUpdate(Entity*);

// ram_sub_080B2248: update function by entity kind.
static void (*const sKindUpdates[])(Entity*) = {
    (void (*)(Entity*))DeleteThisEntity, PlayerUpdate, (void (*)(Entity*))DeleteThisEntity,
    EnemyUpdate, ProjectileUpdate, (void (*)(Entity*))DeleteThisEntity,
    ObjectUpdate, NPCUpdate, ItemUpdate, ManagerUpdate,
};

// Lists updated by mode (gUnk_080026A4): entities use lists 0-7, managers list 8.
static const struct {
    u8 first;
    u8 end;
} sUpdateLists[] = { { 0, 8 }, { 8, 9 } };

// DeleteThisEntity jumps back here so the loop continues with the next entity,
// the way ClearAndUpdateEntities restores the saved stack pointer.
static jmp_buf sUpdateResume;
static bool32 sUpdating;

void ram_UpdateEntities(u32 mode) {
    u32 list;

    gUpdateContext.table = (void*)&sUpdateLists[mode];
    sUpdating = TRUE;
    for (list = sUpdateLists[mode].first; list < sUpdateLists[mode].end; list++) {
        LinkedList* head = &gEntityLists[list];
        Entity* entity;
        gUpdateContext.list_top = head;
        entity = head->first;
        while (entity != (Entity*)head) {
            gEnemyTarget = NULL;
            gUpdateContext.current_entity = entity;
            if (setjmp(sUpdateResume) == 0) {
                sKindUpdates[entity->kind](entity);
                if (gUpdateContext.current_entity == entity) {
                    UpdateCollision(entity);
                }
            }
            entity = gUpdateContext.current_entity->next;
        }
    }
    gUpdateContext.current_entity = NULL;
    sUpdating = FALSE;
}

void ram_ClearAndUpdateEntities(void) {
    if (sUpdating) {
        longjmp(sUpdateResume, 1);
    }
}

// ---- flags and palettes -----------------------------------------------------

u32 CheckBits(void* base, u32 bit, u32 count) {
    u8* p = (u8*)base + (bit >> 3);
    u32 mask = 0x01010101u << (bit & 7);

    do {
        if (!(*p & mask)) {
            return 0;
        }
        // Rotate to the next bit; the carry out of bit 31 moves to the next byte.
        if (mask & 0x80000000u) {
            mask = (mask << 1) + 1;
            p++;
        } else {
            mask <<= 1;
        }
    } while (--count != 0);
    return 1;
}

extern const u16* const gUnk_08000F54[]; // per brightness: red, green, blue tables, 0

void ram_MakeFadeBuff256(u16* src, u16* dest, u32 amount, u32 target) {
    s32 base = amount * target;
    s32 scale = 0x400 - (amount << 2);
    const u16* const* tables = &gUnk_08000F54[gSaveHeader->brightness * 4];

    for (int i = 0; i < 16; i++) {
        u32 color = (u32)*src++ << 1;
        u32 r = (((color & 0x3E) * scale + base) >> 10) & 0x3E;
        u32 g = ((((color >> 5) & 0x3E) * scale + base) >> 10) & 0x3E;
        u32 b = ((((color >> 10) & 0x3E) * scale + base) >> 10) & 0x3E;
        *dest++ = tables[0][r / 2] | tables[1][g / 2] | tables[2][b / 2];
    }
}

// ---- sprites ----------------------------------------------------------------

// The IWRAM copy of unusedLabel_080B2AA8 (data/const/interrupts.s) starts at
// 0x0300681C; tables in it refer to its own sprite lists by IWRAM address.
extern const u8 unusedLabel_080B2AA8[];
#define IWRAM_SPRITE_DATA_BASE 0x0300681Cu

static const u8* IwramSpriteData(u32 address) {
    return unusedLabel_080B2AA8 + (address - IWRAM_SPRITE_DATA_BASE);
}

static u32 ReadWord(const u8* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

#define SPRITE_DATA(romOffset) (unusedLabel_080B2AA8 + (romOffset))
#define SHADOW_OVERLAY_TABLE SPRITE_DATA(0xB0) // 0x080B2B58
#define SHADOW_SPRITE_TABLE SPRITE_DATA(0x130) // 0x080B2BD8
#define OBJ_DIMENSIONS SPRITE_DATA(0x140)      // 0x080B2BE8

extern u32 gFrameObjLists[];

// Draw buckets by priority, and the shadows queued while drawing one (the IWRAM
// buffers at 0x03000520.. that gUnk_081326EC points to on GBA).
#define DRAW_BUCKET_SIZE 80
#define SHADOW_LIST_SIZE 0x40
typedef struct {
    u8 count;
    Entity* entities[DRAW_BUCKET_SIZE];
} DrawBucket;
DrawBucket gDrawBuckets[4];
static struct {
    u8 count;
    u16 entries[SHADOW_LIST_SIZE][2];
} sShadows;

typedef struct {
    s32 x;          // r6
    s32 y;          // r7
    u32 attr;       // r8: OAM attributes 0 and 1
    u32 tile;       // sb: tile number, palette and priority (OAM attribute 2)
    const u8* objs; // sl: frame object list
} DrawState;

static jmp_buf sOamFull;

static void EmitFrameObjs(DrawState* st) {
    const u8* dims;
    u8* out;
    u32 count = *st->objs++;
    u32 index;
    u32 offset;

    if (count == 0) {
        return;
    }
    if (st->attr & 0x300) {
        offset = ((st->attr & 0x300) - 0x100) ? 0xC0 : 0;
    } else {
        offset = ((st->attr & 0x30000000) >> 24) * 3;
    }
    dims = OBJ_DIMENSIONS + offset;
    index = gOAMControls.updated;
    out = (u8*)&gOAMControls.oam[index];

    for (; count != 0; count--, st->objs += 5) {
        const u8* obj = st->objs;
        s32 x = (s8)obj[0];
        s32 y = (s8)obj[1];
        u32 flags = obj[2];
        const u8* d;
        u32 attr;
        u16 tile;

        if (!(st->attr & 0x300)) {
            if (st->attr & 0x20000000) {
                y = -y;
            }
            if (st->attr & 0x10000000) {
                x = -x;
            }
        }
        d = dims + ((flags & 0xF0) >> 2);
        y += st->y - d[1];
        if (y >= 0xA0 || y + d[3] <= 0) {
            continue;
        }
        x += st->x - d[0];
        if (x >= 0xF0 || x + d[2] <= 0) {
            continue;
        }
        attr = (y & 0xFF) | (((u32)x << 23) >> 7) | st->attr | ((flags & 0xC0) << 8);
        attr ^= (flags & 0x3C) << 26;
        memcpy(out, &attr, 4);
        tile = obj[3] + st->tile;
        if (flags & 1) {
            tile &= ~0xF000;
        }
        tile += obj[4] << 8;
        memcpy(out + 4, &tile, 2);
        out += 8;
        if (++index >= 0x80) {
            gOAMControls.updated = 0x80;
            longjmp(sOamFull, 1);
        }
    }
    gOAMControls.updated = index;
}

// sub_080B299C: position and attributes for an entity's sprite.
static void LoadEntityDrawState(DrawState* st, Entity* e) {
    u32 settings;
    u32 flash = e->iframes > 0 ? gOAMControls._0[6] : 0;

    memcpy(&settings, &e->spriteSettings, 4); // spriteSettings..spriteOrientation
    st->tile = e->spriteVramOffset;
    st->tile |= ((e->palette.raw | flash) & 0xF) << 12;
    st->tile |= (((u8*)&e->spriteOrientation)[0] & 0xC0) << 4;
    st->x = e->x.HALF.HI + (s8)e->spriteOffsetX;
    st->y = e->y.HALF.HI + e->z.HALF.HI + e->spriteOffsetY;
    if ((settings & 3) != 2) {
        st->x -= (s16)gOAMControls._4;
        st->y -= (s16)gOAMControls._6;
    }
    st->attr = (settings & 0x3E003F00) | (gOAMControls.spritesOffset << 12) |
               (((e->frameSpriteSettings ^ settings) & 0xC0) << 22);
}

static void DrawFrame(DrawState* st, u32 frameIndex, u32 spriteIndex) {
    const u8* base = (const u8*)gFrameObjLists;
    const u8* frames = base + ReadWord(base + spriteIndex * 4);
    st->objs = base + ReadWord(frames + frameIndex * 4);
    EmitFrameObjs(st);
}

static void DrawEntityFrame(DrawState* st, Entity* e) {
    if (e->frameIndex != 0xFF) {
        DrawFrame(st, e->frameIndex, (u16)e->spriteIndex);
    }
}

// _080B2718: the entity's own sprite, its custom object list, or its sub-sprites.
static void DrawEntitySprite(DrawState* st, Entity* e) {
    s8 mode = e->spriteAnimation[2];

    if (mode == 0) {
        DrawEntityFrame(st, e);
    } else if (mode < 0) {
        st->objs = e->myHeap;
        EmitFrameObjs(st);
    } else {
        struct_gUnk_020000C0_1* sub = gUnk_020000C0[mode].unk_00;
        st->tile &= ~0xF000;
        for (int i = 0; i < 4; i++, sub++) {
            const u8* raw = (const u8*)sub;
            if (!(raw[0] & 1)) {
                return;
            }
            if (raw[0] & 2) {
                // Sub-sprite borrowed from another entity; this leaves its position
                // in the state for the caller, like the asm.
                Entity* linked = sub->unk_0C;
                if (linked != NULL) {
                    LoadEntityDrawState(st, linked);
                    st->y += gOAMControls._0[10];
                    DrawEntityFrame(st, linked);
                }
            } else if (sub->unk_01 != 0xFF) {
                DrawState saved = *st;
                s32 dx = (s8)sub->unk_04.BYTES.byte2;
                st->attr ^= (u32)sub->unk_04.BYTES.byte0 << 28;
                if (st->attr & 0x10000000) {
                    st->x -= dx;
                } else {
                    st->x += dx;
                }
                st->y += (s8)sub->unk_04.BYTES.byte3; // quirk: added for either flip
                st->tile |= sub->unk_04.BYTES.byte1 << 12;
                st->tile += sub->unk_08.BYTES.byte0;
                DrawFrame(st, sub->unk_01, sub->unk_02);
                *st = saved;
            }
        }
    }
}

// sub_080B255C: one entity with its ground overlay and shadow.
static void DrawEntityWithShadow(Entity* e) {
    DrawState st;
    u8 prio = ((u8*)&e->spritePriority)[0];

    LoadEntityDrawState(&st, e);
    gOAMControls._0[10] = 0;
    if (!(prio & 8)) {
        DrawEntitySprite(&st, e);
        return;
    }
    if (e->z.HALF.HI >= 0) {
        u32 actTile = ActTileUnderEntity(e, 0, 0);
        u32 shadowSize = e->spriteSettings.shadow << 4;
        s32 variant = -1;

        if (actTile == 0x19) {
            gOAMControls._0[10] = 2;
            st.y += 2;
        } else if (actTile == 0x2F) {
            variant = ((u8)e->x.HALF.HI ^ (u8)e->y.HALF.HI) & 6;
        } else if (actTile == 0x0F) {
            gOAMControls._0[10] = 2;
            st.y += 2;
            variant = ((gOAMControls.field_0x1 & 0x18) + 0x80) >> 2;
        }
        if (variant >= 0) {
            // Ground overlay (e.g. grass, water) drawn in front, and no shadow.
            DrawState overlay = st;
            overlay.objs = IwramSpriteData(ReadWord(SHADOW_OVERLAY_TABLE + shadowSize + variant * 2));
            overlay.attr = 0;
            overlay.tile &= 0xC00;
            EmitFrameObjs(&overlay);
            DrawEntitySprite(&st, e);
            return;
        }
    }
    DrawEntitySprite(&st, e);

    {
        s32 z = e->z.HALF.HI;
        s32 shadowY = st.y;

        if ((prio & 0x10) && z >= 0) {
            return;
        }
        if ((prio & 0x20) && (gOAMControls.field_0x1 & 1)) {
            return;
        }
        if (z < 0) {
            shadowY -= z;
        }
        if (sShadows.count >= SHADOW_LIST_SIZE) {
            return;
        }
        sShadows.entries[sShadows.count][0] = e->spriteSettings.shadow | (st.x << 6);
        sShadows.entries[sShadows.count][1] = ((st.tile >> 10) & 3) | (shadowY << 6);
        sShadows.count++;
    }
}

// Higher keys first: lower on screen, then higher priority (spritePriority & 7).
static u32 DrawKey(Entity* e) {
    return ((u32)(e->y.WORD + 0x80000000u) >> 3) | ((u32)~((u8*)&e->spritePriority)[0] << 29);
}

// ResolveOamDrawPriority: the shell sort from the asm, same comparisons and order.
static void SortBucket(DrawBucket* bucket) {
    Entity** e = bucket->entities;
    s32 last = bucket->count - 1;
    s32 gap = last;

    while (gap != 0) {
        for (s32 start = 0; start < gap; start++) {
            for (s32 p = last - gap - start; p >= 0; p -= gap) {
                Entity* a = e[p];
                u32 key = DrawKey(a);
                s32 q = p + gap;
                while (q <= last && key < DrawKey(e[q])) {
                    e[q - gap] = e[q];
                    q += gap;
                }
                e[q - gap] = a;
            }
        }
        gap >>= 1;
    }
}

static void DrawBucketEntities(DrawBucket* bucket) {
    if (bucket->count == 0) {
        return;
    }
    sShadows.count = 0;
    SortBucket(bucket);
    for (u32 i = 0; i < bucket->count; i++) {
        DrawEntityWithShadow(bucket->entities[i]);
    }
    for (u32 i = 0; i < sShadows.count; i++) {
        DrawState st;
        s16 a = sShadows.entries[i][0];
        s16 b = sShadows.entries[i][1];
        st.x = a >> 6;
        st.objs = IwramSpriteData(ReadWord(SHADOW_SPRITE_TABLE + (a & 7) * 4));
        st.y = b >> 6;
        st.tile = (b & 3) << 10;
        st.attr = 0;
        EmitFrameObjs(&st);
    }
}

void ram_DrawEntities(void) {
    if (gOAMControls.updated >= 0x80) {
        return;
    }
    if (setjmp(sOamFull) != 0) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        DrawBucketEntities(&gDrawBuckets[i]);
    }
}

void ram_sub_080ADA04(OAMCommand* cmd, void* objs) {
    DrawState st;

    if (*(u8*)objs == 0 || gOAMControls.updated >= 0x80) {
        return;
    }
    if (setjmp(sOamFull) != 0) {
        return;
    }
    st.objs = objs;
    st.x = cmd->x;
    st.y = cmd->y;
    st.attr = cmd->_4 | (cmd->_6 << 16);
    st.tile = cmd->_8;
    EmitFrameObjs(&st);
}

void ram_DrawDirect(OAMCommand* cmd, u32 spriteIndex, u32 frameIndex) {
    const u8* base = (const u8*)gFrameObjLists;
    const u8* frames;

    if (frameIndex == 0xFF) {
        return;
    }
    frames = base + ReadWord(base + spriteIndex * 4);
    ram_sub_080ADA04(cmd, (void*)(base + ReadWord(frames + frameIndex * 4)));
}

// ---- scrolling ----------------------------------------------------------------

// sub_080B197C: copy the visible window of a 128-tile-wide special map into a
// BG buffer: 23 DMA copies of 0x80 bytes, source rows 0x100 apart, destination
// rows 0x40 apart (each copy runs into the next row, which the next one rewrites).
// Quirk: when the view is less than 8 pixels into a tile row, the first source
// row is copied twice instead of starting one row higher.
void ram_sub_080B197C(void* src, void* dest) {
    u32 dx = (u32)(gRoomControls.scroll_x - gRoomControls.origin_x);
    u32 dy = (u32)(gRoomControls.scroll_y - gRoomControls.origin_y);
    const u8* s = (const u8*)src + (((dx >> 4) + ((dy >> 4) << 7)) << 2);
    u8* d = (u8*)dest - 0x40;
    int rows = 0x17;

    if (dy < 8) {
        memcpy(d, s, 0x80);
        goto next;
    }
    s -= 0x100;
    do {
        memcpy(d, s, 0x80);
        s += 0x100;
    next:
        d += 0x40;
    } while (--rows != 0);
}
