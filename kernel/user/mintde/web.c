/* MintDE Web — a small text-first web browser on the MintOS TCP/IP stack.
 *
 * HTTP/1.0 over plain TCP or TLS 1.3 (kernel/user/crypto: certificates are
 * verified against the Mozilla roots; certificate errors stop the load and
 * cannot be bypassed). The fetched HTML is turned into a token stream (words,
 * spaces, line/paragraph breaks, rules and list bullets with bold, heading,
 * preformatted and link styles), laid out with the Inter faces and drawn with
 * the gfx layer. No CSS, scripts, images or forms; the address bar searches
 * DuckDuckGo Lite for anything that is not an address.
 * Everything is statically sized and every parse step is bounds-checked. */
#include "mintde.h"
#include "tls.h"

#define S (&gfx_screen)
static gfx_rect R(int32_t x, int32_t y, int32_t w, int32_t h) { return gfx_rect_xywh(x, y, w, h); }
static int32_t W(gfx_rect r) { return r.x1 - r.x0; }
static int32_t H(gfx_rect r) { return r.y1 - r.y0; }
static int str_eq(const char *a, const char *b) {
    size_t i = 0;
    for (; a[i] && a[i] == b[i]; ++i) {}
    return a[i] == b[i];
}

#define WEB_RAW_MAX   (1024u * 1024u)
#define WEB_POOL_MAX  (384u * 1024u)
#define WEB_TOK_MAX   32768u
#define WEB_RUN_MAX   32768u
#define WEB_LINK_MAX  512u
#define WEB_HREF_MAX  200u
#define WEB_URL_MAX   256u
#define WEB_HISTORY   16u
#define WEB_MARGIN    28
#define WEB_PRE_CELL  7

enum { TK_WORD = 1, TK_SPACE, TK_BR, TK_PARA, TK_HR, TK_BULLET };
enum { ST_BOLD = 1, ST_H1 = 2, ST_H2 = 4, ST_H3 = 8, ST_PRE = 16 };
enum { WS_IDLE = 0, WS_RESOLVE, WS_CONNECT, WS_TLS, WS_RECEIVE };

typedef struct web_tok { uint8_t kind, style, indent, reserved; int16_t link; uint16_t len; uint32_t off; } web_tok;
typedef struct web_run { uint32_t tok; int32_t x, y; int32_t w; uint8_t font, kind; uint16_t reserved; } web_run;
typedef struct web_url { char host[80]; uint16_t port; uint8_t secure; char path[WEB_URL_MAX - 96]; } web_url;

static uint8_t raw[WEB_RAW_MAX];
static uint32_t raw_len;
static char pool[WEB_POOL_MAX];
static uint32_t pool_len;
static web_tok toks[WEB_TOK_MAX];
static uint32_t tok_count;
static web_run runs[WEB_RUN_MAX];
static uint32_t run_count;
static char links[WEB_LINK_MAX][WEB_HREF_MAX];
static uint32_t link_count;
static int32_t doc_height, layout_width = -1, scroll_y;
static int32_t main_token = -1;          /* first token inside <main>/<article>/role=main */
static int scroll_to_main;
static char page_title[96];
static char address[WEB_URL_MAX];
static size_t address_len;
static int address_focused, address_replace;
static web_url page_url;
static int page_is_internal, page_secure;
static MINT_TLS web_tls;
static char history[WEB_HISTORY][WEB_URL_MAX];
static uint32_t hist_count, hist_pos;
static char status_text[96];
static uint64_t status_until;
static int32_t hover_link = -1, hover_dirty;
static int web_dirty;

static struct {
    uint32_t state, redirects;
    web_url url;
    uint32_t ip;
    int64_t handle;
    uint64_t started, last;
} fetch = {.handle = -1};

/* ------------------------------------------------------------------ small helpers */
static uint64_t web_ms(void) {
    mint_timespec64 t = {0, 0};
    (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static int prefix_ci(const char *s, const char *p) {
    for (size_t i = 0; p[i]; ++i) if (!s[i] || lower(s[i]) != lower(p[i])) return 0;
    return 1;
}
static int mem_prefix_ci(const uint8_t *s, size_t n, const char *p) {
    size_t i = 0;
    for (; p[i]; ++i) if (i >= n || lower((char)s[i]) != lower(p[i])) return 0;
    return 1;
}
static uint64_t web_ms(void);
static void set_status(const char *a, const char *b) {
    copy_text(status_text, sizeof(status_text), a ? a : "");
    if (b) str_append(status_text, sizeof(status_text), b);
    status_until = 0;
    web_dirty = 1;
}

/* ------------------------------------------------------------------ URLs */
/* 1 = http(s) URL, 0 = not a URL. */
static int url_parse(const char *in, web_url *u) {
    const char *p = in;
    u->secure = 0;
    if (prefix_ci(p, "https://")) { p += 8; u->secure = 1; }
    else if (prefix_ci(p, "http://")) p += 7;
    size_t n = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && n + 1u < sizeof(u->host)) u->host[n++] = lower(*p++);
    u->host[n] = '\0';
    if (!n) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = u->host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return 0;
    }
    uint32_t port = u->secure ? 443u : 80u;
    if (*p == ':') {
        ++p; port = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10u + (uint32_t)(*p++ - '0'); if (port > 65535u) return 0; }
        if (!port) return 0;
    }
    u->port = (uint16_t)port;
    size_t m = 0;
    if (*p != '/') u->path[m++] = '/';
    while (*p && *p != '#' && m + 1u < sizeof(u->path)) {
        char c = *p++;
        if (c == ' ') { if (m + 3u >= sizeof(u->path)) break; u->path[m++] = '%'; u->path[m++] = '2'; u->path[m++] = '0'; }
        else u->path[m++] = c;
    }
    u->path[m] = '\0';
    return 1;
}
static void url_format(const web_url *u, char *out, size_t cap) {
    copy_text(out, cap, u->secure ? "https://" : "http://");
    str_append(out, cap, u->host);
    if (u->port != (u->secure ? 443u : 80u)) { char n[8]; format_u64(n, sizeof(n), u->port); str_append(out, cap, ":"); str_append(out, cap, n); }
    str_append(out, cap, u->path);
}
/* Removes "." and ".." segments in place (query string untouched). */
static void path_normalize(char *path, size_t cap) {
    char out[WEB_URL_MAX];
    size_t o = 0, i = 0;
    size_t qpos = 0;
    while (path[qpos] && path[qpos] != '?') ++qpos;
    while (i < qpos) {
        if (path[i] == '/') {
            size_t j = i + 1;
            if (j + 1 <= qpos && path[j] == '.' && (j + 1 == qpos || path[j + 1] == '/')) { i = j + 1; continue; }
            if (j + 2 <= qpos && path[j] == '.' && path[j + 1] == '.' && (j + 2 == qpos || path[j + 2] == '/')) {
                while (o && out[o - 1] != '/') --o;
                if (o) --o;
                i = j + 2; continue;
            }
        }
        if (o + 1u < sizeof(out)) out[o++] = path[i];
        ++i;
    }
    if (!o) out[o++] = '/';
    while (path[i] && o + 1u < sizeof(out)) out[o++] = path[i++];
    out[o] = '\0';
    copy_text(path, cap, out);
}
/* Resolves href against the current page into an absolute address string. */
static void url_resolve(const char *href, char *out, size_t cap) {
    while (*href == ' ' || *href == '\n' || *href == '\t') ++href;
    if (prefix_ci(href, "http://") || prefix_ci(href, "https://") || prefix_ci(href, "about:") ||
        prefix_ci(href, "mailto:") || prefix_ci(href, "javascript:")) { copy_text(out, cap, href); return; }
    web_url u = page_url;
    if (href[0] == '/' && href[1] == '/') { copy_text(out, cap, page_url.secure ? "https:" : "http:"); str_append(out, cap, href); return; }
    if (href[0] == '/') copy_text(u.path, sizeof(u.path), href);
    else if (href[0] == '?') {
        size_t n = 0; while (u.path[n] && u.path[n] != '?') ++n;
        u.path[n] = '\0'; str_append(u.path, sizeof(u.path), href);
    } else if (href[0] == '#' || !href[0]) {
        /* same page */
    } else {
        size_t n = 0, last = 0;
        while (u.path[n] && u.path[n] != '?') { if (u.path[n] == '/') last = n; ++n; }
        u.path[last + 1u] = '\0';
        str_append(u.path, sizeof(u.path), href);
    }
    size_t h = 0; while (u.path[h] && u.path[h] != '#') ++h;
    u.path[h] = '\0';
    path_normalize(u.path, sizeof(u.path));
    url_format(&u, out, cap);
}

/* ------------------------------------------------------------------ HTML -> tokens */
static uint8_t cur_style, cur_indent;
static int16_t cur_link = -1;
static int pending_space, in_pre, in_title, skip_depth;
static char skip_tag[12];
static char word[256];
static size_t word_len;
static uint8_t list_ordered[8];
static uint16_t list_counter[8];
static uint32_t list_depth;

static void tok_emit(uint8_t kind, const char *text, size_t len) {
    if (tok_count >= WEB_TOK_MAX) return;
    if (kind == TK_PARA && tok_count && (toks[tok_count - 1].kind == TK_PARA)) return;
    if (kind == TK_SPACE && (!tok_count || toks[tok_count - 1].kind != TK_WORD)) return;
    if (len > 0xFFFFu || pool_len + len > WEB_POOL_MAX) return;
    web_tok *t = &toks[tok_count++];
    t->kind = kind; t->style = cur_style; t->indent = cur_indent; t->link = cur_link;
    t->off = pool_len; t->len = (uint16_t)len;
    for (size_t i = 0; i < len; ++i) pool[pool_len++] = text[i];
}
static void flush_word(void) {
    if (!word_len) return;
    tok_emit(TK_WORD, word, word_len);
    word_len = 0;
}
static void put_byte(char c) {
    if (in_title) {
        size_t n = text_length(page_title);
        if (c == '\n' || c == '\t' || c == '\r') c = ' ';
        if (!(c == ' ' && (!n || page_title[n - 1] == ' ')) && n + 1u < sizeof(page_title)) { page_title[n] = c; page_title[n + 1] = '\0'; }
        return;
    }
    if (skip_depth) return;
    if (in_pre) {
        if (c == '\r') return;
        if (c == '\n') { flush_word(); tok_emit(TK_BR, 0, 0); return; }
        if (c == '\t') { for (int i = 0; i < 4; ++i) put_byte(' '); return; }
        if (word_len + 1u >= sizeof(word)) flush_word();
        word[word_len++] = c;
        return;
    }
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') { flush_word(); pending_space = 1; return; }
    if (pending_space) { tok_emit(TK_SPACE, 0, 0); pending_space = 0; }
    if (word_len + 4u >= sizeof(word)) flush_word();
    word[word_len++] = c;
}
static void put_utf8(uint32_t cp) {
    if (cp == 0xA0u) { put_byte(' '); return; }
    if (cp == 0xADu || cp == 0x200Bu) return;
    if (cp < 0x80u) { put_byte((char)cp); return; }
    if (cp < 0x800u) { put_byte((char)(0xC0u | (cp >> 6))); put_byte((char)(0x80u | (cp & 0x3Fu))); return; }
    if (cp < 0x10000u) {
        put_byte((char)(0xE0u | (cp >> 12))); put_byte((char)(0x80u | ((cp >> 6) & 0x3Fu))); put_byte((char)(0x80u | (cp & 0x3Fu)));
        return;
    }
    put_byte('?');
}
static void block_break(void) { flush_word(); pending_space = 0; tok_emit(TK_PARA, 0, 0); }
static void line_break(void) { flush_word(); pending_space = 0; tok_emit(TK_BR, 0, 0); }

static const struct { const char *name; uint16_t cp; } entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9},
    {"reg", 0xAE}, {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"bull", 0x2022},
    {"middot", 0xB7}, {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"laquo", '<'},
    {"raquo", '>'}, {"times", 0xD7}, {"divide", 0xF7}, {"deg", 0xB0}, {"euro", 0x20AC}, {"pound", 0xA3},
    {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"minus", 0x2212},
};
/* Decodes an entity at s (after '&'); returns bytes consumed or 0. */
static size_t entity(const uint8_t *s, size_t n, uint32_t *cp) {
    size_t i = 0;
    if (n && s[0] == '#') {
        uint32_t v = 0; int hex = n > 1 && (s[1] == 'x' || s[1] == 'X');
        i = hex ? 2u : 1u;
        size_t digits = 0;
        while (i < n && digits < 7u) {
            uint8_t c = s[i];
            uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') : (hex && lower((char)c) >= 'a' && lower((char)c) <= 'f') ? (uint32_t)(lower((char)c) - 'a' + 10) : 99u;
            if (d == 99u) break;
            v = v * (hex ? 16u : 10u) + d; ++i; ++digits;
        }
        if (!digits) return 0;
        if (i < n && s[i] == ';') ++i;
        *cp = v ? v : '?';
        return i;
    }
    while (i < n && i < 10u && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) ++i;
    if (!i || i >= n || s[i] != ';') return 0;
    for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); ++e) {
        size_t len = text_length(entities[e].name);
        if (len == i && mem_prefix_ci(s, i, entities[e].name)) { *cp = entities[e].cp; return i + 1u; }
    }
    *cp = '?';
    return i + 1u;
}

static int tag_is(const char *name, const char *t) { return str_eq(name, t); }

static void handle_tag(const char *name, int closing, const char *href, const char *alt) {
    if (skip_depth) {
        if (closing && tag_is(name, skip_tag)) skip_depth = 0;
        return;
    }
    if (!closing && (tag_is(name, "script") || tag_is(name, "style") || tag_is(name, "svg") ||
                     tag_is(name, "template") || tag_is(name, "select") || tag_is(name, "iframe"))) {
        flush_word(); copy_text(skip_tag, sizeof(skip_tag), name); skip_depth = 1; return;
    }
    if (tag_is(name, "title")) { flush_word(); in_title = !closing; return; }
    flush_word();
    if (tag_is(name, "br")) { line_break(); return; }
    if (tag_is(name, "hr")) { block_break(); tok_emit(TK_HR, 0, 0); return; }
    if (tag_is(name, "p") || tag_is(name, "div") || tag_is(name, "section") || tag_is(name, "article") ||
        tag_is(name, "header") || tag_is(name, "footer") || tag_is(name, "main") || tag_is(name, "nav") ||
        tag_is(name, "form") || tag_is(name, "table") || tag_is(name, "center") || tag_is(name, "figure") ||
        tag_is(name, "aside") || tag_is(name, "address") || tag_is(name, "dl") || tag_is(name, "dt") ||
        tag_is(name, "fieldset") || tag_is(name, "details") || tag_is(name, "summary") || tag_is(name, "body")) {
        block_break(); return;
    }
    if (tag_is(name, "tr")) { line_break(); return; }
    if (tag_is(name, "td") || tag_is(name, "th")) { if (!closing) { pending_space = 1; } return; }
    if (tag_is(name, "dd") || tag_is(name, "blockquote")) {
        block_break();
        if (!closing && cur_indent < 6u) ++cur_indent; else if (closing && cur_indent) --cur_indent;
        return;
    }
    if (tag_is(name, "ul") || tag_is(name, "ol") || tag_is(name, "menu")) {
        block_break();
        if (!closing && list_depth < 8u) { list_ordered[list_depth] = tag_is(name, "ol"); list_counter[list_depth] = 0; ++list_depth; if (cur_indent < 6u) ++cur_indent; }
        else if (closing && list_depth) { --list_depth; if (cur_indent) --cur_indent; }
        return;
    }
    if (tag_is(name, "li")) {
        if (closing) return;
        block_break();
        char bullet[8] = "\xE2\x80\xA2";
        if (list_depth && list_ordered[list_depth - 1u]) {
            format_u64(bullet, sizeof(bullet), ++list_counter[list_depth - 1u]); str_append(bullet, sizeof(bullet), ".");
        }
        uint8_t style = cur_style; cur_style = 0;
        tok_emit(TK_BULLET, bullet, text_length(bullet));
        cur_style = style;
        return;
    }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        block_break();
        cur_style &= (uint8_t)~(ST_H1 | ST_H2 | ST_H3);
        if (!closing) cur_style |= name[1] == '1' ? ST_H1 : name[1] == '2' ? ST_H2 : ST_H3;
        return;
    }
    if (tag_is(name, "b") || tag_is(name, "strong")) {
        if (closing) cur_style &= (uint8_t)~ST_BOLD; else cur_style |= ST_BOLD;
        return;
    }
    if (tag_is(name, "pre") || tag_is(name, "listing") || tag_is(name, "xmp")) {
        block_break();
        in_pre = !closing;
        if (closing) cur_style &= (uint8_t)~ST_PRE; else cur_style |= ST_PRE;
        return;
    }
    if (tag_is(name, "a")) {
        if (closing) { cur_link = -1; return; }
        if (href && href[0] && link_count < WEB_LINK_MAX) {
            copy_text(links[link_count], WEB_HREF_MAX, href);
            cur_link = (int16_t)link_count++;
        }
        return;
    }
    if (tag_is(name, "img") && alt && alt[0]) {
        put_byte('['); for (size_t i = 0; alt[i]; ++i) put_byte(alt[i]); put_byte(']');
        return;
    }
}

/* Reads one attribute value into out (decoding &amp;). Returns new index. */
static size_t read_attr_value(const uint8_t *b, size_t n, size_t i, char *out, size_t cap) {
    size_t o = 0;
    uint8_t quote = 0;
    if (i < n && (b[i] == '"' || b[i] == '\'')) quote = b[i++];
    while (i < n) {
        uint8_t c = b[i];
        if (quote ? c == quote : (c == ' ' || c == '>' || c == '\t' || c == '\n' || c == '\r')) break;
        if (c == '&' && i + 4u < n && mem_prefix_ci(b + i, n - i, "&amp;")) { c = '&'; i += 4; }
        if (o + 1u < cap && c != '\n' && c != '\r' && c != '\t') out[o++] = (char)c;
        ++i;
    }
    if (quote && i < n) ++i;
    out[o] = '\0';
    return i;
}

static void parse_html(const uint8_t *b, size_t n) {
    static char href[WEB_HREF_MAX], alt[96], role[16];
    for (size_t i = 0; i < n;) {
        uint8_t c = b[i];
        if (c == '<') {
            if (mem_prefix_ci(b + i, n - i, "<!--")) {
                size_t j = i + 4;
                while (j + 2 < n && !(b[j] == '-' && b[j + 1] == '-' && b[j + 2] == '>')) ++j;
                i = j + 3; continue;
            }
            size_t j = i + 1;
            int closing = 0;
            if (j < n && b[j] == '/') { closing = 1; ++j; }
            if (j >= n || !((b[j] >= 'a' && b[j] <= 'z') || (b[j] >= 'A' && b[j] <= 'Z') || b[j] == '!' || b[j] == '?')) {
                put_byte('<'); ++i; continue;                     /* a literal '<' */
            }
            char name[12]; size_t m = 0;
            while (j < n && ((b[j] >= 'a' && b[j] <= 'z') || (b[j] >= 'A' && b[j] <= 'Z') || (b[j] >= '0' && b[j] <= '9') || b[j] == '!' || b[j] == '?')) {
                if (m + 1u < sizeof(name)) name[m++] = lower((char)b[j]);
                ++j;
            }
            name[m] = '\0';
            href[0] = alt[0] = role[0] = '\0';
            while (j < n && b[j] != '>') {
                if (b[j] == ' ' || b[j] == '\t' || b[j] == '\n' || b[j] == '\r' || b[j] == '/') { ++j; continue; }
                char attr[12]; size_t a = 0;
                while (j < n && b[j] != '=' && b[j] != '>' && b[j] != ' ' && b[j] != '\t' && b[j] != '\n' && b[j] != '\r') {
                    if (a + 1u < sizeof(attr)) attr[a++] = lower((char)b[j]);
                    ++j;
                }
                attr[a] = '\0';
                while (j < n && (b[j] == ' ' || b[j] == '\t' || b[j] == '\n' || b[j] == '\r')) ++j;
                if (j < n && b[j] == '=') {
                    ++j;
                    while (j < n && (b[j] == ' ' || b[j] == '\t' || b[j] == '\n' || b[j] == '\r')) ++j;
                    if (tag_is(attr, "href")) j = read_attr_value(b, n, j, href, sizeof(href));
                    else if (tag_is(attr, "role")) j = read_attr_value(b, n, j, role, sizeof(role));
                    else if (tag_is(attr, "alt")) j = read_attr_value(b, n, j, alt, sizeof(alt));
                    else { char ignored[4]; j = read_attr_value(b, n, j, ignored, sizeof(ignored)); }
                }
            }
            i = j < n ? j + 1 : n;
            if (name[0] == '!' || name[0] == '?') continue;       /* doctype, processing instruction */
            if (!closing && main_token < 0 && !skip_depth &&
                (tag_is(name, "main") || tag_is(name, "article") || tag_is(role, "main"))) main_token = (int32_t)tok_count;
            handle_tag(name, closing, href, alt);
            continue;
        }
        if (c == '&') {
            uint32_t cp = 0;
            size_t used = entity(b + i + 1, n - i - 1, &cp);
            if (used) { put_utf8(cp); i += used + 1; continue; }
        }
        if (c == 0xC2u && i + 1 < n && (b[i + 1] == 0xA0u || b[i + 1] == 0xADu)) {   /* NBSP, soft hyphen */
            if (b[i + 1] == 0xA0u) put_byte(' ');
            i += 2; continue;
        }
        if (c == 0xE2u && i + 2 < n && b[i + 1] == 0x80u && (b[i + 2] == 0x8Bu || b[i + 2] == 0x8Cu || b[i + 2] == 0x8Du)) {
            i += 3; continue;                                                           /* zero-width characters */
        }
        put_byte((char)c);
        ++i;
    }
    flush_word();
}

static void parse_plain(const uint8_t *b, size_t n) {
    cur_style = ST_PRE; in_pre = 1;
    for (size_t i = 0; i < n; ++i) put_byte((char)b[i]);
    flush_word();
}

static void reset_document(void) {
    tok_count = pool_len = link_count = run_count = 0;
    cur_style = cur_indent = 0; cur_link = -1;
    pending_space = in_pre = in_title = skip_depth = 0;
    word_len = 0; list_depth = 0;
    page_title[0] = '\0';
    scroll_y = 0; layout_width = -1; doc_height = 0;
    main_token = -1; scroll_to_main = 0;
    hover_link = -1;
}

/* ------------------------------------------------------------------ layout */
static int font_for(uint8_t style) {
    if (style & ST_H1) return FONT_TITLE;
    if (style & ST_H2) return FONT_HEADLINE;
    if (style & ST_H3) return FONT_BODY_BOLD;
    if (style & ST_PRE) return FONT_SMALL;
    if (style & ST_BOLD) return FONT_BODY_BOLD;
    return FONT_BODY;
}
static void tok_text(const web_tok *t, char *out, size_t cap) {
    size_t n = t->len < cap - 1u ? t->len : cap - 1u;
    for (size_t i = 0; i < n; ++i) out[i] = pool[t->off + i];
    out[n] = '\0';
}
static int32_t utf8_chars(const char *s) { int32_t n = 0; for (; *s; ++s) if (((uint8_t)*s & 0xC0u) != 0x80u) ++n; return n; }
static int32_t tok_width(const web_tok *t, int font) {
    char text[260];
    tok_text(t, text, sizeof(text));
    if (t->style & ST_PRE) return utf8_chars(text) * WEB_PRE_CELL;
    return gfx_text_width(font, text);
}

static int32_t line_top, line_x;
static uint32_t line_first;
static int line_empty, just_para;

static void finish_line(void) {
    int32_t ascent = gfx_font_ascent(FONT_BODY), height = gfx_font_line(FONT_BODY) + 4;
    if (run_count > line_first) {
        ascent = 0; height = 0;
        for (uint32_t i = line_first; i < run_count; ++i) {
            int32_t a = gfx_font_ascent(runs[i].font), h = gfx_font_line(runs[i].font) + 4;
            if (a > ascent) ascent = a;
            if (h > height) height = h;
        }
    }
    for (uint32_t i = line_first; i < run_count; ++i) runs[i].y = line_top + ascent;
    line_top += height;
    line_first = run_count;
    line_empty = 1;
}
static void add_run(uint32_t tok, int32_t x, int32_t w, int font, uint8_t kind) {
    if (run_count >= WEB_RUN_MAX) return;
    web_run *r = &runs[run_count++];
    r->tok = tok; r->x = x; r->w = w; r->font = (uint8_t)font; r->kind = kind; r->y = 0;
}
static void layout(int32_t width) {
    layout_width = width;
    run_count = 0; line_top = 18; line_first = 0; line_empty = 1; just_para = 1; line_x = WEB_MARGIN;
    int32_t right = width - WEB_MARGIN;
    for (uint32_t i = 0; i < tok_count; ++i) {
        const web_tok *t = &toks[i];
        int font = font_for(t->style);
        int32_t left = WEB_MARGIN + (int32_t)t->indent * 24;
        switch (t->kind) {
        case TK_WORD: {
            int32_t w = tok_width(t, font);
            if (!line_empty && line_x + w > right) finish_line();
            if (line_empty) line_x = left;
            add_run(i, line_x, w, font, 0);
            line_x += w; line_empty = 0; just_para = 0;
            break;
        }
        case TK_SPACE:
            if (!line_empty) line_x += (t->style & ST_PRE) ? WEB_PRE_CELL : gfx_text_width(font, " ");
            break;
        case TK_BR:
            finish_line(); just_para = 0;
            break;
        case TK_PARA:
            if (!line_empty) finish_line();
            if (!just_para) line_top += 8;
            just_para = 1;
            break;
        case TK_HR:
            if (!line_empty) finish_line();
            add_run(i, WEB_MARGIN, right - WEB_MARGIN, FONT_BODY, 1);
            runs[run_count - 1].y = line_top + 8;
            line_first = run_count; line_top += 18;
            break;
        case TK_BULLET: {
            if (!line_empty) finish_line();
            int32_t w = tok_width(t, FONT_BODY);
            add_run(i, left - 8 - w, w, FONT_BODY, 2);
            line_x = left; line_empty = 1;
            break;
        }
        default: break;
        }
    }
    if (!line_empty || run_count > line_first) finish_line();
    doc_height = line_top + 24;
}

/* ------------------------------------------------------------------ pages */
static void html_escape_append(char *out, size_t cap, const char *text) {
    for (size_t i = 0; text[i]; ++i) {
        char c = text[i];
        if (c == '<') str_append(out, cap, "&lt;");
        else if (c == '>') str_append(out, cap, "&gt;");
        else if (c == '&') str_append(out, cap, "&amp;");
        else if (c == '"') str_append(out, cap, "&quot;");
        else { char s[2] = {c, 0}; str_append(out, cap, s); }
    }
}
static void show_html(const char *html) {
    reset_document();
    parse_html((const uint8_t *)html, text_length(html));
    web_dirty = 1;
}
static void show_start_page(void) {
    page_is_internal = 1;
    copy_text(address, sizeof(address), ""); address_len = 0;
    show_html("<title>Start Page</title>"
              "<h1>Welcome to the Web on MintOS</h1>"
              "<p>Type an address above, or a few words to search with DuckDuckGo.</p>"
              "<p>MintOS loads pages over HTTP and secure HTTPS. Secure connections use MintOS\xE2\x80\x99s own TLS 1.3, "
              "and every certificate is checked against the Mozilla list of trusted authorities. "
              "Pages are shown as readable text: there are no scripts, styles or images yet.</p>"
              "<h3>Try these</h3>"
              "<ul>"
              "<li><a href=\"https://en.wikipedia.org/wiki/Special:Random\">A random Wikipedia article</a></li>"
              "<li><a href=\"https://text.npr.org/\">NPR</a> \xE2\x80\x94 text-only news</li>"
              "<li><a href=\"https://lite.cnn.com/\">CNN Lite</a> \xE2\x80\x94 headlines without the clutter</li>"
              "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
              "<li><a href=\"http://68k.news/\">68k.news</a> \xE2\x80\x94 headlines for vintage computers</li>"
              "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first website</a> \xE2\x80\x94 CERN, 1991</li>"
              "</ul>"
              "<h3>Keyboard</h3>"
              "<p>\xE2\x8C\x98L address field \xC2\xB7 \xE2\x8C\x98R reload \xC2\xB7 \xE2\x8C\x98[ back \xC2\xB7 \xE2\x8C\x98] forward \xC2\xB7 "
              "Space and arrow keys scroll.</p>");
    set_status("", 0);
}
static void show_error(const char *heading, const char *detail, const char *suggestion_url, const char *suggestion_text) {
    static char html[1400];
    copy_text(html, sizeof(html), "<title>");
    html_escape_append(html, sizeof(html), heading);
    str_append(html, sizeof(html), "</title><h2>");
    html_escape_append(html, sizeof(html), heading);
    str_append(html, sizeof(html), "</h2><p>");
    html_escape_append(html, sizeof(html), detail);
    str_append(html, sizeof(html), "</p>");
    if (suggestion_url) {
        str_append(html, sizeof(html), "<p><a href=\"");
        html_escape_append(html, sizeof(html), suggestion_url);
        str_append(html, sizeof(html), "\">");
        html_escape_append(html, sizeof(html), suggestion_text);
        str_append(html, sizeof(html), "</a></p>");
    }
    str_append(html, sizeof(html), "<p><a href=\"about:start\">Start Page</a></p>");
    show_html(html);
}

/* ------------------------------------------------------------------ loading */
static void fetch_stop(void) {
    if (fetch.handle >= 0 && fetch.url.secure && (fetch.state == WS_RECEIVE || fetch.state == WS_TLS)) mint_tls_close(&web_tls);
    if (fetch.handle >= 0) (void)mint_tcp_close(fetch.handle);
    fetch.handle = -1;
    fetch.state = WS_IDLE;
}
static void history_push(const char *url) {
    if (hist_count && str_eq(history[hist_pos], url)) return;
    if (hist_count) hist_count = hist_pos + 1u;
    if (hist_count == WEB_HISTORY) {
        for (uint32_t i = 1; i < WEB_HISTORY; ++i) copy_text(history[i - 1], WEB_URL_MAX, history[i]);
        --hist_count;
    }
    copy_text(history[hist_count], WEB_URL_MAX, url);
    hist_pos = hist_count++;
}
static void start_fetch(const web_url *u) {
    fetch_stop();
    fetch.url = *u; fetch.state = WS_RESOLVE; fetch.started = fetch.last = web_ms();
    page_is_internal = 0; page_secure = 0;
    set_status("Looking up ", u->host);
}

static void navigate(const char *input, int add_history, int is_redirect) {
    char text[WEB_URL_MAX];
    size_t n = 0;
    while (*input == ' ') ++input;
    while (input[n] && n + 1u < sizeof(text)) { text[n] = input[n]; ++n; }
    while (n && text[n - 1] == ' ') --n;
    text[n] = '\0';
    if (!is_redirect) fetch.redirects = 0;
    address_focused = 0;
    if (!n || prefix_ci(text, "about:")) {
        fetch_stop();
        if (add_history) history_push("about:start");
        show_start_page();
        return;
    }
    if (prefix_ci(text, "mailto:") || prefix_ci(text, "javascript:")) { set_status("That kind of link is not supported", 0); return; }
    int has_space = 0, has_dot = 0, has_scheme = prefix_ci(text, "http://") || prefix_ci(text, "https://");
    for (size_t i = 0; i < n; ++i) { if (text[i] == ' ') has_space = 1; if (text[i] == '.' || text[i] == ':' || text[i] == '/') has_dot = 1; }
    char url[WEB_URL_MAX];
    if (!has_scheme && (has_space || !has_dot)) {
        static const char hx[] = "0123456789ABCDEF";
        copy_text(url, sizeof(url), "https://lite.duckduckgo.com/lite/?q=");
        size_t m = text_length(url);
        for (size_t i = 0; i < n && m + 4u < sizeof(url); ++i) {
            char c = text[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_') url[m++] = c;
            else if (c == ' ') url[m++] = '+';
            else { url[m++] = '%'; url[m++] = hx[((uint8_t)c) >> 4]; url[m++] = hx[c & 15]; }
        }
        url[m] = '\0';
    } else copy_text(url, sizeof(url), text);
    web_url u;
    int ok = url_parse(url, &u);
    if (!ok) { set_status("That address is not valid", 0); return; }
    url_format(&u, address, sizeof(address)); address_len = text_length(address);
    if (add_history) history_push(address);
    else if (is_redirect && hist_count) copy_text(history[hist_pos], WEB_URL_MAX, address);
    start_fetch(&u);
}

static const char *header_value(const char *headers, size_t n, const char *name, char *out, size_t cap) {
    out[0] = '\0';
    size_t len = text_length(name);
    for (size_t i = 0; i < n; ++i) {
        if ((i == 0 || headers[i - 1] == '\n') && i + len < n && prefix_ci(headers + i, name) && headers[i + len] == ':') {
            size_t j = i + len + 1, o = 0;
            while (j < n && headers[j] == ' ') ++j;
            while (j < n && headers[j] != '\r' && headers[j] != '\n' && o + 1u < cap) out[o++] = headers[j++];
            out[o] = '\0';
            return out;
        }
    }
    return 0;
}
/* In-place chunked transfer decoding; returns the decoded length. */
static size_t dechunk(uint8_t *b, size_t n) {
    size_t in = 0, out = 0;
    while (in < n) {
        size_t size = 0, digits = 0;
        while (in < n) {
            uint8_t c = b[in];
            uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') : (lower((char)c) >= 'a' && lower((char)c) <= 'f') ? (uint32_t)(lower((char)c) - 'a' + 10) : 99u;
            if (d == 99u) break;
            if (size > (WEB_RAW_MAX >> 4)) return out;
            size = size * 16u + d; ++in; ++digits;
        }
        while (in < n && b[in] != '\n') ++in;
        ++in;
        if (!digits || !size) break;
        if (size > n - in) size = n - in;
        for (size_t i = 0; i < size; ++i) b[out++] = b[in + i];
        in += size;
        while (in < n && (b[in] == '\r' || b[in] == '\n')) ++in;
    }
    return out;
}

static void finish_response(void) {
    int secure = fetch.url.secure;
    fetch_stop();
    page_url = fetch.url;          /* base for relative redirects and links */
    size_t head_end = 0;
    for (size_t i = 0; i + 3 < raw_len; ++i) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') { head_end = i + 4; break; }
        if (raw[i] == '\n' && raw[i + 1] == '\n') { head_end = i + 2; break; }
    }
    if (!head_end || !mem_prefix_ci(raw, raw_len, "HTTP/")) {
        show_error("The server sent an invalid response", "The reply was not an HTTP response.", 0, 0);
        set_status("Invalid response", 0);
        return;
    }
    uint32_t code = 0;
    size_t i = 5;
    while (i < head_end && raw[i] != ' ') ++i;
    while (i < head_end && raw[i] == ' ') ++i;
    while (i < head_end && raw[i] >= '0' && raw[i] <= '9') code = code * 10u + (uint32_t)(raw[i++] - '0');
    const char *headers = (const char *)raw;
    char value[WEB_URL_MAX];
    if (code >= 300u && code < 400u && header_value(headers, head_end, "location", value, sizeof(value))) {
        char target[WEB_URL_MAX];
        url_resolve(value, target, sizeof(target));
        if (++fetch.redirects > 5u) { show_error("Too many redirects", "The site keeps redirecting.", 0, 0); return; }
        navigate(target, 0, 1);
        return;
    }
    uint8_t *body = raw + head_end;
    size_t body_len = raw_len - head_end;
    char te[32], ct[64];
    if (header_value(headers, head_end, "transfer-encoding", te, sizeof(te)) && prefix_ci(te, "chunked"))
        body_len = dechunk(body, body_len);
    (void)header_value(headers, head_end, "content-type", ct, sizeof(ct));
    reset_document();
    if (!ct[0] || prefix_ci(ct, "text/html") || prefix_ci(ct, "application/xhtml")) { parse_html(body, body_len); scroll_to_main = 1; }
    else if (prefix_ci(ct, "text/")) parse_plain(body, body_len);
    else {
        static char msg[160];
        copy_text(msg, sizeof(msg), "The file is of type ");
        str_append(msg, sizeof(msg), ct);
        str_append(msg, sizeof(msg), ", which the browser cannot show.");
        show_error("This file cannot be displayed", msg, 0, 0);
    }
    if (!page_title[0]) copy_text(page_title, sizeof(page_title), fetch.url.host);
    page_secure = secure;
    char size[24], codes[8];
    format_bytes(size, sizeof(size), body_len);
    format_u64(codes, sizeof(codes), code);
    copy_text(status_text, sizeof(status_text), code >= 400u ? "HTTP " : "Done \xE2\x80\x94 ");
    str_append(status_text, sizeof(status_text), code >= 400u ? codes : size);
    if (raw_len >= WEB_RAW_MAX) str_append(status_text, sizeof(status_text), " (page truncated)");
    status_until = web_ms() + 2500u;
    web_dirty = 1;
}

static void fail(const char *heading, const char *detail) {
    fetch_stop();
    show_error(heading, detail, 0, 0);
    set_status(heading, 0);
}

/* TLS runs over the kernel TCP socket; the callbacks never block. */
static int64_t tls_tcp_send(void *ctx, const void *data, size_t len) {
    int64_t r = mint_tcp_send(*(int64_t *)ctx, data, len > 4096u ? 4096u : len);
    return r < 0 ? -1 : r;
}
static int64_t tls_tcp_recv(void *ctx, void *out, size_t cap) {
    int64_t r = mint_tcp_recv(*(int64_t *)ctx, out, cap > 4096u ? 4096u : cap);
    if (r == -MINT_EAGAIN) return 0;
    return r > 0 ? r : -1;
}
static int64_t web_now(void) {
    mint_timespec64 t = {0, 0};
    return mint_clock_gettime(MINT_CLOCK_REALTIME, &t) == 0 ? t.tv_sec : 0;
}
static int send_request(void) {
    static char request[600];
    copy_text(request, sizeof(request), "GET ");
    str_append(request, sizeof(request), fetch.url.path);
    str_append(request, sizeof(request), " HTTP/1.0\r\nHost: ");
    str_append(request, sizeof(request), fetch.url.host);
    if (fetch.url.port != (fetch.url.secure ? 443u : 80u)) { char p[8]; format_u64(p, sizeof(p), fetch.url.port); str_append(request, sizeof(request), ":"); str_append(request, sizeof(request), p); }
    str_append(request, sizeof(request), "\r\nUser-Agent: Mozilla/5.0 (compatible; MintOS26 Web/1.1)\r\n"
                                         "Accept: text/html, text/plain;q=0.9, */*;q=0.5\r\nAccept-Encoding: identity\r\n"
                                         "Connection: close\r\n\r\n");
    int64_t want = (int64_t)text_length(request);
    int64_t sent = fetch.url.secure ? mint_tls_write(&web_tls, request, (size_t)want) : mint_tcp_send(fetch.handle, request, (size_t)want);
    return sent == want;
}
static void tls_failed(void) {
    static char detail[300];
    if (web_tls.error == MINT_TLS_ERR_CERT) {
        copy_text(detail, sizeof(detail), "The certificate presented by ");
        str_append(detail, sizeof(detail), fetch.url.host);
        str_append(detail, sizeof(detail), " could not be verified: ");
        str_append(detail, sizeof(detail), mint_tls_error_text(&web_tls));
        str_append(detail, sizeof(detail), ". Someone could be impersonating the site, so MintOS did not load it and sent no request.");
        fetch_stop();
        show_error("This connection is not private", detail, 0, 0);
        set_status("Certificate not trusted", 0);
    } else {
        copy_text(detail, sizeof(detail), "MintOS could not set up an encrypted connection to ");
        str_append(detail, sizeof(detail), fetch.url.host);
        str_append(detail, sizeof(detail), ": ");
        str_append(detail, sizeof(detail), mint_tls_error_text(&web_tls));
        str_append(detail, sizeof(detail), ".");
        fetch_stop();
        show_error("Secure connection failed", detail, 0, 0);
        set_status("Secure connection failed", 0);
    }
}

/* Advances the load; returns 1 when the window needs repainting. */
int web_background(void) {
    int dirty = web_dirty || hover_dirty;
    web_dirty = 0; hover_dirty = 0;
    uint64_t now = web_ms();
    if (status_until && now > status_until) { status_until = 0; status_text[0] = '\0'; dirty = 1; }
    if (fetch.state == WS_IDLE) return dirty;
    if (fetch.state == WS_RESOLVE) {
        int64_t rc = mint_net_resolve_host(fetch.url.host, &fetch.ip);
        if (rc == -MINT_EAGAIN) { if (now - fetch.started > 10000u) fail("Can\xE2\x80\x99t find the server", "The name lookup timed out."); return dirty || web_dirty; }
        if (rc == -MINT_ENETDOWN) { fail("You are not connected to the Internet", "Connect in the Network app or Settings, then reload."); return 1; }
        if (rc < 0) { fail("Can\xE2\x80\x99t find the server", "The name does not exist or the DNS server did not answer."); return 1; }
        int64_t h = mint_tcp_connect(fetch.ip, fetch.url.port);
        if (h < 0) { fail("Can\xE2\x80\x99t open the page", "No connection could be opened."); return 1; }
        fetch.handle = h; fetch.state = WS_CONNECT; fetch.last = now;
        set_status("Connecting to ", fetch.url.host);
        return 1;
    }
    if (fetch.state == WS_CONNECT) {
        int64_t st = mint_tcp_state(fetch.handle);
        if (st == MINT_TCP_STATE_ESTABLISHED) {
            if (fetch.url.secure) {
                MINT_TLS_IO io = {&fetch.handle, tls_tcp_send, tls_tcp_recv};
                mint_tls_init(&web_tls, &io, fetch.url.host, web_now());
                fetch.state = WS_TLS; fetch.last = now;
                set_status("Securing the connection to ", fetch.url.host);
                return 1;
            }
            if (!send_request()) { fail("Can\xE2\x80\x99t open the page", "The request could not be sent."); return 1; }
            raw_len = 0; fetch.state = WS_RECEIVE; fetch.last = now;
            set_status("Waiting for ", fetch.url.host);
            return 1;
        }
        if (st != MINT_TCP_STATE_CONNECTING) {
            fail("Can\xE2\x80\x99t connect to the server", st == MINT_TCP_STATE_REFUSED ? "The server refused the connection." :
                 st == MINT_TCP_STATE_UNREACHABLE ? "The server is unreachable." : "The connection failed.");
            return 1;
        }
        if (now - fetch.last > 15000u) { fail("Can\xE2\x80\x99t connect to the server", "The connection timed out."); return 1; }
        return dirty;
    }
    if (fetch.state == WS_TLS) {
        int rc = mint_tls_step(&web_tls);
        if (rc == MINT_TLS_AGAIN) {
            if (now - fetch.last > 20000u) { fail("Secure connection failed", "The secure handshake timed out."); return 1; }
            return dirty;
        }
        if (rc != MINT_TLS_DONE) { tls_failed(); return 1; }
        if (!send_request()) { fail("Can\xE2\x80\x99t open the page", "The request could not be sent."); return 1; }
        raw_len = 0; fetch.state = WS_RECEIVE; fetch.last = now;
        set_status("Waiting for ", fetch.url.host);
        return 1;
    }
    /* receive */
    for (uint32_t burst = 0; burst < 32u; ++burst) {
        if (raw_len >= WEB_RAW_MAX) { finish_response(); return 1; }
        size_t room = WEB_RAW_MAX - raw_len;
        int64_t n;
        if (fetch.url.secure) {
            n = mint_tls_read(&web_tls, raw + raw_len, room > 16384u ? 16384u : room);
            if (n == MINT_TLS_WOULD_BLOCK) n = -MINT_EAGAIN;
            else if (n < 0 && !raw_len) { tls_failed(); return 1; }
        } else n = mint_tcp_recv(fetch.handle, raw + raw_len, room > 4096u ? 4096u : room);
        if (n > 0) { raw_len += (uint32_t)n; fetch.last = now; continue; }
        if (n == -MINT_EAGAIN) {
            if (now - fetch.last > 20000u) { if (raw_len) finish_response(); else fail("The page took too long", "The server stopped answering."); return 1; }
            static uint32_t shown;
            if (shown != raw_len / 4096u) {
                shown = raw_len / 4096u;
                char size[24]; format_bytes(size, sizeof(size), raw_len);
                set_status("Loading\xE2\x80\xA6 ", size);
                return 1;
            }
            return dirty;
        }
        if (n == 0 || raw_len) { finish_response(); return 1; }
        fail("The connection was lost", "The server closed the connection before sending the page.");
        return 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ view */
static gfx_rect address_rect(const mde_layout *l) {
    int32_t x0 = l->toolbar.x0 + 160, x1 = l->toolbar.x1 - 24;
    if (x1 - x0 > 720) { int32_t mid = (x0 + x1) / 2; x0 = mid - 360; x1 = mid + 360; }
    return R(x0, l->toolbar.y0 + 12, x1 - x0, 28);
}
static gfx_rect back_rect(const mde_layout *l) { return R(l->toolbar.x0 + 86, l->toolbar.y0 + 12, 28, 28); }
static gfx_rect forward_rect(const mde_layout *l) { return R(l->toolbar.x0 + 116, l->toolbar.y0 + 12, 28, 28); }

const char *web_title(void) { return page_title[0] ? page_title : "Web"; }

void web_opened(void) {
    if (!hist_count && fetch.state == WS_IDLE && !tok_count) { history_push("about:start"); show_start_page(); address_focused = 1; address_replace = 1; }
}

void web_paint(const mde_layout *l) {
    /* toolbar */
    ui_symbol_glyph(back_rect(l).x0 + 14, back_rect(l).y0 + 14, SYM_CHEVRON_LEFT, OPAQUE(hist_pos > 0 ? T.text2 : T.text3));
    ui_symbol_glyph(forward_rect(l).x0 + 14, forward_rect(l).y0 + 14, SYM_CHEVRON_RIGHT, OPAQUE(hist_pos + 1u < hist_count ? T.text2 : T.text3));
    gfx_rect ar = address_rect(l);
    if (address_focused) ui_text_field(ar, address, "Search or enter website address", 1);
    else {
        gfx_round_rect(S, ar.x0, ar.y0, W(ar), H(ar), 7, OPAQUE(T.dark ? 0x3A3A3Cu : 0xE9E9EBu));
        /* Like Safari: show the host (with a padlock for verified HTTPS), not the full URL. */
        char shown_buf[WEB_URL_MAX];
        const char *shown = address[0] ? address : "Search or enter website address";
        if (address[0] && !page_is_internal && fetch.state == WS_IDLE) {
            const char *h = address;
            if (prefix_ci(h, "https://")) h += 8; else if (prefix_ci(h, "http://")) h += 7;
            size_t n = 0;
            while (h[n] && h[n] != '/' && n + 1u < sizeof(shown_buf)) { shown_buf[n] = h[n]; ++n; }
            shown_buf[n] = '\0';
            shown = shown_buf;
        }
        int lock = page_secure && !page_is_internal && fetch.state == WS_IDLE;
        int32_t tw = gfx_text_width(FONT_BODY, shown) + (lock ? 14 : 0);
        if (tw > W(ar) - 40) tw = W(ar) - 40;
        int32_t tx = ar.x0 + W(ar) / 2 - tw / 2;
        if (lock) {
            uint32_t lc = OPAQUE(T.text2);
            gfx_round_rect_stroke(S, tx + 1, ar.y0 + 7, 7, 8, 3, lc);       /* shackle */
            gfx_round_rect(S, tx, ar.y0 + 11, 9, 7, 2, lc);                  /* body */
            tx += 14;
        }
        gfx_text_fit(S, FONT_BODY, tx, ar.y0 + 19, shown, W(ar) - 40, OPAQUE(address[0] ? T.text : T.text3));
    }
    if (fetch.state != WS_IDLE) {
        /* thin progress bar under the address field */
        uint32_t phase = (uint32_t)(web_ms() / 30u) % 100u;
        int32_t bw = W(ar) / 4, bx = ar.x0 + (int32_t)((uint32_t)(W(ar) - bw) * phase / 100u);
        gfx_fill(S, bx, ar.y1 - 2, bw, 2, OPAQUE(T.accent));
    }
    /* page */
    gfx_rect c = l->content;
    uint32_t bg = T.dark ? 0x1E1E1Eu : 0xFFFFFFu, fg = T.dark ? 0xE8E8EAu : 0x1D1D1Fu;
    uint32_t link_col = T.dark ? 0x5AB0FFu : 0x0A62D0u, rule = T.dark ? 0x3A3A3Cu : 0xD8D8DCu;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(bg));
    if (layout_width != W(c)) {
        layout(W(c));
        if (scroll_to_main && main_token > 0) {
            for (uint32_t i = 0; i < run_count; ++i) {
                if (runs[i].tok >= (uint32_t)main_token) {
                    int32_t y = runs[i].y - gfx_font_ascent(runs[i].font) - 16;
                    if (y > H(c) / 3) scroll_y = y;
                    break;
                }
            }
            scroll_to_main = 0;
        }
    }
    int32_t max_scroll = doc_height - H(c);
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_y > max_scroll) scroll_y = max_scroll;
    if (scroll_y < 0) scroll_y = 0;
    gfx_rect old = gfx_clip_get();
    gfx_clip_set(gfx_rect_intersect(old, c));
    char text[260];
    for (uint32_t i = 0; i < run_count; ++i) {
        const web_run *r = &runs[i];
        int32_t y = c.y0 + r->y - scroll_y;
        if (y < c.y0 - 40 || y > c.y1 + 40) continue;
        if (r->kind == 1) { gfx_hline(S, c.x0 + r->x, y, r->w, OPAQUE(rule)); continue; }
        const web_tok *t = &toks[r->tok];
        tok_text(t, text, sizeof(text));
        int is_link = t->link >= 0 && r->kind == 0;
        uint32_t col = is_link ? link_col : r->kind == 2 ? (T.dark ? 0xA0A0A6u : 0x6E6E73u) : fg;
        if (t->style & ST_PRE) gfx_text_cells(S, FONT_SMALL, c.x0 + r->x, y, text, WEB_PRE_CELL, OPAQUE(col));
        else gfx_text(S, r->font, c.x0 + r->x, y, text, OPAQUE(col));
        if (is_link && t->link == hover_link) gfx_hline(S, c.x0 + r->x, y + 2, r->w, OPAQUE(link_col));
    }
    /* scroll indicator */
    if (doc_height > H(c)) {
        int32_t track = H(c) - 8, thumb = track * H(c) / doc_height;
        if (thumb < 24) thumb = 24;
        int32_t ty = c.y0 + 4 + (track - thumb) * scroll_y / (max_scroll ? max_scroll : 1);
        gfx_round_rect(S, c.x1 - 9, ty, 5, thumb, 2, T.dark ? ARGB(0x70, 0xFF, 0xFF, 0xFF) : ARGB(0x55, 0, 0, 0));
    }
    /* status pill: hovered link or load state */
    const char *pill = 0;
    char hover_text[WEB_URL_MAX];
    if (hover_link >= 0 && (uint32_t)hover_link < link_count) { url_resolve(links[hover_link], hover_text, sizeof(hover_text)); pill = hover_text; }
    else if (status_text[0] && (fetch.state != WS_IDLE || !page_is_internal)) pill = status_text;
    if (pill) {
        int32_t pw = gfx_text_width(FONT_CAPTION, pill) + 16;
        if (pw > W(c) - 20) pw = W(c) - 20;
        gfx_round_rect(S, c.x0 + 6, c.y1 - 26, pw, 20, 6, OPAQUE(T.dark ? 0x2C2C2Eu : 0xF2F2F4u));
        gfx_round_rect_stroke(S, c.x0 + 6, c.y1 - 26, pw, 20, 6, T.dark ? ARGB(0x40, 0xFF, 0xFF, 0xFF) : ARGB(0x30, 0, 0, 0));
        gfx_text_fit(S, FONT_CAPTION, c.x0 + 14, c.y1 - 12, pill, pw - 16, OPAQUE(T.text2));
    }
    gfx_clip_set(old);
}

static int32_t link_at(const mde_layout *l, int32_t x, int32_t y) {
    gfx_rect c = l->content;
    if (!gfx_rect_contains(c, x, y)) return -1;
    int32_t px = x - c.x0, py = y - c.y0 + scroll_y;
    for (uint32_t i = 0; i < run_count; ++i) {
        const web_run *r = &runs[i];
        if (r->kind != 0) continue;
        const web_tok *t = &toks[r->tok];
        if (t->link < 0) continue;
        int32_t asc = gfx_font_ascent(r->font);
        if (px >= r->x && px < r->x + r->w + 4 && py >= r->y - asc - 2 && py < r->y + 6) return t->link;
    }
    return -1;
}

static void go_history(int delta) {
    if (delta < 0 && hist_pos == 0) return;
    if (delta > 0 && hist_pos + 1u >= hist_count) return;
    hist_pos = (uint32_t)((int32_t)hist_pos + delta);
    navigate(history[hist_pos], 0, 0);
}

int web_click(const mde_layout *l, int32_t x, int32_t y) {
    if (gfx_rect_contains(back_rect(l), x, y)) { go_history(-1); return 1; }
    if (gfx_rect_contains(forward_rect(l), x, y)) { go_history(1); return 1; }
    if (gfx_rect_contains(address_rect(l), x, y)) {
        if (!address_focused) { address_focused = 1; address_replace = 1; address_len = text_length(address); }
        return 1;
    }
    int was_focused = address_focused;
    address_focused = 0;
    int32_t link = link_at(l, x, y);
    if (link >= 0) {
        char target[WEB_URL_MAX];
        url_resolve(links[link], target, sizeof(target));
        navigate(target, 1, 0);
        return 1;
    }
    return was_focused;
}

int web_scroll(const mde_layout *l, int32_t delta) {
    (void)l;
    scroll_y += delta * 48;
    if (scroll_y < 0) scroll_y = 0;
    return 1;
}

int web_key(const mde_layout *l, const MINT_DESKTOP_KEY_EVENT *k) {
    uint32_t ch = k->codepoint;
    int cmd = (k->modifiers & (MINT_KEYMOD_GUI | MINT_KEYMOD_CTRL)) != 0;
    if (cmd) {
        if (ch == 'l' || ch == 'L') { address_focused = 1; address_replace = 1; address_len = text_length(address); return 1; }
        if (ch == 'r' || ch == 'R') { if (!page_is_internal && address[0]) navigate(address, 0, 0); return 1; }
        if (ch == '[') { go_history(-1); return 1; }
        if (ch == ']') { go_history(1); return 1; }
        return 0;
    }
    if (address_focused) {
        if (ch == '\n') { char text[WEB_URL_MAX]; copy_text(text, sizeof(text), address); navigate(text, 1, 0); return 1; }
        if (ch == 27u) { address_focused = 0; return 1; }
        if (ch == 8u) {
            if (address_replace) { address_len = 0; address[0] = '\0'; address_replace = 0; }
            else if (address_len) address[--address_len] = '\0';
            return 1;
        }
        if (ch >= 32u && ch < 127u) {
            if (address_replace) { address_len = 0; address[0] = '\0'; address_replace = 0; }
            if (address_len + 1u < sizeof(address)) { address[address_len++] = (char)ch; address[address_len] = '\0'; }
            return 1;
        }
        return 0;
    }
    int32_t page = H(l->content) - 60;
    switch (k->scancode) {
    case 0x148u: scroll_y -= 40; return 1;
    case 0x150u: scroll_y += 40; return 1;
    case 0x149u: scroll_y -= page; return 1;
    case 0x151u: scroll_y += page; return 1;
    case 0x147u: scroll_y = 0; return 1;
    case 0x14Fu: scroll_y = doc_height; return 1;
    default: break;
    }
    if (ch == ' ') { scroll_y += (k->modifiers & MINT_KEYMOD_SHIFT) ? -page : page; return 1; }
    if (ch == 8u) { go_history(-1); return 1; }
    if (ch == 27u && fetch.state != WS_IDLE) { fetch_stop(); set_status("Stopped", 0); return 1; }
    return 0;
}

uint32_t web_cursor(const mde_layout *l, int32_t x, int32_t y) {
    int32_t link = link_at(l, x, y);
    if (link != hover_link) { hover_link = link; hover_dirty = 1; }
    if (gfx_rect_contains(address_rect(l), x, y)) return MINT_CURSOR_SHAPE_IBEAM;
    return MINT_CURSOR_SHAPE_ARROW;
}
