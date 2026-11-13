/* MintDE 26.5.014.0 shell extras: Notification Center (notifications plus
 * calendar, system, world clock and Notes widgets), notification banners,
 * the volume HUD, Control Center sound and Do Not Disturb controls, and
 * Command-Shift-3 screenshots. desktop.c calls in through a handful of
 * hooks; everything here draws into the shared canvas with gfx. */
#include "mintde.h"

#define S (&gfx_screen)
#define MENU_H 26
#define NC_W 360
#define BANNER_W 344
#define BANNER_H 70
#define BANNER_SLIDE_MS 260u
#define HUD_MS 1600u

static int32_t SW, SH;
static int nc_open;
static uint64_t hud_until;
static int cc_drag;
static gfx_rect cc_slider;
static int32_t hover_banner = -1;
static uint32_t hover_banner_id;
static int64_t nc_minute = -1;
static char notes_lines[5][64];
static uint32_t notes_line_count;

static gfx_rect R(int32_t x, int32_t y, int32_t w, int32_t h) { return gfx_rect_xywh(x, y, w, h); }
static int32_t W(gfx_rect r) { return r.x1 - r.x0; }
static int32_t H(gfx_rect r) { return r.y1 - r.y0; }
static int32_t BL(gfx_rect r, int font) { return r.y0 + (H(r) - gfx_font_line(font)) / 2 + gfx_font_ascent(font); }

uint64_t notify_ms(void) {
    mint_timespec64 t = {0, 0};
    (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void glass_panel(gfx_rect r, int32_t radius) {
    gfx_shadow(S, r.x0, r.y0, W(r), H(r), radius, 24, 10, ARGB(0x40, 0, 0, 0));
    gfx_blur_rounded(S, r, 22, radius);
    gfx_round_rect(S, r.x0, r.y0, W(r), H(r), radius, T.dark ? ARGB(0xC8, 0x28, 0x28, 0x2C) : ARGB(0xC4, 0xF4, 0xF4, 0xF6));
    gfx_round_rect_stroke(S, r.x0, r.y0, W(r), H(r), radius, T.dark ? ARGB(0x40, 0xFF, 0xFF, 0xFF) : ARGB(0x22, 0, 0, 0));
}
static void tile(gfx_rect r, int32_t radius) {
    gfx_round_rect(S, r.x0, r.y0, W(r), H(r), radius, T.dark ? ARGB(0x30, 0xFF, 0xFF, 0xFF) : ARGB(0x90, 0xFF, 0xFF, 0xFF));
}

/* ------------------------------------------------------------ calendar maths */
static int64_t days_from_civil(int64_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t mp = m > 2u ? m - 3u : m + 9u;
    uint32_t doy = (153u * mp + 2u) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civil(int64_t z, int64_t *y, uint32_t *m, uint32_t *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}
static uint32_t weekday(int64_t days) { return (uint32_t)(((days % 7) + 11) % 7); }   /* 0 = Sunday */
static uint32_t month_days(int64_t y, uint32_t m) {
    static const uint8_t md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return md[m - 1u] + (m == 2u && leap ? 1u : 0u);
}
static int64_t nth_sunday(int64_t y, uint32_t m, uint32_t n) {        /* n = 0: last Sunday */
    if (!n) { int64_t last = days_from_civil(y, m, month_days(y, m)); return last - weekday(last); }
    int64_t first = days_from_civil(y, m, 1);
    return first + (7 - weekday(first)) % 7 + 7 * (n - 1u);
}
/* World clock offset in seconds; the hardware clock is taken as UTC. */
static int64_t city_offset(uint32_t city, int64_t utc) {
    int64_t y; uint32_t m, d; civil(utc / 86400, &y, &m, &d);
    switch (city) {
    case 1: {                                                      /* London: EU summer time */
        int64_t a = nth_sunday(y, 3, 0) * 86400 + 3600, b = nth_sunday(y, 10, 0) * 86400 + 3600;
        return utc >= a && utc < b ? 3600 : 0;
    }
    case 2: {                                                      /* New York: US daylight time */
        int64_t a = nth_sunday(y, 3, 2) * 86400 + 7 * 3600, b = nth_sunday(y, 11, 1) * 86400 + 6 * 3600;
        return utc >= a && utc < b ? -4 * 3600 : -5 * 3600;
    }
    case 3: return 9 * 3600;                                       /* Tokyo */
    default: return 0;
    }
}

static const char *const month_names[12] = {"January", "February", "March", "April", "May", "June", "July",
                                             "August", "September", "October", "November", "December"};
static const char *const day_names[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

/* ------------------------------------------------------------ glyphs */
static void speaker_glyph(int32_t x, int32_t cy, uint32_t level, int muted, uint32_t argb) {
    gfx_fill(S, x, cy - 3, 4, 7, argb);
    for (int32_t i = 0; i < 6; ++i) gfx_vline(S, x + 4 + i, cy - 3 - i, 7 + 2 * i, argb);
    if (muted) {
        gfx_line(S, x + 13, cy - 4, x + 20, cy + 3, argb);
        gfx_line(S, x + 20, cy - 4, x + 13, cy + 3, argb);
        return;
    }
    gfx_rect saved = gfx_clip_get();
    gfx_clip_set(gfx_rect_intersect(saved, R(x + 12, cy - 12, 14, 25)));
    if (level > 0u) gfx_circle_stroke(S, x + 9, cy, 5, argb);
    if (level > 33u) gfx_circle_stroke(S, x + 9, cy, 9, argb);
    if (level > 66u) gfx_circle_stroke(S, x + 9, cy, 13, argb);
    gfx_clip_set(saved);
}
static void moon_glyph(int32_t cx, int32_t cy, int32_t r, uint32_t argb) {
    for (int32_t dy = -r; dy <= r; ++dy)
        for (int32_t dx = -r; dx <= r; ++dx) {
            int32_t ox = dx - r * 2 / 3, oy = dy + r / 2;
            if (dx * dx + dy * dy <= r * r && ox * ox + oy * oy > r * r * 7 / 10) gfx_fill(S, cx + dx, cy + dy, 1, 1, argb);
        }
}

/* ------------------------------------------------------------ notifications */
uint32_t notify_post(uint32_t app, const char *title, const char *body) {
    int banner = personality_prefs.banners && !personality_prefs.do_not_disturb && !nc_open;
    uint32_t id = notify_queue_push(app, title, body, shell_now_epoch(), notify_ms(), banner);
    ui_sound(UI_SOUND_NOTIFICATION);
    char line[200];
    copy_text(line, sizeof(line), "[MintDE] notification: ");
    str_append(line, sizeof(line), title ? title : "");
    str_append(line, sizeof(line), " \xE2\x80\x94 ");
    str_append(line, sizeof(line), body ? body : "");
    str_append(line, sizeof(line), banner ? " (banner)\n" : " (Notification Center)\n");
    (void)mint_write(1, line, text_length(line));
    shell_damage(R(SW - BANNER_W - 60, MENU_H, BANNER_W + 60, (int32_t)NOTIFY_BANNERS_SHOWN * (BANNER_H + 8) + 60));
    if (nc_open) shell_damage(R(SW - NC_W - 60, 0, NC_W + 60, SH));
    return id;
}

static gfx_rect banner_rect(uint32_t index, uint64_t posted, uint64_t now) {
    int32_t slide = 0;
    uint64_t t = now - posted;
    if (t < BANNER_SLIDE_MS) {
        uint32_t left = BANNER_SLIDE_MS - (uint32_t)t;
        slide = (int32_t)((uint64_t)left * left * (BANNER_W + 24) / ((uint64_t)BANNER_SLIDE_MS * BANNER_SLIDE_MS));
    }
    return R(SW - BANNER_W - 12 + slide, MENU_H + 8 + (int32_t)index * (BANNER_H + 8), BANNER_W, BANNER_H);
}

static void notification_card(gfx_rect r, const mde_notification *n, uint64_t now, int banner, int hover) {
    if (banner) glass_panel(r, 14); else tile(r, 12);
    gfx_image(S, r.x0 + 12, r.y0 + (H(r) - 32) / 2, 32, 32, app_icon(n->app, 0), 64, 64, 255u);
    char age[24];
    notify_age_text(age, sizeof(age), n->posted_ms, now);
    int32_t aw = gfx_text_width(FONT_CAPTION, age);
    ui_label_right(r.x1 - 12, r.y0 + 24, FONT_CAPTION, age, T.text3);
    gfx_text_fit(S, FONT_BODY_BOLD, r.x0 + 54, r.y0 + 25, n->title, W(r) - 54 - aw - 22, OPAQUE(T.text));
    if (n->body[0]) gfx_text_fit(S, FONT_BODY, r.x0 + 54, r.y0 + 45, n->body, W(r) - 66, OPAQUE(T.text2));
    if (hover) {
        gfx_circle(S, r.x0 + 4, r.y0 + 4, 9, OPAQUE(T.dark ? 0x505055u : 0xF2F2F4u));
        gfx_circle_stroke(S, r.x0 + 4, r.y0 + 4, 9, T.separator);
        gfx_line(S, r.x0 + 1, r.y0 + 1, r.x0 + 7, r.y0 + 7, OPAQUE(T.text2));
        gfx_line(S, r.x0 + 7, r.y0 + 1, r.x0 + 1, r.y0 + 7, OPAQUE(T.text2));
    }
}

static void paint_banners(void) {
    if (nc_open) return;
    uint32_t ids[NOTIFY_BANNERS_SHOWN];
    uint32_t n = notify_banners(ids, NOTIFY_BANNERS_SHOWN);
    uint64_t now = notify_ms();
    for (uint32_t i = 0; i < n; ++i) {
        const mde_notification *note = notify_find(ids[i]);
        if (note) notification_card(banner_rect(i, note->posted_ms, now), note, now, 1, hover_banner == (int32_t)i);
    }
}

/* ------------------------------------------------------------ Notification Center */
static int32_t nc_notification_rows(void) { uint32_t c = notify_count(); return (int32_t)(c > 4u ? 4u : c); }
static gfx_rect nc_rect(void) {
    int32_t rows = nc_notification_rows();
    int32_t h = 14 + 36 + (rows ? rows * 74 + (notify_count() > 4u ? 22 : 0) : 52) + 4 + 172 + 122 + 130 + 4;
    if (h > SH - MENU_H - 12) h = SH - MENU_H - 12;
    return R(SW - NC_W - 12, MENU_H + 6, NC_W, h);
}
static gfx_rect nc_card_rect(int32_t i) { gfx_rect r = nc_rect(); return R(r.x0 + 12, r.y0 + 50 + i * 74, NC_W - 24, 66); }
static gfx_rect nc_clear_rect(void) { gfx_rect r = nc_rect(); return R(r.x1 - 70, r.y0 + 14, 56, 22); }
static int32_t nc_widgets_y(void) {
    int32_t rows = nc_notification_rows();
    return nc_rect().y0 + 50 + (rows ? rows * 74 + (notify_count() > 4u ? 22 : 0) : 52) + 4;
}

static void load_notes_preview(void) {
    notes_line_count = 0;
    MINT_MFS_VOLUME v;
    if (mint_fs_get_volume(&v) != 0 || v.state != MINT_MFS_VOLUME_READY) return;
    /* The most recently edited note in ~/Documents/Notes (data saving's Notes store). */
    static char dir[160], path[160 + MINT_MFS_NAME_MAX + 2], text[1024];
    static MINT_MFS_DIRENT list[MINT_MFS_LIST_MAX];
    copy_text(dir, sizeof(dir), v.home);
    str_append(dir, sizeof(dir), "/Documents/Notes");
    int64_t best_time = -1;
    path[0] = '\0';
    for (uint32_t start = 0; start < 4096u;) {
        int64_t got = mint_dir_list(dir, start, list);
        if (got <= 0) break;
        for (int64_t k = 0; k < got; ++k)
            if (list[k].type == MINT_MFS_FILE && list[k].name[0] != '.' && list[k].mtime > best_time) {
                best_time = list[k].mtime;
                copy_text(path, sizeof(path), dir); str_append(path, sizeof(path), "/"); str_append(path, sizeof(path), list[k].name);
            }
        start += (uint32_t)got;
        if (got < (int64_t)MINT_MFS_LIST_MAX) break;
    }
    if (!path[0]) return;
    int64_t n = mint_file_load(path, text, sizeof(text) - 1u);
    if (n <= 0) return;
    text[n] = '\0';
    size_t i = 0;
    while (text[i] && notes_line_count < 5u) {
        size_t len = 0;
        while (text[i] && text[i] != '\n') {
            if (len + 1u < sizeof(notes_lines[0])) notes_lines[notes_line_count][len++] = text[i];
            ++i;
        }
        notes_lines[notes_line_count][len] = '\0';
        if (len) ++notes_line_count;
        if (text[i] == '\n') ++i;
    }
}

static void widget_calendar(gfx_rect r, int64_t epoch) {
    tile(r, 16);
    if (!epoch) { ui_label(r.x0 + 14, r.y0 + 26, FONT_CAPTION_BOLD, "CALENDAR", T.accent); ui_label(r.x0 + 14, r.y0 + 48, FONT_CAPTION, "No clock", T.text2); return; }
    int64_t days = epoch / 86400, y; uint32_t m, d; civil(days, &y, &m, &d);
    char head[24]; copy_text(head, sizeof(head), month_names[m - 1u]);
    for (char *p = head; *p; ++p) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    ui_label(r.x0 + 14, r.y0 + 24, FONT_CAPTION_BOLD, head, T.accent);
    static const char *const ini[7] = {"M", "T", "W", "T", "F", "S", "S"};
    int32_t cw = (W(r) - 20) / 7;
    for (int i = 0; i < 7; ++i) ui_label_center(r.x0 + 10 + i * cw + cw / 2, r.y0 + 44, FONT_CAPTION, ini[i], T.text3);
    int64_t first = days_from_civil(y, m, 1);
    uint32_t lead = (weekday(first) + 6u) % 7u;                     /* Monday first */
    uint32_t nd = month_days(y, m);
    for (uint32_t day = 1; day <= nd; ++day) {
        uint32_t cell = lead + day - 1u;
        int32_t cx = r.x0 + 10 + (int32_t)(cell % 7u) * cw + cw / 2, cy = r.y0 + 58 + (int32_t)(cell / 7u) * 17;
        char t[4]; format_u64(t, sizeof(t), day);
        if (day == d) {
            gfx_circle(S, cx, cy - 4, 8, OPAQUE(T.accent));
            ui_label_center(cx, cy, FONT_CAPTION_BOLD, t, 0xFFFFFFu);
        } else ui_label_center(cx, cy, FONT_CAPTION, t, T.text);
    }
}

static void bar_row(gfx_rect r, int32_t y, const char *label, const char *value, uint32_t permille) {
    ui_label(r.x0 + 14, y, FONT_CAPTION, label, T.text);
    ui_label_right(r.x1 - 14, y, FONT_CAPTION, value, T.text2);
    ui_progress(R(r.x0 + 14, y + 5, W(r) - 28, 5), permille, permille > 850u ? 0xFF9500u : T.accent);
}

static void widget_system(gfx_rect r) {
    tile(r, 16);
    ui_label(r.x0 + 14, r.y0 + 24, FONT_CAPTION_BOLD, "SYSTEM", T.text2);
    MINT_SYSTEM_STATUS st;
    char v[40], n[24];
    if (mint_system_status_get(&st) == 0) {
        format_u64(v, sizeof(v), st.cpu_load_permille / 10u); str_append(v, sizeof(v), "%");
        bar_row(r, r.y0 + 48, "CPU", v, st.cpu_load_permille);
        uint64_t used = st.memory_total_bytes - st.memory_free_bytes;
        format_u64(v, sizeof(v), used >> 20); str_append(v, sizeof(v), " / "); format_u64(n, sizeof(n), st.memory_total_bytes >> 20);
        str_append(v, sizeof(v), n); str_append(v, sizeof(v), " MB");
        bar_row(r, r.y0 + 82, "RAM", v, st.memory_total_bytes ? (uint32_t)(used * 1000u / st.memory_total_bytes) : 0u);
    }
    MINT_MFS_VOLUME vol;
    if (mint_fs_get_volume(&vol) == 0 && vol.state == MINT_MFS_VOLUME_READY && vol.total_bytes) {
        format_bytes(v, sizeof(v), vol.free_bytes); str_append(v, sizeof(v), " free");
        bar_row(r, r.y0 + 116, "MintOS HD", v, (uint32_t)((vol.total_bytes - vol.free_bytes) * 1000u / vol.total_bytes));
    } else {
        ui_label(r.x0 + 14, r.y0 + 116, FONT_CAPTION, "MintOS HD", T.text);
        ui_label_right(r.x1 - 14, r.y0 + 116, FONT_CAPTION, "Not set up", T.text3);
    }
    mint_timespec64 up = {0, 0}; (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &up);
    copy_text(v, sizeof(v), "Up "); format_u64(n, sizeof(n), (uint64_t)up.tv_sec / 60u); str_append(v, sizeof(v), n); str_append(v, sizeof(v), " min");
    ui_label(r.x0 + 14, r.y0 + 148, FONT_CAPTION, v, T.text3);
}

static void clock_face(int32_t cx, int32_t cy, int32_t rad, int64_t secs, const char *city, const char *sub) {
    uint32_t hh = (uint32_t)((secs % 86400 + 86400) % 86400 / 3600), mm = (uint32_t)((secs % 3600 + 3600) % 3600 / 60);
    int night = hh < 6u || hh >= 18u;
    uint32_t face = night ? 0x2C2C30u : 0xFFFFFFu, ink = night ? 0xF2F2F4u : 0x1D1D1Fu;
    gfx_circle(S, cx, cy, rad, OPAQUE(face));
    gfx_circle_stroke(S, cx, cy, rad, T.separator);
    for (int32_t k = 0; k < 12; ++k) {
        int32_t a = k * 1024 / 12;
        int32_t tx = cx + wp_sin(a) * (rad - 5) / 16384, ty = cy - wp_sin(a + 256) * (rad - 5) / 16384;
        gfx_fill(S, tx - (k % 3 == 0), ty - (k % 3 == 0), 1 + 2 * (k % 3 == 0), 1 + 2 * (k % 3 == 0), WITH_ALPHA(ink, 0xB0));
    }
    int32_t ma = (int32_t)(mm * 1024u / 60u), ha = (int32_t)(((hh % 12u) * 60u + mm) * 1024u / 720u);
    gfx_line(S, cx, cy, cx + wp_sin(ha) * (rad * 5 / 10) / 16384, cy - wp_sin(ha + 256) * (rad * 5 / 10) / 16384, OPAQUE(ink));
    gfx_line(S, cx + 1, cy, cx + 1 + wp_sin(ha) * (rad * 5 / 10) / 16384, cy - wp_sin(ha + 256) * (rad * 5 / 10) / 16384, OPAQUE(ink));
    gfx_line(S, cx, cy, cx + wp_sin(ma) * (rad * 8 / 10) / 16384, cy - wp_sin(ma + 256) * (rad * 8 / 10) / 16384, OPAQUE(ink));
    gfx_circle(S, cx, cy, 2, OPAQUE(T.accent));
    ui_label_center(cx, cy + rad + 16, FONT_CAPTION_BOLD, city, T.text);
    ui_label_center(cx, cy + rad + 30, FONT_CAPTION, sub, T.text2);
}

static void widget_world_clock(gfx_rect r, int64_t epoch) {
    tile(r, 16);
    static const char *const cities[4] = {"This Mac", "London", "New York", "Tokyo"};
    int32_t cw = W(r) / 4;
    for (uint32_t i = 0; i < 4u; ++i) {
        int64_t off = i ? city_offset(i, epoch) : 0;
        char sub[16];
        if (!i) copy_text(sub, sizeof(sub), "Clock");
        else {
            int64_t h = off / 3600;
            copy_text(sub, sizeof(sub), h >= 0 ? "UTC+" : "UTC\xE2\x88\x92");
            char n[8]; format_u64(n, sizeof(n), (uint64_t)(h >= 0 ? h : -h)); str_append(sub, sizeof(sub), n);
        }
        clock_face(r.x0 + cw * (int32_t)i + cw / 2, r.y0 + 40, 26, epoch + off, cities[i], sub);
    }
}

static void widget_notes(gfx_rect r) {
    tile(r, 16);
    gfx_image(S, r.x0 + 12, r.y0 + 10, 18, 18, app_icon(APP_NOTES, 0), 64, 64, 255u);
    ui_label(r.x0 + 36, r.y0 + 24, FONT_CAPTION_BOLD, "NOTES", T.text2);
    if (!notes_line_count) { ui_label(r.x0 + 14, r.y0 + 52, FONT_CAPTION, "No notes yet. Click to write one.", T.text3); return; }
    gfx_text_fit(S, FONT_BODY_BOLD, r.x0 + 14, r.y0 + 50, notes_lines[0], W(r) - 28, OPAQUE(T.text));
    for (uint32_t i = 1; i < notes_line_count && i < 5u; ++i)
        gfx_text_fit(S, FONT_CAPTION, r.x0 + 14, r.y0 + 52 + (int32_t)i * 16, notes_lines[i], W(r) - 28, OPAQUE(T.text2));
}

static void paint_nc(void) {
    if (!nc_open) return;
    gfx_rect r = nc_rect();
    glass_panel(r, 18);
    gfx_rect saved = gfx_clip_get();
    gfx_clip_set(gfx_rect_intersect(saved, r));
    int64_t epoch = shell_now_epoch();
    char head[48];
    if (epoch) {
        int64_t days = epoch / 86400, y; uint32_t m, d; civil(days, &y, &m, &d);
        copy_text(head, sizeof(head), day_names[weekday(days)]); str_append(head, sizeof(head), ", ");
        char n[8]; format_u64(n, sizeof(n), d); str_append(head, sizeof(head), n); str_append(head, sizeof(head), " ");
        str_append(head, sizeof(head), month_names[m - 1u]);
    } else copy_text(head, sizeof(head), "Notification Center");
    ui_label(r.x0 + 16, r.y0 + 32, FONT_HEADLINE, head, T.text);
    uint32_t count = notify_count();
    uint64_t now = notify_ms();
    if (count) {
        gfx_rect c = nc_clear_rect();
        tile(c, 11);
        ui_label_center(c.x0 + W(c) / 2, BL(c, FONT_CAPTION_BOLD), FONT_CAPTION_BOLD, "Clear", T.text2);
        for (int32_t i = 0; i < nc_notification_rows(); ++i) notification_card(nc_card_rect(i), notify_at((uint32_t)i), now, 0, 0);
        if (count > 4u) {
            char more[40]; format_u64(more, sizeof(more), count - 4u); str_append(more, sizeof(more), " more notifications");
            ui_label_center(r.x0 + NC_W / 2, nc_card_rect(4).y0 + 6, FONT_CAPTION, more, T.text2);
        }
    } else {
        gfx_rect e = R(r.x0 + 12, r.y0 + 50, NC_W - 24, 44);
        tile(e, 12);
        ui_label_center(e.x0 + W(e) / 2, BL(e, FONT_BODY), FONT_BODY, "No Notifications", T.text3);
    }
    int32_t wy = nc_widgets_y();
    widget_calendar(R(r.x0 + 12, wy, 162, 162), epoch);
    widget_system(R(r.x0 + 186, wy, 162, 162));
    widget_world_clock(R(r.x0 + 12, wy + 172, 336, 112), epoch);
    widget_notes(R(r.x0 + 12, wy + 294, 336, 120));
    gfx_clip_set(saved);
}

static int nc_click(int32_t x, int32_t y) {
    gfx_rect r = nc_rect();
    if (!gfx_rect_contains(r, x, y)) { extras_close_overlays(); return 1; }   /* click outside closes it */
    if (notify_count() && gfx_rect_contains(nc_clear_rect(), x, y)) { notify_clear_all(); shell_damage(gfx_rect_xywh(0, 0, SW, SH)); return 1; }
    for (int32_t i = 0; i < nc_notification_rows(); ++i)
        if (gfx_rect_contains(nc_card_rect(i), x, y)) {
            const mde_notification *n = notify_at((uint32_t)i);
            uint32_t app = n->app, id = n->id;
            notify_dismiss(id);
            extras_close_overlays();
            if (app < APP_COUNT) shell_open_app(app);
            return 1;
        }
    int32_t wy = nc_widgets_y();
    if (gfx_rect_contains(R(r.x0 + 12, wy + 294, 336, 120), x, y)) { extras_close_overlays(); shell_open_app(APP_NOTES); return 1; }
    if (gfx_rect_contains(R(r.x0 + 186, wy, 162, 162), x, y)) { extras_close_overlays(); shell_open_app(APP_MONITOR); return 1; }
    return 1;
}

void extras_toggle_notification_center(void) {
    nc_open = !nc_open;
    if (nc_open) {
        load_notes_preview();
        notify_mark_all_read();
        uint32_t ids[NOTIFY_BANNERS_SHOWN];
        uint32_t n = notify_banners(ids, NOTIFY_BANNERS_SHOWN);
        for (uint32_t i = 0; i < n; ++i) notify_hide_banner(ids[i]);
    }
    shell_damage(gfx_rect_xywh(0, 0, SW, SH));
}
int extras_notification_center_open(void) { return nc_open; }
void extras_close_overlays(void) {
    if (nc_open) { nc_open = 0; shell_damage(gfx_rect_xywh(0, 0, SW, SH)); }
}

/* ------------------------------------------------------------ volume HUD */
static gfx_rect hud_rect(void) { return R((SW - 300) / 2, MENU_H + 12, 300, 54); }
void extras_show_volume_hud(void) {
    hud_until = notify_ms() + HUD_MS;
    shell_damage(gfx_rect_intersect(gfx_rect_xywh(0, 0, SW, SH), R(hud_rect().x0 - 60, 0, 420, MENU_H + 140)));
}
static void paint_hud(void) {
    if (!hud_until) return;
    gfx_rect r = hud_rect();
    glass_panel(r, 14);
    uint32_t vol = personality_prefs.volume;
    int muted = personality_prefs.muted || !vol;
    speaker_glyph(r.x0 + 16, r.y0 + 27, vol, muted, OPAQUE(T.text));
    gfx_rect bar = R(r.x0 + 52, r.y0 + 23, W(r) - 104, 8);
    ui_progress(bar, muted ? 0u : vol * 10u, T.dark ? 0xF2F2F4u : 0x3A3A3Cu);
    char t[12]; format_u64(t, sizeof(t), muted ? 0u : vol); str_append(t, sizeof(t), "%");
    ui_label_right(r.x1 - 14, BL(r, FONT_CAPTION_BOLD), FONT_CAPTION_BOLD, t, T.text2);
    if (!sound_output_ready()) ui_label_center(r.x0 + W(r) / 2, r.y1 - 6, FONT_CAPTION, "No audio output", T.text3);
}

/* ------------------------------------------------------------ Control Center */
int32_t extras_cc_sound_height(void) { return 64; }
void extras_cc_sound_paint(gfx_rect r) {
    tile(r, 12);
    ui_label(r.x0 + 14, r.y0 + 22, FONT_BODY_BOLD, "Sound", T.text);
    MINT_AUDIO_INFO ai;
    int ok = sound_info(&ai) && ai.state == MINT_AUDIO_RUNNING;
    if (!ok) ui_label_right(r.x1 - 14, r.y0 + 22, FONT_CAPTION, ai.state == MINT_AUDIO_FOUND ? "Not started" : "No output", T.text3);
    cc_slider = R(r.x0 + 14, r.y0 + 32, W(r) - 28, 22);
    ui_slider(cc_slider, personality_prefs.muted ? 0u : personality_prefs.volume, 100u, 1);
    speaker_glyph(cc_slider.x0 + 6, cc_slider.y0 + 11, personality_prefs.volume, personality_prefs.muted, ARGB(0xB0, 0x60, 0x60, 0x66));
}
int extras_cc_sound_click(gfx_rect r, int32_t x, int32_t y) {
    gfx_rect slider = R(r.x0 + 14, r.y0 + 32, W(r) - 28, 22);
    if (!gfx_rect_contains(gfx_rect_xywh(slider.x0 - 4, slider.y0 - 6, W(slider) + 8, H(slider) + 12), x, y)) return 0;
    cc_slider = slider;
    cc_drag = 1;
    personality_prefs.volume = ui_slider_value(slider, x, 100u);
    personality_prefs.muted = 0u;
    personality_prefs_apply();
    shell_damage(r);
    return 1;
}
void extras_cc_dnd_paint(gfx_rect r) {
    tile(r, 12);
    int on = personality_prefs.do_not_disturb != 0;
    gfx_circle(S, r.x0 + 26, r.y0 + 35, 14, OPAQUE(on ? 0x5E5CE6u : (T.dark ? 0x48484Au : 0xD8D8DCu)));
    moon_glyph(r.x0 + 26, r.y0 + 35, 7, OPAQUE(on ? 0xFFFFFFu : 0x3A3A3Cu));
    ui_label(r.x0 + 48, r.y0 + 32, FONT_BODY_BOLD, "Do Not Disturb", T.text);
    ui_label(r.x0 + 48, r.y0 + 49, FONT_CAPTION, on ? "On" : "Off", T.text2);
}

/* ------------------------------------------------------------ screenshots */
static void two(char *out, size_t cap, uint32_t v) { char t[3] = {(char)('0' + v / 10u % 10u), (char)('0' + v % 10u), 0}; str_append(out, cap, t); }
int extras_screenshot(void) {
    MINT_MFS_VOLUME vol;
    if (mint_fs_get_volume(&vol) != 0 || vol.state != MINT_MFS_VOLUME_READY) {
        ui_sound(UI_SOUND_ALERT);
        notify_post(APP_FILES, "Screenshot not saved", "Screenshots are saved on MintOS HD. Set one up in Disk Utility.");
        return 0;
    }
    static char dir[160], name[96], path[260], tmp[260];
    copy_text(dir, sizeof(dir), vol.home); str_append(dir, sizeof(dir), "/Desktop");
    (void)mint_dir_create(dir);
    int64_t e = shell_now_epoch();
    copy_text(name, sizeof(name), "Screenshot ");
    if (e) {
        int64_t y; uint32_t m, d; civil(e / 86400, &y, &m, &d);
        char n[8]; format_u64(n, sizeof(n), (uint64_t)y); str_append(name, sizeof(name), n); str_append(name, sizeof(name), "-");
        two(name, sizeof(name), m); str_append(name, sizeof(name), "-"); two(name, sizeof(name), d);
        uint32_t s = (uint32_t)(e % 86400);
        str_append(name, sizeof(name), " at "); two(name, sizeof(name), s / 3600u); str_append(name, sizeof(name), ".");
        two(name, sizeof(name), s / 60u % 60u); str_append(name, sizeof(name), "."); two(name, sizeof(name), s % 60u);
    } else {
        char n[24]; format_u64(n, sizeof(n), notify_ms()); str_append(name, sizeof(name), n);
    }
    str_append(name, sizeof(name), ".bmp");
    copy_text(path, sizeof(path), dir); str_append(path, sizeof(path), "/"); str_append(path, sizeof(path), name);
    copy_text(tmp, sizeof(tmp), dir); str_append(tmp, sizeof(tmp), "/.~"); str_append(tmp, sizeof(tmp), name);
    int64_t fd = mint_file_open(tmp, MINT_O_WRITE | MINT_O_CREATE | MINT_O_TRUNC);
    if (fd < 0) { ui_sound(UI_SOUND_ALERT); notify_post(APP_FILES, "Screenshot not saved", "The Desktop folder could not be written."); return 0; }
    /* 24-bit BMP, rows bottom-up (each 1920 * 3 bytes is already 4-byte aligned). */
    static uint8_t buf[65536];
    uint32_t w = (uint32_t)gfx_screen.w, h = (uint32_t)gfx_screen.h, row = (w * 3u + 3u) & ~3u;
    uint32_t image = row * h, file = 54u + image;
    for (uint32_t i = 0; i < 54u; ++i) buf[i] = 0;
    buf[0] = 'B'; buf[1] = 'M';
    for (int i = 0; i < 4; ++i) { buf[2 + i] = (uint8_t)(file >> (8 * i)); buf[18 + i] = (uint8_t)(w >> (8 * i)); buf[22 + i] = (uint8_t)(h >> (8 * i)); buf[34 + i] = (uint8_t)(image >> (8 * i)); }
    buf[10] = 54; buf[14] = 40; buf[26] = 1; buf[28] = 24;
    buf[38] = 0x13; buf[39] = 0x0B; buf[42] = 0x13; buf[43] = 0x0B;           /* 72 dpi */
    uint32_t used = 54u;
    int64_t rc = 0;
    for (uint32_t y = h; y-- > 0 && rc >= 0;) {
        if (used + row > sizeof(buf)) { rc = mint_file_write(fd, buf, used); used = 0; if (rc < 0) break; }
        const uint32_t *src = gfx_screen.px + (uint64_t)y * (uint32_t)gfx_screen.stride;
        uint8_t *dst = buf + used;
        for (uint32_t x = 0; x < w; ++x) { uint32_t p = src[x]; dst[x * 3u] = (uint8_t)p; dst[x * 3u + 1u] = (uint8_t)(p >> 8); dst[x * 3u + 2u] = (uint8_t)(p >> 16); }
        for (uint32_t x = w * 3u; x < row; ++x) dst[x] = 0;
        used += row;
    }
    if (rc >= 0 && used) rc = mint_file_write(fd, buf, used);
    (void)mint_file_close(fd);
    if (rc >= 0) rc = mint_path_rename(tmp, path);
    if (rc < 0) {
        (void)mint_path_remove(tmp);
        ui_sound(UI_SOUND_ALERT);
        notify_post(APP_FILES, "Screenshot not saved", "MintOS HD may be full.");
        return 0;
    }
    ui_sound(UI_SOUND_SCREENSHOT);
    char body[160]; copy_text(body, sizeof(body), "Saved to the Desktop as \xE2\x80\x9C"); str_append(body, sizeof(body), name); str_append(body, sizeof(body), "\xE2\x80\x9D.");
    notify_post(APP_FILES, "Screenshot", body);
    desktop_items_refresh(1);
    shell_damage(desktop_items_bounds());
    return 1;
}

/* ------------------------------------------------------------ hooks */
void extras_init(int32_t screen_w, int32_t screen_h) {
    SW = screen_w; SH = screen_h;
    notify_queue_reset();
    desktop_items_refresh(1);
}

void extras_paint_desktop(gfx_rect clip) {
    if (personality_prefs.desktop_items) desktop_items_paint(clip);
}

void extras_paint_overlays(void) {
    paint_nc();
    paint_banners();
    paint_hud();
}

uint32_t extras_glass_rects(gfx_rect *out, uint32_t max) {
    uint32_t n = 0;
    if (nc_open && n < max) out[n++] = nc_rect();
    if (!nc_open) {
        uint32_t ids[NOTIFY_BANNERS_SHOWN];
        uint32_t b = notify_banners(ids, NOTIFY_BANNERS_SHOWN);
        uint64_t now = notify_ms();
        for (uint32_t i = 0; i < b && n < max; ++i) {
            const mde_notification *note = notify_find(ids[i]);
            if (note) out[n++] = banner_rect(i, note->posted_ms, now);
        }
    }
    if (hud_until && n < max) out[n++] = hud_rect();
    return n;
}

int extras_pointer_pressed(int32_t x, int32_t y, uint32_t clicks, int desktop_hit) {
    if (nc_open) return nc_click(x, y);
    uint32_t ids[NOTIFY_BANNERS_SHOWN];
    uint32_t b = notify_banners(ids, NOTIFY_BANNERS_SHOWN);
    uint64_t now = notify_ms();
    for (uint32_t i = 0; i < b; ++i) {
        const mde_notification *note = notify_find(ids[i]);
        if (!note) continue;
        gfx_rect r = banner_rect(i, note->posted_ms, now);
        if (!gfx_rect_contains(gfx_rect_xywh(r.x0 - 10, r.y0 - 10, W(r) + 10, H(r) + 10), x, y)) continue;
        uint32_t app = note->app, id = note->id;
        int close = x < r.x0 + 14 && y < r.y0 + 14;
        notify_hide_banner(id);
        shell_damage(R(SW - BANNER_W - 60, MENU_H, BANNER_W + 60, (int32_t)NOTIFY_BANNERS_SHOWN * (BANNER_H + 8) + 60));
        if (!close) {
            notify_dismiss(id);
            if (app < APP_COUNT) shell_open_app(app);
        }
        return 1;
    }
    if (desktop_hit && personality_prefs.desktop_items) return desktop_items_click(x, y, clicks);
    return 0;
}

int extras_pointer_moved(int32_t x, int32_t y) {
    if (cc_drag) {
        uint32_t v = ui_slider_value(cc_slider, x, 100u);
        if (v != personality_prefs.volume || personality_prefs.muted) {
            personality_prefs.volume = v; personality_prefs.muted = 0u;
            personality_prefs_apply();
            shell_damage(gfx_rect_xywh(cc_slider.x0 - 30, cc_slider.y0 - 30, W(cc_slider) + 60, 80));
        }
        return 1;
    }
    int32_t hb = -1;
    if (!nc_open) {
        uint32_t ids[NOTIFY_BANNERS_SHOWN];
        uint32_t b = notify_banners(ids, NOTIFY_BANNERS_SHOWN);
        uint64_t now = notify_ms();
        for (uint32_t i = 0; i < b; ++i) {
            const mde_notification *note = notify_find(ids[i]);
            if (note && gfx_rect_contains(banner_rect(i, note->posted_ms, now), x, y)) { hb = (int32_t)i; hover_banner_id = ids[i]; }
        }
    }
    if (hb >= 0) notify_hold_banner(hover_banner_id, notify_ms());
    if (hb != hover_banner) {
        hover_banner = hb;
        shell_damage(R(SW - BANNER_W - 60, MENU_H, BANNER_W + 60, (int32_t)NOTIFY_BANNERS_SHOWN * (BANNER_H + 8) + 60));
    }
    return 0;
}

void extras_pointer_released(void) {
    if (cc_drag) {
        cc_drag = 0;
        if (personality_prefs.volume_feedback && !personality_prefs.muted) (void)mint_audio_play(MINT_SOUND_VOLUME, 1000u);
    }
}

int extras_key(const MINT_DESKTOP_KEY_EVENT *k, int window_focused) {
    uint16_t sc = k->scancode;
    if (sc == 0x120u) { sound_mute_toggle(); return 1; }                 /* Mute */
    if (sc == 0x12Eu) { sound_volume_step(-6); return 1; }               /* Volume down */
    if (sc == 0x130u) { sound_volume_step(6); return 1; }                /* Volume up */
    int cmd = (k->modifiers & MINT_KEYMOD_GUI) != 0, shift = (k->modifiers & MINT_KEYMOD_SHIFT) != 0;
    if (cmd && shift && (k->codepoint == '3' || k->codepoint == '#' || sc == 0x04u)) { (void)extras_screenshot(); return 1; }
    if (k->codepoint == 27u && nc_open) { extras_close_overlays(); return 1; }
    if (!window_focused && personality_prefs.desktop_items && desktop_items_key(k)) return 1;
    return 0;
}

void extras_tick(void) {
    uint64_t now = notify_ms();
    gfx_rect column = R(SW - BANNER_W - 60, MENU_H, BANNER_W + 60, (int32_t)NOTIFY_BANNERS_SHOWN * (BANNER_H + 8) + 60);
    if (notify_expire_banners(now)) shell_damage(column);
    uint32_t ids[NOTIFY_BANNERS_SHOWN];
    uint32_t b = notify_banners(ids, NOTIFY_BANNERS_SHOWN);
    for (uint32_t i = 0; i < b; ++i) {
        const mde_notification *note = notify_find(ids[i]);
        if (note && now - note->posted_ms < BANNER_SLIDE_MS + 40u) { shell_damage(column); break; }
    }
    if (hud_until && now >= hud_until) {
        hud_until = 0;
        shell_damage(R(hud_rect().x0 - 60, 0, 420, MENU_H + 140));
    }
    if (personality_wallpaper_tick()) shell_damage(gfx_rect_xywh(0, 0, SW, SH));
    if (personality_prefs.desktop_items && desktop_items_poll(now)) shell_damage(desktop_items_bounds());
    int64_t minute = shell_now_epoch() / 60;
    if (nc_open && minute != nc_minute) { nc_minute = minute; shell_damage(nc_rect()); }
}
