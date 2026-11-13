/* MintDE 2.3.0 — native ring-3 desktop for MintOS26 26.5.013.0.
 *
 * A macOS-style shell rendered entirely in user space into the kernel-shared
 * canvas: window manager (z-order, focus, traffic lights, drag, resize, zoom,
 * minimize), translucent menu bar with menus and clock, glass Dock,
 * Launchpad, Spotlight, Control Center, Command-Tab switcher and the
 * Shut Down / Restart alert. Only damaged rectangles are recomposed and
 * presented; the pointer is a kernel overlay so moving it never repaints.
 * Applications live in mintde/apps.c, controls in mintde/widgets.c. */
#include "mintde/mintde.h"

MINT_DESKTOP_SETTINGS desktop_settings;
MINT_DISPLAY_CANVAS canvas;
int32_t pointer_x, pointer_y;

#define S (&gfx_screen)
#define MENUBAR_H 26
#define WIN_R 10
#define DOCK_GAP 6
#define DOCK_PAD 7
#define DOCK_BLUR 18
#define MENU_BLUR 22
#define SIDEBAR_BLUR 26          /* window sidebars: macOS-style vibrancy */

static int32_t SW, SH;
static mde_window windows[APP_COUNT];
static uint32_t zorder[APP_COUNT];
static uint32_t zcount;
static int32_t focused_app = -1;
static uint32_t cascade;
static gfx_rect damage;

/* interaction state */
static int32_t drag_app = -1, drag_dx, drag_dy;
static int32_t resize_app = -1, resize_edges, resize_x0, resize_y0, resize_w0, resize_h0;
static int32_t hover_lights = -1, hover_dock = -1;
static int32_t active_menu = -1;
static int32_t hovered_menu_item = -1;
static int launchpad_open, spotlight_open, control_center_open, switcher_open;
static uint32_t switcher_index;
static char spotlight_query[40];
static int32_t spotlight_sel;
static uint32_t power_confirm;           /* 0, MINT_POWER_ACTION_*, or POWER_FAILED */
#define POWER_FAILED 3u
static uint64_t last_click_ms;
static int32_t last_click_x, last_click_y;
static int64_t last_clock_minute = -1;
static int night_mode;
static uint32_t cursor_shape = MINT_CURSOR_SHAPE_ARROW;

static gfx_rect R(int32_t x, int32_t y, int32_t w, int32_t h) { return gfx_rect_xywh(x, y, w, h); }
static int32_t W(gfx_rect r) { return r.x1 - r.x0; }
static int32_t H(gfx_rect r) { return r.y1 - r.y0; }
static gfx_rect grow(gfx_rect r, int32_t l, int32_t t, int32_t rr, int32_t b) {
    gfx_rect o = {r.x0 - l, r.y0 - t, r.x1 + rr, r.y1 + b}; return o;
}
static int32_t BL(gfx_rect r, int font) { return r.y0 + (H(r) - gfx_font_line(font)) / 2 + gfx_font_ascent(font); }

/* ------------------------------------------------------------ damage */
void shell_damage(gfx_rect r) { damage = gfx_rect_union(damage, r); }
static void damage_all(void) { damage = R(0, 0, SW, SH); }

static gfx_rect window_frame(uint32_t app) {
    mde_window *w = &windows[app];
    return R(w->x, w->y, w->w, w->h);
}
static gfx_rect window_bounds(uint32_t app) { return grow(window_frame(app), 42, 28, 42, 62); } /* incl. shadow */
void shell_damage_window(uint32_t app) { if (app < APP_COUNT && windows[app].open) shell_damage(window_bounds(app)); }

/* ------------------------------------------------------------ clock */
int64_t shell_now_epoch(void) {
    mint_timespec64 ts = {0, 0};
    return mint_clock_gettime(MINT_CLOCK_REALTIME, &ts) == 0 ? ts.tv_sec : 0;
}
static void civil_from_days(int64_t z, int64_t *y, uint32_t *m, uint32_t *d) {
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
void shell_format_clock(char *out, size_t cap, int64_t epoch, int with_seconds) {
    static const char *const wd[7] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
    static const char *const mo[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (!epoch) {
        mint_timespec64 up = {0, 0}; (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &up);
        char n[24]; copy_text(out, cap, "Up ");
        format_u64(n, sizeof(n), (uint64_t)up.tv_sec / 60u); str_append(out, cap, n); str_append(out, cap, " min");
        return;
    }
    int64_t days = epoch / 86400, secs = epoch % 86400;
    int64_t y; uint32_t m, d; civil_from_days(days, &y, &m, &d);
    char n[24];
    copy_text(out, cap, wd[(uint32_t)(days % 7)]); str_append(out, cap, " ");
    format_u64(n, sizeof(n), d); str_append(out, cap, n); str_append(out, cap, " ");
    str_append(out, cap, mo[m - 1]); str_append(out, cap, "  ");
    uint32_t hh = (uint32_t)(secs / 3600), mm = (uint32_t)((secs / 60) % 60), ss = (uint32_t)(secs % 60);
    char t[12] = {(char)('0' + hh / 10), (char)('0' + hh % 10), ':', (char)('0' + mm / 10), (char)('0' + mm % 10), 0, 0, 0, 0};
    if (with_seconds) { t[5] = ':'; t[6] = (char)('0' + ss / 10); t[7] = (char)('0' + ss % 10); }
    str_append(out, cap, t);
}
void shell_format_date(char *out, size_t cap, int64_t epoch) {
    static const char *const mo[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (epoch <= 0) { copy_text(out, cap, "--"); return; }
    int64_t now = shell_now_epoch(), days = epoch / 86400, secs = epoch % 86400;
    if (now && days == now / 86400) copy_text(out, cap, "Today at ");
    else if (now && days == now / 86400 - 1) copy_text(out, cap, "Yesterday at ");
    else {
        int64_t y; uint32_t m, d; civil_from_days(days, &y, &m, &d);
        char n[24]; format_u64(out, cap, d); str_append(out, cap, " "); str_append(out, cap, mo[m - 1]); str_append(out, cap, " ");
        format_u64(n, sizeof(n), (uint64_t)y); str_append(out, cap, n); str_append(out, cap, " at ");
    }
    uint32_t hh = (uint32_t)(secs / 3600), mm = (uint32_t)((secs / 60) % 60);
    char t[6] = {(char)('0' + hh / 10), (char)('0' + hh % 10), ':', (char)('0' + mm / 10), (char)('0' + mm % 10), 0};
    str_append(out, cap, t);
}
static int is_night(void) {
    int64_t e = shell_now_epoch();
    if (!e) return 0;
    int32_t hour = (int32_t)((e % 86400) / 3600);
    return hour >= 19 || hour < 7;
}

/* ------------------------------------------------------------ appearance */
static void render_wallpaper(void) {
    if (personality_render_wallpaper()) return;          /* procedural MintOS wallpapers */
    uint32_t variant = desktop_settings.wallpaper == 1u ? MINT_WALLPAPER_LIGHT :
                       desktop_settings.wallpaper == 2u ? MINT_WALLPAPER_DARK :
                       (T.dark ? MINT_WALLPAPER_DARK : MINT_WALLPAPER_LIGHT);
    if (mint_ui_render_wallpaper(variant) != 0) {
        gfx_rect all = R(0, 0, SW, SH);
        gfx_clip_set(all);
        gfx_round_rect_grad(&gfx_wall, 0, 0, SW, SH, 0, OPAQUE(T.dark ? 0x141A40u : 0x86BEF0u), OPAQUE(T.dark ? 0x4A2A68u : 0xF6D8C8u));
    }
}
int shell_apply_settings(void) {
    desktop_settings.version = MINT_DESKTOP_SETTINGS_VERSION;
    int ok = mint_desktop_settings_set(&desktop_settings) == 0;
    (void)mint_desktop_settings_get(&desktop_settings);
    if (ok) userdata_store_appearance();              /* saved to ~/Library/Preferences (debounced) */
    night_mode = is_night();
    theme_apply(&desktop_settings, night_mode);
    render_wallpaper();
    damage_all();
    return ok;
}

/* ------------------------------------------------------------ window manager */
static mde_layout window_layout(uint32_t app) {
    mde_layout l;
    mde_window *w = &windows[app];
    int chrome = app_chrome(app);
    int32_t bar = chrome == CHROME_TOOLBAR ? 52 : 28;
    int32_t sw = app_sidebar_width(app);
    l.frame = R(w->x, w->y, w->w, w->h);
    l.sidebar = R(w->x, w->y + bar, sw, w->h - bar);
    l.toolbar = R(w->x + sw, w->y, w->w - sw, bar);
    l.content = R(w->x + sw, w->y + bar, w->w - sw, w->h - bar);
    l.focused = focused_app == (int32_t)app && !launchpad_open;
    return l;
}
static int32_t window_bar(uint32_t app) { return app_chrome(app) == CHROME_TOOLBAR ? 52 : 28; }

static void dock_geometry(gfx_rect *r, int32_t *icon);
static gfx_rect usable_area(void) {
    gfx_rect d; int32_t icon; dock_geometry(&d, &icon);
    return R(0, MENUBAR_H, SW, d.y0 - MENUBAR_H - 6);
}

static void refocus(void) {
    focused_app = -1;
    for (uint32_t i = zcount; i-- > 0;)
        if (!windows[zorder[i]].minimized) { focused_app = (int32_t)zorder[i]; break; }
}
static void raise_window(uint32_t app) {
    uint32_t j = 0;
    for (uint32_t i = 0; i < zcount; ++i) if (zorder[i] != app) zorder[j++] = zorder[i];
    zorder[j++] = app; zcount = j;
    if (focused_app >= 0) shell_damage_window((uint32_t)focused_app);
    if (focused_app != (int32_t)app) shell_damage(R(0, 0, SW, MENUBAR_H));   /* menu bar shows the active app */
    windows[app].minimized = 0;
    focused_app = (int32_t)app;
    shell_damage_window(app);
}
void shell_open_app(uint32_t app) {
    if (app >= APP_COUNT) return;
    if (app == APP_LAUNCHPAD) { launchpad_open = 1; active_menu = -1; damage_all(); return; }
    launchpad_open = 0; spotlight_open = 0; control_center_open = 0;
    mde_window *w = &windows[app];
    if (!w->open) {
        int32_t ww, hh; app_default_size(app, &ww, &hh);
        gfx_rect u = usable_area();
        if (ww > SW - 40) ww = SW - 40;
        if (hh > H(u) - 20) hh = H(u) - 20;
        w->w = ww; w->h = hh;
        w->x = (SW - ww) / 2 + (int32_t)(cascade % 6u) * 26 - 60;
        w->y = u.y0 + (H(u) - hh) / 2 + (int32_t)(cascade % 6u) * 22 - 40;
        if (w->x < 8) w->x = 8;
        if (w->y < MENUBAR_H + 8) w->y = MENUBAR_H + 8;
        ++cascade;
        w->open = 1; w->minimized = 0; w->zoomed = 0;
    }
    app_opened(app);
    raise_window(app);
    damage_all();
}
void shell_close_app(uint32_t app) {
    if (app >= APP_COUNT || !windows[app].open) return;
    shell_damage_window(app);
    windows[app].open = 0;
    uint32_t j = 0;
    for (uint32_t i = 0; i < zcount; ++i) if (zorder[i] != app) zorder[j++] = zorder[i];
    zcount = j;
    refocus();
    if (focused_app >= 0) shell_damage_window((uint32_t)focused_app);
    shell_damage(R(0, 0, SW, MENUBAR_H));
    damage_all();
}
static void minimize_app(uint32_t app) {
    if (!windows[app].open) return;
    shell_damage_window(app);
    windows[app].minimized = 1;
    refocus();
    damage_all();
}
static void zoom_app(uint32_t app) {
    mde_window *w = &windows[app];
    shell_damage_window(app);
    if (w->zoomed) { w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh; w->zoomed = 0; }
    else {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        gfx_rect u = usable_area();
        w->x = 6; w->y = u.y0 + 6; w->w = SW - 12; w->h = H(u) - 8; w->zoomed = 1;
    }
    damage_all();
}
void shell_request_power(uint32_t action) {
    power_confirm = action;
    damage_all();
}

/* ------------------------------------------------------------ painting: windows */
static void traffic_lights(gfx_rect frame, int32_t bar, int focused, int hover) {
    static const uint32_t fill[3] = {0xFF5F57u, 0xFEBC2Eu, 0x28C840u};
    static const uint32_t ring[3] = {0xE0443Eu, 0xDEA123u, 0x1AAB29u};
    int32_t cy = frame.y0 + bar / 2;
    for (int i = 0; i < 3; ++i) {
        int32_t cx = frame.x0 + 20 + i * 20;
        if (focused || hover) {
            gfx_circle(S, cx, cy, 6, OPAQUE(ring[i]));
            gfx_circle(S, cx, cy, 5, OPAQUE(fill[i]));
        } else {
            gfx_circle(S, cx, cy, 6, OPAQUE(T.dark ? 0x4A4A4Du : 0xD0D0D4u));
            gfx_circle(S, cx, cy, 5, OPAQUE(T.dark ? 0x56565Au : 0xDCDCE0u));
        }
        if (hover) {
            uint32_t g = ARGB(0xB0, 0x40, 0x10, 0x10);
            if (i == 0) { gfx_line(S, cx - 3, cy - 3, cx + 3, cy + 3, g); gfx_line(S, cx + 3, cy - 3, cx - 3, cy + 3, g); }
            else if (i == 1) gfx_fill(S, cx - 3, cy, 7, 1, ARGB(0xC0, 0x80, 0x50, 0x00));
            else { gfx_fill(S, cx - 3, cy, 7, 1, ARGB(0xC0, 0x00, 0x50, 0x00)); gfx_fill(S, cx, cy - 3, 1, 7, ARGB(0xC0, 0x00, 0x50, 0x00)); }
        }
    }
}

static uint32_t corner_buf[4 * WIN_R * WIN_R];
static void paint_window(uint32_t app, gfx_rect clip) {
    mde_window *w = &windows[app];
    if (!w->open || w->minimized || !gfx_rect_overlaps(window_bounds(app), clip)) return;
    mde_layout l = window_layout(app);
    int focused = l.focused;
    int chrome = app_chrome(app);
    int32_t bar = window_bar(app);
    int32_t sw = app_sidebar_width(app);
    gfx_rect sidebar = R(w->x, w->y, sw, w->h), sidebar_clip = gfx_rect_intersect(sidebar, clip);
    if (sw && !gfx_rect_empty(sidebar_clip)) {
        /* Vibrancy: blur what lies behind the window under the sidebar (before
           the window's own shadow touches it) and park the result in the
           scratch surface until the window body has been drawn. */
        gfx_clip_set(clip);
        gfx_blur_rounded(S, sidebar, SIDEBAR_BLUR, 0);
        gfx_copy(&gfx_scratch, S, sidebar_clip);
    }
    gfx_clip_set(clip);
    gfx_shadow(S, w->x, w->y, w->w, w->h, WIN_R, focused ? 38 : 22, focused ? 16 : 8, ARGB(focused ? 0x5A : 0x34, 0, 0, 0));
    gfx_clip_set(clip);
    gfx_corners_save(S, w->x, w->y, w->w, w->h, WIN_R, corner_buf);
    gfx_fill(S, w->x, w->y, w->w, w->h, OPAQUE(app == APP_CALCULATOR ? 0x2B2B2Du : T.window));
    if (sw) {
        if (!gfx_rect_empty(sidebar_clip)) {
            gfx_copy(S, &gfx_scratch, sidebar_clip);
            gfx_fill(S, sidebar.x0, sidebar.y0, sw, w->h, T.sidebar_tint);
        }
        gfx_vline(S, w->x + sw, w->y, w->h, T.separator);
    }
    if (chrome != CHROME_PLAIN) gfx_hline(S, w->x + sw, w->y + bar - 1, w->w - sw, T.separator);
    /* App content may not draw outside its window. */
    gfx_clip_set(gfx_rect_intersect(clip, l.frame));
    app_paint(app, &l);
    gfx_clip_set(gfx_rect_intersect(clip, l.frame));
    if (chrome == CHROME_TITLE) {
        const char *title = app_window_title(app);
        int32_t tw = gfx_text_width(FONT_BODY_BOLD, title);
        ui_label(w->x + (w->w - tw) / 2, BL(R(0, w->y, 1, bar), FONT_BODY_BOLD), FONT_BODY_BOLD, title, focused ? T.text : T.text3);
    }
    traffic_lights(l.frame, bar, focused || app == APP_CALCULATOR, hover_lights == (int32_t)app);
    gfx_clip_set(clip);
    gfx_corners_restore(S, w->x, w->y, w->w, w->h, WIN_R, corner_buf);
    gfx_round_rect_stroke(S, w->x, w->y, w->w, w->h, WIN_R, T.dark ? ARGB(0x40, 0xFF, 0xFF, 0xFF) : ARGB(0x2E, 0, 0, 0));
}

/* ------------------------------------------------------------ Dock */
static const uint32_t dock_apps[] = {APP_LAUNCHPAD, APP_FILES, APP_WEB, APP_NOTES, APP_TERMINAL, APP_CALCULATOR,
                                     APP_SETTINGS, APP_MONITOR, APP_STORAGE, APP_DISKS, APP_DRIVERS, APP_NETWORK};
#define DOCK_APP_COUNT ((int32_t)(sizeof(dock_apps) / sizeof(dock_apps[0])))
#define DOCK_TRASH DOCK_APP_COUNT
static void dock_geometry(gfx_rect *r, int32_t *icon) {
    int32_t ic = 54, n = DOCK_APP_COUNT + 1;
    int32_t w = n * (ic + DOCK_GAP) - DOCK_GAP + 2 * DOCK_PAD + 17;
    while (w > SW - 24 && ic > 32) { --ic; w = n * (ic + DOCK_GAP) - DOCK_GAP + 2 * DOCK_PAD + 17; }
    int32_t h = ic + 2 * DOCK_PAD + 4;
    *r = R((SW - w) / 2, SH - h - 6, w, h);
    *icon = ic;
}
static gfx_rect dock_icon_rect(int32_t i) {
    gfx_rect d; int32_t ic; dock_geometry(&d, &ic);
    int32_t x = d.x0 + DOCK_PAD + i * (ic + DOCK_GAP) + (i >= DOCK_TRASH ? 17 : 0);
    return R(x, d.y0 + DOCK_PAD, ic, ic);
}
static int32_t dock_hit(int32_t x, int32_t y) {
    gfx_rect d; int32_t ic; dock_geometry(&d, &ic);
    if (!gfx_rect_contains(d, x, y)) return -1;
    for (int32_t i = 0; i <= DOCK_TRASH; ++i) {
        gfx_rect r = grow(dock_icon_rect(i), DOCK_GAP / 2, DOCK_PAD, DOCK_GAP / 2, DOCK_PAD);
        if (gfx_rect_contains(r, x, y)) return i;
    }
    return -2;
}
static void glass(gfx_rect r, int32_t radius, int32_t blur, uint32_t tint) {
    gfx_blur_rounded(S, r, blur, radius);
    gfx_round_rect(S, r.x0, r.y0, W(r), H(r), radius, tint);
}
static gfx_rect dock_tooltip_rect(void) {
    if (hover_dock < 0) return R(0, 0, 0, 0);
    const char *name = hover_dock == DOCK_TRASH ? "Trash" : app_name(dock_apps[hover_dock]);
    gfx_rect ir = dock_icon_rect(hover_dock);
    int32_t w = gfx_text_width(FONT_BODY, name) + 24;
    return R(ir.x0 + W(ir) / 2 - w / 2, ir.y0 - DOCK_PAD - 38, w, 26);
}
static void paint_dock(void) {
    if (launchpad_open) return;
    gfx_rect d; int32_t ic; dock_geometry(&d, &ic);
    gfx_shadow(S, d.x0, d.y0, W(d), H(d), 18, 18, 6, ARGB(0x30, 0, 0, 0));
    glass(d, 18, DOCK_BLUR, T.dark ? ARGB(0x8C, 0x2A, 0x2A, 0x2E) : ARGB(0x70, 0xF4, 0xF4, 0xF7));
    gfx_round_rect_stroke(S, d.x0, d.y0, W(d), H(d), 18, T.dark ? ARGB(0x38, 0xFF, 0xFF, 0xFF) : ARGB(0x50, 0xFF, 0xFF, 0xFF));
    for (int32_t i = 0; i < DOCK_APP_COUNT; ++i) {
        gfx_rect r = dock_icon_rect(i);
        uint32_t app = dock_apps[i];
        gfx_image(S, r.x0, r.y0, ic, ic, app_icon(app, ic > 64), ic > 64 ? 128 : 64, ic > 64 ? 128 : 64, 255u);
        if (windows[app].open) gfx_circle(S, r.x0 + ic / 2, d.y1 - 4, 2, OPAQUE(T.dark ? 0xE5E5EAu : 0x3A3A3Cu));
    }
    gfx_rect sep = dock_icon_rect(DOCK_TRASH);
    gfx_vline(S, sep.x0 - 9, d.y0 + 10, H(d) - 20, T.dark ? ARGB(0x50, 0xFF, 0xFF, 0xFF) : ARGB(0x30, 0, 0, 0));
    gfx_image(S, sep.x0, sep.y0, ic, ic, mint_icon_trash_64, 64, 64, 255u);
    if (hover_dock >= 0) {
        const char *name = hover_dock == DOCK_TRASH ? "Trash" : app_name(dock_apps[hover_dock]);
        gfx_rect t = dock_tooltip_rect();
        gfx_shadow(S, t.x0, t.y0, W(t), H(t), 7, 8, 3, ARGB(0x30, 0, 0, 0));
        gfx_round_rect(S, t.x0, t.y0, W(t), H(t), 7, OPAQUE(T.dark ? 0x3A3A3Cu : 0xF2F2F4u));
        gfx_round_rect_stroke(S, t.x0, t.y0, W(t), H(t), 7, T.separator);
        ui_label_center(t.x0 + W(t) / 2, BL(t, FONT_BODY), FONT_BODY, name, T.text);
    }
}

/* ------------------------------------------------------------ Menu bar and menus */
enum { ACT_NONE = 0, ACT_ABOUT_OS, ACT_ABOUT_DE, ACT_SETTINGS, ACT_LAUNCHPAD, ACT_RESTART, ACT_SHUTDOWN,
       ACT_HIDE, ACT_QUIT, ACT_NEW_FILES, ACT_NEW_TERMINAL, ACT_CLOSE, ACT_MINIMIZE, ACT_ZOOM,
       ACT_BRING_ALL, ACT_TOGGLE_DARK, ACT_SPOTLIGHT, ACT_HELP, ACT_RELEASE_NOTES, ACT_FOCUS_BASE = 100,
       ACT_FILES_BASE = 200, ACT_WEB_BASE = 220, ACT_NOTES_BASE = 300 };
typedef struct { const char *label; const char *shortcut; uint32_t action; uint8_t enabled, separator, checked; } menu_item;
static menu_item menu_items[24];
static uint32_t menu_count;
static char menu_label_buf[6][40];

static const char *menu_titles[7] = {"", "", "File", "Edit", "View", "Window", "Help"};
static uint32_t current_app_for_menu(void) { return focused_app >= 0 ? (uint32_t)focused_app : APP_FILES; }
static void add_item(const char *label, const char *sc, uint32_t act, int enabled) {
    if (menu_count < 24u) menu_items[menu_count++] = (menu_item){label, sc, act, (uint8_t)enabled, 0, 0};
}
static void add_sep(void) { if (menu_count) menu_items[menu_count - 1].separator = 1; }
static void build_menu(int32_t m) {
    menu_count = 0;
    uint32_t app = current_app_for_menu();
    switch (m) {
    case 0:
        add_item("About MintOS", 0, ACT_ABOUT_OS, 1); add_sep();
        add_item("System Settings\xE2\x80\xA6", "\xE2\x8C\x98,", ACT_SETTINGS, 1);
        add_item("Launchpad", 0, ACT_LAUNCHPAD, 1); add_sep();
        add_item("Restart\xE2\x80\xA6", 0, ACT_RESTART, 1);
        add_item("Shut Down\xE2\x80\xA6", 0, ACT_SHUTDOWN, 1);
        break;
    case 1:
        copy_text(menu_label_buf[0], 40, "About "); str_append(menu_label_buf[0], 40, app_name(app));
        copy_text(menu_label_buf[1], 40, "Hide "); str_append(menu_label_buf[1], 40, app_name(app));
        copy_text(menu_label_buf[2], 40, "Quit "); str_append(menu_label_buf[2], 40, app_name(app));
        add_item(menu_label_buf[0], 0, ACT_ABOUT_DE, 1); add_sep();
        add_item("Settings\xE2\x80\xA6", "\xE2\x8C\x98,", ACT_SETTINGS, 1); add_sep();
        add_item(menu_label_buf[1], "\xE2\x8C\x98H", ACT_HIDE, focused_app >= 0); add_sep();
        add_item(menu_label_buf[2], "\xE2\x8C\x98Q", ACT_QUIT, focused_app >= 0);
        break;
    case 2:
        add_item("New Files Window", "\xE2\x8C\x98N", ACT_NEW_FILES, 1);
        if (app == APP_FILES && focused_app == APP_FILES) {
            add_item("New Folder", "\xE2\x87\xA7\xE2\x8C\x98N", ACT_FILES_BASE + FILES_CMD_NEW_FOLDER, files_command_enabled(FILES_CMD_NEW_FOLDER));
            add_sep();
            add_item("Rename", "\xE2\x8F\x8E", ACT_FILES_BASE + FILES_CMD_RENAME, files_command_enabled(FILES_CMD_RENAME));
            add_item("Duplicate", "\xE2\x8C\x98" "D", ACT_FILES_BASE + FILES_CMD_DUPLICATE, files_command_enabled(FILES_CMD_DUPLICATE));
            add_item("Move to Trash", "\xE2\x8C\x98\xE2\x8C\xAB", ACT_FILES_BASE + FILES_CMD_TRASH, files_command_enabled(FILES_CMD_TRASH));
            add_sep();
            add_item("Empty Trash\xE2\x80\xA6", "\xE2\x87\xA7\xE2\x8C\x98\xE2\x8C\xAB", ACT_FILES_BASE + FILES_CMD_EMPTY_TRASH, files_command_enabled(FILES_CMD_EMPTY_TRASH));
            add_sep();
        } else if (app == APP_NOTES && focused_app == APP_NOTES) {
            add_item("New Note", "\xE2\x8C\x98N", ACT_NOTES_BASE + NOTES_CMD_NEW, notes_command_enabled(NOTES_CMD_NEW));
            add_item("Delete Note", "\xE2\x8C\x98\xE2\x8C\xAB", ACT_NOTES_BASE + NOTES_CMD_DELETE, notes_command_enabled(NOTES_CMD_DELETE));
            add_sep();
        } else if (app == APP_WEB && focused_app == APP_WEB) {
            add_item("Add to Favorites", "\xE2\x8C\x98" "D", ACT_WEB_BASE + 1, 1);
            add_item("Show Favorites", 0, ACT_WEB_BASE + 3, 1);
            add_item("Show Downloads", "\xE2\x87\xA7\xE2\x8C\x98J", ACT_WEB_BASE + 2, 1); add_sep();
        } else {
            add_item("New Terminal Window", 0, ACT_NEW_TERMINAL, 1); add_sep();
        }
        add_item("Close Window", "\xE2\x8C\x98W", ACT_CLOSE, focused_app >= 0);
        break;
    case 3:
        add_item("Undo", "\xE2\x8C\x98Z", ACT_NONE, 0); add_item("Redo", "\xE2\x87\xA7\xE2\x8C\x98Z", ACT_NONE, 0); add_sep();
        add_item("Cut", "\xE2\x8C\x98X", ACT_NONE, 0); add_item("Copy", "\xE2\x8C\x98" "C", ACT_NONE, 0);
        add_item("Paste", "\xE2\x8C\x98V", ACT_NONE, 0); add_item("Select All", "\xE2\x8C\x98" "A", ACT_NONE, 0);
        break;
    case 4:
        add_item(T.dark ? "Use Light Appearance" : "Use Dark Appearance", 0, ACT_TOGGLE_DARK, 1);
        add_item("Spotlight Search", "\xE2\x8C\x98Space", ACT_SPOTLIGHT, 1); add_sep();
        add_item("Enter Full Screen", "\xE2\x8C\x83\xE2\x8C\x98" "F", ACT_ZOOM, focused_app >= 0);
        break;
    case 5:
        add_item("Minimize", "\xE2\x8C\x98M", ACT_MINIMIZE, focused_app >= 0);
        add_item("Zoom", 0, ACT_ZOOM, focused_app >= 0); add_sep();
        add_item("Bring All to Front", 0, ACT_BRING_ALL, zcount > 0); add_sep();
        for (uint32_t i = 0; i < zcount && menu_count < 22u; ++i) {
            add_item(app_name(zorder[i]), 0, ACT_FOCUS_BASE + zorder[i], 1);
            menu_items[menu_count - 1].checked = focused_app == (int32_t)zorder[i];
        }
        break;
    case 6:
        add_item("MintOS Help", 0, ACT_HELP, 1);
        add_item("Release Notes", 0, ACT_RELEASE_NOTES, 1);
        break;
    default: break;
    }
}
static int32_t menu_title_x(int32_t m) {
    int32_t x = 46;
    for (int32_t i = 1; i < m; ++i)
        x += gfx_text_width(i == 1 ? FONT_BODY_BOLD : FONT_BODY, i == 1 ? app_name(current_app_for_menu()) : menu_titles[i]) + 22;
    return m == 0 ? 10 : x;
}
static int32_t menu_title_w(int32_t m) {
    if (m == 0) return 32;
    return gfx_text_width(m == 1 ? FONT_BODY_BOLD : FONT_BODY, m == 1 ? app_name(current_app_for_menu()) : menu_titles[m]) + 22;
}
static gfx_rect menu_rect(void) {
    int32_t w = 220;
    for (uint32_t i = 0; i < menu_count; ++i) {
        int32_t iw = gfx_text_width(FONT_BODY, menu_items[i].label) + (menu_items[i].shortcut ? gfx_text_width(FONT_BODY, menu_items[i].shortcut) + 40 : 0) + 48;
        if (iw > w) w = iw;
    }
    int32_t h = 10;
    for (uint32_t i = 0; i < menu_count; ++i) h += 22 + (menu_items[i].separator ? 11 : 0);
    int32_t x = menu_title_x(active_menu);
    if (x + w > SW - 6) x = SW - 6 - w;
    return R(x, MENUBAR_H + 1, w, h);
}
static gfx_rect menu_item_rect(uint32_t idx) {
    gfx_rect m = menu_rect();
    int32_t y = m.y0 + 5;
    for (uint32_t i = 0; i < idx; ++i) y += 22 + (menu_items[i].separator ? 11 : 0);
    return R(m.x0 + 5, y, W(m) - 10, 22);
}
static gfx_rect clock_rect(void) {
    char t[48]; shell_format_clock(t, sizeof(t), shell_now_epoch(), 0);
    int32_t w = gfx_text_width(FONT_BODY, t) + 16;
    return R(SW - 10 - w, 0, w, MENUBAR_H);
}
static gfx_rect spotlight_icon_rect(void) { gfx_rect c = clock_rect(); return R(c.x0 - 32, 0, 30, MENUBAR_H); }
static gfx_rect control_icon_rect(void) { gfx_rect c = spotlight_icon_rect(); return R(c.x0 - 32, 0, 30, MENUBAR_H); }

static void paint_menu_bar(void) {
    gfx_rect bar = R(0, 0, SW, MENUBAR_H);
    if (launchpad_open) return;
    glass(bar, 0, MENU_BLUR, T.dark ? ARGB(0x8A, 0x1E, 0x1E, 0x22) : ARGB(0x7E, 0xF6, 0xF6, 0xF8));
    gfx_hline(S, 0, MENUBAR_H - 1, SW, T.dark ? ARGB(0x30, 0, 0, 0) : ARGB(0x18, 0, 0, 0));
    uint32_t fg = T.text;
    for (int32_t m = 0; m < 7; ++m) {
        gfx_rect tr = R(menu_title_x(m) - (m ? 9 : 0), 3, menu_title_w(m) - (m ? 4 : 0), MENUBAR_H - 6);
        if (m == 0) tr = R(8, 3, 34, MENUBAR_H - 6);
        if (active_menu == m) gfx_round_rect(S, tr.x0, tr.y0, W(tr), H(tr), 5, T.dark ? ARGB(0x40, 0xFF, 0xFF, 0xFF) : ARGB(0x22, 0, 0, 0));
        if (m == 0) gfx_mask(S, 17, 5, 16, 16, mint_icon_mint_leaf_a8, 32, 32, OPAQUE(fg));
        else ui_label(menu_title_x(m), BL(bar, FONT_BODY), m == 1 ? FONT_BODY_BOLD : FONT_BODY,
                      m == 1 ? app_name(current_app_for_menu()) : menu_titles[m], fg);
    }
    char t[48]; shell_format_clock(t, sizeof(t), shell_now_epoch(), 0);
    gfx_rect c = clock_rect();
    if (extras_notification_center_open()) gfx_round_rect(S, c.x0, 3, W(c), MENUBAR_H - 6, 5, ARGB(0x22, 0, 0, 0));
    ui_label(c.x0 + 8, BL(bar, FONT_BODY), FONT_BODY, t, fg);
    gfx_rect sp = spotlight_icon_rect(), cc = control_icon_rect();
    if (spotlight_open) gfx_round_rect(S, sp.x0, 3, W(sp), MENUBAR_H - 6, 5, ARGB(0x22, 0, 0, 0));
    if (control_center_open) gfx_round_rect(S, cc.x0, 3, W(cc), MENUBAR_H - 6, 5, ARGB(0x22, 0, 0, 0));
    ui_symbol_glyph(sp.x0 + 15, MENUBAR_H / 2, SYM_SEARCH, OPAQUE(fg));
    ui_symbol_glyph(cc.x0 + 15, MENUBAR_H / 2, SYM_TOGGLES, OPAQUE(fg));
}

static void panel(gfx_rect r, int32_t radius) {
    gfx_shadow(S, r.x0, r.y0, W(r), H(r), radius, 24, 10, ARGB(0x40, 0, 0, 0));
    glass(r, radius, MENU_BLUR, T.dark ? ARGB(0xC8, 0x28, 0x28, 0x2C) : ARGB(0xC4, 0xF4, 0xF4, 0xF6));
    gfx_round_rect_stroke(S, r.x0, r.y0, W(r), H(r), radius, T.dark ? ARGB(0x40, 0xFF, 0xFF, 0xFF) : ARGB(0x22, 0, 0, 0));
}

static void paint_menu_dropdown(void) {
    if (active_menu < 0) return;
    build_menu(active_menu);
    gfx_rect m = menu_rect();
    panel(m, 8);
    for (uint32_t i = 0; i < menu_count; ++i) {
        gfx_rect r = menu_item_rect(i);
        menu_item *it = &menu_items[i];
        int hl = (int32_t)i == hovered_menu_item && it->enabled;
        if (hl) gfx_round_rect(S, r.x0, r.y0, W(r), H(r), 5, OPAQUE(T.accent));
        uint32_t c = hl ? 0xFFFFFFu : it->enabled ? T.text : T.text3;
        if (it->checked) ui_symbol_glyph(r.x0 + 12, r.y0 + 11, SYM_CHECK, OPAQUE(c));
        ui_label(r.x0 + 24, BL(r, FONT_BODY), FONT_BODY, it->label, c);
        if (it->shortcut) ui_label_right(r.x1 - 12, BL(r, FONT_BODY), FONT_BODY, it->shortcut, hl ? 0xFFFFFFu : T.text3);
        if (it->separator) gfx_hline(S, r.x0 + 6, r.y1 + 5, W(r) - 12, T.separator);
    }
}

static void run_action(uint32_t act) {
    active_menu = -1; hovered_menu_item = -1;
    damage_all();
    uint32_t app = current_app_for_menu();
    if (act >= ACT_FOCUS_BASE && act < ACT_FOCUS_BASE + APP_COUNT) { shell_open_app(act - ACT_FOCUS_BASE); return; }
    if (act > ACT_FILES_BASE && act < ACT_FILES_BASE + FILES_CMD_COUNT) { if (files_command(act - ACT_FILES_BASE)) shell_damage_window(APP_FILES); return; }
    if (act > ACT_NOTES_BASE && act < ACT_NOTES_BASE + NOTES_CMD_COUNT) { if (notes_command(act - ACT_NOTES_BASE)) shell_damage_window(APP_NOTES); return; }
    if (act > ACT_WEB_BASE && act < ACT_WEB_BASE + 8) { if (web_command(act - ACT_WEB_BASE)) shell_damage_window(APP_WEB); return; }
    switch (act) {
    case ACT_ABOUT_OS: shell_open_app(APP_ABOUT_OS); break;
    case ACT_ABOUT_DE: shell_open_app(APP_ABOUT_MINTDE); break;
    case ACT_SETTINGS: shell_open_app(APP_SETTINGS); break;
    case ACT_LAUNCHPAD: shell_open_app(APP_LAUNCHPAD); break;
    case ACT_RESTART: shell_request_power(MINT_POWER_ACTION_RESTART); break;
    case ACT_SHUTDOWN: shell_request_power(MINT_POWER_ACTION_SHUTDOWN); break;
    case ACT_HIDE: case ACT_MINIMIZE: if (focused_app >= 0) minimize_app(app); break;
    case ACT_QUIT: case ACT_CLOSE: if (focused_app >= 0) shell_close_app(app); break;
    case ACT_NEW_FILES: shell_open_app(APP_FILES); break;
    case ACT_NEW_TERMINAL: shell_open_app(APP_TERMINAL); break;
    case ACT_ZOOM: if (focused_app >= 0) zoom_app(app); break;
    case ACT_BRING_ALL: for (uint32_t i = 0; i < zcount; ++i) windows[zorder[i]].minimized = 0; refocus(); break;
    case ACT_TOGGLE_DARK:
        desktop_settings.theme = T.dark ? MINT_THEME_LIGHT : MINT_THEME_DARK; (void)shell_apply_settings(); break;
    case ACT_SPOTLIGHT: spotlight_open = 1; spotlight_query[0] = '\0'; spotlight_sel = 0; break;
    case ACT_HELP: shell_open_app(APP_TERMINAL); break;
    case ACT_RELEASE_NOTES: shell_open_app(APP_ABOUT_OS); break;
    default: break;
    }
}

/* ------------------------------------------------------------ Control Center */
static gfx_rect cc_rect(void) { return R(SW - 336, MENUBAR_H + 6, 326, 252 + extras_cc_sound_height() + 10); }
static gfx_rect cc_sound_rect(void) { gfx_rect r = cc_rect(); return R(r.x0 + 12, r.y0 + 162, W(r) - 24, extras_cc_sound_height()); }
static void paint_control_center(void) {
    if (!control_center_open) return;
    gfx_rect r = cc_rect();
    panel(r, 16);
    gfx_rect a = R(r.x0 + 12, r.y0 + 12, 146, 70), b = R(r.x0 + 168, r.y0 + 12, 146, 70);
    gfx_round_rect(S, a.x0, a.y0, W(a), H(a), 12, T.dark ? ARGB(0x30, 0xFF, 0xFF, 0xFF) : ARGB(0x90, 0xFF, 0xFF, 0xFF));
    gfx_circle(S, a.x0 + 26, a.y0 + 35, 14, OPAQUE(T.dark ? T.accent : (T.dark ? 0x48484Au : 0xD8D8DCu)));
    gfx_circle(S, a.x0 + 26, a.y0 + 35, 6, OPAQUE(T.dark ? 0xFFFFFFu : 0x3A3A3Cu));
    ui_label(a.x0 + 48, a.y0 + 32, FONT_BODY_BOLD, "Dark Mode", T.text);
    ui_label(a.x0 + 48, a.y0 + 49, FONT_CAPTION, T.dark ? "On" : "Off", T.text2);
    extras_cc_dnd_paint(b);
    gfx_rect c = R(r.x0 + 12, r.y0 + 92, W(r) - 24, 60);
    gfx_round_rect(S, c.x0, c.y0, W(c), H(c), 12, T.dark ? ARGB(0x30, 0xFF, 0xFF, 0xFF) : ARGB(0x90, 0xFF, 0xFF, 0xFF));
    ui_label(c.x0 + 14, c.y0 + 22, FONT_BODY_BOLD, "Accent Color", T.text);
    static const uint32_t sw[5] = {0x00B894u, 0x007AFFu, 0xAF52DEu, 0xFF9500u, 0xFF2D55u};
    for (int i = 0; i < 5; ++i) {
        gfx_circle(S, c.x0 + 22 + i * 30, c.y0 + 42, 9, OPAQUE(sw[i]));
        if ((uint32_t)i == desktop_settings.accent) gfx_circle(S, c.x0 + 22 + i * 30, c.y0 + 42, 3, OPAQUE(0xFFFFFFu));
    }
    extras_cc_sound_paint(cc_sound_rect());
    gfx_rect d = R(r.x0 + 12, cc_sound_rect().y1 + 10, W(r) - 24, 78);
    gfx_round_rect(S, d.x0, d.y0, W(d), H(d), 12, T.dark ? ARGB(0x30, 0xFF, 0xFF, 0xFF) : ARGB(0x90, 0xFF, 0xFF, 0xFF));
    ui_label(d.x0 + 14, d.y0 + 22, FONT_BODY_BOLD, "Display & Input", T.text);
    char res[32], n[12]; format_u64(res, sizeof(res), (uint64_t)SW); str_append(res, sizeof(res), " \xC3\x97 ");
    format_u64(n, sizeof(n), (uint64_t)SH); str_append(res, sizeof(res), n);
    ui_label(d.x0 + 14, d.y0 + 40, FONT_CAPTION, res, T.text2);
    MINT_INPUT_INFO in = {0}; (void)mint_input_get_info(&in);
    char ready[64];
    copy_text(ready, sizeof(ready), in.keyboard_ready ? "Keyboard ready" : "No keyboard");
    str_append(ready, sizeof(ready), in.mouse_ready ? " \xC2\xB7 Pointer ready" : " \xC2\xB7 No pointer");
    ui_label(d.x0 + 14, d.y0 + 58, FONT_CAPTION, ready, T.text3);
}
static int control_center_click(int32_t x, int32_t y) {
    gfx_rect r = cc_rect();
    if (!gfx_rect_contains(r, x, y)) return 0;
    if (gfx_rect_contains(R(r.x0 + 12, r.y0 + 12, 146, 70), x, y)) {
        desktop_settings.theme = T.dark ? MINT_THEME_LIGHT : MINT_THEME_DARK; (void)shell_apply_settings(); return 1;
    }
    if (gfx_rect_contains(R(r.x0 + 168, r.y0 + 12, 146, 70), x, y)) {
        personality_prefs.do_not_disturb ^= 1u; personality_prefs_apply(); return 1;
    }
    if (extras_cc_sound_click(cc_sound_rect(), x, y)) return 1;
    for (int i = 0; i < 5; ++i)
        if (gfx_rect_contains(R(r.x0 + 12 + 22 + i * 30 - 12, r.y0 + 92 + 30, 24, 24), x, y)) {
            desktop_settings.accent = (uint32_t)i; (void)shell_apply_settings(); return 1;
        }
    return 1;
}

/* ------------------------------------------------------------ Spotlight */
typedef struct { const char *title; const char *kind; uint32_t app; uint32_t settings_pane; } spot_result;
static spot_result spot_results[10];
static uint32_t spot_count;
static int ci_has(const char *hay, const char *needle) {
    if (!needle[0]) return 0;
    for (size_t i = 0; hay[i]; ++i) {
        size_t j = 0;
        while (needle[j] && hay[i + j]) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            ++j;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}
static void spotlight_search(void) {
    spot_count = 0;
    for (uint32_t a = 0; a < APP_COUNT && spot_count < 8u; ++a)
        if (ci_has(app_name(a), spotlight_query)) spot_results[spot_count++] = (spot_result){app_name(a), "Application", a, 0xFFu};
    static const char *const panes[] = {"Appearance", "Wallpaper", "Displays", "Power", "Storage", "Network", "Sound", "Notifications", "Users & Groups"};
    for (uint32_t p = 0; p < 9u && spot_count < 10u; ++p)
        if (ci_has(panes[p], spotlight_query)) spot_results[spot_count++] = (spot_result){panes[p], "System Settings", APP_SETTINGS, p};
    if (spotlight_sel >= (int32_t)spot_count) spotlight_sel = spot_count ? (int32_t)spot_count - 1 : 0;
}
static gfx_rect spotlight_rect(void) {
    int32_t w = SW < 760 ? SW - 40 : 680;
    int32_t h = 56 + (spot_count ? 8 + (int32_t)spot_count * 44 : 0);
    return R((SW - w) / 2, SH / 5, w, h);
}
static void paint_spotlight(void) {
    if (!spotlight_open) return;
    spotlight_search();
    gfx_rect r = spotlight_rect();
    panel(r, 14);
    ui_symbol_glyph(r.x0 + 30, r.y0 + 28, SYM_SEARCH, OPAQUE(T.text2));
    int32_t tx = r.x0 + 52;
    if (spotlight_query[0]) {
        int32_t w = gfx_text(S, FONT_LARGE, tx, r.y0 + 36, spotlight_query, OPAQUE(T.text));
        gfx_fill(S, tx + w + 2, r.y0 + 15, 2, 26, OPAQUE(T.accent));
    } else {
        gfx_fill(S, tx, r.y0 + 15, 2, 26, OPAQUE(T.accent));
        gfx_text(S, FONT_LARGE, tx + 6, r.y0 + 36, "Spotlight Search", OPAQUE(T.text3));
    }
    if (!spot_count) return;
    gfx_hline(S, r.x0, r.y0 + 56, W(r), T.separator);
    for (uint32_t i = 0; i < spot_count; ++i) {
        gfx_rect row = R(r.x0 + 8, r.y0 + 62 + (int32_t)i * 44, W(r) - 16, 42);
        int sel = (int32_t)i == spotlight_sel;
        if (sel) gfx_round_rect(S, row.x0, row.y0, W(row), H(row), 8, OPAQUE(T.accent));
        gfx_image(S, row.x0 + 8, row.y0 + 5, 32, 32, app_icon(spot_results[i].app, 0), 64, 64, 255u);
        ui_label(row.x0 + 52, row.y0 + 20, FONT_BODY_MEDIUM, spot_results[i].title, sel ? 0xFFFFFFu : T.text);
        ui_label(row.x0 + 52, row.y0 + 36, FONT_CAPTION, spot_results[i].kind, sel ? 0xE6EEFFu : T.text2);
    }
}
static void spotlight_activate(int32_t i) {
    if (i < 0 || (uint32_t)i >= spot_count) return;
    spot_result res = spot_results[i];
    spotlight_open = 0;
    if (res.settings_pane != 0xFFu) {
        static const uint32_t map[9] = {1u, 2u, 3u, 8u, 5u, 6u, 9u, 10u, 11u}; /* Settings pane indices */
        settings_select_pane(map[res.settings_pane]);
    }
    shell_open_app(res.app);
}

/* ------------------------------------------------------------ App switcher */
static gfx_rect switcher_rect(void) {
    int32_t n = (int32_t)(zcount ? zcount : 1u), w = n * 96 + 24;
    return R((SW - w) / 2, (SH - 128) / 2, w, 128);
}
static void paint_switcher(void) {
    if (!switcher_open || !zcount) return;
    gfx_rect r = switcher_rect();
    panel(r, 18);
    for (uint32_t i = 0; i < zcount; ++i) {
        uint32_t app = zorder[zcount - 1u - i];
        gfx_rect c = R(r.x0 + 12 + (int32_t)i * 96, r.y0 + 12, 96, 104);
        if (i == switcher_index) gfx_round_rect(S, c.x0 + 2, c.y0, 92, 104, 12, T.dark ? ARGB(0x40, 0xFF, 0xFF, 0xFF) : ARGB(0x28, 0, 0, 0));
        gfx_image(S, c.x0 + 12, c.y0 + 6, 72, 72, app_icon(app, 1), 128, 128, 255u);
        if (i == switcher_index) ui_label_center(c.x0 + 48, c.y0 + 96, FONT_CAPTION, app_name(app), T.text);
    }
}

/* ------------------------------------------------------------ Launchpad */
static uint32_t launchpad_apps[APP_COUNT];
static uint32_t launchpad_count(void) {
    uint32_t n = 0;
    for (uint32_t a = 0; a < APP_COUNT; ++a) if (a != APP_LAUNCHPAD) launchpad_apps[n++] = a;
    return n;
}
static gfx_rect launchpad_cell(uint32_t i, uint32_t n) {
    int32_t cols = SW >= 1400 ? 7 : 5, cw = 150, ch = 150;
    int32_t rows = (int32_t)(n + (uint32_t)cols - 1u) / cols;
    int32_t x0 = (SW - cols * cw) / 2, y0 = (SH - rows * ch) / 2 + 20;
    return R(x0 + (int32_t)(i % (uint32_t)cols) * cw, y0 + (int32_t)(i / (uint32_t)cols) * ch, cw, ch);
}
static void paint_launchpad(void) {
    gfx_rect all = R(0, 0, SW, SH);
    gfx_blur(S, all, 30);
    gfx_fill(S, 0, 0, SW, SH, T.dark ? ARGB(0x60, 0, 0, 0) : ARGB(0x48, 0x10, 0x10, 0x20));
    gfx_rect sf = R(SW / 2 - 120, 52, 240, 28);
    gfx_round_rect(S, sf.x0, sf.y0, W(sf), H(sf), 8, ARGB(0x40, 0xFF, 0xFF, 0xFF));
    ui_symbol_glyph(sf.x0 + 14, sf.y0 + 14, SYM_SEARCH, ARGB(0xD0, 0xFF, 0xFF, 0xFF));
    gfx_text(S, FONT_BODY, sf.x0 + 28, BL(sf, FONT_BODY), "Search", ARGB(0xD0, 0xFF, 0xFF, 0xFF));
    uint32_t n = launchpad_count();
    for (uint32_t i = 0; i < n; ++i) {
        gfx_rect c = launchpad_cell(i, n);
        gfx_image(S, c.x0 + 19, c.y0 + 8, 112, 112, app_icon(launchpad_apps[i], 1), 128, 128, 255u);
        int32_t tw = gfx_text_width(FONT_BODY_MEDIUM, app_name(launchpad_apps[i]));
        gfx_text(S, FONT_BODY_MEDIUM, c.x0 + 75 - tw / 2 + 1, c.y0 + 139, app_name(launchpad_apps[i]), ARGB(0x90, 0, 0, 0));
        gfx_text(S, FONT_BODY_MEDIUM, c.x0 + 75 - tw / 2, c.y0 + 138, app_name(launchpad_apps[i]), OPAQUE(0xFFFFFFu));
    }
}

/* ------------------------------------------------------------ Power alert */
static gfx_rect alert_rect(void) { return R((SW - 300) / 2, SH / 2 - 190, 300, 268); }
static gfx_rect alert_button(int i) {
    gfx_rect r = alert_rect();
    if (power_confirm == POWER_FAILED) return R(r.x0 + 20, r.y1 - 48, W(r) - 40, 30);
    return R(r.x0 + 20, r.y1 - 88 + i * 40, W(r) - 40, 30);
}
static void paint_alert(void) {
    if (!power_confirm) return;
    gfx_rect r = alert_rect();
    panel(r, 14);
    gfx_image(S, r.x0 + W(r) / 2 - 32, r.y0 + 20, 64, 64, mint_icon_about_mintos_128, 128, 128, 255u);
    const char *title = power_confirm == MINT_POWER_ACTION_RESTART ? "Are you sure you want to restart your computer now?" :
                        power_confirm == MINT_POWER_ACTION_SHUTDOWN ? "Are you sure you want to shut down your computer now?" :
                        "MintOS could not switch this computer off.";
    /* two-line wrap of the bold title */
    char l1[64], l2[64]; size_t n = text_length(title), cut = n;
    if (gfx_text_width(FONT_BODY_BOLD, title) > W(r) - 40) { cut = n / 2; while (cut < n && title[cut] != ' ') ++cut; }
    copy_text(l1, cut + 1 < sizeof(l1) ? cut + 1 : sizeof(l1), title);
    copy_text(l2, sizeof(l2), cut < n ? title + cut + 1 : "");
    ui_label_center(r.x0 + W(r) / 2, r.y0 + 108, FONT_BODY_BOLD, l1, T.text);
    ui_label_center(r.x0 + W(r) / 2, r.y0 + 126, FONT_BODY_BOLD, l2, T.text);
    ui_label_center(r.x0 + W(r) / 2, r.y0 + 150, FONT_CAPTION,
                    power_confirm == POWER_FAILED ? "Hold the power button to switch it off." :
                    prefs_on_disk() ? "Your notes and settings are saved on MintOS HD first." : "Notes and settings in Session (RAM) are not kept.", T.text2);
    if (power_confirm == POWER_FAILED) { ui_button(alert_button(0), "OK", 1, 1); return; }
    ui_button(alert_button(0), power_confirm == MINT_POWER_ACTION_RESTART ? "Restart" : "Shut Down", 1, 1);
    ui_button(alert_button(1), "Cancel", 0, 1);
}
static void run_power(void) {
    uint32_t action = power_confirm;
    gfx_rect all = R(0, 0, SW, SH);
    gfx_clip_set(all);
    gfx_fill(S, 0, 0, SW, SH, OPAQUE(0x000000u));
    gfx_mask(S, SW / 2 - 40, SH / 2 - 70, 80, 80, mint_icon_mint_leaf_a8, 32, 32, OPAQUE(0xE8E8EAu));
    ui_label_center(SW / 2, SH / 2 + 50, FONT_BODY, action == MINT_POWER_ACTION_RESTART ? "Restarting\xE2\x80\xA6" : "Shutting down\xE2\x80\xA6", 0xA0A0A6u);
    (void)mint_display_cursor(pointer_x, pointer_y, MINT_CURSOR_SHAPE_HIDDEN);
    (void)mint_display_present(0, 0, SW, SH);
    /* Safe shutdown: pending Notes, Terminal history and preferences are
       written and MintFS commits before the kernel's own final flush. */
    app_save_all();
    static const char saved[] = "[MintDE] notes, history and preferences saved before power-off\r\n";
    (void)mint_write(1, saved, sizeof(saved) - 1u);
    static const char msg_r[] = "[MintDE] restart confirmed\r\n", msg_s[] = "[MintDE] shutdown confirmed\r\n";
    if (action == MINT_POWER_ACTION_RESTART) (void)mint_write(1, msg_r, sizeof(msg_r) - 1u);
    else (void)mint_write(1, msg_s, sizeof(msg_s) - 1u);
    (void)mint_power_request(action);
    power_confirm = POWER_FAILED;          /* only reached when every power path was ignored */
    (void)mint_display_cursor(pointer_x, pointer_y, cursor_shape);
    damage_all();
}

/* ------------------------------------------------------------ compose / present */
static void compose(gfx_rect d) {
    gfx_clip_set(d);
    if (setup_active()) { setup_paint(d); return; }  /* full-screen Setup Assistant */
    gfx_copy(S, &gfx_wall, d);
    if (launchpad_open) {
        paint_launchpad();
    } else {
        extras_paint_desktop(d);                       /* ~/Desktop icons, under the windows */
        for (uint32_t i = 0; i < zcount; ++i) paint_window(zorder[i], d);
        gfx_clip_set(d);
        paint_dock();
    }
    gfx_clip_set(d);
    paint_menu_bar();
    paint_menu_dropdown();
    paint_control_center();
    paint_spotlight();
    paint_switcher();
    gfx_clip_set(d);
    extras_paint_overlays();                           /* Notification Center, banners, volume HUD */
    paint_alert();
}
/* Blurred panels sample a 2*radius margin around themselves; make sure that
   margin is recomposed in the same pass so no stale glass feeds back in. */
static gfx_rect expand_for_glass(gfx_rect d) {
    if (gfx_rect_empty(d)) return d;
    gfx_rect panels[12 + APP_COUNT]; uint32_t n = 0; int32_t ic;
    dock_geometry(&panels[n++], &ic);
    panels[n++] = R(0, 0, SW, MENUBAR_H);
    if (active_menu >= 0) { build_menu(active_menu); panels[n++] = menu_rect(); }
    if (control_center_open) panels[n++] = cc_rect();
    if (spotlight_open) { spotlight_search(); panels[n++] = spotlight_rect(); }
    if (power_confirm) panels[n++] = alert_rect();
    n += extras_glass_rects(&panels[n], 6u);
    if (!launchpad_open)
        for (uint32_t i = 0; i < zcount; ++i) {          /* translucent window sidebars sample behind themselves */
            uint32_t app = zorder[i];
            int32_t sw = app_sidebar_width(app);
            if (sw && windows[app].open && !windows[app].minimized) panels[n++] = R(windows[app].x, windows[app].y, sw, windows[app].h);
        }
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < n; ++i)
            if (gfx_rect_overlaps(d, panels[i])) d = gfx_rect_union(d, grow(panels[i], 2 * SIDEBAR_BLUR, 2 * SIDEBAR_BLUR, 2 * SIDEBAR_BLUR, 2 * SIDEBAR_BLUR));
    return gfx_rect_intersect(d, R(0, 0, SW, SH));
}
static void render(void) {
    gfx_rect d = expand_for_glass(gfx_rect_intersect(damage, R(0, 0, SW, SH)));
    damage = R(0, 0, 0, 0);
    if (gfx_rect_empty(d)) return;
    if (launchpad_open) d = R(0, 0, SW, SH);
    compose(d);
    (void)mint_display_present(d.x0, d.y0, W(d), H(d));
}

/* ------------------------------------------------------------ hit testing */
static int32_t window_at(int32_t x, int32_t y) {
    for (uint32_t i = zcount; i-- > 0;) {
        uint32_t app = zorder[i];
        if (!windows[app].minimized && gfx_rect_contains(window_frame(app), x, y)) return (int32_t)app;
    }
    return -1;
}
static int lights_hit(uint32_t app, int32_t x, int32_t y) {
    gfx_rect f = window_frame(app);
    int32_t cy = f.y0 + window_bar(app) / 2;
    if (y < cy - 9 || y > cy + 9) return -1;
    for (int i = 0; i < 3; ++i) { int32_t cx = f.x0 + 20 + i * 20; if (x >= cx - 9 && x <= cx + 9) return i; }
    return -1;
}
static int32_t resize_edges_at(uint32_t app, int32_t x, int32_t y) {
    gfx_rect f = window_frame(app);
    if (app == APP_ABOUT_OS || app == APP_ABOUT_MINTDE || app == APP_CALCULATOR) return 0;
    int32_t e = 0;
    if (x >= f.x1 - 5 && x < f.x1 + 3) e |= 1;
    if (y >= f.y1 - 5 && y < f.y1 + 3) e |= 2;
    if (x >= f.x1 - 14 && y >= f.y1 - 14) e |= 3;
    return e;
}

static void update_cursor(void) {
    uint32_t shape = MINT_CURSOR_SHAPE_ARROW;
    if (resize_app >= 0) shape = resize_edges == 1 ? MINT_CURSOR_SHAPE_RESIZE_EW : resize_edges == 2 ? MINT_CURSOR_SHAPE_RESIZE_NS : MINT_CURSOR_SHAPE_RESIZE_NWSE;
    else if (!launchpad_open && !power_confirm && active_menu < 0 && !spotlight_open && pointer_y >= MENUBAR_H) {
        int32_t app = window_at(pointer_x, pointer_y);
        int32_t probe = app;
        if (app < 0) for (uint32_t i = zcount; i-- > 0;) {
            uint32_t a = zorder[i];
            if (!windows[a].minimized && resize_edges_at(a, pointer_x, pointer_y)) { probe = (int32_t)a; break; }
        }
        if (probe >= 0) {
            int32_t e = resize_edges_at((uint32_t)probe, pointer_x, pointer_y);
            if (e) shape = e == 1 ? MINT_CURSOR_SHAPE_RESIZE_EW : e == 2 ? MINT_CURSOR_SHAPE_RESIZE_NS : MINT_CURSOR_SHAPE_RESIZE_NWSE;
            else if (app >= 0) { mde_layout l = window_layout((uint32_t)app); shape = app_cursor((uint32_t)app, &l, pointer_x, pointer_y); }
        }
    }
    cursor_shape = shape;
    (void)mint_display_cursor(pointer_x, pointer_y, shape);
}

static void pointer_moved(void) {
    if (extras_pointer_moved(pointer_x, pointer_y)) return;     /* Control Center volume drag */
    if (drag_app >= 0) {
        mde_window *w = &windows[drag_app];
        shell_damage_window((uint32_t)drag_app);
        w->x = pointer_x - drag_dx; w->y = pointer_y - drag_dy;
        if (w->y < MENUBAR_H) w->y = MENUBAR_H;
        if (w->y > SH - 60) w->y = SH - 60;
        if (w->x > SW - 80) w->x = SW - 80;
        if (w->x + w->w < 80) w->x = 80 - w->w;
        w->zoomed = 0;
        shell_damage_window((uint32_t)drag_app);
        return;
    }
    if (resize_app >= 0) {
        mde_window *w = &windows[resize_app];
        shell_damage_window((uint32_t)resize_app);
        if (resize_edges & 1) { w->w = resize_w0 + pointer_x - resize_x0; if (w->w < 380) w->w = 380; if (w->x + w->w > SW) w->w = SW - w->x; }
        if (resize_edges & 2) { w->h = resize_h0 + pointer_y - resize_y0; if (w->h < 260) w->h = 260; if (w->y + w->h > SH) w->h = SH - w->y; }
        w->zoomed = 0;
        shell_damage_window((uint32_t)resize_app);
        return;
    }
    /* hover states that change pixels */
    if (active_menu >= 0) {
        if (pointer_y < MENUBAR_H) {
            for (int32_t m = 0; m < 7; ++m) {
                int32_t x0 = m == 0 ? 8 : menu_title_x(m) - 9;
                if (pointer_x >= x0 && pointer_x < x0 + (m == 0 ? 34 : menu_title_w(m)) && m != active_menu) {
                    build_menu(active_menu); shell_damage(grow(menu_rect(), 30, 30, 30, 40));
                    active_menu = m; hovered_menu_item = -1; build_menu(m);
                    shell_damage(grow(menu_rect(), 30, 30, 30, 40)); shell_damage(R(0, 0, SW, MENUBAR_H));
                }
            }
        }
        build_menu(active_menu);
        int32_t hi = -1;
        for (uint32_t i = 0; i < menu_count; ++i) if (gfx_rect_contains(menu_item_rect(i), pointer_x, pointer_y)) hi = (int32_t)i;
        if (hi != hovered_menu_item) { hovered_menu_item = hi; shell_damage(menu_rect()); }
    }
    int32_t lights = -1;
    if (focused_app >= 0 && !launchpad_open) {
        int32_t top = window_at(pointer_x, pointer_y);
        if (top >= 0 && lights_hit((uint32_t)top, pointer_x, pointer_y) >= 0) lights = top;
    }
    if (lights != hover_lights) {
        gfx_rect a = hover_lights >= 0 ? window_frame((uint32_t)hover_lights) : R(0, 0, 0, 0);
        gfx_rect b = lights >= 0 ? window_frame((uint32_t)lights) : R(0, 0, 0, 0);
        if (hover_lights >= 0) shell_damage(R(a.x0, a.y0, 80, 52));
        if (lights >= 0) shell_damage(R(b.x0, b.y0, 80, 52));
        hover_lights = lights;
    }
    int32_t dh = launchpad_open || power_confirm || active_menu >= 0 ? -1 : dock_hit(pointer_x, pointer_y);
    if (dh < 0) dh = -1;
    if (dh != hover_dock) {
        if (hover_dock >= 0) shell_damage(grow(dock_tooltip_rect(), 12, 12, 12, 16));
        hover_dock = dh;
        if (hover_dock >= 0) shell_damage(grow(dock_tooltip_rect(), 12, 12, 12, 16));
    }
}

static void close_overlays(void) {
    extras_close_overlays();
    if (active_menu >= 0 || control_center_open || spotlight_open) damage_all();
    active_menu = -1; hovered_menu_item = -1; control_center_open = 0; spotlight_open = 0;
}

static void pointer_pressed(uint32_t clicks) {
    int32_t x = pointer_x, y = pointer_y;
    if (power_confirm) {
        if (gfx_rect_contains(alert_button(0), x, y)) {
            if (power_confirm == POWER_FAILED) { power_confirm = 0; damage_all(); }
            else run_power();
        } else if (power_confirm != POWER_FAILED && gfx_rect_contains(alert_button(1), x, y)) { power_confirm = 0; damage_all(); }
        return;
    }
    if (extras_pointer_pressed(x, y, clicks, 0)) return;     /* Notification Center and banners */
    if (spotlight_open) {
        gfx_rect r = spotlight_rect();
        if (gfx_rect_contains(r, x, y)) {
            if (y >= r.y0 + 62) spotlight_activate((y - r.y0 - 62) / 44);
            damage_all(); return;
        }
        spotlight_open = 0; damage_all();
        if (!gfx_rect_contains(spotlight_icon_rect(), x, y)) return;
    }
    if (control_center_open) {
        if (control_center_click(x, y)) { damage_all(); return; }
        control_center_open = 0; damage_all();
        if (gfx_rect_contains(control_icon_rect(), x, y)) return;
    }
    if (active_menu >= 0) {
        build_menu(active_menu);
        for (uint32_t i = 0; i < menu_count; ++i)
            if (gfx_rect_contains(menu_item_rect(i), x, y)) { if (menu_items[i].enabled) run_action(menu_items[i].action); return; }
        int on_bar = y < MENUBAR_H;
        active_menu = -1; hovered_menu_item = -1; damage_all();
        if (!on_bar) return;
    }
    if (y < MENUBAR_H && !launchpad_open) {
        if (gfx_rect_contains(clock_rect(), x, y)) { control_center_open = 0; extras_toggle_notification_center(); return; }
        if (gfx_rect_contains(spotlight_icon_rect(), x, y)) { spotlight_open = 1; spotlight_query[0] = '\0'; spotlight_sel = 0; damage_all(); return; }
        if (gfx_rect_contains(control_icon_rect(), x, y)) { control_center_open = 1; damage_all(); return; }
        for (int32_t m = 0; m < 7; ++m) {
            int32_t x0 = m == 0 ? 8 : menu_title_x(m) - 9;
            if (x >= x0 && x < x0 + (m == 0 ? 34 : menu_title_w(m))) { active_menu = m; hovered_menu_item = -1; damage_all(); return; }
        }
        return;
    }
    if (launchpad_open) {
        uint32_t n = launchpad_count();
        for (uint32_t i = 0; i < n; ++i) {
            gfx_rect c = launchpad_cell(i, n);
            if (gfx_rect_contains(R(c.x0 + 15, c.y0 + 4, 120, 140), x, y)) { launchpad_open = 0; shell_open_app(launchpad_apps[i]); return; }
        }
        launchpad_open = 0; damage_all();
        return;
    }
    int32_t dh = dock_hit(x, y);
    if (dh >= 0) {
        if (dh == DOCK_TRASH) { if (files_show_trash()) shell_open_app(APP_FILES); return; }
        uint32_t app = dock_apps[dh];
        if (windows[app].open && focused_app == (int32_t)app && !windows[app].minimized && app != APP_LAUNCHPAD) return;
        shell_open_app(app);
        return;
    }
    if (dh == -2) return;
    /* resize handles sit just outside the frame too */
    for (uint32_t i = zcount; i-- > 0;) {
        uint32_t a = zorder[i];
        if (windows[a].minimized) continue;
        int32_t e = resize_edges_at(a, x, y);
        if (e && (window_at(x, y) == (int32_t)a || window_at(x, y) < 0)) {
            raise_window(a);
            resize_app = (int32_t)a; resize_edges = e; resize_x0 = x; resize_y0 = y;
            resize_w0 = windows[a].w; resize_h0 = windows[a].h;
            return;
        }
        if (gfx_rect_contains(window_frame(a), x, y)) break;
    }
    int32_t app = window_at(x, y);
    if (app < 0) {
        if (extras_pointer_pressed(x, y, clicks, 1)) return;     /* desktop items */
        if (focused_app >= 0) { shell_damage_window((uint32_t)focused_app); focused_app = -1; shell_damage(R(0, 0, SW, MENUBAR_H)); }
        return;
    }
    if (focused_app != app) raise_window((uint32_t)app);
    mde_layout l = window_layout((uint32_t)app);
    int light = lights_hit((uint32_t)app, x, y);
    if (light == 0) { shell_close_app((uint32_t)app); return; }
    if (light == 1) { minimize_app((uint32_t)app); return; }
    if (light == 2) { zoom_app((uint32_t)app); return; }
    int handled = app_click((uint32_t)app, &l, x, y, clicks);
    if (handled) { shell_damage_window((uint32_t)app); return; }
    if (y < l.frame.y0 + window_bar((uint32_t)app) || app_chrome((uint32_t)app) == CHROME_PLAIN) {
        if (clicks >= 2 && y < l.frame.y0 + window_bar((uint32_t)app)) { zoom_app((uint32_t)app); return; }
        drag_app = app; drag_dx = x - windows[app].x; drag_dy = y - windows[app].y;
    }
    shell_damage(R(0, 0, SW, MENUBAR_H));
}

static void pointer_released(void) {
    extras_pointer_released();
    if (drag_app >= 0 || resize_app >= 0) damage_all();
    drag_app = -1; resize_app = -1;
}

/* ------------------------------------------------------------ keyboard */
static void key_pressed(const MINT_DESKTOP_KEY_EVENT *k) {
    uint32_t ch = k->codepoint;
    int cmd = (k->modifiers & MINT_KEYMOD_GUI) != 0;
    if (k->scancode == 0x15Bu || k->scancode == 0x15Cu) return;   /* Command alone */
    if (power_confirm) {
        if (ch == 27u && power_confirm != POWER_FAILED) { power_confirm = 0; damage_all(); }
        else if (ch == '\n') { if (power_confirm == POWER_FAILED) { power_confirm = 0; damage_all(); } else run_power(); }
        return;
    }
    /* volume keys, screenshots; desktop items only when nothing else has keyboard focus */
    if (extras_key(k, (focused_app >= 0 && !launchpad_open) || spotlight_open || launchpad_open || active_menu >= 0 ||
                      control_center_open || switcher_open)) return;
    if (cmd && ch == 9u) {                                   /* Command-Tab */
        if (!zcount) return;
        if (!switcher_open) { switcher_open = 1; switcher_index = zcount > 1 ? 1u : 0u; }
        else switcher_index = (switcher_index + 1u) % zcount;
        damage_all(); return;
    }
    if (cmd && ch == ' ') { spotlight_open = !spotlight_open; spotlight_query[0] = '\0'; spotlight_sel = 0; damage_all(); return; }
    if (spotlight_open) {
        size_t n = text_length(spotlight_query);
        if (ch == 27u) spotlight_open = 0;
        else if (ch == '\n') spotlight_activate(spotlight_sel);
        else if (ch == 8u) { if (n) spotlight_query[n - 1u] = '\0'; spotlight_sel = 0; }
        else if (k->scancode == 0x150u) ++spotlight_sel;
        else if (k->scancode == 0x148u) { if (spotlight_sel > 0) --spotlight_sel; }
        else if (ch >= 32u && ch < 127u && n + 1u < sizeof(spotlight_query)) { spotlight_query[n] = (char)ch; spotlight_query[n + 1u] = '\0'; spotlight_sel = 0; }
        spotlight_search();
        if (spotlight_sel >= (int32_t)spot_count) spotlight_sel = spot_count ? (int32_t)spot_count - 1 : 0;
        damage_all(); return;
    }
    if (ch == 27u && (launchpad_open || active_menu >= 0 || control_center_open)) { launchpad_open = 0; close_overlays(); damage_all(); return; }
    if (cmd) {
        uint32_t app = current_app_for_menu();
        switch (ch) {
        case 'q': case 'Q': case 'w': case 'W': if (focused_app >= 0) shell_close_app(app); return;
        case 'm': case 'M': case 'h': case 'H': if (focused_app >= 0) minimize_app(app); return;
        case ',': shell_open_app(APP_SETTINGS); return;
        case 'n': case 'N':
            if ((k->modifiers & MINT_KEYMOD_SHIFT) && focused_app == APP_FILES) break;   /* New Folder */
            if (focused_app == APP_NOTES) break;                                         /* New Note */
            shell_open_app(APP_FILES); return;
        case 'l': case 'L': if (focused_app == APP_WEB) break; shell_open_app(APP_LAUNCHPAD); return;
        default: break;
        }
    }
    if (focused_app >= 0 && !launchpad_open) {
        mde_layout l = window_layout((uint32_t)focused_app);
        if (app_key((uint32_t)focused_app, &l, k)) shell_damage_window((uint32_t)focused_app);
    }
}
static void key_released(const MINT_DESKTOP_KEY_EVENT *k) {
    if ((k->scancode == 0x15Bu || k->scancode == 0x15Cu) && switcher_open) {
        switcher_open = 0;
        if (switcher_index < zcount) shell_open_app(zorder[zcount - 1u - switcher_index]);
        damage_all();
    }
}

static void handle_event(const MINT_DESKTOP_EVENT *e) {
    if (setup_active()) {                             /* the Setup Assistant owns all input */
        if (e->type == MINT_DESKTOP_EVENT_POINTER_MOVE || e->type == MINT_DESKTOP_EVENT_POINTER_BUTTON) {
            pointer_x = e->data.pointer.x; pointer_y = e->data.pointer.y;
            if (e->type == MINT_DESKTOP_EVENT_POINTER_BUTTON && (e->data.pointer.changed_buttons & 1u) && (e->data.pointer.buttons & 1u))
                setup_click(pointer_x, pointer_y);
            (void)mint_display_cursor(pointer_x, pointer_y, setup_cursor(pointer_x, pointer_y));
        } else if (e->type == MINT_DESKTOP_EVENT_KEY && e->data.key.pressed) setup_key(&e->data.key);
        if (!setup_active()) update_cursor();
        return;
    }
    if (e->type == MINT_DESKTOP_EVENT_POINTER_MOVE || e->type == MINT_DESKTOP_EVENT_POINTER_BUTTON) {
        pointer_x = e->data.pointer.x; pointer_y = e->data.pointer.y;
        pointer_moved();
        if (e->type == MINT_DESKTOP_EVENT_POINTER_BUTTON && (e->data.pointer.changed_buttons & 1u)) {
            if (e->data.pointer.buttons & 1u) {
                uint32_t clicks = 1;
                int32_t dx = pointer_x - last_click_x, dy = pointer_y - last_click_y;
                if (e->timestamp_ms - last_click_ms < 450u && dx * dx + dy * dy < 25) clicks = 2;
                last_click_ms = clicks == 2 ? 0 : e->timestamp_ms; last_click_x = pointer_x; last_click_y = pointer_y;
                pointer_pressed(clicks);
            } else pointer_released();
        }
        update_cursor();
    } else if (e->type == MINT_DESKTOP_EVENT_KEY) {
        if (e->data.key.pressed) key_pressed(&e->data.key); else key_released(&e->data.key);
    } else if (e->type == MINT_DESKTOP_EVENT_SCROLL) {
        /* Mouse wheel: scroll the window under the pointer (macOS behaviour). */
        if (launchpad_open || active_menu >= 0 || spotlight_open || control_center_open) return;
        int32_t app = window_at(pointer_x, pointer_y);
        if (app >= 0) {
            mde_layout l = window_layout((uint32_t)app);
            if (app_scroll((uint32_t)app, &l, pointer_x, pointer_y, e->data.pointer.dy)) shell_damage(l.content);
        }
    }
}

static void tick(void) {
    int64_t epoch = shell_now_epoch();
    int64_t minute = epoch ? epoch / 60 : 0;
    if (!epoch) { mint_timespec64 up = {0, 0}; (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &up); minute = up.tv_sec / 60; }
    if (minute != last_clock_minute) {
        last_clock_minute = minute;
        shell_damage(R(SW - 300, 0, 300, MENUBAR_H));
        int night = is_night();
        if (desktop_settings.theme == MINT_THEME_WARM && night != night_mode) (void)shell_apply_settings();
    }
    (void)setup_tick();
    extras_tick();
    uint32_t background = app_background();
    for (uint32_t app = 0; background && app < APP_COUNT; ++app)
        if ((background & (1u << app)) && windows[app].open && !windows[app].minimized) shell_damage_window(app);
    if (focused_app >= 0 && app_tick((uint32_t)focused_app)) {
        mde_layout l = window_layout((uint32_t)focused_app);
        shell_damage(l.content);
    }
}

int64_t mint_user_main(void) {
    int boot_animation = personality_boot_handoff();      /* stop the kernel boot animation first */
    if (mint_display_map_canvas(&canvas) != 0 || canvas.version != MINT_DISPLAY_CANVAS_VERSION ||
        !canvas.width || !canvas.height || canvas.stride_pixels < canvas.width) {
        static const char failed[] = "[user-desktop] shared display canvas unavailable; MintDE cannot start\n";
        (void)mint_write(1, failed, sizeof(failed) - 1u);
        return 1;
    }
    SW = (int32_t)canvas.width; SH = (int32_t)canvas.height;
    gfx_screen = (gfx_surface){(uint32_t *)(uintptr_t)canvas.canvas, SW, SH, (int32_t)canvas.stride_pixels};
    gfx_wall = (gfx_surface){(uint32_t *)(uintptr_t)canvas.wallpaper, SW, SH, (int32_t)canvas.stride_pixels};
    gfx_scratch = (gfx_surface){(uint32_t *)(uintptr_t)canvas.scratch, SW, SH, (int32_t)canvas.stride_pixels};
    if (mint_desktop_settings_get(&desktop_settings) != 0) {
        desktop_settings.version = MINT_DESKTOP_SETTINGS_VERSION; desktop_settings.theme = MINT_THEME_LIGHT;
        desktop_settings.accent = MINT_ACCENT_MINT; desktop_settings.ui_scale = 1u;
    }
    userdata_start();                                 /* account + saved preferences from MintOS HD */
    night_mode = is_night();
    theme_apply(&desktop_settings, night_mode);
    personality_init();
    render_wallpaper();
    app_init();
    extras_init(SW, SH);
    setup_boot_check();                               /* first boot: Setup Assistant */
    pointer_x = SW / 2; pointer_y = SH / 2;
    damage_all();
    /* Compose the first frame without presenting it: personality_startup()
       crossfades into it from the boot animation (or presents it at once). */
    compose(R(0, 0, SW, SH));
    damage = R(0, 0, 0, 0);
    personality_startup(boot_animation);                 /* startup chime + crossfade */
    (void)mint_display_cursor(pointer_x, pointer_y, MINT_CURSOR_SHAPE_ARROW);
    static const char ready[] = "[user-desktop] MintDE " MINTDE_VERSION "; macOS-style shell, shared canvas, Dock, Launchpad, Spotlight and Control Center ready\n";
    (void)mint_write(1, ready, sizeof(ready) - 1u);
    for (;;) {
        MINT_DESKTOP_EVENT events[32];
        uint32_t count = 0;
        while (count < 32u) {
            MINT_DESKTOP_EVENT ev = {0};
            if (mint_desktop_event_poll(&ev) != 1) break;
            events[count++] = ev;
        }
        for (uint32_t i = 0; i < count;) {
            if (events[i].type == MINT_DESKTOP_EVENT_POINTER_MOVE) {
                uint32_t last = i;
                while (last + 1u < count && events[last + 1u].type == MINT_DESKTOP_EVENT_POINTER_MOVE) ++last;
                handle_event(&events[last]);
                i = last + 1u;
            } else handle_event(&events[i++]);
        }
        tick();
        render();
        if (!count) (void)mint_sleep_ticks(1u);
        else (void)mint_sched_yield();
    }
}
