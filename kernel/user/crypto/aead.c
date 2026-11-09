/* AEAD ciphers for TLS 1.3: ChaCha20-Poly1305 (RFC 8439) and AES-128/256-GCM
 * (FIPS 197, NIST SP 800-38D). Integer-only; ChaCha20 and Poly1305 are
 * constant time. AES uses the byte-oriented S-box (table lookups indexed by
 * secret data), acceptable for a client that does not share its CPU with an
 * attacker; ChaCha20-Poly1305 is offered first. */
#include "crypto.h"

static void bzero_(void *p, size_t n) { volatile uint8_t *b = (volatile uint8_t *)p; while (n--) *b++ = 0; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint64_t be64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v; }
static void put_be64(uint8_t *p, uint64_t v) { for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)v; v >>= 8; } }

/* ------------------------------------------------------------------ ChaCha20 */
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) a += b; d ^= a; d = ROTL(d, 16); c += d; b ^= c; b = ROTL(b, 12); \
                       a += b; d ^= a; d = ROTL(d, 8);  c += d; b ^= c; b = ROTL(b, 7)

void mint_chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu; s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; ++i) s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; ++i) s[13 + i] = le32(nonce + 4 * i);
    for (int i = 0; i < 16; ++i) x[i] = s[i];
    for (int i = 0; i < 10; ++i) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) put_le32(out + 4 * i, x[i] + s[i]);
    bzero_(x, sizeof(x)); bzero_(s, sizeof(s));
}

static void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t ks[64];
    while (len) {
        mint_chacha20_block(key, counter++, nonce, ks);
        size_t n = len < 64u ? len : 64u;
        for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(in[i] ^ ks[i]);
        in += n; out += n; len -= n;
    }
    bzero_(ks, sizeof(ks));
}

/* ------------------------------------------------------------------ Poly1305 (26-bit limbs) */
typedef struct { uint32_t r[5], h[5], pad[4]; uint8_t buf[16]; size_t used; } POLY;

static void poly_init(POLY *p, const uint8_t key[32]) {
    p->r[0] = (le32(key + 0)) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; ++i) p->h[i] = 0;
    for (int i = 0; i < 4; ++i) p->pad[i] = le32(key + 16 + 4 * i);
    p->used = 0;
}
static void poly_blocks(POLY *p, const uint8_t *m, size_t bytes, uint32_t hibit) {
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (bytes >= 16) {
        h0 += (le32(m + 0)) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += 16; bytes -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}
static void poly_update(POLY *p, const uint8_t *m, size_t len) {
    if (p->used) {
        size_t take = 16 - p->used < len ? 16 - p->used : len;
        for (size_t i = 0; i < take; ++i) p->buf[p->used + i] = m[i];
        p->used += take; m += take; len -= take;
        if (p->used < 16) return;
        poly_blocks(p, p->buf, 16, 1u << 24);
        p->used = 0;
    }
    size_t full = len & ~(size_t)15;
    if (full) { poly_blocks(p, m, full, 1u << 24); m += full; len -= full; }
    for (size_t i = 0; i < len; ++i) p->buf[i] = m[i];
    p->used = len;
}
static void poly_pad16(POLY *p) {          /* AEAD construction pads each part with zeros */
    if (p->used) { for (size_t i = p->used; i < 16; ++i) p->buf[i] = 0; poly_blocks(p, p->buf, 16, 1u << 24); p->used = 0; }
}
static void poly_final(POLY *p, uint8_t mac[16]) {
    if (p->used) {
        p->buf[p->used] = 1;
        for (size_t i = p->used + 1; i < 16; ++i) p->buf[i] = 0;
        poly_blocks(p, p->buf, 16, 0);
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff; h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1u;              /* all ones if h >= p */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = (h0 | (h1 << 26)); h1 = ((h1 >> 6) | (h2 << 20)); h2 = ((h2 >> 12) | (h3 << 14)); h3 = ((h3 >> 18) | (h4 << 8));
    uint64_t f;
    f = (uint64_t)h0 + p->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + p->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + p->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + p->pad[3] + (f >> 32); h3 = (uint32_t)f;
    put_le32(mac, h0); put_le32(mac + 4, h1); put_le32(mac + 8, h2); put_le32(mac + 12, h3);
    bzero_(p, sizeof(*p));
}

static void chacha_poly_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                            const uint8_t *ct, size_t len, uint8_t tag[16]) {
    uint8_t block[64];
    mint_chacha20_block(key, 0, nonce, block);
    POLY p;
    poly_init(&p, block);
    poly_update(&p, aad, aad_len); poly_pad16(&p);
    poly_update(&p, ct, len); poly_pad16(&p);
    uint8_t lens[16];
    for (int i = 0; i < 8; ++i) { lens[i] = (uint8_t)((uint64_t)aad_len >> (8 * i)); lens[8 + i] = (uint8_t)((uint64_t)len >> (8 * i)); }
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
    bzero_(block, sizeof(block));
}

/* ------------------------------------------------------------------ AES */
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,
    0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,
    0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,
    0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,
    0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,
    0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static uint32_t sub_word(uint32_t w) {
    return ((uint32_t)SBOX[w >> 24] << 24) | ((uint32_t)SBOX[(w >> 16) & 0xFF] << 16) | ((uint32_t)SBOX[(w >> 8) & 0xFF] << 8) | SBOX[w & 0xFF];
}
static void aes_expand(MINT_AEAD *a, const uint8_t *key, uint32_t key_words) {
    uint32_t total = 4u * (key_words + 7u);
    a->rounds = key_words + 6u;
    for (uint32_t i = 0; i < key_words; ++i)
        a->rk[i] = ((uint32_t)key[4*i] << 24) | ((uint32_t)key[4*i+1] << 16) | ((uint32_t)key[4*i+2] << 8) | key[4*i+3];
    uint8_t rcon = 1;
    for (uint32_t i = key_words; i < total; ++i) {
        uint32_t t = a->rk[i - 1];
        if (i % key_words == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t)rcon << 24);
            rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
        } else if (key_words > 6u && i % key_words == 4u) t = sub_word(t);
        a->rk[i] = a->rk[i - key_words] ^ t;
    }
}
static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

void mint_aes_encrypt_block(const MINT_AEAD *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) s[4 * c + r] = (uint8_t)(in[4 * c + r] ^ (a->rk[c] >> (24 - 8 * r)));
    for (uint32_t round = 1; round <= a->rounds; ++round) {
        uint8_t t[16];
        for (int i = 0; i < 16; ++i) s[i] = SBOX[s[i]];
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) t[4 * c + r] = s[4 * ((c + r) & 3) + r];   /* ShiftRows */
        if (round != a->rounds) {
            for (int c = 0; c < 4; ++c) {                                                                  /* MixColumns */
                uint8_t *col = t + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
            s[4 * c + r] = (uint8_t)(t[4 * c + r] ^ (a->rk[4 * round + (uint32_t)c] >> (24 - 8 * r)));
    }
    for (int i = 0; i < 16; ++i) out[i] = s[i];
    bzero_(s, sizeof(s));
}

/* GHASH: multiplication in GF(2^128) with the GCM bit order (SP 800-38D 6.3). */
static void gf_mult(uint64_t *xh, uint64_t *xl, uint64_t hh, uint64_t hl) {
    uint64_t zh = 0, zl = 0, vh = hh, vl = hl;
    for (int i = 0; i < 128; ++i) {
        uint64_t bit = i < 64 ? (*xh >> (63 - i)) & 1u : (*xl >> (127 - i)) & 1u;
        uint64_t m = (uint64_t)0 - bit;
        zh ^= vh & m; zl ^= vl & m;
        uint64_t lsb = vl & 1u;
        vl = (vl >> 1) | (vh << 63);
        vh = (vh >> 1) ^ (0xE100000000000000ULL & ((uint64_t)0 - lsb));
    }
    *xh = zh; *xl = zl;
}
static void ghash_update(const MINT_AEAD *a, uint64_t *yh, uint64_t *yl, const uint8_t *data, size_t len) {
    while (len) {
        uint8_t block[16] = {0};
        size_t n = len < 16u ? len : 16u;
        for (size_t i = 0; i < n; ++i) block[i] = data[i];
        *yh ^= be64(block); *yl ^= be64(block + 8);
        gf_mult(yh, yl, a->h_hi, a->h_lo);
        data += n; len -= n;
    }
}
static void gcm_ctr(const MINT_AEAD *a, const uint8_t nonce[12], uint32_t counter, const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t cb[16], ks[16];
    for (int i = 0; i < 12; ++i) cb[i] = nonce[i];
    while (len) {
        cb[12] = (uint8_t)(counter >> 24); cb[13] = (uint8_t)(counter >> 16); cb[14] = (uint8_t)(counter >> 8); cb[15] = (uint8_t)counter;
        mint_aes_encrypt_block(a, cb, ks);
        size_t n = len < 16u ? len : 16u;
        for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(in[i] ^ ks[i]);
        in += n; out += n; len -= n; ++counter;
    }
    bzero_(ks, sizeof(ks));
}
static void gcm_tag(const MINT_AEAD *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, size_t len, uint8_t tag[16]) {
    uint64_t yh = 0, yl = 0;
    ghash_update(a, &yh, &yl, aad, aad_len);
    ghash_update(a, &yh, &yl, ct, len);
    yh ^= (uint64_t)aad_len * 8u; yl ^= (uint64_t)len * 8u;
    gf_mult(&yh, &yl, a->h_hi, a->h_lo);
    uint8_t s[16], j0[16];
    put_be64(s, yh); put_be64(s + 8, yl);
    for (int i = 0; i < 12; ++i) j0[i] = nonce[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    uint8_t ek[16];
    mint_aes_encrypt_block(a, j0, ek);
    for (int i = 0; i < 16; ++i) tag[i] = (uint8_t)(s[i] ^ ek[i]);
}

/* ------------------------------------------------------------------ AEAD interface */
size_t mint_aead_key_len(uint32_t kind) { return kind == MINT_AEAD_AES128_GCM ? 16u : 32u; }

void mint_aead_init(MINT_AEAD *a, uint32_t kind, const uint8_t *key) {
    bzero_(a, sizeof(*a));
    a->kind = kind;
    if (kind == MINT_AEAD_CHACHA20_POLY1305) { for (int i = 0; i < 32; ++i) a->key[i] = key[i]; return; }
    aes_expand(a, key, kind == MINT_AEAD_AES128_GCM ? 4u : 8u);
    uint8_t zero[16] = {0}, h[16];
    mint_aes_encrypt_block(a, zero, h);
    a->h_hi = be64(h); a->h_lo = be64(h + 8);
}

void mint_aead_seal(const MINT_AEAD *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]) {
    if (a->kind == MINT_AEAD_CHACHA20_POLY1305) {
        chacha20_xor(a->key, 1, nonce, in, out, len);
        chacha_poly_tag(a->key, nonce, aad, aad_len, out, len, tag);
    } else {
        gcm_ctr(a, nonce, 2, in, out, len);
        gcm_tag(a, nonce, aad, aad_len, out, len, tag);
    }
}

int mint_aead_open(const MINT_AEAD *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                   const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out) {
    uint8_t expect[16];
    if (a->kind == MINT_AEAD_CHACHA20_POLY1305) chacha_poly_tag(a->key, nonce, aad, aad_len, in, len, expect);
    else gcm_tag(a, nonce, aad, aad_len, in, len, expect);
    if (!mint_ct_equal(expect, tag, 16)) return 0;
    if (a->kind == MINT_AEAD_CHACHA20_POLY1305) chacha20_xor(a->key, 1, nonce, in, out, len);
    else gcm_ctr(a, nonce, 2, in, out, len);
    return 1;
}
