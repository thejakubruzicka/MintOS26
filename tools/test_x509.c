/* Host driver for signature verification and X.509 chain validation.
 *   verify SCHEME SPKI MSG SIG            -> 1 or 0
 *   chain HOST NOW ROOTS CERT...          -> result code (ROOTS: "mozilla" or SUBJECT:SPKI hex pair)
 *   host CERT HOST                        -> 1 or 0
 * SCHEME is a TLS SignatureScheme number (decimal) or a MINT_SIG_* value. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/user/crypto/x509.h"

void mint_random_bytes(void *out, size_t len) { arc4random_buf(out, len); }

static size_t unhex(const char *s, uint8_t **out) {
    size_t n = strlen(s) / 2;
    *out = malloc(n + 1);
    for (size_t i = 0; i < n; ++i) { unsigned v; sscanf(s + 2 * i, "%2x", &v); (*out)[i] = (uint8_t)v; }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "verify") && argc == 6) {
        uint8_t *spki, *msg, *sig;
        size_t sl = unhex(argv[3], &spki), ml = unhex(argv[4], &msg), gl = unhex(argv[5], &sig);
        MINT_PUBKEY key;
        if (!mint_spki_parse(spki, sl, &key)) { printf("badkey\n"); return 0; }
        printf("%d\n", mint_signature_verify(&key, (uint32_t)strtoul(argv[2], 0, 0), msg, ml, sig, gl));
        return 0;
    }
    if (!strcmp(argv[1], "chain") && argc >= 6) {
        const char *host = strcmp(argv[2], "-") ? argv[2] : NULL;
        int64_t now = strtoll(argv[3], 0, 10);
        MINT_TLS_ROOT custom;
        const MINT_TLS_ROOT *roots = mint_tls_roots;
        uint32_t root_count = mint_tls_root_count;
        if (strcmp(argv[4], "mozilla")) {
            char *colon = strchr(argv[4], ':');
            if (!colon) return 2;
            *colon = 0;
            uint8_t *subj, *spki;
            custom.subject_len = (uint32_t)unhex(argv[4], &subj); custom.subject = subj;
            custom.spki_len = (uint32_t)unhex(colon + 1, &spki); custom.spki = spki;
            custom.label = "test root";
            roots = &custom; root_count = 1;
        }
        const uint8_t *certs[8]; size_t lens[8];
        size_t count = 0;
        for (int i = 5; i < argc && count < 8; ++i) { uint8_t *c; lens[count] = unhex(argv[i], &c); certs[count++] = c; }
        printf("%d\n", mint_x509_verify_chain(certs, lens, count, host, now, roots, root_count));
        return 0;
    }
    if (!strcmp(argv[1], "host") && argc == 4) {
        uint8_t *c; size_t l = unhex(argv[2], &c);
        MINT_X509 x;
        if (!mint_x509_parse(c, l, &x)) { printf("malformed\n"); return 0; }
        printf("%d\n", mint_x509_matches_host(&x, argv[3]));
        return 0;
    }
    return 2;
}
