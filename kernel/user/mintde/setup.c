/* Setup Assistant and Users & Groups.
 *
 * On the first boot with MintOS HD and no account (or with no MintOS disk but
 * a blank disk that could become one) MintDE shows a full-screen, macOS-style
 * Setup Assistant instead of the desktop:
 *   Welcome     "Hello" in several languages over the blurred wallpaper
 *   MintOS HD   only without a MintOS disk: initialize a blank disk with the
 *               same typed "FORMAT sdX" confirmation as Disk Utility, or
 *               continue for this session only
 *   Account     full name + account name (suggested, validated); creates
 *               /Users/<account>, moving the default /Users/mintos into it
 *   Look        Light / Dark / Auto and the accent colour, previewed live
 *   Done
 * System Settings > Users & Groups shows the account and can run the
 * assistant again (to rename the account or change the look).
 * This file also restores the saved appearance when MintDE starts. */
#include "mintde.h"

#define S (&gfx_screen)
enum { PAGE_WELCOME = 0, PAGE_DISK, PAGE_ACCOUNT, PAGE_LOOK, PAGE_DONE, PAGE_COUNT };

static int setup_on, rerun, bg_valid;
static uint32_t page;
static int with_disk_page;
static char full_name[ACCOUNT_FULL_NAME_MAX];
static char acct[MINT_ACCOUNT_NAME_MAX + 1];
static size_t full_len, acct_len;
static int focus_field, acct_edited, show_errors;
static char error_text[112];
/* disk page */
static MINT_STORAGE_SNAPSHOT snap;
static uint32_t blank[8], blank_count, disk_pick;
static char confirm[MINT_STORAGE_FORMAT_CONFIRM_LEN];
static size_t confirm_len;
static int disk_done, disk_skipped;
/* hello animation */
static const char *const hello[] = {"Hello", "Ahoj", "Hallo", "Bonjour", "Hola", "Ciao", "Hej", "Salut", "Aloha", "Merhaba"};
#define HELLO_COUNT (sizeof(hello) / sizeof(hello[0]))
#define HELLO_PERIOD 2600u
#define HELLO_FADE 450u
static uint64_t hello_start;
static uint32_t hello_shown = 0xFFFFFFFFu;

static gfx_rect R(int32_t x, int32_t y, int32_t w, int32_t h) { return gfx_rect_xywh(x, y, w, h); }
static int32_t W(gfx_rect r) { return r.x1 - r.x0; }
static int32_t H(gfx_rect r) { return r.y1 - r.y0; }
static int in(gfx_rect r, int32_t x, int32_t y) { return gfx_rect_contains(r, x, y); }
static int32_t SW(void) { return gfx_screen.w; }
static int32_t SH(void) { return gfx_screen.h; }
static uint64_t now_ms(void) {
    mint_timespec64 t = {0, 0};
    (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static void damage_all(void) { shell_damage(R(0, 0, SW(), SH())); }
static int volume_ready(MINT_MFS_VOLUME *v) { return mint_fs_get_volume(v) == 0 && v->state == MINT_MFS_VOLUME_READY; }

/* ------------------------------------------------------------ appearance restore/store */
void userdata_start(void) {
    prefs_load();
    (void)account_load();
    MINT_DESKTOP_SETTINGS s = desktop_settings;
    s.version = MINT_DESKTOP_SETTINGS_VERSION;
    int64_t v;
    if ((v = prefs_get_int("desktop.theme", -1)) >= 0 && v <= MINT_THEME_WARM) s.theme = (uint32_t)v;
    if ((v = prefs_get_int("desktop.accent", -1)) >= 0 && v <= MINT_ACCENT_ROSE) s.accent = (uint32_t)v;
    if ((v = prefs_get_int("desktop.ui_scale", -1)) >= 1 && v <= 3) s.ui_scale = (uint32_t)v;
    if ((v = prefs_get_int("desktop.wallpaper", -1)) >= 0 && v <= 255) s.wallpaper = (uint32_t)v;
    if ((v = prefs_get_int("desktop.reduce_motion", -1)) >= 0)
        s.flags = v ? (s.flags | MINT_DESKTOP_SETTING_REDUCE_MOTION) : (s.flags & ~MINT_DESKTOP_SETTING_REDUCE_MOTION);
    if (mint_desktop_settings_set(&s) == 0 || (s.wallpaper = desktop_settings.wallpaper, mint_desktop_settings_set(&s) == 0))
        (void)mint_desktop_settings_get(&desktop_settings);
    static const char line[] = "[MintDE] preferences loaded\n";
    (void)mint_write(1, line, sizeof(line) - 1u);
}
void userdata_store_appearance(void) {
    (void)prefs_set_int("desktop.theme", desktop_settings.theme);
    (void)prefs_set_int("desktop.accent", desktop_settings.accent);
    (void)prefs_set_int("desktop.ui_scale", desktop_settings.ui_scale);
    (void)prefs_set_int("desktop.wallpaper", desktop_settings.wallpaper);
    (void)prefs_set_int("desktop.reduce_motion", (desktop_settings.flags & MINT_DESKTOP_SETTING_REDUCE_MOTION) != 0);
    bg_valid = 0;                                   /* the wallpaper behind the assistant may have changed */
}

/* ------------------------------------------------------------ smooth large text */
/* Bilinear upscale of a glyph (or any A8 mask) into an A8 buffer. */
#define BIG_W 1100
#define BIG_H 220
static uint8_t big_cur[BIG_W * BIG_H], big_old[BIG_W * BIG_H];
static int32_t big_cur_w, big_old_w, big_h, big_base;
static uint32_t a8_at(const uint8_t *src, int32_t sw, int32_t sh, int32_t x, int32_t y) {
    return x < 0 || y < 0 || x >= sw || y >= sh ? 0u : src[y * sw + x];
}
static void a8_upscale(const uint8_t *src, int32_t sw, int32_t sh, int32_t scale,
                       uint8_t *dst, int32_t stride, int32_t dh_max, int32_t ox, int32_t oy) {
    for (int32_t dy = 0; dy < sh * scale; ++dy) {
        int32_t yy = oy + dy;
        if (yy < 0 || yy >= dh_max) continue;
        int32_t v = ((2 * dy + 1) * 256) / (2 * scale) - 128;
        int32_t iy = v >= 0 ? v >> 8 : -((-v + 255) >> 8), fy = v - iy * 256;
        for (int32_t dx = 0; dx < sw * scale; ++dx) {
            int32_t xx = ox + dx;
            if (xx < 0 || xx >= stride) continue;
            int32_t u = ((2 * dx + 1) * 256) / (2 * scale) - 128;
            int32_t ix = u >= 0 ? u >> 8 : -((-u + 255) >> 8), fx = u - ix * 256;
            uint32_t a = a8_at(src, sw, sh, ix, iy), b = a8_at(src, sw, sh, ix + 1, iy);
            uint32_t c = a8_at(src, sw, sh, ix, iy + 1), d = a8_at(src, sw, sh, ix + 1, iy + 1);
            uint32_t top = a * (uint32_t)(256 - fx) + b * (uint32_t)fx, bot = c * (uint32_t)(256 - fx) + d * (uint32_t)fx;
            uint32_t val = (top * (uint32_t)(256 - fy) + bot * (uint32_t)fy) >> 16;
            uint8_t *p = &dst[yy * stride + xx];
            if (val > *p) *p = (uint8_t)(val > 255u ? 255u : val);
        }
    }
}
static const MINT_GLYPH *glyph(const MINT_FONT *f, uint32_t cp) {
    for (uint32_t i = 0; i < f->glyph_count; ++i) if (f->glyphs[i].codepoint == cp) return &f->glyphs[i];
    return 0;
}
static int32_t big_render(const char *t, int32_t scale, uint8_t *buf) {
    const MINT_FONT *f = &mint_fonts[FONT_DISPLAY];
    big_h = (f->ascent + f->descent + 4) * scale;
    if (big_h > BIG_H) big_h = BIG_H;
    big_base = (f->ascent + 2) * scale;
    for (int32_t i = 0; i < BIG_W * big_h; ++i) buf[i] = 0;
    int32_t pen = 0;                                  /* 26.6 at the font's size */
    for (const char *p = t; *p; ++p) {
        const MINT_GLYPH *g = glyph(f, (uint8_t)*p);
        if (!g) continue;
        int32_t gx = ((pen + 32) >> 6) + g->xoff, gy = (f->ascent + 2) + g->yoff;
        if (g->w) a8_upscale(f->bits + g->offset, g->w, g->h, scale, buf, BIG_W, big_h, (gx + 2) * scale, gy * scale);
        pen += g->advance64;
    }
    int32_t w = (((pen + 32) >> 6) + 4) * scale;
    return w > BIG_W ? BIG_W : w;
}

/* ------------------------------------------------------------ avatar */
void account_avatar(int32_t cx, int32_t cy, int32_t r, const char *full, int font) {
    char ini[12];
    account_initials(full && full[0] ? full : "MintOS", ini, sizeof(ini));
    gfx_circle(S, cx, cy + 1, r, ARGB(0x30, 0, 0, 0));
    gfx_circle(S, cx, cy, r, OPAQUE(gfx_lerp_rgb(T.accent, 0x000000u, 40)));
    gfx_circle(S, cx, cy - 1, r - 1, OPAQUE(T.accent));
    int32_t w = gfx_text_width(font, ini);
    (void)gfx_text(S, font, cx - w / 2, cy + gfx_font_ascent(font) / 2 - 1, ini, OPAQUE(0xFFFFFFu));
}

/* ------------------------------------------------------------ flow */
static uint32_t find_blank_disks(void) {
    blank_count = 0;
    if (mint_storage_diag_get(&snap) != 0) return 0;
    static MINT_FAT16_VOLUME_LIST fat;
    if (mint_fat16_get_volumes(&fat) != 0) fat.count = 0;
    for (uint32_t i = 0; i < snap.device_count && i < MINT_STORAGE_MAX_DEVICES && blank_count < 8u; ++i) {
        MINT_STORAGE_DEVICE_INFO *d = &snap.devices[i];
        if (!d->block_registered || d->mbr_signature_ok || d->sector_count < 131072u) continue;
        int mounted = 0;
        for (uint32_t v = 0; v < fat.count; ++v) if (fat.block_indices[v] == d->block_index) mounted = 1;
        if (!mounted) blank[blank_count++] = i;
    }
    return blank_count;
}
int setup_active(void) { return setup_on; }
void setup_begin(int again) {
    MINT_MFS_VOLUME v;
    rerun = again;
    with_disk_page = !volume_ready(&v) && find_blank_disks() > 0;
    page = PAGE_WELCOME;
    copy_text(full_name, sizeof(full_name), account.present ? account.full_name : "");
    copy_text(acct, sizeof(acct), account.present ? account.account : "");
    full_len = text_length(full_name); acct_len = text_length(acct);
    acct_edited = account.present; focus_field = 0; show_errors = 0; error_text[0] = '\0';
    confirm[0] = '\0'; confirm_len = 0; disk_done = 0; disk_skipped = 0; disk_pick = 0;
    hello_start = now_ms(); hello_shown = 0xFFFFFFFFu;
    bg_valid = 0; setup_on = 1;
    static const char line[] = "[MintDE] Setup Assistant started\n";
    (void)mint_write(1, line, sizeof(line) - 1u);
    damage_all();
}
void setup_boot_check(void) {
    MINT_MFS_VOLUME v;
    if (volume_ready(&v)) { if (!account.present) setup_begin(0); return; }
    if (!account.present && find_blank_disks() > 0) setup_begin(0);
}
static void finish(void) {
    setup_on = 0;
    (void)prefs_flush();
    (void)mint_fs_flush();
    static const char line[] = "[MintDE] Setup Assistant finished\n";
    (void)mint_write(1, line, sizeof(line) - 1u);
    damage_all();
}
static int full_problem_shown(void) { return show_errors || full_len; }
static const char *account_page_problem(void) {
    const char *p = account_full_name_problem(full_name);
    if (p) return p;
    return account_name_problem(acct);
}
static int create_account(void) {
    const char *p = account_page_problem();
    if (p) { show_errors = 1; copy_text(error_text, sizeof(error_text), p); return 0; }
    app_save_all();                                   /* everything lands in the old home before it moves */
    int migrated = 0;
    int64_t rc = account_create(full_name, acct, &migrated);
    if (rc < 0) {
        copy_text(error_text, sizeof(error_text), rc == -MINT_EEXIST ? "A file is in the way of the new home folder." :
                  "MintOS HD couldn\xE2\x80\x99t create the account (error ");
        if (rc != -MINT_EEXIST) { char n[16]; format_u64(n, sizeof(n), (uint64_t)-rc); str_append(error_text, sizeof(error_text), n); str_append(error_text, sizeof(error_text), ")."); }
        return 0;
    }
    if (account.on_disk) prefs_set_home(account.home);
    app_home_changed();
    error_text[0] = '\0';
    char line[96];
    copy_text(line, sizeof(line), "[MintDE] account \"");
    str_append(line, sizeof(line), account.account);
    str_append(line, sizeof(line), account.on_disk ? (migrated ? "\" created; previous home moved\n" : "\" created\n") : "\" for this session only\n");
    (void)mint_write(1, line, text_length(line));
    return 1;
}
static void go_next(void) {
    error_text[0] = '\0';
    switch (page) {
    case PAGE_WELCOME: page = with_disk_page ? PAGE_DISK : PAGE_ACCOUNT; break;
    case PAGE_DISK: if (disk_done || disk_skipped) page = PAGE_ACCOUNT; break;
    case PAGE_ACCOUNT: if (create_account()) page = PAGE_LOOK; break;
    case PAGE_LOOK: page = PAGE_DONE; break;
    case PAGE_DONE: finish(); return;
    default: break;
    }
    damage_all();
}
static void go_back(void) {
    error_text[0] = '\0';
    if (page == PAGE_ACCOUNT) page = with_disk_page ? PAGE_DISK : PAGE_WELCOME;
    else if (page == PAGE_DISK) page = PAGE_WELCOME;
    else if (page == PAGE_LOOK) page = PAGE_ACCOUNT;
    damage_all();
}
static void run_disk_init(void) {
    if (disk_pick >= blank_count) return;
    MINT_STORAGE_DEVICE_INFO *d = &snap.devices[blank[disk_pick]];
    MINT_STORAGE_FORMAT_REQUEST rq;
    for (size_t i = 0; i < sizeof(rq); ++i) ((uint8_t *)&rq)[i] = 0;
    rq.version = MINT_STORAGE_FORMAT_REQUEST_VERSION;
    rq.device_index = blank[disk_pick];
    rq.expected_sector_count = d->sector_count;
    copy_text(rq.expected_serial, sizeof(rq.expected_serial), d->serial);
    copy_text(rq.confirmation, sizeof(rq.confirmation), confirm);
    int64_t rc = mint_storage_init_mintos(&rq);
    MINT_MFS_VOLUME v;
    if (rc == 0 && volume_ready(&v)) {
        disk_done = 1;
        prefs_set_home(v.home);
        app_home_changed();
        static const char line[] = "[MintDE] Setup Assistant initialized MintOS HD\n";
        (void)mint_write(1, line, sizeof(line) - 1u);
    } else {
        copy_text(error_text, sizeof(error_text), "The disk couldn\xE2\x80\x99t be initialized (error ");
        char n[16]; format_u64(n, sizeof(n), (uint64_t)(rc < 0 ? -rc : 5)); str_append(error_text, sizeof(error_text), n);
        str_append(error_text, sizeof(error_text), "). Nothing was changed if the checks failed.");
    }
    confirm[0] = '\0'; confirm_len = 0;
}
static void expected_phrase(char *out, size_t cap) {
    copy_text(out, cap, "FORMAT ");
    if (disk_pick < blank_count) str_append(out, cap, snap.devices[blank[disk_pick]].name);
}
static int phrase_matches(void) {
    char p[32]; expected_phrase(p, sizeof(p));
    size_t n = text_length(p);
    if (confirm_len != n) return 0;
    for (size_t i = 0; i < n; ++i) {
        char a = confirm[i], b = p[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------ geometry */
static gfx_rect card(void) {
    int32_t w = SW() - 80 < 760 ? SW() - 80 : 760, h = SH() - 80 < 540 ? SH() - 80 : 540;
    return R((SW() - w) / 2, (SH() - h) / 2, w, h);
}
static gfx_rect next_button(void) { gfx_rect c = card(); return R(c.x1 - 32 - 150, c.y1 - 60, 150, 32); }
static gfx_rect back_button(void) { gfx_rect c = card(); return R(c.x0 + 32, c.y1 - 60, 110, 32); }
static gfx_rect skip_button(void) { gfx_rect c = card(); return R(c.x1 - 32 - 150 - 12 - 120, c.y1 - 60, 120, 32); }
static gfx_rect welcome_button(void) { gfx_rect c = card(); return R(c.x0 + W(c) / 2 - 90, c.y1 - 78, 180, 34); }
static gfx_rect field_rect(int i) { gfx_rect c = card(); return R(c.x0 + W(c) / 2 - 170, c.y0 + 236 + i * 70, 340, 30); }
static gfx_rect disk_row(uint32_t i) { gfx_rect c = card(); return R(c.x0 + 80, c.y0 + 168 + (int32_t)i * 50, W(c) - 160, 44); }
static gfx_rect confirm_rect(void) { gfx_rect c = card(); return R(c.x0 + W(c) / 2 - 150, c.y0 + 196 + (int32_t)(blank_count < 4u ? blank_count : 4u) * 50 + 30, 300, 30); }
static gfx_rect theme_rect(int i) { gfx_rect c = card(); return R(c.x0 + W(c) / 2 - 270 + i * 190, c.y0 + 150, 160, 104); }
static gfx_rect swatch_rect(int i) { gfx_rect c = card(); return R(c.x0 + W(c) / 2 - 2 * 44 - 16 + i * 44, c.y0 + 334, 32, 32); }
static gfx_rect hello_rect(void) { gfx_rect c = card(); return R(c.x0 + 10, c.y0 + 170, W(c) - 20, BIG_H); }
static int next_enabled(void) {
    if (page == PAGE_DISK) return disk_done || disk_skipped || (blank_count && phrase_matches());
    if (page == PAGE_ACCOUNT) return !account_page_problem();
    return 1;
}
static const char *next_label(void) {
    if (page == PAGE_DISK && !disk_done) return "Initialize";
    if (page == PAGE_DONE) return "Start Using MintOS";
    return "Continue";
}

/* ------------------------------------------------------------ painting */
static void title_center(gfx_rect c, int32_t y, const char *t) { ui_label_center(c.x0 + W(c) / 2, y, FONT_TITLE, t, T.text); }
static void body_center(gfx_rect c, int32_t y, const char *t, uint32_t rgb) { ui_label_center(c.x0 + W(c) / 2, y, FONT_BODY, t, rgb); }
static void theme_thumb(gfx_rect r, int dark, int autom, int selected) {
    if (selected) gfx_round_rect(S, r.x0 - 4, r.y0 - 4, W(r) + 8, H(r) + 8, 12, OPAQUE(T.accent));
    gfx_round_rect_grad(S, r.x0, r.y0, W(r), H(r), 9, OPAQUE(dark ? 0x141A40u : 0x86BEF0u), OPAQUE(dark ? 0x4A2A68u : 0xF6D8C8u));
    if (autom) {
        gfx_rect saved = gfx_clip_get();
        gfx_clip_set(gfx_rect_intersect(saved, R(r.x0 + W(r) / 2, r.y0, W(r) / 2, H(r))));
        gfx_round_rect_grad(S, r.x0, r.y0, W(r), H(r), 9, OPAQUE(0x141A40u), OPAQUE(0x4A2A68u));
        gfx_clip_set(saved);
    }
    /* a tiny window on the thumbnail */
    gfx_rect w = R(r.x0 + 18, r.y0 + 18, W(r) - 50, H(r) - 34);
    gfx_round_rect(S, w.x0, w.y0, W(w), H(w), 5, OPAQUE(dark && !autom ? 0x2C2C2Eu : 0xFFFFFFu));
    if (autom) {
        gfx_rect saved = gfx_clip_get();
        gfx_clip_set(gfx_rect_intersect(saved, R(r.x0 + W(r) / 2, r.y0, W(r), H(r))));
        gfx_round_rect(S, w.x0, w.y0, W(w), H(w), 5, OPAQUE(0x2C2C2Eu));
        gfx_clip_set(saved);
    }
    gfx_fill(S, w.x0, w.y0 + 12, 26, H(w) - 12, OPAQUE(dark && !autom ? 0x3A3A3Cu : 0xE8E8EDu));
    gfx_circle(S, w.x0 + 7, w.y0 + 6, 2, OPAQUE(0xFF5F57u));
    gfx_circle(S, w.x0 + 14, w.y0 + 6, 2, OPAQUE(0xFEBC2Eu));
    gfx_circle(S, w.x0 + 21, w.y0 + 6, 2, OPAQUE(0x28C840u));
    gfx_round_rect(S, w.x0 + 34, w.y0 + 18, 40, 6, 3, OPAQUE(T.accent));
    gfx_round_rect_stroke(S, r.x0, r.y0, W(r), H(r), 9, T.separator);
}
static void paint_dots(gfx_rect c) {
    uint32_t total = with_disk_page ? 5u : 4u, at = page;
    if (!with_disk_page && page > PAGE_DISK) --at;
    int32_t x0 = c.x0 + W(c) / 2 - (int32_t)(total - 1u) * 9;
    for (uint32_t i = 0; i < total; ++i)
        gfx_circle(S, x0 + (int32_t)i * 18, c.y1 - 22, 3, i == at ? OPAQUE(T.accent) : (T.dark ? ARGB(0x50, 0xFF, 0xFF, 0xFF) : ARGB(0x30, 0, 0, 0)));
}
static void paint_hello(gfx_rect c) {
    uint64_t t = now_ms() - hello_start;
    uint32_t idx = (uint32_t)(t / HELLO_PERIOD) % HELLO_COUNT;
    uint32_t in_period = (uint32_t)(t % HELLO_PERIOD);
    int32_t scale = SW() >= 1280 && SH() >= 760 ? 3 : 2;
    if (idx != hello_shown) {
        for (int32_t i = 0; i < BIG_W * BIG_H; ++i) big_old[i] = big_cur[i];
        big_old_w = hello_shown == 0xFFFFFFFFu ? 0 : big_cur_w;
        big_cur_w = big_render(hello[idx], scale, big_cur);
        hello_shown = idx;
    }
    uint32_t a = in_period < HELLO_FADE && big_old_w ? in_period * 255u / HELLO_FADE : 255u;
    int32_t top = c.y0 + 186;
    uint32_t color = T.dark ? 0xF2F2F7u : 0x1D1D1Fu;
    /* 1:1 copy of the left part of the BIG_W-wide mask, limited by the clip */
    gfx_rect saved = gfx_clip_get();
    if (a < 255u && big_old_w) {
        gfx_clip_set(gfx_rect_intersect(saved, R(c.x0 + W(c) / 2 - big_old_w / 2, top, big_old_w, big_h)));
        gfx_mask(S, c.x0 + W(c) / 2 - big_old_w / 2, top, BIG_W, big_h, big_old, BIG_W, big_h, WITH_ALPHA(color, 255u - a));
    }
    gfx_clip_set(gfx_rect_intersect(saved, R(c.x0 + W(c) / 2 - big_cur_w / 2, top, big_cur_w, big_h)));
    gfx_mask(S, c.x0 + W(c) / 2 - big_cur_w / 2, top, BIG_W, big_h, big_cur, BIG_W, big_h, WITH_ALPHA(color, a));
    gfx_clip_set(saved);
}
static void paint_welcome(gfx_rect c) {
    gfx_image(S, c.x0 + W(c) / 2 - 48, c.y0 + 54, 96, 96, mint_icon_about_mintos_128, 128, 128, 255u);
    paint_hello(c);
    title_center(c, c.y0 + 186 + big_h + 24, rerun ? "Welcome back to MintOS" : "Welcome to MintOS");
    body_center(c, c.y0 + 186 + big_h + 50, rerun ? "Change your account name or your look. Your files stay where they are."
                                                  : "Let\xE2\x80\x99s set up MintOS so it remembers your things.", T.text2);
    ui_button(welcome_button(), rerun ? "Continue" : "Get Started", 1, 1);
}
static void paint_disk(gfx_rect c) {
    title_center(c, c.y0 + 70, "Where Should MintOS Keep Your Things?");
    body_center(c, c.y0 + 100, "MintOS saves your account, notes and settings on a MintOS disk.", T.text2);
    if (disk_done) {
        gfx_circle(S, c.x0 + W(c) / 2, c.y0 + 200, 34, OPAQUE(0x34C759u));
        ui_symbol_glyph(c.x0 + W(c) / 2, c.y0 + 200, SYM_CHECK, OPAQUE(0xFFFFFFu));
        title_center(c, c.y0 + 270, "MintOS HD Is Ready");
        body_center(c, c.y0 + 298, "Everything you save from now on stays after a restart.", T.text2);
        return;
    }
    body_center(c, c.y0 + 124, blank_count ? "This computer has a blank disk MintOS can initialize:" : "No blank disk was found.", T.text2);
    for (uint32_t i = 0; i < blank_count && i < 4u; ++i) {
        MINT_STORAGE_DEVICE_INFO *d = &snap.devices[blank[i]];
        gfx_rect r = disk_row(i);
        int on = disk_pick == i;
        gfx_round_rect(S, r.x0, r.y0, W(r), H(r), 9, on ? WITH_ALPHA(T.accent, 0x30) : (T.dark ? ARGB(0x18, 0xFF, 0xFF, 0xFF) : ARGB(0x0A, 0, 0, 0)));
        if (on) gfx_round_rect_stroke(S, r.x0, r.y0, W(r), H(r), 9, OPAQUE(T.accent));
        gfx_image(S, r.x0 + 10, r.y0 + 6, 32, 32, mint_icon_disk_utility_64, 64, 64, 255u);
        char t[96], cap[32];
        format_bytes(cap, sizeof(cap), d->capacity_bytes);
        copy_text(t, sizeof(t), d->model[0] ? d->model : "Disk");
        gfx_text_fit(S, FONT_BODY_MEDIUM, r.x0 + 54, r.y0 + 19, t, W(r) - 200, OPAQUE(T.text));
        copy_text(t, sizeof(t), d->name); str_append(t, sizeof(t), " \xC2\xB7 "); str_append(t, sizeof(t), cap);
        str_append(t, sizeof(t), " \xC2\xB7 blank");
        ui_label(r.x0 + 54, r.y0 + 36, FONT_CAPTION, t, T.text2);
        gfx_circle(S, r.x1 - 22, r.y0 + H(r) / 2, 8, OPAQUE(on ? T.accent : T.text3));
        gfx_circle(S, r.x1 - 22, r.y0 + H(r) / 2, on ? 3 : 6, OPAQUE(on ? 0xFFFFFFu : T.window));
    }
    if (blank_count) {
        char p[32], prompt[64];
        expected_phrase(p, sizeof(p));
        copy_text(prompt, sizeof(prompt), "Initializing erases this disk. Type \xE2\x80\x9C"); str_append(prompt, sizeof(prompt), p);
        str_append(prompt, sizeof(prompt), "\xE2\x80\x9D to confirm:");
        gfx_rect f = confirm_rect();
        ui_label_center(c.x0 + W(c) / 2, f.y0 - 12, FONT_BODY, prompt, T.text);
        ui_text_field_caret(f, confirm, p, 1, confirm_len);
    }
    body_center(c, c.y1 - 92, "Not Now keeps MintOS working for this session only; nothing is saved.", T.text3);
}
static void paint_account(gfx_rect c) {
    title_center(c, c.y0 + 66, rerun ? "Your Computer Account" : "Create a Computer Account");
    account_avatar(c.x0 + W(c) / 2, c.y0 + 138, 44, full_name, FONT_HUGE);
    static const char *const labels[2] = {"Full name", "Account name"};
    for (int i = 0; i < 2; ++i) {
        gfx_rect f = field_rect(i);
        ui_label(f.x0 + 2, f.y0 - 9, FONT_CAPTION_BOLD, labels[i], T.text2);
        ui_text_field_caret(f, i ? acct : full_name, i ? "jappleseed" : "Jane Appleseed", focus_field == i, i ? acct_len : full_len);
    }
    gfx_rect f = field_rect(1);
    char hint[96];
    MINT_MFS_VOLUME v;
    if (volume_ready(&v)) {
        copy_text(hint, sizeof(hint), "Your home folder: /Users/");
        str_append(hint, sizeof(hint), acct[0] ? acct : "\xE2\x80\xA6");
    } else copy_text(hint, sizeof(hint), "Without MintOS HD the account lasts until restart.");
    ui_label_center(c.x0 + W(c) / 2, f.y1 + 22, FONT_CAPTION, hint, T.text2);
    const char *problem = error_text[0] ? error_text : 0;
    if (!problem && full_problem_shown()) problem = account_page_problem();
    if (problem && (show_errors || full_len || acct_len)) ui_label_center(c.x0 + W(c) / 2, f.y1 + 44, FONT_CAPTION, problem, 0xFF6A3Du);
}
static void paint_look(gfx_rect c) {
    title_center(c, c.y0 + 70, "Choose Your Look");
    body_center(c, c.y0 + 98, "You can change this any time in System Settings \xE2\x80\xBA Appearance.", T.text2);
    static const char *const names[3] = {"Light", "Dark", "Auto"};
    static const uint32_t themes[3] = {MINT_THEME_LIGHT, MINT_THEME_DARK, MINT_THEME_WARM};
    for (int i = 0; i < 3; ++i) {
        gfx_rect r = theme_rect(i);
        theme_thumb(r, i == 1, i == 2, desktop_settings.theme == themes[i]);
        ui_label_center(r.x0 + W(r) / 2, r.y1 + 24, desktop_settings.theme == themes[i] ? FONT_BODY_BOLD : FONT_BODY, names[i], T.text);
    }
    ui_label_center(c.x0 + W(c) / 2, c.y0 + 318, FONT_BODY_MEDIUM, "Accent color", T.text);
    static const uint32_t sw[5] = {0x00B894u, 0x007AFFu, 0xAF52DEu, 0xFF9500u, 0xFF2D55u};
    static const char *const sw_names[5] = {"Mint", "Blue", "Violet", "Orange", "Rose"};
    for (int i = 0; i < 5; ++i) {
        gfx_rect r = swatch_rect(i);
        int on = desktop_settings.accent == (uint32_t)i;
        if (on) gfx_circle(S, r.x0 + 16, r.y0 + 16, 16, OPAQUE(sw[i]));
        if (on) gfx_circle(S, r.x0 + 16, r.y0 + 16, 14, OPAQUE(T.window));
        gfx_circle(S, r.x0 + 16, r.y0 + 16, on ? 12 : 13, OPAQUE(sw[i]));
    }
    ui_label_center(c.x0 + W(c) / 2, c.y0 + 392, FONT_CAPTION, sw_names[desktop_settings.accent < 5u ? desktop_settings.accent : 0], T.text2);
}
static void paint_done(gfx_rect c) {
    account_avatar(c.x0 + W(c) / 2, c.y0 + 104, 46, account.full_name, FONT_HUGE);
    char t[96], first[48];
    size_t i = 0;
    for (; account.full_name[i] && account.full_name[i] != ' ' && i + 1u < sizeof(first); ++i) first[i] = account.full_name[i];
    first[i] = '\0';
    copy_text(t, sizeof(t), rerun ? "All Set, " : "Welcome to MintOS, ");
    str_append(t, sizeof(t), first[0] ? first : "friend");
    str_append(t, sizeof(t), "!");
    title_center(c, c.y0 + 190, t);
    gfx_rect g = R(c.x0 + W(c) / 2 - 220, c.y0 + 222, 440, 3 * 36);
    ui_group(g);
    char who[96];
    copy_text(who, sizeof(who), account.full_name); str_append(who, sizeof(who), " (");
    str_append(who, sizeof(who), account.account); str_append(who, sizeof(who), ")");
    ui_group_row(R(g.x0, g.y0, W(g), 36), 0, 3, "Account", who);
    ui_group_row(R(g.x0, g.y0 + 36, W(g), 36), 1, 3, "Home folder", account.on_disk ? account.home : "None (session only)");
    ui_group_row(R(g.x0, g.y0 + 72, W(g), 36), 2, 3, "Saved on", account.on_disk ? "MintOS HD \xE2\x80\x94 kept after restart" : "Nothing is saved");
    body_center(c, g.y1 + 30, account.on_disk ? "Your notes, files, Terminal history and settings are saved automatically."
                                              : "Initialize a disk in Disk Utility to keep your things.", T.text2);
}
void setup_paint(gfx_rect d) {
    gfx_rect all = R(0, 0, SW(), SH());
    if (!bg_valid) {
        gfx_clip_set(all);
        gfx_copy(S, &gfx_wall, all);
        gfx_blur(S, all, 34);                           /* leaves the blurred image in gfx_scratch */
        gfx_fill(&gfx_scratch, 0, 0, SW(), SH(), T.dark ? ARGB(0x70, 0x0A, 0x0A, 0x10) : ARGB(0x50, 0xFF, 0xFF, 0xFF));
        bg_valid = 1;
    }
    gfx_clip_set(d);
    gfx_copy(S, &gfx_scratch, d);
    gfx_rect c = card();
    gfx_shadow(S, c.x0, c.y0, W(c), H(c), 18, 40, 14, ARGB(0x50, 0, 0, 0));
    gfx_round_rect(S, c.x0, c.y0, W(c), H(c), 18, T.dark ? ARGB(0xF0, 0x26, 0x26, 0x29) : ARGB(0xF4, 0xFB, 0xFB, 0xFC));
    gfx_round_rect_stroke(S, c.x0, c.y0, W(c), H(c), 18, T.dark ? ARGB(0x40, 0xFF, 0xFF, 0xFF) : ARGB(0x22, 0, 0, 0));
    switch (page) {
    case PAGE_WELCOME: paint_welcome(c); break;
    case PAGE_DISK: paint_disk(c); break;
    case PAGE_ACCOUNT: paint_account(c); break;
    case PAGE_LOOK: paint_look(c); break;
    default: paint_done(c); break;
    }
    if (page != PAGE_WELCOME) {
        if (page != PAGE_DONE) ui_button(back_button(), "Back", 0, 1);
        if (page == PAGE_DISK && !disk_done) ui_button(skip_button(), "Not Now", 0, 1);
        ui_button(next_button(), next_label(), 1, next_enabled());
        if (page == PAGE_DISK && error_text[0]) ui_label_center(c.x0 + W(c) / 2, c.y1 - 110, FONT_CAPTION, error_text, 0xFF6A3Du);
    }
    paint_dots(c);
}

/* ------------------------------------------------------------ input */
int setup_tick(void) {
    if (!setup_on || page != PAGE_WELCOME) return 0;
    uint64_t t = now_ms() - hello_start;
    uint32_t idx = (uint32_t)(t / HELLO_PERIOD) % HELLO_COUNT, in_period = (uint32_t)(t % HELLO_PERIOD);
    static uint64_t last_frame;
    if (idx != hello_shown || (in_period < HELLO_FADE + 40u && now_ms() - last_frame >= 33u)) {
        last_frame = now_ms();
        shell_damage(hello_rect());
        return 1;
    }
    return 0;
}
static void set_theme(uint32_t theme, uint32_t accent) {
    desktop_settings.theme = theme;
    desktop_settings.accent = accent;
    (void)shell_apply_settings();                   /* live preview: menu bar, wallpaper, this card */
    bg_valid = 0;
}
void setup_click(int32_t x, int32_t y) {
    gfx_rect c = card();
    if (page == PAGE_WELCOME) { if (in(welcome_button(), x, y)) go_next(); return; }
    if (page != PAGE_DONE && in(back_button(), x, y)) { go_back(); return; }
    if (page == PAGE_DISK && !disk_done && in(skip_button(), x, y)) { disk_skipped = 1; go_next(); return; }
    if (in(next_button(), x, y)) {
        if (page == PAGE_DISK && !disk_done && !disk_skipped) { if (phrase_matches()) run_disk_init(); damage_all(); return; }
        if (next_enabled() || page == PAGE_ACCOUNT) go_next();
        return;
    }
    if (page == PAGE_DISK) {
        for (uint32_t i = 0; i < blank_count && i < 4u; ++i)
            if (in(disk_row(i), x, y)) { disk_pick = i; confirm[0] = '\0'; confirm_len = 0; }
    } else if (page == PAGE_ACCOUNT) {
        for (int i = 0; i < 2; ++i) if (in(field_rect(i), x, y)) focus_field = i;
    } else if (page == PAGE_LOOK) {
        static const uint32_t themes[3] = {MINT_THEME_LIGHT, MINT_THEME_DARK, MINT_THEME_WARM};
        for (int i = 0; i < 3; ++i) if (in(theme_rect(i), x, y)) set_theme(themes[i], desktop_settings.accent);
        for (int i = 0; i < 5; ++i) if (in(swatch_rect(i), x, y)) set_theme(desktop_settings.theme, (uint32_t)i);
    }
    (void)c;
    damage_all();
}
static void type_into(char *buf, size_t *len, size_t cap, uint32_t ch, int account_field) {
    if (ch == 8u) {
        if (!*len) return;
        do --*len; while (*len && ((unsigned char)buf[*len] & 0xC0u) == 0x80u);
        buf[*len] = '\0';
        return;
    }
    if (account_field) {
        if (ch >= 'A' && ch <= 'Z') ch += 32u;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_')) return;
    }
    if (ch < 32u || ch == 127u) return;
    char t[4]; size_t n;
    if (ch < 0x80u) { t[0] = (char)ch; n = 1; }
    else if (ch < 0x800u) { t[0] = (char)(0xC0u | (ch >> 6)); t[1] = (char)(0x80u | (ch & 0x3Fu)); n = 2; }
    else if (ch < 0x10000u) { t[0] = (char)(0xE0u | (ch >> 12)); t[1] = (char)(0x80u | ((ch >> 6) & 0x3Fu)); t[2] = (char)(0x80u | (ch & 0x3Fu)); n = 3; }
    else return;
    if (*len + n + 1u > cap) return;
    for (size_t i = 0; i < n; ++i) buf[(*len)++] = t[i];
    buf[*len] = '\0';
}
void setup_key(const MINT_DESKTOP_KEY_EVENT *k) {
    uint32_t ch = k->codepoint;
    if (k->modifiers & MINT_KEYMOD_GUI) return;
    if (ch == '\n') {
        if (page == PAGE_DISK && !disk_done) { if (phrase_matches()) run_disk_init(); damage_all(); return; }
        if (next_enabled() || page == PAGE_ACCOUNT) go_next();
        return;
    }
    if (ch == 27u) { if (page != PAGE_WELCOME && page != PAGE_DONE) go_back(); return; }
    if (page == PAGE_ACCOUNT) {
        if (ch == 9u) { focus_field ^= 1; damage_all(); return; }
        error_text[0] = '\0';
        if (focus_field == 0) {
            type_into(full_name, &full_len, sizeof(full_name), ch, 0);
            if (!acct_edited) { account_suggest_name(full_name, acct, sizeof(acct)); acct_len = text_length(acct); if (!full_len) { acct[0] = '\0'; acct_len = 0; } }
        } else {
            type_into(acct, &acct_len, sizeof(acct), ch, 1);
            acct_edited = acct_len > 0;
        }
        damage_all();
    } else if (page == PAGE_DISK && !disk_done) {
        if (ch == 9u) { disk_pick = blank_count ? (disk_pick + 1u) % blank_count : 0u; confirm[0] = '\0'; confirm_len = 0; }
        else if (ch < 0x80u) type_into(confirm, &confirm_len, sizeof(confirm), ch, 0);
        damage_all();
    } else if (page == PAGE_LOOK) {
        if (k->scancode == 0x14Bu && desktop_settings.accent > 0u) set_theme(desktop_settings.theme, desktop_settings.accent - 1u);
        if (k->scancode == 0x14Du && desktop_settings.accent < 4u) set_theme(desktop_settings.theme, desktop_settings.accent + 1u);
        damage_all();
    }
}
uint32_t setup_cursor(int32_t x, int32_t y) {
    if (page == PAGE_ACCOUNT && (in(field_rect(0), x, y) || in(field_rect(1), x, y))) return MINT_CURSOR_SHAPE_IBEAM;
    if (page == PAGE_DISK && blank_count && in(confirm_rect(), x, y)) return MINT_CURSOR_SHAPE_IBEAM;
    return MINT_CURSOR_SHAPE_ARROW;
}

/* ------------------------------------------------------------ Settings: Users & Groups */
void users_card_paint(gfx_rect r, int selected, int focused) {
    if (selected) gfx_round_rect(S, r.x0, r.y0, W(r), H(r), 8, OPAQUE(focused ? T.accent : T.selection_inactive));
    uint32_t fg = selected && focused ? 0xFFFFFFu : T.text, fg2 = selected && focused ? 0xE6F0FFu : T.text2;
    account_avatar(r.x0 + 26, r.y0 + H(r) / 2, 18, account.present ? account.full_name : "", FONT_BODY_BOLD);
    gfx_text_fit(S, FONT_BODY_BOLD, r.x0 + 54, r.y0 + H(r) / 2 - 2, account_display_name(), W(r) - 62, OPAQUE(fg));
    gfx_text_fit(S, FONT_CAPTION, r.x0 + 54, r.y0 + H(r) / 2 + 14, account.present ? (account.on_disk ? "MintOS Account" : "Session account") : "Set up your account",
                 W(r) - 62, OPAQUE(fg2));
}
static gfx_rect users_button(gfx_rect b) { return R(b.x0, b.y0 + 300, 210, 28); }
void users_pane_paint(gfx_rect b) {
    gfx_rect hero = R(b.x0, b.y0, W(b), 104);
    ui_group(hero);
    account_avatar(hero.x0 + 52, hero.y0 + 52, 34, account.present ? account.full_name : "", FONT_TITLE);
    ui_label(hero.x0 + 102, hero.y0 + 46, FONT_TITLE, account.present ? account.full_name : "No Account Yet", T.text);
    ui_label(hero.x0 + 102, hero.y0 + 68, FONT_BODY, account.present ? "Admin" : "Create an account to keep your things on MintOS HD.", T.text2);
    MINT_MFS_VOLUME v;
    int disk = volume_ready(&v);
    const char *labels[4] = {"Account name", "Home folder", "Login shell", "Saved on"};
    const char *values[4] = {account.present ? account.account : "\xE2\x80\x94", disk ? v.home : "None (no MintOS HD)", "mintsh (MintOS Terminal)",
                             account.on_disk ? "MintOS HD \xE2\x80\x94 /Library/Preferences" : "Session only"};
    gfx_rect g = R(b.x0, hero.y1 + 18, W(b), 4 * 36);
    ui_group(g);
    for (uint32_t i = 0; i < 4u; ++i) ui_group_row(R(g.x0, g.y0 + (int32_t)i * 36, W(g), 36), i, 4u, labels[i], values[i]);
    ui_button(users_button(b), account.present ? "Run Setup Assistant\xE2\x80\xA6" : "Create Account\xE2\x80\xA6", 0, 1);
    ui_label(b.x0 + 4, b.y0 + 352, FONT_CAPTION, "MintOS has one account per MintOS disk for now; passwords and more users come later.", T.text2);
    ui_label(b.x0 + 4, b.y0 + 372, FONT_CAPTION, "The Setup Assistant can rename the account; its home folder moves with it.", T.text2);
}
int users_pane_click(gfx_rect b, int32_t x, int32_t y) {
    if (in(users_button(b), x, y)) { setup_begin(account.present); return 1; }
    return 0;
}
