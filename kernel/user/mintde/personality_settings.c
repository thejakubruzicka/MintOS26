/* MintDE 26.5.014.0 Settings panes: Sound, Notifications and the Wallpaper
 * picker (thumbnails rendered live by the wallpaper renderer), plus the
 * slider control they and Control Center share. apps.c hands each pane its
 * body rectangle; row geometry here is used for both painting and clicks. */
#include "mintde.h"

#define S (&gfx_screen)
#define ROW 40

static gfx_rect R(int32_t x, int32_t y, int32_t w, int32_t h) { return gfx_rect_xywh(x, y, w, h); }
static int32_t W(gfx_rect r) { return r.x1 - r.x0; }
static int32_t H(gfx_rect r) { return r.y1 - r.y0; }
static int32_t BL(gfx_rect r, int font) { return r.y0 + (H(r) - gfx_font_line(font)) / 2 + gfx_font_ascent(font); }
static int in(gfx_rect r, int32_t x, int32_t y) { return gfx_rect_contains(r, x, y); }

static const char *const alert_names[MINT_SOUND_ALERT_COUNT] = {"Mint", "Dewdrop", "Fern", "Ripple", "Woodblock"};

/* ------------------------------------------------------------ slider */
void ui_slider(gfx_rect r, uint32_t value, uint32_t max, int enabled) {
    if (!max) max = 1u;
    if (value > max) value = max;
    int32_t cy = r.y0 + H(r) / 2, x0 = r.x0 + 9, x1 = r.x1 - 9;
    int32_t kx = x0 + (int32_t)((int64_t)(x1 - x0) * value / max);
    gfx_round_rect(S, x0 - 2, cy - 2, x1 - x0 + 4, 4, 2, OPAQUE(T.dark ? 0x4A4A4Eu : 0xD8D8DCu));
    if (kx > x0) gfx_round_rect(S, x0 - 2, cy - 2, kx - x0 + 2, 4, 2, OPAQUE(enabled ? T.accent : T.text3));
    gfx_shadow(S, kx - 9, cy - 9, 18, 18, 9, 3, 1, ARGB(0x40, 0, 0, 0));
    gfx_circle(S, kx, cy, 9, OPAQUE(0xFFFFFFu));
    gfx_circle_stroke(S, kx, cy, 9, T.control_border);
}
uint32_t ui_slider_value(gfx_rect r, int32_t x, uint32_t max) {
    int32_t x0 = r.x0 + 9, x1 = r.x1 - 9;
    if (x <= x0) return 0;
    if (x >= x1) return max;
    return (uint32_t)(((int64_t)(x - x0) * max + (x1 - x0) / 2) / (x1 - x0));
}

static void row_text(gfx_rect g, int32_t i, const char *label, const char *sub) {
    gfx_rect r = R(g.x0, g.y0 + i * ROW, W(g), ROW);
    if (i) gfx_hline(S, r.x0 + 12, r.y0, W(r) - 24, T.separator);
    if (sub) {
        ui_label(r.x0 + 14, r.y0 + 17, FONT_BODY, label, T.text);
        ui_label(r.x0 + 14, r.y0 + 32, FONT_CAPTION, sub, T.text2);
    } else ui_label(r.x0 + 14, BL(r, FONT_BODY), FONT_BODY, label, T.text);
}
static void row_switch(gfx_rect g, int32_t i, const char *label, const char *sub, int on) {
    row_text(g, i, label, sub);
    ui_switch(g.x1 - 52, g.y0 + i * ROW + 9, on);
}
static int row_switch_hit(gfx_rect g, int32_t i, int32_t x, int32_t y) {
    return in(R(g.x1 - 70, g.y0 + i * ROW, 70, ROW), x, y);
}
static void header(gfx_rect *b, const char *text) {
    ui_label(b->x0 + 4, b->y0 + 14, FONT_CAPTION_BOLD, text, T.text2);
    b->y0 += 22;
}

/* ------------------------------------------------------------ Sound */
static void output_text(const MINT_AUDIO_INFO *ai, char *out, size_t cap) {
    if (ai->state != MINT_AUDIO_RUNNING || !ai->output_count) {
        copy_text(out, cap, ai->state == MINT_AUDIO_ABSENT ? "None" : "Not started");
        return;
    }
    static const char *const kinds[3] = {"Line Out", "Speakers", "Headphones"};
    out[0] = '\0';
    for (uint32_t i = 0; i < ai->output_count && i < MINT_AUDIO_MAX_OUTPUTS; ++i) {
        if (i) str_append(out, cap, ", ");
        str_append(out, cap, kinds[ai->output_kind[i] < 3u ? ai->output_kind[i] : 0u]);
    }
    str_append(out, cap, " \xC2\xB7 ");
    str_append(out, cap, ai->codec_name);
}

static gfx_rect sound_effects_group(gfx_rect b) { return R(b.x0, b.y0 + 22, W(b), 5 * ROW); }
static gfx_rect sound_output_group(gfx_rect b) { gfx_rect g = sound_effects_group(b); return R(b.x0, g.y1 + 18 + 22, W(b), 4 * ROW); }
static gfx_rect sound_button(gfx_rect b) { gfx_rect g = sound_output_group(b); return R(b.x0, g.y1 + 16, 170, 28); }

void settings_sound_paint(gfx_rect body) {
    gfx_rect b = body;
    header(&b, "SOUND EFFECTS");
    gfx_rect g = sound_effects_group(body);
    ui_group(g);
    row_text(g, 0, "Alert sound", 0);
    ui_popup(R(g.x1 - 174, g.y0 + 7, 160, 26), alert_names[personality_prefs.alert_sound % MINT_SOUND_ALERT_COUNT], 1);
    row_text(g, 1, "Alert volume", 0);
    ui_slider(R(g.x1 - 254, g.y0 + ROW, 240, ROW), personality_prefs.alert_volume, 100u, 1);
    row_switch(g, 2, "Play sound on startup", 0, (int)personality_prefs.startup_sound);
    row_switch(g, 3, "Play user interface sound effects", 0, (int)personality_prefs.ui_sounds);
    row_switch(g, 4, "Play feedback when volume is changed", 0, (int)personality_prefs.volume_feedback);
    b.y0 = g.y1 + 18;
    header(&b, "OUTPUT");
    gfx_rect o = sound_output_group(body);
    ui_group(o);
    MINT_AUDIO_INFO ai;
    int have = sound_info(&ai);
    row_text(o, 0, "Output volume", 0);
    ui_slider(R(o.x1 - 254, o.y0, 240, ROW), personality_prefs.muted ? 0u : personality_prefs.volume, 100u, !personality_prefs.muted);
    row_switch(o, 1, "Mute", 0, (int)personality_prefs.muted);
    char dev[96], st[96];
    if (have) output_text(&ai, dev, sizeof(dev)); else copy_text(dev, sizeof(dev), "Unknown");
    row_text(o, 2, "Output device", 0);
    gfx_text_fit(S, FONT_BODY, o.x0 + 160, BL(R(o.x0, o.y0 + 2 * ROW, W(o), ROW), FONT_BODY), dev, W(o) - 174, OPAQUE(T.text2));
    copy_text(st, sizeof(st), have ? ai.status : "The kernel did not answer");
    row_text(o, 3, "Driver", 0);
    gfx_text_fit(S, FONT_BODY, o.x0 + 160, BL(R(o.x0, o.y0 + 3 * ROW, W(o), ROW), FONT_BODY), st, W(o) - 174, OPAQUE(T.text2));
    gfx_rect btn = sound_button(body);
    if (have && (ai.state == MINT_AUDIO_FOUND || ai.state == MINT_AUDIO_STOPPED || ai.state == MINT_AUDIO_FAILED)) {
        ui_button(btn, "Start Audio Driver", 1, 1);
        ui_label(btn.x1 + 14, BL(btn, FONT_CAPTION), FONT_CAPTION,
                 ai.experimental ? "Experimental on this computer (like the I219 network driver)." : "Intel High Definition Audio", T.text2);
    } else if (have && ai.state == MINT_AUDIO_RUNNING) {
        ui_button(btn, "Play Startup Sound", 0, 1);
        ui_label(btn.x1 + 14, BL(btn, FONT_CAPTION), FONT_CAPTION, "Original MintOS sounds, 48 kHz stereo.", T.text2);
    } else {
        ui_label(b.x0 + 4, btn.y0 + 18, FONT_CAPTION, "No High Definition Audio controller was found.", T.text2);
    }
}

int settings_sound_click(gfx_rect body, int32_t x, int32_t y) {
    gfx_rect g = sound_effects_group(body), o = sound_output_group(body);
    if (in(R(g.x1 - 174, g.y0 + 7, 160, 26), x, y)) {
        personality_prefs.alert_sound = (personality_prefs.alert_sound + 1u) % MINT_SOUND_ALERT_COUNT;
        personality_prefs_apply();
        ui_sound_alert_preview(personality_prefs.alert_sound);
        return 1;
    }
    if (in(R(g.x1 - 260, g.y0 + ROW, 252, ROW), x, y)) {
        personality_prefs.alert_volume = ui_slider_value(R(g.x1 - 254, g.y0 + ROW, 240, ROW), x, 100u);
        personality_prefs_apply();
        ui_sound_alert_preview(personality_prefs.alert_sound);
        return 1;
    }
    if (row_switch_hit(g, 2, x, y)) { personality_prefs.startup_sound ^= 1u; personality_prefs_apply(); return 1; }
    if (row_switch_hit(g, 3, x, y)) { personality_prefs.ui_sounds ^= 1u; personality_prefs_apply(); return 1; }
    if (row_switch_hit(g, 4, x, y)) { personality_prefs.volume_feedback ^= 1u; personality_prefs_apply(); return 1; }
    if (in(R(o.x1 - 260, o.y0, 252, ROW), x, y)) {
        sound_volume_set((int32_t)ui_slider_value(R(o.x1 - 254, o.y0, 240, ROW), x, 100u), 0);
        return 1;
    }
    if (row_switch_hit(o, 1, x, y)) { personality_prefs.muted ^= 1u; personality_prefs_apply(); return 1; }
    if (in(sound_button(body), x, y)) {
        MINT_AUDIO_INFO ai;
        if (!sound_info(&ai)) return 1;
        if (ai.state == MINT_AUDIO_RUNNING) { (void)mint_audio_play(MINT_SOUND_STARTUP, 1000u); return 1; }
        if (ai.state != MINT_AUDIO_ABSENT) {
            int64_t rc = mint_audio_control(MINT_AUDIO_OP_START, MINT_AUDIO_START_TOKEN);
            if (rc == 0) { personality_prefs_apply(); notify_post(APP_SETTINGS, "Sound", "The audio driver is running."); }
            else { ui_sound(UI_SOUND_ALERT); notify_post(APP_SETTINGS, "Sound", "The audio driver could not start. See Storage & Logs."); }
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ Notifications */
static gfx_rect notif_group(gfx_rect b) { return R(b.x0, b.y0 + 22, W(b), 4 * ROW); }
static gfx_rect notif_test(gfx_rect b) { gfx_rect g = notif_group(b); return R(b.x0, g.y1 + 18, 180, 28); }
static gfx_rect notif_clear(gfx_rect b) { gfx_rect t = notif_test(b); return R(t.x1 + 12, t.y0, 110, 28); }

void settings_notifications_paint(gfx_rect body) {
    gfx_rect b = body;
    header(&b, "NOTIFICATIONS");
    gfx_rect g = notif_group(body);
    ui_group(g);
    row_switch(g, 0, "Show notification banners", "Banners appear at the top right and go away after a few seconds.", (int)personality_prefs.banners);
    row_switch(g, 1, "Do Not Disturb", "Notifications arrive silently in Notification Center.", (int)personality_prefs.do_not_disturb);
    row_switch(g, 2, "Play sound for notifications", 0, (int)personality_prefs.notification_sound);
    char v[48]; format_u64(v, sizeof(v), notify_count());
    str_append(v, sizeof(v), notify_count() == 1u ? " notification" : " notifications");
    row_text(g, 3, "Notification Center", 0);
    ui_label_right(g.x1 - 14, BL(R(g.x0, g.y0 + 3 * ROW, W(g), ROW), FONT_BODY), FONT_BODY, v, T.text2);
    ui_button(notif_test(body), "Send Test Notification", 0, 1);
    ui_button(notif_clear(body), "Clear All", 0, notify_count() > 0u);
    gfx_rect t = notif_test(body);
    ui_label(b.x0 + 4, t.y1 + 26, FONT_CAPTION, "Click the date and time in the menu bar to open Notification Center.", T.text2);
    ui_label(b.x0 + 4, t.y1 + 44, FONT_CAPTION, "Disk Utility, Files, Terminal and screenshots post notifications.", T.text2);
}

int settings_notifications_click(gfx_rect body, int32_t x, int32_t y) {
    gfx_rect g = notif_group(body);
    if (row_switch_hit(g, 0, x, y)) { personality_prefs.banners ^= 1u; personality_prefs_apply(); return 1; }
    if (row_switch_hit(g, 1, x, y)) { personality_prefs.do_not_disturb ^= 1u; personality_prefs_apply(); return 1; }
    if (row_switch_hit(g, 2, x, y)) { personality_prefs.notification_sound ^= 1u; personality_prefs_apply(); return 1; }
    if (in(notif_test(body), x, y)) { notify_post(APP_SETTINGS, "Hello from MintOS", "Notifications are working. Click to open Settings."); return 1; }
    if (in(notif_clear(body), x, y)) { notify_clear_all(); return 1; }
    return 0;
}

/* ------------------------------------------------------------ Wallpaper */
#define WP_COLS 5
static gfx_rect wp_hero(gfx_rect b) { return R(b.x0, b.y0 + 4, 212, 120); }
static gfx_rect wp_thumb(gfx_rect b, uint32_t i) {
    int32_t gap = 12, tw = (W(b) - gap * (WP_COLS - 1)) / WP_COLS, th = tw * 9 / 16;
    int32_t y0 = wp_hero(b).y1 + 40;
    return R(b.x0 + (int32_t)(i % WP_COLS) * (tw + gap), y0 + (int32_t)(i / WP_COLS) * (th + 34), tw, th);
}

/* Render a wallpaper thumbnail straight into the canvas, clipped to the
   current damage region, with rounded corners. */
static void thumb(gfx_rect r, uint32_t id, int selected) {
    gfx_rect clip = gfx_rect_intersect(gfx_clip_get(), r);
    if (selected) gfx_round_rect(S, r.x0 - 4, r.y0 - 4, W(r) + 8, H(r) + 8, 10, OPAQUE(T.accent));
    if (gfx_rect_empty(clip)) return;
    static uint32_t corners[4 * 7 * 7];
    gfx_corners_save(S, r.x0, r.y0, W(r), H(r), 7, corners);
    uint32_t minute = 720u;
    int64_t e = shell_now_epoch();
    if (e > 0) minute = (uint32_t)((e % 86400) / 60);
    if (id == WP_MINT_WAVES_AUTO) {
        int32_t half = r.x0 + W(r) / 2;
        gfx_rect left = gfx_rect_intersect(clip, R(r.x0, r.y0, half - r.x0, H(r)));
        gfx_rect right = gfx_rect_intersect(clip, R(half, r.y0, r.x1 - half, H(r)));
        if (!gfx_rect_empty(left))
            wallpaper_render(S->px + (int64_t)left.y0 * S->stride + left.x0, S->stride, W(r), H(r),
                             left.x0 - r.x0, left.y0 - r.y0, left.x1 - r.x0, left.y1 - r.y0, WP_MINT_WAVES_LIGHT, minute);
        if (!gfx_rect_empty(right))
            wallpaper_render(S->px + (int64_t)right.y0 * S->stride + right.x0, S->stride, W(r), H(r),
                             right.x0 - r.x0, right.y0 - r.y0, right.x1 - r.x0, right.y1 - r.y0, WP_MINT_WAVES_DARK, minute);
    } else {
        wallpaper_render(S->px + (int64_t)clip.y0 * S->stride + clip.x0, S->stride, W(r), H(r),
                         clip.x0 - r.x0, clip.y0 - r.y0, clip.x1 - r.x0, clip.y1 - r.y0, id, minute);
    }
    gfx_corners_restore(S, r.x0, r.y0, W(r), H(r), 7, corners);
    gfx_round_rect_stroke(S, r.x0, r.y0, W(r), H(r), 7, T.separator);
}

void settings_wallpaper_paint(gfx_rect body) {
    uint32_t cur = personality_prefs.wallpaper;
    gfx_rect hero = wp_hero(body);
    thumb(hero, cur, 0);
    ui_label(hero.x1 + 18, hero.y0 + 30, FONT_TITLE, wallpaper_name(cur), T.text);
    const char *what = wallpaper_is_dynamic(cur) ? "Dynamic: dawn, day, dusk and night follow the clock." :
                       cur == WP_MINT_WAVES_AUTO ? "Follows the Light or Dark appearance." :
                       wallpaper_is_photo(cur) ? "Original MintOS artwork." :
                       cur >= WP_SOLID_MINT ? "Solid colour." : "Original MintOS design, drawn by MintDE.";
    gfx_text_fit(S, FONT_BODY, hero.x1 + 18, hero.y0 + 54, what, body.x1 - hero.x1 - 18, OPAQUE(T.text2));
    if (wallpaper_is_dynamic(cur)) {
        int64_t e = shell_now_epoch();
        uint32_t minute = e > 0 ? (uint32_t)((e % 86400) / 60) : 720u;
        char now[48]; copy_text(now, sizeof(now), "Now showing: "); str_append(now, sizeof(now), wallpaper_phase_name(wallpaper_phase(minute)));
        ui_badge(hero.x1 + 18, hero.y0 + 68, now, T.accent);
    }
    ui_label(body.x0 + 4, hero.y1 + 28, FONT_CAPTION_BOLD, "MINTOS WALLPAPERS", T.text2);
    for (uint32_t i = 0; i < WP_COUNT; ++i) {
        gfx_rect r = wp_thumb(body, i);
        thumb(r, i, i == cur);
        gfx_text_fit(S, FONT_CAPTION, r.x0 + 2, r.y1 + 16, wallpaper_name(i), W(r) - 4, OPAQUE(i == cur ? T.text : T.text2));
    }
}

int settings_wallpaper_click(gfx_rect body, int32_t x, int32_t y) {
    for (uint32_t i = 0; i < WP_COUNT; ++i)
        if (in(wp_thumb(body, i), x, y)) {
            if (personality_prefs.wallpaper != i) {
                personality_prefs.wallpaper = i;
                if (wallpaper_is_photo(i)) desktop_settings.wallpaper = (uint32_t)i;   /* kernel's Mint Waves variant */
                personality_prefs_apply();
            }
            return 1;
        }
    return 0;
}
