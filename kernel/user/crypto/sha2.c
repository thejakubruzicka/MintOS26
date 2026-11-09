/* SHA-256, SHA-384, SHA-512 (FIPS 180-4), HMAC (RFC 2104) and HKDF (RFC 5869)
 * with the TLS 1.3 HKDF-Expand-Label helper. */
#include "crypto.h"

static void bzero_(void *p, size_t n) { volatile uint8_t *b = (volatile uint8_t *)p; while (n--) *b++ = 0; }
static void bcopy_(void *d, const void *s, size_t n) { uint8_t *o = (uint8_t *)d; const uint8_t *i = (const uint8_t *)s; while (n--) *o++ = *i++; }

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,0xd807aa98a3030242ULL,0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL,0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL,0x142929670a0e6e70ULL,0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL,0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL,0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL,0x1b710b35131c471bULL,0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void block256(uint32_t *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

static void block512(uint64_t *s, const uint8_t *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        uint64_t v = 0;
        for (int j = 0; j < 8; ++j) v = (v << 8) | p[8 * i + j];
        w[i] = v;
    }
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t t1 = h + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

size_t mint_hash_len(uint32_t alg) { return alg == MINT_SHA256 ? 32u : alg == MINT_SHA384 ? 48u : alg == MINT_SHA512 ? 64u : 0u; }
static size_t block_len(uint32_t alg) { return alg == MINT_SHA256 ? 64u : 128u; }

void mint_hash_init(MINT_HASH *h, uint32_t alg) {
    static const uint32_t i256[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    static const uint64_t i384[8] = {0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
                                     0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL};
    static const uint64_t i512[8] = {0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                                     0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    bzero_(h, sizeof(*h));
    h->alg = alg;
    for (int i = 0; i < 8; ++i) {
        h->h32[i] = i256[i];
        h->h64[i] = alg == MINT_SHA384 ? i384[i] : i512[i];
    }
}

void mint_hash_update(MINT_HASH *h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t bl = block_len(h->alg);
    h->total += len;
    while (len) {
        size_t take = bl - h->used;
        if (take > len) take = len;
        bcopy_(h->buf + h->used, p, take);
        h->used += (uint32_t)take; p += take; len -= take;
        if (h->used == bl) {
            if (h->alg == MINT_SHA256) block256(h->h32, h->buf); else block512(h->h64, h->buf);
            h->used = 0;
        }
    }
}

void mint_hash_final(MINT_HASH *src, uint8_t *out) {
    MINT_HASH h = *src;
    size_t bl = block_len(h.alg), lenbytes = h.alg == MINT_SHA256 ? 8u : 16u;
    uint64_t bits = h.total * 8u;
    uint8_t pad[144];
    size_t padlen = bl - (h.used % bl);
    if (padlen < lenbytes + 1u) padlen += bl;
    bzero_(pad, sizeof(pad));
    pad[0] = 0x80;
    for (int i = 0; i < 8; ++i) pad[padlen - 1u - (size_t)i] = (uint8_t)(bits >> (8 * i));
    h.total -= 0;                      /* total no longer used */
    mint_hash_update(&h, pad, padlen);
    if (h.alg == MINT_SHA256) {
        for (int i = 0; i < 8; ++i) { out[4*i] = (uint8_t)(h.h32[i] >> 24); out[4*i+1] = (uint8_t)(h.h32[i] >> 16); out[4*i+2] = (uint8_t)(h.h32[i] >> 8); out[4*i+3] = (uint8_t)h.h32[i]; }
    } else {
        size_t words = mint_hash_len(h.alg) / 8u;
        for (size_t i = 0; i < words; ++i) for (int j = 0; j < 8; ++j) out[8*i + (size_t)j] = (uint8_t)(h.h64[i] >> (56 - 8 * j));
    }
    bzero_(&h, sizeof(h));
}

void mint_hash(uint32_t alg, const void *data, size_t len, uint8_t *out) {
    MINT_HASH h;
    mint_hash_init(&h, alg); mint_hash_update(&h, data, len); mint_hash_final(&h, out);
    bzero_(&h, sizeof(h));
}

void mint_hmac_init(MINT_HMAC *m, uint32_t alg, const uint8_t *key, size_t key_len) {
    uint8_t k[128], pad[128];
    size_t bl = block_len(alg);
    bzero_(k, sizeof(k));
    if (key_len > bl) mint_hash(alg, key, key_len, k); else bcopy_(k, key, key_len);
    for (size_t i = 0; i < bl; ++i) pad[i] = (uint8_t)(k[i] ^ 0x36u);
    mint_hash_init(&m->inner, alg); mint_hash_update(&m->inner, pad, bl);
    for (size_t i = 0; i < bl; ++i) pad[i] = (uint8_t)(k[i] ^ 0x5Cu);
    mint_hash_init(&m->outer, alg); mint_hash_update(&m->outer, pad, bl);
    bzero_(k, sizeof(k)); bzero_(pad, sizeof(pad));
}
void mint_hmac_update(MINT_HMAC *m, const void *data, size_t len) { mint_hash_update(&m->inner, data, len); }
void mint_hmac_final(MINT_HMAC *m, uint8_t *out) {
    uint8_t ih[MINT_HASH_MAX];
    mint_hash_final(&m->inner, ih);
    MINT_HASH o = m->outer;
    mint_hash_update(&o, ih, mint_hash_len(o.alg));
    mint_hash_final(&o, out);
    bzero_(ih, sizeof(ih)); bzero_(&o, sizeof(o));
}
void mint_hmac(uint32_t alg, const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t *out) {
    MINT_HMAC m;
    mint_hmac_init(&m, alg, key, key_len); mint_hmac_update(&m, data, len); mint_hmac_final(&m, out);
    bzero_(&m, sizeof(m));
}

void mint_hkdf_extract(uint32_t alg, const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t *prk) {
    uint8_t zero[MINT_HASH_MAX];
    bzero_(zero, sizeof(zero));
    if (!salt || !salt_len) { salt = zero; salt_len = mint_hash_len(alg); }
    mint_hmac(alg, salt, salt_len, ikm, ikm_len, prk);
}

void mint_hkdf_expand(uint32_t alg, const uint8_t *prk, const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len) {
    size_t hl = mint_hash_len(alg), done = 0;
    uint8_t t[MINT_HASH_MAX];
    uint8_t counter = 1;
    while (done < out_len) {
        MINT_HMAC m;
        mint_hmac_init(&m, alg, prk, hl);
        if (counter > 1) mint_hmac_update(&m, t, hl);
        mint_hmac_update(&m, info, info_len);
        mint_hmac_update(&m, &counter, 1);
        mint_hmac_final(&m, t);
        size_t take = out_len - done < hl ? out_len - done : hl;
        bcopy_(out + done, t, take);
        done += take; ++counter;
    }
    bzero_(t, sizeof(t));
}

void mint_hkdf_expand_label(uint32_t alg, const uint8_t *secret, const char *label,
                            const uint8_t *context, size_t context_len, uint8_t *out, size_t out_len) {
    uint8_t info[2 + 1 + 6 + 64 + 1 + 64];
    size_t label_len = 0, n = 0;
    while (label[label_len] && label_len < 64u) ++label_len;
    if (context_len > 64u) context_len = 64u;
    info[n++] = (uint8_t)(out_len >> 8); info[n++] = (uint8_t)out_len;
    info[n++] = (uint8_t)(6u + label_len);
    bcopy_(info + n, "tls13 ", 6); n += 6;
    bcopy_(info + n, label, label_len); n += label_len;
    info[n++] = (uint8_t)context_len;
    if (context_len) bcopy_(info + n, context, context_len);
    n += context_len;
    mint_hkdf_expand(alg, secret, info, n, out, out_len);
}

int mint_ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
