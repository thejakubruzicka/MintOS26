/* MintDE Web — the MintOS browser on the MintOS Web engine (kernel/user/web).
 *
 * Loading: HTTP/1.1 with keep-alive over up to WEB_CONNS parallel
 * connections (plain TCP or MintOS TLS 1.3 / 1.2 with certificate checks that
 * cannot be bypassed), chunked bodies, gzip/deflate, redirects, cookies
 * (persisted to ~/Library/Web/Cookies on MintOS HD).
 * Pages: HTML -> DOM, the page's style sheets (<style>, <link>, style=) ->
 * computed styles, layout -> display list, painted with the MintDE renderer.
 * Images (PNG, JPEG, GIF, BMP) load in parallel and are decoded one per frame.
 * Forms: text, password, search, hidden, checkbox, radio, select, textarea,
 * submit and button controls, submitted with GET or POST.
 * Downloads go to ~/Downloads with progress; Favorites live in
 * ~/Library/Web/Bookmarks (Cmd-D). No scripts are ever run.
 * All memory comes from one bounded heap; every limit fails gracefully. */
#include "mintde.h"
#include "tls.h"
#include "../web/wmem.h"
#include "../web/whttp.h"
#include "../web/wdoc.h"
#include "../web/wlayout.h"
#include "../web/wimage.h"
#include "../web/inflate.h"
#include "font.h"

#define S (&gfx_screen)
static gfx_rect R(int32_t x, int32_t y, int32_t w, int32_t h) { return gfx_rect_xywh(x, y, w, h); }
static int32_t W(gfx_rect r) { return r.x1 - r.x0; }
static int32_t H(gfx_rect r) { return r.y1 - r.y0; }

/* Raise after the app-platform frame pool lands (MINT_ASPACE_MAX_USER_FRAMES = 16384). */
#define WEB_HEAP_BYTES   (8u * 1024u * 1024u)
#define WEB_CONNS        4u
#define WEB_REQS         48u
#define WEB_IMAGES       160u
#define WEB_HISTORY      32u
#define WEB_DOWNLOADS    12u
#define WEB_BOOKMARKS    48u
#define DOC_MAX          (6u * 1024u * 1024u)
#define CSS_MAX          (1536u * 1024u)
#define IMG_MAX          (3u * 1024u * 1024u)
#define IMG_PIXELS_MAX   (1600u * 1200u)
#define HOME_DIR         "/Users/mintos"
#define WEB_DIR          "/Users/mintos/Library/Web"
#define COOKIE_FILE      "/Users/mintos/Library/Web/Cookies"
#define BOOKMARK_FILE    "/Users/mintos/Library/Web/Bookmarks"
#define DOWNLOAD_DIR     "/Users/mintos/Downloads"

static uint8_t heap_mem[WEB_HEAP_BYTES] __attribute__((aligned(16)));
static int heap_ready;

/* ------------------------------------------------------------------ helpers */
static uint64_t web_ms(void) {
    mint_timespec64 t = {0, 0};
    (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static int64_t web_now(void) {
    mint_timespec64 t = {0, 0};
    return mint_clock_gettime(MINT_CLOCK_REALTIME, &t) == 0 ? t.tv_sec : 0;
}
static int str_eq(const char *a, const char *b) { size_t i = 0; for (; a[i] && a[i] == b[i]; ++i) {} return a[i] == b[i]; }

static char status_text[160];
static uint64_t status_until;
static int web_dirty;
static void set_status(const char *a, const char *b) {
    copy_text(status_text, sizeof(status_text), a ? a : "");
    if (b) str_append(status_text, sizeof(status_text), b);
    status_until = 0;
    web_dirty = 1;
}
static void flash_status(const char *a, const char *b) { set_status(a, b); status_until = web_ms() + 2500u; }

/* ------------------------------------------------------------------ requests and connections */
enum { RK_DOC = 1, RK_CSS, RK_IMG, RK_DL };
enum { RQ_FREE = 0, RQ_QUEUED, RQ_ACTIVE };
enum { CS_FREE = 0, CS_RESOLVE, CS_CONNECT, CS_TLS, CS_BUSY, CS_IDLE };

typedef struct req {
    int state, kind, method, conn, redirects, target, retried;
    uint32_t gen;
    wurl u;
    char *body; size_t body_len;
    char referer[256];
    whttp_resp resp;
    uint64_t started, last;
} req;

typedef struct conn {
    int state, req;
    wurl origin;
    uint32_t ip;
    int64_t handle;
    uint64_t since, last;
    char *out; size_t out_len, out_off;
    int reused, got_bytes;
} conn;

static req reqs[WEB_REQS];
static conn conns[WEB_CONNS];
static MINT_TLS tls_ctx[WEB_CONNS];
static uint32_t page_gen;

/* ------------------------------------------------------------------ images */
enum { IS_FREE = 0, IS_QUEUED, IS_LOADING, IS_DATA, IS_READY, IS_FAILED };
typedef struct imgent {
    int state;
    char url[WURL_MAX];
    uint8_t *data; size_t len;
    int32_t w, h;                 /* natural size */
    uint8_t *rgba;                /* decoded straight RGBA, until the first paint scales it */
    uint32_t *px; int32_t pw, ph; /* premultiplied ARGB at display size */
    uint64_t used_at;
    uint32_t gen;
} imgent;
static imgent images[WEB_IMAGES];

/* ------------------------------------------------------------------ page */
static struct page {
    int have;
    wdoc doc;
    wcss *css;
    wlayout_out lay;
    int laid_out, need_layout, need_style;
    int32_t lay_width;
    int16_t *img_slot;            /* doc image -> images[] index (-1) */
    uint32_t sheets_pending;
    uint64_t css_deadline, refresh_at;
    wurl url;
    int secure, internal, is_image;
    char fragment[128];
    char refresh_url[WURL_MAX];
    char *source;                 /* UTF-8 HTML kept for the DOM's lifetime */
} P;

static char page_title[200];
static char pending_fragment[128];
static char address[WURL_MAX];
static size_t address_len;
static int address_focused, address_replace;
static int32_t scroll_y;
static int32_t hover_link = -1, hover_dirty;
static int32_t focus_ctl = -1;
static int32_t select_open = -1;          /* control index with an open option list */
static int panel;                         /* 0 none, 1 favourites, 2 downloads */
static uint64_t caret_blink;
static char history[WEB_HISTORY][WURL_MAX];
static uint32_t hist_count, hist_pos;
static uint32_t cookie_gen_saved;

typedef struct dl { int state; char name[128]; char path[300]; uint64_t got, total; int64_t fd; char error[80]; int r; } dl;
enum { DL_FREE = 0, DL_ACTIVE, DL_DONE, DL_FAILED };
static dl downloads[WEB_DOWNLOADS];
typedef struct bookmark { char title[96]; char url[WURL_MAX]; } bookmark;
static bookmark bookmarks[WEB_BOOKMARKS];
static uint32_t bookmark_count;

static void navigate(const char *input, int add_history);
static void image_drop(int i);
static void start_page(void);
static void show_message(const char *title, const char *heading, const char *detail);
static void layout_page(int32_t width);

/* ------------------------------------------------------------------ files on MintOS HD */
static int home_available(void) {
    MINT_MFS_STAT st;
    return mint_path_stat(HOME_DIR, &st) == 0;
}
static void ensure_web_dir(void) {
    (void)mint_dir_create(HOME_DIR "/Library");
    (void)mint_dir_create(WEB_DIR);
}
static void save_cookies(void) {
    if (wcookie_generation() == cookie_gen_saved || !home_available()) return;
    size_t cap = 256u * 1024u;
    char *buf = (char *)wmem_alloc(cap);
    if (!buf) return;
    size_t n = wcookie_save(buf, cap, web_now());
    ensure_web_dir();
    if (mint_file_save(COOKIE_FILE, buf, n) >= 0) cookie_gen_saved = wcookie_generation();
    wmem_free(buf);
}
static void load_cookies(void) {
    if (!home_available()) return;
    size_t cap = 256u * 1024u;
    char *buf = (char *)wmem_alloc(cap);
    if (!buf) return;
    int64_t n = mint_file_load(COOKIE_FILE, buf, cap);
    if (n > 0) wcookie_load(buf, (size_t)n, web_now());
    cookie_gen_saved = wcookie_generation();
    wmem_free(buf);
}
static void save_bookmarks(void) {
    if (!home_available()) { flash_status("Favorites need a MintOS HD (set one up in Disk Utility)", 0); return; }
    static char buf[WEB_BOOKMARKS * (WURL_MAX + 100)];
    size_t n = 0;
    for (uint32_t i = 0; i < bookmark_count; ++i) {
        n += wstr_copy(buf + n, sizeof(buf) - n, bookmarks[i].title);
        n += wstr_copy(buf + n, sizeof(buf) - n, "\t");
        n += wstr_copy(buf + n, sizeof(buf) - n, bookmarks[i].url);
        n += wstr_copy(buf + n, sizeof(buf) - n, "\n");
    }
    ensure_web_dir();
    (void)mint_file_save(BOOKMARK_FILE, buf, n);
}
static void load_bookmarks(void) {
    static char buf[WEB_BOOKMARKS * (WURL_MAX + 100)];
    bookmark_count = 0;
    int64_t n = home_available() ? mint_file_load(BOOKMARK_FILE, buf, sizeof(buf) - 1u) : -1;
    if (n <= 0) {
        static const char *const defaults[][2] = {
            {"Wikipedia", "https://en.wikipedia.org/wiki/Main_Page"}, {"DuckDuckGo", "https://lite.duckduckgo.com/lite/"},
            {"Hacker News", "https://news.ycombinator.com/"}, {"NPR Text", "https://text.npr.org/"},
            {"MintOS on GitHub", "https://github.com/"}, {"CERN: first website", "http://info.cern.ch/hypertext/WWW/TheProject.html"},
        };
        for (uint32_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
            copy_text(bookmarks[i].title, sizeof(bookmarks[i].title), defaults[i][0]);
            copy_text(bookmarks[i].url, sizeof(bookmarks[i].url), defaults[i][1]);
        }
        bookmark_count = sizeof(defaults) / sizeof(defaults[0]);
        return;
    }
    buf[n] = '\0';
    size_t i = 0;
    while (i < (size_t)n && bookmark_count < WEB_BOOKMARKS) {
        size_t s = i;
        while (i < (size_t)n && buf[i] != '\n') ++i;
        size_t tab = s;
        while (tab < i && buf[tab] != '\t') ++tab;
        if (tab < i && tab > s) {
            bookmark *b = &bookmarks[bookmark_count];
            size_t tl = tab - s < sizeof(b->title) - 1u ? tab - s : sizeof(b->title) - 1u;
            for (size_t k = 0; k < tl; ++k) b->title[k] = buf[s + k];
            b->title[tl] = '\0';
            size_t ul = i - tab - 1 < sizeof(b->url) - 1u ? i - tab - 1 : sizeof(b->url) - 1u;
            for (size_t k = 0; k < ul; ++k) b->url[k] = buf[tab + 1 + k];
            b->url[ul] = '\0';
            if (b->url[0]) ++bookmark_count;
        }
        ++i;
    }
}
static int bookmark_index(const char *url) { for (uint32_t i = 0; i < bookmark_count; ++i) if (str_eq(bookmarks[i].url, url)) return (int)i; return -1; }
static void toggle_bookmark(void) {
    if (P.internal || !address[0]) { flash_status("Open a web page first, then press \xE2\x8C\x98" "D", 0); return; }
    int i = bookmark_index(address);
    if (i >= 0) {
        for (uint32_t k = (uint32_t)i + 1u; k < bookmark_count; ++k) bookmarks[k - 1] = bookmarks[k];
        --bookmark_count;
        save_bookmarks();
        flash_status("Removed from Favorites", 0);
        return;
    }
    if (bookmark_count >= WEB_BOOKMARKS) { flash_status("Favorites are full", 0); return; }
    bookmark *b = &bookmarks[bookmark_count++];
    copy_text(b->title, sizeof(b->title), page_title[0] ? page_title : P.url.host);
    copy_text(b->url, sizeof(b->url), address);
    save_bookmarks();
    flash_status("Added to Favorites: ", b->title);
}

/* ------------------------------------------------------------------ request queue */
static int req_new(int kind, const char *url, int method, const char *body, size_t body_len, int target) {
    for (uint32_t i = 0; i < WEB_REQS; ++i) {
        req *r = &reqs[i];
        if (r->state != RQ_FREE) continue;
        wb_set(r, 0, sizeof(*r));
        if (!wurl_parse(url, &r->u)) return -1;
        r->state = RQ_QUEUED; r->kind = kind; r->method = method; r->target = target; r->conn = -1; r->gen = page_gen;
        if (body && body_len) {
            r->body = (char *)wmem_alloc(body_len);
            if (!r->body) { r->state = RQ_FREE; return -1; }
            wb_copy(r->body, body, body_len); r->body_len = body_len;
        }
        if (!P.internal && P.have) wurl_format(&P.url, r->referer, sizeof(r->referer));
        r->started = r->last = web_ms();
        return (int)i;
    }
    return -1;
}
static void req_free(int i) {
    req *r = &reqs[i];
    whttp_resp_free(&r->resp);
    wmem_free(r->body);
    wb_set(r, 0, sizeof(*r));
    r->conn = -1;
}

static void conn_close(int c) {
    conn *k = &conns[c];
    if (k->state == CS_FREE) return;
    if (k->origin.secure && k->handle >= 0 && (k->state == CS_BUSY || k->state == CS_IDLE || k->state == CS_TLS)) mint_tls_close(&tls_ctx[c]);
    if (k->handle >= 0) (void)mint_tcp_close(k->handle);
    wmem_free(k->out);
    wb_set(k, 0, sizeof(*k));
    k->handle = -1; k->req = -1;
}

static int64_t tls_tcp_send(void *ctx, const void *data, size_t len) {
    int64_t r = mint_tcp_send(*(int64_t *)ctx, data, len > 4096u ? 4096u : len);
    if (r == -MINT_EAGAIN) return 0;
    return r < 0 ? -1 : r;
}
static int64_t tls_tcp_recv(void *ctx, void *out, size_t cap) {
    int64_t r = mint_tcp_recv(*(int64_t *)ctx, out, cap > 4096u ? 4096u : cap);
    if (r == -MINT_EAGAIN) return 0;
    return r > 0 ? r : -1;
}

/* Builds and queues the request text for req r on connection c. */
static int conn_send_request(int c) {
    conn *k = &conns[c];
    req *r = &reqs[k->req];
    static char cookie[4096];
    wcookie_header(&r->u, web_now(), cookie, sizeof(cookie));
    const char *accept = r->kind == RK_IMG ? "image/png,image/jpeg,image/gif,image/bmp;q=0.9,*/*;q=0.5" :
                         r->kind == RK_CSS ? "text/css,*/*;q=0.1" : 0;
    whttp_request_info info = {r->method, &r->u, accept, cookie, r->referer[0] ? r->referer : 0,
                               "application/x-www-form-urlencoded", r->body_len, 1};
    size_t cap = 8192u + r->body_len;
    char *buf = (char *)wmem_alloc(cap);
    if (!buf) return 0;
    size_t n = whttp_build_request(buf, cap - r->body_len, &info);
    if (!n) { wmem_free(buf); return 0; }
    if (r->body_len) { wb_copy(buf + n, r->body, r->body_len); n += r->body_len; }
    wmem_free(k->out);
    k->out = buf; k->out_len = n; k->out_off = 0;
    whttp_resp_init(&r->resp, r->kind == RK_DOC ? DOC_MAX : r->kind == RK_CSS ? CSS_MAX : r->kind == RK_IMG ? IMG_MAX : DOC_MAX);
    if (r->kind == RK_DOC) r->resp.pause_after_headers = 1;
    k->got_bytes = 0;
    return 1;
}

static void conn_flush(int c) {
    conn *k = &conns[c];
    if (!k->out || k->out_off >= k->out_len) return;
    if (k->origin.secure) {
        int64_t w = mint_tls_write(&tls_ctx[c], k->out + k->out_off, k->out_len - k->out_off);
        if (w > 0) k->out_off += (size_t)w;
        (void)mint_tls_flush(&tls_ctx[c]);
    } else {
        int64_t w = mint_tcp_send(k->handle, k->out + k->out_off, k->out_len - k->out_off);
        if (w > 0) k->out_off += (size_t)w;
    }
    if (k->out_off >= k->out_len) { wmem_free(k->out); k->out = 0; k->out_len = k->out_off = 0; }
}

/* ------------------------------------------------------------------ responses */
static void doc_response(int ri);
static void css_response(int ri);
static void img_response(int ri);
static void dl_finish(int ri, int ok, const char *why);

static void request_failed(int ri, const char *heading, const char *detail) {
    req *r = &reqs[ri];
    if (r->kind == RK_DOC && r->gen == page_gen) { show_message(heading, heading, detail); set_status(heading, 0); }
    else if (r->kind == RK_CSS && r->gen == page_gen && P.sheets_pending) { --P.sheets_pending; P.need_style = 1; }
    else if (r->kind == RK_IMG && r->target >= 0) { images[r->target].state = IS_FAILED; P.need_layout = 1; }
    else if (r->kind == RK_DL) dl_finish(ri, 0, detail);
    req_free(ri);
}

static void tls_failure_text(int c, char *out, size_t cap) {
    copy_text(out, cap, mint_tls_error_text(&tls_ctx[c]));
}

/* Handles a completed response for request ri (redirects, cookies, dispatch). */
static void request_done(int ri) {
    req *r = &reqs[ri];
    whttp_resp *rs = &r->resp;
    char v[WURL_MAX];
    for (uint32_t i = 0; whttp_header(rs, "set-cookie", i, v, sizeof(v)); ++i) (void)wcookie_set(v, &r->u, web_now());
    if (rs->status >= 300 && rs->status < 400 && rs->status != 304 && whttp_header(rs, "location", 0, v, sizeof(v)) && v[0]) {
        char target[WURL_MAX];
        if (!wurl_resolve(&r->u, v, target, sizeof(target)) || ++r->redirects > 10) {
            request_failed(ri, "Too many redirects", "The site keeps redirecting, or redirects to an address MintOS cannot open.");
            return;
        }
        wurl nu;
        if (!wurl_parse(target, &nu)) { request_failed(ri, "Can\xE2\x80\x99t open the page", "The redirect target is not valid."); return; }
        r->u = nu;
        if (rs->status == 303 || ((rs->status == 301 || rs->status == 302) && r->method == WHTTP_POST)) {
            r->method = WHTTP_GET; wmem_free(r->body); r->body = 0; r->body_len = 0;
        }
        whttp_resp_free(rs);
        r->state = RQ_QUEUED; r->conn = -1; r->retried = 0;
        if (r->kind == RK_DOC && r->gen == page_gen) {
            wurl_format(&nu, address, sizeof(address)); address_len = text_length(address);
            if (hist_count) copy_text(history[hist_pos], WURL_MAX, address);
        }
        return;
    }
    switch (r->kind) {
    case RK_DOC: doc_response(ri); break;
    case RK_CSS: css_response(ri); break;
    case RK_IMG: img_response(ri); break;
    case RK_DL: dl_finish(ri, rs->status < 400, rs->status >= 400 ? whttp_status_text(rs->status) : 0); break;
    default: break;
    }
    req_free(ri);
}

/* Download sink: writes body bytes to the open file. */
static int dl_sink(void *ctx, const uint8_t *data, size_t len) {
    dl *d = (dl *)ctx;
    if (d->fd < 0) return 0;
    if (mint_file_write(d->fd, data, len) < 0) { copy_text(d->error, sizeof(d->error), "the disk is full or not writable"); return 0; }
    d->got += len;
    web_dirty = 1;
    return 1;
}

static int dl_begin(int ri, const char *name_hint) {
    req *r = &reqs[ri];
    int slot = -1;
    for (uint32_t i = 0; i < WEB_DOWNLOADS; ++i) if (downloads[i].state == DL_FREE || downloads[i].state == DL_DONE || downloads[i].state == DL_FAILED) {
        if (downloads[i].state == DL_FREE) { slot = (int)i; break; }
        if (slot < 0) slot = (int)i;
    }
    if (slot < 0) return 0;
    dl *d = &downloads[slot];
    wb_set(d, 0, sizeof(*d));
    d->fd = -1; d->r = ri;
    copy_text(d->name, sizeof(d->name), name_hint && name_hint[0] ? name_hint : "download");
    if (!home_available()) {
        d->state = DL_FAILED;
        copy_text(d->error, sizeof(d->error), "downloads need a MintOS HD");
        panel = 2;
        return 0;
    }
    (void)mint_dir_create(DOWNLOAD_DIR);
    /* unique name: "file.pdf", "file 2.pdf", ... */
    char base[128], ext[32];
    copy_text(base, sizeof(base), d->name);
    ext[0] = '\0';
    size_t bl = text_length(base);
    for (size_t k = bl; k > 1; --k) if (base[k - 1] == '.') { if (bl - (k - 1) < sizeof(ext)) { copy_text(ext, sizeof(ext), base + k - 1); base[k - 1] = '\0'; } break; }
    for (int n = 1; n < 100; ++n) {
        copy_text(d->path, sizeof(d->path), DOWNLOAD_DIR "/");
        str_append(d->path, sizeof(d->path), base);
        if (n > 1) { char num[8]; format_u64(num, sizeof(num), (uint64_t)n); str_append(d->path, sizeof(d->path), " "); str_append(d->path, sizeof(d->path), num); }
        str_append(d->path, sizeof(d->path), ext);
        MINT_MFS_STAT st;
        if (mint_path_stat(d->path, &st) != 0) break;
    }
    const char *slash = d->path;
    for (const char *p = d->path; *p; ++p) if (*p == '/') slash = p + 1;
    copy_text(d->name, sizeof(d->name), slash);
    d->fd = mint_file_open(d->path, MINT_O_WRITE | MINT_O_CREATE | MINT_O_TRUNC);
    if (d->fd < 0) { d->state = DL_FAILED; copy_text(d->error, sizeof(d->error), "the file could not be created"); panel = 2; return 0; }
    d->state = DL_ACTIVE;
    d->total = r->resp.content_length > 0 ? (uint64_t)r->resp.content_length : 0;
    r->kind = RK_DL;
    r->target = slot;
    if (r->resp.encoding == WENC_IDENTITY) { r->resp.sink = dl_sink; r->resp.sink_ctx = d; }
    else r->resp.body_max = 64u * 1024u * 1024u;          /* compressed: keep, decode at the end */
    panel = 2;
    flash_status("Downloading ", d->name);
    return 1;
}

static void dl_finish(int ri, int ok, const char *why) {
    req *r = &reqs[ri];
    if (r->target < 0 || r->target >= (int)WEB_DOWNLOADS) return;
    dl *d = &downloads[r->target];
    if (ok && r->resp.encoding != WENC_IDENTITY) {
        if (whttp_decode_body(&r->resp, 64u * 1024u * 1024u) < 0) { ok = 0; why = "the download could not be decompressed"; }
        else if (r->resp.body_len && mint_file_write(d->fd, r->resp.body, r->resp.body_len) < 0) { ok = 0; why = "the disk is full"; }
        else d->got = r->resp.body_len;
    }
    if (d->fd >= 0) { (void)mint_file_close(d->fd); d->fd = -1; }
    (void)mint_fs_flush();
    if (ok && !r->resp.truncated) { d->state = DL_DONE; d->total = d->got; flash_status("Downloaded ", d->name); }
    else {
        d->state = DL_FAILED;
        copy_text(d->error, sizeof(d->error), why ? why : d->error[0] ? d->error : "the connection was interrupted");
        (void)mint_path_remove(d->path);
    }
    d->r = -1;
    web_dirty = 1;
}

/* Called when a document request has its headers: decides page vs download. */
static int doc_headers(int ri) {
    req *r = &reqs[ri];
    whttp_resp *rs = &r->resp;
    if (rs->status >= 300 && rs->status < 400) return 1;
    char type[96], cs[32], name[128];
    whttp_content_type(rs, type, sizeof(type), cs, sizeof(cs));
    int attachment = whttp_disposition(rs, name, sizeof(name));
    int displayable = !type[0] || wstr_iprefix(type, "text/") || wstr_iprefix(type, "application/xhtml") ||
                      wstr_iprefix(type, "image/png") || wstr_iprefix(type, "image/jpeg") || wstr_iprefix(type, "image/gif") ||
                      wstr_iprefix(type, "image/bmp") || wstr_ieq(type, "application/json") || wstr_ieq(type, "application/xml");
    if (rs->status < 400 && (attachment || !displayable)) {
        if (!name[0]) wurl_file_name(&r->u, name, sizeof(name));
        if (!dl_begin(ri, name)) { request_failed(ri, "Download failed", "MintOS could not save the file. Downloads need a MintOS HD."); return 0; }
        /* the page being shown stays; restore its address */
        if (hist_count && P.have) { wurl_format(&P.url, address, sizeof(address)); address_len = text_length(address); }
        return 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ network pump */
static int conn_for(int ri) {
    req *r = &reqs[ri];
    for (uint32_t c = 0; c < WEB_CONNS; ++c)
        if (conns[c].state == CS_IDLE && wurl_same_origin(&conns[c].origin, &r->u)) return (int)c;
    for (uint32_t c = 0; c < WEB_CONNS; ++c) if (conns[c].state == CS_FREE) return (int)c;
    /* reuse the least recently used idle connection to another site */
    int best = -1;
    for (uint32_t c = 0; c < WEB_CONNS; ++c) if (conns[c].state == CS_IDLE && (best < 0 || conns[c].last < conns[best].last)) best = (int)c;
    if (best >= 0) { conn_close(best); return best; }
    return -1;
}

static void assign_requests(void) {
    /* documents first, then style sheets, downloads, images */
    static const int order[4] = {RK_DOC, RK_CSS, RK_DL, RK_IMG};
    for (int o = 0; o < 4; ++o) {
        for (uint32_t i = 0; i < WEB_REQS; ++i) {
            req *r = &reqs[i];
            if (r->state != RQ_QUEUED || r->kind != order[o]) continue;
            if (r->kind == RK_CSS && r->gen != page_gen) { req_free((int)i); continue; }
            if (r->kind == RK_IMG && r->target >= 0 && images[r->target].gen != page_gen) { image_drop(r->target); req_free((int)i); continue; }
            int c = conn_for((int)i);
            if (c < 0) return;
            conn *k = &conns[c];
            r->state = RQ_ACTIVE; r->conn = c; r->last = web_ms();
            if (k->state == CS_IDLE) {
                k->state = CS_BUSY; k->req = (int)i; k->reused = 1; k->last = web_ms();
                if (!conn_send_request(c)) { conn_close(c); request_failed((int)i, "Can\xE2\x80\x99t open the page", "The request could not be built."); }
                continue;
            }
            wb_set(k, 0, sizeof(*k));
            k->handle = -1; k->req = (int)i; k->origin = r->u; k->state = CS_RESOLVE; k->since = k->last = web_ms();
        }
    }
}

static void conn_failed(int c, const char *heading, const char *detail) {
    int ri = conns[c].req;
    conn_close(c);
    if (ri >= 0 && reqs[ri].state == RQ_ACTIVE) request_failed(ri, heading, detail);
}

/* Advances one connection; returns 1 if something changed. */
static int pump_conn(int c, uint64_t now) {
    conn *k = &conns[c];
    if (k->state == CS_FREE) return 0;
    if (k->state == CS_IDLE) {
        /* idle keep-alive: drop it after 20 s or if the server closed it */
        if (now - k->last > 20000u || mint_tcp_state(k->handle) != MINT_TCP_STATE_ESTABLISHED) { conn_close(c); return 0; }
        return 0;
    }
    int ri = k->req;
    if (ri < 0) { conn_close(c); return 0; }
    req *r = &reqs[ri];
    if (k->state == CS_RESOLVE) {
        int64_t rc = mint_net_resolve_host(k->origin.host, &k->ip);
        if (rc == -MINT_EAGAIN) {
            if (now - k->since > 10000u) conn_failed(c, "Can\xE2\x80\x99t find the server", "The name lookup timed out.");
            return 0;
        }
        if (rc == -MINT_ENETDOWN) { conn_failed(c, "You are not connected to the Internet", "Connect in the Network app or Settings, then reload."); return 1; }
        if (rc < 0) { conn_failed(c, "Can\xE2\x80\x99t find the server", "The name does not exist or the DNS server did not answer."); return 1; }
        int64_t h = mint_tcp_connect(k->ip, k->origin.port);
        if (h < 0) { conn_failed(c, "Can\xE2\x80\x99t open the page", "No connection could be opened (all network connections are busy)."); return 1; }
        k->handle = h; k->state = CS_CONNECT; k->last = now;
        if (r->kind == RK_DOC) set_status("Connecting to ", k->origin.host);
        return 1;
    }
    if (k->state == CS_CONNECT) {
        int64_t st = mint_tcp_state(k->handle);
        if (st == MINT_TCP_STATE_ESTABLISHED) {
            if (k->origin.secure) {
                MINT_TLS_IO io = {&k->handle, tls_tcp_send, tls_tcp_recv};
                mint_tls_init(&tls_ctx[c], &io, k->origin.host, web_now());
                k->state = CS_TLS; k->last = now;
                if (r->kind == RK_DOC) set_status("Securing the connection to ", k->origin.host);
                return 1;
            }
            k->state = CS_BUSY; k->last = now;
            if (!conn_send_request(c)) { conn_failed(c, "Can\xE2\x80\x99t open the page", "The request could not be built."); return 1; }
            if (r->kind == RK_DOC) set_status("Waiting for ", k->origin.host);
            return 1;
        }
        if (st != MINT_TCP_STATE_CONNECTING) {
            conn_failed(c, "Can\xE2\x80\x99t connect to the server", st == MINT_TCP_STATE_REFUSED ? "The server refused the connection." :
                        st == MINT_TCP_STATE_UNREACHABLE ? "The server is unreachable." : "The connection failed.");
            return 1;
        }
        if (now - k->last > 15000u) { conn_failed(c, "Can\xE2\x80\x99t connect to the server", "The connection timed out."); return 1; }
        return 0;
    }
    if (k->state == CS_TLS) {
        int rc = mint_tls_step(&tls_ctx[c]);
        if (rc == MINT_TLS_AGAIN) {
            if (now - k->last > 20000u) { conn_failed(c, "Secure connection failed", "The secure handshake timed out."); return 1; }
            return 0;
        }
        if (rc != MINT_TLS_DONE) {
            static char detail[360];
            char why[200];
            tls_failure_text(c, why, sizeof(why));
            if (tls_ctx[c].error == MINT_TLS_ERR_CERT) {
                copy_text(detail, sizeof(detail), "The certificate presented by ");
                str_append(detail, sizeof(detail), k->origin.host);
                str_append(detail, sizeof(detail), " could not be verified: ");
                str_append(detail, sizeof(detail), why);
                str_append(detail, sizeof(detail), ". Someone could be impersonating the site, so MintOS did not load it and sent no request.");
                conn_failed(c, "This connection is not private", detail);
            } else {
                copy_text(detail, sizeof(detail), "MintOS could not set up an encrypted connection to ");
                str_append(detail, sizeof(detail), k->origin.host);
                str_append(detail, sizeof(detail), ": ");
                str_append(detail, sizeof(detail), why);
                str_append(detail, sizeof(detail), ".");
                conn_failed(c, "Secure connection failed", detail);
            }
            return 1;
        }
        k->state = CS_BUSY; k->last = now;
        if (!conn_send_request(c)) { conn_failed(c, "Can\xE2\x80\x99t open the page", "The request could not be built."); return 1; }
        if (r->kind == RK_DOC) set_status("Waiting for ", k->origin.host);
        return 1;
    }
    /* busy: send what is pending, then read */
    conn_flush(c);
    static uint8_t buf[16384];
    int changed = 0;
    for (uint32_t burst = 0; burst < 24u; ++burst) {
        int64_t n;
        if (k->origin.secure) {
            n = mint_tls_read(&tls_ctx[c], buf, sizeof(buf));
            if (n == MINT_TLS_WOULD_BLOCK) n = -MINT_EAGAIN;
            else if (n < 0) n = -1;
        } else n = mint_tcp_recv(k->handle, buf, sizeof(buf) > 4096u ? 4096u : sizeof(buf));
        if (n == -MINT_EAGAIN) break;
        if (n <= 0) {
            /* connection closed */
            if (!k->got_bytes && k->reused && !r->retried) {          /* stale keep-alive: retry once */
                conn_close(c);
                r->state = RQ_QUEUED; r->conn = -1; r->retried = 1;
                whttp_resp_free(&r->resp);
                return 1;
            }
            whttp_eof(&r->resp);
            conn_close(c);
            if (r->resp.state == WH_DONE) request_done(ri);
            else request_failed(ri, "The connection was lost", "The server closed the connection before sending the page.");
            return 1;
        }
        k->got_bytes = 1; k->last = now; r->last = now; changed = 1;
        size_t at = 0;
        while (at < (size_t)n) {
            size_t used = whttp_feed(&r->resp, buf + at, (size_t)n - at);
            at += used;
            if (r->resp.headers_ready && r->kind == RK_DOC && !r->resp.sink && r->resp.state != WH_ERROR && !r->resp.pause_after_headers) {
                r->resp.headers_ready = 0;
                if (!doc_headers(ri)) return 1;
                if (r->kind == RK_DL) r = &reqs[ri];
            }
            if (r->resp.state == WH_DONE || r->resp.state == WH_ERROR) break;
            if (!used) break;
        }
        if (r->resp.state == WH_ERROR) { conn_failed(c, "The server sent an invalid response", "The reply was not valid HTTP."); return 1; }
        if (r->resp.state == WH_DONE) {
            int keep = !r->resp.conn_close && r->resp.http_minor >= 1;
            if (keep) { k->state = CS_IDLE; k->req = -1; k->last = now; }
            else conn_close(c);
            request_done(ri);
            return 1;
        }
    }
    if (r->kind == RK_DOC && r->gen == page_gen) {
        static uint64_t shown;
        if (shown != r->resp.body_total / 8192u) {
            shown = r->resp.body_total / 8192u;
            char size[24]; format_bytes(size, sizeof(size), r->resp.body_total);
            set_status("Loading\xE2\x80\xA6 ", size);
        }
    }
    uint64_t limit = r->kind == RK_DL ? 60000u : 25000u;
    if (now - k->last > limit) {
        if (r->resp.state == WH_BODY_CLOSE || (r->resp.body_total && r->kind == RK_DOC)) {
            whttp_eof(&r->resp);
            conn_close(c);
            if (r->resp.state == WH_DONE) request_done(ri); else request_failed(ri, "The page took too long", "The server stopped answering.");
        } else conn_failed(c, "The page took too long", "The server stopped answering.");
        return 1;
    }
    return changed;
}

static void stop_all(int keep_downloads) {
    for (uint32_t i = 0; i < WEB_REQS; ++i) {
        if (reqs[i].state == RQ_FREE) continue;
        if (keep_downloads && reqs[i].kind == RK_DL) continue;
        if (reqs[i].conn >= 0) conn_close(reqs[i].conn);
        if (reqs[i].kind == RK_DL) dl_finish((int)i, 0, "stopped");
        req_free((int)i);
    }
}
static int loading(void) {
    for (uint32_t i = 0; i < WEB_REQS; ++i) if (reqs[i].state != RQ_FREE && (reqs[i].kind == RK_DOC || ((reqs[i].kind == RK_CSS || reqs[i].kind == RK_IMG) && reqs[i].gen == page_gen))) return 1;
    return 0;
}
static int doc_loading(void) {
    for (uint32_t i = 0; i < WEB_REQS; ++i) if (reqs[i].state != RQ_FREE && reqs[i].kind == RK_DOC) return 1;
    return 0;
}

/* ------------------------------------------------------------------ images */
static int image_size_cb(void *ctx, int32_t img, int32_t *w, int32_t *h) {
    (void)ctx;
    if (!P.img_slot || img < 0 || (uint32_t)img >= P.doc.img_count || P.img_slot[img] < 0) return 0;
    imgent *e = &images[P.img_slot[img]];
    if (e->state != IS_READY && !(e->state == IS_DATA && e->w)) return 0;
    *w = e->w; *h = e->h;
    return e->w > 0 && e->h > 0;
}
static int image_state_cb(void *ctx, int32_t img) {
    (void)ctx;
    if (!P.img_slot || img < 0 || (uint32_t)img >= P.doc.img_count || P.img_slot[img] < 0) return -1;
    int s = images[P.img_slot[img]].state;
    return s == IS_READY ? 1 : s == IS_FAILED ? -1 : 0;
}

static void image_drop(int i) {
    imgent *e = &images[i];
    wmem_free(e->data); wmem_free(e->px); wmem_free(e->rgba);
    wb_set(e, 0, sizeof(*e));
}
static int base64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
/* Finds or creates the cache entry for url and queues its fetch. */
static int image_request(const char *url) {
    uint64_t now = web_ms();
    for (uint32_t i = 0; i < WEB_IMAGES; ++i) if (images[i].state != IS_FREE && str_eq(images[i].url, url)) { images[i].used_at = now; images[i].gen = page_gen; return (int)i; }
    int slot = -1;
    for (uint32_t i = 0; i < WEB_IMAGES; ++i) if (images[i].state == IS_FREE) { slot = (int)i; break; }
    if (slot < 0) {                                   /* evict the least recently used finished image */
        for (uint32_t i = 0; i < WEB_IMAGES; ++i) {
            int s = images[i].state;
            if ((s == IS_READY || s == IS_FAILED) && images[i].gen != page_gen && (slot < 0 || images[i].used_at < images[slot].used_at)) slot = (int)i;
        }
        if (slot < 0) return -1;
        image_drop(slot);
    }
    imgent *e = &images[slot];
    copy_text(e->url, sizeof(e->url), url);
    e->used_at = now; e->gen = page_gen;
    if (wstr_iprefix(url, "data:")) {                 /* inline base64 image */
        const char *comma = url;
        while (*comma && *comma != ',') ++comma;
        int b64 = 0;
        for (const char *p = url; p < comma; ++p) if (wstr_iprefix(p, ";base64")) b64 = 1;
        if (!*comma || !b64) { e->state = IS_FAILED; return slot; }
        ++comma;
        size_t n = text_length(comma);
        e->data = (uint8_t *)wmem_alloc(n * 3u / 4u + 4u);
        if (!e->data) { e->state = IS_FAILED; return slot; }
        uint32_t acc = 0; int bits = 0; size_t o = 0;
        for (size_t k = 0; k < n; ++k) {
            int v = base64_val(comma[k]);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v; bits += 6;
            if (bits >= 8) { bits -= 8; e->data[o++] = (uint8_t)(acc >> bits); }
        }
        e->len = o; e->state = IS_DATA;
        return slot;
    }
    if (req_new(RK_IMG, url, WHTTP_GET, 0, 0, slot) < 0) { e->state = IS_QUEUED; return slot; }
    e->state = IS_LOADING;
    return slot;
}

static void img_response(int ri) {
    req *r = &reqs[ri];
    if (r->target < 0) return;
    imgent *e = &images[r->target];
    if (r->resp.status >= 400 || whttp_decode_body(&r->resp, IMG_MAX) < 0 || !r->resp.body_len || r->resp.truncated) {
        e->state = IS_FAILED; P.need_layout = 1; return;
    }
    e->data = r->resp.body; e->len = r->resp.body_len;
    r->resp.body = 0; r->resp.body_len = r->resp.body_cap = 0;
    int w = 0, h = 0;
    if (wimage_info(e->data, e->len, &w, &h)) { e->w = w; e->h = h; }
    e->state = IS_DATA;
}

/* Decodes one fetched image (called once per frame). */
static int decode_one(void) {
    for (uint32_t i = 0; i < WEB_IMAGES; ++i) {
        imgent *e = &images[i];
        if (e->state == IS_QUEUED) { if (req_new(RK_IMG, e->url, WHTTP_GET, 0, 0, (int)i) >= 0) e->state = IS_LOADING; continue; }
        if (e->state != IS_DATA) continue;
        int w = 0, h = 0;
        uint8_t *rgba = wimage_decode(e->data, e->len, &w, &h, IMG_PIXELS_MAX);
        if (!rgba) { e->state = IS_FAILED; wmem_free(e->data); e->data = 0; P.need_layout = 1; return 1; }
        /* scale once to the size the page asks for (width attribute, or at most the page width) */
        e->w = w; e->h = h;
        int32_t tw = w, th = h, limit = P.lay_width > 100 ? P.lay_width : 1200;
        for (uint32_t k = 0; P.img_slot && k < P.doc.img_count; ++k) {
            if (P.img_slot[k] != (int16_t)i) continue;
            const char *aw = wnode_attr(P.doc.imgs[k], "width");
            int32_t v = 0;
            for (const char *q = aw ? aw : ""; *q >= '0' && *q <= '9' && v < 10000; ++q) v = v * 10 + (*q - '0');
            if (v > 0 && v < tw) { th = (int32_t)((int64_t)th * v / tw); tw = v; }
            break;
        }
        if (tw > limit) { th = (int32_t)((int64_t)th * limit / tw); tw = limit; }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        e->px = wimage_scale(rgba, w, h, tw, th);
        e->pw = tw; e->ph = th;
        wmem_free(rgba);
        wmem_free(e->data); e->data = 0; e->len = 0;
        if (!e->px) { e->state = IS_FAILED; P.need_layout = 1; return 1; }
        e->state = IS_READY;
        /* keep memory for the current page: drop bitmaps of other pages when the heap runs low */
        if (wmem_used() > WEB_HEAP_BYTES / 10u * 8u)
            for (uint32_t k = 0; k < WEB_IMAGES; ++k) if (images[k].gen != page_gen && images[k].state != IS_FREE && images[k].state != IS_LOADING) image_drop((int)k);
        P.need_layout = 1;
        return 1;
    }
    return 0;
}

/* Resolves the source of <img> (src, lazy-load data-src, srcset). */
static const char *img_source(const wnode *n) {
    const char *src = wnode_attr(n, "src");
    const char *lazy = wnode_attr(n, "data-src");
    if (!lazy) lazy = wnode_attr(n, "data-lazy-src");
    if (!lazy) lazy = wnode_attr(n, "data-original");
    if ((!src || !src[0] || (wstr_iprefix(src, "data:") && text_length(src) < 200)) && lazy && lazy[0]) return lazy;
    if (src && src[0]) return src;
    const char *ss = wnode_attr(n, "srcset");
    if (!ss) ss = wnode_attr(n, "data-srcset");
    return ss;
}

/* ------------------------------------------------------------------ page pipeline */
static void page_free(void) {
    wlayout_free(&P.lay);
    if (P.css) wcss_free(P.css);
    wmem_free(P.img_slot);
    if (P.have) wdoc_free(&P.doc);
    wmem_free(P.source);
    wb_set(&P, 0, sizeof(P));
    focus_ctl = -1; select_open = -1; hover_link = -1;
}

static void page_set_html(const char *utf8, size_t len, int as_text, const wurl *u, int internal) {
    page_free();
    ++page_gen;
    P.internal = internal;
    if (u) P.url = *u;
    P.source = (char *)wmem_alloc(len + 1u);
    if (P.source) { wb_copy(P.source, utf8, len); P.source[len] = '\0'; }
    if (!P.source || whtml_parse(&P.doc, P.source, len, as_text) != 0) {
        set_status("Not enough memory to show this page", 0);
        return;
    }
    P.have = 1;
    copy_text(page_title, sizeof(page_title), P.doc.title[0] ? P.doc.title : (u ? u->host : "Web"));
    /* base URL */
    if (P.doc.base_href && !internal) {
        char abs[WURL_MAX];
        wurl b;
        if (wurl_resolve(&P.url, P.doc.base_href, abs, sizeof(abs)) && wurl_parse(abs, &b)) P.url = b;
    }
    /* style sheets */
    for (uint32_t i = 0; i < P.doc.sheet_count && !internal; ++i) {
        wsheet_src *s = &P.doc.sheets[i];
        if (s->kind != WSHEET_LINK || !s->href) continue;
        char abs[WURL_MAX];
        if (!wurl_resolve(&P.url, s->href, abs, sizeof(abs))) continue;
        if (s->media && s->media[0] && (wstr_iprefix(s->media, "print") || wstr_iprefix(s->media, "speech"))) continue;
        if (req_new(RK_CSS, abs, WHTTP_GET, 0, 0, (int)i) >= 0) ++P.sheets_pending;
    }
    P.css_deadline = web_ms() + 6000u;
    /* images */
    if (P.doc.img_count) {
        P.img_slot = (int16_t *)wmem_alloc(P.doc.img_count * sizeof(int16_t));
        for (uint32_t i = 0; P.img_slot && i < P.doc.img_count; ++i) {
            P.img_slot[i] = -1;
            const char *src = img_source(P.doc.imgs[i]);
            if (!src || !src[0]) continue;
            char abs[WURL_MAX];
            /* srcset: the first candidate */
            char first[WURL_MAX]; size_t k = 0;
            while (src[k] && src[k] != ' ' && src[k] != ',' && k + 1 < sizeof(first)) { first[k] = src[k]; ++k; }
            first[k] = '\0';
            if (wstr_iprefix(src, "data:")) copy_text(abs, sizeof(abs), src);
            else if (!wurl_resolve(&P.url, first, abs, sizeof(abs))) continue;
            P.img_slot[i] = (int16_t)image_request(abs);
        }
    }
    /* meta refresh */
    if (P.doc.refresh && !internal) {
        const char *c = P.doc.refresh;
        int delay = 0;
        while (*c >= '0' && *c <= '9') delay = delay * 10 + (*c++ - '0');
        while (*c == ';' || *c == ',' || *c == ' ') ++c;
        if (wstr_iprefix(c, "url=")) c += 4;
        if (*c == '\'' || *c == '"') ++c;
        if (*c && delay <= 10) {
            char tmp[WURL_MAX]; copy_text(tmp, sizeof(tmp), c);
            size_t l = text_length(tmp);
            while (l && (tmp[l - 1] == '\'' || tmp[l - 1] == '"' || tmp[l - 1] == ' ')) tmp[--l] = '\0';
            if (wurl_resolve(&P.url, tmp, P.refresh_url, sizeof(P.refresh_url))) P.refresh_at = web_ms() + (uint64_t)delay * 1000u + 300u;
        }
    }
    P.need_style = 1;
    scroll_y = 0;
    web_dirty = 1;
}

static void style_page(int32_t width) {
    if (!P.have) return;
    if (P.css) { wcss_free(P.css); P.css = 0; }
    P.css = wcss_create(&P.doc, width, 0);
    if (!P.css || wcss_apply(P.css, &P.doc) != 0) set_status("Not enough memory for this page\xE2\x80\x99s styles", 0);
    P.need_style = 0;
    P.need_layout = 1;
}

static void layout_page(int32_t width) {
    if (!P.have || !P.css) return;
    wlayout_env env = {width, 800, image_size_cb, image_state_cb, 0};
    if (wlayout_run(&P.doc, &env, &P.lay) != 0) set_status("Not enough memory to lay out the whole page", 0);
    P.lay_width = width;
    P.laid_out = 1;
    P.need_layout = 0;
    if (P.fragment[0]) {
        wnode *n = wdoc_find_id(&P.doc, P.fragment);
        if (n) scroll_y = n->y > 8 ? n->y - 8 : 0;
        P.fragment[0] = '\0';
    }
    web_dirty = 1;
}

static void doc_response(int ri) {
    req *r = &reqs[ri];
    whttp_resp *rs = &r->resp;
    if (r->gen != page_gen) return;
    int64_t dr = whttp_decode_body(rs, DOC_MAX);
    if (dr < 0) { show_message("This page can\xE2\x80\x99t be shown", "This page can\xE2\x80\x99t be shown", winf_error_text(dr)); return; }
    char type[96], cs[32];
    whttp_content_type(rs, type, sizeof(type), cs, sizeof(cs));
    int secure = r->u.secure;
    if (wstr_iprefix(type, "image/")) {
        /* show the image on its own page */
        static char html[WURL_MAX + 200];
        char url[WURL_MAX];
        wurl_format(&r->u, url, sizeof(url));
        copy_text(html, sizeof(html), "<title>Image</title><body style=\"margin:0;background:#2b2b2b;text-align:center\"><img src=\"");
        str_append(html, sizeof(html), url);
        str_append(html, sizeof(html), "\" style=\"max-width:100%\"></body>");
        int slot = image_request(url);
        if (slot >= 0 && images[slot].state == IS_LOADING) {
            /* reuse the bytes we already have */
            for (uint32_t i = 0; i < WEB_REQS; ++i) if (reqs[i].state != RQ_FREE && reqs[i].kind == RK_IMG && reqs[i].target == slot) { if (reqs[i].conn >= 0) conn_close(reqs[i].conn); req_free((int)i); }
            images[slot].data = rs->body; images[slot].len = rs->body_len; images[slot].state = IS_DATA;
            rs->body = 0; rs->body_len = rs->body_cap = 0;
        }
        page_set_html(html, text_length(html), 0, &r->u, 0);
        P.secure = secure; P.is_image = 1;
        const char *slash = r->u.path;
        for (const char *p = r->u.path; *p && *p != '?'; ++p) if (*p == '/') slash = p + 1;
        copy_text(page_title, sizeof(page_title), slash[0] ? slash : "Image");
        return;
    }
    char sniff[32];
    if (!cs[0]) { whtml_sniff_charset(rs->body, rs->body_len, sniff, sizeof(sniff)); copy_text(cs, sizeof(cs), sniff); }
    size_t ul = 0;
    char *utf = wcharset_to_utf8(rs->body ? rs->body : (const uint8_t *)"", rs->body_len, cs, &ul);
    whttp_resp_free(rs);                           /* the raw bytes are no longer needed */
    if (!utf) { show_message("Not enough memory", "Not enough memory", "This page is too large for the memory MintOS gives the browser."); return; }
    int as_text = type[0] && !wstr_iprefix(type, "text/html") && !wstr_iprefix(type, "application/xhtml");
    wurl u = r->u;
    page_set_html(utf, ul, as_text, &u, 0);
    wmem_free(utf);
    P.secure = secure;
    char size[24], codes[8];
    format_bytes(size, sizeof(size), ul);
    format_u64(codes, sizeof(codes), (uint64_t)rs->status);
    if (rs->status >= 400) { set_status("HTTP ", codes); str_append(status_text, sizeof(status_text), " "); str_append(status_text, sizeof(status_text), whttp_status_text(rs->status)); }
    else set_status("Done \xE2\x80\x94 ", size);
    if (rs->truncated || P.doc.truncated) str_append(status_text, sizeof(status_text), " (page truncated)");
    status_until = web_ms() + 3000u;
    save_cookies();
}

static void css_response(int ri) {
    req *r = &reqs[ri];
    if (r->gen != page_gen || !P.have) return;
    if (P.sheets_pending) --P.sheets_pending;
    if (r->resp.status < 400 && whttp_decode_body(&r->resp, CSS_MAX) == 0 && r->resp.body_len && (uint32_t)r->target < P.doc.sheet_count) {
        char *text = warena_strndup(&P.doc.arena, (const char *)r->resp.body, r->resp.body_len);
        if (text) wcss_set_sheet_text(&P.doc, (uint32_t)r->target, text, r->resp.body_len);
    }
    if (!P.sheets_pending) P.need_style = 1;
    else if (web_ms() > P.css_deadline) P.need_style = 1;
}

/* ------------------------------------------------------------------ navigation */
static void history_push(const char *url) {
    if (hist_count && str_eq(history[hist_pos], url)) return;
    if (hist_count) hist_count = hist_pos + 1u;
    if (hist_count == WEB_HISTORY) {
        for (uint32_t i = 1; i < WEB_HISTORY; ++i) copy_text(history[i - 1], WURL_MAX, history[i]);
        --hist_count;
    }
    copy_text(history[hist_count], WURL_MAX, url);
    hist_pos = hist_count++;
}

static void load_url(const char *url, int method, const char *body, size_t body_len) {
    stop_all(1);
    ++page_gen;
    wurl u;
    if (!wurl_parse(url, &u)) { flash_status("That address is not valid", 0); return; }
    wurl_format(&u, address, sizeof(address)); address_len = text_length(address);
    /* same document, different fragment */
    const char *hash = 0;
    for (const char *p = url; *p; ++p) if (*p == '#') { hash = p + 1; break; }
    if (hash && P.have && !P.internal && wurl_same_origin(&u, &P.url) && str_eq(u.path, P.url.path) && method == WHTTP_GET) {
        wnode *n = wdoc_find_id(&P.doc, hash);
        if (n) { scroll_y = n->y > 8 ? n->y - 8 : 0; web_dirty = 1; return; }
    }
    P.fragment[0] = '\0';
    int ri = req_new(RK_DOC, url, method, body, body_len, -1);
    if (ri < 0) { flash_status("Too many requests are open", 0); return; }
    if (hash) copy_text(pending_fragment, sizeof(pending_fragment), hash);
    set_status("Looking up ", u.host);
}


static void navigate(const char *input, int add_history) {
    char text[WURL_MAX];
    size_t n = 0;
    while (*input == ' ') ++input;
    while (input[n] && n + 1u < sizeof(text)) { text[n] = input[n]; ++n; }
    while (n && text[n - 1] == ' ') --n;
    text[n] = '\0';
    address_focused = 0; panel = 0; select_open = -1;
    if (!n || wstr_iprefix(text, "about:")) {
        stop_all(1);
        if (add_history) history_push("about:start");
        start_page();
        return;
    }
    if (wstr_iprefix(text, "mailto:") || wstr_iprefix(text, "javascript:") || wstr_iprefix(text, "tel:")) {
        flash_status(wstr_iprefix(text, "javascript:") ? "Scripts are not supported" : "That kind of link is not supported", 0);
        return;
    }
    int has_space = 0, has_dot = 0, has_scheme = wstr_iprefix(text, "http://") || wstr_iprefix(text, "https://");
    for (size_t i = 0; i < n; ++i) { if (text[i] == ' ') has_space = 1; if (text[i] == '.' || text[i] == ':' || text[i] == '/') has_dot = 1; }
    char url[WURL_MAX];
    if (!has_scheme && (has_space || !has_dot)) {
        char q[WURL_MAX];
        (void)wform_append(q, sizeof(q), 0, "q", text);
        copy_text(url, sizeof(url), "https://lite.duckduckgo.com/lite/?");
        str_append(url, sizeof(url), q);
    } else if (!has_scheme) {
        copy_text(url, sizeof(url), "https://");
        str_append(url, sizeof(url), text);
    } else copy_text(url, sizeof(url), text);
    pending_fragment[0] = '\0';
    load_url(url, WHTTP_GET, 0, 0);
    if (add_history) history_push(address);
}

static void go_history(int delta) {
    if (delta < 0 && hist_pos == 0) return;
    if (delta > 0 && hist_pos + 1u >= hist_count) return;
    hist_pos = (uint32_t)((int32_t)hist_pos + delta);
    navigate(history[hist_pos], 0);
}

/* ------------------------------------------------------------------ internal pages */
static void html_escape_append(char *out, size_t cap, const char *text) {
    for (size_t i = 0; text[i]; ++i) {
        char c = text[i];
        if (c == '<') str_append(out, cap, "&lt;");
        else if (c == '>') str_append(out, cap, "&gt;");
        else if (c == '&') str_append(out, cap, "&amp;");
        else if (c == '"') str_append(out, cap, "&quot;");
        else { char s2[2] = {c, 0}; str_append(out, cap, s2); }
    }
}
static const char PAGE_STYLE[] =
    "<style>body{margin:0;font-family:sans-serif;color:#1d1d1f;background:#f5f5f7}"
    ".wrap{max-width:760px;margin:0 auto;padding:36px 28px}h1{font-size:30px;margin:0 0 6px}"
    "h2{font-size:15px;color:#6e6e73;margin:28px 0 10px;font-weight:bold}"
    ".fav{display:inline-block;width:150px;margin:0 12px 14px 0;padding:12px;background:#fff;border:1px solid #e3e3e8;border-radius:10px;text-decoration:none}"
    ".fav b{display:block;color:#1d1d1f}.fav span{display:block;color:#86868b;font-size:12px}"
    "p{color:#424245;line-height:1.5}a{color:#0066cc;text-decoration:none}"
    ".card{background:#fff;border:1px solid #e3e3e8;border-radius:12px;padding:20px 24px}</style>";

static void start_page(void) {
    static char html[16384];
    copy_text(html, sizeof(html), "<title>Start Page</title>");
    str_append(html, sizeof(html), PAGE_STYLE);
    str_append(html, sizeof(html), "<div class=wrap><h1>Favorites</h1><p>Type an address or a search above. "
               "Press \xE2\x8C\x98" "D on any page to add it here.</p>");
    for (uint32_t i = 0; i < bookmark_count; ++i) {
        wurl u; char host[WHOST_MAX] = "";
        if (wurl_parse(bookmarks[i].url, &u)) copy_text(host, sizeof(host), u.host);
        str_append(html, sizeof(html), "<a class=fav href=\"");
        html_escape_append(html, sizeof(html), bookmarks[i].url);
        str_append(html, sizeof(html), "\"><b>");
        html_escape_append(html, sizeof(html), bookmarks[i].title);
        str_append(html, sizeof(html), "</b><span>");
        html_escape_append(html, sizeof(html), host);
        str_append(html, sizeof(html), "</span></a>");
    }
    str_append(html, sizeof(html), "<h2>About this browser</h2><div class=card><p>MintOS Web draws pages with its own engine: "
               "HTML and CSS layout with images, forms, cookies and downloads, over HTTP/1.1 and MintOS\xE2\x80\x99s own TLS 1.3 and 1.2. "
               "Certificates are checked against the Mozilla list of trusted authorities and cannot be bypassed. "
               "Scripts are never run.</p><p>\xE2\x8C\x98L address field \xC2\xB7 \xE2\x8C\x98R reload \xC2\xB7 \xE2\x8C\x98[ back \xC2\xB7 "
               "\xE2\x8C\x98] forward \xC2\xB7 \xE2\x8C\x98" "D favorite \xC2\xB7 Space and arrow keys scroll \xC2\xB7 Tab moves between fields</p></div></div>");
    page_set_html(html, text_length(html), 0, 0, 1);
    copy_text(page_title, sizeof(page_title), "Start Page");
    address[0] = '\0'; address_len = 0;
    status_text[0] = '\0';
}

static void show_message(const char *title, const char *heading, const char *detail) {
    static char html[4096];
    copy_text(html, sizeof(html), "<title>");
    html_escape_append(html, sizeof(html), title);
    str_append(html, sizeof(html), "</title>");
    str_append(html, sizeof(html), PAGE_STYLE);
    str_append(html, sizeof(html), "<div class=wrap><div class=card><h1 style=\"font-size:22px\">");
    html_escape_append(html, sizeof(html), heading);
    str_append(html, sizeof(html), "</h1><p>");
    html_escape_append(html, sizeof(html), detail ? detail : "");
    str_append(html, sizeof(html), "</p><p><a href=\"about:start\">Start Page</a></p></div></div>");
    wurl keep = P.url;
    page_set_html(html, text_length(html), 0, &keep, 1);
}

/* ------------------------------------------------------------------ forms */
static void form_submit(wnode *form, int32_t submitter) {
    if (!P.have) return;
    const char *action = form ? wnode_attr(form, "action") : 0;
    const char *method = form ? wnode_attr(form, "method") : 0;
    if (submitter >= 0) {
        const char *fa = wnode_attr(P.doc.ctls[submitter].node, "formaction");
        if (fa) action = fa;
    }
    char target[WURL_MAX];
    if (!wurl_resolve(&P.url, action && action[0] ? action : "", target, sizeof(target))) { flash_status("This form cannot be sent", 0); return; }
    size_t cap = 64u * 1024u, len = 0;
    char *body = (char *)wmem_alloc(cap);
    if (!body) return;
    body[0] = '\0';
    for (uint32_t i = 0; i < P.doc.ctl_count; ++i) {
        wctl *c = &P.doc.ctls[i];
        if (c->form != form || !c->name || !c->name[0] || c->disabled) continue;
        switch (c->type) {
        case WCT_CHECKBOX: case WCT_RADIO:
            if (c->checked) len = wform_append(body, cap, len, c->name, c->value && c->value[0] ? c->value : "on");
            break;
        case WCT_SUBMIT: case WCT_BUTTON_EL: case WCT_IMAGE:
            if ((int32_t)i == submitter) {
                if (c->type == WCT_IMAGE) {
                    char nm[160]; copy_text(nm, sizeof(nm), c->name); str_append(nm, sizeof(nm), ".x");
                    len = wform_append(body, cap, len, nm, "1");
                    copy_text(nm, sizeof(nm), c->name); str_append(nm, sizeof(nm), ".y");
                    len = wform_append(body, cap, len, nm, "1");
                } else len = wform_append(body, cap, len, c->name, c->value ? c->value : "");
            }
            break;
        case WCT_RESET: case WCT_BUTTON: case WCT_FILE: case WCT_OTHER: break;
        case WCT_SELECT:
            if (c->selected >= 0 && c->selected < c->nopts) {
                const wnode *o = c->opts[c->selected];
                const char *v = wnode_attr(o, "value");
                char t[512];
                if (!v) { wnode_text(o, t, sizeof(t)); v = t; }
                len = wform_append(body, cap, len, c->name, v);
            }
            break;
        default: len = wform_append(body, cap, len, c->name, c->value ? c->value : ""); break;
        }
    }
    int post = method && wstr_ieq(method, "post");
    if (submitter >= 0) { const char *fm = wnode_attr(P.doc.ctls[submitter].node, "formmethod"); if (fm) post = wstr_ieq(fm, "post"); }
    if (post) {
        load_url(target, WHTTP_POST, body, len);
        history_push(address);
    } else {
        /* GET: the query replaces the action's query; fragments are dropped */
        char url[WURL_MAX];
        copy_text(url, sizeof(url), target);
        size_t q = 0;
        while (url[q] && url[q] != '?' && url[q] != '#') ++q;
        url[q] = '\0';
        str_append(url, sizeof(url), "?");
        str_append(url, sizeof(url), body);
        load_url(url, WHTTP_GET, 0, 0);
        history_push(address);
    }
    wmem_free(body);
}

static void ctl_activate(int32_t i) {
    wctl *c = &P.doc.ctls[i];
    if (c->disabled) return;
    switch (c->type) {
    case WCT_CHECKBOX: c->checked = !c->checked; break;
    case WCT_RADIO:
        for (uint32_t k = 0; k < P.doc.ctl_count; ++k) {
            wctl *o = &P.doc.ctls[k];
            if (o->type == WCT_RADIO && o->form == c->form && o->name && c->name && str_eq(o->name, c->name)) o->checked = 0;
        }
        c->checked = 1;
        break;
    case WCT_SUBMIT: case WCT_IMAGE: form_submit(c->form, i); break;
    case WCT_BUTTON_EL: if (c->form) form_submit(c->form, i); break;
    case WCT_RESET:
        for (uint32_t k = 0; k < P.doc.ctl_count; ++k) {
            wctl *o = &P.doc.ctls[k];
            if (o->form != c->form) continue;
            if (o->type == WCT_TEXT || o->type == WCT_PASSWORD || o->type == WCT_SEARCH || o->type == WCT_TEXTAREA) {
                const char *v = wnode_attr(o->node, "value");
                if (o->type == WCT_TEXTAREA) v = "";
                copy_text(o->value, o->cap, v ? v : "");
                o->len = (uint32_t)text_length(o->value); o->caret = o->len;
            } else if (o->type == WCT_CHECKBOX || o->type == WCT_RADIO) o->checked = wnode_attr(o->node, "checked") != 0;
        }
        break;
    case WCT_SELECT: select_open = select_open == i ? -1 : i; break;
    default:
        if (c->focus_ok) { focus_ctl = i; address_focused = 0; }
        break;
    }
    web_dirty = 1;
}

static void implicit_submit(int32_t from) {
    wctl *c = &P.doc.ctls[from];
    int32_t sub = -1;
    for (uint32_t k = 0; k < P.doc.ctl_count; ++k) {
        wctl *o = &P.doc.ctls[k];
        if (o->form == c->form && (o->type == WCT_SUBMIT || o->type == WCT_IMAGE || o->type == WCT_BUTTON_EL) && !o->disabled) { sub = (int32_t)k; break; }
    }
    form_submit(c->form, sub);
}

static int edit_key(wctl *c, const MINT_DESKTOP_KEY_EVENT *k) {
    uint32_t ch = k->codepoint;
    if (k->scancode == 0x14Bu) { while (c->caret && ((uint8_t)c->value[c->caret - 1] & 0xC0u) == 0x80u) --c->caret; if (c->caret) --c->caret; while (c->caret && ((uint8_t)c->value[c->caret] & 0xC0u) == 0x80u) --c->caret; return 1; }
    if (k->scancode == 0x14Du) { if (c->caret < c->len) { ++c->caret; while (c->caret < c->len && ((uint8_t)c->value[c->caret] & 0xC0u) == 0x80u) ++c->caret; } return 1; }
    if (k->scancode == 0x147u) { c->caret = 0; return 1; }
    if (k->scancode == 0x14Fu) { c->caret = c->len; return 1; }
    if (ch == 8u) {
        if (!c->caret) return 1;
        uint32_t s = c->caret - 1u;
        while (s && ((uint8_t)c->value[s] & 0xC0u) == 0x80u) --s;
        for (uint32_t i = c->caret; i <= c->len; ++i) c->value[s + i - c->caret] = c->value[i];
        c->len -= c->caret - s; c->caret = s;
        return 1;
    }
    if (k->scancode == 0x153u) {
        if (c->caret >= c->len) return 1;
        uint32_t e = c->caret + 1u;
        while (e < c->len && ((uint8_t)c->value[e] & 0xC0u) == 0x80u) ++e;
        for (uint32_t i = e; i <= c->len; ++i) c->value[c->caret + i - e] = c->value[i];
        c->len -= e - c->caret;
        return 1;
    }
    if (ch == '\n' && c->type != WCT_TEXTAREA) return 0;
    if (ch == '\t' || ch == 27u) return 0;
    if (ch < 32u && ch != '\n') return 0;
    if (ch == 127u) return 0;
    const char *ml = wnode_attr(c->node, "maxlength");
    if (ml) { uint32_t m = 0; for (const char *p = ml; *p >= '0' && *p <= '9'; ++p) m = m * 10u + (uint32_t)(*p - '0'); if (m && c->len >= m) return 1; }
    char u[4]; size_t n = wutf8_put(u, ch);
    if (c->len + n + 1u > c->cap) return 1;
    for (uint32_t i = c->len + 1u; i-- > c->caret;) c->value[i + n] = c->value[i];
    for (size_t i = 0; i < n; ++i) c->value[c->caret + i] = u[i];
    c->len += (uint32_t)n; c->caret += (uint32_t)n;
    return 1;
}

static void focus_next(int dir) {
    if (!P.have || !P.doc.ctl_count) return;
    int32_t n = (int32_t)P.doc.ctl_count;
    int32_t i = focus_ctl;
    for (int32_t step = 0; step < n; ++step) {
        i = (i + dir + n) % n;
        wctl *c = &P.doc.ctls[i];
        if (c->focus_ok && c->w > 0) { focus_ctl = i; if (c->y < scroll_y || c->y > scroll_y + 500) scroll_y = c->y > 120 ? c->y - 120 : 0; return; }
    }
}

/* ------------------------------------------------------------------ view geometry */
static gfx_rect address_rect(const mde_layout *l) {
    int32_t x0 = l->toolbar.x0 + 196, x1 = l->toolbar.x1 - 64;
    if (x1 - x0 > 720) { int32_t mid = (x0 + x1) / 2; x0 = mid - 360; x1 = mid + 360; }
    return R(x0, l->toolbar.y0 + 12, x1 - x0, 28);
}
static gfx_rect back_rect(const mde_layout *l) { return R(l->toolbar.x0 + 86, l->toolbar.y0 + 12, 28, 28); }
static gfx_rect forward_rect(const mde_layout *l) { return R(l->toolbar.x0 + 116, l->toolbar.y0 + 12, 28, 28); }
static gfx_rect fav_rect(const mde_layout *l) { return R(l->toolbar.x0 + 152, l->toolbar.y0 + 12, 30, 28); }
static gfx_rect dl_rect(const mde_layout *l) { return R(l->toolbar.x1 - 52, l->toolbar.y0 + 12, 30, 28); }
static gfx_rect panel_rect(const mde_layout *l) {
    if (panel == 1) return R(fav_rect(l).x0, l->content.y0 + 4, 340, 48 + 30 * (int32_t)(bookmark_count ? bookmark_count : 1));
    int32_t n = 0;
    for (uint32_t i = 0; i < WEB_DOWNLOADS; ++i) if (downloads[i].state != DL_FREE) ++n;
    return R(l->toolbar.x1 - 372, l->content.y0 + 4, 360, 44 + 46 * (n ? n : 1));
}

const char *web_title(void) { return page_title[0] ? page_title : "Web"; }

void web_opened(void) {
    if (!heap_ready) {
        wmem_init(heap_mem, sizeof(heap_mem));
        heap_ready = 1;
        for (uint32_t c = 0; c < WEB_CONNS; ++c) { conns[c].handle = -1; conns[c].req = -1; }
        load_cookies();
        load_bookmarks();
    }
    if (!hist_count && !doc_loading() && !P.have) { history_push("about:start"); start_page(); address_focused = 1; address_replace = 1; }
}

/* ------------------------------------------------------------------ painting */
static void blit(const uint32_t *px, int32_t pw, int32_t ph, int32_t x, int32_t y, int32_t w, int32_t h) {
    gfx_rect c = gfx_rect_intersect(gfx_clip_get(), R(x, y, w, h));
    if (gfx_rect_empty(c) || pw <= 0 || ph <= 0) return;
    for (int32_t yy = c.y0; yy < c.y1; ++yy) {
        int32_t sy = (int32_t)((int64_t)(yy - y) * ph / h);
        const uint32_t *src = px + (int64_t)sy * pw;
        uint32_t *dst = S->px + (int64_t)yy * S->stride;
        for (int32_t xx = c.x0; xx < c.x1; ++xx) {
            uint32_t s = src[(int64_t)(xx - x) * pw / w];
            uint32_t a = s >> 24;
            if (a == 255u) dst[xx] = s & 0xFFFFFFu;
            else if (a) {
                uint32_t d = dst[xx], ia = 255u - a;
                uint32_t r = ((s >> 16) & 255u) + (((d >> 16) & 255u) * ia + 127u) / 255u;
                uint32_t g = ((s >> 8) & 255u) + (((d >> 8) & 255u) * ia + 127u) / 255u;
                uint32_t b = (s & 255u) + ((d & 255u) * ia + 127u) / 255u;
                dst[xx] = ((r > 255u ? 255u : r) << 16) | ((g > 255u ? 255u : g) << 8) | (b > 255u ? 255u : b);
            }
        }
    }
}

/* Image at display size: rescales the cached bitmap once when the size changes. */
static void draw_image(int32_t img, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!P.img_slot || img < 0 || (uint32_t)img >= P.doc.img_count || P.img_slot[img] < 0 || w <= 0 || h <= 0) return;
    imgent *e = &images[P.img_slot[img]];
    e->used_at = web_ms();
    if (e->state == IS_READY && e->rgba && (int64_t)w * h <= 1600 * 1200) {
        uint32_t *px = wimage_scale(e->rgba, e->w, e->h, w, h);
        if (px) { wmem_free(e->px); e->px = px; e->pw = w; e->ph = h; wmem_free(e->rgba); e->rgba = 0; }
    }
    if (e->state != IS_READY || !e->px) {
        gfx_fill(S, x, y, w, h, OPAQUE(0xEDEDF0u));
        if (e->state == IS_FAILED && w > 16 && h > 16) gfx_round_rect_stroke(S, x, y, w, h, 3, OPAQUE(0xC8C8CCu));
        return;
    }
    blit(e->px, e->pw, e->ph, x, y, w, h);              /* nearest-neighbour if shown at a second size */
}

static void draw_italic(int font, int32_t x, int32_t baseline, const char *t, uint32_t argb) {
    const MINT_FONT *f = &mint_fonts[font];
    gfx_rect c = gfx_clip_get();
    int32_t pen = x * 64;
    const char *end = t + text_length(t);
    uint32_t a = argb >> 24;
    while (t < end) {
        uint32_t cp = wutf8_next(&t, end);
        if (cp == 0xA0u) cp = ' ';
        const MINT_GLYPH *g = 0;
        if (cp >= 32u && cp < 127u && f->glyph_count > cp - 32u && f->glyphs[cp - 32u].codepoint == cp) g = &f->glyphs[cp - 32u];
        else {
            int32_t lo = 0, hi = (int32_t)f->glyph_count - 1;
            while (lo <= hi) { int32_t mid = (lo + hi) / 2; if (f->glyphs[mid].codepoint == cp) { g = &f->glyphs[mid]; break; } if (f->glyphs[mid].codepoint < cp) lo = mid + 1; else hi = mid - 1; }
            if (!g) g = &f->glyphs['?' - 32];
        }
        int32_t gx = ((pen + 32) >> 6) + g->xoff, gy = baseline + g->yoff;
        const uint8_t *bits = f->bits + g->offset;
        for (int32_t r = 0; r < g->h; ++r) {
            int32_t yy = gy + r;
            if (yy < c.y0 || yy >= c.y1) continue;
            int32_t shear = (baseline - yy) / 4;
            uint32_t *row = S->px + (int64_t)yy * S->stride;
            for (int32_t col = 0; col < g->w; ++col) {
                int32_t xx = gx + col + shear;
                if (xx < c.x0 || xx >= c.x1) continue;
                uint32_t cov = bits[r * g->w + col];
                if (cov) row[xx] = gfx_mix(row[xx], argb, (a * cov + 127u) / 255u);
            }
        }
        pen += g->advance64;
    }
}

static void draw_text_item(const witem *it, int32_t ox, int32_t oy) {
    const char *t = P.lay.pool + it->a;
    int32_t x = ox + it->x, base = oy + it->y + wfont_ascent(it->font);
    uint32_t col = it->color;
    if (!(col >> 24)) return;
    int hovered = it->link >= 0 && it->link == hover_link;
    if (it->flags & WIF_MONO) gfx_text_cells(S, it->font, x, base, t, it->extra ? it->extra : 7, col);
    else if (it->flags & WIF_ITALIC) draw_italic(it->font, x, base, t, col);
    else gfx_text(S, it->font, x, base, t, col);
    if ((it->flags & WIF_UNDERLINE) || hovered) gfx_hline(S, x, base + 2, it->w, col);
    if (it->flags & WIF_STRIKE) gfx_hline(S, x, base - wfont_ascent(it->font) * 3 / 10 - 1, it->w, col);
    if (it->flags & WIF_OVERLINE) gfx_hline(S, x, base - wfont_ascent(it->font) - 1, it->w, col);
}

static void draw_border(const witem *it, int32_t ox, int32_t oy) {
    int32_t x = ox + it->x, y = oy + it->y, w = it->w, h = it->h;
    int32_t bt = (int32_t)(it->a & 255u), br = (int32_t)((it->a >> 8) & 255u), bb = (int32_t)((it->a >> 16) & 255u), bl = (int32_t)(it->a >> 24);
    if (it->font && bt == br && bt == bb && bt == bl && bt == 1) { gfx_round_rect_stroke(S, x, y, w, h, it->font, it->color); return; }
    if (it->extra) {                                   /* dotted / dashed */
        int32_t dash = it->extra == 1 ? 2 : 6;
        for (int32_t k = 0; k < w; k += dash * 2) {
            int32_t len = k + dash > w ? w - k : dash;
            if (bt) gfx_fill(S, x + k, y, len, bt, it->color);
            if (bb) gfx_fill(S, x + k, y + h - bb, len, bb, it->c);
        }
        for (int32_t k = 0; k < h; k += dash * 2) {
            int32_t len = k + dash > h ? h - k : dash;
            if (bl) gfx_fill(S, x, y + k, bl, len, it->b);
            if (br) gfx_fill(S, x + w - br, y + k, br, len, it->color);
        }
        return;
    }
    if (bt) gfx_fill(S, x, y, w, bt, it->color);
    if (bb) gfx_fill(S, x, y + h - bb, w, bb, it->c);
    if (bl) gfx_fill(S, x, y, bl, h, it->b);
    if (br) gfx_fill(S, x + w - br, y, br, h, it->color);
}

static void draw_control(const witem *it, int32_t ox, int32_t oy) {
    if (it->a >= P.doc.ctl_count) return;
    wctl *c = &P.doc.ctls[it->a];
    int32_t x = ox + it->x, y = oy + it->y, w = it->w, h = it->h;
    int focused = focus_ctl == (int32_t)it->a && !address_focused;
    uint32_t border = OPAQUE(0xB8B8BEu), text = OPAQUE(0x1D1D1Fu), muted = OPAQUE(0x8E8E93u);
    switch (c->type) {
    case WCT_CHECKBOX: case WCT_RADIO: {
        int32_t sz = 14, bx = x + (w - sz) / 2, by = y + (h - sz) / 2;
        if (c->type == WCT_RADIO) {
            gfx_circle(S, bx + sz / 2, by + sz / 2, sz / 2, c->checked ? OPAQUE(0x007AFFu) : OPAQUE(0xFFFFFFu));
            gfx_circle_stroke(S, bx + sz / 2, by + sz / 2, sz / 2, c->checked ? OPAQUE(0x0063D1u) : border);
            if (c->checked) gfx_circle(S, bx + sz / 2, by + sz / 2, 3, OPAQUE(0xFFFFFFu));
        } else {
            gfx_round_rect(S, bx, by, sz, sz, 3, c->checked ? OPAQUE(0x007AFFu) : OPAQUE(0xFFFFFFu));
            gfx_round_rect_stroke(S, bx, by, sz, sz, 3, c->checked ? OPAQUE(0x0063D1u) : border);
            if (c->checked) { gfx_line(S, bx + 3, by + 7, bx + 6, by + 10, OPAQUE(0xFFFFFFu)); gfx_line(S, bx + 6, by + 10, bx + 11, by + 4, OPAQUE(0xFFFFFFu));
                              gfx_line(S, bx + 3, by + 8, bx + 6, by + 11, OPAQUE(0xFFFFFFu)); gfx_line(S, bx + 6, by + 11, bx + 11, by + 5, OPAQUE(0xFFFFFFu)); }
        }
        return;
    }
    case WCT_SUBMIT: case WCT_RESET: case WCT_BUTTON: case WCT_IMAGE: case WCT_BUTTON_EL: case WCT_FILE: {
        gfx_round_rect(S, x, y, w, h, 6, OPAQUE(0xFFFFFFu));
        gfx_round_rect_stroke(S, x, y, w, h, 6, border);
        const char *lab = c->type == WCT_FILE ? "Choose File\xE2\x80\xA6" : c->label ? c->label : "";
        int32_t tw = gfx_text_width(FONT_BODY, lab);
        gfx_text_fit(S, FONT_BODY, x + (w - tw > 8 ? (w - tw) / 2 : 6), y + h / 2 + 5, lab, w - 10, c->disabled || c->type == WCT_FILE ? muted : text);
        return;
    }
    case WCT_SELECT: {
        gfx_round_rect(S, x, y, w, h, 6, OPAQUE(0xFFFFFFu));
        gfx_round_rect_stroke(S, x, y, w, h, 6, focused || select_open == (int32_t)it->a ? OPAQUE(0x007AFFu) : border);
        char t[200] = "";
        if (c->selected >= 0 && c->selected < c->nopts) wnode_text(c->opts[c->selected], t, sizeof(t));
        gfx_text_fit(S, FONT_BODY, x + 8, y + h / 2 + 5, t, w - 30, text);
        int32_t ax = x + w - 16, ay = y + h / 2;
        gfx_line(S, ax - 3, ay - 2, ax, ay - 5, text); gfx_line(S, ax, ay - 5, ax + 3, ay - 2, text);
        gfx_line(S, ax - 3, ay + 2, ax, ay + 5, text); gfx_line(S, ax, ay + 5, ax + 3, ay + 2, text);
        return;
    }
    default: break;
    }
    /* text fields and text areas */
    gfx_fill(S, x, y, w, h, OPAQUE(0xFFFFFFu));
    if (focused) gfx_round_rect_stroke(S, x - 2, y - 2, w + 4, h + 4, 5, OPAQUE(0x7FB5FFu));
    gfx_round_rect_stroke(S, x, y, w, h, c->type == WCT_SEARCH ? h / 2 : 3, focused ? OPAQUE(0x007AFFu) : border);
    gfx_rect old = gfx_clip_get();
    gfx_clip_set(gfx_rect_intersect(old, R(x + 2, y + 1, w - 4, h - 2)));
    int32_t tx = x + (c->type == WCT_SEARCH ? 10 : 6);
    if (!c->len && c->placeholder && !focused) gfx_text(S, FONT_BODY, tx, y + 16, c->placeholder, muted);
    else if (c->type == WCT_TEXTAREA) {
        int32_t ly = y + 16;
        const char *p = c->value;
        uint32_t off = 0;
        while (*p || off == 0) {
            char line[256]; size_t n = 0;
            const char *ls = p;
            while (*p && *p != '\n' && n + 1 < sizeof(line)) line[n++] = *p++;
            line[n] = '\0';
            gfx_text(S, FONT_BODY, tx, ly, line, text);
            if (focused && c->caret >= (uint32_t)(ls - c->value) && c->caret <= (uint32_t)(p - c->value) && (web_ms() / 530u) % 2u == 0u) {
                char pre[256]; size_t pn = c->caret - (uint32_t)(ls - c->value); if (pn > n) pn = n;
                for (size_t k = 0; k < pn; ++k) pre[k] = line[k];
                pre[pn] = '\0';
                gfx_fill(S, tx + gfx_text_width(FONT_BODY, pre), ly - 12, 1, 15, OPAQUE(0x007AFFu));
            }
            if (*p == '\n') ++p; else if (!*p) break;
            ly += 17; ++off;
        }
    } else {
        char shown[600]; size_t n = 0;
        if (c->type == WCT_PASSWORD) { for (uint32_t k = 0; k < c->len && n + 3 < sizeof(shown); ++k) if (((uint8_t)c->value[k] & 0xC0u) != 0x80u) { shown[n++] = (char)0xE2; shown[n++] = (char)0x80; shown[n++] = (char)0xA2; } shown[n] = '\0'; }
        else copy_text(shown, sizeof(shown), c->value ? c->value : "");
        char pre[600];
        size_t pc = c->type == WCT_PASSWORD ? 0 : c->caret;
        if (c->type == WCT_PASSWORD) { for (uint32_t k = 0; k < c->caret; ++k) if (((uint8_t)c->value[k] & 0xC0u) != 0x80u) pc += 3; }
        if (pc > text_length(shown)) pc = text_length(shown);
        for (size_t k = 0; k < pc && k + 1 < sizeof(pre); ++k) pre[k] = shown[k];
        pre[pc < sizeof(pre) ? pc : sizeof(pre) - 1] = '\0';
        int32_t cx = gfx_text_width(FONT_BODY, pre);
        int32_t scroll = cx > w - 16 ? cx - (w - 16) : 0;
        gfx_text(S, FONT_BODY, tx - scroll, y + h / 2 + 5, shown, text);
        if (focused && (web_ms() / 530u) % 2u == 0u) gfx_fill(S, tx - scroll + cx, y + 4, 1, h - 8, OPAQUE(0x007AFFu));
    }
    gfx_clip_set(old);
}

static void paint_select_list(gfx_rect c) {
    if (select_open < 0 || (uint32_t)select_open >= P.doc.ctl_count) return;
    wctl *k = &P.doc.ctls[select_open];
    int32_t rows = k->nopts > 14 ? 14 : k->nopts;
    int32_t x = c.x0 + k->x, y = c.y0 + k->y + k->h - scroll_y + 2, w = k->w < 160 ? 160 : k->w, h = rows * 22 + 8;
    if (y + h > c.y1) y = c.y0 + k->y - scroll_y - h - 2;
    gfx_shadow(S, x, y, w, h, 8, 12, 4, ARGB(0x40, 0, 0, 0));
    gfx_round_rect(S, x, y, w, h, 8, OPAQUE(0xFFFFFFu));
    gfx_round_rect_stroke(S, x, y, w, h, 8, ARGB(0x30, 0, 0, 0));
    int32_t first = k->selected > rows - 1 ? k->selected - rows + 1 : 0;
    for (int32_t i = 0; i < rows; ++i) {
        int32_t oi = first + i;
        if (oi >= k->nopts) break;
        char t[200]; wnode_text(k->opts[oi], t, sizeof(t));
        if (oi == k->selected) gfx_round_rect(S, x + 4, y + 4 + i * 22, w - 8, 22, 5, OPAQUE(T.accent));
        gfx_text_fit(S, FONT_BODY, x + 12, y + 20 + i * 22, t, w - 20, OPAQUE(oi == k->selected ? 0xFFFFFFu : 0x1D1D1Fu));
    }
}
static int select_list_click(gfx_rect c, int32_t px, int32_t py) {
    wctl *k = &P.doc.ctls[select_open];
    int32_t rows = k->nopts > 14 ? 14 : k->nopts;
    int32_t x = c.x0 + k->x, y = c.y0 + k->y + k->h - scroll_y + 2, w = k->w < 160 ? 160 : k->w, h = rows * 22 + 8;
    if (y + h > c.y1) y = c.y0 + k->y - scroll_y - h - 2;
    int32_t first = k->selected > rows - 1 ? k->selected - rows + 1 : 0;
    if (px >= x && px < x + w && py >= y + 4 && py < y + 4 + rows * 22) {
        int32_t oi = first + (py - y - 4) / 22;
        if (oi < k->nopts) k->selected = oi;
    }
    select_open = -1;
    return 1;
}

static void paint_panel(const mde_layout *l) {
    if (!panel) return;
    gfx_rect r = panel_rect(l);
    gfx_shadow(S, r.x0, r.y0, W(r), H(r), 12, 18, 6, ARGB(0x45, 0, 0, 0));
    gfx_round_rect(S, r.x0, r.y0, W(r), H(r), 12, OPAQUE(T.dark ? 0x2C2C2Eu : 0xFFFFFFu));
    gfx_round_rect_stroke(S, r.x0, r.y0, W(r), H(r), 12, T.dark ? ARGB(0x40, 0xFF, 0xFF, 0xFF) : ARGB(0x26, 0, 0, 0));
    if (panel == 1) {
        ui_label(r.x0 + 16, r.y0 + 26, FONT_BODY_BOLD, "Favorites", T.text);
        int marked = !P.internal && address[0] && bookmark_index(address) >= 0;
        ui_label_right(r.x1 - 16, r.y0 + 26, FONT_BODY, marked ? "Remove This Page" : "Add This Page  \xE2\x8C\x98" "D", OPAQUE(T.accent));
        if (!bookmark_count) ui_label(r.x0 + 16, r.y0 + 62, FONT_BODY, "No favorites yet.", T.text3);
        for (uint32_t i = 0; i < bookmark_count; ++i) {
            int32_t y = r.y0 + 44 + (int32_t)i * 30;
            gfx_circle(S, r.x0 + 26, y + 14, 10, OPAQUE(T.accent));
            char letter[2] = {bookmarks[i].title[0] ? bookmarks[i].title[0] : '?', 0};
            if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] = (char)(letter[0] - 32);
            ui_label_center(r.x0 + 26, y + 19, FONT_CAPTION_BOLD, letter, 0xFFFFFFu);
            gfx_text_fit(S, FONT_BODY, r.x0 + 44, y + 19, bookmarks[i].title, W(r) - 60, OPAQUE(T.text));
        }
        return;
    }
    ui_label(r.x0 + 16, r.y0 + 26, FONT_BODY_BOLD, "Downloads", T.text);
    ui_label_right(r.x1 - 16, r.y0 + 26, FONT_BODY, "Show in Files", OPAQUE(T.accent));
    int32_t row = 0;
    for (uint32_t i = 0; i < WEB_DOWNLOADS; ++i) {
        dl *d = &downloads[i];
        if (d->state == DL_FREE) continue;
        int32_t y = r.y0 + 40 + row * 46;
        ui_symbol_glyph(r.x0 + 26, y + 18, SYM_DOC, OPAQUE(T.text2));
        gfx_text_fit(S, FONT_BODY, r.x0 + 46, y + 16, d->name, W(r) - 62, OPAQUE(T.text));
        char line[120], a[24], b[24];
        format_bytes(a, sizeof(a), d->got);
        if (d->state == DL_ACTIVE) {
            copy_text(line, sizeof(line), a);
            if (d->total) { format_bytes(b, sizeof(b), d->total); str_append(line, sizeof(line), " of "); str_append(line, sizeof(line), b); }
            uint32_t pm = d->total ? (uint32_t)(d->got * 1000u / d->total) : (uint32_t)((web_ms() / 4u) % 1000u);
            ui_progress(R(r.x0 + 46, y + 22, W(r) - 62, 4), pm > 1000u ? 1000u : pm, OPAQUE(T.accent));
            ui_label(r.x0 + 46, y + 40, FONT_CAPTION, line, T.text2);
        } else if (d->state == DL_DONE) {
            copy_text(line, sizeof(line), a); str_append(line, sizeof(line), " \xE2\x80\x94 saved in Downloads");
            ui_label(r.x0 + 46, y + 34, FONT_CAPTION, line, T.text2);
        } else {
            copy_text(line, sizeof(line), "Failed: "); str_append(line, sizeof(line), d->error);
            ui_label(r.x0 + 46, y + 34, FONT_CAPTION, line, 0xD70015u);
        }
        ++row;
    }
    if (!row) ui_label(r.x0 + 16, r.y0 + 62, FONT_BODY, "No downloads.", T.text3);
}

void web_paint(const mde_layout *l) {
    /* toolbar */
    ui_symbol_glyph(back_rect(l).x0 + 14, back_rect(l).y0 + 14, SYM_CHEVRON_LEFT, OPAQUE(hist_pos > 0 ? T.text2 : T.text3));
    ui_symbol_glyph(forward_rect(l).x0 + 14, forward_rect(l).y0 + 14, SYM_CHEVRON_RIGHT, OPAQUE(hist_pos + 1u < hist_count ? T.text2 : T.text3));
    gfx_rect fr = fav_rect(l);
    if (panel == 1) gfx_round_rect(S, fr.x0, fr.y0, W(fr), H(fr), 6, T.dark ? ARGB(0x30, 0xFF, 0xFF, 0xFF) : ARGB(0x18, 0, 0, 0));
    {   /* a small open book */
        uint32_t c = OPAQUE(T.text2);
        int32_t cx = fr.x0 + 15, cy = fr.y0 + 14;
        gfx_round_rect_stroke(S, cx - 9, cy - 6, 9, 13, 2, c);
        gfx_round_rect_stroke(S, cx, cy - 6, 9, 13, 2, c);
    }
    int any_dl = 0;
    for (uint32_t i = 0; i < WEB_DOWNLOADS; ++i) if (downloads[i].state != DL_FREE) any_dl = 1;
    if (any_dl) {
        gfx_rect dr = dl_rect(l);
        if (panel == 2) gfx_round_rect(S, dr.x0, dr.y0, W(dr), H(dr), 6, T.dark ? ARGB(0x30, 0xFF, 0xFF, 0xFF) : ARGB(0x18, 0, 0, 0));
        ui_symbol_glyph(dr.x0 + 15, dr.y0 + 14, SYM_DOWNLOAD, OPAQUE(T.text2));
    }
    gfx_rect ar = address_rect(l);
    if (address_focused) ui_text_field(ar, address, "Search or enter website address", 1);
    else {
        gfx_round_rect(S, ar.x0, ar.y0, W(ar), H(ar), 7, OPAQUE(T.dark ? 0x3A3A3Cu : 0xE9E9EBu));
        char shown_buf[WURL_MAX];
        const char *shown = address[0] ? address : "Search or enter website address";
        if (address[0] && !P.internal && !doc_loading()) {
            const char *h = address;
            if (wstr_iprefix(h, "https://")) h += 8; else if (wstr_iprefix(h, "http://")) h += 7;
            size_t n = 0;
            while (h[n] && h[n] != '/' && n + 1u < sizeof(shown_buf)) { shown_buf[n] = h[n]; ++n; }
            shown_buf[n] = '\0';
            shown = shown_buf;
        }
        int lock = P.secure && !P.internal && !doc_loading();
        int32_t tw = gfx_text_width(FONT_BODY, shown) + (lock ? 14 : 0);
        if (tw > W(ar) - 40) tw = W(ar) - 40;
        int32_t tx = ar.x0 + W(ar) / 2 - tw / 2;
        if (lock) {
            uint32_t lc = OPAQUE(T.text2);
            gfx_round_rect_stroke(S, tx + 1, ar.y0 + 7, 7, 8, 3, lc);
            gfx_round_rect(S, tx, ar.y0 + 11, 9, 7, 2, lc);
            tx += 14;
        }
        gfx_text_fit(S, FONT_BODY, tx, ar.y0 + 19, shown, W(ar) - 40, OPAQUE(address[0] ? T.text : T.text3));
    }
    if (loading()) {
        uint32_t phase = (uint32_t)(web_ms() / 30u) % 100u;
        int32_t bw = W(ar) / 4, bx = ar.x0 + (int32_t)((uint32_t)(W(ar) - bw) * phase / 100u);
        gfx_fill(S, bx, ar.y1 - 2, bw, 2, OPAQUE(T.accent));
    }
    /* page */
    gfx_rect c = l->content;
    int32_t vw = W(c) - 12;                                         /* room for the scroll indicator */
    if (P.have && P.need_style && (!P.sheets_pending || web_ms() > P.css_deadline || P.internal)) style_page(vw);
    if (P.have && P.css && (P.need_layout || P.lay_width != vw)) {
        if (P.lay_width && P.lay_width != vw && P.css) {
            /* width changed: media queries may change too */
            style_page(vw);
        }
        layout_page(vw);
    }
    uint32_t bg = P.have && P.laid_out ? P.lay.page_bg : 0xFFFFFFFFu;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(bg & 0xFFFFFFu));
    int32_t doc_h = P.laid_out ? P.lay.height : 0;
    int32_t max_scroll = doc_h - H(c);
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_y > max_scroll) scroll_y = max_scroll;
    if (scroll_y < 0) scroll_y = 0;
    gfx_rect old = gfx_clip_get();
    gfx_clip_set(gfx_rect_intersect(old, c));
    if (P.have && P.laid_out) {
        int32_t ox = c.x0, oy = c.y0 - scroll_y;
        int32_t top = scroll_y - 40, bottom = scroll_y + H(c) + 40;
        for (uint32_t i = 0; i < P.lay.count; ++i) {
            const witem *it = &P.lay.items[i];
            if (!it->kind || it->y > bottom || it->y + it->h < top) continue;
            switch (it->kind) {
            case WI_RECT:
                if (it->a) gfx_round_rect(S, ox + it->x, oy + it->y, it->w, it->h, (int32_t)it->a > it->h / 2 ? it->h / 2 : (int32_t)it->a, it->color);
                else gfx_fill(S, ox + it->x, oy + it->y, it->w, it->h, it->color);
                break;
            case WI_TEXT: draw_text_item(it, ox, oy); break;
            case WI_IMAGE: draw_image((int32_t)it->a, ox + it->x, oy + it->y, it->w, it->h); break;
            case WI_CTL: draw_control(it, ox, oy); break;
            case WI_BORDER: draw_border(it, ox, oy); break;
            case WI_BULLET:
                if (it->extra == WBUL_SQUARE) gfx_fill(S, ox + it->x, oy + it->y, it->w, it->h, it->color);
                else if (it->extra == WBUL_CIRCLE) gfx_circle_stroke(S, ox + it->x + it->w / 2, oy + it->y + it->h / 2, it->w / 2 + 1, it->color);
                else gfx_circle(S, ox + it->x + it->w / 2, oy + it->y + it->h / 2, it->w / 2 + 1, it->color);
                break;
            default: break;
            }
        }
        paint_select_list(c);
    } else if (doc_loading()) {
        ui_label_center(c.x0 + W(c) / 2, c.y0 + H(c) / 3, FONT_BODY, "Loading\xE2\x80\xA6", 0x8E8E93u);
    }
    /* scroll indicator */
    if (doc_h > H(c)) {
        int32_t track = H(c) - 8, thumb = (int32_t)((int64_t)track * H(c) / doc_h);
        if (thumb < 24) thumb = 24;
        int32_t ty = c.y0 + 4 + (int32_t)((int64_t)(track - thumb) * scroll_y / (max_scroll ? max_scroll : 1));
        gfx_round_rect(S, c.x1 - 9, ty, 5, thumb, 2, ARGB(0x55, 0, 0, 0));
    }
    /* status pill */
    const char *pill = 0;
    char hover_text[WURL_MAX];
    if (hover_link >= 0 && P.have && (uint32_t)hover_link < P.doc.link_count) { wurl_resolve(&P.url, P.doc.links[hover_link], hover_text, sizeof(hover_text)); pill = hover_text; }
    else if (status_text[0] && (loading() || !P.internal || status_until)) pill = status_text;
    if (pill) {
        int32_t pw = gfx_text_width(FONT_CAPTION, pill) + 16;
        if (pw > W(c) - 20) pw = W(c) - 20;
        gfx_round_rect(S, c.x0 + 6, c.y1 - 26, pw, 20, 6, OPAQUE(0xF2F2F4u));
        gfx_round_rect_stroke(S, c.x0 + 6, c.y1 - 26, pw, 20, 6, ARGB(0x30, 0, 0, 0));
        gfx_text_fit(S, FONT_CAPTION, c.x0 + 14, c.y1 - 12, pill, pw - 16, OPAQUE(0x48484Au));
    }
    gfx_clip_set(old);
    paint_panel(l);
}

/* ------------------------------------------------------------------ input */
static int32_t link_at(const mde_layout *l, int32_t x, int32_t y) {
    gfx_rect c = l->content;
    if (!P.laid_out || !gfx_rect_contains(c, x, y)) return -1;
    int32_t px = x - c.x0, py = y - c.y0 + scroll_y;
    for (uint32_t i = P.lay.count; i-- > 0;) {
        const witem *it = &P.lay.items[i];
        if (it->link < 0 || !it->kind) continue;
        if (px >= it->x - 1 && px < it->x + it->w + 2 && py >= it->y - 1 && py < it->y + it->h + 1) return it->link;
    }
    return -1;
}
static int32_t ctl_at(const mde_layout *l, int32_t x, int32_t y) {
    gfx_rect c = l->content;
    if (!P.laid_out || !gfx_rect_contains(c, x, y)) return -1;
    int32_t px = x - c.x0, py = y - c.y0 + scroll_y;
    for (uint32_t i = 0; i < P.doc.ctl_count; ++i) {
        const wctl *k = &P.doc.ctls[i];
        if (k->w > 0 && px >= k->x && px < k->x + k->w && py >= k->y && py < k->y + k->h) return (int32_t)i;
    }
    /* <label> around a checkbox or radio */
    return -1;
}

int web_command(uint32_t cmd) {
    if (cmd == 1) { toggle_bookmark(); return 1; }
    if (cmd == 2) { panel = panel == 2 ? 0 : 2; return 1; }
    if (cmd == 3) { panel = panel == 1 ? 0 : 1; return 1; }
    return 0;
}

int web_click(const mde_layout *l, int32_t x, int32_t y) {
    if (panel) {
        gfx_rect r = panel_rect(l);
        if (gfx_rect_contains(r, x, y)) {
            if (panel == 1) {
                if (y < r.y0 + 40 && x > r.x0 + W(r) / 2) { toggle_bookmark(); return 1; }
                int32_t i = (y - r.y0 - 44) / 30;
                if (y >= r.y0 + 44 && i >= 0 && (uint32_t)i < bookmark_count) { char u[WURL_MAX]; copy_text(u, sizeof(u), bookmarks[i].url); panel = 0; navigate(u, 1); }
            } else if (y < r.y0 + 40 && x > r.x0 + W(r) / 2) { panel = 0; shell_open_app(APP_FILES); }
            return 1;
        }
        panel = 0;
        if (!gfx_rect_contains(fav_rect(l), x, y) && !gfx_rect_contains(dl_rect(l), x, y)) return 1;
    }
    if (gfx_rect_contains(back_rect(l), x, y)) { go_history(-1); return 1; }
    if (gfx_rect_contains(forward_rect(l), x, y)) { go_history(1); return 1; }
    if (gfx_rect_contains(fav_rect(l), x, y)) { panel = panel == 1 ? 0 : 1; return 1; }
    if (gfx_rect_contains(dl_rect(l), x, y)) { panel = panel == 2 ? 0 : 2; return 1; }
    if (gfx_rect_contains(address_rect(l), x, y)) {
        if (!address_focused) { address_focused = 1; address_replace = 1; address_len = text_length(address); }
        return 1;
    }
    int was = address_focused || focus_ctl >= 0 || select_open >= 0;
    address_focused = 0;
    if (select_open >= 0) return select_list_click(l->content, x, y);
    int32_t ci = ctl_at(l, x, y);
    if (ci >= 0) {
        focus_ctl = P.doc.ctls[ci].focus_ok ? ci : -1;
        wctl *c = &P.doc.ctls[ci];
        if (c->focus_ok) {
            /* caret from the click position */
            int32_t px = x - l->content.x0 - c->x - 6;
            uint32_t best = c->len;
            if (c->type != WCT_TEXTAREA && c->type != WCT_PASSWORD) {
                for (uint32_t k = 0; k <= c->len; ++k) {
                    if (k < c->len && ((uint8_t)c->value[k] & 0xC0u) == 0x80u) continue;
                    char pre[600]; uint32_t n = k < sizeof(pre) - 1 ? k : (uint32_t)sizeof(pre) - 1;
                    for (uint32_t q = 0; q < n; ++q) pre[q] = c->value[q];
                    pre[n] = '\0';
                    if (gfx_text_width(FONT_BODY, pre) >= px) { best = k; break; }
                }
            }
            c->caret = best;
        }
        ctl_activate(ci);
        return 1;
    }
    focus_ctl = -1;
    int32_t link = link_at(l, x, y);
    if (link >= 0 && (uint32_t)link < P.doc.link_count) {
        char target[WURL_MAX];
        const char *href = P.doc.links[link];
        if (href[0] == '#') {
            wnode *n = wdoc_find_id(&P.doc, href + 1);
            if (n) { scroll_y = n->y > 8 ? n->y - 8 : 0; return 1; }
        }
        if (P.internal && wstr_iprefix(href, "about:")) { navigate(href, 1); return 1; }
        wurl_resolve(P.internal && !P.url.host[0] ? 0 : &P.url, href, target, sizeof(target));
        navigate(target, 1);
        return 1;
    }
    return was;
}

int web_scroll(const mde_layout *l, int32_t delta) {
    (void)l;
    if (select_open >= 0) { select_open = -1; }
    scroll_y += delta * 48;
    if (scroll_y < 0) scroll_y = 0;
    return 1;
}

int web_key(const mde_layout *l, const MINT_DESKTOP_KEY_EVENT *k) {
    uint32_t ch = k->codepoint;
    int cmd = (k->modifiers & (MINT_KEYMOD_GUI | MINT_KEYMOD_CTRL)) != 0;
    if (cmd) {
        if (ch == 'l' || ch == 'L') { address_focused = 1; address_replace = 1; address_len = text_length(address); focus_ctl = -1; return 1; }
        if (ch == 'r' || ch == 'R') { if (!P.internal && address[0]) { char a[WURL_MAX]; copy_text(a, sizeof(a), address); navigate(a, 0); } return 1; }
        if (ch == '[') { go_history(-1); return 1; }
        if (ch == ']') { go_history(1); return 1; }
        if (ch == 'd' || ch == 'D') { toggle_bookmark(); return 1; }
        if ((ch == 'j' || ch == 'J') && (k->modifiers & MINT_KEYMOD_SHIFT)) { panel = panel == 2 ? 0 : 2; return 1; }
        if (ch == 'a' || ch == 'A') { if (focus_ctl >= 0) { P.doc.ctls[focus_ctl].caret = P.doc.ctls[focus_ctl].len; return 1; } }
        return 0;
    }
    if (panel && ch == 27u) { panel = 0; return 1; }
    if (select_open >= 0) {
        wctl *c = &P.doc.ctls[select_open];
        if (k->scancode == 0x150u && c->selected + 1 < c->nopts) ++c->selected;
        else if (k->scancode == 0x148u && c->selected > 0) --c->selected;
        else if (ch == '\n' || ch == 27u || ch == ' ') select_open = -1;
        return 1;
    }
    if (address_focused) {
        if (ch == '\n') { char text[WURL_MAX]; copy_text(text, sizeof(text), address); navigate(text, 1); return 1; }
        if (ch == 27u) { address_focused = 0; return 1; }
        if (ch == 8u) {
            if (address_replace) { address_len = 0; address[0] = '\0'; address_replace = 0; }
            else if (address_len) { --address_len; while (address_len && ((uint8_t)address[address_len] & 0xC0u) == 0x80u) --address_len; address[address_len] = '\0'; }
            return 1;
        }
        if (ch >= 32u && ch != 127u) {
            if (address_replace) { address_len = 0; address[0] = '\0'; address_replace = 0; }
            char u[4]; size_t n = wutf8_put(u, ch);
            if (address_len + n + 1u < sizeof(address)) { for (size_t i = 0; i < n; ++i) address[address_len++] = u[i]; address[address_len] = '\0'; }
            return 1;
        }
        return 0;
    }
    if (ch == '\t') { focus_next((k->modifiers & MINT_KEYMOD_SHIFT) ? -1 : 1); return 1; }
    if (focus_ctl >= 0 && (uint32_t)focus_ctl < P.doc.ctl_count) {
        wctl *c = &P.doc.ctls[focus_ctl];
        if (ch == 27u) { focus_ctl = -1; return 1; }
        if (ch == '\n' && c->type != WCT_TEXTAREA) { implicit_submit(focus_ctl); return 1; }
        if (edit_key(c, k)) { caret_blink = web_ms(); return 1; }
    }
    int32_t page = H(l->content) - 60;
    switch (k->scancode) {
    case 0x148u: scroll_y -= 40; return 1;
    case 0x150u: scroll_y += 40; return 1;
    case 0x149u: scroll_y -= page; return 1;
    case 0x151u: scroll_y += page; return 1;
    case 0x147u: scroll_y = 0; return 1;
    case 0x14Fu: scroll_y = P.lay.height; return 1;
    default: break;
    }
    if (ch == ' ') { scroll_y += (k->modifiers & MINT_KEYMOD_SHIFT) ? -page : page; return 1; }
    if (ch == 8u) { go_history(-1); return 1; }
    if (ch == 27u && loading()) { stop_all(1); flash_status("Stopped", 0); return 1; }
    return 0;
}

uint32_t web_cursor(const mde_layout *l, int32_t x, int32_t y) {
    int32_t link = panel ? -1 : link_at(l, x, y);
    if (link != hover_link) { hover_link = link; hover_dirty = 1; }
    if (gfx_rect_contains(address_rect(l), x, y)) return MINT_CURSOR_SHAPE_IBEAM;
    int32_t ci = panel ? -1 : ctl_at(l, x, y);
    if (ci >= 0 && P.doc.ctls[ci].focus_ok) return MINT_CURSOR_SHAPE_IBEAM;
    return MINT_CURSOR_SHAPE_ARROW;
}

/* ------------------------------------------------------------------ background work */
int web_background(void) {
    int dirty = web_dirty || hover_dirty;
    web_dirty = 0; hover_dirty = 0;
    if (!heap_ready) return dirty;
    uint64_t now = web_ms();
    if (status_until && now > status_until) { status_until = 0; status_text[0] = '\0'; dirty = 1; }
    assign_requests();
    for (uint32_t c = 0; c < WEB_CONNS; ++c) if (pump_conn((int)c, now)) dirty = 1;
    assign_requests();
    if (decode_one()) dirty = 1;
    if (P.have && P.need_style && P.sheets_pending && now > P.css_deadline) { P.sheets_pending = 0; dirty = 1; }
    if (P.have && (P.need_style || P.need_layout)) dirty = 1;
    if (P.refresh_at && now > P.refresh_at) { char u[WURL_MAX]; copy_text(u, sizeof(u), P.refresh_url); P.refresh_at = 0; navigate(u, 0); dirty = 1; }
    if (pending_fragment[0] && P.have && !doc_loading()) { copy_text(P.fragment, sizeof(P.fragment), pending_fragment); pending_fragment[0] = '\0'; P.need_layout = 1; }
    if (loading()) dirty = 1;                                   /* progress bar animation */
    for (uint32_t i = 0; i < WEB_DOWNLOADS; ++i) if (downloads[i].state == DL_ACTIVE) dirty = 1;
    if (focus_ctl >= 0) { static uint64_t last_blink; if (now / 530u != last_blink) { last_blink = now / 530u; dirty = 1; } }
    if (dirty && !loading()) save_cookies();
    return dirty;
}
