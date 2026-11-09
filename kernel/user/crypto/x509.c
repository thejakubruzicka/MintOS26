/* X.509 v3 certificate parsing and web-PKI chain validation for TLS.
 * Supports RSA (PKCS#1 v1.5 SHA-256/384/512) and ECDSA (P-256/P-384 with
 * SHA-256/384/512) certificate signatures; validity dates; basicConstraints;
 * subjectAltName dNSName/iPAddress matching with single-label wildcards.
 * Not checked: revocation (CRL/OCSP), name constraints, policy, key usage. */
#include "x509.h"

static int tlv(const uint8_t *p, size_t len, size_t at, uint8_t *tag, size_t *start, size_t *clen) {
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
    *start = at + h; *clen = l;
    return 1;
}
static int eq(const uint8_t *a, size_t al, const uint8_t *b, size_t bl) {
    if (al != bl) return 0;
    for (size_t i = 0; i < al; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static int parse_time(uint8_t tag, const uint8_t *s, size_t len, int64_t *out) {
    int v[7] = {0}; size_t i = 0;
    int64_t year;
    if (tag == 0x17 && len == 13) { v[0] = (s[0] - '0') * 10 + (s[1] - '0'); year = v[0] < 50 ? 2000 + v[0] : 1900 + v[0]; i = 2; }
    else if (tag == 0x18 && len == 15) { year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0'); i = 4; }
    else return 0;
    if (s[len - 1] != 'Z') return 0;
    for (size_t k = 0; k < len - 1; ++k) if (s[k] < '0' || s[k] > '9') return 0;
    int mo = (s[i] - '0') * 10 + (s[i + 1] - '0'), d = (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
    int h = (s[i + 4] - '0') * 10 + (s[i + 5] - '0'), mi = (s[i + 6] - '0') * 10 + (s[i + 7] - '0'), se = (s[i + 8] - '0') * 10 + (s[i + 9] - '0');
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return 0;
    *out = days_from_civil(year, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    return 1;
}
static uint32_t sig_scheme(const uint8_t *oid, size_t len) {
    static const uint8_t RSA_PREFIX[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01};
    static const uint8_t ECDSA_PREFIX[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03};
    if (len == 9 && eq(oid, 8, RSA_PREFIX, 8)) {
        if (oid[8] == 0x0b) return MINT_SIG_RSA_PKCS1_SHA256;
        if (oid[8] == 0x0c) return MINT_SIG_RSA_PKCS1_SHA384;
        if (oid[8] == 0x0d) return MINT_SIG_RSA_PKCS1_SHA512;
    }
    if (len == 8 && eq(oid, 7, ECDSA_PREFIX, 7)) {
        if (oid[7] == 0x02) return MINT_SIG_ECDSA_SHA256;
        if (oid[7] == 0x03) return MINT_SIG_ECDSA_SHA384;
        if (oid[7] == 0x04) return MINT_SIG_ECDSA_SHA512;
    }
    return 0;
}

static void parse_extensions(const uint8_t *p, size_t len, MINT_X509 *c) {
    static const uint8_t SAN[] = {0x55, 0x1d, 0x11}, BC[] = {0x55, 0x1d, 0x13};
    uint8_t tag; size_t s, l;
    if (!tlv(p, len, 0, &tag, &s, &l) || tag != 0x30) return;
    size_t at = s, end = s + l;
    while (at < end) {
        size_t es, el;
        if (!tlv(p, end, at, &tag, &es, &el) || tag != 0x30) return;
        size_t next = es + el, os, ol;
        if (!tlv(p, next, es, &tag, &os, &ol) || tag != 0x06) return;
        size_t vat = os + ol;
        if (vat < next && p[vat] == 0x01) { size_t bs, bl; if (!tlv(p, next, vat, &tag, &bs, &bl)) return; vat = bs + bl; }  /* critical */
        size_t vs, vl;
        if (!tlv(p, next, vat, &tag, &vs, &vl) || tag != 0x04) return;                 /* OCTET STRING */
        if (eq(p + os, ol, SAN, 3)) {
            size_t gs, gl;
            if (tlv(p, vs + vl, vs, &tag, &gs, &gl) && tag == 0x30) { c->san = p + gs; c->san_len = gl; }
        } else if (eq(p + os, ol, BC, 3)) {
            size_t bs, bl;
            c->has_basic_constraints = 1;
            if (tlv(p, vs + vl, vs, &tag, &bs, &bl) && tag == 0x30) {
                size_t i = bs;
                if (i < bs + bl && p[i] == 0x01) { size_t ts, tl; if (tlv(p, bs + bl, i, &tag, &ts, &tl) && tl == 1) c->is_ca = p[ts] != 0; i = ts + tl; }
                if (i < bs + bl && p[i] == 0x02) { size_t ts, tl; if (tlv(p, bs + bl, i, &tag, &ts, &tl) && tl == 1) c->path_len = p[ts]; }
            }
        }
        at = next;
    }
}

static void find_cn(MINT_X509 *c) {
    static const uint8_t CN[] = {0x55, 0x04, 0x03};
    const uint8_t *p = c->subject; size_t len = c->subject_len;
    uint8_t tag; size_t s, l;
    if (!tlv(p, len, 0, &tag, &s, &l) || tag != 0x30) return;
    for (size_t at = s; at < s + l;) {
        size_t ss, sl; if (!tlv(p, len, at, &tag, &ss, &sl) || tag != 0x31) return;      /* SET */
        size_t as, al; if (!tlv(p, len, ss, &tag, &as, &al) || tag != 0x30) return;      /* AttributeTypeAndValue */
        size_t os, ol; if (!tlv(p, len, as, &tag, &os, &ol) || tag != 0x06) return;
        size_t vs, vl; if (!tlv(p, len, os + ol, &tag, &vs, &vl)) return;
        if (eq(p + os, ol, CN, 3)) { c->cn = p + vs; c->cn_len = vl; }
        at = ss + sl;
    }
}

int mint_x509_parse(const uint8_t *der, size_t len, MINT_X509 *c) {
    uint8_t tag; size_t s, l;
    for (size_t i = 0; i < sizeof(*c); ++i) ((uint8_t *)c)[i] = 0;
    c->path_len = -1;
    if (!tlv(der, len, 0, &tag, &s, &l) || tag != 0x30) return 0;
    size_t end = s + l, ts, tl;
    if (!tlv(der, end, s, &tag, &ts, &tl) || tag != 0x30) return 0;                    /* tbsCertificate */
    c->tbs = der + s; c->tbs_len = (ts + tl) - s;
    size_t at = ts, tend = ts + tl, fs, fl;
    if (!tlv(der, tend, at, &tag, &fs, &fl)) return 0;
    if (tag == 0xA0) { at = fs + fl; if (!tlv(der, tend, at, &tag, &fs, &fl)) return 0; }   /* version */
    if (tag != 0x02) return 0; at = fs + fl;                                             /* serialNumber */
    if (!tlv(der, tend, at, &tag, &fs, &fl) || tag != 0x30) return 0; at = fs + fl;      /* signature alg (inner) */
    if (!tlv(der, tend, at, &tag, &fs, &fl) || tag != 0x30) return 0;                    /* issuer */
    c->issuer = der + at; c->issuer_len = fs + fl - at; at = fs + fl;
    if (!tlv(der, tend, at, &tag, &fs, &fl) || tag != 0x30) return 0;                    /* validity */
    size_t v1s, v1l, v2s, v2l; uint8_t t1, t2;
    if (!tlv(der, fs + fl, fs, &t1, &v1s, &v1l) || !tlv(der, fs + fl, v1s + v1l, &t2, &v2s, &v2l)) return 0;
    if (!parse_time(t1, der + v1s, v1l, &c->not_before) || !parse_time(t2, der + v2s, v2l, &c->not_after)) return 0;
    at = fs + fl;
    if (!tlv(der, tend, at, &tag, &fs, &fl) || tag != 0x30) return 0;                    /* subject */
    c->subject = der + at; c->subject_len = fs + fl - at; at = fs + fl;
    if (!tlv(der, tend, at, &tag, &fs, &fl) || tag != 0x30) return 0;                    /* subjectPublicKeyInfo */
    c->spki = der + at; c->spki_len = fs + fl - at; at = fs + fl;
    while (at < tend) {
        if (!tlv(der, tend, at, &tag, &fs, &fl)) return 0;
        if (tag == 0xA3) parse_extensions(der + fs, fl, c);
        at = fs + fl;
    }
    at = tend;
    size_t as, al, os, ol;
    if (!tlv(der, end, at, &tag, &as, &al) || tag != 0x30) return 0;                     /* signatureAlgorithm */
    if (!tlv(der, as + al, as, &tag, &os, &ol) || tag != 0x06) return 0;
    c->sig_scheme = sig_scheme(der + os, ol);
    at = as + al;
    if (!tlv(der, end, at, &tag, &fs, &fl) || tag != 0x03 || fl < 2 || der[fs] != 0) return 0;
    c->sig = der + fs + 1; c->sig_len = fl - 1;
    find_cn(c);
    return 1;
}

static char lc(char ch) { return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch; }
static int dns_match(const uint8_t *pat, size_t plen, const char *host) {
    size_t hlen = 0; while (host[hlen]) ++hlen;
    if (plen >= 2 && pat[0] == '*' && pat[1] == '.') {
        size_t dot = 0;
        while (dot < hlen && host[dot] != '.') ++dot;
        if (!dot || dot == hlen) return 0;                 /* wildcard needs a non-empty first label */
        if (hlen - dot != plen - 1) return 0;
        for (size_t i = 0; i < plen - 1; ++i) if (lc(host[dot + i]) != lc((char)pat[1 + i])) return 0;
        return 1;
    }
    if (plen != hlen) return 0;
    for (size_t i = 0; i < plen; ++i) if (lc(host[i]) != lc((char)pat[i])) return 0;
    return 1;
}
static int ipv4_text(const char *host, uint8_t ip[4]) {
    uint32_t n = 0, v = 0, digits = 0;
    for (const char *p = host;; ++p) {
        if (*p >= '0' && *p <= '9') { v = v * 10u + (uint32_t)(*p - '0'); if (++digits > 3 || v > 255) return 0; continue; }
        if ((*p == '.' || !*p) && digits) { if (n >= 4) return 0; ip[n++] = (uint8_t)v; v = 0; digits = 0; if (!*p) break; continue; }
        return 0;
    }
    return n == 4;
}
int mint_x509_matches_host(const MINT_X509 *c, const char *host) {
    uint8_t ip[4];
    int is_ip = ipv4_text(host, ip);
    if (c->san) {
        uint8_t tag; size_t s, l;
        for (size_t at = 0; at < c->san_len; at = s + l) {
            if (!tlv(c->san, c->san_len, at, &tag, &s, &l)) return 0;
            if (!is_ip && tag == 0x82 && dns_match(c->san + s, l, host)) return 1;
            if (is_ip && tag == 0x87 && l == 4 && eq(c->san + s, 4, ip, 4)) return 1;
        }
        return 0;
    }
    return !is_ip && c->cn && dns_match(c->cn, c->cn_len, host);
}

static int check_signature(const MINT_X509 *child, const uint8_t *issuer_spki, size_t issuer_spki_len) {
    MINT_PUBKEY key;
    if (!child->sig_scheme) return MINT_CERT_UNSUPPORTED;
    if (!mint_spki_parse(issuer_spki, issuer_spki_len, &key)) return MINT_CERT_UNSUPPORTED;
    return mint_signature_verify(&key, child->sig_scheme, child->tbs, child->tbs_len, child->sig, child->sig_len)
           ? MINT_CERT_OK : MINT_CERT_BAD_SIGNATURE;
}

int mint_x509_verify_chain(const uint8_t *const *certs, const size_t *lens, size_t count, const char *host,
                           int64_t now, const MINT_TLS_ROOT *roots, uint32_t root_count) {
    static MINT_X509 parsed[8];
    if (!count) return MINT_CERT_MALFORMED;
    if (count > 8) count = 8;
    for (size_t i = 0; i < count; ++i) if (!mint_x509_parse(certs[i], lens[i], &parsed[i])) return MINT_CERT_MALFORMED;
    if (host && !mint_x509_matches_host(&parsed[0], host)) return MINT_CERT_WRONG_HOST;
    size_t cur = 0;
    uint32_t used = 1u;
    for (uint32_t depth = 0; depth < 8; ++depth) {
        const MINT_X509 *c = &parsed[cur];
        if (now) {
            if (now < c->not_before) return MINT_CERT_NOT_YET_VALID;
            if (now > c->not_after) return MINT_CERT_EXPIRED;
        }
        if (depth > 0 && (!c->has_basic_constraints || !c->is_ca)) return MINT_CERT_NOT_CA;
        if (depth > 1 && c->path_len >= 0 && (int)depth - 1 > c->path_len) return MINT_CERT_NOT_CA;
        /* Trusted root that issued this certificate? */
        int best = MINT_CERT_UNTRUSTED;
        for (uint32_t r = 0; r < root_count; ++r) {
            if (!eq(roots[r].subject, roots[r].subject_len, c->issuer, c->issuer_len)) continue;
            int rc = check_signature(c, roots[r].spki, roots[r].spki_len);
            if (rc == MINT_CERT_OK) return MINT_CERT_OK;
            best = rc;
        }
        /* The server may also send the root itself. */
        for (uint32_t r = 0; r < root_count; ++r)
            if (eq(roots[r].subject, roots[r].subject_len, c->subject, c->subject_len) &&
                eq(roots[r].spki, roots[r].spki_len, c->spki, c->spki_len)) return MINT_CERT_OK;
        /* Otherwise continue with an unused intermediate from the server. */
        size_t next = count;
        for (size_t i = 1; i < count; ++i) {
            if ((used >> i) & 1u) continue;
            if (!eq(parsed[i].subject, parsed[i].subject_len, c->issuer, c->issuer_len)) continue;
            int rc = check_signature(c, parsed[i].spki, parsed[i].spki_len);
            if (rc == MINT_CERT_OK) { next = i; break; }
            best = rc;
        }
        if (next == count) return best;
        used |= 1u << next;
        cur = next;
    }
    return MINT_CERT_UNTRUSTED;
}

const char *mint_cert_error_text(int code) {
    switch (code) {
    case MINT_CERT_OK: return "valid";
    case MINT_CERT_MALFORMED: return "the certificate could not be read";
    case MINT_CERT_EXPIRED: return "the certificate has expired";
    case MINT_CERT_NOT_YET_VALID: return "the certificate is not valid yet (check the clock)";
    case MINT_CERT_WRONG_HOST: return "the certificate belongs to a different site";
    case MINT_CERT_UNTRUSTED: return "the certificate was not issued by a trusted authority";
    case MINT_CERT_BAD_SIGNATURE: return "a certificate signature is invalid";
    case MINT_CERT_UNSUPPORTED: return "the certificate uses an unsupported algorithm";
    case MINT_CERT_NOT_CA: return "an intermediate certificate is not a certificate authority";
    default: return "certificate error";
    }
}
