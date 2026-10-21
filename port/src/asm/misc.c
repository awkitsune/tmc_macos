/*
 * C versions of the small asm files: code_08000E44.s, code_08000F10.s,
 * code_08007CAC.s and projectileUpdate.s.
 */

#include "global.h"
#include "asm.h"
#include "entity.h"
#include "port/resources.h"

// ---- code_08000E44.s ----------------------------------------------------------

extern u32 gRand;
extern u8 gUnk_03003DE0;

// Sign of a value: -1, 0 or 1.
s32 sub_08000E44(s32 value) {
    if (value == 0) {
        return 0;
    }
    return value < 0 ? -1 : 1;
}

u32 Random(void) {
    u32 r = gRand * 3;
    r = (r >> 13) | (r << 19);
    gRand = r;
    return r >> 1;
}

// Number of set bits.
u32 sub_08000E62(u32 value) {
    return __builtin_popcount(value);
}

ResourceRequest gResourceQueue[RESOURCE_QUEUE_SIZE];

static void QueueResource(u32 type, const void* src, void* dest, u32 size) {
    ResourceRequest* request;

    if (gUnk_03003DE0 >= RESOURCE_QUEUE_SIZE) {
        return;
    }
    request = &gResourceQueue[gUnk_03003DE0++];
    request->type = type;
    request->size = size;
    request->src = src;
    request->dest = dest;
}

void LoadResourceAsync(const void* src, void* dest, u32 size) {
    QueueResource(0, src, dest, size);
}

void sub_08000E92(const void* src, void* dest, u32 size) {
    QueueResource(1, src, dest, size);
}

// ---- code_08000F10.s ----------------------------------------------------------

void SumDropProbabilities(s16* out, const s16* a, const s16* b, const s16* c) {
    for (int i = 15; i >= 0; i--) {
        out[i] = a[i] + b[i] + c[i];
    }
}

u32 SumDropProbabilities2(s16* out, const s16* a, const s16* b, const s16* c) {
    u32 sum = 0;
    for (int i = 15; i >= 0; i--) {
        s32 value = a[i] + b[i] + c[i];
        if (value < 0) {
            value = 0;
        }
        out[i] = value;
        sum += value;
    }
    return sum;
}

// ---- code_08007CAC.s ----------------------------------------------------------

u32 FindValueForKey(u32 key, const KeyValuePair* list) {
    for (; list->key != 0; list++) {
        if (list->key == key) {
            return list->value;
        }
    }
    return 0;
}

// ---- projectileUpdate.s -------------------------------------------------------

bool32 ProjectileInit(Entity*);
void DrawEntity(Entity*);
extern void (*const gProjectileFunctions[])(Entity*);

void ProjectileUpdate(Entity* this) {
    if (this->action == 0) {
        if (!ProjectileInit(this)) {
            DeleteThisEntity();
        }
    } else {
        if (EntityDisabled(this)) {
            DrawEntity(this);
            return;
        }
        sub_080028E0(this);
    }
    gProjectileFunctions[this->id](this);
    this->contactFlags &= 0x7F;
    DrawEntity(this);
}
