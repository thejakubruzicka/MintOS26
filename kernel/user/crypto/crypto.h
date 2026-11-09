#ifndef MINT_CRYPTO_H
#define MINT_CRYPTO_H
/* MintOS user-space cryptography for TLS 1.3: SHA-2, HMAC/HKDF, ChaCha20-
 * Poly1305, AES-GCM, X25519, big-number RSA and NIST P-256/P-384 ECDSA
 * verification. Integer-only (the MintOS ring-3 ABI forbids SSE), no heap,
 * portable C so the same files are tested on the build host. */
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ hashes */
enum { MINT_SHA256 = 1, MINT_SHA384 = 2, MINT_SHA512 = 3 };
#define MINT_HASH_MAX 64u
typedef struct mint_hash {
    uint32_t alg, used;
    uint64_t total;
    uint32_t h32[8];
    uint64_t h64[8];
    uint8_t buf[128];
} MINT_HASH;
size_t mint_hash_len(uint32_t alg);
void mint_hash_init(MINT_HASH *h, uint32_t alg);
void mint_hash_update(MINT_HASH *h, const void *data, size_t len);
void mint_hash_final(MINT_HASH *h, uint8_t *out);           /* does not modify h */
void mint_hash(uint32_t alg, const void *data, size_t len, uint8_t *out);

typedef struct mint_hmac { MINT_HASH inner, outer; } MINT_HMAC;
void mint_hmac_init(MINT_HMAC *m, uint32_t alg, const uint8_t *key, size_t key_len);
void mint_hmac_update(MINT_HMAC *m, const void *data, size_t len);
void mint_hmac_final(MINT_HMAC *m, uint8_t *out);
void mint_hmac(uint32_t alg, const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t *out);
void mint_hkdf_extract(uint32_t alg, const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t *prk);
void mint_hkdf_expand(uint32_t alg, const uint8_t *prk, const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len);
/* TLS 1.3 HKDF-Expand-Label (RFC 8446 7.1); label without the "tls13 " prefix. */
void mint_hkdf_expand_label(uint32_t alg, const uint8_t *secret, const char *label,
                            const uint8_t *context, size_t context_len, uint8_t *out, size_t out_len);
int mint_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);   /* constant time, 1 if equal */

/* ------------------------------------------------------------------ AEADs */
enum { MINT_AEAD_AES128_GCM = 1, MINT_AEAD_AES256_GCM = 2, MINT_AEAD_CHACHA20_POLY1305 = 3 };
typedef struct mint_aead {
    uint32_t kind;
    uint32_t rk[60];          /* AES round keys */
    uint32_t rounds;
    uint64_t h_hi, h_lo;      /* GHASH key */
    uint8_t key[32];          /* ChaCha20 key */
} MINT_AEAD;
size_t mint_aead_key_len(uint32_t kind);
void mint_aead_init(MINT_AEAD *a, uint32_t kind, const uint8_t *key);
/* 12-byte nonce, 16-byte tag appended to / checked at the end of the data. */
void mint_aead_seal(const MINT_AEAD *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]);
int mint_aead_open(const MINT_AEAD *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                   const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out);

/* Raw primitives exposed for tests. */
void mint_chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);
void mint_aes_encrypt_block(const MINT_AEAD *a, const uint8_t in[16], uint8_t out[16]);

/* ------------------------------------------------------------------ X25519 */
void mint_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void mint_x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* ------------------------------------------------------------------ randomness */
/* Fills out with unpredictable bytes (RDSEED/RDRAND mixed with TSC jitter
 * through SHA-512 on MintOS; the host test build supplies its own). */
void mint_random_bytes(void *out, size_t len);

/* ------------------------------------------------------------------ public keys */
/* Signature schemes (TLS SignatureScheme code points). */
enum {
    MINT_SIG_RSA_PKCS1_SHA256 = 0x0401, MINT_SIG_RSA_PKCS1_SHA384 = 0x0501, MINT_SIG_RSA_PKCS1_SHA512 = 0x0601,
    MINT_SIG_ECDSA_P256_SHA256 = 0x0403, MINT_SIG_ECDSA_P384_SHA384 = 0x0503,
    MINT_SIG_RSA_PSS_SHA256 = 0x0804, MINT_SIG_RSA_PSS_SHA384 = 0x0805, MINT_SIG_RSA_PSS_SHA512 = 0x0806,
    /* certificate signatures where the curve comes from the key, not the scheme */
    MINT_SIG_ECDSA_SHA256 = 0x7F01, MINT_SIG_ECDSA_SHA384 = 0x7F02, MINT_SIG_ECDSA_SHA512 = 0x7F03
};
enum { MINT_KEY_NONE = 0, MINT_KEY_RSA = 1, MINT_KEY_EC_P256 = 2, MINT_KEY_EC_P384 = 3 };
typedef struct mint_pubkey {
    uint32_t type;
    const uint8_t *n; size_t n_len;        /* RSA modulus (big-endian, no leading zero) */
    const uint8_t *e; size_t e_len;
    const uint8_t *point; size_t point_len; /* EC: uncompressed 0x04 || X || Y */
} MINT_PUBKEY;
/* Parses a DER SubjectPublicKeyInfo; pointers refer into spki. */
int mint_spki_parse(const uint8_t *spki, size_t len, MINT_PUBKEY *key);
/* Verifies sig over msg (hashed internally) with scheme; 1 = valid. */
int mint_signature_verify(const MINT_PUBKEY *key, uint32_t scheme, const uint8_t *msg, size_t msg_len,
                          const uint8_t *sig, size_t sig_len);
#endif
