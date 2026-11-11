/* Host harness for the MintOS Web engine core (kernel/user/web).
 *   test-web-core unit                        built-in URL, cookie, form, date, request and heap tests
 *   test-web-core inflate FORMAT IN OUT        decode raw|zlib|gzip|auto into OUT, prints the length or ERR
 *   test-web-core image IN OUT                 decode an image, write "W H\n" + RGBA to OUT
 *   test-web-core scale IN DW DH OUT           decode + resample, write "W H\n" + premultiplied ARGB (BGRA bytes)
 *   test-web-core http IN OUT [SEED]           parse an HTTP response fed in random pieces, decode
 *                                              Content-Encoding, write the body to OUT
 * Prints PASS/FAIL lines; exits non-zero on failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/user/web/wmem.h"
#include "../kernel/user/web/inflate.h"
#include "../kernel/user/web/whttp.h"
#include "../kernel/user/web/wimage.h"

static int failures;
static void check(int ok, const char *name, const char *detail) {
    printf("%s: %s", ok ? "PASS" : "FAIL", name);
    if (!ok && detail) printf("  [%s]", detail);
    printf("\n");
    if (!ok) ++failures;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void resolve_case(const char *base, const char *href, const char *want) {
    wurl b; char out[WURL_MAX + 32], name[400];
    if (!wurl_parse(base, &b)) { check(0, "base parses", base); return; }
    wurl_resolve(&b, href, out, sizeof(out));
    snprintf(name, sizeof(name), "resolve %s + \"%s\" = %s", base, href, want);
    check(strcmp(out, want) == 0, name, out);
}

static void unit(void) {
    /* --- URLs (RFC 3986 section 5.4 examples plus browser behaviour) */
    const char *base = "http://a/b/c/d;p?q";
    resolve_case(base, "g", "http://a/b/c/g");
    resolve_case(base, "./g", "http://a/b/c/g");
    resolve_case(base, "g/", "http://a/b/c/g/");
    resolve_case(base, "/g", "http://a/g");
    resolve_case(base, "//g", "http://g/");
    resolve_case(base, "?y", "http://a/b/c/d;p?y");
    resolve_case(base, "g?y", "http://a/b/c/g?y");
    resolve_case(base, "#s", "http://a/b/c/d;p?q");
    resolve_case(base, "g#s", "http://a/b/c/g");
    resolve_case(base, "", "http://a/b/c/d;p?q");
    resolve_case(base, ".", "http://a/b/c/");
    resolve_case(base, "./", "http://a/b/c/");
    resolve_case(base, "..", "http://a/b/");
    resolve_case(base, "../", "http://a/b/");
    resolve_case(base, "../g", "http://a/b/g");
    resolve_case(base, "../..", "http://a/");
    resolve_case(base, "../../g", "http://a/g");
    resolve_case(base, "../../../g", "http://a/g");
    resolve_case(base, "/./g", "http://a/g");
    resolve_case(base, "/../g", "http://a/g");
    resolve_case(base, "g.", "http://a/b/c/g.");
    resolve_case(base, "..g", "http://a/b/c/..g");
    resolve_case(base, "./g/.", "http://a/b/c/g/");
    resolve_case(base, "g/../h", "http://a/b/c/h");
    resolve_case("https://en.wikipedia.org/wiki/Prague", "//upload.wikimedia.org/x/y.png?a=1", "https://upload.wikimedia.org/x/y.png?a=1");
    resolve_case("https://en.wikipedia.org/wiki/Prague", "  /wiki/Czech Republic\n", "https://en.wikipedia.org/wiki/Czech%20Republic");
    resolve_case("https://example.com/a/b", "HTTPS://Example.COM:443/x", "https://example.com/x");
    resolve_case("https://example.com/a/b", "http://example.com:8080/\xC3\xA9", "http://example.com:8080/%C3%A9");
    resolve_case("https://example.com/a/b", "mailto:x@y.z", "mailto:x@y.z");
    resolve_case("https://example.com/a/b", "javascript:void(0)", "javascript:void(0)");
    resolve_case("https://example.com/a/b?x=1", "?q=a+b", "https://example.com/a/b?q=a+b");
    resolve_case("http://user:pw@host.test:81/p", "q", "http://host.test:81/q");
    wurl u;
    check(!wurl_parse("http://exa mple.com/", &u), "a host with a space is rejected", 0);
    check(!wurl_parse("http://example.com:99999/", &u), "an out-of-range port is rejected", 0);
    check(!wurl_parse("ftp://example.com/", &u), "non-http schemes are not fetched", 0);
    check(wurl_parse("https://Example.COM./a", &u) && !strcmp(u.host, "example.com"), "host names are lower-cased, trailing dot dropped", u.host);
    wurl_parse("https://example.com/files/Annual%20Report.pdf?dl=1", &u);
    char fname[200]; wurl_file_name(&u, fname, sizeof(fname));
    check(!strcmp(fname, "Annual Report.pdf"), "download file names are decoded from the URL", fname);
    wurl_parse("https://example.com/", &u); wurl_file_name(&u, fname, sizeof(fname));
    check(!strcmp(fname, "index.html"), "a URL without a file name downloads as index.html", fname);

    /* --- requests */
    char req[4096];
    wurl_parse("https://example.com:8443/search?q=1", &u);
    whttp_request_info ri = {WHTTP_POST, &u, 0, "a=b; c=d", "https://example.com:8443/form", 0, 9, 1};
    size_t rl = whttp_build_request(req, sizeof(req), &ri);
    check(rl && !strncmp(req, "POST /search?q=1 HTTP/1.1\r\nHost: example.com:8443\r\n", 50) && strstr(req, "Content-Length: 9\r\n") &&
          strstr(req, "Content-Type: application/x-www-form-urlencoded\r\n") && strstr(req, "Cookie: a=b; c=d\r\n") &&
          strstr(req, "Accept-Encoding: gzip, deflate\r\n") && strstr(req, "Referer: https://example.com:8443/form\r\n") &&
          strstr(req, "Origin: https://example.com:8443\r\n") && strstr(req, "User-Agent: Mozilla/5.0") &&
          strstr(req, "Connection: keep-alive\r\n\r\n"), "POST request carries length, type, cookies, referer, origin", req);
    wurl other; wurl_parse("http://plain.test/", &other);
    whttp_request_info r2 = {WHTTP_GET, &other, 0, 0, "https://secure.test/private?token=1", 0, 0, 0};
    rl = whttp_build_request(req, sizeof(req), &r2);
    check(rl && !strstr(req, "Referer") && strstr(req, "Connection: close"), "no Referer is sent from https to http", req);
    whttp_request_info r3 = {WHTTP_GET, &u, 0, 0, "https://elsewhere.test/private?token=1", 0, 0, 1};
    rl = whttp_build_request(req, sizeof(req), &r3);
    check(rl && strstr(req, "Referer: https://elsewhere.test/\r\n"), "cross-origin Referer is trimmed to the origin", req);
    check(whttp_build_request(req, 64, &r3) == 0, "a request that does not fit is refused", 0);

    /* --- forms */
    char form[256]; size_t fl = 0;
    fl = wform_append(form, sizeof(form), fl, "search", "Prague castle & bridge");
    fl = wform_append(form, sizeof(form), fl, "title", "Special:Search");
    fl = wform_append(form, sizeof(form), fl, "q", "\xC5\x99\xC3\xAD" "1+1=2\n~*");
    check(!strcmp(form, "search=Prague+castle+%26+bridge&title=Special%3ASearch&q=%C5%99%C3%AD1%2B1%3D2%0D%0A%7E*"),
          "application/x-www-form-urlencoded encoding (spaces, UTF-8, newlines)", form);
    char tiny[12]; size_t tl = wform_append(tiny, sizeof(tiny), 0, "name", "toolongvalue");
    check(tl == 0, "form encoding never overflows its buffer", 0);

    /* --- dates */
    check(whttp_parse_date("Wed, 21 Oct 2015 07:28:00 GMT") == 1445412480, "RFC 1123 date", 0);
    check(whttp_parse_date("Sunday, 06-Nov-94 08:49:37 GMT") == 784111777, "RFC 850 date with 2-digit year", 0);
    check(whttp_parse_date("Sun Nov  6 08:49:37 1994") == 784111777, "asctime date", 0);
    check(whttp_parse_date("not a date") == -1, "garbage dates are rejected", 0);

    /* --- cookies */
    int64_t now = 1762800000;   /* Nov 2025 */
    wurl a, sub, other_site, plain, admin;
    wurl_parse("https://www.example.com/shop/cart", &a);
    wurl_parse("https://news.example.com/", &sub);
    wurl_parse("https://example.org/", &other_site);
    wurl_parse("http://www.example.com/shop/item", &plain);
    wurl_parse("https://www.example.com/admin/x", &admin);
    wcookie_clear();
    check(wcookie_set("sid=abc123; Path=/; Secure; HttpOnly", &a, now), "a secure session cookie is stored", 0);
    check(wcookie_set("lang=cs; Domain=.example.com; Path=/; Max-Age=3600", &a, now), "a domain cookie is stored", 0);
    check(wcookie_set("cart=3", &a, now), "a cookie with the default path is stored", 0);
    check(!wcookie_set("evil=1; Domain=example.org", &a, now), "a cookie for an unrelated domain is rejected", 0);
    check(!wcookie_set("tld=1; Domain=com", &a, now), "a cookie for a bare suffix is rejected", 0);
    check(!wcookie_set("sec=1; Secure", &plain, now), "a Secure cookie from plain http is rejected", 0);
    check(!wcookie_set("noequals", &a, now), "a cookie without '=' is ignored", 0);
    check(!wcookie_set("__Host-x=1; Secure; Domain=example.com; Path=/", &a, now), "__Host- cookies must be host-only", 0);
    char ch[1024];
    wcookie_header(&a, now, ch, sizeof(ch));
    check(!strcmp(ch, "cart=3; sid=abc123; lang=cs"), "Cookie header: longest path first, then creation order", ch);
    wcookie_header(&sub, now, ch, sizeof(ch));
    check(!strcmp(ch, "lang=cs"), "domain cookies reach subdomains, host-only ones do not", ch);
    wcookie_header(&other_site, now, ch, sizeof(ch));
    check(!strcmp(ch, ""), "no cookies leak to another site", ch);
    wcookie_header(&plain, now, ch, sizeof(ch));
    check(!strcmp(ch, "cart=3; lang=cs"), "Secure cookies are not sent over http", ch);
    wcookie_header(&admin, now, ch, sizeof(ch));
    check(!strcmp(ch, "sid=abc123; lang=cs"), "path matching keeps /shop cookies out of /admin", ch);
    wcookie_header(&sub, now + 7200, ch, sizeof(ch));
    check(!strcmp(ch, ""), "Max-Age cookies expire", ch);
    wcookie_set("lang=en; Domain=example.com; Path=/; Expires=Wed, 21 Oct 2037 07:28:00 GMT", &a, now);
    wcookie_set("cart=; Max-Age=0", &a, now);
    wcookie_header(&a, now, ch, sizeof(ch));
    check(!strcmp(ch, "sid=abc123; lang=en"), "cookies are replaced and deleted by name/domain/path", ch);
    char saved[8192];
    size_t sl = wcookie_save(saved, sizeof(saved), now);
    check(sl && strstr(saved, "example.com\t0\t/\t0\t2139722880\tlang\ten\t0\n") && !strstr(saved, "sid"),
          "only persistent cookies are saved", saved);
    wcookie_clear();
    wcookie_load(saved, sl, now);
    wcookie_header(&sub, now, ch, sizeof(ch));
    check(!strcmp(ch, "lang=en") && wcookie_count() == 1, "saved cookies load back", ch);
    wcookie_clear();
    for (int i = 0; i < 400; ++i) { char c[64]; snprintf(c, sizeof(c), "c%d=%d; Max-Age=999", i, i); wcookie_set(c, &a, now); }
    check(wcookie_count() == WCOOKIE_MAX, "the jar is bounded (oldest cookies are evicted)", 0);
    wcookie_clear();

    /* --- heap */
    int ok = 1;
    void *ptrs[512] = {0};
    unsigned seed = 12345;
    for (int round = 0; round < 20000 && ok; ++round) {
        seed = seed * 1103515245u + 12345u;
        int k = (int)((seed >> 8) % 512);
        if (ptrs[k] && (seed & 1)) { wmem_free(ptrs[k]); ptrs[k] = 0; }
        else if (ptrs[k]) {
            size_t n = (seed >> 12) % 70000;
            void *q = wmem_realloc(ptrs[k], n + 1);
            if (q) { ((uint8_t *)q)[n] = (uint8_t)k; ptrs[k] = q; }
        } else {
            size_t n = (seed >> 12) % 9000;
            ptrs[k] = wmem_alloc(n + 1);
            if (ptrs[k]) memset(ptrs[k], k, n + 1);
        }
        if (round % 500 == 0) ok = wmem_check();
    }
    for (int k = 0; k < 512; ++k) wmem_free(ptrs[k]);
    check(ok && wmem_check() && wmem_used() == 0, "heap stays consistent through 20000 random alloc/realloc/free", 0);
    check(wmem_alloc((size_t)1 << 40) == 0, "an impossible allocation fails cleanly", 0);
    warena ar; warena_init(&ar);
    for (int i = 0; i < 10000; ++i) { char *s = warena_alloc(&ar, 37); if (!s) { ok = 0; break; } s[36] = 1; }
    void *big = warena_alloc(&ar, 300000);
    check(ok && big && ar.total > 300000 && wmem_check(), "arenas allocate small and large blocks", 0);
    warena_free(&ar);
    check(wmem_used() == 0, "freeing an arena returns all its memory", 0);
}

static int64_t sink_drop(void *ctx, const uint8_t *p, size_t n) { (void)ctx; (void)p; (void)n; return 1; }

int main(int argc, char **argv) {
    static uint8_t arena[64u << 20];
    wmem_init(arena, sizeof(arena));
    (void)sink_drop;
    if (argc >= 2 && !strcmp(argv[1], "unit")) { unit(); return failures ? 1 : 0; }
    if (argc >= 5 && !strcmp(argv[1], "inflate")) {
        size_t n; uint8_t *in = read_file(argv[3], &n);
        if (!in) { printf("ERR read\n"); return 1; }
        int fmt = !strcmp(argv[2], "zlib") ? WINF_ZLIB : !strcmp(argv[2], "gzip") ? WINF_GZIP : !strcmp(argv[2], "auto") ? WINF_AUTO_DEFLATE : WINF_RAW;
        uint8_t *out = 0;
        int64_t r = winf_decode(in, n, fmt, 32u << 20, &out);
        if (r < 0) { printf("ERR %lld %s\n", (long long)r, winf_error_text(r)); return 0; }
        FILE *f = fopen(argv[4], "wb"); fwrite(out, 1, (size_t)r, f); fclose(f);
        wmem_free(out);
        printf("OK %lld\n", (long long)r);
        return wmem_check() && wmem_used() == 0 ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[1], "http")) {
        size_t n; uint8_t *in = read_file(argv[2], &n);
        if (!in) { printf("ERR read\n"); return 1; }
        unsigned seed = argc >= 5 ? (unsigned)atoi(argv[4]) : 1u;
        whttp_resp r; whttp_resp_init(&r, 8u << 20);
        size_t at = 0;
        while (at < n && r.state != WH_DONE && r.state != WH_ERROR) {
            seed = seed * 1103515245u + 12345u;
            size_t piece = 1 + (seed >> 8) % 700;
            if (piece > n - at) piece = n - at;
            size_t used = whttp_feed(&r, in + at, piece);
            at += used;
            if (used < piece) break;
        }
        if (r.state != WH_DONE && r.state != WH_ERROR) whttp_eof(&r);
        if (r.state != WH_DONE) { printf("ERR state %d error %d\n", r.state, r.error); return 0; }
        int64_t d = whttp_decode_body(&r, 32u << 20);
        if (d < 0) { printf("ERR decode %lld\n", (long long)d); return 0; }
        char type[64], cs[32], loc[256], name[128];
        whttp_content_type(&r, type, sizeof(type), cs, sizeof(cs));
        whttp_header(&r, "location", 0, loc, sizeof(loc));
        int att = whttp_disposition(&r, name, sizeof(name));
        FILE *f = fopen(argv[3], "wb"); if (r.body_len) fwrite(r.body, 1, r.body_len, f); fclose(f);
        printf("OK status=%d len=%zu consumed=%zu close=%d type=%s charset=%s location=%s attachment=%d name=%s truncated=%d cookies=",
               r.status, r.body_len, at, r.conn_close, type, cs, loc, att, name, r.truncated);
        char sc[512];
        for (uint32_t i = 0; whttp_header(&r, "set-cookie", i, sc, sizeof(sc)); ++i) printf("%s[%s]", i ? "," : "", sc);
        printf("\n");
        whttp_resp_free(&r);
        return wmem_check() && wmem_used() == 0 ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[1], "image")) {
        size_t n; uint8_t *in = read_file(argv[2], &n);
        if (!in) { printf("ERR read\n"); return 1; }
        int w = 0, h = 0;
        uint8_t *px = wimage_decode(in, n, &w, &h, 4u << 20);
        if (!px) { printf("ERR %s (%s)\n", wimage_failure(), wimage_format_name(wimage_sniff(in, n))); return wmem_used() == 0 ? 0 : 1; }
        FILE *f = fopen(argv[3], "wb"); fprintf(f, "%d %d\n", w, h); fwrite(px, 1, (size_t)w * h * 4, f); fclose(f);
        wmem_free(px);
        printf("OK %s %dx%d\n", wimage_format_name(wimage_sniff(in, n)), w, h);
        return wmem_check() && wmem_used() == 0 ? 0 : 1;
    }
    if (argc >= 6 && !strcmp(argv[1], "scale")) {
        size_t n; uint8_t *in = read_file(argv[2], &n);
        int w = 0, h = 0, dw = atoi(argv[3]), dh = atoi(argv[4]);
        uint8_t *px = in ? wimage_decode(in, n, &w, &h, 4u << 20) : 0;
        if (!px) { printf("ERR decode\n"); return 1; }
        uint32_t *o = wimage_scale(px, w, h, dw, dh);
        if (!o) { printf("ERR scale\n"); return 1; }
        FILE *f = fopen(argv[5], "wb"); fprintf(f, "%d %d\n", dw, dh); fwrite(o, 4, (size_t)dw * dh, f); fclose(f);
        wmem_free(o); wmem_free(px);
        printf("OK %dx%d\n", dw, dh);
        return wmem_check() && wmem_used() == 0 ? 0 : 1;
    }
    fprintf(stderr, "usage: test-web-core unit | inflate FMT IN OUT | http IN OUT [SEED]\n");
    return 2;
}
