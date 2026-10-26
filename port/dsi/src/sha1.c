/*
 * SHA-1 for the ROM check. The DSi BIOS has one (swiSHA1*, SWI 0x24-0x27), but
 * it is software too, and in melonDS 1.1 SWI 0x25 jumps into the header area
 * at 0x02FFC00C and faults. This one runs as ARM code from ITCM.
 */

#include <nds/ndstypes.h>
#include <string.h>

#include "dsi.h"

static inline uint32_t Rol(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

// One round with the variables renamed instead of moved: five calls in a row
// with the names rotated are five rounds.
#define ROUND(a, b, c, d, e, f, k, x)            \
    do {                                         \
        e += Rol(a, 5) + (f) + (k) + (x);        \
        b = Rol(b, 30);                          \
    } while (0)
#define F0(b, c, d) (d ^ (b & (c ^ d)))
#define F1(b, c, d) (b ^ c ^ d)
#define F2(b, c, d) ((b & c) | (d & (b | c)))
#define LOAD(i) (w[i] = (uint32_t)data[4 * (i)] << 24 | (uint32_t)data[4 * (i) + 1] << 16 | \
                        (uint32_t)data[4 * (i) + 2] << 8 | data[4 * (i) + 3])
#define EXPAND(i) \
    (w[(i) & 15] = Rol(w[((i) + 13) & 15] ^ w[((i) + 8) & 15] ^ w[((i) + 2) & 15] ^ w[(i) & 15], 1))
#define FIVE(F, k, X, i)                                \
    do {                                                \
        ROUND(a, b, c, d, e, F(b, c, d), k, X(i));      \
        ROUND(e, a, b, c, d, F(a, b, c), k, X(i + 1));  \
        ROUND(d, e, a, b, c, F(e, a, b), k, X(i + 2));  \
        ROUND(c, d, e, a, b, F(d, e, a), k, X(i + 3));  \
        ROUND(b, c, d, e, a, F(c, d, e), k, X(i + 4));  \
    } while (0)

ARM_CODE ITCM_CODE static void Blocks(uint32_t* state, const uint8_t* data, size_t count) {
    uint32_t w[16];

    while (count--) {
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        int i;

        FIVE(F0, 0x5A827999, LOAD, 0);
        FIVE(F0, 0x5A827999, LOAD, 5);
        FIVE(F0, 0x5A827999, LOAD, 10);
        ROUND(a, b, c, d, e, F0(b, c, d), 0x5A827999, LOAD(15));
        ROUND(e, a, b, c, d, F0(a, b, c), 0x5A827999, EXPAND(16));
        ROUND(d, e, a, b, c, F0(e, a, b), 0x5A827999, EXPAND(17));
        ROUND(c, d, e, a, b, F0(d, e, a), 0x5A827999, EXPAND(18));
        ROUND(b, c, d, e, a, F0(c, d, e), 0x5A827999, EXPAND(19));
        for (i = 20; i < 40; i += 5) {
            FIVE(F1, 0x6ED9EBA1, EXPAND, i);
        }
        for (; i < 60; i += 5) {
            FIVE(F2, 0x8F1BBCDC, EXPAND, i);
        }
        for (; i < 80; i += 5) {
            FIVE(F1, 0xCA62C1D6, EXPAND, i);
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        data += 64;
    }
}

void DsiSha1Init(DsiSha1* ctx) {
    static const uint32_t initial[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };

    memcpy(ctx->state, initial, sizeof(initial));
    ctx->length = 0;
}

void DsiSha1Update(DsiSha1* ctx, const void* data, size_t length) {
    const uint8_t* p = data;
    size_t used = ctx->length % 64;

    ctx->length += length;
    if (used != 0) {
        size_t take = 64 - used < length ? 64 - used : length;
        memcpy(ctx->buffer + used, p, take);
        p += take;
        length -= take;
        if (used + take < 64) {
            return;
        }
        Blocks(ctx->state, ctx->buffer, 1);
    }
    Blocks(ctx->state, p, length / 64);
    memcpy(ctx->buffer, p + length / 64 * 64, length % 64);
}

void DsiSha1Final(DsiSha1* ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->length * 8;
    uint8_t pad[72] = { 0x80 };
    size_t padLength = 64 - (ctx->length + 8) % 64;

    if (padLength == 0) {
        padLength = 64;
    }
    for (int i = 0; i < 8; i++) {
        pad[padLength + i] = bits >> (56 - 8 * i);
    }
    DsiSha1Update(ctx, pad, padLength + 8);
    for (int i = 0; i < 20; i++) {
        digest[i] = ctx->state[i / 4] >> (24 - 8 * (i % 4));
    }
}
