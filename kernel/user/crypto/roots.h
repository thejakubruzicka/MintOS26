#ifndef MINT_TLS_ROOTS_H
#define MINT_TLS_ROOTS_H
#include <stdint.h>
/* Trusted root certificate authorities (tools/make_roots.py). */
typedef struct mint_tls_root {
    const uint8_t *subject; uint32_t subject_len;   /* DER Name */
    const uint8_t *spki; uint32_t spki_len;         /* DER SubjectPublicKeyInfo */
    const char *label;
} MINT_TLS_ROOT;
extern const MINT_TLS_ROOT mint_tls_roots[];
extern const uint32_t mint_tls_root_count;
#endif
