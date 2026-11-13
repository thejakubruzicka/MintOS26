#ifndef MINT_TLS_H
#define MINT_TLS_H
/* TLS client.
 * TLS 1.3 (RFC 8446): X25519, or P-256 after a HelloRetryRequest;
 * TLS_CHACHA20_POLY1305_SHA256, TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384;
 * CertificateVerify with ECDSA P-256/P-384 or RSA-PSS.
 * TLS 1.2 (RFC 5246) for servers without 1.3: ECDHE (X25519 or P-256) with
 * ECDSA or RSA signatures and AEAD suites only (AES-GCM RFC 5288,
 * ChaCha20-Poly1305 RFC 7905), extended master secret (RFC 7627), secure
 * renegotiation indication, the RFC 8446 downgrade sentinel checked.
 * Both: SNI, ALPN http/1.1, certificate chain to the Mozilla roots with no
 * bypass. Non-blocking: the caller supplies send/recv callbacks that never
 * wait and drives the handshake with mint_tls_step(). No session resumption,
 * 0-RTT, renegotiation, client certificates, CBC/RSA key exchange or SHA-1. */
#include <stdint.h>
#include <stddef.h>
#include "crypto.h"
#include "x509.h"

/* recv: >0 bytes, 0 nothing available yet, <0 connection closed or failed.
 * send: >=0 bytes accepted (0 when the transport is full), <0 failed. */
typedef struct mint_tls_io {
    void *ctx;
    int64_t (*send)(void *ctx, const void *data, size_t len);
    int64_t (*recv)(void *ctx, void *out, size_t cap);
} MINT_TLS_IO;

enum {
    MINT_TLS_DONE = 0,          /* handshake complete */
    MINT_TLS_AGAIN = 1,         /* call again later */
    MINT_TLS_WOULD_BLOCK = -100,
    MINT_TLS_ERR_IO = -1, MINT_TLS_ERR_PROTOCOL = -2, MINT_TLS_ERR_CERT = -3, MINT_TLS_ERR_ALERT = -4,
    MINT_TLS_ERR_UNSUPPORTED = -5, MINT_TLS_ERR_DECRYPT = -6
};

#define MINT_TLS_RECORD_MAX (16384u + 256u + 5u)
#define MINT_TLS_HS_MAX (64u * 1024u)

typedef struct mint_tls_dir { MINT_AEAD aead; uint8_t iv[12]; uint64_t seq; uint8_t secret[MINT_HASH_MAX]; int on, explicit_nonce; } MINT_TLS_DIR;

typedef struct mint_tls {
    MINT_TLS_IO io;
    char host[256];
    int64_t now;                    /* Unix seconds for certificate dates, 0 = skip */
    const MINT_TLS_ROOT *roots; uint32_t root_count;
    uint16_t suites[9]; uint32_t suite_count;      /* offered, in preference order */
    uint16_t version;                              /* 0x0304 or 0x0303 once negotiated */
    uint16_t group;                                /* key-exchange group used */
    uint8_t client_random[32], server_random[32];
    uint8_t p256_priv[32], p256_pub[65];
    int hrr_done, transcript_started, ems, key_is_ec;
    uint8_t cookie[256]; size_t cookie_len;
    uint8_t master12[48];
    uint8_t premaster[32];
    int state, error, cert_error, alert;
    uint16_t suite; uint32_t hash_alg; size_t hash_len;
    uint8_t priv[32], pub[32], session_id[32];
    uint8_t client_hello[1024]; size_t client_hello_len;
    MINT_HASH transcript;
    uint8_t hs_secret[MINT_HASH_MAX], master[MINT_HASH_MAX];
    uint8_t c_hs[MINT_HASH_MAX], s_hs[MINT_HASH_MAX];
    MINT_TLS_DIR rd, wr;
    int cert_requested, got_cert, got_cert_verify;
    uint8_t leaf_spki[1200]; size_t leaf_spki_len;
    uint8_t rbuf[MINT_TLS_RECORD_MAX]; size_t rlen;
    uint8_t hs[MINT_TLS_HS_MAX]; size_t hs_len;
    uint8_t plain[16384u + 256u]; size_t plain_off, plain_len;
    uint8_t wbuf[2u * MINT_TLS_RECORD_MAX]; size_t wlen;
    int peer_closed;
    char alpn[16];
} MINT_TLS;

/* host must outlive the call only during init (it is copied). */
void mint_tls_init(MINT_TLS *t, const MINT_TLS_IO *io, const char *host, int64_t now);
/* Advances the handshake: MINT_TLS_DONE, MINT_TLS_AGAIN or a negative error. */
int mint_tls_step(MINT_TLS *t);
/* Application data (after DONE): >0 bytes; 0 = peer closed; MINT_TLS_WOULD_BLOCK; <0 error. */
int64_t mint_tls_read(MINT_TLS *t, void *out, size_t cap);
/* Queues and sends; returns bytes accepted (may be less than len), or <0. */
int64_t mint_tls_write(MINT_TLS *t, const void *data, size_t len);
/* Flushes queued output; MINT_TLS_DONE when empty, MINT_TLS_AGAIN otherwise. */
int mint_tls_flush(MINT_TLS *t);
void mint_tls_close(MINT_TLS *t);
const char *mint_tls_error_text(const MINT_TLS *t);
const char *mint_tls_suite_name(uint16_t suite);
const char *mint_tls_version_name(const MINT_TLS *t);
#endif
