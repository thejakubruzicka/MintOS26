/* X25519 (RFC 7748): Montgomery ladder over GF(2^255-19) with five 51-bit
 * limbs and 64x64->128 multiplication. Constant time (conditional swaps by
 * masking; no secret-dependent branches or indices). */
#include "crypto.h"

typedef unsigned __int128 u128;
typedef uint64_t fe[5];
#define MASK51 0x7FFFFFFFFFFFFULL

static void fe_copy(fe o, const fe a) { for (int i = 0; i < 5; ++i) o[i] = a[i]; }
static void fe_add(fe o, const fe a, const fe b) { for (int i = 0; i < 5; ++i) o[i] = a[i] + b[i]; }
/* a - b + 2p keeps limbs positive for inputs below 2^52. */
static void fe_sub(fe o, const fe a, const fe b) {
    o[0] = a[0] + 0xFFFFFFFFFFFDAULL - b[0];
    o[1] = a[1] + 0xFFFFFFFFFFFFEULL - b[1];
    o[2] = a[2] + 0xFFFFFFFFFFFFEULL - b[2];
    o[3] = a[3] + 0xFFFFFFFFFFFFEULL - b[3];
    o[4] = a[4] + 0xFFFFFFFFFFFFEULL - b[4];
}
static void fe_carry(fe o, u128 t[5]) {
    uint64_t c;
    t[1] += (uint64_t)(t[0] >> 51); o[0] = (uint64_t)t[0] & MASK51;
    t[2] += (uint64_t)(t[1] >> 51); o[1] = (uint64_t)t[1] & MASK51;
    t[3] += (uint64_t)(t[2] >> 51); o[2] = (uint64_t)t[2] & MASK51;
    t[4] += (uint64_t)(t[3] >> 51); o[3] = (uint64_t)t[3] & MASK51;
    c = (uint64_t)(t[4] >> 51); o[4] = (uint64_t)t[4] & MASK51;
    o[0] += c * 19; c = o[0] >> 51; o[0] &= MASK51;
    o[1] += c;
}
static void fe_mul(fe o, const fe a, const fe b) {
    uint64_t b1 = b[1] * 19, b2 = b[2] * 19, b3 = b[3] * 19, b4 = b[4] * 19;
    u128 t[5];
    t[0] = (u128)a[0] * b[0] + (u128)a[1] * b4 + (u128)a[2] * b3 + (u128)a[3] * b2 + (u128)a[4] * b1;
    t[1] = (u128)a[0] * b[1] + (u128)a[1] * b[0] + (u128)a[2] * b4 + (u128)a[3] * b3 + (u128)a[4] * b2;
    t[2] = (u128)a[0] * b[2] + (u128)a[1] * b[1] + (u128)a[2] * b[0] + (u128)a[3] * b4 + (u128)a[4] * b3;
    t[3] = (u128)a[0] * b[3] + (u128)a[1] * b[2] + (u128)a[2] * b[1] + (u128)a[3] * b[0] + (u128)a[4] * b4;
    t[4] = (u128)a[0] * b[4] + (u128)a[1] * b[3] + (u128)a[2] * b[2] + (u128)a[3] * b[1] + (u128)a[4] * b[0];
    fe_carry(o, t);
}
static void fe_sq(fe o, const fe a) { fe_mul(o, a, a); }
static void fe_mul_small(fe o, const fe a, uint64_t k) {
    u128 t[5];
    for (int i = 0; i < 5; ++i) t[i] = (u128)a[i] * k;
    fe_carry(o, t);
}
static void fe_cswap(fe a, fe b, uint64_t swap) {
    uint64_t m = (uint64_t)0 - swap;
    for (int i = 0; i < 5; ++i) { uint64_t x = m & (a[i] ^ b[i]); a[i] ^= x; b[i] ^= x; }
}
static void fe_from_bytes(fe o, const uint8_t s[32]) {
    uint64_t w[4];
    for (int i = 0; i < 4; ++i) { w[i] = 0; for (int j = 7; j >= 0; --j) w[i] = (w[i] << 8) | s[8 * i + j]; }
    o[0] = w[0] & MASK51;
    o[1] = ((w[0] >> 51) | (w[1] << 13)) & MASK51;
    o[2] = ((w[1] >> 38) | (w[2] << 26)) & MASK51;
    o[3] = ((w[2] >> 25) | (w[3] << 39)) & MASK51;
    o[4] = (w[3] >> 12) & MASK51;                       /* drops bit 255, as RFC 7748 requires */
}
static void fe_to_bytes(uint8_t s[32], const fe a) {
    fe t; fe_copy(t, a);
    /* Two carry passes bring each limb below 2^51; then subtract p if t >= p. */
    for (int pass = 0; pass < 2; ++pass) {
        uint64_t c;
        c = t[0] >> 51; t[0] &= MASK51; t[1] += c;
        c = t[1] >> 51; t[1] &= MASK51; t[2] += c;
        c = t[2] >> 51; t[2] &= MASK51; t[3] += c;
        c = t[3] >> 51; t[3] &= MASK51; t[4] += c;
        c = t[4] >> 51; t[4] &= MASK51; t[0] += c * 19;
    }
    /* q = 1 if t >= p: compute t + 19 and look at bit 255 */
    uint64_t q = (t[0] + 19) >> 51;
    q = (t[1] + q) >> 51; q = (t[2] + q) >> 51; q = (t[3] + q) >> 51; q = (t[4] + q) >> 51;
    t[0] += 19 * q;
    uint64_t c;
    c = t[0] >> 51; t[0] &= MASK51; t[1] += c;
    c = t[1] >> 51; t[1] &= MASK51; t[2] += c;
    c = t[2] >> 51; t[2] &= MASK51; t[3] += c;
    c = t[3] >> 51; t[3] &= MASK51; t[4] += c;
    t[4] &= MASK51;
    uint64_t w[4];
    w[0] = t[0] | (t[1] << 51);
    w[1] = (t[1] >> 13) | (t[2] << 38);
    w[2] = (t[2] >> 26) | (t[3] << 25);
    w[3] = (t[3] >> 39) | (t[4] << 12);
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) s[8 * i + j] = (uint8_t)(w[i] >> (8 * j));
}
static void fe_invert(fe o, const fe z) {
    fe z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t;
    fe_sq(z2, z);                                   /* 2 */
    fe_sq(t, z2); fe_sq(t, t);                      /* 8 */
    fe_mul(z9, t, z);                               /* 9 */
    fe_mul(z11, z9, z2);                            /* 11 */
    fe_sq(t, z11);                                  /* 22 */
    fe_mul(z2_5_0, t, z9);                          /* 2^5 - 2^0 */
    fe_sq(t, z2_5_0); for (int i = 1; i < 5; ++i) fe_sq(t, t);
    fe_mul(z2_10_0, t, z2_5_0);
    fe_sq(t, z2_10_0); for (int i = 1; i < 10; ++i) fe_sq(t, t);
    fe_mul(z2_20_0, t, z2_10_0);
    fe_sq(t, z2_20_0); for (int i = 1; i < 20; ++i) fe_sq(t, t);
    fe_mul(t, t, z2_20_0);
    fe_sq(t, t); for (int i = 1; i < 10; ++i) fe_sq(t, t);
    fe_mul(z2_50_0, t, z2_10_0);
    fe_sq(t, z2_50_0); for (int i = 1; i < 50; ++i) fe_sq(t, t);
    fe_mul(z2_100_0, t, z2_50_0);
    fe_sq(t, z2_100_0); for (int i = 1; i < 100; ++i) fe_sq(t, t);
    fe_mul(t, t, z2_100_0);
    fe_sq(t, t); for (int i = 1; i < 50; ++i) fe_sq(t, t);
    fe_mul(t, t, z2_50_0);
    fe_sq(t, t); for (int i = 1; i < 5; ++i) fe_sq(t, t);   /* 2^255 - 2^5 */
    fe_mul(o, t, z11);                                      /* 2^255 - 21 = p - 2 */
}

void mint_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32];
    for (int i = 0; i < 32; ++i) e[i] = scalar[i];
    e[0] &= 248; e[31] &= 127; e[31] |= 64;
    fe x1, x2 = {1, 0, 0, 0, 0}, z2 = {0, 0, 0, 0, 0}, x3, z3 = {1, 0, 0, 0, 0}, a, aa, b, bb, ee, c, d, da, cb, t;
    fe_from_bytes(x1, point);
    fe_copy(x3, x1);
    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; --pos) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1u;
        swap ^= bit;
        fe_cswap(x2, x3, swap); fe_cswap(z2, z3, swap);
        swap = bit;
        fe_add(a, x2, z2); fe_sq(aa, a);
        fe_sub(b, x2, z2); fe_sq(bb, b);
        fe_sub(ee, aa, bb);
        fe_add(c, x3, z3); fe_sub(d, x3, z3);
        fe_mul(da, d, a); fe_mul(cb, c, b);
        fe_add(t, da, cb); fe_sq(x3, t);
        fe_sub(t, da, cb); fe_sq(t, t); fe_mul(z3, x1, t);
        fe_mul(x2, aa, bb);
        fe_mul_small(t, ee, 121665); fe_add(t, aa, t); fe_mul(z2, ee, t);
    }
    fe_cswap(x2, x3, swap); fe_cswap(z2, z3, swap);
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_to_bytes(out, x2);
    volatile uint8_t *v = e; for (int i = 0; i < 32; ++i) v[i] = 0;
}

void mint_x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    static const uint8_t nine[32] = {9};
    mint_x25519(out, scalar, nine);
}
