/* TLS 1.3 / 1.2 client — see tls.h for scope. Every length read from the
 * network is bounds-checked against the containing structure before use. */
#include "tls.h"

enum { ST_START = 0, ST_WAIT_SH, ST_WAIT_EE, ST_WAIT_CERT, ST_WAIT_CV, ST_WAIT_FIN, ST_DONE, ST_FAILED,
       ST12_WAIT_CERT, ST12_WAIT_SKE, ST12_WAIT_SHD, ST12_WAIT_CCS, ST12_WAIT_FIN };
enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APP = 23 };
enum { HS_HELLO_REQUEST = 0, HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXTENSIONS = 8,
       HS_CERTIFICATE = 11, HS_SERVER_KEY_EXCHANGE = 12, HS_CERTIFICATE_REQUEST = 13, HS_SERVER_HELLO_DONE = 14,
       HS_CERTIFICATE_VERIFY = 15, HS_CLIENT_KEY_EXCHANGE = 16, HS_FINISHED = 20, HS_KEY_UPDATE = 24, HS_MESSAGE_HASH = 254 };
#define SUITE_AES128 0x1301u
#define SUITE_AES256 0x1302u
#define SUITE_CHACHA 0x1303u
#define S12_ECDSA_AES128 0xC02Bu
#define S12_ECDSA_AES256 0xC02Cu
#define S12_RSA_AES128 0xC02Fu
#define S12_RSA_AES256 0xC030u
#define S12_ECDSA_CHACHA 0xCCA9u
#define S12_RSA_CHACHA 0xCCA8u
#define GROUP_X25519 0x001Du
#define GROUP_P256 0x0017u

static const uint8_t HRR_RANDOM[32] = {0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
                                       0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C};
static const uint8_t DOWNGRADE[7] = {'D', 'O', 'W', 'N', 'G', 'R', 'D'};

static void mcopy(void *d, const void *s, size_t n) { uint8_t *o = (uint8_t *)d; const uint8_t *i = (const uint8_t *)s; while (n--) *o++ = *i++; }
static void mzero(void *d, size_t n) { volatile uint8_t *o = (volatile uint8_t *)d; while (n--) *o++ = 0; }
static uint16_t g16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t g24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void p24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static int is_tls12_suite(uint16_t s) {
    return s == S12_ECDSA_AES128 || s == S12_ECDSA_AES256 || s == S12_RSA_AES128 || s == S12_RSA_AES256 || s == S12_ECDSA_CHACHA || s == S12_RSA_CHACHA;
}
static int suite_is_ecdsa(uint16_t s) { return s == S12_ECDSA_AES128 || s == S12_ECDSA_AES256 || s == S12_ECDSA_CHACHA; }
static uint32_t suite_aead(uint16_t s) {
    if (s == SUITE_CHACHA || s == S12_ECDSA_CHACHA || s == S12_RSA_CHACHA) return MINT_AEAD_CHACHA20_POLY1305;
    if (s == SUITE_AES256 || s == S12_ECDSA_AES256 || s == S12_RSA_AES256) return MINT_AEAD_AES256_GCM;
    return MINT_AEAD_AES128_GCM;
}
static uint32_t suite_hash(uint16_t s) { return (s == SUITE_AES256 || s == S12_ECDSA_AES256 || s == S12_RSA_AES256) ? MINT_SHA384 : MINT_SHA256; }

static int fail(MINT_TLS *t, int code) { if (t->state != ST_FAILED) { t->state = ST_FAILED; t->error = code; } return t->error; }

/* ------------------------------------------------------------------ output */
static int flush_out(MINT_TLS *t) {
    while (t->wlen) {
        int64_t n = t->io.send(t->io.ctx, t->wbuf, t->wlen);
        if (n < 0) return fail(t, MINT_TLS_ERR_IO);
        if (n == 0) return MINT_TLS_AGAIN;
        for (size_t i = (size_t)n; i < t->wlen; ++i) t->wbuf[i - (size_t)n] = t->wbuf[i];
        t->wlen -= (size_t)n;
    }
    return MINT_TLS_DONE;
}
static void make_nonce(const MINT_TLS_DIR *d, uint8_t nonce[12]) {
    for (int i = 0; i < 12; ++i) nonce[i] = d->iv[i];
    for (int i = 0; i < 8; ++i) nonce[11 - i] ^= (uint8_t)(d->seq >> (8 * i));
}
static void aad12(const MINT_TLS_DIR *d, uint8_t type, size_t len, uint8_t aad[13]) {
    for (int i = 0; i < 8; ++i) aad[i] = (uint8_t)(d->seq >> (56 - 8 * i));
    aad[8] = type; aad[9] = 3; aad[10] = 3; p16(aad + 11, (uint32_t)len);
}
/* Appends one record (encrypting when keys are set); returns 0 if no room. */
static int queue_record(MINT_TLS *t, uint8_t type, const uint8_t *data, size_t len) {
    int v12 = t->version == 0x0303;
    size_t overhead = !t->wr.on ? 0u : v12 ? (t->wr.explicit_nonce ? 24u : 16u) : 17u;
    size_t total = 5u + len + overhead;
    if (len > 16384u || t->wlen + total > sizeof(t->wbuf)) return 0;
    uint8_t *r = t->wbuf + t->wlen;
    if (!t->wr.on) {
        r[0] = type; r[1] = 3; r[2] = (type == CT_HANDSHAKE && !v12) ? 1 : 3; p16(r + 3, (uint32_t)len);
        mcopy(r + 5, data, len);
    } else if (v12) {
        uint8_t nonce[12], aad[13];
        r[0] = type; r[1] = 3; r[2] = 3; p16(r + 3, (uint32_t)(len + overhead));
        aad12(&t->wr, type, len, aad);
        uint8_t *ct = r + 5;
        if (t->wr.explicit_nonce) {
            for (int i = 0; i < 4; ++i) nonce[i] = t->wr.iv[i];
            for (int i = 0; i < 8; ++i) nonce[4 + i] = aad[i];          /* explicit nonce = sequence number */
            mcopy(r + 5, nonce + 4, 8);
            ct = r + 13;
        } else make_nonce(&t->wr, nonce);
        mcopy(ct, data, len);
        mint_aead_seal(&t->wr.aead, nonce, aad, 13, ct, len, ct, ct + len);
        ++t->wr.seq;
    } else {
        r[0] = CT_APP; r[1] = 3; r[2] = 3; p16(r + 3, (uint32_t)(len + 17u));
        mcopy(r + 5, data, len);
        r[5 + len] = type;
        uint8_t nonce[12];
        make_nonce(&t->wr, nonce);
        mint_aead_seal(&t->wr.aead, nonce, r, 5, r + 5, len + 1u, r + 5, r + 5 + len + 1u);
        ++t->wr.seq;
    }
    t->wlen += total;
    return 1;
}

/* ------------------------------------------------------------------ keys */
static void set_keys(MINT_TLS *t, MINT_TLS_DIR *d, const uint8_t *secret) {
    uint32_t kind = suite_aead(t->suite);
    uint8_t key[32];
    mcopy(d->secret, secret, t->hash_len);
    mint_hkdf_expand_label(t->hash_alg, secret, "key", 0, 0, key, mint_aead_key_len(kind));
    mint_hkdf_expand_label(t->hash_alg, secret, "iv", 0, 0, d->iv, 12);
    mint_aead_init(&d->aead, kind, key);
    d->seq = 0; d->on = 1; d->explicit_nonce = 0;
    mzero(key, sizeof(key));
}
static void transcript_hash(MINT_TLS *t, uint8_t *out) { mint_hash_final(&t->transcript, out); }
static void derive_secret(MINT_TLS *t, const uint8_t *secret, const char *label, const uint8_t *thash, uint8_t *out) {
    mint_hkdf_expand_label(t->hash_alg, secret, label, thash, t->hash_len, out, t->hash_len);
}

/* TLS 1.2 PRF (RFC 5246 section 5) with the suite's hash. */
static void prf12(MINT_TLS *t, const uint8_t *secret, size_t slen, const char *label, const uint8_t *seed, size_t seedlen,
                  uint8_t *out, size_t outlen) {
    size_t ll = 0; while (label[ll]) ++ll;
    uint8_t a[MINT_HASH_MAX], blk[MINT_HASH_MAX], tmp[MINT_HASH_MAX];
    MINT_HMAC m;
    mint_hmac_init(&m, t->hash_alg, secret, slen);
    mint_hmac_update(&m, label, ll); mint_hmac_update(&m, seed, seedlen);
    mint_hmac_final(&m, a);
    size_t done = 0;
    while (done < outlen) {
        mint_hmac_init(&m, t->hash_alg, secret, slen);
        mint_hmac_update(&m, a, t->hash_len); mint_hmac_update(&m, label, ll); mint_hmac_update(&m, seed, seedlen);
        mint_hmac_final(&m, blk);
        for (size_t i = 0; i < t->hash_len && done < outlen; ++i) out[done++] = blk[i];
        mint_hmac(t->hash_alg, secret, slen, a, t->hash_len, tmp);
        mcopy(a, tmp, t->hash_len);
    }
    mzero(a, sizeof(a)); mzero(blk, sizeof(blk)); mzero(tmp, sizeof(tmp));
}

/* ------------------------------------------------------------------ ClientHello */
static size_t build_client_hello(MINT_TLS *t) {
    uint8_t *m = t->client_hello;
    size_t n = 4;
    m[n++] = 3; m[n++] = 3;
    mcopy(m + n, t->client_random, 32); n += 32;
    m[n++] = 32; mcopy(m + n, t->session_id, 32); n += 32;
    p16(m + n, (uint32_t)(2u * t->suite_count)); n += 2;
    for (uint32_t i = 0; i < t->suite_count; ++i) { p16(m + n, t->suites[i]); n += 2; }
    m[n++] = 1; m[n++] = 0;
    size_t ext_len_at = n; n += 2;
    size_t host_len = 0; while (t->host[host_len]) ++host_len;
    int is_ip = 1;
    for (size_t i = 0; i < host_len; ++i) if (!((t->host[i] >= '0' && t->host[i] <= '9') || t->host[i] == '.')) is_ip = 0;
    if (host_len && !is_ip) {                                         /* server_name */
        p16(m + n, 0); p16(m + n + 2, (uint32_t)(host_len + 5u)); p16(m + n + 4, (uint32_t)(host_len + 3u));
        m[n + 6] = 0; p16(m + n + 7, (uint32_t)host_len); mcopy(m + n + 9, t->host, host_len);
        n += 9 + host_len;
    }
    /* secp384r1 last: it lets TLS 1.2 servers use P-384 certificates (RFC 8422); key exchange uses X25519 or P-256 */
    static const uint8_t groups[] = {0x00,0x0a, 0x00,0x08, 0x00,0x06, 0x00,0x1d, 0x00,0x17, 0x00,0x18};
    mcopy(m + n, groups, sizeof(groups)); n += sizeof(groups);
    static const uint8_t point_formats[] = {0x00,0x0b, 0x00,0x02, 0x01, 0x00};
    mcopy(m + n, point_formats, sizeof(point_formats)); n += sizeof(point_formats);
    static const uint8_t sigalgs[] = {0x00,0x0d, 0x00,0x14, 0x00,0x12, 0x04,0x03, 0x05,0x03, 0x06,0x03, 0x08,0x04, 0x08,0x05, 0x08,0x06,
                                      0x04,0x01, 0x05,0x01, 0x06,0x01};
    mcopy(m + n, sigalgs, sizeof(sigalgs)); n += sizeof(sigalgs);
    static const uint8_t versions[] = {0x00,0x2b, 0x00,0x05, 0x04, 0x03,0x04, 0x03,0x03};
    mcopy(m + n, versions, sizeof(versions)); n += sizeof(versions);
    static const uint8_t alpn[] = {0x00,0x10, 0x00,0x0b, 0x00,0x09, 0x08,'h','t','t','p','/','1','.','1'};
    mcopy(m + n, alpn, sizeof(alpn)); n += sizeof(alpn);
    static const uint8_t ems_reneg[] = {0x00,0x17, 0x00,0x00, 0xff,0x01, 0x00,0x01, 0x00};
    mcopy(m + n, ems_reneg, sizeof(ems_reneg)); n += sizeof(ems_reneg);
    if (t->cookie_len) {                                              /* echo the HelloRetryRequest cookie */
        p16(m + n, 44); p16(m + n + 2, (uint32_t)(t->cookie_len + 2u)); p16(m + n + 4, (uint32_t)t->cookie_len);
        mcopy(m + n + 6, t->cookie, t->cookie_len); n += 6 + t->cookie_len;
    }
    if (!t->hrr_done) {                                               /* key_share: X25519 */
        p16(m + n, 51); p16(m + n + 2, 38); p16(m + n + 4, 36); p16(m + n + 6, GROUP_X25519); p16(m + n + 8, 32);
        mcopy(m + n + 10, t->pub, 32); n += 42;
    } else {                                                          /* key_share: P-256 after HelloRetryRequest */
        p16(m + n, 51); p16(m + n + 2, 71); p16(m + n + 4, 69); p16(m + n + 6, GROUP_P256); p16(m + n + 8, 65);
        mcopy(m + n + 10, t->p256_pub, 65); n += 75;
    }
    p16(m + ext_len_at, (uint32_t)(n - ext_len_at - 2u));
    m[0] = HS_CLIENT_HELLO; p24(m + 1, (uint32_t)(n - 4u));
    t->client_hello_len = n;
    return n;
}

static int send_client_hello(MINT_TLS *t) {
    size_t n = build_client_hello(t);
    if (!queue_record(t, CT_HANDSHAKE, t->client_hello, n)) return fail(t, MINT_TLS_ERR_PROTOCOL);
    t->state = ST_WAIT_SH;
    return MINT_TLS_AGAIN;
}

/* ------------------------------------------------------------------ ServerHello */
static int handle_hrr(MINT_TLS *t, const uint8_t *m, size_t len, uint16_t suite, const uint8_t *ext, const uint8_t *end) {
    if (t->hrr_done) return fail(t, MINT_TLS_ERR_PROTOCOL);          /* a second HelloRetryRequest */
    uint16_t group = 0;
    int version_ok = 0;
    const uint8_t *b = ext;
    while (b < end) {
        if (end - b < 4) return fail(t, MINT_TLS_ERR_PROTOCOL);
        uint16_t type = g16(b), elen = g16(b + 2);
        b += 4;
        if ((size_t)(end - b) < elen) return fail(t, MINT_TLS_ERR_PROTOCOL);
        if (type == 43 && elen == 2 && g16(b) == 0x0304) version_ok = 1;
        if (type == 51 && elen == 2) group = g16(b);
        if (type == 44 && elen >= 2) {
            size_t cl = g16(b);
            if (cl + 2u != elen || cl > sizeof(t->cookie)) return fail(t, MINT_TLS_ERR_PROTOCOL);
            mcopy(t->cookie, b + 2, cl); t->cookie_len = cl;
        }
        b += elen;
    }
    if (!version_ok) return fail(t, MINT_TLS_ERR_PROTOCOL);
    if (group != GROUP_P256) return fail(t, MINT_TLS_ERR_UNSUPPORTED);
    if (!mint_p256_keygen(t->p256_priv, t->p256_pub)) return fail(t, MINT_TLS_ERR_PROTOCOL);
    t->suite = suite;
    t->hash_alg = suite_hash(suite);
    t->hash_len = mint_hash_len(t->hash_alg);
    /* transcript = message_hash(ClientHello1) || HelloRetryRequest || ClientHello2 */
    uint8_t synthetic[4 + MINT_HASH_MAX];
    synthetic[0] = HS_MESSAGE_HASH; synthetic[1] = 0; synthetic[2] = 0; synthetic[3] = (uint8_t)t->hash_len;
    mint_hash(t->hash_alg, t->client_hello, t->client_hello_len, synthetic + 4);
    mint_hash_init(&t->transcript, t->hash_alg);
    mint_hash_update(&t->transcript, synthetic, 4u + t->hash_len);
    mint_hash_update(&t->transcript, m, len);
    t->hrr_done = 1;
    size_t n = build_client_hello(t);
    mint_hash_update(&t->transcript, t->client_hello, n);
    t->transcript_started = 1;
    if (!queue_record(t, CT_HANDSHAKE, t->client_hello, n)) return fail(t, MINT_TLS_ERR_PROTOCOL);
    t->state = ST_WAIT_SH;
    return MINT_TLS_AGAIN;
}

static int start_tls12(MINT_TLS *t, const uint8_t *m, size_t len, uint16_t suite, int ems) {
    if (!is_tls12_suite(suite)) return fail(t, MINT_TLS_ERR_PROTOCOL);
    t->version = 0x0303;
    t->suite = suite;
    t->hash_alg = suite_hash(suite);
    t->hash_len = mint_hash_len(t->hash_alg);
    t->ems = ems;
    mint_hash_init(&t->transcript, t->hash_alg);
    mint_hash_update(&t->transcript, t->client_hello, t->client_hello_len);
    mint_hash_update(&t->transcript, m, len);
    t->state = ST12_WAIT_CERT;
    return MINT_TLS_AGAIN;
}

static int handle_server_hello(MINT_TLS *t, const uint8_t *m, size_t len) {
    if (len < 4 + 2 + 32 + 1) return fail(t, MINT_TLS_ERR_PROTOCOL);
    const uint8_t *b = m + 4, *end = m + len;
    if (b[0] != 3 || b[1] != 3) return fail(t, MINT_TLS_ERR_PROTOCOL);
    int hrr = 1;
    for (int i = 0; i < 32; ++i) if (b[2 + i] != HRR_RANDOM[i]) hrr = 0;
    mcopy(t->server_random, b + 2, 32);
    b += 34;
    size_t sid = *b++;
    if (sid > 32 || (size_t)(end - b) < sid + 3u) return fail(t, MINT_TLS_ERR_PROTOCOL);
    const uint8_t *sid_p = b;
    b += sid;
    uint16_t suite = g16(b); b += 2;
    if (*b++ != 0) return fail(t, MINT_TLS_ERR_PROTOCOL);
    int offered = 0;
    for (uint32_t i = 0; i < t->suite_count; ++i) if (t->suites[i] == suite) offered = 1;
    if (!offered) return fail(t, MINT_TLS_ERR_PROTOCOL);
    size_t ext_total = 0;
    if (end - b >= 2) { ext_total = g16(b); b += 2; }
    if ((size_t)(end - b) != ext_total) return fail(t, MINT_TLS_ERR_PROTOCOL);
    if (hrr) {
        if (sid != 32 || !mint_ct_equal(sid_p, t->session_id, 32)) return fail(t, MINT_TLS_ERR_PROTOCOL);
        return handle_hrr(t, m, len, suite, b, end);
    }
    int version_ok = 0, ems = 0;
    const uint8_t *share = 0; uint16_t share_group = 0; size_t share_len = 0;
    while (b < end) {
        if (end - b < 4) return fail(t, MINT_TLS_ERR_PROTOCOL);
        uint16_t type = g16(b), elen = g16(b + 2);
        b += 4;
        if ((size_t)(end - b) < elen) return fail(t, MINT_TLS_ERR_PROTOCOL);
        if (type == 43 && elen == 2 && g16(b) == 0x0304) version_ok = 1;
        if (type == 51 && elen >= 4) { share_group = g16(b); share_len = g16(b + 2); if (share_len + 4u == elen) share = b + 4; }
        if (type == 23 && elen == 0) ems = 1;
        b += elen;
    }
    if (!version_ok) {
        /* TLS 1.2: the server must not be a TLS 1.3 server pretending (RFC 8446 4.1.3) */
        int sentinel = 1;
        for (int i = 0; i < 7; ++i) if (t->server_random[24 + i] != DOWNGRADE[i]) sentinel = 0;
        if (sentinel && (t->server_random[31] == 1 || t->server_random[31] == 0)) return fail(t, MINT_TLS_ERR_PROTOCOL);
        if (t->hrr_done) return fail(t, MINT_TLS_ERR_PROTOCOL);
        return start_tls12(t, m, len, suite, ems);
    }
    if (sid != 32 || !mint_ct_equal(sid_p, t->session_id, 32)) return fail(t, MINT_TLS_ERR_PROTOCOL);
    if (is_tls12_suite(suite) || (t->hrr_done && suite != t->suite)) return fail(t, MINT_TLS_ERR_PROTOCOL);
    uint8_t shared[32];
    if (t->hrr_done) {
        if (!share || share_group != GROUP_P256 || share_len != 65 || !mint_p256_ecdh(shared, t->p256_priv, share, 65)) return fail(t, MINT_TLS_ERR_PROTOCOL);
        t->group = GROUP_P256;
    } else {
        if (!share || share_group != GROUP_X25519 || share_len != 32) return fail(t, MINT_TLS_ERR_PROTOCOL);
        mint_x25519(shared, t->priv, share);
        uint8_t acc = 0;
        for (int i = 0; i < 32; ++i) acc |= shared[i];
        if (!acc) return fail(t, MINT_TLS_ERR_PROTOCOL);
        t->group = GROUP_X25519;
    }
    t->version = 0x0304;
    t->suite = suite;
    t->hash_alg = suite_hash(suite);
    t->hash_len = mint_hash_len(t->hash_alg);
    if (!t->transcript_started) {
        mint_hash_init(&t->transcript, t->hash_alg);
        mint_hash_update(&t->transcript, t->client_hello, t->client_hello_len);
    }
    mint_hash_update(&t->transcript, m, len);
    uint8_t zero[MINT_HASH_MAX], empty_hash[MINT_HASH_MAX], derived[MINT_HASH_MAX], early[MINT_HASH_MAX], th[MINT_HASH_MAX];
    mzero(zero, sizeof(zero));
    mint_hash(t->hash_alg, "", 0, empty_hash);
    mint_hkdf_extract(t->hash_alg, 0, 0, zero, t->hash_len, early);
    derive_secret(t, early, "derived", empty_hash, derived);
    mint_hkdf_extract(t->hash_alg, derived, t->hash_len, shared, 32, t->hs_secret);
    transcript_hash(t, th);
    derive_secret(t, t->hs_secret, "c hs traffic", th, t->c_hs);
    derive_secret(t, t->hs_secret, "s hs traffic", th, t->s_hs);
    derive_secret(t, t->hs_secret, "derived", empty_hash, derived);
    mint_hkdf_extract(t->hash_alg, derived, t->hash_len, zero, t->hash_len, t->master);
    set_keys(t, &t->rd, t->s_hs);
    mzero(shared, sizeof(shared)); mzero(t->priv, sizeof(t->priv)); mzero(t->p256_priv, sizeof(t->p256_priv));
    t->state = ST_WAIT_EE;
    return MINT_TLS_AGAIN;
}

/* ------------------------------------------------------------------ certificates */
/* Validates the chain (certs/lens) and keeps the leaf key. */
static int accept_chain(MINT_TLS *t, const uint8_t *const *certs, const size_t *lens, size_t count) {
    if (!count) return fail(t, MINT_TLS_ERR_PROTOCOL);
    int rc = mint_x509_verify_chain(certs, lens, count, t->host, t->now, t->roots, t->root_count);
    if (rc != MINT_CERT_OK) { t->cert_error = rc; return fail(t, MINT_TLS_ERR_CERT); }
    MINT_X509 leaf;
    if (!mint_x509_parse(certs[0], lens[0], &leaf) || leaf.spki_len > sizeof(t->leaf_spki)) return fail(t, MINT_TLS_ERR_CERT);
    mcopy(t->leaf_spki, leaf.spki, leaf.spki_len);
    t->leaf_spki_len = leaf.spki_len;
    MINT_PUBKEY key;
    if (mint_spki_parse(t->leaf_spki, t->leaf_spki_len, &key)) t->key_is_ec = key.type == MINT_KEY_EC_P256 || key.type == MINT_KEY_EC_P384;
    t->got_cert = 1;
    return MINT_TLS_AGAIN;
}

static int handle_certificate(MINT_TLS *t, const uint8_t *m, size_t len) {
    const uint8_t *b = m + 4, *end = m + len;
    if (end - b < 4) return fail(t, MINT_TLS_ERR_PROTOCOL);
    size_t ctx = *b++;
    if ((size_t)(end - b) < ctx + 3u) return fail(t, MINT_TLS_ERR_PROTOCOL);
    b += ctx;
    size_t list = g24(b); b += 3;
    if ((size_t)(end - b) != list || !list) return fail(t, MINT_TLS_ERR_PROTOCOL);
    const uint8_t *certs[8]; size_t lens[8]; size_t count = 0;
    while (b < end) {
        if (end - b < 3) return fail(t, MINT_TLS_ERR_PROTOCOL);
        size_t cl = g24(b); b += 3;
        if ((size_t)(end - b) < cl + 2u || !cl) return fail(t, MINT_TLS_ERR_PROTOCOL);
        if (count < 8) { certs[count] = b; lens[count] = cl; ++count; }
        b += cl;
        size_t el = g16(b); b += 2;
        if ((size_t)(end - b) < el) return fail(t, MINT_TLS_ERR_PROTOCOL);
        b += el;
    }
    return accept_chain(t, certs, lens, count);
}

/* TLS 1.2 Certificate: no request context and no per-certificate extensions. */
static int handle_certificate12(MINT_TLS *t, const uint8_t *m, size_t len) {
    const uint8_t *b = m + 4, *end = m + len;
    if (end - b < 3) return fail(t, MINT_TLS_ERR_PROTOCOL);
    size_t list = g24(b); b += 3;
    if ((size_t)(end - b) != list || !list) return fail(t, MINT_TLS_ERR_PROTOCOL);
    const uint8_t *certs[8]; size_t lens[8]; size_t count = 0;
    while (b < end) {
        if (end - b < 3) return fail(t, MINT_TLS_ERR_PROTOCOL);
        size_t cl = g24(b); b += 3;
        if ((size_t)(end - b) < cl || !cl) return fail(t, MINT_TLS_ERR_PROTOCOL);
        if (count < 8) { certs[count] = b; lens[count] = cl; ++count; }
        b += cl;
    }
    if (accept_chain(t, certs, lens, count) != MINT_TLS_AGAIN) return t->error;
    if (suite_is_ecdsa(t->suite) != t->key_is_ec) return fail(t, MINT_TLS_ERR_PROTOCOL);   /* key must match the suite */
    return MINT_TLS_AGAIN;
}

static int handle_cert_verify(MINT_TLS *t, const uint8_t *m, size_t len, const uint8_t *thash) {
    if (len < 8) return fail(t, MINT_TLS_ERR_PROTOCOL);
    uint16_t scheme = g16(m + 4), sl = g16(m + 6);
    if ((size_t)sl + 8u != len) return fail(t, MINT_TLS_ERR_PROTOCOL);
    if (scheme != MINT_SIG_ECDSA_P256_SHA256 && scheme != MINT_SIG_ECDSA_P384_SHA384 &&
        scheme != MINT_SIG_RSA_PSS_SHA256 && scheme != MINT_SIG_RSA_PSS_SHA384 && scheme != MINT_SIG_RSA_PSS_SHA512)
        return fail(t, MINT_TLS_ERR_UNSUPPORTED);
    uint8_t content[64 + 34 + MINT_HASH_MAX];
    for (int i = 0; i < 64; ++i) content[i] = 0x20;
    mcopy(content + 64, "TLS 1.3, server CertificateVerify", 33);
    content[97] = 0;
    mcopy(content + 98, thash, t->hash_len);
    MINT_PUBKEY key;
    if (!mint_spki_parse(t->leaf_spki, t->leaf_spki_len, &key) ||
        !mint_signature_verify(&key, scheme, content, 98u + t->hash_len, m + 8, sl)) {
        t->cert_error = MINT_CERT_BAD_SIGNATURE;
        return fail(t, MINT_TLS_ERR_CERT);
    }
    t->got_cert_verify = 1;
    return MINT_TLS_AGAIN;
}

static void finished_mac(MINT_TLS *t, const uint8_t *base_secret, const uint8_t *thash, uint8_t *out) {
    uint8_t fk[MINT_HASH_MAX];
    mint_hkdf_expand_label(t->hash_alg, base_secret, "finished", 0, 0, fk, t->hash_len);
    mint_hmac(t->hash_alg, fk, t->hash_len, thash, t->hash_len, out);
    mzero(fk, sizeof(fk));
}

static int handle_finished(MINT_TLS *t, const uint8_t *m, size_t len, const uint8_t *thash) {
    uint8_t expect[MINT_HASH_MAX];
    if (len != 4u + t->hash_len) return fail(t, MINT_TLS_ERR_PROTOCOL);
    finished_mac(t, t->s_hs, thash, expect);
    if (!mint_ct_equal(expect, m + 4, t->hash_len)) return fail(t, MINT_TLS_ERR_DECRYPT);
    mint_hash_update(&t->transcript, m, len);
    uint8_t th[MINT_HASH_MAX], c_ap[MINT_HASH_MAX], s_ap[MINT_HASH_MAX];
    transcript_hash(t, th);
    derive_secret(t, t->master, "c ap traffic", th, c_ap);
    derive_secret(t, t->master, "s ap traffic", th, s_ap);
    set_keys(t, &t->wr, t->c_hs);
    if (t->cert_requested) {                                          /* empty client Certificate */
        uint8_t cert[8] = {HS_CERTIFICATE, 0, 0, 4, 0, 0, 0, 0};
        mint_hash_update(&t->transcript, cert, sizeof(cert));
        transcript_hash(t, th);
        if (!queue_record(t, CT_HANDSHAKE, cert, sizeof(cert))) return fail(t, MINT_TLS_ERR_PROTOCOL);
    }
    uint8_t fin[4 + MINT_HASH_MAX];
    fin[0] = HS_FINISHED; p24(fin + 1, (uint32_t)t->hash_len);
    finished_mac(t, t->c_hs, th, fin + 4);
    if (!queue_record(t, CT_HANDSHAKE, fin, 4u + t->hash_len)) return fail(t, MINT_TLS_ERR_PROTOCOL);
    set_keys(t, &t->rd, s_ap);
    set_keys(t, &t->wr, c_ap);
    mzero(c_ap, sizeof(c_ap)); mzero(s_ap, sizeof(s_ap));
    t->state = ST_DONE;
    return MINT_TLS_AGAIN;
}

static void update_traffic_secret(MINT_TLS *t, MINT_TLS_DIR *d) {
    uint8_t next[MINT_HASH_MAX];
    mint_hkdf_expand_label(t->hash_alg, d->secret, "traffic upd", 0, 0, next, t->hash_len);
    set_keys(t, d, next);
    mzero(next, sizeof(next));
}

/* ------------------------------------------------------------------ TLS 1.2 key exchange */
static int handle_ske12(MINT_TLS *t, const uint8_t *m, size_t len) {
    const uint8_t *b = m + 4, *end = m + len;
    if (end - b < 4 || b[0] != 3) return fail(t, MINT_TLS_ERR_UNSUPPORTED);        /* named_curve only */
    uint16_t group = g16(b + 1);
    size_t pl = b[3];
    if ((size_t)(end - b) < 4u + pl + 4u) return fail(t, MINT_TLS_ERR_PROTOCOL);
    const uint8_t *point = b + 4;
    size_t params_len = 4u + pl;
    const uint8_t *sp = b + params_len;
    uint16_t scheme = g16(sp), sl = g16(sp + 2);
    if ((size_t)(end - sp) != 4u + sl) return fail(t, MINT_TLS_ERR_PROTOCOL);
    uint32_t verify_scheme;
    switch (scheme) {
    case 0x0401: case 0x0501: case 0x0601: case 0x0804: case 0x0805: case 0x0806: verify_scheme = scheme; break;
    case 0x0403: verify_scheme = MINT_SIG_ECDSA_SHA256; break;
    case 0x0503: verify_scheme = MINT_SIG_ECDSA_SHA384; break;
    case 0x0603: verify_scheme = MINT_SIG_ECDSA_SHA512; break;
    default: return fail(t, MINT_TLS_ERR_UNSUPPORTED);
    }
    static uint8_t signed_data[64 + 4 + 133];
    if (params_len > 4u + 133u) return fail(t, MINT_TLS_ERR_PROTOCOL);
    mcopy(signed_data, t->client_random, 32);
    mcopy(signed_data + 32, t->server_random, 32);
    mcopy(signed_data + 64, b, params_len);
    MINT_PUBKEY key;
    if (!mint_spki_parse(t->leaf_spki, t->leaf_spki_len, &key) ||
        !mint_signature_verify(&key, verify_scheme, signed_data, 64u + params_len, sp + 4, sl)) {
        t->cert_error = MINT_CERT_BAD_SIGNATURE;
        return fail(t, MINT_TLS_ERR_CERT);
    }
    if (group == GROUP_X25519 && pl == 32) {
        mint_x25519(t->premaster, t->priv, point);
        uint8_t acc = 0;
        for (int i = 0; i < 32; ++i) acc |= t->premaster[i];
        if (!acc) return fail(t, MINT_TLS_ERR_PROTOCOL);
    } else if (group == GROUP_P256 && pl == 65) {
        if (!mint_p256_keygen(t->p256_priv, t->p256_pub) || !mint_p256_ecdh(t->premaster, t->p256_priv, point, 65))
            return fail(t, MINT_TLS_ERR_PROTOCOL);
    } else return fail(t, MINT_TLS_ERR_UNSUPPORTED);
    t->group = group;
    t->got_cert_verify = 1;
    return MINT_TLS_AGAIN;
}

static void set_keys12(MINT_TLS *t) {
    uint32_t kind = suite_aead(t->suite);
    size_t kl = mint_aead_key_len(kind), ivl = kind == MINT_AEAD_CHACHA20_POLY1305 ? 12u : 4u;
    uint8_t seed[64], kb[2 * 32 + 2 * 12];
    mcopy(seed, t->server_random, 32); mcopy(seed + 32, t->client_random, 32);
    prf12(t, t->master12, 48, "key expansion", seed, 64, kb, 2u * kl + 2u * ivl);
    mint_aead_init(&t->wr.aead, kind, kb);
    mint_aead_init(&t->rd.aead, kind, kb + kl);
    mzero(t->wr.iv, 12); mzero(t->rd.iv, 12);
    mcopy(t->wr.iv, kb + 2 * kl, ivl);
    mcopy(t->rd.iv, kb + 2 * kl + ivl, ivl);
    t->wr.explicit_nonce = t->rd.explicit_nonce = ivl == 4u;
    t->wr.seq = t->rd.seq = 0;
    mzero(kb, sizeof(kb));
}

/* ServerHelloDone: send [Certificate], ClientKeyExchange, ChangeCipherSpec, Finished. */
static int client_flight12(MINT_TLS *t) {
    if (t->cert_requested) {
        uint8_t cert[7] = {HS_CERTIFICATE, 0, 0, 3, 0, 0, 0};
        mint_hash_update(&t->transcript, cert, sizeof(cert));
        if (!queue_record(t, CT_HANDSHAKE, cert, sizeof(cert))) return fail(t, MINT_TLS_ERR_PROTOCOL);
    }
    uint8_t cke[4 + 1 + 65];
    size_t pl = t->group == GROUP_P256 ? 65u : 32u;
    cke[0] = HS_CLIENT_KEY_EXCHANGE; p24(cke + 1, (uint32_t)(1u + pl)); cke[4] = (uint8_t)pl;
    mcopy(cke + 5, t->group == GROUP_P256 ? t->p256_pub : t->pub, pl);
    mint_hash_update(&t->transcript, cke, 5u + pl);
    if (!queue_record(t, CT_HANDSHAKE, cke, 5u + pl)) return fail(t, MINT_TLS_ERR_PROTOCOL);
    /* master secret: extended (RFC 7627) when the server agreed, else the classic one */
    if (t->ems) {
        uint8_t sh[MINT_HASH_MAX];
        transcript_hash(t, sh);
        prf12(t, t->premaster, 32, "extended master secret", sh, t->hash_len, t->master12, 48);
    } else {
        uint8_t seed[64];
        mcopy(seed, t->client_random, 32); mcopy(seed + 32, t->server_random, 32);
        prf12(t, t->premaster, 32, "master secret", seed, 64, t->master12, 48);
    }
    mzero(t->premaster, sizeof(t->premaster)); mzero(t->priv, sizeof(t->priv)); mzero(t->p256_priv, sizeof(t->p256_priv));
    static const uint8_t ccs = 1;
    if (!queue_record(t, CT_CCS, &ccs, 1)) return fail(t, MINT_TLS_ERR_PROTOCOL);
    set_keys12(t);
    t->wr.on = 1;
    uint8_t th[MINT_HASH_MAX], fin[16];
    transcript_hash(t, th);
    fin[0] = HS_FINISHED; p24(fin + 1, 12);
    prf12(t, t->master12, 48, "client finished", th, t->hash_len, fin + 4, 12);
    mint_hash_update(&t->transcript, fin, 16);
    if (!queue_record(t, CT_HANDSHAKE, fin, 16)) return fail(t, MINT_TLS_ERR_PROTOCOL);
    t->state = ST12_WAIT_CCS;
    return MINT_TLS_AGAIN;
}

static int handle_finished12(MINT_TLS *t, const uint8_t *m, size_t len) {
    if (len != 16u) return fail(t, MINT_TLS_ERR_PROTOCOL);
    uint8_t th[MINT_HASH_MAX], expect[12];
    transcript_hash(t, th);
    prf12(t, t->master12, 48, "server finished", th, t->hash_len, expect, 12);
    if (!mint_ct_equal(expect, m + 4, 12)) return fail(t, MINT_TLS_ERR_DECRYPT);
    t->state = ST_DONE;
    return MINT_TLS_AGAIN;
}

/* Processes complete handshake messages in t->hs. */
static int process_handshake(MINT_TLS *t) {
    size_t off = 0;
    while (t->state != ST_FAILED && t->hs_len - off >= 4u) {
        const uint8_t *m = t->hs + off;
        size_t body = g24(m + 1), len = 4u + body;
        if (body > MINT_TLS_HS_MAX - 4u) return fail(t, MINT_TLS_ERR_PROTOCOL);
        if (t->hs_len - off < len) break;
        uint8_t type = m[0];
        uint8_t th[MINT_HASH_MAX];
        switch (t->state) {
        case ST_WAIT_SH:
            if (type != HS_SERVER_HELLO) return fail(t, MINT_TLS_ERR_PROTOCOL);
            handle_server_hello(t, m, len);
            break;
        case ST_WAIT_EE:
            if (type != HS_ENCRYPTED_EXTENSIONS) return fail(t, MINT_TLS_ERR_PROTOCOL);
            mint_hash_update(&t->transcript, m, len);
            t->state = ST_WAIT_CERT;
            break;
        case ST_WAIT_CERT:
            if (type == HS_CERTIFICATE_REQUEST) { t->cert_requested = 1; mint_hash_update(&t->transcript, m, len); break; }
            if (type != HS_CERTIFICATE) return fail(t, MINT_TLS_ERR_PROTOCOL);
            mint_hash_update(&t->transcript, m, len);
            if (handle_certificate(t, m, len) == MINT_TLS_AGAIN) t->state = ST_WAIT_CV;
            break;
        case ST_WAIT_CV:
            if (type != HS_CERTIFICATE_VERIFY) return fail(t, MINT_TLS_ERR_PROTOCOL);
            transcript_hash(t, th);
            if (handle_cert_verify(t, m, len, th) == MINT_TLS_AGAIN) { mint_hash_update(&t->transcript, m, len); t->state = ST_WAIT_FIN; }
            break;
        case ST_WAIT_FIN:
            if (type != HS_FINISHED) return fail(t, MINT_TLS_ERR_PROTOCOL);
            transcript_hash(t, th);
            handle_finished(t, m, len, th);
            break;
        case ST12_WAIT_CERT:
            if (type != HS_CERTIFICATE) return fail(t, MINT_TLS_ERR_PROTOCOL);
            mint_hash_update(&t->transcript, m, len);
            if (handle_certificate12(t, m, len) == MINT_TLS_AGAIN) t->state = ST12_WAIT_SKE;
            break;
        case ST12_WAIT_SKE:
            if (type != HS_SERVER_KEY_EXCHANGE) return fail(t, MINT_TLS_ERR_PROTOCOL);   /* ECDHE is required */
            mint_hash_update(&t->transcript, m, len);
            if (handle_ske12(t, m, len) == MINT_TLS_AGAIN) t->state = ST12_WAIT_SHD;
            break;
        case ST12_WAIT_SHD:
            mint_hash_update(&t->transcript, m, len);
            if (type == HS_CERTIFICATE_REQUEST) { t->cert_requested = 1; break; }
            if (type != HS_SERVER_HELLO_DONE || len != 4) return fail(t, MINT_TLS_ERR_PROTOCOL);
            client_flight12(t);
            break;
        case ST12_WAIT_CCS:
            if (type != HS_NEW_SESSION_TICKET) return fail(t, MINT_TLS_ERR_PROTOCOL);
            mint_hash_update(&t->transcript, m, len);
            break;
        case ST12_WAIT_FIN:
            if (type != HS_FINISHED) return fail(t, MINT_TLS_ERR_PROTOCOL);
            handle_finished12(t, m, len);
            break;
        case ST_DONE:
            if (t->version == 0x0303) {
                if (type == HS_HELLO_REQUEST) break;                  /* renegotiation is declined by ignoring it */
                return fail(t, MINT_TLS_ERR_PROTOCOL);
            }
            if (type == HS_KEY_UPDATE && len == 5) {
                update_traffic_secret(t, &t->rd);
                if (m[4] == 1) {                                      /* update_requested: answer, then rekey */
                    uint8_t ku[5] = {HS_KEY_UPDATE, 0, 0, 1, 0};
                    if (!queue_record(t, CT_HANDSHAKE, ku, 5)) return fail(t, MINT_TLS_ERR_PROTOCOL);
                    update_traffic_secret(t, &t->wr);
                }
            } else if (type != HS_NEW_SESSION_TICKET) return fail(t, MINT_TLS_ERR_PROTOCOL);
            break;
        default:
            return fail(t, MINT_TLS_ERR_PROTOCOL);
        }
        off += len;
    }
    for (size_t i = off; i < t->hs_len; ++i) t->hs[i - off] = t->hs[i];
    t->hs_len -= off;
    return t->state == ST_FAILED ? t->error : MINT_TLS_AGAIN;
}

/* Reads and processes at most one record. Returns 1 if one was processed,
 * 0 if more bytes are needed, or a negative error. */
static int read_record(MINT_TLS *t) {
    if (t->rlen < 5u) {
        int64_t n = t->io.recv(t->io.ctx, t->rbuf + t->rlen, sizeof(t->rbuf) - t->rlen);
        if (n < 0) { t->peer_closed = 1; return t->state == ST_DONE ? 0 : fail(t, MINT_TLS_ERR_IO); }
        t->rlen += (size_t)n;
        if (t->rlen < 5u) return 0;
    }
    uint8_t type = t->rbuf[0];
    size_t len = g16(t->rbuf + 3);
    if (len > 16384u + 256u || t->rbuf[1] != 3) return fail(t, MINT_TLS_ERR_PROTOCOL);
    if (t->rlen < 5u + len) {
        int64_t n = t->io.recv(t->io.ctx, t->rbuf + t->rlen, sizeof(t->rbuf) - t->rlen);
        if (n < 0) { t->peer_closed = 1; return t->state == ST_DONE ? 0 : fail(t, MINT_TLS_ERR_IO); }
        t->rlen += (size_t)n;
        if (t->rlen < 5u + len) return 0;
    }
    uint8_t *body = t->rbuf + 5;
    size_t plen = len;
    uint8_t inner = type;
    int v12 = t->version == 0x0303;
    if (type == CT_CCS) {
        if (len != 1 || body[0] != 1) return fail(t, MINT_TLS_ERR_PROTOCOL);
        if (v12) {                                                    /* the server switches to its keys */
            if (t->state != ST12_WAIT_CCS || t->rd.on) return fail(t, MINT_TLS_ERR_PROTOCOL);
            t->rd.on = 1;
            t->state = ST12_WAIT_FIN;
        } else if (t->state == ST_DONE) return fail(t, MINT_TLS_ERR_PROTOCOL);   /* middlebox compatibility, ignored */
        goto consumed;
    }
    if (t->rd.on && v12) {
        uint8_t nonce[12], aad[13];
        size_t explicit_len = t->rd.explicit_nonce ? 8u : 0u;
        if (len < explicit_len + 16u) return fail(t, MINT_TLS_ERR_PROTOCOL);
        plen = len - explicit_len - 16u;
        if (t->rd.explicit_nonce) { for (int i = 0; i < 4; ++i) nonce[i] = t->rd.iv[i]; mcopy(nonce + 4, body, 8); }
        else make_nonce(&t->rd, nonce);
        aad12(&t->rd, type, plen, aad);
        if (!mint_aead_open(&t->rd.aead, nonce, aad, 13, body + explicit_len, plen, body + explicit_len + plen, body)) return fail(t, MINT_TLS_ERR_DECRYPT);
        ++t->rd.seq;
    } else if (t->rd.on) {
        if (type != CT_APP || len < 17u) return fail(t, MINT_TLS_ERR_PROTOCOL);
        uint8_t nonce[12];
        make_nonce(&t->rd, nonce);
        if (!mint_aead_open(&t->rd.aead, nonce, t->rbuf, 5, body, len - 16u, body + len - 16u, body)) return fail(t, MINT_TLS_ERR_DECRYPT);
        ++t->rd.seq;
        plen = len - 16u;
        while (plen && !body[plen - 1]) --plen;                         /* strip padding */
        if (!plen) return fail(t, MINT_TLS_ERR_PROTOCOL);
        inner = body[--plen];
    } else if (type != CT_HANDSHAKE && type != CT_ALERT) return fail(t, MINT_TLS_ERR_PROTOCOL);
    if (inner == CT_ALERT) {
        if (plen < 2) return fail(t, MINT_TLS_ERR_PROTOCOL);
        t->alert = body[1];
        if (body[1] == 0) { t->peer_closed = 1; goto consumed; }     /* close_notify */
        if (v12 && body[0] == 1 && body[1] == 100) goto consumed;     /* no_renegotiation warning */
        return fail(t, MINT_TLS_ERR_ALERT);
    }
    if (inner == CT_HANDSHAKE) {
        if (t->hs_len + plen > sizeof(t->hs)) return fail(t, MINT_TLS_ERR_PROTOCOL);
        mcopy(t->hs + t->hs_len, body, plen);
        t->hs_len += plen;
        int rc = process_handshake(t);
        if (rc < 0) return rc;
    } else if (inner == CT_APP) {
        if (t->state != ST_DONE || t->plain_len) return fail(t, MINT_TLS_ERR_PROTOCOL);
        mcopy(t->plain, body, plen);
        t->plain_off = 0; t->plain_len = plen;
    } else return fail(t, MINT_TLS_ERR_PROTOCOL);
consumed:
    for (size_t i = 5u + len; i < t->rlen; ++i) t->rbuf[i - 5u - len] = t->rbuf[i];
    t->rlen -= 5u + len;
    return 1;
}

/* ------------------------------------------------------------------ API */
void mint_tls_init(MINT_TLS *t, const MINT_TLS_IO *io, const char *host, int64_t now) {
    mzero(t, sizeof(*t));
    t->io = *io;
    size_t i = 0;
    for (; host && host[i] && i + 1u < sizeof(t->host); ++i) t->host[i] = host[i];
    t->host[i] = '\0';
    t->now = now;
    t->roots = mint_tls_roots; t->root_count = mint_tls_root_count;
    static const uint16_t suites[9] = {SUITE_CHACHA, SUITE_AES128, SUITE_AES256, S12_ECDSA_CHACHA, S12_RSA_CHACHA,
                                       S12_ECDSA_AES128, S12_RSA_AES128, S12_ECDSA_AES256, S12_RSA_AES256};
    for (uint32_t k = 0; k < 9; ++k) t->suites[k] = suites[k];
    t->suite_count = 9;
    mint_random_bytes(t->priv, 32);
    mint_random_bytes(t->session_id, 32);
    mint_random_bytes(t->client_random, 32);
    mint_x25519_base(t->pub, t->priv);
    t->state = ST_START;
}

int mint_tls_step(MINT_TLS *t) {
    if (t->state == ST_FAILED) return t->error;
    if (t->state == ST_START) send_client_hello(t);
    int rc = flush_out(t);
    if (rc < 0) return rc;
    for (int budget = 0; budget < 64 && t->state != ST_DONE && t->state != ST_FAILED; ++budget) {
        int r = read_record(t);
        if (r < 0) return r;
        if (r == 0) break;
        rc = flush_out(t);
        if (rc < 0) return rc;
    }
    if (t->state == ST_FAILED) return t->error;
    rc = flush_out(t);
    if (rc < 0) return rc;
    return t->state == ST_DONE && rc == MINT_TLS_DONE ? MINT_TLS_DONE : MINT_TLS_AGAIN;
}

int64_t mint_tls_read(MINT_TLS *t, void *out, size_t cap) {
    if (t->state == ST_FAILED) return t->error;
    if (t->state != ST_DONE) return MINT_TLS_WOULD_BLOCK;
    for (int budget = 0; budget < 64 && !t->plain_len && !t->peer_closed; ++budget) {
        int r = read_record(t);
        if (r < 0) return r;
        if (r == 0) break;
    }
    (void)flush_out(t);                                               /* KeyUpdate answers */
    if (t->plain_len) {
        size_t n = t->plain_len < cap ? t->plain_len : cap;
        mcopy(out, t->plain + t->plain_off, n);
        t->plain_off += n; t->plain_len -= n;
        return (int64_t)n;
    }
    return t->peer_closed ? 0 : MINT_TLS_WOULD_BLOCK;
}

int64_t mint_tls_write(MINT_TLS *t, const void *data, size_t len) {
    if (t->state == ST_FAILED) return t->error;
    if (t->state != ST_DONE) return 0;
    const uint8_t *p = (const uint8_t *)data;
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < 16384u ? len - done : 16384u;
        if (!queue_record(t, CT_APP, p + done, n)) break;
        done += n;
        if (flush_out(t) < 0) return t->error;
    }
    if (flush_out(t) < 0) return t->error;
    return (int64_t)done;
}

int mint_tls_flush(MINT_TLS *t) {
    if (t->state == ST_FAILED) return t->error;
    return flush_out(t);
}

void mint_tls_close(MINT_TLS *t) {
    if (t->state == ST_DONE) {
        uint8_t alert[2] = {1, 0};
        (void)queue_record(t, CT_ALERT, alert, 2);
        (void)flush_out(t);
    }
    mzero(&t->rd, sizeof(t->rd)); mzero(&t->wr, sizeof(t->wr));
    mzero(t->hs_secret, sizeof(t->hs_secret)); mzero(t->master, sizeof(t->master)); mzero(t->master12, sizeof(t->master12));
    mzero(t->c_hs, sizeof(t->c_hs)); mzero(t->s_hs, sizeof(t->s_hs)); mzero(t->priv, sizeof(t->priv));
    mzero(t->p256_priv, sizeof(t->p256_priv)); mzero(t->premaster, sizeof(t->premaster));
    if (t->state != ST_FAILED) t->state = ST_FAILED, t->error = MINT_TLS_ERR_IO;
}

const char *mint_tls_suite_name(uint16_t suite) {
    switch (suite) {
    case SUITE_CHACHA: return "TLS_CHACHA20_POLY1305_SHA256";
    case SUITE_AES128: return "TLS_AES_128_GCM_SHA256";
    case SUITE_AES256: return "TLS_AES_256_GCM_SHA384";
    case S12_ECDSA_AES128: return "ECDHE-ECDSA-AES128-GCM-SHA256";
    case S12_ECDSA_AES256: return "ECDHE-ECDSA-AES256-GCM-SHA384";
    case S12_RSA_AES128: return "ECDHE-RSA-AES128-GCM-SHA256";
    case S12_RSA_AES256: return "ECDHE-RSA-AES256-GCM-SHA384";
    case S12_ECDSA_CHACHA: return "ECDHE-ECDSA-CHACHA20-POLY1305";
    case S12_RSA_CHACHA: return "ECDHE-RSA-CHACHA20-POLY1305";
    default: return "none";
    }
}

const char *mint_tls_version_name(const MINT_TLS *t) {
    return t->version == 0x0304 ? "TLS 1.3" : t->version == 0x0303 ? "TLS 1.2" : "TLS";
}

const char *mint_tls_error_text(const MINT_TLS *t) {
    switch (t->error) {
    case MINT_TLS_ERR_CERT: return mint_cert_error_text(t->cert_error);
    case MINT_TLS_ERR_IO: return "the connection was closed during the secure handshake";
    case MINT_TLS_ERR_ALERT:
        switch (t->alert) {
        case 40: return t->hash_len ? "the server shares no cipher or signature algorithm with MintOS"
                                    : "the server offers no TLS 1.3/1.2 cipher suite MintOS supports (MintOS needs ECDHE with AES-GCM or ChaCha20)";
        case 70: return "the server only speaks an old TLS version (MintOS needs TLS 1.2 or 1.3)";
        case 112: return "the server does not recognise this host name";
        case 42: case 43: case 44: case 45: case 46: return "the server rejected the connection's certificate handling";
        default: return "the server refused the secure connection";
        }
    case MINT_TLS_ERR_UNSUPPORTED: return "the server needs a TLS feature MintOS does not support yet";
    case MINT_TLS_ERR_DECRYPT: return "a secure record failed authentication";
    case MINT_TLS_ERR_PROTOCOL: return "the server sent an unexpected TLS message";
    default: return "secure connection error";
    }
}
