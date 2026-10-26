/*
 * GBA BIOS functions in C. Behaviour follows GBATEK; these replace
 * asm/lib/libagbsyscall.s for the native build.
 */

#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "port/hw.h"
#include "port/platform.h"
#include "port/state.h"

typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

// ---- memory -----------------------------------------------------------------

#define CPU_SET_SRC_FIXED (1u << 24)
#define CPU_SET_32BIT (1u << 26)

void CpuSet(const void* src, void* dest, u32 control) {
    u32 count = control & 0x1FFFFF;
    int fixed = (control & CPU_SET_SRC_FIXED) != 0;

    if (control & CPU_SET_32BIT) {
        const u32* s = src;
        u32* d = dest;
        for (u32 i = 0; i < count; i++) {
            d[i] = fixed ? s[0] : s[i];
        }
    } else {
        const u16* s = src;
        u16* d = dest;
        for (u32 i = 0; i < count; i++) {
            d[i] = fixed ? s[0] : s[i];
        }
    }
}

void CpuFastSet(const void* src, void* dest, u32 control) {
    // Always 32-bit, in blocks of 8 words.
    u32 count = ((control & 0x1FFFFF) + 7) & ~7u;
    CpuSet(src, dest, (control & CPU_SET_SRC_FIXED) | CPU_SET_32BIT | count);
}

void RegisterRamReset(u32 flags) {
    if (flags & 0x04) {
        memset(gHwPltt, 0, sizeof(gHwPltt));
    }
    if (flags & 0x08) {
        memset(gHwVram, 0, sizeof(gHwVram));
    }
    if (flags & 0x10) {
        memset(gHwOam, 0, sizeof(gHwOam));
    }
    if (flags & 0x80) {
        // KEYINPUT reflects the buttons; reset leaves it alone.
        uint16_t keys;
        memcpy(&keys, gHwIo + 0x130, 2);
        memset(gHwIo, 0, sizeof(gHwIo));
        memcpy(gHwIo + 0x130, &keys, 2);
    }
    if (flags & 0x40) {
        // The sound registers and wave RAM (0x060-0x0A7) are cleared, except
        // SOUNDBIAS, which gets its default 0200h (bias level 100h).
        memset(gHwIo + 0x60, 0, 0x48);
        gHwIo[0x89] = 0x02;
    }
    // EWRAM/IWRAM (0x01/0x02) are ordinary globals natively; only boot and soft
    // reset ask for them, and both start from zeroed (or deliberately kept) state.
}

// ---- decompression ----------------------------------------------------------

static u32 ReadHeader(const u8** src) {
    const u8* s = *src;
    u32 header = s[0] | (s[1] << 8) | (s[2] << 16) | ((u32)s[3] << 24);
    *src = s + 4;
    return header >> 8;
}

// The Vram variants write in 16-bit units on hardware; byte writes are equivalent
// for host memory.
static void LZ77UnComp(const void* src, void* dest) {
    const u8* s = src;
    u8* d = dest;
    u32 remaining = ReadHeader(&s);

    while (remaining > 0) {
        u8 flags = *s++;
        for (int i = 0; i < 8 && remaining > 0; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (s[0] >> 4) + 3;
                u32 disp = (((s[0] & 0xF) << 8) | s[1]) + 1;
                s += 2;
                for (; len > 0 && remaining > 0; len--, remaining--, d++) {
                    *d = *(d - disp);
                }
            } else {
                *d++ = *s++;
                remaining--;
            }
        }
    }
}

void LZ77UnCompWram(const void* src, void* dest) {
    LZ77UnComp(src, dest);
}

void LZ77UnCompVram(const void* src, void* dest) {
    LZ77UnComp(src, dest);
}

static void RLUnComp(const void* src, void* dest) {
    const u8* s = src;
    u8* d = dest;
    u32 remaining = ReadHeader(&s);

    while (remaining > 0) {
        u8 flag = *s++;
        if (flag & 0x80) {
            u32 len = (flag & 0x7F) + 3;
            u8 value = *s++;
            for (; len > 0 && remaining > 0; len--, remaining--) {
                *d++ = value;
            }
        } else {
            u32 len = (flag & 0x7F) + 1;
            for (; len > 0 && remaining > 0; len--, remaining--) {
                *d++ = *s++;
            }
        }
    }
}

void RLUnCompWram(const void* src, void* dest) {
    RLUnComp(src, dest);
}

void RLUnCompVram(const void* src, void* dest) {
    RLUnComp(src, dest);
}

// ---- arithmetic -------------------------------------------------------------

// Returns the quotient in the low word and the remainder in the high word, as the
// BIOS does in r0/r1: the game's DivAndMod calls Div through an s64-returning cast,
// while callers using the s32 prototype only see the quotient.
s64 Div(s32 num, s32 denom) {
    if (denom == 0) {
        return 0;
    }
    s32 quot = num / denom;
    s32 rem = num % denom;
    return (s64)(((uint64_t)(u32)rem << 32) | (u32)quot);
}

s32 Mod(s32 num, s32 denom) {
    return denom == 0 ? 0 : num % denom;
}

u16 Sqrt(u32 num) {
    u32 root = 0;
    u32 bit = 1u << 30;
    while (bit > num) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (num >= root + bit) {
            num -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (u16)root;
}

static s32 ArcTan(s32 i) {
    s32 a = -((i * i) >> 14);
    s32 b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}

u16 ArcTan2(s16 x, s16 y) {
    if (y == 0) {
        return x >= 0 ? 0 : 0x8000;
    }
    if (x == 0) {
        return y >= 0 ? 0x4000 : 0xC000;
    }
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y) {
                return ArcTan((y << 14) / x);
            }
        } else if (-x >= y) {
            return ArcTan((y << 14) / x) + 0x8000;
        }
        return 0x4000 - ArcTan((x << 14) / y);
    }
    if (x <= 0) {
        if (-x > -y) {
            return ArcTan((y << 14) / x) + 0x8000;
        }
    } else if (x >= -y) {
        return ArcTan((y << 14) / x) + 0x10000;
    }
    return 0xC000 - ArcTan((x << 14) / y);
}

// ---- affine -----------------------------------------------------------------

struct BgAffineSrcData {
    s32 texX;
    s32 texY;
    s16 scrX;
    s16 scrY;
    s16 sx;
    s16 sy;
    u16 alpha;
};

struct BgAffineDstData {
    s16 pa;
    s16 pb;
    s16 pc;
    s16 pd;
    s32 dx;
    s32 dy;
};

struct ObjAffineSrcData {
    s16 xScale;
    s16 yScale;
    u16 rotation;
};

// The BIOS uses a 256-entry sine table in 1.14 fixed point, indexed by angle >> 8.
static s16 BiosSin(u8 index) {
    return (s16)lrint(sin(index * (2.0 * M_PI / 256.0)) * 0x4000);
}

static s16 BiosCos(u8 index) {
    return BiosSin((u8)(index + 64));
}

void BgAffineSet(struct BgAffineSrcData* src, struct BgAffineDstData* dest, s32 count) {
    for (; count > 0; count--, src++, dest++) {
        u8 angle = src->alpha >> 8;
        s32 sin = BiosSin(angle), cos = BiosCos(angle);
        s32 pa = (cos * src->sx) >> 14;
        s32 pb = (-sin * src->sx) >> 14;
        s32 pc = (sin * src->sy) >> 14;
        s32 pd = (cos * src->sy) >> 14;
        dest->pa = pa;
        dest->pb = pb;
        dest->pc = pc;
        dest->pd = pd;
        dest->dx = src->texX - pa * src->scrX - pb * src->scrY;
        dest->dy = src->texY - pc * src->scrX - pd * src->scrY;
    }
}

// The BIOS reads 8-byte source entries (scale x, scale y, angle, 2 unused bytes);
// struct ObjAffineSrcData has only the 6 used ones. The game passes its 8-byte
// gOAMControls.unk entries, so stepping by the struct read every group after
// the first from the wrong place (all zero: solid squares for affine sprites).
#define OBJ_AFFINE_SRC_STRIDE 8

void ObjAffineSet(struct ObjAffineSrcData* src, void* dest, s32 count, s32 offset) {
    u8* d = dest;
    for (; count > 0; count--, src = (struct ObjAffineSrcData*)((u8*)src + OBJ_AFFINE_SRC_STRIDE)) {
        u8 angle = src->rotation >> 8;
        s32 sin = BiosSin(angle), cos = BiosCos(angle);
        s16 params[4] = {
            (s16)((cos * src->xScale) >> 14),
            (s16)((-sin * src->xScale) >> 14),
            (s16)((sin * src->yScale) >> 14),
            (s16)((cos * src->yScale) >> 14),
        };
        for (int i = 0; i < 4; i++, d += offset) {
            memcpy(d, &params[i], sizeof(s16));
        }
    }
}

// ---- system -----------------------------------------------------------------

void SoundBiasReset(void) {
}

void SoundBiasSet(void) {
}

void VBlankIntrWait(void) {
    PlatformVBlank();
}

static bool sAtLoopBoundary STATE_EXCLUDE;

void PlatformLoopVBlank(void) {
    sAtLoopBoundary = true;
    PlatformVBlank();
    sAtLoopBoundary = false;
}

bool PlatformAtLoopBoundary(void) {
    return sAtLoopBoundary;
}

void SoftReset(u32 flags) {
    RegisterRamReset(flags);
    PlatformSoftReset();
}
