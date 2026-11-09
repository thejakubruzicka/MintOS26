/* Host driver for the MintOS crypto library (kernel/user/crypto).
 * Usage (hex arguments, hex output on stdout):
 *   hash ALG DATA | hmac ALG KEY DATA | hkdf ALG SALT IKM INFO LEN
 *   label ALG SECRET LABEL CTX LEN | seal KIND KEY NONCE AAD PT | open KIND KEY NONCE AAD CT_TAG
 *   chacha KEY COUNTER NONCE | aes KIND KEY BLOCK | x25519 SCALAR POINT | x25519base SCALAR
 * ALG: 256/384/512. KIND: 1 AES-128-GCM, 2 AES-256-GCM, 3 ChaCha20-Poly1305. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/user/crypto/crypto.h"

void mint_random_bytes(void *out, size_t len) { arc4random_buf(out, len); }

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = strlen(s) / 2;
    if (!strcmp(s, "-")) return 0;
    if (n > cap) { fprintf(stderr, "argument too long\n"); exit(2); }
    for (size_t i = 0; i < n; ++i) { unsigned v; sscanf(s + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
    return n;
}
static void hex(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; ++i) printf("%02x", p[i]); printf("\n"); }
static uint32_t alg_of(const char *s) { return !strcmp(s, "256") ? MINT_SHA256 : !strcmp(s, "384") ? MINT_SHA384 : MINT_SHA512; }

static uint8_t A[1 << 20], B[1 << 20], C[1 << 16], D[1 << 16], OUT[1 << 20];

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *op = argv[1];
    if (!strcmp(op, "hash") && argc == 4) {
        size_t n = unhex(argv[3], A, sizeof(A));
        uint32_t alg = alg_of(argv[2]);
        /* also exercise incremental updates with odd chunk sizes */
        MINT_HASH h; mint_hash_init(&h, alg);
        for (size_t i = 0; i < n;) { size_t step = 1 + (i * 7) % 97; if (step > n - i) step = n - i; mint_hash_update(&h, A + i, step); i += step; }
        uint8_t d1[64], d2[64];
        mint_hash_final(&h, d1);
        mint_hash(alg, A, n, d2);
        if (memcmp(d1, d2, mint_hash_len(alg))) { printf("incremental-mismatch\n"); return 1; }
        hex(d1, mint_hash_len(alg));
    } else if (!strcmp(op, "hmac") && argc == 5) {
        size_t kn = unhex(argv[3], A, sizeof(A)), dn = unhex(argv[4], B, sizeof(B));
        uint32_t alg = alg_of(argv[2]);
        uint8_t d[64]; mint_hmac(alg, A, kn, B, dn, d); hex(d, mint_hash_len(alg));
    } else if (!strcmp(op, "hkdf") && argc == 7) {
        uint32_t alg = alg_of(argv[2]);
        size_t sn = unhex(argv[3], A, sizeof(A)), in = unhex(argv[4], B, sizeof(B)), fn = unhex(argv[5], C, sizeof(C));
        size_t len = (size_t)atoi(argv[6]);
        uint8_t prk[64]; mint_hkdf_extract(alg, sn ? A : NULL, sn, B, in, prk);
        mint_hkdf_expand(alg, prk, C, fn, OUT, len); hex(OUT, len);
    } else if (!strcmp(op, "label") && argc == 7) {
        uint32_t alg = alg_of(argv[2]);
        size_t sn = unhex(argv[3], A, sizeof(A)), cn = unhex(argv[5], C, sizeof(C)); (void)sn;
        size_t len = (size_t)atoi(argv[6]);
        mint_hkdf_expand_label(alg, A, argv[4], C, cn, OUT, len); hex(OUT, len);
    } else if ((!strcmp(op, "seal") || !strcmp(op, "open")) && argc == 7) {
        uint32_t kind = (uint32_t)atoi(argv[2]);
        uint8_t key[32], nonce[12];
        unhex(argv[3], key, sizeof(key)); unhex(argv[4], nonce, sizeof(nonce));
        size_t an = unhex(argv[5], C, sizeof(C)), pn = unhex(argv[6], A, sizeof(A));
        MINT_AEAD a; mint_aead_init(&a, kind, key);
        if (!strcmp(op, "seal")) {
            uint8_t tag[16];
            mint_aead_seal(&a, nonce, C, an, A, pn, OUT, tag);
            memcpy(OUT + pn, tag, 16); hex(OUT, pn + 16);
        } else {
            if (pn < 16) return 1;
            if (!mint_aead_open(&a, nonce, C, an, A, pn - 16, A + pn - 16, OUT)) { printf("auth-fail\n"); return 0; }
            hex(OUT, pn - 16);
        }
    } else if (!strcmp(op, "chacha") && argc == 5) {
        uint8_t key[32], nonce[12], out[64];
        unhex(argv[2], key, 32); unhex(argv[4], nonce, 12);
        mint_chacha20_block(key, (uint32_t)strtoul(argv[3], 0, 10), nonce, out); hex(out, 64);
    } else if (!strcmp(op, "aes") && argc == 5) {
        uint32_t kind = (uint32_t)atoi(argv[2]);
        uint8_t key[32], in[16], out[16];
        unhex(argv[3], key, 32); unhex(argv[4], in, 16);
        MINT_AEAD a; mint_aead_init(&a, kind, key); mint_aes_encrypt_block(&a, in, out); hex(out, 16);
    } else if (!strcmp(op, "x25519") && argc == 4) {
        uint8_t s[32], p[32], o[32];
        unhex(argv[2], s, 32); unhex(argv[3], p, 32); mint_x25519(o, s, p); hex(o, 32);
    } else if (!strcmp(op, "x25519base") && argc == 3) {
        uint8_t s[32], o[32];
        unhex(argv[2], s, 32); mint_x25519_base(o, s); hex(o, 32);
    } else return 2;
    (void)D;
    return 0;
}
