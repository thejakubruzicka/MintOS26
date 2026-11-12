/* HTTP/1.1, URLs, cookies and form encoding for the MintOS Web engine — see whttp.h. */
#include "whttp.h"
#include "wmem.h"
#include "inflate.h"

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
static const char HEX[] = "0123456789ABCDEF";
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int wstr_ieq(const char *a, const char *b) {
    size_t i = 0;
    for (; a[i] && b[i]; ++i) if (lc(a[i]) != lc(b[i])) return 0;
    return a[i] == b[i];
}
int wstr_iprefix(const char *s, const char *p) {
    for (size_t i = 0; p[i]; ++i) if (!s[i] || lc(s[i]) != lc(p[i])) return 0;
    return 1;
}
size_t wstr_copy(char *out, size_t cap, const char *in) {
    if (!cap) return 0;
    size_t n = 0;
    while (in && in[n] && n + 1u < cap) { out[n] = in[n]; ++n; }
    out[n] = '\0';
    return n;
}
size_t wstr_append(char *out, size_t cap, const char *in) {
    size_t n = wstrlen(out);
    if (n >= cap) return n;
    return n + wstr_copy(out + n, cap - n, in);
}
void wstr_u64(char *out, size_t cap, uint64_t v) {
    char t[24]; size_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < sizeof(t));
    size_t o = 0;
    while (n && o + 1u < cap) out[o++] = t[--n];
    if (cap) out[o] = '\0';
}

/* ------------------------------------------------------------------ URLs */
/* Appends one path/query byte, %-encoding what may not appear on the wire. */
static size_t path_put(char *out, size_t cap, size_t o, uint8_t c) {
    if (c == '\t' || c == '\n' || c == '\r') return o;
    if (c <= 0x20u || c >= 0x7Fu || c == '"' || c == '<' || c == '>' || c == '`' || c == '{' || c == '}' || c == '|' || c == '^') {
        if (o + 4u > cap) return o;
        out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15u];
        return o;
    }
    if (o + 2u > cap) return o;
    out[o++] = (char)(c == '\\' ? '/' : c);
    return o;
}

/* Removes "." and ".." segments from an absolute path (query untouched). */
static void remove_dots(char *path) {
    static char out[WURL_MAX];
    size_t o = 0, i = 0, q = 0;
    while (path[q] && path[q] != '?') ++q;
    while (i < q) {
        if (path[i] == '/') {
            size_t j = i + 1;
            if (j < q + 1 && path[j] == '.' && (j + 1 == q || path[j + 1] == '/')) { i = j + 1; if (i >= q && o + 1 < sizeof(out)) out[o++] = '/'; continue; }
            if (j + 1 < q + 1 && path[j] == '.' && path[j + 1] == '.' && (j + 2 == q || path[j + 2] == '/')) {
                while (o && out[o - 1] != '/') --o;
                if (o) --o;
                i = j + 2;
                if (i >= q && o + 1 < sizeof(out)) out[o++] = '/';
                continue;
            }
        }
        if (o + 1u < sizeof(out)) out[o++] = path[i];
        ++i;
    }
    if (!o || out[0] != '/') {                     /* keep it absolute */
        for (size_t k = o; k > 0; --k) if (k < sizeof(out) - 1u) out[k] = out[k - 1];
        out[0] = '/';
        if (o + 1u < sizeof(out)) ++o;
    }
    while (path[i] && o + 1u < sizeof(out)) out[o++] = path[i++];
    out[o] = '\0';
    wstr_copy(path, WURL_MAX, out);
}

int wurl_parse(const char *in, wurl *u) {
    const char *p = in;
    while (is_space(*p)) ++p;
    u->secure = 0;
    if (wstr_iprefix(p, "https://")) { p += 8; u->secure = 1; }
    else if (wstr_iprefix(p, "http://")) p += 7;
    else return 0;
    while (*p == '/' || *p == '\\') ++p;
    const char *auth_end = p;
    while (*auth_end && *auth_end != '/' && *auth_end != '\\' && *auth_end != '?' && *auth_end != '#') ++auth_end;
    for (const char *a = p; a < auth_end; ++a) if (*a == '@') p = a + 1;     /* drop user:password@ */
    size_t n = 0;
    while (p < auth_end && *p != ':' && n + 1u < sizeof(u->host)) {
        char c = lc(*p++);
        if (!(is_alpha(c) || is_digit(c) || c == '.' || c == '-' || c == '_')) return 0;
        u->host[n++] = c;
    }
    while (n && u->host[n - 1] == '.') --n;        /* trailing dot */
    u->host[n] = '\0';
    if (!n || (p < auth_end && *p != ':')) return 0;
    uint32_t port = u->secure ? 443u : 80u;
    if (p < auth_end && *p == ':') {
        ++p;
        if (p < auth_end) {
            port = 0;
            while (p < auth_end) {
                if (!is_digit(*p)) return 0;
                port = port * 10u + (uint32_t)(*p++ - '0');
                if (port > 65535u) return 0;
            }
            if (!port) return 0;
        }
    }
    u->port = (uint16_t)port;
    size_t m = 0;
    if (*p != '/' && *p != '\\') u->path[m++] = '/';
    while (*p && *p != '#') m = path_put(u->path, sizeof(u->path), m, (uint8_t)*p++);
    u->path[m] = '\0';
    remove_dots(u->path);
    return 1;
}

void wurl_format(const wurl *u, char *out, size_t cap) {
    wstr_copy(out, cap, u->secure ? "https://" : "http://");
    wstr_append(out, cap, u->host);
    if (u->port != (u->secure ? 443u : 80u)) {
        char n[8]; wstr_u64(n, sizeof(n), u->port);
        wstr_append(out, cap, ":"); wstr_append(out, cap, n);
    }
    wstr_append(out, cap, u->path);
}

int wurl_same_origin(const wurl *a, const wurl *b) {
    return a->secure == b->secure && a->port == b->port && wstr_ieq(a->host, b->host);
}

int wurl_resolve(const wurl *base, const char *href, char *out, size_t cap) {
    static char clean[WURL_MAX];
    size_t n = 0;
    while (is_space(*href)) ++href;
    for (; *href && n + 1u < sizeof(clean); ++href) if (*href != '\t' && *href != '\n' && *href != '\r') clean[n++] = *href;
    while (n && is_space(clean[n - 1])) --n;
    clean[n] = '\0';
    /* scheme? */
    size_t s = 0;
    if (is_alpha(clean[0])) {
        s = 1;
        while (is_alpha(clean[s]) || is_digit(clean[s]) || clean[s] == '+' || clean[s] == '-' || clean[s] == '.') ++s;
        if (clean[s] != ':') s = 0;
    }
    if (s) {
        if (wstr_iprefix(clean, "http:") || wstr_iprefix(clean, "https:")) {
            int sec = wstr_iprefix(clean, "https:");
            const char *rest = clean + (sec ? 6 : 5);
            if (!(rest[0] == '/' || rest[0] == '\\') && base && base->secure == sec) {
                /* "http:foo" relative with a scheme: resolve against base */
                return wurl_resolve(base, rest, out, cap);
            }
            wurl u;
            static char abs[WURL_MAX + 16];
            wstr_copy(abs, sizeof(abs), sec ? "https://" : "http://");
            while (*rest == '/' || *rest == '\\') ++rest;
            wstr_append(abs, sizeof(abs), rest);
            if (!wurl_parse(abs, &u)) { wstr_copy(out, cap, clean); return 0; }
            wurl_format(&u, out, cap);
            return 1;
        }
        wstr_copy(out, cap, clean);
        return 0;
    }
    if (!base) { wstr_copy(out, cap, clean); return 0; }
    wurl u = *base;
    if ((clean[0] == '/' || clean[0] == '\\') && (clean[1] == '/' || clean[1] == '\\')) {
        static char abs[WURL_MAX + 16];
        wstr_copy(abs, sizeof(abs), base->secure ? "https:" : "http:");
        wstr_append(abs, sizeof(abs), clean);
        if (!wurl_parse(abs, &u)) { wstr_copy(out, cap, clean); return 0; }
        wurl_format(&u, out, cap);
        return 1;
    }
    size_t m = 0;
    if (clean[0] == '/' || clean[0] == '\\') {
        for (size_t i = 0; clean[i] && clean[i] != '#'; ++i) m = path_put(u.path, sizeof(u.path), m, (uint8_t)clean[i]);
        u.path[m] = '\0';
    } else if (clean[0] == '?') {
        while (u.path[m] && u.path[m] != '?') ++m;
        for (size_t i = 0; clean[i] && clean[i] != '#'; ++i) m = path_put(u.path, sizeof(u.path), m, (uint8_t)clean[i]);
        u.path[m] = '\0';
    } else if (clean[0] == '#' || !clean[0]) {
        /* same document */
    } else {
        size_t last = 0;
        while (u.path[m] && u.path[m] != '?') { if (u.path[m] == '/') last = m; ++m; }
        m = last + 1u;
        for (size_t i = 0; clean[i] && clean[i] != '#'; ++i) m = path_put(u.path, sizeof(u.path), m, (uint8_t)clean[i]);
        u.path[m] = '\0';
    }
    remove_dots(u.path);
    wurl_format(&u, out, cap);
    return 1;
}

void wurl_file_name(const wurl *u, char *out, size_t cap) {
    size_t q = 0, last = 0;
    while (u->path[q] && u->path[q] != '?') { if (u->path[q] == '/') last = q + 1u; ++q; }
    size_t o = 0;
    for (size_t i = last; i < q && o + 1u < cap && o < 120u; ++i) {
        char c = u->path[i];
        if (c == '%' && i + 2 < q && hexval(u->path[i + 1]) >= 0 && hexval(u->path[i + 2]) >= 0) {
            c = (char)(hexval(u->path[i + 1]) * 16 + hexval(u->path[i + 2])); i += 2;
        }
        if ((uint8_t)c < 0x20u || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        if (!o && c == '.') c = '_';
        out[o++] = c;
    }
    out[o] = '\0';
    if (!o) wstr_copy(out, cap, "index.html");
}

/* ------------------------------------------------------------------ requests */
size_t whttp_build_request(char *out, size_t cap, const whttp_request_info *info) {
    const wurl *u = info->url;
    out[0] = '\0';
    wstr_append(out, cap, info->method == WHTTP_POST ? "POST " : "GET ");
    wstr_append(out, cap, u->path);
    wstr_append(out, cap, " HTTP/1.1\r\nHost: ");
    wstr_append(out, cap, u->host);
    if (u->port != (u->secure ? 443u : 80u)) { char p[8]; wstr_u64(p, sizeof(p), u->port); wstr_append(out, cap, ":"); wstr_append(out, cap, p); }
    wstr_append(out, cap, "\r\nUser-Agent: " WHTTP_USER_AGENT "\r\nAccept: ");
    wstr_append(out, cap, info->accept ? info->accept : "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    wstr_append(out, cap, "\r\nAccept-Language: en-US,en;q=0.9\r\nAccept-Encoding: gzip, deflate\r\n");
    if (info->referer && info->referer[0]) {
        wurl r;
        if (wurl_parse(info->referer, &r) && !(r.secure && !u->secure)) {
            char ref[WURL_MAX + 16];
            if (wurl_same_origin(&r, u)) wurl_format(&r, ref, sizeof(ref));
            else { wstr_copy(r.path, sizeof(r.path), "/"); wurl_format(&r, ref, sizeof(ref)); }
            wstr_append(out, cap, "Referer: "); wstr_append(out, cap, ref); wstr_append(out, cap, "\r\n");
        }
    }
    if (info->cookie && info->cookie[0]) { wstr_append(out, cap, "Cookie: "); wstr_append(out, cap, info->cookie); wstr_append(out, cap, "\r\n"); }
    if (info->method == WHTTP_POST) {
        char n[24]; wstr_u64(n, sizeof(n), info->body_len);
        wstr_append(out, cap, "Content-Type: ");
        wstr_append(out, cap, info->content_type ? info->content_type : "application/x-www-form-urlencoded");
        wstr_append(out, cap, "\r\nContent-Length: "); wstr_append(out, cap, n); wstr_append(out, cap, "\r\n");
        if (info->referer && info->referer[0]) {
            wurl r;
            if (wurl_parse(info->referer, &r)) {
                char origin[WHOST_MAX + 32];
                wstr_copy(r.path, sizeof(r.path), "");
                wurl_format(&r, origin, sizeof(origin));
                wstr_append(out, cap, "Origin: "); wstr_append(out, cap, origin); wstr_append(out, cap, "\r\n");
            }
        }
    }
    wstr_append(out, cap, info->keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
    size_t len = wstrlen(out);
    if (len + 1u >= cap || len < 4 || out[len - 1] != '\n' || out[len - 3] != '\n') return 0;   /* did not fit */
    return len;
}

/* ------------------------------------------------------------------ responses */
#define HEAD_MAX (64u * 1024u)
enum { CS_SIZE = 0, CS_EXT, CS_DATA, CS_DATA_CR, CS_TRAILER };

void whttp_resp_init(whttp_resp *r, size_t body_max) {
    wb_set(r, 0, sizeof(*r));
    r->content_length = -1;
    r->body_max = body_max;
    r->state = WH_STATUS;
}
void whttp_resp_free(whttp_resp *r) {
    wmem_free(r->head); wmem_free(r->body);
    r->head = 0; r->body = 0; r->head_len = r->head_cap = r->body_len = r->body_cap = 0;
}

static int put_body(whttp_resp *r, const uint8_t *p, size_t n) {
    r->body_total += n;
    if (r->sink) {
        if (!r->sink(r->sink_ctx, p, n)) { r->state = WH_ERROR; r->error = 2; return 0; }
        return 1;
    }
    if (r->body_len + n > r->body_max) { r->truncated = 1; n = r->body_max > r->body_len ? r->body_max - r->body_len : 0; }
    if (!n) return 1;
    if (r->body_len + n > r->body_cap) {
        size_t cap = r->body_cap ? r->body_cap : 16384u;
        while (cap < r->body_len + n) cap *= 2u;
        if (cap > r->body_max) cap = r->body_max;
        if (r->content_length > 0 && (uint64_t)r->content_length <= r->body_max && cap < (size_t)r->content_length && r->body_len + n <= (size_t)r->content_length)
            cap = (size_t)r->content_length;              /* one allocation when the size is known */
        uint8_t *b = (uint8_t *)wmem_realloc(r->body, cap);
        if (!b) { r->truncated = 1; return 1; }           /* keep what we have, discard the rest */
        r->body = b; r->body_cap = cap;
    }
    wb_copy(r->body + r->body_len, p, n);
    r->body_len += n;
    return 1;
}

static int line_has_token(const char *v, const char *tok) {
    size_t tl = wstrlen(tok);
    for (size_t i = 0; v[i]; ++i) {
        if ((i == 0 || v[i - 1] == ',' || v[i - 1] == ' ') && wstr_iprefix(v + i, tok) &&
            (!v[i + tl] || v[i + tl] == ',' || v[i + tl] == ' ' || v[i + tl] == ';')) return 1;
    }
    return 0;
}

const char *whttp_header(const whttp_resp *r, const char *name, uint32_t idx, char *out, size_t cap) {
    if (cap) out[0] = '\0';
    if (!r->head) return 0;
    size_t len = wstrlen(name);
    const char *h = r->head;
    size_t n = r->head_len, i = 0;
    while (i < n && h[i] != '\n') ++i;              /* skip the status line */
    while (i < n) {
        ++i;
        if (i + len < n && wstr_iprefix(h + i, name) && h[i + len] == ':') {
            if (idx--) { while (i < n && h[i] != '\n') ++i; continue; }
            size_t j = i + len + 1, o = 0;
            while (j < n && (h[j] == ' ' || h[j] == '\t')) ++j;
            while (j < n && h[j] != '\r' && h[j] != '\n') { if (o + 1u < cap) out[o++] = h[j]; ++j; }
            while (o && (out[o - 1] == ' ' || out[o - 1] == '\t')) --o;
            if (cap) out[o] = '\0';
            return out;
        }
        while (i < n && h[i] != '\n') ++i;
    }
    return 0;
}

static void headers_parsed(whttp_resp *r) {
    char v[256];
    if (whttp_header(r, "content-length", 0, v, sizeof(v))) {
        int64_t cl = 0; size_t i = 0;
        while (v[i] == ' ') ++i;
        if (is_digit(v[i])) {
            while (is_digit(v[i]) && cl < ((int64_t)1 << 50)) cl = cl * 10 + (v[i++] - '0');
            r->content_length = cl;
        }
    }
    if (whttp_header(r, "transfer-encoding", 0, v, sizeof(v)) && line_has_token(v, "chunked")) r->chunked = 1;
    r->conn_close = r->http_minor == 0;
    if (whttp_header(r, "connection", 0, v, sizeof(v))) {
        if (line_has_token(v, "close")) r->conn_close = 1;
        else if (line_has_token(v, "keep-alive")) r->conn_close = 0;
    }
    r->encoding = WENC_IDENTITY;
    if (whttp_header(r, "content-encoding", 0, v, sizeof(v))) {
        if (line_has_token(v, "gzip") || line_has_token(v, "x-gzip")) r->encoding = WENC_GZIP;
        else if (line_has_token(v, "deflate")) r->encoding = WENC_DEFLATE;
        else if (v[0] && !wstr_ieq(v, "identity") && !wstr_ieq(v, "none")) r->encoding = WENC_UNSUPPORTED;
    }
    if (r->head_request || r->status == 204 || r->status == 304 || (r->status >= 100 && r->status < 200)) r->state = WH_DONE;
    else if (r->chunked) { r->state = WH_BODY_CHUNKED; r->cstate = CS_SIZE; r->chunk_left = 0; r->line_len = 0; }
    else if (r->content_length >= 0) r->state = r->content_length ? WH_BODY_LENGTH : WH_DONE;
    else { r->state = WH_BODY_CLOSE; r->conn_close = 1; }
}

/* Parses the status line once the header block is complete. */
static int parse_status(whttp_resp *r) {
    const char *h = r->head;
    if (r->head_len < 12 || !wstr_iprefix(h, "HTTP/1.")) return 0;
    r->http_minor = h[7] == '0' ? 0 : 1;
    size_t i = 8;
    while (i < r->head_len && h[i] == ' ') ++i;
    int code = 0, digits = 0;
    while (i < r->head_len && is_digit(h[i]) && digits < 3) { code = code * 10 + (h[i++] - '0'); ++digits; }
    if (digits != 3) return 0;
    r->status = code;
    return 1;
}

size_t whttp_feed(whttp_resp *r, const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len && r->state != WH_DONE && r->state != WH_ERROR) {
        if (r->state == WH_STATUS || r->state == WH_HEADERS) {
            if (r->head_len + 1u >= r->head_cap) {
                if (r->head_cap >= HEAD_MAX) { r->state = WH_ERROR; r->error = 1; break; }
                size_t cap = r->head_cap ? r->head_cap * 2u : 4096u;
                char *h = (char *)wmem_realloc(r->head, cap);
                if (!h) { r->state = WH_ERROR; r->error = 3; break; }
                r->head = h; r->head_cap = cap;
            }
            char c = (char)data[i++];
            if (r->state == WH_STATUS && r->head_len == 0 && (c == '\r' || c == '\n')) continue;   /* stray CRLF */
            r->head[r->head_len++] = c;
            r->head[r->head_len] = '\0';
            r->state = WH_HEADERS;
            size_t n = r->head_len;
            if (c == '\n' && ((n >= 2 && r->head[n - 2] == '\n') || (n >= 4 && r->head[n - 2] == '\r' && r->head[n - 3] == '\n'))) {
                if (!parse_status(r)) { r->state = WH_ERROR; r->error = 4; break; }
                if (r->status >= 100 && r->status < 200 && r->status != 101) {   /* interim response: start over */
                    r->head_len = 0; r->head[0] = '\0'; r->state = WH_STATUS; continue;
                }
                headers_parsed(r);
                r->headers_ready = 1;
                if (r->pause_after_headers) { r->pause_after_headers = 0; return i; }
            }
            continue;
        }
        if (r->state == WH_BODY_LENGTH) {
            uint64_t left = (uint64_t)r->content_length - r->body_total;
            size_t n = len - i < left ? len - i : (size_t)left;
            if (!put_body(r, data + i, n)) break;
            i += n;
            if (r->body_total >= (uint64_t)r->content_length) r->state = WH_DONE;
            continue;
        }
        if (r->state == WH_BODY_CLOSE) {
            if (!put_body(r, data + i, len - i)) break;
            i = len;
            continue;
        }
        /* chunked */
        char c = (char)data[i];
        switch (r->cstate) {
        case CS_SIZE:
            ++i;
            if (hexval(c) >= 0) {
                if (r->chunk_left > ((uint64_t)1 << 40)) { r->state = WH_ERROR; r->error = 5; break; }
                r->chunk_left = r->chunk_left * 16u + (uint64_t)hexval(c);
                ++r->line_len;
            } else if (c == '\n') {
                if (!r->line_len) { r->state = WH_ERROR; r->error = 5; break; }
                r->line_len = 0;
                r->cstate = r->chunk_left ? CS_DATA : CS_TRAILER;
            } else if (c == ';' || c == ' ' || c == '\t' || c == '\r') r->cstate = CS_EXT;
            else { r->state = WH_ERROR; r->error = 5; }
            break;
        case CS_EXT:
            ++i;
            if (c == '\n') {
                if (!r->line_len) { r->state = WH_ERROR; r->error = 5; break; }
                r->line_len = 0;
                r->cstate = r->chunk_left ? CS_DATA : CS_TRAILER;
            }
            break;
        case CS_DATA: {
            size_t n = len - i < r->chunk_left ? len - i : (size_t)r->chunk_left;
            if (!put_body(r, data + i, n)) break;
            i += n; r->chunk_left -= n;
            if (!r->chunk_left) r->cstate = CS_DATA_CR;
            break;
        }
        case CS_DATA_CR:
            ++i;
            if (c == '\n') { r->cstate = CS_SIZE; r->line_len = 0; r->chunk_left = 0; }
            else if (c != '\r') { r->state = WH_ERROR; r->error = 5; }
            break;
        case CS_TRAILER:
            ++i;
            if (c == '\n') {
                if (!r->line_len) r->state = WH_DONE;         /* empty line ends the trailer */
                r->line_len = 0;
            } else if (c != '\r') ++r->line_len;
            break;
        default: r->state = WH_ERROR; break;
        }
    }
    return i;
}

void whttp_eof(whttp_resp *r) {
    if (r->state == WH_BODY_CLOSE) { r->state = WH_DONE; return; }
    if (r->state == WH_DONE || r->state == WH_ERROR) return;
    if ((r->state == WH_BODY_LENGTH || r->state == WH_BODY_CHUNKED) && r->body_total) {
        r->truncated = 1; r->state = WH_DONE;                 /* keep the partial page */
        return;
    }
    r->state = WH_ERROR;
    if (!r->error) r->error = 6;
}

int64_t whttp_decode_body(whttp_resp *r, size_t max_out) {
    if (r->encoding == WENC_IDENTITY || !r->body_len) return 0;
    if (r->encoding == WENC_UNSUPPORTED) return WINF_ERR_HEADER;
    uint8_t *out = 0;
    int64_t n = winf_decode(r->body, r->body_len, r->encoding == WENC_GZIP ? WINF_GZIP : WINF_AUTO_DEFLATE, max_out, &out);
    if (n < 0 && r->truncated) return n;
    if (n < 0) return n;
    wmem_free(r->body);
    r->body = out; r->body_len = r->body_cap = (size_t)n;
    r->encoding = WENC_IDENTITY;
    return 0;
}

void whttp_content_type(const whttp_resp *r, char *type, size_t tcap, char *charset, size_t ccap) {
    char v[200];
    if (tcap) type[0] = '\0';
    if (ccap) charset[0] = '\0';
    if (!whttp_header(r, "content-type", 0, v, sizeof(v))) return;
    size_t i = 0, o = 0;
    while (v[i] == ' ') ++i;
    while (v[i] && v[i] != ';' && v[i] != ' ') { if (o + 1u < tcap) type[o++] = lc(v[i]); ++i; }
    if (tcap) type[o] = '\0';
    for (; v[i]; ++i) {
        if (wstr_iprefix(v + i, "charset=")) {
            i += 8;
            if (v[i] == '"' || v[i] == '\'') ++i;
            o = 0;
            while (v[i] && v[i] != ';' && v[i] != '"' && v[i] != '\'' && v[i] != ' ') { if (o + 1u < ccap) charset[o++] = lc(v[i]); ++i; }
            if (ccap) charset[o] = '\0';
            break;
        }
    }
}

static void sanitize_name(char *s) {
    size_t o = 0;
    for (size_t i = 0; s[i]; ++i) {
        char c = s[i];
        if (c == '/' || c == '\\') { o = 0; continue; }          /* keep only the last path component */
        if ((uint8_t)c < 0x20u || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        s[o++] = c;
    }
    s[o] = '\0';
    while (s[0] == '.') for (size_t i = 0; s[i]; ++i) s[i] = s[i + 1];
}

int whttp_disposition(const whttp_resp *r, char *name, size_t cap) {
    char v[400];
    if (cap) name[0] = '\0';
    if (!whttp_header(r, "content-disposition", 0, v, sizeof(v))) return 0;
    int attachment = wstr_iprefix(v, "attachment");
    for (size_t i = 0; v[i]; ++i) {
        if (wstr_iprefix(v + i, "filename*=")) {             /* RFC 5987: charset'lang'pct-encoded */
            size_t j = i + 10, q = 0;
            while (v[j] && v[j] != ';' && q < 2) { if (v[j] == '\'') ++q; ++j; }
            if (q == 2) {
                size_t o = 0;
                while (v[j] && v[j] != ';' && o + 1u < cap) {
                    if (v[j] == '%' && hexval(v[j + 1]) >= 0 && hexval(v[j + 2]) >= 0) { name[o++] = (char)(hexval(v[j + 1]) * 16 + hexval(v[j + 2])); j += 3; }
                    else name[o++] = v[j++];
                }
                name[o] = '\0';
                sanitize_name(name);
                if (name[0]) return attachment;
            }
        }
    }
    for (size_t i = 0; v[i]; ++i) {
        if ((i == 0 || v[i - 1] == ';' || v[i - 1] == ' ') && wstr_iprefix(v + i, "filename=")) {
            size_t j = i + 9, o = 0;
            char quote = 0;
            if (v[j] == '"' || v[j] == '\'') quote = v[j++];
            while (v[j] && (quote ? v[j] != quote : v[j] != ';') && o + 1u < cap) name[o++] = v[j++];
            while (o && name[o - 1] == ' ') --o;
            name[o] = '\0';
            sanitize_name(name);
            break;
        }
    }
    return attachment;
}

const char *whttp_status_text(int s) {
    switch (s) {
    case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden";
    case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 410: return "Gone";
    case 429: return "Too Many Requests"; case 500: return "Internal Server Error"; case 502: return "Bad Gateway";
    case 503: return "Service Unavailable"; case 504: return "Gateway Timeout";
    default: return s >= 500 ? "Server Error" : s >= 400 ? "Client Error" : "";
    }
}

/* ------------------------------------------------------------------ dates */
int64_t whttp_days_from_civil(int64_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (m + (m > 2 ? (uint32_t)-3 : 9u)) + 2u) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int date_delim(char c) {
    uint8_t u = (uint8_t)c;
    return u == 0x09 || (u >= 0x20 && u <= 0x2F) || (u >= 0x3B && u <= 0x40) || (u >= 0x5B && u <= 0x60) || (u >= 0x7B && u <= 0x7E);
}

int64_t whttp_parse_date(const char *s) {
    static const char *const months[12] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    int have_time = 0, have_day = 0, have_month = 0, have_year = 0;
    int64_t hh = 0, mm = 0, ss = 0, day = 0, month = 0, year = 0;
    size_t i = 0;
    while (s[i]) {
        while (s[i] && date_delim(s[i])) ++i;
        size_t start = i;
        while (s[i] && !date_delim(s[i])) ++i;
        size_t len = i - start;
        if (!len) continue;
        const char *t = s + start;
        if (!have_time) {
            int64_t v[3] = {0, 0, 0}; size_t k = 0, part = 0, digits = 0; int ok = 1;
            for (; k < len && part < 3; ++k) {
                if (is_digit(t[k])) { v[part] = v[part] * 10 + (t[k] - '0'); if (++digits > 2) { ok = 0; break; } }
                else if (t[k] == ':' && digits) { ++part; digits = 0; }
                else break;
            }
            if (ok && part == 2 && digits) { hh = v[0]; mm = v[1]; ss = v[2]; have_time = 1; continue; }
        }
        if (!have_day && len <= 2 && is_digit(t[0]) && (len == 1 || is_digit(t[1]))) {
            day = t[0] - '0'; if (len == 2) day = day * 10 + (t[1] - '0');
            have_day = 1; continue;
        }
        if (!have_month && len >= 3) {
            int found = -1;
            for (int m = 0; m < 12; ++m) if (lc(t[0]) == months[m][0] && lc(t[1]) == months[m][1] && lc(t[2]) == months[m][2]) found = m;
            if (found >= 0) { month = found + 1; have_month = 1; continue; }
        }
        if (!have_year && len >= 2 && len <= 4) {
            int all = 1; int64_t v = 0;
            for (size_t k = 0; k < len; ++k) { if (!is_digit(t[k])) { all = 0; break; } v = v * 10 + (t[k] - '0'); }
            if (all) { year = v; have_year = 1; continue; }
        }
    }
    if (!have_time || !have_day || !have_month || !have_year) return -1;
    if (year >= 70 && year <= 99) year += 1900;
    else if (year < 70) year += 2000;
    if (day < 1 || day > 31 || year < 1601 || hh > 23 || mm > 59 || ss > 59) return -1;
    return whttp_days_from_civil(year, (uint32_t)month, (uint32_t)day) * 86400 + hh * 3600 + mm * 60 + ss;
}

/* ------------------------------------------------------------------ cookies */
typedef struct wck {
    char *name, *value, *domain, *path;          /* one wmem block: name\0value\0domain\0path\0 */
    int64_t expires;                             /* 0 = session */
    uint64_t created;
    uint8_t secure, host_only, http_only, used;
} wck;
static wck jar[WCOOKIE_MAX];
static uint32_t jar_count, jar_gen;
static uint64_t create_seq;

uint32_t wcookie_count(void) { return jar_count; }
uint32_t wcookie_generation(void) { return jar_gen; }
static void ck_drop(uint32_t i) {
    wmem_free(jar[i].name);
    jar[i] = jar[--jar_count];
    jar[jar_count].used = 0;
    ++jar_gen;
}
void wcookie_clear(void) { while (jar_count) ck_drop(jar_count - 1u); }

static int domain_match(const char *host, const char *domain) {
    size_t h = wstrlen(host), d = wstrlen(domain);
    if (h == d) return wstr_ieq(host, domain);
    if (h < d + 1u || host[h - d - 1u] != '.') return 0;
    if (!wstr_ieq(host + h - d, domain)) return 0;
    int ip = 1;                                     /* no domain matching for IP literals */
    for (size_t i = 0; host[i]; ++i) if (!is_digit(host[i]) && host[i] != '.') ip = 0;
    return !ip;
}
static int path_match(const char *req, const char *cpath) {
    size_t c = wstrlen(cpath);
    for (size_t i = 0; i < c; ++i) if (req[i] != cpath[i]) return 0;
    char next = req[c];
    return next == '\0' || next == '?' || cpath[c - 1] == '/' || next == '/';
}
static void default_path(const wurl *u, char *out, size_t cap) {
    size_t q = 0, last = 0;
    while (u->path[q] && u->path[q] != '?') { if (u->path[q] == '/') last = q; ++q; }
    if (!last) { wstr_copy(out, cap, "/"); return; }
    size_t n = last < cap - 1u ? last : cap - 1u;
    for (size_t i = 0; i < n; ++i) out[i] = u->path[i];
    out[n] = '\0';
}
static void trim(const char **s, size_t *n) {
    while (*n && is_space(**s)) { ++*s; --*n; }
    while (*n && is_space((*s)[*n - 1])) --*n;
}

static int ck_store(const char *name, size_t nl, const char *value, size_t vl, const char *domain, const char *path,
                    int64_t expires, int secure, int host_only, int http_only, int64_t now) {
    uint64_t created = 0;
    for (uint32_t i = 0; i < jar_count; ++i) {
        wck *c = &jar[i];
        if (wstrlen(c->name) != nl || !wstr_ieq(c->domain, domain)) continue;
        int same = 1;
        for (size_t k = 0; k < nl; ++k) if (c->name[k] != name[k]) same = 0;
        for (size_t k = 0; same && (c->path[k] || path[k]); ++k) if (c->path[k] != path[k]) same = 0;
        if (!same) continue;
        created = c->created;                             /* replacing keeps the creation time */
        ck_drop(i);
        if (expires && expires <= now) return 1;          /* deletion */
        break;
    }
    if (expires && expires <= now) return 0;
    if (jar_count >= WCOOKIE_MAX) {                       /* evict: expired first, then the oldest */
        uint32_t victim = 0;
        for (uint32_t i = 1; i < jar_count; ++i) if (jar[i].created < jar[victim].created) victim = i;
        ck_drop(victim);
    }
    size_t dl = wstrlen(domain), pl = wstrlen(path);
    char *blob = (char *)wmem_alloc(nl + vl + dl + pl + 4u);
    if (!blob) return 0;
    wck *c = &jar[jar_count++];
    c->name = blob; wb_copy(blob, name, nl); blob[nl] = '\0';
    c->value = blob + nl + 1; wb_copy(c->value, value, vl); c->value[vl] = '\0';
    c->domain = c->value + vl + 1; wb_copy(c->domain, domain, dl); c->domain[dl] = '\0';
    for (size_t k = 0; k < dl; ++k) c->domain[k] = lc(c->domain[k]);
    c->path = c->domain + dl + 1; wb_copy(c->path, path, pl); c->path[pl] = '\0';
    c->expires = expires; c->created = created ? created : ++create_seq;
    c->secure = (uint8_t)secure; c->host_only = (uint8_t)host_only; c->http_only = (uint8_t)http_only; c->used = 1;
    ++jar_gen;
    return 1;
}

int wcookie_set(const char *sc, const wurl *u, int64_t now) {
    size_t total = wstrlen(sc);
    size_t semi = 0;
    while (semi < total && sc[semi] != ';') ++semi;
    const char *pair = sc; size_t pl = semi;
    size_t eq = 0;
    while (eq < pl && pair[eq] != '=') ++eq;
    if (eq >= pl) return 0;                              /* no '=': ignored */
    const char *name = pair; size_t nl = eq;
    const char *value = pair + eq + 1; size_t vl = pl - eq - 1;
    trim(&name, &nl); trim(&value, &vl);
    if (!nl || nl + vl > 4096u) return 0;
    for (size_t i = 0; i < nl; ++i) if ((uint8_t)name[i] < 0x21u || name[i] == 0x7F || name[i] == '=' || name[i] == '\t') return 0;
    for (size_t i = 0; i < vl; ++i) if ((uint8_t)value[i] < 0x20u || value[i] == 0x7F || value[i] == '\t') return 0;
    char domain[WHOST_MAX] = "", path[256] = "";
    int64_t expires = 0, max_age_set = 0, max_age = 0;
    int secure = 0, http_only = 0;
    size_t i = semi;
    while (i < total) {
        ++i;                                              /* skip ';' */
        size_t s = i;
        while (i < total && sc[i] != ';') ++i;
        const char *a = sc + s; size_t al = i - s;
        size_t e = 0; while (e < al && a[e] != '=') ++e;
        const char *an = a; size_t anl = e;
        const char *av = e < al ? a + e + 1 : a + al; size_t avl = e < al ? al - e - 1 : 0;
        trim(&an, &anl); trim(&av, &avl);
        char key[16]; size_t k = 0;
        for (; k < anl && k + 1u < sizeof(key); ++k) key[k] = lc(an[k]);
        key[k] = '\0';
        if (k != anl) continue;
        char val[256]; size_t vn = avl < sizeof(val) - 1u ? avl : sizeof(val) - 1u;
        for (size_t q = 0; q < vn; ++q) val[q] = av[q];
        val[vn] = '\0';
        if (wstr_ieq(key, "expires")) { int64_t t = whttp_parse_date(val); if (t >= 0 && !max_age_set) expires = t > 0 ? t : 1; }
        else if (wstr_ieq(key, "max-age")) {
            int neg = 0; size_t q = 0; int64_t v = 0;
            if (val[0] == '-') { neg = 1; q = 1; }
            if (!is_digit(val[q])) continue;
            for (; is_digit(val[q]) && v < ((int64_t)1 << 40); ++q) v = v * 10 + (val[q] - '0');
            max_age_set = 1; max_age = neg ? -v : v;
            expires = max_age <= 0 ? 1 : now + max_age;
        } else if (wstr_ieq(key, "domain")) {
            const char *d = val; while (*d == '.') ++d;
            wstr_copy(domain, sizeof(domain), d);
            for (size_t q = 0; domain[q]; ++q) domain[q] = lc(domain[q]);
        } else if (wstr_ieq(key, "path")) {
            if (val[0] == '/') wstr_copy(path, sizeof(path), val);
        } else if (wstr_ieq(key, "secure")) secure = 1;
        else if (wstr_ieq(key, "httponly")) http_only = 1;
    }
    int host_only = 1;
    if (domain[0]) {
        int dot = 0;
        for (size_t q = 0; domain[q]; ++q) if (domain[q] == '.') dot = 1;
        if (!domain_match(u->host, domain)) return 0;
        if (!dot && !wstr_ieq(domain, u->host)) return 0;  /* "com" and other bare suffixes */
        host_only = !dot;
    } else wstr_copy(domain, sizeof(domain), u->host);
    if (!path[0]) default_path(u, path, sizeof(path));
    if (secure && !u->secure) return 0;                   /* RFC 6265bis: no Secure cookies from http */
    if (nl >= 9 && wstr_iprefix(name, "__Secure-") && !secure) return 0;
    if (nl >= 7 && wstr_iprefix(name, "__Host-") && (!secure || !host_only || path[0] != '/' || path[1])) return 0;
    if (expires == 1 && now > 1) expires = now;           /* already expired → delete */
    return ck_store(name, nl, value, vl, domain, path, expires, secure, host_only, http_only, now);
}

size_t wcookie_header(const wurl *u, int64_t now, char *out, size_t cap) {
    uint32_t idx[WCOOKIE_MAX], n = 0;
    for (uint32_t i = 0; i < jar_count; ) {
        wck *c = &jar[i];
        if (c->expires && now && c->expires <= now) { ck_drop(i); continue; }
        int dm = c->host_only ? wstr_ieq(u->host, c->domain) : domain_match(u->host, c->domain);
        if (dm && path_match(u->path, c->path) && (!c->secure || u->secure)) idx[n++] = i;
        ++i;
    }
    for (uint32_t a = 1; a < n; ++a) {                    /* longer paths first, then older */
        uint32_t v = idx[a], b = a;
        while (b && (wstrlen(jar[idx[b - 1]].path) < wstrlen(jar[v].path) ||
                     (wstrlen(jar[idx[b - 1]].path) == wstrlen(jar[v].path) && jar[idx[b - 1]].created > jar[v].created))) { idx[b] = idx[b - 1]; --b; }
        idx[b] = v;
    }
    if (cap) out[0] = '\0';
    size_t len = 0;
    for (uint32_t k = 0; k < n; ++k) {
        wck *c = &jar[idx[k]];
        size_t need = wstrlen(c->name) + wstrlen(c->value) + 3u;
        if (len + need + 1u > cap) break;
        if (len) { out[len++] = ';'; out[len++] = ' '; }
        len += wstr_copy(out + len, cap - len, c->name);
        out[len++] = '=';
        len += wstr_copy(out + len, cap - len, c->value);
    }
    if (cap) out[len < cap ? len : cap - 1u] = '\0';
    return len;
}

size_t wcookie_save(char *out, size_t cap, int64_t now) {
    size_t len = 0;
    if (cap) out[0] = '\0';
    const char *header = "# MintOS Web cookies: domain host-only path secure expires name value http-only\n";
    if (wstrlen(header) + 1u < cap) len = wstr_copy(out, cap, header);
    for (uint32_t i = 0; i < jar_count; ++i) {
        wck *c = &jar[i];
        if (!c->expires || (now && c->expires <= now)) continue;
        char line[4600], num[24];
        line[0] = '\0';
        wstr_append(line, sizeof(line), c->domain); wstr_append(line, sizeof(line), c->host_only ? "\t1\t" : "\t0\t");
        wstr_append(line, sizeof(line), c->path); wstr_append(line, sizeof(line), c->secure ? "\t1\t" : "\t0\t");
        wstr_u64(num, sizeof(num), (uint64_t)c->expires); wstr_append(line, sizeof(line), num);
        wstr_append(line, sizeof(line), "\t"); wstr_append(line, sizeof(line), c->name);
        wstr_append(line, sizeof(line), "\t"); wstr_append(line, sizeof(line), c->value);
        wstr_append(line, sizeof(line), c->http_only ? "\t1\n" : "\t0\n");
        size_t ll = wstrlen(line);
        if (len + ll + 1u > cap) break;
        len += wstr_copy(out + len, cap - len, line);
    }
    return len;
}

void wcookie_load(const char *text, size_t len, int64_t now) {
    size_t i = 0;
    while (i < len) {
        size_t s = i;
        while (i < len && text[i] != '\n') ++i;
        const char *line = text + s; size_t ll = i - s;
        ++i;
        if (!ll || line[0] == '#') continue;
        const char *f[8]; size_t fl[8]; size_t nf = 0, fs = 0;
        for (size_t k = 0; k <= ll && nf < 8; ++k) {
            if (k == ll || line[k] == '\t') { f[nf] = line + fs; fl[nf] = k - fs; ++nf; fs = k + 1; }
        }
        if (nf != 8 || !fl[0] || !fl[2] || !fl[5] || fl[0] >= WHOST_MAX || fl[2] >= 256) continue;
        char domain[WHOST_MAX], path[256];
        for (size_t k = 0; k < fl[0]; ++k) domain[k] = f[0][k];
        domain[fl[0]] = '\0';
        for (size_t k = 0; k < fl[2]; ++k) path[k] = f[2][k];
        path[fl[2]] = '\0';
        int64_t exp = 0;
        for (size_t k = 0; k < fl[4] && is_digit(f[4][k]); ++k) exp = exp * 10 + (f[4][k] - '0');
        if (exp <= now || fl[5] + fl[6] > 4096u) continue;
        (void)ck_store(f[5], fl[5], f[6], fl[6], domain, path, exp, f[3][0] == '1', f[1][0] == '1', f[7][0] == '1', now);
    }
}

/* ------------------------------------------------------------------ forms */
size_t wform_append(char *out, size_t cap, size_t len, const char *name, const char *value) {
    size_t o = len;
    if (o) { if (o + 2u > cap) return len; out[o++] = '&'; }
    for (int part = 0; part < 2; ++part) {
        const char *s = part ? value : name;
        if (part) { if (o + 2u > cap) return len; out[o++] = '='; }
        for (size_t i = 0; s && s[i]; ++i) {
            uint8_t c = (uint8_t)s[i];
            if (c == '\r' && s[i + 1] == '\n') continue;           /* newlines become CRLF below */
            if (c == '\n' || c == '\r') {
                if (o + 7u > cap) return len;
                out[o++] = '%'; out[o++] = '0'; out[o++] = 'D'; out[o++] = '%'; out[o++] = '0'; out[o++] = 'A';
                continue;
            }
            if (is_alpha((char)c) || is_digit((char)c) || c == '*' || c == '-' || c == '.' || c == '_') {
                if (o + 2u > cap) return len;
                out[o++] = (char)c;
            } else if (c == ' ') {
                if (o + 2u > cap) return len;
                out[o++] = '+';
            } else {
                if (o + 4u > cap) return len;
                out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15u];
            }
        }
    }
    out[o] = '\0';
    return o;
}
