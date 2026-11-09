/* MintDE 2 controls and theme (macOS-style look, original implementation). */
#include "mintde.h"

mde_theme T;

/* ------------------------------------------------------------ strings */
size_t text_length(const char *s) { size_t n = 0; if (s) while (s[n] && n < 4096u) ++n; return n; }
size_t copy_text(char *out, size_t capacity, const char *in) {
    size_t n = 0; if (!out || !capacity) return 0;
    while (in && in[n] && n + 1u < capacity) { out[n] = in[n]; ++n; }
    out[n] = '\0'; return n;
}
void str_append(char *dst, size_t capacity, const char *src) {
    if (!dst || !capacity || !src) return;
    size_t n = text_length(dst), i = 0;
    while (src[i] && n + 1u < capacity) dst[n++] = src[i++];
    dst[n] = '\0';
}
void format_u64(char *out, size_t capacity, uint64_t value) {
    if (!out || capacity < 2u) return;
    char rev[24]; size_t n = 0, k = 0;
    do { rev[n++] = (char)('0' + value % 10u); value /= 10u; } while (value && n < sizeof(rev));
    while (n && k + 1u < capacity) out[k++] = rev[--n];
    out[k] = '\0';
}
void format_hex64(char *out, size_t capacity, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    if (!out || capacity < 4u) return;
    char rev[16]; size_t n = 0, k = 0;
    do { rev[n++] = digits[value & 15u]; value >>= 4; } while (value && n < sizeof(rev));
    out[k++] = '0'; out[k++] = 'x';
    while (n && k + 1u < capacity) out[k++] = rev[--n];
    out[k] = '\0';
}
/* "512 MB", "1.5 GB" — decimal units like macOS Finder. */
void format_bytes(char *out, size_t capacity, uint64_t bytes) {
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB"};
    uint64_t whole = bytes, frac = 0; uint32_t u = 0;
    while (whole >= 1000u && u < 4u) { frac = (whole % 1000u) / 100u; whole /= 1000u; ++u; }
    char num[24]; format_u64(num, sizeof(num), whole);
    copy_text(out, capacity, num);
    if (u && whole < 100u && frac) { char f[4] = {'.', (char)('0' + frac), 0, 0}; str_append(out, capacity, f); }
    str_append(out, capacity, " ");
    str_append(out, capacity, (u == 0 && bytes == 1u) ? "byte" : units[u]);
}

/* ------------------------------------------------------------ theme */
void theme_apply(const MINT_DESKTOP_SETTINGS *s, int night) {
    int dark = s->theme == MINT_THEME_DARK || (s->theme == MINT_THEME_WARM && night);
    static const uint32_t accents_light[5] = {0x00B894u, 0x007AFFu, 0xAF52DEu, 0xFF9500u, 0xFF2D55u};
    static const uint32_t accents_dark[5] = {0x00D1A7u, 0x0A84FFu, 0xBF5AF2u, 0xFF9F0Au, 0xFF375Fu};
    uint32_t a = s->accent <= MINT_ACCENT_ROSE ? s->accent : 0u;
    T.dark = dark;
    T.accent = dark ? accents_dark[a] : accents_light[a];
    T.accent_text = 0xFFFFFFu;
    if (!dark) {
        T.window = 0xF6F6F6u; T.content = 0xFFFFFFu; T.sidebar = 0xEAEAEDu;
        T.group = 0xFFFFFFu; T.group_bg = 0xF2F2F5u; T.field = 0xFFFFFFu; T.control = 0xFFFFFFu;
        T.text = 0x1D1D1Fu; T.text2 = 0x6E6E73u; T.text3 = 0xA8A8ADu;
        T.separator = ARGB(0x1E, 0, 0, 0); T.control_border = ARGB(0x2A, 0, 0, 0);
        T.row_alt = 0xF4F5F6u; T.selection_inactive = 0xDCDCE0u;
        T.menu_tint = ARGB(0xB8, 0xF4, 0xF4, 0xF6);
    } else {
        T.window = 0x2A2A2Cu; T.content = 0x1E1E20u; T.sidebar = 0x252527u;
        T.group = 0x2C2C2Eu; T.group_bg = 0x1E1E20u; T.field = 0x1C1C1Eu; T.control = 0x3A3A3Cu;
        T.text = 0xF2F2F7u; T.text2 = 0x9A9AA0u; T.text3 = 0x66666Bu;
        T.separator = ARGB(0x26, 0xFF, 0xFF, 0xFF); T.control_border = ARGB(0x22, 0xFF, 0xFF, 0xFF);
        T.row_alt = 0x232325u; T.selection_inactive = 0x46464Au;
        T.menu_tint = ARGB(0xB0, 0x24, 0x24, 0x28);
    }
}

/* ------------------------------------------------------------ labels */
void ui_label(int32_t x, int32_t baseline, int font, const char *text, uint32_t rgb) {
    (void)gfx_text(&gfx_screen, font, x, baseline, text, OPAQUE(rgb));
}
void ui_label_right(int32_t right, int32_t baseline, int font, const char *text, uint32_t rgb) {
    (void)gfx_text(&gfx_screen, font, right - gfx_text_width(font, text), baseline, text, OPAQUE(rgb));
}
void ui_label_center(int32_t cx, int32_t baseline, int font, const char *text, uint32_t rgb) {
    (void)gfx_text(&gfx_screen, font, cx - gfx_text_width(font, text) / 2, baseline, text, OPAQUE(rgb));
}
static int32_t baseline_in(gfx_rect r, int font) {
    int32_t asc = gfx_font_ascent(font), line = gfx_font_line(font);
    return r.y0 + (r.y1 - r.y0 - line) / 2 + asc;
}

/* ------------------------------------------------------------ symbols */
void ui_symbol_glyph(int32_t cx, int32_t cy, uint32_t sym, uint32_t c) {
    gfx_surface *s = &gfx_screen;
    switch (sym) {
    case SYM_CHEVRON_LEFT:
        gfx_line(s, cx + 3, cy - 6, cx - 3, cy, c); gfx_line(s, cx - 3, cy, cx + 3, cy + 6, c);
        gfx_line(s, cx + 4, cy - 6, cx - 2, cy, c); gfx_line(s, cx - 2, cy, cx + 4, cy + 6, c); break;
    case SYM_CHEVRON_RIGHT:
        gfx_line(s, cx - 3, cy - 6, cx + 3, cy, c); gfx_line(s, cx + 3, cy, cx - 3, cy + 6, c);
        gfx_line(s, cx - 4, cy - 6, cx + 2, cy, c); gfx_line(s, cx + 2, cy, cx - 4, cy + 6, c); break;
    case SYM_SEARCH:
        gfx_circle_stroke(s, cx - 2, cy - 2, 5, c); gfx_circle_stroke(s, cx - 2, cy - 2, 4, c);
        gfx_line(s, cx + 2, cy + 2, cx + 6, cy + 6, c); gfx_line(s, cx + 3, cy + 2, cx + 6, cy + 5, c); break;
    case SYM_FOLDER:
        gfx_round_rect(s, cx - 8, cy - 6, 7, 4, 1, c);
        gfx_round_rect(s, cx - 8, cy - 4, 16, 11, 2, c); break;
    case SYM_DISK:
        gfx_round_rect(s, cx - 8, cy - 5, 16, 11, 2, c);
        gfx_circle(s, cx + 5, cy + 2, 1, OPAQUE(0xFFFFFFu)); break;
    case SYM_GEAR:
        gfx_circle(s, cx, cy, 7, c);
        gfx_circle(s, cx, cy, 3, OPAQUE(0xFFFFFFu)); break;
    case SYM_TOGGLES:
        gfx_round_rect_stroke(s, cx - 8, cy - 7, 16, 6, 3, c); gfx_circle(s, cx - 5, cy - 4, 2, c);
        gfx_round_rect_stroke(s, cx - 8, cy + 1, 16, 6, 3, c); gfx_circle(s, cx + 5, cy + 4, 2, c); break;
    case SYM_PLUS: gfx_fill(s, cx - 5, cy - 1, 11, 2, c); gfx_fill(s, cx - 1, cy - 5, 2, 11, c); break;
    case SYM_MINUS: gfx_fill(s, cx - 5, cy - 1, 11, 2, c); break;
    case SYM_INFO:
        gfx_circle_stroke(s, cx, cy, 7, c); gfx_fill(s, cx - 1, cy - 1, 2, 5, c); gfx_fill(s, cx - 1, cy - 4, 2, 2, c); break;
    case SYM_WARNING:
        gfx_line(s, cx, cy - 7, cx - 7, cy + 6, c); gfx_line(s, cx, cy - 7, cx + 7, cy + 6, c);
        gfx_hline(s, cx - 7, cy + 6, 15, c); gfx_fill(s, cx - 1, cy - 2, 2, 5, c); gfx_fill(s, cx - 1, cy + 4, 2, 1, c); break;
    case SYM_CHECK:
        gfx_line(s, cx - 5, cy, cx - 1, cy + 4, c); gfx_line(s, cx - 1, cy + 4, cx + 6, cy - 4, c);
        gfx_line(s, cx - 5, cy + 1, cx - 1, cy + 5, c); gfx_line(s, cx - 1, cy + 5, cx + 6, cy - 3, c); break;
    case SYM_DOC:
        gfx_round_rect_stroke(s, cx - 6, cy - 8, 12, 16, 2, c);
        gfx_hline(s, cx - 3, cy - 3, 6, c); gfx_hline(s, cx - 3, cy, 6, c); gfx_hline(s, cx - 3, cy + 3, 4, c); break;
    default: break;
    }
}

/* ------------------------------------------------------------ controls */
void ui_button(gfx_rect r, const char *label, int primary, int enabled) {
    int32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    gfx_surface *s = &gfx_screen;
    if (primary && enabled) {
        gfx_shadow(s, r.x0, r.y0, w, h, 6, 2, 1, ARGB(0x30, 0, 0, 0));
        gfx_round_rect_grad(s, r.x0, r.y0, w, h, 6, OPAQUE(gfx_lerp_rgb(T.accent, 0xFFFFFFu, 30)), OPAQUE(T.accent));
        ui_label_center(r.x0 + w / 2, baseline_in(r, FONT_BODY), FONT_BODY, label, 0xFFFFFFu);
        return;
    }
    gfx_shadow(s, r.x0, r.y0, w, h, 6, 2, 1, ARGB(T.dark ? 0x40 : 0x18, 0, 0, 0));
    gfx_round_rect(s, r.x0, r.y0, w, h, 6, OPAQUE(T.control));
    gfx_round_rect_stroke(s, r.x0, r.y0, w, h, 6, T.control_border);
    ui_label_center(r.x0 + w / 2, baseline_in(r, FONT_BODY), FONT_BODY, label, enabled ? T.text : T.text3);
}

void ui_popup(gfx_rect r, const char *value, int enabled) {
    ui_button(r, "", 0, enabled);
    int32_t h = r.y1 - r.y0;
    ui_label(r.x0 + 10, baseline_in(r, FONT_BODY), FONT_BODY, value, enabled ? T.text : T.text3);
    int32_t cx = r.x1 - 13, cy = r.y0 + h / 2;
    gfx_round_rect(&gfx_screen, r.x1 - 22, r.y0 + 3, 18, h - 6, 4, OPAQUE(T.accent));
    uint32_t w = OPAQUE(0xFFFFFFu);
    gfx_line(&gfx_screen, cx - 3, cy - 2, cx, cy - 5, w); gfx_line(&gfx_screen, cx, cy - 5, cx + 3, cy - 2, w);
    gfx_line(&gfx_screen, cx - 3, cy + 2, cx, cy + 5, w); gfx_line(&gfx_screen, cx, cy + 5, cx + 3, cy + 2, w);
}

void ui_switch(int32_t x, int32_t y, int on) {
    gfx_surface *s = &gfx_screen;
    uint32_t track = on ? T.accent : (T.dark ? 0x3E3E42u : 0xE3E3E8u);
    gfx_round_rect(s, x, y, 38, 22, 11, OPAQUE(track));
    if (!on) gfx_round_rect_stroke(s, x, y, 38, 22, 11, T.control_border);
    int32_t kx = on ? x + 27 : x + 11;
    gfx_shadow(s, kx - 10, y + 1, 20, 20, 10, 3, 1, ARGB(0x40, 0, 0, 0));
    gfx_circle(s, kx, y + 11, 10, OPAQUE(0xFFFFFFu));
}

void ui_segmented(gfx_rect r, const char *const *labels, uint32_t count, uint32_t selected) {
    if (!count) return;
    gfx_surface *s = &gfx_screen;
    int32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    gfx_round_rect(s, r.x0, r.y0, w, h, 7, OPAQUE(T.dark ? 0x3A3A3Cu : 0xE3E3E8u));
    int32_t seg = w / (int32_t)count;
    for (uint32_t i = 0; i < count; ++i) {
        gfx_rect sr = gfx_rect_xywh(r.x0 + (int32_t)i * seg, r.y0, seg, h);
        if (i == selected) {
            gfx_shadow(s, sr.x0 + 2, sr.y0 + 2, seg - 4, h - 4, 5, 2, 1, ARGB(0x30, 0, 0, 0));
            gfx_round_rect(s, sr.x0 + 2, sr.y0 + 2, seg - 4, h - 4, 5, OPAQUE(T.dark ? 0x636366u : 0xFFFFFFu));
        } else if (i + 1u != selected && i + 1u < count) {
            gfx_vline(s, sr.x1, r.y0 + 6, h - 12, T.separator);
        }
        ui_label_center(sr.x0 + seg / 2, baseline_in(sr, i == selected ? FONT_BODY_MEDIUM : FONT_BODY),
                        i == selected ? FONT_BODY_MEDIUM : FONT_BODY, labels[i], T.text);
    }
}
int32_t ui_segment_at(gfx_rect r, uint32_t count, int32_t x) {
    if (!count || x < r.x0 || x >= r.x1) return -1;
    int32_t seg = (r.x1 - r.x0) / (int32_t)count;
    int32_t i = (x - r.x0) / (seg ? seg : 1);
    return i < (int32_t)count ? i : (int32_t)count - 1;
}

void ui_search_field(gfx_rect r, const char *text, int focused) {
    gfx_surface *s = &gfx_screen;
    int32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (focused) gfx_round_rect(s, r.x0 - 3, r.y0 - 3, w + 6, h + 6, 10, WITH_ALPHA(T.accent, 0x60));
    gfx_round_rect(s, r.x0, r.y0, w, h, 7, T.dark ? ARGB(0x30, 0xFF, 0xFF, 0xFF) : ARGB(0x12, 0, 0, 0));
    ui_symbol_glyph(r.x0 + 14, r.y0 + h / 2, SYM_SEARCH, OPAQUE(T.text2));
    int has = text && text[0];
    gfx_text_fit(s, FONT_BODY, r.x0 + 26, baseline_in(r, FONT_BODY), has ? text : "Search", w - 34,
                 OPAQUE(has ? T.text : T.text2));
    if (focused) {
        int32_t cx = r.x0 + 26 + (has ? gfx_text_width(FONT_BODY, text) : 0) + 1;
        gfx_fill(s, cx, r.y0 + 5, 1, h - 10, OPAQUE(T.accent));
    }
}

void ui_text_field(gfx_rect r, const char *text, const char *placeholder, int focused) {
    gfx_surface *s = &gfx_screen;
    int32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (focused) gfx_round_rect(s, r.x0 - 3, r.y0 - 3, w + 6, h + 6, 8, WITH_ALPHA(T.accent, 0x70));
    gfx_round_rect(s, r.x0, r.y0, w, h, 5, OPAQUE(T.field));
    gfx_round_rect_stroke(s, r.x0, r.y0, w, h, 5, T.control_border);
    int has = text && text[0];
    gfx_text_fit(s, FONT_BODY, r.x0 + 8, baseline_in(r, FONT_BODY), has ? text : (placeholder ? placeholder : ""),
                 w - 16, OPAQUE(has ? T.text : T.text3));
    if (focused) {
        int32_t cx = r.x0 + 8 + (has ? gfx_text_width(FONT_BODY, text) : 0) + 1;
        gfx_fill(s, cx, r.y0 + 4, 1, h - 8, OPAQUE(T.text));
    }
}

void ui_group(gfx_rect r) {
    gfx_round_rect(&gfx_screen, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, 10, OPAQUE(T.group));
    gfx_round_rect_stroke(&gfx_screen, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, 10, T.separator);
}

void ui_group_row(gfx_rect r, uint32_t index, uint32_t count, const char *label, const char *value) {
    if (index > 0u) gfx_hline(&gfx_screen, r.x0 + 12, r.y0, r.x1 - r.x0 - 24, T.separator);
    (void)count;
    if (label) ui_label(r.x0 + 14, baseline_in(r, FONT_BODY), FONT_BODY, label, T.text);
    if (value) {
        int32_t room = (r.x1 - r.x0) - 28 - (label ? gfx_text_width(FONT_BODY, label) + 24 : 0);
        int32_t vw = gfx_text_width(FONT_BODY, value);
        if (vw > room) vw = room;
        gfx_text_fit(&gfx_screen, FONT_BODY, r.x1 - 14 - vw, baseline_in(r, FONT_BODY), value, vw, OPAQUE(T.text2));
    }
}

void ui_sidebar_header(int32_t x, int32_t y, const char *label) {
    ui_label(x, y, FONT_CAPTION_BOLD, label, T.text3);
}

void ui_sidebar_item(gfx_rect r, const char *label, const uint8_t *icon64, uint32_t color, int selected, int focused) {
    gfx_surface *s = &gfx_screen;
    int32_t h = r.y1 - r.y0;
    if (selected)
        gfx_round_rect(s, r.x0, r.y0, r.x1 - r.x0, h, 6,
                       focused ? OPAQUE(T.accent) : (T.dark ? ARGB(0x30, 0xFF, 0xFF, 0xFF) : ARGB(0x1C, 0, 0, 0)));
    int on_accent = selected && focused;
    int32_t ix = r.x0 + 8, iy = r.y0 + (h - 20) / 2;
    if (icon64) gfx_image(s, ix - 1, iy - 1, 22, 22, icon64, 64, 64, 255u);
    else if (color) {
        gfx_round_rect(s, ix, iy, 20, 20, 5, OPAQUE(color));
    }
    ui_label(r.x0 + 36, baseline_in(r, FONT_BODY), FONT_BODY, label, on_accent ? 0xFFFFFFu : T.text);
}

void ui_list_row(gfx_rect r, uint32_t index, int selected, int focused) {
    uint32_t bg = selected ? (focused ? T.accent : T.selection_inactive) : ((index & 1u) ? T.row_alt : T.content);
    gfx_fill(&gfx_screen, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, OPAQUE(bg));
}

void ui_progress(gfx_rect r, uint32_t permille, uint32_t color) {
    int32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (permille > 1000u) permille = 1000u;
    gfx_round_rect(&gfx_screen, r.x0, r.y0, w, h, h / 2, OPAQUE(T.dark ? 0x3A3A3Cu : 0xE5E5EAu));
    int32_t fw = (int32_t)((int64_t)w * permille / 1000);
    if (fw > 0) gfx_round_rect(&gfx_screen, r.x0, r.y0, fw < h ? h : fw, h, h / 2, OPAQUE(color));
}

void ui_badge(int32_t x, int32_t y, const char *text, uint32_t color) {
    int32_t w = gfx_text_width(FONT_CAPTION_BOLD, text) + 12;
    gfx_round_rect(&gfx_screen, x, y, w, 18, 9, WITH_ALPHA(color, T.dark ? 0x50 : 0x2A));
    ui_label(x + 6, y + 13, FONT_CAPTION_BOLD, text, T.dark ? gfx_lerp_rgb(color, 0xFFFFFFu, 120) : gfx_lerp_rgb(color, 0, 60));
}
