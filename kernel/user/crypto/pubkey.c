/* Public-key signature verification for TLS and X.509:
 *   RSA PKCS#1 v1.5 and RSA-PSS (RFC 8017) with SHA-256/384/512, moduli up
 *   to 4096 bits; ECDSA (FIPS 186-4) on NIST P-256 and P-384.
 * Only verification is implemented, so every input is public and the code
 * need not be constant time. Numbers are little-endian arrays of 32-bit limbs
 * with Montgomery multiplication (CIOS). */
#include "crypto.h"

#define BN_LIMBS 130u                     /* 4160 bits */
typedef struct { uint32_t v[BN_LIMBS]; } BN;
typedef struct { BN m, r2, one; uint32_t n, m0inv; } MONT;

static void bn_zero(BN *a) { for (uint32_t i = 0; i < BN_LIMBS; ++i) a->v[i] = 0; }
static int bn_from_bytes(BN *a, const uint8_t *p, size_t len, uint32_t limbs) {
    bn_zero(a);
    while (len && !*p) { ++p; --len; }
    if (len > (size_t)limbs * 4u) return 0;
    for (size_t i = 0; i < len; ++i) a->v[i / 4] |= (uint32_t)p[len - 1 - i] << (8 * (i % 4));
    return 1;
}
static void bn_to_bytes(const BN *a, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; ++i) out[len - 1 - i] = (uint8_t)(i / 4 < BN_LIMBS ? a->v[i / 4] >> (8 * (i % 4)) : 0);
}
static int bn_cmp(const BN *a, const BN *b, uint32_t n) {
    for (uint32_t i = n; i-- > 0;) { if (a->v[i] != b->v[i]) return a->v[i] > b->v[i] ? 1 : -1; }
    return 0;
}
static int bn_is_zero(const BN *a, uint32_t n) { for (uint32_t i = 0; i < n; ++i) if (a->v[i]) return 0; return 1; }
static uint32_t bn_sub(BN *o, const BN *a, const BN *b, uint32_t n) {
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t d = (uint64_t)a->v[i] - b->v[i] - borrow;
        o->v[i] = (uint32_t)d; borrow = (d >> 63) & 1u;
    }
    for (uint32_t i = n; i < BN_LIMBS; ++i) o->v[i] = 0;    /* results never carry stale high limbs */
    return (uint32_t)borrow;
}
static uint32_t bn_add(BN *o, const BN *a, const BN *b, uint32_t n) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < n; ++i) { uint64_t s = (uint64_t)a->v[i] + b->v[i] + carry; o->v[i] = (uint32_t)s; carry = s >> 32; }
    for (uint32_t i = n; i < BN_LIMBS; ++i) o->v[i] = 0;
    return (uint32_t)carry;
}
static uint32_t bn_bits(const BN *a, uint32_t n) {
    for (uint32_t i = n; i-- > 0;) if (a->v[i]) { uint32_t b = 32; while (!(a->v[i] >> (b - 1))) --b; return i * 32u + b; }
    return 0;
}
static int bn_bit(const BN *a, uint32_t i) { return (a->v[i / 32] >> (i % 32)) & 1u; }

/* --- modular arithmetic for an odd modulus --- */
static void mod_add(BN *o, const BN *a, const BN *b, const MONT *m) {
    uint32_t carry = bn_add(o, a, b, m->n);
    if (carry || bn_cmp(o, &m->m, m->n) >= 0) (void)bn_sub(o, o, &m->m, m->n);
}
static void mod_sub(BN *o, const BN *a, const BN *b, const MONT *m) {
    if (bn_sub(o, a, b, m->n)) (void)bn_add(o, o, &m->m, m->n);
}
static void mont_mul(BN *o, const BN *a, const BN *b, const MONT *m) {
    uint32_t n = m->n;
    static uint32_t t[BN_LIMBS + 2];
    for (uint32_t i = 0; i < n + 2; ++i) t[i] = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t c = 0;
        for (uint32_t j = 0; j < n; ++j) { uint64_t s = (uint64_t)a->v[j] * b->v[i] + t[j] + c; t[j] = (uint32_t)s; c = s >> 32; }
        uint64_t s = (uint64_t)t[n] + c; t[n] = (uint32_t)s; t[n + 1] = (uint32_t)(s >> 32);
        uint32_t q = t[0] * m->m0inv;
        c = ((uint64_t)q * m->m.v[0] + t[0]) >> 32;
        for (uint32_t j = 1; j < n; ++j) { s = (uint64_t)q * m->m.v[j] + t[j] + c; t[j - 1] = (uint32_t)s; c = s >> 32; }
        s = (uint64_t)t[n] + c; t[n - 1] = (uint32_t)s; t[n] = t[n + 1] + (uint32_t)(s >> 32);
    }
    static BN r; bn_zero(&r);
    for (uint32_t i = 0; i < n; ++i) r.v[i] = t[i];
    if (t[n] || bn_cmp(&r, &m->m, n) >= 0) (void)bn_sub(&r, &r, &m->m, n);
    *o = r;
}
static int mont_init(MONT *m, const BN *mod) {
    m->m = *mod;
    m->n = (bn_bits(mod, BN_LIMBS) + 31u) / 32u;
    if (!m->n || !(mod->v[0] & 1u) || m->n > BN_LIMBS - 2u) return 0;
    uint32_t inv = 1;                                   /* Newton iteration for m^-1 mod 2^32 */
    for (int i = 0; i < 5; ++i) inv *= 2u - mod->v[0] * inv;
    m->m0inv = (uint32_t)0 - inv;
    /* r2 = 2^(64n) mod m by repeated doubling of 1. */
    BN x; bn_zero(&x); x.v[0] = 1;
    for (uint32_t i = 0; i < 64u * m->n; ++i) {
        uint32_t carry = bn_add(&x, &x, &x, m->n);
        if (carry || bn_cmp(&x, &m->m, m->n) >= 0) (void)bn_sub(&x, &x, &m->m, m->n);
    }
    m->r2 = x;
    BN one; bn_zero(&one); one.v[0] = 1;
    mont_mul(&m->one, &one, &m->r2, m);                 /* R mod m */
    return 1;
}
static void to_mont(BN *o, const BN *a, const MONT *m) { mont_mul(o, a, &m->r2, m); }
static void from_mont(BN *o, const BN *a, const MONT *m) { BN one; bn_zero(&one); one.v[0] = 1; mont_mul(o, a, &one, m); }
/* o = a^e mod m with a in Montgomery form; result in Montgomery form. */
static void mont_pow(BN *o, const BN *a, const BN *e, const MONT *m) {
    BN r = m->one;
    for (uint32_t i = bn_bits(e, BN_LIMBS); i-- > 0;) {
        mont_mul(&r, &r, &r, m);
        if (bn_bit(e, i)) mont_mul(&r, &r, a, m);
    }
    *o = r;
}

/* ------------------------------------------------------------------ RSA */
static const uint8_t DI_SHA256[] = {0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
static const uint8_t DI_SHA384[] = {0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30};
static const uint8_t DI_SHA512[] = {0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40};

/* em = sig^e mod n, k bytes. */
static int rsa_public(const MINT_PUBKEY *key, const uint8_t *sig, size_t sig_len, uint8_t *em, size_t *k_out) {
    static BN n, e, s, x;
    static MONT m;
    if (!bn_from_bytes(&n, key->n, key->n_len, 128u) || !bn_from_bytes(&e, key->e, key->e_len, 4u)) return 0;
    uint32_t bits = bn_bits(&n, BN_LIMBS);
    if (bits < 1024u || bits > 4096u || !mont_init(&m, &n)) return 0;
    size_t k = (bits + 7u) / 8u;
    if (sig_len != k || !bn_from_bytes(&s, sig, sig_len, 128u) || bn_cmp(&s, &n, m.n) >= 0) return 0;
    to_mont(&x, &s, &m);
    mont_pow(&x, &x, &e, &m);
    from_mont(&x, &x, &m);
    bn_to_bytes(&x, em, k);
    *k_out = k;
    return 1;
}
static int rsa_pkcs1_verify(const MINT_PUBKEY *key, uint32_t alg, const uint8_t *msg, size_t msg_len, const uint8_t *sig, size_t sig_len) {
    static uint8_t em[512], expect[512];
    size_t k = 0;
    if (!rsa_public(key, sig, sig_len, em, &k)) return 0;
    const uint8_t *di = alg == MINT_SHA256 ? DI_SHA256 : alg == MINT_SHA384 ? DI_SHA384 : DI_SHA512;
    size_t dl = sizeof(DI_SHA256), hl = mint_hash_len(alg);
    if (k < dl + hl + 11u) return 0;
    expect[0] = 0; expect[1] = 1;
    size_t ps = k - 3u - dl - hl;
    for (size_t i = 0; i < ps; ++i) expect[2 + i] = 0xFF;
    expect[2 + ps] = 0;
    for (size_t i = 0; i < dl; ++i) expect[3 + ps + i] = di[i];
    mint_hash(alg, msg, msg_len, expect + 3 + ps + dl);
    return mint_ct_equal(em, expect, k);
}
static void mgf1(uint32_t alg, const uint8_t *seed, size_t seed_len, uint8_t *mask, size_t len) {
    size_t hl = mint_hash_len(alg), done = 0;
    uint32_t counter = 0;
    uint8_t d[MINT_HASH_MAX];
    while (done < len) {
        uint8_t c[4] = {(uint8_t)(counter >> 24), (uint8_t)(counter >> 16), (uint8_t)(counter >> 8), (uint8_t)counter};
        MINT_HASH h; mint_hash_init(&h, alg); mint_hash_update(&h, seed, seed_len); mint_hash_update(&h, c, 4); mint_hash_final(&h, d);
        for (size_t i = 0; i < hl && done < len; ++i) mask[done++] ^= d[i];
        ++counter;
    }
}
static int rsa_pss_verify(const MINT_PUBKEY *key, uint32_t alg, const uint8_t *msg, size_t msg_len, const uint8_t *sig, size_t sig_len) {
    static uint8_t em_full[512];
    size_t k = 0;
    if (!rsa_public(key, sig, sig_len, em_full, &k)) return 0;
    static BN n; bn_from_bytes(&n, key->n, key->n_len, 128u);
    uint32_t mod_bits = bn_bits(&n, BN_LIMBS), em_bits = mod_bits - 1u;
    size_t em_len = (em_bits + 7u) / 8u, hl = mint_hash_len(alg), salt_len = hl;
    const uint8_t *em = em_full + (k - em_len);
    if (k != em_len && em_full[0]) return 0;
    if (em_len < hl + salt_len + 2u || em[em_len - 1] != 0xBC) return 0;
    size_t db_len = em_len - hl - 1u;
    static uint8_t db[512];
    for (size_t i = 0; i < db_len; ++i) db[i] = em[i];
    const uint8_t *hash = em + db_len;
    uint32_t top = 8u * (uint32_t)em_len - em_bits;
    if (top && (db[0] >> (8u - top))) return 0;
    mgf1(alg, hash, hl, db, db_len);
    if (top) db[0] &= (uint8_t)(0xFFu >> top);
    size_t ps = db_len - salt_len - 1u;
    for (size_t i = 0; i < ps; ++i) if (db[i]) return 0;
    if (db[ps] != 1) return 0;
    uint8_t mhash[MINT_HASH_MAX], h2[MINT_HASH_MAX], zeros[8] = {0};
    mint_hash(alg, msg, msg_len, mhash);
    MINT_HASH h; mint_hash_init(&h, alg);
    mint_hash_update(&h, zeros, 8); mint_hash_update(&h, mhash, hl); mint_hash_update(&h, db + ps + 1, salt_len);
    mint_hash_final(&h, h2);
    return mint_ct_equal(h2, hash, hl);
}

/* ------------------------------------------------------------------ ECDSA */
typedef struct { uint32_t bytes; const char *p, *n, *b, *gx, *gy; } CURVE_HEX;
static const CURVE_HEX P256 = {32,
    "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
    "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
    "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"};
static const CURVE_HEX P384 = {48,
    "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
    "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f"};

static void bn_from_hex(BN *a, const char *h) {
    uint8_t b[64]; size_t n = 0;
    for (; h[2 * n] && n < sizeof(b); ++n) {
        uint8_t v = 0;
        for (int k = 0; k < 2; ++k) { char c = h[2 * n + (size_t)k]; v = (uint8_t)(v * 16u + (uint8_t)(c <= '9' ? c - '0' : c - 'a' + 10)); }
        b[n] = v;
    }
    bn_from_bytes(a, b, n, BN_LIMBS);
}

typedef struct { BN x, y, z; int inf; } JPOINT;        /* Jacobian, coordinates in Montgomery form */
typedef struct { MONT fp, fn; BN b, gx, gy; uint32_t bytes; } CURVE;

static void fmul(BN *o, const BN *a, const BN *b, const CURVE *c) { mont_mul(o, a, b, &c->fp); }
static void jdouble(JPOINT *r, const JPOINT *p, const CURVE *c) {
    if (p->inf || bn_is_zero(&p->y, c->fp.n)) { r->inf = 1; return; }
    static BN delta, gamma, beta, alpha, t1, t2, x3, y3, z3;
    fmul(&delta, &p->z, &p->z, c);
    fmul(&gamma, &p->y, &p->y, c);
    fmul(&beta, &p->x, &gamma, c);
    mod_sub(&t1, &p->x, &delta, &c->fp); mod_add(&t2, &p->x, &delta, &c->fp);
    fmul(&alpha, &t1, &t2, c);
    mod_add(&t1, &alpha, &alpha, &c->fp); mod_add(&alpha, &alpha, &t1, &c->fp);              /* 3(x-d)(x+d) */
    fmul(&x3, &alpha, &alpha, c);
    mod_add(&t1, &beta, &beta, &c->fp); mod_add(&t1, &t1, &t1, &c->fp); mod_add(&t2, &t1, &t1, &c->fp);   /* t1 = 4b, t2 = 8b */
    mod_sub(&x3, &x3, &t2, &c->fp);
    mod_add(&t2, &p->y, &p->z, &c->fp); fmul(&z3, &t2, &t2, c);
    mod_sub(&z3, &z3, &gamma, &c->fp); mod_sub(&z3, &z3, &delta, &c->fp);
    mod_sub(&t1, &t1, &x3, &c->fp); fmul(&y3, &alpha, &t1, c);
    fmul(&t2, &gamma, &gamma, c);
    mod_add(&t2, &t2, &t2, &c->fp); mod_add(&t2, &t2, &t2, &c->fp); mod_add(&t2, &t2, &t2, &c->fp);       /* 8 gamma^2 */
    mod_sub(&y3, &y3, &t2, &c->fp);
    r->x = x3; r->y = y3; r->z = z3; r->inf = 0;
}
static void jadd(JPOINT *r, const JPOINT *p, const JPOINT *q, const CURVE *c) {
    if (p->inf) { *r = *q; return; }
    if (q->inf) { *r = *p; return; }
    static BN z1z1, z2z2, u1, u2, s1, s2, h, rr, t, hh, hhh, v, x3, y3, z3;
    fmul(&z1z1, &p->z, &p->z, c); fmul(&z2z2, &q->z, &q->z, c);
    fmul(&u1, &p->x, &z2z2, c); fmul(&u2, &q->x, &z1z1, c);
    fmul(&t, &q->z, &z2z2, c); fmul(&s1, &p->y, &t, c);
    fmul(&t, &p->z, &z1z1, c); fmul(&s2, &q->y, &t, c);
    mod_sub(&h, &u2, &u1, &c->fp); mod_sub(&rr, &s2, &s1, &c->fp);
    if (bn_is_zero(&h, c->fp.n)) {
        if (bn_is_zero(&rr, c->fp.n)) { jdouble(r, p, c); return; }
        r->inf = 1; return;
    }
    fmul(&hh, &h, &h, c); fmul(&hhh, &h, &hh, c); fmul(&v, &u1, &hh, c);
    fmul(&x3, &rr, &rr, c); mod_sub(&x3, &x3, &hhh, &c->fp);
    mod_sub(&x3, &x3, &v, &c->fp); mod_sub(&x3, &x3, &v, &c->fp);
    mod_sub(&t, &v, &x3, &c->fp); fmul(&y3, &rr, &t, c);
    fmul(&t, &s1, &hhh, c); mod_sub(&y3, &y3, &t, &c->fp);
    fmul(&t, &p->z, &q->z, c); fmul(&z3, &t, &h, c);
    r->x = x3; r->y = y3; r->z = z3; r->inf = 0;
}
static int curve_load(CURVE *c, const CURVE_HEX *h) {
    BN p, n;
    bn_from_hex(&p, h->p); bn_from_hex(&n, h->n);
    if (!mont_init(&c->fp, &p) || !mont_init(&c->fn, &n)) return 0;
    BN t;
    bn_from_hex(&t, h->b); to_mont(&c->b, &t, &c->fp);
    bn_from_hex(&t, h->gx); to_mont(&c->gx, &t, &c->fp);
    bn_from_hex(&t, h->gy); to_mont(&c->gy, &t, &c->fp);
    c->bytes = h->bytes;
    return 1;
}
/* y^2 == x^3 - 3x + b, inputs in Montgomery form */
static int on_curve(const BN *x, const BN *y, const CURVE *c) {
    static BN l, r, t;
    fmul(&l, y, y, c);
    fmul(&t, x, x, c); fmul(&r, &t, x, c);
    mod_sub(&r, &r, x, &c->fp); mod_sub(&r, &r, x, &c->fp); mod_sub(&r, &r, x, &c->fp);
    mod_add(&r, &r, &c->b, &c->fp);
    return bn_cmp(&l, &r, c->fp.n) == 0;
}

static int ecdsa_verify(uint32_t key_type, const uint8_t *point, size_t point_len, uint32_t alg,
                        const uint8_t *msg, size_t msg_len, const uint8_t *sig, size_t sig_len) {
    static CURVE curves[2];
    static int loaded[2];
    int ci = key_type == MINT_KEY_EC_P384;
    CURVE *c = &curves[ci];
    if (!loaded[ci]) { if (!curve_load(c, ci ? &P384 : &P256)) return 0; loaded[ci] = 1; }
    size_t L = c->bytes;
    if (point_len != 1u + 2u * L || point[0] != 4) return 0;
    /* DER Ecdsa-Sig-Value: SEQUENCE { INTEGER r, INTEGER s } */
    if (sig_len < 8 || sig[0] != 0x30 || sig[1] & 0x80 || (size_t)sig[1] + 2u != sig_len) {
        if (!(sig_len > 3 && sig[0] == 0x30 && sig[1] == 0x81 && (size_t)sig[2] + 3u == sig_len)) return 0;
    }
    size_t at = sig[1] == 0x81 ? 3u : 2u;
    const uint8_t *ints[2]; size_t lens[2];
    for (int i = 0; i < 2; ++i) {
        if (at + 2u > sig_len || sig[at] != 0x02 || sig[at + 1] & 0x80) return 0;
        lens[i] = sig[at + 1]; ints[i] = sig + at + 2;
        if (!lens[i] || at + 2u + lens[i] > sig_len) return 0;
        at += 2u + lens[i];
    }
    if (at != sig_len) return 0;
    static BN r, s, e, qx, qy, t;
    if (!bn_from_bytes(&r, ints[0], lens[0], c->fn.n) || !bn_from_bytes(&s, ints[1], lens[1], c->fn.n)) return 0;
    if (bn_is_zero(&r, c->fn.n) || bn_is_zero(&s, c->fn.n) || bn_cmp(&r, &c->fn.m, c->fn.n) >= 0 || bn_cmp(&s, &c->fn.m, c->fn.n) >= 0) return 0;
    uint8_t digest[MINT_HASH_MAX];
    mint_hash(alg, msg, msg_len, digest);
    size_t hl = mint_hash_len(alg);
    bn_from_bytes(&e, digest, hl < L ? hl : L, c->fn.n);      /* leftmost bits (both orders are byte sized) */
    if (bn_cmp(&e, &c->fn.m, c->fn.n) >= 0) (void)bn_sub(&e, &e, &c->fn.m, c->fn.n);
    bn_from_bytes(&t, point + 1, L, c->fp.n); to_mont(&qx, &t, &c->fp);
    bn_from_bytes(&t, point + 1 + L, L, c->fp.n); to_mont(&qy, &t, &c->fp);
    if (!on_curve(&qx, &qy, c)) return 0;
    /* w = s^(n-2) mod n; u1 = e*w, u2 = r*w (Montgomery form mod n) */
    static BN sm, w, nm2, two, u1, u2, em, rm;
    to_mont(&sm, &s, &c->fn);
    bn_zero(&two); two.v[0] = 2;
    (void)bn_sub(&nm2, &c->fn.m, &two, c->fn.n);
    mont_pow(&w, &sm, &nm2, &c->fn);
    to_mont(&em, &e, &c->fn); to_mont(&rm, &r, &c->fn);
    mont_mul(&u1, &em, &w, &c->fn); from_mont(&u1, &u1, &c->fn);
    mont_mul(&u2, &rm, &w, &c->fn); from_mont(&u2, &u2, &c->fn);
    /* R = u1*G + u2*Q (Shamir's trick) */
    static JPOINT G, Q, GQ, R;
    G.x = c->gx; G.y = c->gy; G.z = c->fp.one; G.inf = 0;
    Q.x = qx; Q.y = qy; Q.z = c->fp.one; Q.inf = 0;
    R.inf = 1;
    jadd(&GQ, &G, &Q, c);
    uint32_t bits = bn_bits(&u1, c->fn.n), b2 = bn_bits(&u2, c->fn.n);
    if (b2 > bits) bits = b2;
    for (uint32_t i = bits; i-- > 0;) {
        jdouble(&R, &R, c);
        int a1 = bn_bit(&u1, i), a2 = bn_bit(&u2, i);
        if (a1 && a2) jadd(&R, &R, &GQ, c); else if (a1) jadd(&R, &R, &G, c); else if (a2) jadd(&R, &R, &Q, c);
    }
    if (R.inf) return 0;
    /* affine x = X / Z^2 */
    static BN zi, z2, x, pm2;
    bn_zero(&two); two.v[0] = 2;
    (void)bn_sub(&pm2, &c->fp.m, &two, c->fp.n);
    mont_pow(&zi, &R.z, &pm2, &c->fp);
    fmul(&z2, &zi, &zi, c); fmul(&x, &R.x, &z2, c);
    from_mont(&x, &x, &c->fp);
    while (bn_cmp(&x, &c->fn.m, c->fn.n) >= 0) (void)bn_sub(&x, &x, &c->fn.m, c->fn.n);
    return bn_cmp(&x, &r, c->fn.n) == 0;
}

/* ------------------------------------------------------------------ SPKI */
static int der_tlv(const uint8_t *p, size_t len, size_t at, uint8_t *tag, size_t *hdr, size_t *clen) {
    if (at + 2u > len) return 0;
    *tag = p[at];
    size_t l = p[at + 1], h = 2;
    if (l & 0x80u) {
        size_t n = l & 0x7Fu;
        if (!n || n > 3u || at + 2u + n > len) return 0;
        l = 0;
        for (size_t i = 0; i < n; ++i) l = (l << 8) | p[at + 2 + i];
        h += n;
    }
    if (l > len - at - h) return 0;
    *hdr = h; *clen = l;
    return 1;
}
static int oid_is(const uint8_t *p, size_t len, const uint8_t *oid, size_t oid_len) {
    if (len != oid_len) return 0;
    for (size_t i = 0; i < len; ++i) if (p[i] != oid[i]) return 0;
    return 1;
}
int mint_spki_parse(const uint8_t *spki, size_t len, MINT_PUBKEY *key) {
    static const uint8_t RSA_OID[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
    static const uint8_t EC_OID[] = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
    static const uint8_t P256_OID[] = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
    static const uint8_t P384_OID[] = {0x2b,0x81,0x04,0x00,0x22};
    uint8_t tag; size_t h, l;
    key->type = MINT_KEY_NONE;
    if (!der_tlv(spki, len, 0, &tag, &h, &l) || tag != 0x30) return 0;
    size_t at = h, end = h + l;
    size_t ah, al;
    if (!der_tlv(spki, end, at, &tag, &ah, &al) || tag != 0x30) return 0;          /* AlgorithmIdentifier */
    size_t alg_end = at + ah + al, oh, ol;
    if (!der_tlv(spki, alg_end, at + ah, &tag, &oh, &ol) || tag != 0x06) return 0;
    const uint8_t *oid = spki + at + ah + oh;
    size_t params = at + ah + oh + ol;
    at = alg_end;
    size_t bh, bl;
    if (!der_tlv(spki, end, at, &tag, &bh, &bl) || tag != 0x03 || bl < 2 || spki[at + bh] != 0) return 0;   /* BIT STRING */
    const uint8_t *bits = spki + at + bh + 1; size_t bits_len = bl - 1;
    if (oid_is(oid, ol, RSA_OID, sizeof(RSA_OID))) {
        size_t sh, sl, nh, nl, eh, el;
        if (!der_tlv(bits, bits_len, 0, &tag, &sh, &sl) || tag != 0x30) return 0;
        if (!der_tlv(bits, bits_len, sh, &tag, &nh, &nl) || tag != 0x02) return 0;
        size_t e_at = sh + nh + nl;
        if (!der_tlv(bits, bits_len, e_at, &tag, &eh, &el) || tag != 0x02) return 0;
        key->n = bits + sh + nh; key->n_len = nl;
        while (key->n_len && !key->n[0]) { ++key->n; --key->n_len; }
        key->e = bits + e_at + eh; key->e_len = el;
        while (key->e_len && !key->e[0]) { ++key->e; --key->e_len; }
        if (!key->n_len || !key->e_len || key->e_len > 4) return 0;
        key->type = MINT_KEY_RSA;
        return 1;
    }
    if (oid_is(oid, ol, EC_OID, sizeof(EC_OID))) {
        size_t ph, pl;
        if (!der_tlv(spki, alg_end, params, &tag, &ph, &pl) || tag != 0x06) return 0;
        const uint8_t *curve = spki + params + ph;
        if (oid_is(curve, pl, P256_OID, sizeof(P256_OID))) key->type = MINT_KEY_EC_P256;
        else if (oid_is(curve, pl, P384_OID, sizeof(P384_OID))) key->type = MINT_KEY_EC_P384;
        else return 0;
        key->point = bits; key->point_len = bits_len;
        return 1;
    }
    return 0;
}

int mint_signature_verify(const MINT_PUBKEY *key, uint32_t scheme, const uint8_t *msg, size_t msg_len,
                          const uint8_t *sig, size_t sig_len) {
    switch (scheme) {
    case MINT_SIG_RSA_PKCS1_SHA256: return key->type == MINT_KEY_RSA && rsa_pkcs1_verify(key, MINT_SHA256, msg, msg_len, sig, sig_len);
    case MINT_SIG_RSA_PKCS1_SHA384: return key->type == MINT_KEY_RSA && rsa_pkcs1_verify(key, MINT_SHA384, msg, msg_len, sig, sig_len);
    case MINT_SIG_RSA_PKCS1_SHA512: return key->type == MINT_KEY_RSA && rsa_pkcs1_verify(key, MINT_SHA512, msg, msg_len, sig, sig_len);
    case MINT_SIG_RSA_PSS_SHA256: return key->type == MINT_KEY_RSA && rsa_pss_verify(key, MINT_SHA256, msg, msg_len, sig, sig_len);
    case MINT_SIG_RSA_PSS_SHA384: return key->type == MINT_KEY_RSA && rsa_pss_verify(key, MINT_SHA384, msg, msg_len, sig, sig_len);
    case MINT_SIG_RSA_PSS_SHA512: return key->type == MINT_KEY_RSA && rsa_pss_verify(key, MINT_SHA512, msg, msg_len, sig, sig_len);
    case MINT_SIG_ECDSA_P256_SHA256: return key->type == MINT_KEY_EC_P256 && ecdsa_verify(key->type, key->point, key->point_len, MINT_SHA256, msg, msg_len, sig, sig_len);
    case MINT_SIG_ECDSA_P384_SHA384: return key->type == MINT_KEY_EC_P384 && ecdsa_verify(key->type, key->point, key->point_len, MINT_SHA384, msg, msg_len, sig, sig_len);
    case MINT_SIG_ECDSA_SHA256: case MINT_SIG_ECDSA_SHA384: case MINT_SIG_ECDSA_SHA512:
        if (key->type != MINT_KEY_EC_P256 && key->type != MINT_KEY_EC_P384) return 0;
        return ecdsa_verify(key->type, key->point, key->point_len,
                            scheme == MINT_SIG_ECDSA_SHA256 ? MINT_SHA256 : scheme == MINT_SIG_ECDSA_SHA384 ? MINT_SHA384 : MINT_SHA512,
                            msg, msg_len, sig, sig_len);
    default: return 0;
    }
}
