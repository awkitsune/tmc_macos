#ifndef GLOBAL_H
#define GLOBAL_H

/**
 * @defgroup Tasks Tasks
 * @defgroup Subtasks Subtasks
 * @brief Subtasks override the game task for short periods.
 * @defgroup WorldEvents World Events
 * @brief Cutscenes that happen after a kinstone fusion.
 */

/**
 * @defgroup Entities Entities
 */
///@{
/**
 * @defgroup Player Player
 * @defgroup Enemies Enemies
 * @defgroup Projectiles Projectiles
 * @defgroup Objects Objects
 * @defgroup NPCs NPCs
 * @defgroup Items Items
 * @defgroup Managers Managers
 * @brief Entities with a smaller footprint of 0x40 bytes.
 */
///@}

#include "gba/gba.h"
#include <string.h>

// Prevent cross-jump optimization.
#define BLOCK_CROSS_JUMP asm("");

// to help in decompiling
#define asm_comment(x) asm volatile("@ -- " x " -- ")
#define asm_unified(x) asm(".syntax unified\n" x "\n.syntax divided")

#if (defined(__APPLE__) || defined(__CYGWIN__)) && !defined(PLATFORM_NATIVE)
// Get the IDE to stfu

// We define it this way to fool preproc.
#define INCBIN(...) \
    { 0 }
#define INCBIN_U8 INCBIN
#define INCBIN_U16 INCBIN
#define INCBIN_U32 INCBIN
#define INCBIN_S8 INCBIN
#define INCBIN_S16 INCBIN
#define INCBIN_S32 INCBIN
#define _(x) (x)
#define __(x) (x)
#endif // __APPLE__

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define SWAP(a, b, temp) \
    {                    \
        (temp) = a;      \
        (a) = b;         \
        (b) = temp;      \
    }

// useful math macros

// Converts a number to Q8.8 fixed-point format
#define Q_8_8(n) ((s16)((n)*256))

// Converts a number to Q16.16 fixed-point format
#define Q_16_16(n) ((s32)((n) * (1 << 16)))

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) >= (b) ? (a) : (b))

#ifdef PLATFORM_NATIVE
// Size asserts pin down the 32-bit GBA layout, which 64-bit pointers change by design.
#define static_assert(cond)
#else
#define static_assert(cond) extern char assertion[(cond) ? 1 : -1]
#endif

#ifdef PLATFORM_NATIVE
// The ROM's __divsi3 / __modsi3 (asm/lib/libgcc.s) return 0 for a zero divisor and otherwise use the BIOS
// Div, which gives INT_MIN / -1 = INT_MIN rem 0. Natively arm64 gives the dividend for % 0 and x86_64 traps
// on both, so a division whose divisor can be 0 goes through these (see port/notes/native-hazards.md).
static inline s32 GbaDiv(s32 num, s32 denom) {
    return denom == 0 ? 0 : denom == -1 ? (s32)(0u - (u32)num) : num / denom;
}
static inline s32 GbaMod(s32 num, s32 denom) {
    return denom == 0 || denom == -1 ? 0 : num % denom;
}
#define GBA_DIV(num, denom) GbaDiv(num, denom)
#define GBA_MOD(num, denom) GbaMod(num, denom)
#else
#define GBA_DIV(num, denom) ((num) / (denom))
#define GBA_MOD(num, denom) ((num) % (denom))
#endif

#define super (&this->base)

#if NON_MATCHING
#define ASM_FUNC(path, decl)
#else
#define ASM_FUNC(path, decl)    \
    NAKED decl {                \
        asm(".include " #path); \
    }
#endif

#if NON_MATCHING
#define NONMATCH(path, decl) decl
#define END_NONMATCH
#else
#define NONMATCH(path, decl)    \
    NAKED decl {                \
        asm(".include " #path); \
        if (0)
#define END_NONMATCH }
#endif

#if NON_MATCHING
#define FORCE_REGISTER(var, reg) var
#else
#define FORCE_REGISTER(var, reg) register var asm(#reg)
#endif

#if NON_MATCHING
#define MEMORY_BARRIER
#else
#define MEMORY_BARRIER asm("" ::: "memory")
#endif

typedef union {
    s32 WORD;
    struct {
        s16 x, y;
    } HALF;
} Coords;

typedef struct {
    s8 x;
    s8 y;
} PACKED Coords8;

union SplitDWord {
    s64 DWORD;
    u64 DWORD_U;
    struct {
        s32 LO, HI;
    } HALF;
    struct {
        u32 LO, HI;
    } HALF_U;
};

union SplitWord {
    s32 WORD;
    u32 WORD_U;
    struct {
        s16 LO, HI;
    } HALF;
    struct {
        u16 LO, HI;
    } HALF_U;
    struct {
        u8 byte0, byte1, byte2, byte3;
    } BYTES;
};

union SplitHWord {
    u16 HWORD;
    struct {
        u8 LO, HI;
    } PACKED HALF;
} PACKED;

#define FORCE_WORD_ALIGNED __attribute__((packed, aligned(2)))

/* forward decls */
struct Entity_;

/**
 * bitset macros
 */

#define BIT(bit) (1 << (bit))
#define IS_BIT_SET(value, bit) ((value)&BIT(bit))

/**
 * Multi return function data type casts
 */
typedef u64 (*MultiReturnTypeSingleEntityArg)(struct Entity_*);
typedef s64 (*MultiReturnTypeTwoS32Arg)(s32, s32);

#endif // GLOBAL_H
