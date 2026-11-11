/* DEFLATE / zlib / gzip decoder — see inflate.h. Canonical Huffman codes are
 * decoded with a 9-bit lookup table and a bit-serial fallback for longer codes
 * (the structure follows Mark Adler's public-domain puff.c). */
#include "inflate.h"
#include "wmem.h"

#define MAXBITS 15
#define FAST_BITS 9
#define MAXLCODES 286
#define MAXDCODES 30

typedef struct huff {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[288];
    uint16_t fast[1u << FAST_BITS];     /* (symbol << 4) | length, 0 = use the slow path */
} huff;

typedef struct state {
    const uint8_t *in; size_t in_len, pos;
    uint64_t bitbuf; uint32_t bitcnt;
    uint8_t *out; size_t out_len, out_cap, out_max;
    int err;
} state;

static int fill(state *s, uint32_t need) {
    while (s->bitcnt < need) {
        if (s->pos >= s->in_len) return 0;
        s->bitbuf |= (uint64_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    return 1;
}
static int bits(state *s, uint32_t need, uint32_t *v) {
    if (!need) { *v = 0; return 1; }
    if (!fill(s, need)) { s->err = WINF_ERR_TRUNCATED; return 0; }
    *v = (uint32_t)(s->bitbuf & ((1ull << need) - 1u));
    s->bitbuf >>= need; s->bitcnt -= need;
    return 1;
}

static int out_reserve(state *s, size_t more) {
    if (s->out_len + more <= s->out_cap) return 1;
    if (s->out_len + more > s->out_max) { s->err = WINF_ERR_TOO_BIG; return 0; }
    size_t cap = s->out_cap ? s->out_cap : 4096u;
    while (cap < s->out_len + more) cap = cap > s->out_max / 2u ? s->out_max : cap * 2u;
    if (cap < s->out_len + more) cap = s->out_len + more;
    uint8_t *n = (uint8_t *)wmem_realloc(s->out, cap);
    if (!n) { s->err = WINF_ERR_MEMORY; return 0; }
    s->out = n; s->out_cap = cap;
    return 1;
}

static uint32_t reverse_bits(uint32_t code, uint32_t len) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < len; ++i) { r = (r << 1) | (code & 1u); code >>= 1; }
    return r;
}

/* Returns 0 for a complete code, >0 for an incomplete one (allowed only for
 * single-code distance trees), <0 for an over-subscribed set. */
static int construct(huff *h, const uint16_t *length, uint32_t n) {
    for (uint32_t len = 0; len <= MAXBITS; ++len) h->count[len] = 0;
    for (uint32_t sym = 0; sym < n; ++sym) h->count[length[sym]]++;
    for (uint32_t i = 0; i < (1u << FAST_BITS); ++i) h->fast[i] = 0;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (uint32_t len = 1; len <= MAXBITS; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    uint16_t offs[MAXBITS + 1];
    offs[1] = 0;
    for (uint32_t len = 1; len < MAXBITS; ++len) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (uint32_t sym = 0; sym < n; ++sym) if (length[sym]) h->symbol[offs[length[sym]]++] = (uint16_t)sym;
    /* fast table: walk the canonical codes in order */
    uint32_t code = 0, index = 0;
    for (uint32_t len = 1; len <= FAST_BITS; ++len) {
        for (uint32_t k = 0; k < h->count[len]; ++k, ++index, ++code) {
            uint32_t rev = reverse_bits(code, len);
            for (uint32_t fill_i = rev; fill_i < (1u << FAST_BITS); fill_i += 1u << len)
                h->fast[fill_i] = (uint16_t)((h->symbol[index] << 4) | len);
        }
        code <<= 1;
    }
    return left;
}

static int decode(state *s, const huff *h) {
    (void)fill(s, FAST_BITS);                       /* may come up short at the very end */
    uint16_t e = h->fast[s->bitbuf & ((1u << FAST_BITS) - 1u)];
    if (e && (e & 15u) <= s->bitcnt) {
        s->bitbuf >>= e & 15u; s->bitcnt -= e & 15u;
        return e >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (uint32_t len = 1; len <= MAXBITS; ++len) {
        uint32_t b;
        if (!bits(s, 1, &b)) return -1;
        code |= (int)b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count;
        first <<= 1; code <<= 1;
    }
    s->err = WINF_ERR_DATA;
    return -1;
}

static const uint16_t LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint16_t LEXT[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint16_t DEXT[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int codes(state *s, const huff *lencode, const huff *distcode) {
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) { if (!s->err) s->err = WINF_ERR_DATA; return 0; }
        if (sym < 256) {
            if (!out_reserve(s, 1)) return 0;
            s->out[s->out_len++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            sym -= 257;
            if (sym >= 29) { s->err = WINF_ERR_DATA; return 0; }
            uint32_t extra;
            if (!bits(s, LEXT[sym], &extra)) return 0;
            uint32_t len = LBASE[sym] + extra;
            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30) { if (!s->err) s->err = WINF_ERR_DATA; return 0; }
            if (!bits(s, DEXT[dsym], &extra)) return 0;
            size_t dist = DBASE[dsym] + extra;
            if (dist > s->out_len) { s->err = WINF_ERR_DATA; return 0; }
            if (!out_reserve(s, len)) return 0;
            uint8_t *o = s->out + s->out_len;
            const uint8_t *from = o - dist;
            for (uint32_t i = 0; i < len; ++i) o[i] = from[i];
            s->out_len += len;
        }
    }
}

static int stored(state *s) {
    s->bitbuf >>= s->bitcnt & 7u; s->bitcnt -= s->bitcnt & 7u;   /* byte align */
    uint32_t len, nlen;
    if (!bits(s, 16, &len) || !bits(s, 16, &nlen)) return 0;
    if (len != (~nlen & 0xFFFFu)) { s->err = WINF_ERR_DATA; return 0; }
    if (!out_reserve(s, len)) return 0;
    while (len && s->bitcnt >= 8) {                              /* bytes still in the bit buffer */
        s->out[s->out_len++] = (uint8_t)s->bitbuf;
        s->bitbuf >>= 8; s->bitcnt -= 8; --len;
    }
    if (s->in_len - s->pos < len) { s->err = WINF_ERR_TRUNCATED; return 0; }
    wb_copy(s->out + s->out_len, s->in + s->pos, len);
    s->out_len += len; s->pos += len;
    return 1;
}

static int fixed(state *s) {
    static huff lencode, distcode;
    static int built;
    if (!built) {
        uint16_t lengths[288];
        uint32_t sym = 0;
        for (; sym < 144; ++sym) lengths[sym] = 8;
        for (; sym < 256; ++sym) lengths[sym] = 9;
        for (; sym < 280; ++sym) lengths[sym] = 7;
        for (; sym < 288; ++sym) lengths[sym] = 8;
        (void)construct(&lencode, lengths, 288);
        for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
        (void)construct(&distcode, lengths, 30);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(state *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    static huff lencode, distcode;
    uint16_t lengths[MAXLCODES + MAXDCODES];
    uint32_t nlen, ndist, ncode;
    if (!bits(s, 5, &nlen) || !bits(s, 5, &ndist) || !bits(s, 4, &ncode)) return 0;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) { s->err = WINF_ERR_DATA; return 0; }
    uint32_t index = 0;
    for (; index < ncode; ++index) { uint32_t v; if (!bits(s, 3, &v)) return 0; lengths[order[index]] = (uint16_t)v; }
    for (; index < 19; ++index) lengths[order[index]] = 0;
    if (construct(&lencode, lengths, 19) != 0) { s->err = WINF_ERR_DATA; return 0; }
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (sym < 0) { if (!s->err) s->err = WINF_ERR_DATA; return 0; }
        if (sym < 16) { lengths[index++] = (uint16_t)sym; continue; }
        uint16_t len = 0;
        uint32_t rep;
        if (sym == 16) {
            if (!index) { s->err = WINF_ERR_DATA; return 0; }
            len = lengths[index - 1];
            if (!bits(s, 2, &rep)) return 0;
            rep += 3;
        } else if (sym == 17) { if (!bits(s, 3, &rep)) return 0; rep += 3; }
        else { if (!bits(s, 7, &rep)) return 0; rep += 11; }
        if (index + rep > nlen + ndist) { s->err = WINF_ERR_DATA; return 0; }
        while (rep--) lengths[index++] = len;
    }
    if (!lengths[256]) { s->err = WINF_ERR_DATA; return 0; }
    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) { s->err = WINF_ERR_DATA; return 0; }
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) { s->err = WINF_ERR_DATA; return 0; }
    return codes(s, &lencode, &distcode);
}

static int inflate_raw(state *s) {
    uint32_t last, type;
    do {
        if (!bits(s, 1, &last) || !bits(s, 2, &type)) return 0;
        int ok = type == 0 ? stored(s) : type == 1 ? fixed(s) : type == 2 ? dynamic(s) : (s->err = WINF_ERR_DATA, 0);
        if (!ok) { if (!s->err) s->err = WINF_ERR_DATA; return 0; }
    } while (!last);
    /* return unused whole bytes to the input */
    while (s->bitcnt >= 8) { s->bitcnt -= 8; --s->pos; }
    s->bitbuf = 0; s->bitcnt = 0;
    return 1;
}

uint32_t winf_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    static int built;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        built = 1;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

uint32_t winf_adler32(uint32_t a, const uint8_t *p, size_t n) {
    uint32_t s1 = a & 0xFFFFu, s2 = a >> 16;
    while (n) {
        size_t k = n < 5552u ? n : 5552u;
        n -= k;
        while (k--) { s1 += *p++; s2 += s1; }
        s1 %= 65521u; s2 %= 65521u;
    }
    return (s2 << 16) | s1;
}

static int64_t finish(state *s, uint8_t **out) {
    if (s->err) { wmem_free(s->out); *out = 0; return s->err; }
    if (!s->out) { s->out = (uint8_t *)wmem_alloc(1); if (!s->out) return WINF_ERR_MEMORY; }
    *out = s->out;
    return (int64_t)s->out_len;
}

int64_t winf_decode(const uint8_t *in, size_t in_len, int format, size_t max_out, uint8_t **out) {
    state s;
    wb_set(&s, 0, sizeof(s));
    s.in = in; s.in_len = in_len; s.out_max = max_out;
    *out = 0;
    if (format == WINF_AUTO_DEFLATE) {
        int zlib = in_len >= 2 && (in[0] & 0x0Fu) == 8 && (in[0] >> 4) <= 7 && ((in[0] << 8) | in[1]) % 31 == 0;
        format = zlib ? WINF_ZLIB : WINF_RAW;
    }
    if (format == WINF_ZLIB) {
        if (in_len < 6 || (in[0] & 0x0Fu) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20u)) return WINF_ERR_HEADER;
        s.pos = 2;
        if (!inflate_raw(&s)) return finish(&s, out);
        if (s.in_len - s.pos < 4) { s.err = WINF_ERR_TRUNCATED; return finish(&s, out); }
        uint32_t want = ((uint32_t)in[s.pos] << 24) | ((uint32_t)in[s.pos + 1] << 16) | ((uint32_t)in[s.pos + 2] << 8) | in[s.pos + 3];
        if (winf_adler32(1, s.out, s.out_len) != want) s.err = WINF_ERR_CHECKSUM;
        return finish(&s, out);
    }
    if (format == WINF_GZIP) {
        /* concatenated members are decoded one after another */
        while (s.pos < in_len) {
            size_t p = s.pos;
            if (in_len - p < 18 || in[p] != 0x1F || in[p + 1] != 0x8B || in[p + 2] != 8) {
                if (p && s.out_len) break;                                 /* trailing garbage after a member */
                return WINF_ERR_HEADER;
            }
            uint8_t flags = in[p + 3];
            p += 10;
            if (flags & 4u) {                                              /* FEXTRA */
                if (in_len - p < 2) return WINF_ERR_TRUNCATED;
                size_t xlen = in[p] | ((size_t)in[p + 1] << 8);
                p += 2;
                if (in_len - p < xlen) return WINF_ERR_TRUNCATED;
                p += xlen;
            }
            for (int field = 8; field <= 16; field <<= 1) {                /* FNAME, FCOMMENT */
                if (!(flags & field)) continue;
                while (p < in_len && in[p]) ++p;
                if (p >= in_len) return WINF_ERR_TRUNCATED;
                ++p;
            }
            if (flags & 2u) p += 2;                                        /* FHCRC */
            if (p > in_len) return WINF_ERR_TRUNCATED;
            size_t start = s.out_len;
            s.pos = p;
            if (!inflate_raw(&s)) return finish(&s, out);
            if (s.in_len - s.pos < 8) { s.err = WINF_ERR_TRUNCATED; return finish(&s, out); }
            const uint8_t *t = in + s.pos;
            uint32_t crc = t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
            uint32_t isize = t[4] | ((uint32_t)t[5] << 8) | ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
            if (winf_crc32(0, s.out + start, s.out_len - start) != crc || (uint32_t)(s.out_len - start) != isize) {
                s.err = WINF_ERR_CHECKSUM; return finish(&s, out);
            }
            s.pos += 8;
        }
        return finish(&s, out);
    }
    (void)inflate_raw(&s);
    return finish(&s, out);
}

const char *winf_error_text(int64_t code) {
    switch (code) {
    case WINF_ERR_DATA: return "the compressed data is corrupt";
    case WINF_ERR_TRUNCATED: return "the compressed data ends too early";
    case WINF_ERR_TOO_BIG: return "the page is too large once decompressed";
    case WINF_ERR_MEMORY: return "not enough memory to decompress the page";
    case WINF_ERR_CHECKSUM: return "the compressed data failed its checksum";
    case WINF_ERR_HEADER: return "the compression header is invalid";
    default: return "decompression error";
    }
}
