#ifndef MINT_X509_H
#define MINT_X509_H
#include <stdint.h>
#include <stddef.h>
#include "crypto.h"
#include "roots.h"

typedef struct mint_x509 {
    const uint8_t *tbs; size_t tbs_len;
    const uint8_t *issuer; size_t issuer_len;
    const uint8_t *subject; size_t subject_len;
    const uint8_t *spki; size_t spki_len;
    const uint8_t *sig; size_t sig_len;
    const uint8_t *san; size_t san_len;          /* GeneralNames contents */
    const uint8_t *cn; size_t cn_len;            /* subject commonName (legacy fallback) */
    int64_t not_before, not_after;               /* Unix seconds */
    uint32_t sig_scheme;                         /* MINT_SIG_* or 0 if unsupported */
    int is_ca, has_basic_constraints, path_len;  /* path_len -1 = unlimited */
} MINT_X509;

enum {
    MINT_CERT_OK = 0, MINT_CERT_MALFORMED = 1, MINT_CERT_EXPIRED = 2, MINT_CERT_NOT_YET_VALID = 3,
    MINT_CERT_WRONG_HOST = 4, MINT_CERT_UNTRUSTED = 5, MINT_CERT_BAD_SIGNATURE = 6,
    MINT_CERT_UNSUPPORTED = 7, MINT_CERT_NOT_CA = 8
};

int mint_x509_parse(const uint8_t *der, size_t len, MINT_X509 *out);
int mint_x509_matches_host(const MINT_X509 *c, const char *host);
/* Validates certs[0] (the server's) for host up to one of roots. now = Unix
 * seconds, or 0 to skip date checks when no clock is available. */
int mint_x509_verify_chain(const uint8_t *const *certs, const size_t *lens, size_t count, const char *host,
                           int64_t now, const MINT_TLS_ROOT *roots, uint32_t root_count);
const char *mint_cert_error_text(int code);
#endif
