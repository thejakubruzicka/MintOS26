#ifndef MINT_TLS_H
#define MINT_TLS_H
/* TLS 1.3 client (RFC 8446): X25519 key exchange; TLS_CHACHA20_POLY1305_SHA256,
 * TLS_AES_128_GCM_SHA256 and TLS_AES_256_GCM_SHA384; server authentication by
 * certificate chain to the Mozilla roots plus CertificateVerify (ECDSA P-256/
 * P-384, RSA-PSS). Non-blocking: the caller supplies send/recv callbacks that
 * never wait, and drives the handshake with mint_tls_step(). No session
 * resumption, 0-RTT, client certificates or TLS 1.2 fallback. */
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

typedef struct mint_tls_dir { MINT_AEAD aead; uint8_t iv[12]; uint64_t seq; uint8_t secret[MINT_HASH_MAX]; int on; } MINT_TLS_DIR;

typedef struct mint_tls {
    MINT_TLS_IO io;
    char host[256];
    int64_t now;                    /* Unix seconds for certificate dates, 0 = skip */
    const MINT_TLS_ROOT *roots; uint32_t root_count;
    uint16_t suites[3]; uint32_t suite_count;      /* offered, in preference order */
    int state, error, cert_error, alert;
    uint16_t suite; uint32_t hash_alg; size_t hash_len;
    uint8_t priv[32], pub[32], session_id[32];
    uint8_t client_hello[512]; size_t client_hello_len;
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
#endif
