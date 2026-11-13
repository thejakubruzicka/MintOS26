/* HTML parsing for the MintOS Web engine — see wdoc.h.
 *
 * A single-pass tokenizer feeds a tree builder that follows the HTML5 error
 * recovery rules that matter for real pages: implied html/head/body, implied
 * end tags (p, li, dt/dd, option, table rows and cells), void and raw-text
 * elements, foster parenting of stray content in tables, and end tags that
 * only close elements in scope. Scripts are never run; <noscript> content is
 * shown. All nodes, attributes and text live in the document arena. */
#include "wdoc.h"
#include "whttp.h"

#define MAX_DEPTH 400u
#define MAX_NODES 300000u
#define MAX_ATTRS 48u

static const char *const tag_names[WT_COUNT] = {
    [WT_NONE] = "", [WT_TEXT] = "#text", [WT_DOCUMENT] = "#document", [WT_UNKNOWN] = "",
#define WT_NAME(id, s) [WT_##id] = s,
    WTAG_LIST(WT_NAME)
#undef WT_NAME
};

const char *wtag_name(uint32_t tag) { return tag < WT_COUNT ? tag_names[tag] : ""; }

uint32_t wtag_lookup(const char *name, size_t len) {
    /* binary search: the X-macro list is in alphabetical order */
    uint32_t lo = WT_UNKNOWN + 1, hi = WT_COUNT - 1;
    while (lo <= hi) {
        uint32_t mid = (lo + hi) / 2;
        const char *t = tag_names[mid];
        int c = 0;
        size_t i = 0;
        for (; i < len && t[i]; ++i) { if (name[i] != t[i]) { c = (uint8_t)name[i] < (uint8_t)t[i] ? -1 : 1; break; } }
        if (!c) c = i < len ? 1 : t[i] ? -1 : 0;
        if (!c) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return WT_UNKNOWN;
}

/* ------------------------------------------------------------------ UTF-8 */
size_t wutf8_put(char *o, uint32_t cp) {
    if (cp < 0x80u) { o[0] = (char)cp; return 1; }
    if (cp < 0x800u) { o[0] = (char)(0xC0u | (cp >> 6)); o[1] = (char)(0x80u | (cp & 0x3Fu)); return 2; }
    if (cp < 0x10000u) {
        o[0] = (char)(0xE0u | (cp >> 12)); o[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); o[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp > 0x10FFFFu) { o[0] = '?'; return 1; }
    o[0] = (char)(0xF0u | (cp >> 18)); o[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    o[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); o[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

uint32_t wutf8_next(const char **p, const char *end) {
    const uint8_t *s = (const uint8_t *)*p;
    size_t left = (size_t)(end - *p);
    if (!left) return 0;
    uint32_t c = s[0];
    if (c < 0x80u) { *p += 1; return c; }
    if ((c & 0xE0u) == 0xC0u && left >= 2 && (s[1] & 0xC0u) == 0x80u) { *p += 2; return ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu); }
    if ((c & 0xF0u) == 0xE0u && left >= 3 && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        *p += 3; return ((c & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    }
    if ((c & 0xF8u) == 0xF0u && left >= 4 && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u) {
        *p += 4; return ((c & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) | ((uint32_t)(s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
    }
    *p += 1;
    return 0xFFFDu;
}

/* ------------------------------------------------------------------ charsets */
static const uint16_t CP1252_80[32] = {
    0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
    0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178};
static const uint16_t CP1250_80[128] = {
    0x20AC,0x0081,0x201A,0x0083,0x201E,0x2026,0x2020,0x2021,0x0088,0x2030,0x0160,0x2039,0x015A,0x0164,0x017D,0x0179,
    0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x0098,0x2122,0x0161,0x203A,0x015B,0x0165,0x017E,0x017A,
    0x00A0,0x02C7,0x02D8,0x0141,0x00A4,0x0104,0x00A6,0x00A7,0x00A8,0x00A9,0x015E,0x00AB,0x00AC,0x00AD,0x00AE,0x017B,
    0x00B0,0x00B1,0x02DB,0x0142,0x00B4,0x00B5,0x00B6,0x00B7,0x00B8,0x0105,0x015F,0x00BB,0x013D,0x02DD,0x013E,0x017C,
    0x0154,0x00C1,0x00C2,0x0102,0x00C4,0x0139,0x0106,0x00C7,0x010C,0x00C9,0x0118,0x00CB,0x011A,0x00CD,0x00CE,0x010E,
    0x0110,0x0143,0x0147,0x00D3,0x00D4,0x0150,0x00D6,0x00D7,0x0158,0x016E,0x00DA,0x0170,0x00DC,0x00DD,0x0162,0x00DF,
    0x0155,0x00E1,0x00E2,0x0103,0x00E4,0x013A,0x0107,0x00E7,0x010D,0x00E9,0x0119,0x00EB,0x011B,0x00ED,0x00EE,0x010F,
    0x0111,0x0144,0x0148,0x00F3,0x00F4,0x0151,0x00F6,0x00F7,0x0159,0x016F,0x00FA,0x0171,0x00FC,0x00FD,0x0163,0x02D9};
static const uint16_t ISO8859_2_A0[32] = {
    0x00A0,0x0104,0x02D8,0x0141,0x00A4,0x013D,0x015A,0x00A7,0x00A8,0x0160,0x015E,0x0164,0x0179,0x00AD,0x017D,0x017B,
    0x00B0,0x0105,0x02DB,0x0142,0x00B4,0x013E,0x015B,0x02C7,0x00B8,0x0161,0x015F,0x0165,0x017A,0x02DD,0x017E,0x017C};

enum { CS_UTF8 = 0, CS_1252, CS_LATIN1, CS_1250, CS_LATIN2 };
static int charset_id(const char *cs) {
    if (!cs || !cs[0] || wstr_ieq(cs, "utf-8") || wstr_ieq(cs, "utf8") || wstr_ieq(cs, "unicode-1-1-utf-8")) return CS_UTF8;
    if (wstr_ieq(cs, "windows-1250") || wstr_ieq(cs, "cp1250") || wstr_ieq(cs, "x-cp1250")) return CS_1250;
    if (wstr_ieq(cs, "iso-8859-2") || wstr_ieq(cs, "iso8859-2") || wstr_ieq(cs, "latin2") || wstr_ieq(cs, "l2") || wstr_ieq(cs, "iso_8859-2")) return CS_LATIN2;
    /* WHATWG: ISO-8859-1 and US-ASCII labels mean windows-1252 */
    if (wstr_ieq(cs, "windows-1252") || wstr_ieq(cs, "iso-8859-1") || wstr_ieq(cs, "latin1") || wstr_ieq(cs, "us-ascii") ||
        wstr_ieq(cs, "ascii") || wstr_ieq(cs, "cp1252") || wstr_ieq(cs, "iso8859-1") || wstr_ieq(cs, "iso_8859-1") || wstr_ieq(cs, "l1")) return CS_1252;
    return CS_UTF8;
}

static uint32_t legacy_cp(int cs, uint8_t b) {
    if (b < 0x80) return b;
    if (cs == CS_1250) return CP1250_80[b - 0x80];
    if (cs == CS_LATIN2) return b >= 0xC0 ? CP1250_80[b - 0x80] : b >= 0xA0 ? ISO8859_2_A0[b - 0xA0] : b;
    return b < 0xA0 ? CP1252_80[b - 0x80] : b;
}

char *wcharset_to_utf8(const uint8_t *in, size_t len, const char *charset, size_t *out_len) {
    int cs = charset_id(charset);
    size_t start = (len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) ? 3u : 0u;   /* BOM */
    char *out = 0;
    size_t o = 0;
    for (int pass = 0; pass < 2; ++pass) {
        o = 0;
        char tmp[4];
        if (cs == CS_UTF8) {
            const char *p = (const char *)in + start, *end = (const char *)in + len;
            while (p < end) {
                const char *s0 = p;
                uint32_t c = wutf8_next(&p, end);
                if (c == 0xFFFDu && !(p - s0 == 3 && (uint8_t)s0[0] == 0xEF)) c = legacy_cp(CS_1252, (uint8_t)*s0);  /* stray byte */
                if (!c) c = 0xFFFDu;
                size_t n = wutf8_put(pass ? out + o : tmp, c);
                o += n;
            }
        } else {
            for (size_t i = start; i < len; ++i) {
                uint32_t c = legacy_cp(cs, in[i]);
                if (!c) c = 0xFFFDu;
                o += wutf8_put(pass ? out + o : tmp, c);
            }
        }
        if (!pass) {
            out = (char *)wmem_alloc(o + 1u);
            if (!out) { *out_len = 0; return 0; }
        }
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_alnum(char c) { return is_alpha(c) || (c >= '0' && c <= '9'); }

static int mem_ifind(const uint8_t *s, size_t n, const char *needle) {
    size_t k = wstrlen(needle);
    for (size_t i = 0; i + k <= n; ++i) {
        size_t j = 0;
        while (j < k && lc((char)s[i + j]) == needle[j]) ++j;
        if (j == k) return (int)i;
    }
    return -1;
}

void whtml_sniff_charset(const uint8_t *in, size_t len, char *out, size_t cap) {
    if (cap) out[0] = '\0';
    if (len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) { wstr_copy(out, cap, "utf-8"); return; }
    size_t n = len < 2048u ? len : 2048u;
    int at = mem_ifind(in, n, "charset");
    while (at >= 0) {
        size_t i = (size_t)at + 7;
        while (i < n && (is_ws((char)in[i]))) ++i;
        if (i < n && in[i] == '=') {
            ++i;
            while (i < n && (is_ws((char)in[i]) || in[i] == '"' || in[i] == '\'')) ++i;
            size_t o = 0;
            while (i < n && (is_alnum((char)in[i]) || in[i] == '-' || in[i] == '_') && o + 1u < cap) out[o++] = lc((char)in[i++]);
            if (cap) out[o] = '\0';
            if (o) return;
        }
        int next = mem_ifind(in + at + 7, n - (size_t)at - 7, "charset");
        at = next < 0 ? -1 : at + 7 + next;
    }
}

/* ------------------------------------------------------------------ entities */
static const char LATIN1_NAMES[] =
    "nbsp iexcl cent pound curren yen brvbar sect uml copy ordf laquo not shy reg macr deg plusmn sup2 sup3 acute micro "
    "para middot cedil sup1 ordm raquo frac14 frac12 frac34 iquest Agrave Aacute Acirc Atilde Auml Aring AElig Ccedil "
    "Egrave Eacute Ecirc Euml Igrave Iacute Icirc Iuml ETH Ntilde Ograve Oacute Ocirc Otilde Ouml times Oslash Ugrave "
    "Uacute Ucirc Uuml Yacute THORN szlig agrave aacute acirc atilde auml aring aelig ccedil egrave eacute ecirc euml "
    "igrave iacute icirc iuml eth ntilde ograve oacute ocirc otilde ouml divide oslash ugrave uacute ucirc uuml yacute thorn yuml ";
static const char GREEK_UPPER[] = "Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota Kappa Lambda Mu Nu Xi Omicron Pi Rho - Sigma Tau Upsilon Phi Chi Psi Omega ";
static const char GREEK_LOWER[] = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigmaf sigma tau upsilon phi chi psi omega ";
static const struct { const char *name; uint16_t cp; } OTHER_ENTITIES[] = {
    {"quot", 34}, {"amp", 38}, {"lt", 60}, {"gt", 62}, {"apos", 39}, {"OElig", 338}, {"oelig", 339}, {"Scaron", 352},
    {"scaron", 353}, {"Yuml", 376}, {"fnof", 402}, {"circ", 710}, {"tilde", 732}, {"ensp", 8194}, {"emsp", 8195},
    {"thinsp", 8201}, {"zwnj", 8204}, {"zwj", 8205}, {"lrm", 8206}, {"rlm", 8207}, {"ndash", 8211}, {"mdash", 8212},
    {"lsquo", 8216}, {"rsquo", 8217}, {"sbquo", 8218}, {"ldquo", 8220}, {"rdquo", 8221}, {"bdquo", 8222}, {"dagger", 8224},
    {"Dagger", 8225}, {"bull", 8226}, {"hellip", 8230}, {"permil", 8240}, {"prime", 8242}, {"Prime", 8243}, {"lsaquo", 8249},
    {"rsaquo", 8250}, {"oline", 8254}, {"frasl", 8260}, {"euro", 8364}, {"trade", 8482}, {"larr", 8592}, {"uarr", 8593},
    {"rarr", 8594}, {"darr", 8595}, {"harr", 8596}, {"crarr", 8629}, {"lArr", 8656}, {"uArr", 8657}, {"rArr", 8658},
    {"dArr", 8659}, {"hArr", 8660}, {"forall", 8704}, {"part", 8706}, {"exist", 8707}, {"empty", 8709}, {"nabla", 8711},
    {"isin", 8712}, {"notin", 8713}, {"ni", 8715}, {"prod", 8719}, {"sum", 8721}, {"minus", 8722}, {"lowast", 8727},
    {"radic", 8730}, {"prop", 8733}, {"infin", 8734}, {"ang", 8736}, {"and", 8743}, {"or", 8744}, {"cap", 8745},
    {"cup", 8746}, {"int", 8747}, {"there4", 8756}, {"sim", 8764}, {"cong", 8773}, {"asymp", 8776}, {"ne", 8800},
    {"equiv", 8801}, {"le", 8804}, {"ge", 8805}, {"sub", 8834}, {"sup", 8835}, {"nsub", 8836}, {"sube", 8838},
    {"supe", 8839}, {"oplus", 8853}, {"otimes", 8855}, {"perp", 8869}, {"sdot", 8901}, {"lceil", 8968}, {"rceil", 8969},
    {"lfloor", 8970}, {"rfloor", 8971}, {"lang", 10216}, {"rang", 10217}, {"loz", 9674}, {"spades", 9824}, {"clubs", 9827},
    {"hearts", 9829}, {"diams", 9830}, {"thetasym", 977}, {"upsih", 978}, {"piv", 982}, {"ZeroWidthSpace", 8203},
    {"NewLine", 10}, {"Tab", 9}, {"colon", 58}, {"comma", 44}, {"period", 46}, {"lpar", 40}, {"rpar", 41}, {"lbrack", 91},
    {"rbrack", 93}, {"lowbar", 95}, {"hyphen", 8208}, {"dash", 8208}, {"ast", 42}, {"excl", 33}, {"quest", 63}, {"num", 35},
    {"dollar", 36}, {"percnt", 37}, {"plus", 43}, {"equals", 61}, {"sol", 47}, {"bsol", 92}, {"verbar", 124}, {"vert", 124},
    {"check", 10003}, {"star", 9734}, {"starf", 9733}, {"nbhy", 8209}, {"rsquor", 8217}, {"lbrace", 123}, {"rbrace", 125},
    {"commat", 64}, {"Hat", 94}, {"grave", 96}, {"half", 189}, {"centerdot", 183}, {"times", 215}, {"nbsp", 160},
};

static int name_in_list(const char *list, const char *s, size_t n, uint32_t *index) {
    uint32_t idx = 0;
    for (const char *p = list; *p; ++idx) {
        const char *e = p;
        while (*e && *e != ' ') ++e;
        size_t len = (size_t)(e - p);
        if (len == n) {
            size_t k = 0;
            while (k < n && p[k] == s[k]) ++k;
            if (k == n) { *index = idx; return 1; }
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int named_entity(const char *s, size_t n, uint32_t *cp) {
    uint32_t idx;
    if (name_in_list(LATIN1_NAMES, s, n, &idx)) { *cp = 160u + idx; return 1; }
    if (name_in_list(GREEK_UPPER, s, n, &idx)) { *cp = 913u + idx; return 1; }
    if (name_in_list(GREEK_LOWER, s, n, &idx)) { *cp = 945u + idx; return 1; }
    for (size_t e = 0; e < sizeof(OTHER_ENTITIES) / sizeof(OTHER_ENTITIES[0]); ++e) {
        const char *name = OTHER_ENTITIES[e].name;
        size_t k = 0;
        while (k < n && name[k] == s[k]) ++k;
        if (k == n && !name[k]) { *cp = OTHER_ENTITIES[e].cp; return 1; }
    }
    return 0;
}

size_t whtml_entity(const char *s, size_t n, uint32_t *cp, uint32_t *cp2) {
    *cp2 = 0;
    if (!n) return 0;
    if (s[0] == '#') {
        int hex = n > 1 && (s[1] == 'x' || s[1] == 'X');
        size_t i = hex ? 2u : 1u, digits = 0;
        uint32_t v = 0;
        while (i < n) {
            char c = s[i];
            uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') : (hex && lc(c) >= 'a' && lc(c) <= 'f') ? (uint32_t)(lc(c) - 'a' + 10) : 99u;
            if (d == 99u) break;
            if (v < 0x110000u) v = v * (hex ? 16u : 10u) + d;
            ++i; ++digits;
        }
        if (!digits) return 0;
        if (i < n && s[i] == ';') ++i;
        if (v >= 0x80u && v < 0xA0u) v = CP1252_80[v - 0x80u];
        if (!v || v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu)) v = 0xFFFDu;
        *cp = v;
        return i;
    }
    size_t i = 0;
    while (i < n && i < 32u && is_alnum(s[i])) ++i;
    if (!i) return 0;
    if (i < n && s[i] == ';') {
        if (named_entity(s, i, cp)) return i + 1u;
        return 0;
    }
    /* legacy entities without ';' (only the common ones, and not before '=' / alnum in attributes) */
    static const char *const legacy[] = {"amp", "lt", "gt", "quot", "nbsp", "copy", "reg"};
    for (size_t k = 0; k < sizeof(legacy) / sizeof(legacy[0]); ++k) {
        size_t l = wstrlen(legacy[k]);
        if (l <= i) {
            size_t m = 0;
            while (m < l && s[m] == legacy[k][m]) ++m;
            if (m == l && (l == i) && named_entity(s, l, cp)) return l;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ tree builder */
typedef struct builder {
    wdoc *d;
    wnode *stack[MAX_DEPTH];
    uint32_t depth;
    wnode *form;
    char *text; size_t text_len, text_cap;
    int oom;
} builder;

static builder *B;

static wnode *new_node(uint16_t tag) {
    wdoc *d = B->d;
    if (d->node_count >= MAX_NODES) { d->truncated = 1; return 0; }
    wnode *n = (wnode *)warena_alloc(&d->arena, sizeof(wnode));
    if (!n) { B->oom = 1; d->truncated = 1; return 0; }
    n->tag = tag; n->order = d->node_count++;
    n->ctl = n->img = n->link = -1;
    if (tag > WT_UNKNOWN || tag == WT_UNKNOWN) d->elem_count++;
    return n;
}
static void append_child(wnode *parent, wnode *child) {
    child->parent = parent;
    child->prev = parent->last;
    if (parent->last) parent->last->next = child; else parent->first = child;
    parent->last = child;
}
static void insert_before(wnode *ref, wnode *child) {
    wnode *parent = ref->parent;
    child->parent = parent;
    child->next = ref; child->prev = ref->prev;
    if (ref->prev) ref->prev->next = child; else parent->first = child;
    ref->prev = child;
}
static int name_eq(const char *a, const char *b, size_t n) {
    if (!a) return 0;
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return a[n] == '\0';
}
static wnode *walk_next(wnode *cur, wnode *root) {
    if (cur->first) return cur->first;
    while (cur != root && !cur->next) cur = cur->parent;
    return cur == root ? 0 : cur->next;
}
static wnode *current(void) { return B->depth ? B->stack[B->depth - 1] : B->d->root; }
static void push(wnode *n) { if (B->depth < MAX_DEPTH) B->stack[B->depth++] = n; else B->d->truncated = 1; }
static int on_stack(uint16_t tag) { for (uint32_t i = B->depth; i-- > 0;) if (B->stack[i]->tag == tag) return (int)i; return -1; }

static int is_table_ctx(uint16_t t) { return t == WT_TABLE || t == WT_TBODY || t == WT_THEAD || t == WT_TFOOT || t == WT_TR; }

static wnode *ensure_html(void) {
    if (!B->d->html) {
        wnode *h = new_node(WT_HTML);
        if (!h) return 0;
        append_child(B->d->root, h);
        B->d->html = h;
        B->depth = 0; push(h);
    }
    return B->d->html;
}
static wnode *ensure_head(void) {
    if (!ensure_html()) return 0;
    if (!B->d->head) {
        wnode *h = new_node(WT_HEAD);
        if (!h) return 0;
        append_child(B->d->html, h);
        B->d->head = h;
    }
    return B->d->head;
}
static wnode *ensure_body(void) {
    if (!ensure_html()) return 0;
    if (!B->d->body) {
        (void)ensure_head();
        wnode *b = new_node(WT_BODY);
        if (!b) return 0;
        append_child(B->d->html, b);
        B->d->body = b;
        B->depth = 0; push(B->d->html); push(b);
    }
    return B->d->body;
}

/* Inserts text into the current node (or fosters it before a table). */
static void flush_text(int in_head) {
    if (!B->text_len) return;
    size_t n = B->text_len;
    B->text_len = 0;
    int all_ws = 1;
    for (size_t i = 0; i < n; ++i) if (!is_ws(B->text[i])) { all_ws = 0; break; }
    wnode *parent;
    (void)in_head;
    if (!B->d->body) {
        if (all_ws) return;                                  /* whitespace before <body> */
        if (!ensure_body()) return;
    }
    parent = current();
    wnode *foster = 0;
    if (is_table_ctx(parent->tag)) {
        if (all_ws) return;
        int t = on_stack(WT_TABLE);
        if (t >= 0) foster = B->stack[t];
    }
    if (parent->tag == WT_HTML || parent->tag == WT_DOCUMENT || parent->tag == WT_HEAD) {
        if (all_ws) return;
        if (!ensure_body()) return;
        parent = B->d->body;
    }
    if (parent->tag == WT_SELECT && all_ws) return;
    wnode *last = foster ? foster->prev : parent->last;
    char *t = warena_strndup(&B->d->arena, B->text, n);
    if (!t) { B->oom = 1; B->d->truncated = 1; return; }
    if (last && last->tag == WT_TEXT && !foster) {          /* merge adjacent text */
        char *m = (char *)warena_alloc(&B->d->arena, last->text_len + n + 1u);
        if (!m) { B->oom = 1; return; }
        wb_copy(m, last->text, last->text_len); wb_copy(m + last->text_len, t, n); m[last->text_len + n] = '\0';
        last->text = m; last->text_len += (uint32_t)n;
        return;
    }
    wnode *tn = new_node(WT_TEXT);
    if (!tn) return;
    tn->text = t; tn->text_len = (uint32_t)n;
    if (foster) insert_before(foster, tn); else append_child(parent, tn);
}

static void text_put(const char *s, size_t n) {
    if (B->text_len + n + 1u > B->text_cap) {
        size_t cap = B->text_cap ? B->text_cap * 2u : 4096u;
        while (cap < B->text_len + n + 1u) cap *= 2u;
        char *t = (char *)wmem_realloc(B->text, cap);
        if (!t) { B->oom = 1; return; }
        B->text = t; B->text_cap = cap;
    }
    wb_copy(B->text + B->text_len, s, n);
    B->text_len += n;
}

/* Pops the stack down to (and including) index i. */
static void pop_to(int i) {
    if (i < 0) return;
    for (uint32_t k = (uint32_t)i; k < B->depth; ++k) if (B->stack[k] == B->form) B->form = 0;
    B->depth = (uint32_t)i;
}

static int is_scope_boundary(uint16_t t) {
    return t == WT_HTML || t == WT_TABLE || t == WT_TD || t == WT_TH || t == WT_CAPTION || t == WT_TEMPLATE ||
           t == WT_OBJECT || t == WT_BODY;
}
/* Index of the nearest open element with tag in scope, or -1. */
static int in_scope(uint16_t tag, int button_scope) {
    for (uint32_t i = B->depth; i-- > 0;) {
        uint16_t t = B->stack[i]->tag;
        if (t == tag) return (int)i;
        if (is_scope_boundary(t) || (button_scope && t == WT_BUTTON)) return -1;
    }
    return -1;
}
static void close_p(void) { int i = in_scope(WT_P, 1); if (i >= 0) pop_to(i); }

static int closes_p(uint16_t t) {
    switch (t) {
    case WT_ADDRESS: case WT_ARTICLE: case WT_ASIDE: case WT_BLOCKQUOTE: case WT_CENTER: case WT_DETAILS: case WT_DIALOG:
    case WT_DIR: case WT_DIV: case WT_DL: case WT_FIELDSET: case WT_FIGCAPTION: case WT_FIGURE: case WT_FOOTER: case WT_FORM:
    case WT_H1: case WT_H2: case WT_H3: case WT_H4: case WT_H5: case WT_H6: case WT_HEADER: case WT_HGROUP: case WT_HR:
    case WT_MAIN: case WT_MENU: case WT_NAV: case WT_OL: case WT_P: case WT_PRE: case WT_SECTION: case WT_SUMMARY:
    case WT_TABLE: case WT_UL: case WT_LI: case WT_DD: case WT_DT: case WT_LISTING: case WT_PLAINTEXT: case WT_XMP: case WT_SEARCH:
        return 1;
    default: return 0;
    }
}
static int is_void(uint16_t t) {
    return t == WT_AREA || t == WT_BASE || t == WT_BR || t == WT_COL || t == WT_EMBED || t == WT_HR || t == WT_IMG ||
           t == WT_INPUT || t == WT_LINK || t == WT_META || t == WT_PARAM || t == WT_SOURCE || t == WT_TRACK || t == WT_WBR;
}
static int head_element(uint16_t t) {
    return t == WT_META || t == WT_LINK || t == WT_TITLE || t == WT_STYLE || t == WT_SCRIPT || t == WT_BASE || t == WT_TEMPLATE;
}
static int table_structure(uint16_t t) {
    return t == WT_TR || t == WT_TD || t == WT_TH || t == WT_TBODY || t == WT_THEAD || t == WT_TFOOT || t == WT_CAPTION ||
           t == WT_COL || t == WT_COLGROUP || t == WT_TABLE;
}

static const char *attr_of(wattr *a, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; ++i) if (wstr_ieq(a[i].name, name)) return a[i].value;
    return 0;
}

static void *grow(void *arr, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return arr;
    uint32_t nc = *cap ? *cap * 2u : 64u;
    while (nc < need) nc *= 2u;
    void *n = wmem_realloc(arr, (size_t)nc * elem);
    if (!n) return 0;
    *cap = nc;
    return n;
}

static void register_element(wnode *n) {
    wdoc *d = B->d;
    switch (n->tag) {
    case WT_A: {
        const char *href = wnode_attr(n, "href");
        if (href) {
            const char **l = (const char **)grow((void *)d->links, &d->link_cap, d->link_count + 1u, sizeof(char *));
            if (l) { d->links = l; n->link = (int32_t)d->link_count; d->links[d->link_count++] = href; }
        }
        break;
    }
    case WT_IMG: {
        wnode **l = (wnode **)grow(d->imgs, &d->img_cap, d->img_count + 1u, sizeof(wnode *));
        if (l) { d->imgs = l; n->img = (int32_t)d->img_count; d->imgs[d->img_count++] = n; }
        break;
    }
    case WT_INPUT: case WT_TEXTAREA: case WT_SELECT: case WT_BUTTON: {
        wctl *l = (wctl *)grow(d->ctls, &d->ctl_cap, d->ctl_count + 1u, sizeof(wctl));
        if (!l) break;
        d->ctls = l;
        wctl *c = &d->ctls[d->ctl_count];
        wb_set(c, 0, sizeof(*c));
        c->node = n; c->form = B->form; c->selected = -1;
        n->ctl = (int32_t)d->ctl_count++;
        break;
    }
    case WT_BASE: if (!d->base_href) d->base_href = wnode_attr(n, "href"); break;
    case WT_META: {
        const char *he = wnode_attr(n, "http-equiv");
        if (he && wstr_ieq(he, "refresh") && !d->refresh) d->refresh = wnode_attr(n, "content");
        break;
    }
    case WT_LINK: {
        const char *rel = wnode_attr(n, "rel"), *href = wnode_attr(n, "href");
        if (rel && href && d->sheet_count < WDOC_MAX_SHEETS) {
            int sheet = 0, alt = 0;
            for (const char *p = rel; *p; ++p) {
                if (wstr_iprefix(p, "stylesheet")) sheet = 1;
                if (wstr_iprefix(p, "alternate")) alt = 1;
            }
            if (sheet && !alt) {
                wsheet_src *s = &d->sheets[d->sheet_count++];
                s->kind = WSHEET_LINK; s->href = href; s->media = wnode_attr(n, "media"); s->text = 0; s->len = 0;
            }
        }
        break;
    }
    default: break;
    }
}

/* Raw text (script, style, title, textarea...) for element n. */
static void raw_text(wnode *n, const char *s, size_t len, int decode) {
    if (n->tag == WT_SCRIPT || n->tag == WT_NOEMBED || n->tag == WT_NOFRAMES || n->tag == WT_IFRAME) return;   /* never used */
    B->text_len = 0;
    if (decode) {
        for (size_t i = 0; i < len;) {
            if (s[i] == '&') {
                uint32_t cp, cp2; char u[8];
                size_t used = whtml_entity(s + i + 1, len - i - 1, &cp, &cp2);
                if (used) { text_put(u, wutf8_put(u, cp)); i += used + 1; continue; }
            }
            text_put(s + i, 1); ++i;
        }
    } else text_put(s, len);
    if (n->tag == WT_TITLE) {
        if (!B->d->title[0]) {
            size_t o = 0; int sp = 0;
            for (size_t i = 0; i < B->text_len && o + 2u < sizeof(B->d->title); ++i) {
                char c = B->text[i];
                if (is_ws(c)) { sp = o > 0; continue; }
                if (sp) { B->d->title[o++] = ' '; sp = 0; }
                B->d->title[o++] = c;
            }
            B->d->title[o] = '\0';
        }
        B->text_len = 0;
        return;
    }
    char *t = warena_strndup(&B->d->arena, B->text, B->text_len);
    size_t tl = B->text_len;
    B->text_len = 0;
    if (!t) return;
    if (n->tag == WT_STYLE) {
        if (B->d->sheet_count < WDOC_MAX_SHEETS) {
            wsheet_src *sh = &B->d->sheets[B->d->sheet_count++];
            sh->kind = WSHEET_INLINE; sh->text = t; sh->len = tl; sh->href = 0; sh->media = wnode_attr(n, "media");
        }
        return;
    }
    wnode *tn = new_node(WT_TEXT);
    if (!tn) return;
    tn->text = t; tn->text_len = (uint32_t)tl;
    append_child(n, tn);
}

static wnode *start_tag(uint16_t tag, const char *name, size_t name_len, wattr *attrs, uint32_t nattr, int self_closing) {
    wdoc *d = B->d;
    if (tag == WT_HTML) { (void)ensure_html(); return 0; }
    if (!ensure_html()) return 0;
    wnode *parent = 0;
    if (tag == WT_HEAD) { (void)ensure_head(); return 0; }
    if (tag == WT_BODY) {
        if (!d->body) {
            (void)ensure_body();
            if (d->body && nattr) {                                /* keep <body> attributes (bgcolor, class) */
                d->body->attrs = attrs; d->body->nattr = (uint16_t)nattr;
                d->body->id = attr_of(attrs, nattr, "id"); d->body->cls = attr_of(attrs, nattr, "class");
            }
        }
        return 0;
    }
    if (tag == WT_FRAMESET || tag == WT_FRAME) return 0;
    if (head_element(tag) && !d->body) parent = ensure_head();
    else if (!d->body) { if (!ensure_body()) return 0; }
    if (!parent) {
        /* implied end tags */
        if (closes_p(tag)) close_p();
        if (tag == WT_LI) {
            for (uint32_t i = B->depth; i-- > 0;) {
                uint16_t t = B->stack[i]->tag;
                if (t == WT_LI) { pop_to((int)i); break; }
                if (t == WT_UL || t == WT_OL || t == WT_MENU || t == WT_DIR || is_scope_boundary(t)) break;
            }
        } else if (tag == WT_DD || tag == WT_DT) {
            for (uint32_t i = B->depth; i-- > 0;) {
                uint16_t t = B->stack[i]->tag;
                if (t == WT_DD || t == WT_DT) { pop_to((int)i); break; }
                if (t == WT_DL || is_scope_boundary(t)) break;
            }
        } else if (tag == WT_OPTION || tag == WT_OPTGROUP) {
            if (current()->tag == WT_OPTION) pop_to((int)B->depth - 1);
            if (tag == WT_OPTGROUP && current()->tag == WT_OPTGROUP) pop_to((int)B->depth - 1);
        } else if (tag >= WT_H1 && tag <= WT_H6) {
            uint16_t t = current()->tag;
            if (t >= WT_H1 && t <= WT_H6) pop_to((int)B->depth - 1);
        } else if (tag == WT_A) {
            int i = in_scope(WT_A, 0);
            if (i >= 0) pop_to(i);
        } else if (tag == WT_NOBR) {
            int i = in_scope(WT_NOBR, 0);
            if (i >= 0) pop_to(i);
        } else if (tag == WT_BUTTON) {
            int i = in_scope(WT_BUTTON, 0);
            if (i >= 0) pop_to(i);
        } else if (tag == WT_TR) {
            for (uint32_t i = B->depth; i-- > 0;) {
                uint16_t t = B->stack[i]->tag;
                if (t == WT_TR) { pop_to((int)i); break; }
                if (t == WT_TABLE || t == WT_TBODY || t == WT_THEAD || t == WT_TFOOT || t == WT_HTML) break;
            }
        } else if (tag == WT_TD || tag == WT_TH) {
            for (uint32_t i = B->depth; i-- > 0;) {
                uint16_t t = B->stack[i]->tag;
                if (t == WT_TD || t == WT_TH) { pop_to((int)i); break; }
                if (t == WT_TR || t == WT_TABLE || t == WT_HTML) break;
            }
            uint16_t t = current()->tag;
            if (t == WT_TABLE || t == WT_TBODY || t == WT_THEAD || t == WT_TFOOT) {     /* implied <tr> */
                wnode *tr = new_node(WT_TR);
                if (tr) { append_child(current(), tr); push(tr); }
            }
        } else if (tag == WT_TBODY || tag == WT_THEAD || tag == WT_TFOOT || tag == WT_CAPTION || tag == WT_COLGROUP) {
            int tbl = on_stack(WT_TABLE);
            if (tbl >= 0) pop_to(tbl + 1);
        } else if (tag == WT_FORM) {
            if (B->form && on_stack(WT_FORM) >= 0) return 0;      /* nested forms are ignored */
        } else if (tag == WT_TABLE) {
            /* a table directly inside a table row context ends the open cell structure only in broken markup */
        }
        parent = current();
        if (parent->tag == WT_HTML || parent->tag == WT_DOCUMENT || parent->tag == WT_HEAD) { if (!ensure_body()) return 0; parent = d->body; }
    }
    wnode *n = new_node(tag);
    if (!n) return 0;
    if (tag == WT_UNKNOWN) n->name = warena_strndup(&d->arena, name, name_len);
    n->attrs = attrs; n->nattr = (uint16_t)nattr;
    n->id = attr_of(attrs, nattr, "id");
    n->cls = attr_of(attrs, nattr, "class");
    /* foster parenting: non-table content directly inside table structure goes before the table */
    if (is_table_ctx(parent->tag) && !table_structure(tag) && tag != WT_FORM && tag != WT_SCRIPT && tag != WT_STYLE &&
        tag != WT_TEMPLATE && !(tag == WT_INPUT && attr_of(attrs, nattr, "type") && wstr_ieq(attr_of(attrs, nattr, "type"), "hidden"))) {
        int t = on_stack(WT_TABLE);
        if (t >= 0) insert_before(B->stack[t], n); else append_child(parent, n);
    } else if ((tag == WT_TR) && (parent->tag == WT_TABLE)) {
        append_child(parent, n);                               /* rows directly in tables are fine for our layout */
    } else append_child(parent, n);
    if (tag == WT_FORM) B->form = n;
    register_element(n);
    if (!is_void(tag) && !self_closing) push(n);
    else if (!is_void(tag) && self_closing && tag != WT_SVG && tag != WT_UNKNOWN && tag != WT_MATH) push(n);   /* "<div/>" is not self-closing in HTML */
    return n;
}

static void end_tag(uint16_t tag, const char *name, size_t name_len) {
    if (tag == WT_BODY || tag == WT_HTML || tag == WT_HEAD) return;
    if (tag == WT_BR) { (void)start_tag(WT_BR, "br", 2, 0, 0, 1); return; }
    if (tag == WT_P && in_scope(WT_P, 1) < 0) { wnode *p = start_tag(WT_P, "p", 1, 0, 0, 0); (void)p; pop_to((int)B->depth - 1); return; }
    if (tag == WT_FORM) { int i = on_stack(WT_FORM); B->form = 0; if (i >= 0) pop_to(i); return; }
    int table_tag = tag == WT_TABLE || tag == WT_TR || tag == WT_TD || tag == WT_TH || tag == WT_TBODY || tag == WT_THEAD ||
                    tag == WT_TFOOT || tag == WT_CAPTION;
    for (uint32_t i = B->depth; i-- > 0;) {
        wnode *n = B->stack[i];
        if (n->tag == tag && (tag != WT_UNKNOWN || name_eq(n->name, name, name_len))) {
            pop_to((int)i);
            return;
        }
        uint16_t t = n->tag;
        if (table_tag) { if (t == WT_TABLE && tag != WT_TABLE) return; if (t == WT_HTML) return; continue; }
        if (is_scope_boundary(t) || t == WT_SELECT) return;
    }
}

/* ------------------------------------------------------------------ tokenizer */
static int raw_kind(uint16_t tag) {
    switch (tag) {
    case WT_SCRIPT: case WT_STYLE: case WT_XMP: case WT_IFRAME: case WT_NOEMBED: case WT_NOFRAMES: return 1;   /* raw */
    case WT_TITLE: case WT_TEXTAREA: return 2;                                                                    /* RCDATA */
    case WT_PLAINTEXT: return 3;
    default: return 0;
    }
}

int whtml_parse(wdoc *d, const char *s, size_t len, int as_text) {
    static builder b;
    wb_set(d, 0, sizeof(*d));
    warena_init(&d->arena);
    wb_set(&b, 0, sizeof(b));
    B = &b; b.d = d;
    d->root = (wnode *)warena_alloc(&d->arena, sizeof(wnode));
    if (!d->root) return -1;
    d->root->tag = WT_DOCUMENT; d->root->ctl = d->root->img = d->root->link = -1;
    if (as_text) {
        d->is_plain_text = 1;
        ensure_body();
        wnode *pre = start_tag(WT_PRE, "pre", 3, 0, 0, 0);
        if (pre) { raw_text(pre, s, len, 0); }
        wmem_free(b.text);
        return b.oom ? -1 : 0;
    }
    static wattr tmp_attrs[MAX_ATTRS];
    size_t i = 0;
    while (i < len && !b.oom) {
        char c = s[i];
        if (c == '<' && i + 1 < len) {
            char n1 = s[i + 1];
            if (n1 == '!') {
                flush_text(!d->body);
                if (i + 3 < len && s[i + 2] == '-' && s[i + 3] == '-') {            /* comment */
                    size_t j = i + 4;
                    while (j + 2 < len && !(s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>')) ++j;
                    i = j + 2 < len ? j + 3 : len;
                } else if (i + 8 < len && wstr_iprefix(s + i + 2, "[CDATA[")) {
                    size_t j = i + 9;
                    while (j + 2 < len && !(s[j] == ']' && s[j + 1] == ']' && s[j + 2] == '>')) ++j;
                    i = j + 2 < len ? j + 3 : len;
                } else {
                    if (i + 9 < len && wstr_iprefix(s + i + 2, "doctype")) d->has_doctype = 1;
                    while (i < len && s[i] != '>') ++i;
                    ++i;
                }
                continue;
            }
            if (n1 == '?') { flush_text(!d->body); while (i < len && s[i] != '>') ++i; ++i; continue; }
            int closing = n1 == '/';
            size_t j = i + 1 + (size_t)closing;
            if (j >= len || !is_alpha(s[j])) {
                if (closing && j < len && s[j] == '>') { i = j + 1; continue; }   /* "</>" */
                text_put("<", 1); ++i; continue;                                  /* literal '<' */
            }
            flush_text(!d->body);
            char name[64]; size_t nl = 0;
            while (j < len && !is_ws(s[j]) && s[j] != '/' && s[j] != '>') { if (nl + 1u < sizeof(name)) name[nl++] = lc(s[j]); ++j; }
            name[nl] = '\0';
            uint16_t tag = (uint16_t)wtag_lookup(name, nl);
            uint32_t na = 0;
            int self_closing = 0;
            while (j < len && s[j] != '>') {
                if (is_ws(s[j])) { ++j; continue; }
                if (s[j] == '/') { self_closing = j + 1 < len && s[j + 1] == '>'; ++j; continue; }
                size_t an = j;
                while (j < len && !is_ws(s[j]) && s[j] != '=' && s[j] != '>' && !(s[j] == '/' && j + 1 < len && s[j + 1] == '>')) ++j;
                size_t ae = j;
                while (j < len && is_ws(s[j])) ++j;
                const char *vs = 0; size_t vl = 0;
                if (j < len && s[j] == '=') {
                    ++j;
                    while (j < len && is_ws(s[j])) ++j;
                    if (j < len && (s[j] == '"' || s[j] == '\'')) {
                        char q = s[j++];
                        vs = s + j;
                        while (j < len && s[j] != q) ++j;
                        vl = (size_t)(s + j - vs);
                        if (j < len) ++j;
                    } else {
                        vs = s + j;
                        while (j < len && !is_ws(s[j]) && s[j] != '>') ++j;
                        vl = (size_t)(s + j - vs);
                    }
                }
                if (closing || na >= MAX_ATTRS || ae == an) continue;
                /* attributes nothing in the engine reads: scripts, data blobs, RDFa, most ARIA */
                {
                    const char *an0 = s + an; size_t al = ae - an;
                    #define APFX(p) (al >= sizeof(p) - 1 && wstr_iprefix(an0, p))
                    int keep_data = APFX("data-src") || APFX("data-lazy") || APFX("data-original") || APFX("data-srcset");
                    if ((al > 2 && (an0[0] == 'o' || an0[0] == 'O') && (an0[1] == 'n' || an0[1] == 'N')) ||
                        (APFX("data-") && !keep_data) || APFX("typeof") || APFX("about") || APFX("resource") || APFX("property") ||
                        APFX("prefix") || APFX("integrity") || APFX("crossorigin") || APFX("nonce") || APFX("srcdoc") || APFX("jsaction") ||
                        APFX("jsname") || APFX("jscontroller") || APFX("jsmodel") || APFX("ping") ||
                        (APFX("aria-") && !APFX("aria-hidden") && !APFX("aria-label") && !APFX("aria-current") && !APFX("aria-expanded")))
                        continue;
                    #undef APFX
                }
                char *aname = (char *)warena_alloc(&d->arena, ae - an + 1u);
                if (!aname) { b.oom = 1; break; }
                for (size_t k = 0; k < ae - an; ++k) aname[k] = lc(s[an + k]);
                int dup = 0;
                for (uint32_t k = 0; k < na; ++k) if (wstr_ieq(tmp_attrs[k].name, aname)) dup = 1;
                if (dup) continue;
                /* decode entities in the value */
                char *val = (char *)warena_alloc(&d->arena, vl * 1u + 8u);
                if (!val) { b.oom = 1; break; }
                size_t o = 0;
                for (size_t k = 0; k < vl;) {
                    if (vs[k] == '&') {
                        uint32_t cp, cp2;
                        size_t used = whtml_entity(vs + k + 1, vl - k - 1, &cp, &cp2);
                        /* in attributes, a legacy entity followed by '=' or alnum is literal */
                        if (used && !(vs[k + used] != ';' && k + 1 + used < vl && (vs[k + 1 + used] == '=' || is_alnum(vs[k + 1 + used])))) {
                            char u[8]; size_t ul = wutf8_put(u, cp);
                            if (ul <= used + 1u) { wb_copy(val + o, u, ul); o += ul; k += used + 1; continue; }
                        }
                    }
                    char ch = vs[k++];
                    val[o++] = (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
                }
                val[o] = '\0';
                tmp_attrs[na].name = aname; tmp_attrs[na].value = val; ++na;
            }
            i = j < len ? j + 1 : len;
            if (closing) { end_tag(tag, name, nl); continue; }
            wattr *attrs = 0;
            if (na) {
                attrs = (wattr *)warena_alloc(&d->arena, na * sizeof(wattr));
                if (!attrs) { b.oom = 1; break; }
                for (uint32_t k = 0; k < na; ++k) attrs[k] = tmp_attrs[k];
            }
            /* <noscript> in <head> holds link/style/meta: parse it like the body would */
            wnode *n = start_tag(tag, name, nl, attrs, na, self_closing);
            int rk = raw_kind(tag);
            if (rk == 3) {                                  /* <plaintext>: the rest is text */
                if (n) raw_text(n, s + i, len - i, 0);
                i = len;
                break;
            }
            if (rk) {
                /* find "</name" */
                size_t k = i;
                while (k < len) {
                    if (s[k] == '<' && k + 1 + nl < len && s[k + 1] == '/' ) {
                        size_t m = 0;
                        while (m < nl && lc(s[k + 2 + m]) == name[m]) ++m;
                        if (m == nl && (k + 2 + nl >= len || is_ws(s[k + 2 + nl]) || s[k + 2 + nl] == '>' || s[k + 2 + nl] == '/')) break;
                    }
                    ++k;
                }
                if (n) raw_text(n, s + i, k - i, rk == 2);
                i = k;
                while (i < len && s[i] != '>') ++i;
                if (i < len) ++i;
                if (n) { int at = -1; for (uint32_t q = B->depth; q-- > 0;) if (B->stack[q] == n) { at = (int)q; break; } if (at >= 0) pop_to(at); }
            }
            continue;
        }
        if (c == '&') {
            uint32_t cp, cp2;
            size_t used = whtml_entity(s + i + 1, len - i - 1, &cp, &cp2);
            if (used) { char u[8]; text_put(u, wutf8_put(u, cp)); i += used + 1; continue; }
        }
        if (c == '\r') { if (i + 1 < len && s[i + 1] == '\n') { ++i; continue; } c = '\n'; }
        if (c == '\0') { ++i; continue; }
        text_put(&c, 1);
        ++i;
    }
    flush_text(!d->body);
    if (!d->body) ensure_body();
    /* post-pass: form controls */
    for (uint32_t k = 0; k < d->ctl_count; ++k) {
        wctl *c = &d->ctls[k];
        wnode *n = c->node;
        const char *type = wnode_attr(n, "type");
        c->name = wnode_attr(n, "name");
        c->placeholder = wnode_attr(n, "placeholder");
        c->disabled = wnode_attr(n, "disabled") != 0;
        c->readonly = wnode_attr(n, "readonly") != 0;
        c->checked = wnode_attr(n, "checked") != 0;
        const char *val = wnode_attr(n, "value");
        if (n->tag == WT_INPUT) {
            c->type = WCT_TEXT;
            if (type) {
                if (wstr_ieq(type, "password")) c->type = WCT_PASSWORD;
                else if (wstr_ieq(type, "search")) c->type = WCT_SEARCH;
                else if (wstr_ieq(type, "hidden")) c->type = WCT_HIDDEN;
                else if (wstr_ieq(type, "checkbox")) c->type = WCT_CHECKBOX;
                else if (wstr_ieq(type, "radio")) c->type = WCT_RADIO;
                else if (wstr_ieq(type, "submit")) c->type = WCT_SUBMIT;
                else if (wstr_ieq(type, "reset")) c->type = WCT_RESET;
                else if (wstr_ieq(type, "button")) c->type = WCT_BUTTON;
                else if (wstr_ieq(type, "image")) c->type = WCT_IMAGE;
                else if (wstr_ieq(type, "file")) c->type = WCT_FILE;
                else if (wstr_ieq(type, "color") || wstr_ieq(type, "range")) c->type = WCT_OTHER;
                /* email, url, tel, number, date... are edited as text */
            }
            if (c->type == WCT_SUBMIT) c->label = val ? val : "Submit";
            else if (c->type == WCT_RESET) c->label = val ? val : "Reset";
            else if (c->type == WCT_BUTTON) c->label = val ? val : "";
            else if (c->type == WCT_IMAGE) c->label = wnode_attr(n, "alt") ? wnode_attr(n, "alt") : "Submit";
            const char *sz = wnode_attr(n, "size");
            c->size = 0;
            if (sz) for (const char *p = sz; *p >= '0' && *p <= '9' && c->size < 400; ++p) c->size = c->size * 10 + (*p - '0');
        } else if (n->tag == WT_TEXTAREA) {
            c->type = WCT_TEXTAREA;
            static char tbuf[8192];
            size_t tl = 0;
            for (wnode *t = n->first; t; t = t->next) if (t->tag == WT_TEXT) {
                for (uint32_t q = 0; q < t->text_len && tl + 1u < sizeof(tbuf); ++q) tbuf[tl++] = t->text[q];
            }
            size_t st = tl && tbuf[0] == '\n' ? 1u : 0u;              /* a leading newline is dropped */
            tbuf[tl] = '\0';
            val = tbuf + st;
            n->first = n->last = 0;                                    /* the control shows the value itself */
            const char *cols = wnode_attr(n, "cols"), *rows = wnode_attr(n, "rows");
            c->size = 0; c->rows = 0;
            if (cols) for (const char *p = cols; *p >= '0' && *p <= '9' && c->size < 400; ++p) c->size = c->size * 10 + (*p - '0');
            if (rows) for (const char *p = rows; *p >= '0' && *p <= '9' && c->rows < 100; ++p) c->rows = c->rows * 10 + (*p - '0');
        } else if (n->tag == WT_SELECT) {
            c->type = WCT_SELECT;
            uint32_t count = 0;
            for (wnode *o = walk_next(n, n); o && count < 4000u; o = walk_next(o, n)) if (o->tag == WT_OPTION) ++count;
            c->opts = count ? (wnode **)warena_alloc(&d->arena, count * sizeof(wnode *)) : 0;
            if (!c->opts) count = 0;
            uint32_t k2 = 0;
            for (wnode *o = walk_next(n, n); o && k2 < count; o = walk_next(o, n)) {
                if (o->tag != WT_OPTION) continue;
                if (wnode_attr(o, "selected") && c->selected < 0) c->selected = (int32_t)k2;
                c->opts[k2++] = o;
            }
            count = k2;
            c->nopts = (int32_t)count;
            if (c->selected < 0 && count) c->selected = 0;
            val = 0;
        } else {
            c->type = WCT_BUTTON_EL;
            static char lbuf[256];
            wnode_text(n, lbuf, sizeof(lbuf));
            c->label = warena_strndup(&d->arena, lbuf, wstrlen(lbuf));
            if (type && (wstr_ieq(type, "button") || wstr_ieq(type, "reset"))) c->type = wstr_ieq(type, "reset") ? WCT_RESET : WCT_BUTTON;
        }
        size_t vl = val ? wstrlen(val) : 0;
        c->cap = (uint32_t)(vl + (c->type == WCT_TEXTAREA ? 4096u : 512u));
        c->value = (char *)warena_alloc(&d->arena, c->cap);
        if (!c->value) { c->cap = 0; continue; }
        if (vl) wb_copy(c->value, val, vl);
        c->value[vl] = '\0';
        c->len = (uint32_t)vl; c->caret = c->len;
        c->focus_ok = (c->type == WCT_TEXT || c->type == WCT_PASSWORD || c->type == WCT_SEARCH || c->type == WCT_TEXTAREA) && !c->disabled && !c->readonly;
    }
    wmem_free(b.text);
    b.text = 0;
    if (b.oom) d->truncated = 1;
    return 0;
}

void wdoc_free(wdoc *d) {
    wmem_free((void *)d->links); wmem_free(d->imgs); wmem_free(d->ctls);
    warena_free(&d->arena);
    wb_set(d, 0, sizeof(*d));
}

const char *wnode_attr(const wnode *n, const char *name) {
    if (!n) return 0;
    for (uint32_t i = 0; i < n->nattr; ++i) if (wstr_ieq(n->attrs[i].name, name)) return n->attrs[i].value;
    return 0;
}

int wnode_has_class(const wnode *n, const char *cls, size_t len) {
    const char *c = n->cls;
    if (!c) return 0;
    while (*c) {
        while (*c && is_ws(*c)) ++c;
        const char *s = c;
        while (*c && !is_ws(*c)) ++c;
        if ((size_t)(c - s) == len) {
            size_t k = 0;
            while (k < len && s[k] == cls[k]) ++k;
            if (k == len) return 1;
        }
    }
    return 0;
}

size_t wnode_text(const wnode *n, char *out, size_t cap) {
    size_t o = 0;
    int sp = 0;
    if (!cap) return 0;
    const wnode *cur = n->first;
    while (cur && cur != n) {
        if (cur->tag == WT_TEXT) {
            for (uint32_t i = 0; i < cur->text_len && o + 2u < cap; ++i) {
                char c = cur->text[i];
                if (is_ws(c)) { sp = o > 0; continue; }
                if (sp) { out[o++] = ' '; sp = 0; }
                out[o++] = c;
            }
        }
        if (cur->first && cur->tag != WT_SCRIPT && cur->tag != WT_STYLE) { cur = cur->first; continue; }
        while (cur && cur != n && !cur->next) cur = cur->parent;
        if (cur && cur != n) cur = cur->next;
    }
    out[o] = '\0';
    return o;
}

wnode *wdoc_find_id(const wdoc *d, const char *id) {
    if (!d->root || !id || !id[0]) return 0;
    wnode *cur = d->root;
    while (cur) {
        if (cur->id && wstr_ieq(cur->id, id)) return cur;
        if (cur->tag == WT_A) { const char *nm = wnode_attr(cur, "name"); if (nm && wstr_ieq(nm, id)) return cur; }
        if (cur->first) { cur = cur->first; continue; }
        while (cur && !cur->next) cur = cur->parent;
        if (cur) cur = cur->next;
    }
    return 0;
}
