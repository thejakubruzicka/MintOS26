/* MintDE 26.5.014.0 personality core: preferences, the boot hand-off and
 * crossfade, UI sound policy, procedural wallpapers and Terminal commands. */
#include "mintde.h"
#include <boot_splash.h>

mde_personality_prefs personality_prefs;
uint32_t personality_prefs_generation;

/* "domain.name" keys for the preferences store (prefs.c in the data-saving
 * work): load with prefs_get_int(key, *field) and save with prefs_set_int
 * whenever personality_prefs_generation changes. */
const mde_pref_key personality_pref_keys[] = {
    {"sound.volume", &personality_prefs.volume, 100u},
    {"sound.muted", &personality_prefs.muted, 1u},
    {"sound.alert", &personality_prefs.alert_sound, MINT_SOUND_ALERT_COUNT - 1u},
    {"sound.alert_volume", &personality_prefs.alert_volume, 100u},
    {"sound.startup", &personality_prefs.startup_sound, 1u},
    {"sound.ui", &personality_prefs.ui_sounds, 1u},
    {"sound.volume_feedback", &personality_prefs.volume_feedback, 1u},
    {"desktop.wallpaper", &personality_prefs.wallpaper, WP_COUNT - 1u},
    {"notifications.banners", &personality_prefs.banners, 1u},
    {"notifications.dnd", &personality_prefs.do_not_disturb, 1u},
    {"notifications.sound", &personality_prefs.notification_sound, 1u},
    {"desktop.items", &personality_prefs.desktop_items, 1u},
};
const uint32_t personality_pref_key_count = sizeof(personality_pref_keys) / sizeof(personality_pref_keys[0]);
static uint32_t applied_volume = 0xFFFFu, applied_muted = 0xFFFFu, applied_wallpaper = 0xFFFFu;
static uint32_t wallpaper_minute = 0xFFFFu;

/* 26.5.014.0 merge: personality settings live in the data-saving prefs store
 * (~/Library/Preferences/com.mintos.<domain>.prefs). A saved volume wins over
 * the kernel's default after a reboot. */
static int saved_volume;
void personality_init(void) {
    personality_prefs_defaults(&personality_prefs);
    if (desktop_settings.wallpaper <= WP_MINT_WAVES_DARK) personality_prefs.wallpaper = desktop_settings.wallpaper;
    for (uint32_t i = 0; i < personality_pref_key_count; ++i) {
        int64_t v = prefs_get_int(personality_pref_keys[i].key, -1);
        if (v >= 0 && (uint64_t)v <= personality_pref_keys[i].max) *personality_pref_keys[i].field = (uint32_t)v;
    }
    saved_volume = prefs_get_int("sound.volume", -1) >= 0;
    applied_wallpaper = personality_prefs.wallpaper;
}

void personality_prefs_defaults(mde_personality_prefs *p) {
    p->version = MDE_PERSONALITY_PREFS_VERSION;
    p->volume = 70u; p->muted = 0u;
    p->alert_sound = 0u; p->alert_volume = 80u;
    p->startup_sound = 1u; p->ui_sounds = 1u; p->volume_feedback = 1u;
    p->wallpaper = WP_MINT_WAVES_AUTO;
    p->banners = 1u; p->do_not_disturb = 0u; p->notification_sound = 1u;
    p->desktop_items = 1u;
}


static uint32_t minute_of_day(void) {
    int64_t e = shell_now_epoch();
    return e > 0 ? (uint32_t)((e % 86400) / 60) : 720u;
}

void personality_prefs_apply(void) {
    for (uint32_t i = 0; i < personality_pref_key_count; ++i)
        if (*personality_pref_keys[i].field > personality_pref_keys[i].max) *personality_pref_keys[i].field = personality_pref_keys[i].max;
    personality_prefs.version = MDE_PERSONALITY_PREFS_VERSION;
    if (personality_prefs.volume != applied_volume || personality_prefs.muted != applied_muted) {
        (void)mint_audio_volume(personality_prefs.volume, personality_prefs.muted);
        applied_volume = personality_prefs.volume; applied_muted = personality_prefs.muted;
    }
    if (personality_prefs.wallpaper != applied_wallpaper) {
        applied_wallpaper = personality_prefs.wallpaper;
        /* The kernel's session setting keeps knowing the Mint Waves variant. */
        desktop_settings.wallpaper = wallpaper_is_photo(personality_prefs.wallpaper) ? personality_prefs.wallpaper : 0u;
        (void)shell_apply_settings();        /* re-renders the wallpaper and repaints */
    }
    ++personality_prefs_generation;
    for (uint32_t i = 0; i < personality_pref_key_count; ++i)      /* debounced save by the prefs store */
        (void)prefs_set_int(personality_pref_keys[i].key, *personality_pref_keys[i].field);
}

/* ------------------------------------------------------------ wallpaper */
int personality_render_wallpaper(void) {
    uint32_t id = personality_prefs.wallpaper;
    if (wallpaper_is_photo(id) || !gfx_wall.px) return 0;
    wallpaper_minute = minute_of_day();
    wallpaper_render(gfx_wall.px, gfx_wall.stride, gfx_wall.w, gfx_wall.h, 0, 0, gfx_wall.w, gfx_wall.h, id, wallpaper_minute);
    return 1;
}

/* Dynamic wallpaper: redraw when the clock has moved enough (5 minutes
   around dawn and dusk, 15 otherwise). Returns 1 if it was redrawn. */
int personality_wallpaper_tick(void) {
    if (!wallpaper_is_dynamic(personality_prefs.wallpaper)) return 0;
    uint32_t now = minute_of_day();
    uint32_t diff = now >= wallpaper_minute ? now - wallpaper_minute : now + 1440u - wallpaper_minute;
    if (wallpaper_minute != 0xFFFFu && diff < wallpaper_refresh_minutes(now)) return 0;
    (void)personality_render_wallpaper();
    return 1;
}

/* ------------------------------------------------------------ boot hand-off */
int personality_boot_handoff(void) {
    int running = mint_boot_splash(MINT_SPLASH_OP_STATUS) == 1;
    (void)mint_boot_splash(MINT_SPLASH_OP_FINISH);
    return running;
}

static uint32_t blend(uint32_t a, uint32_t b, uint32_t t) {        /* t 0..256 towards b */
    uint32_t ra = (a >> 16) & 255u, ga = (a >> 8) & 255u, ba = a & 255u;
    uint32_t rb = (b >> 16) & 255u, gb = (b >> 8) & 255u, bb = b & 255u;
    return (((ra * (256u - t) + rb * t) >> 8) << 16) | (((ga * (256u - t) + gb * t) >> 8) << 8) | ((ba * (256u - t) + bb * t) >> 8);
}

/* s holds the composed desktop; draw the final boot frame over it with
   weight alpha (0..256) using the same renderer as the kernel splash. */
void personality_paint_splash(gfx_surface *s, uint32_t alpha) {
    static uint32_t row[4096];
    if (!s || !s->px || s->w > 4096) return;
    MINT_SPLASH_GEOMETRY g;
    mint_splash_geometry(&g, s->w, s->h);
    for (int32_t y = 0; y < s->h; ++y) {
        mint_splash_render(row, 4096, &g, 0, y, s->w, y + 1, 255u, 1000u, 0u);
        uint32_t *dst = s->px + (int64_t)y * s->stride;
        for (int32_t x = 0; x < s->w; ++x) dst[x] = blend(dst[x], row[x], alpha);
    }
}

void personality_startup(int crossfade) {
    MINT_AUDIO_INFO ai;
    if (saved_volume) {                     /* saved on MintOS HD: restore it in the kernel mixer */
        (void)mint_audio_volume(personality_prefs.volume, personality_prefs.muted);
    } else if (sound_info(&ai)) {           /* the kernel keeps the volume across MintDE restarts */
        personality_prefs.volume = ai.volume <= 100u ? ai.volume : 70u;
        personality_prefs.muted = ai.muted ? 1u : 0u;
    }
    applied_volume = personality_prefs.volume; applied_muted = personality_prefs.muted;
    if (personality_prefs.startup_sound && crossfade) (void)mint_audio_play(MINT_SOUND_STARTUP, 1000u);
    if (!crossfade || (desktop_settings.flags & MINT_DESKTOP_SETTING_REDUCE_MOTION)) {
        (void)mint_display_present(0, 0, gfx_screen.w, gfx_screen.h);
        return;
    }
    /* Crossfade from the kernel's last boot frame into the desktop: keep the
       composed desktop in the scratch buffer and blend the splash over it. */
    gfx_rect all = gfx_rect_xywh(0, 0, gfx_screen.w, gfx_screen.h);
    gfx_clip_set(all);
    gfx_copy(&gfx_scratch, &gfx_screen, all);
    static const uint32_t steps[5] = {224u, 176u, 120u, 64u, 20u};
    for (uint32_t i = 0; i < 5u; ++i) {
        gfx_copy(&gfx_screen, &gfx_scratch, all);
        personality_paint_splash(&gfx_screen, steps[i]);
        (void)mint_display_present(0, 0, gfx_screen.w, gfx_screen.h);
    }
    gfx_copy(&gfx_screen, &gfx_scratch, all);
    (void)mint_display_present(0, 0, gfx_screen.w, gfx_screen.h);
    static const char msg[] = "[MintDE] boot animation crossfaded into the desktop\n";
    (void)mint_write(1, msg, sizeof(msg) - 1u);
}

/* ------------------------------------------------------------ sounds */
int sound_info(MINT_AUDIO_INFO *out) { return out && mint_audio_info(out) == 0; }
int sound_output_ready(void) {
    MINT_AUDIO_INFO ai;
    return sound_info(&ai) && ai.state == MINT_AUDIO_RUNNING;
}

void ui_sound(uint32_t which) {
    const mde_personality_prefs *p = &personality_prefs;
    switch (which) {
    case UI_SOUND_TRASH: if (p->ui_sounds) (void)mint_audio_play(MINT_SOUND_TRASH, 850u); break;
    case UI_SOUND_EMPTY_TRASH: if (p->ui_sounds) (void)mint_audio_play(MINT_SOUND_EMPTY_TRASH, 900u); break;
    case UI_SOUND_SCREENSHOT: if (p->ui_sounds) (void)mint_audio_play(MINT_SOUND_SCREENSHOT, 900u); break;
    case UI_SOUND_ALERT: (void)mint_audio_play(MINT_SOUND_ALERT_FIRST + (p->alert_sound % MINT_SOUND_ALERT_COUNT), p->alert_volume * 10u); break;
    case UI_SOUND_NOTIFICATION: if (p->notification_sound && !p->do_not_disturb) (void)mint_audio_play(MINT_SOUND_NOTIFICATION, 800u); break;
    default: break;
    }
}
void ui_sound_alert_preview(uint32_t alert) {
    (void)mint_audio_play(MINT_SOUND_ALERT_FIRST + (alert % MINT_SOUND_ALERT_COUNT), personality_prefs.alert_volume * 10u);
}

void sound_volume_set(int32_t volume, int show_hud) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    personality_prefs.volume = (uint32_t)volume;
    if (show_hud && volume > 0) personality_prefs.muted = 0u;    /* volume keys unmute, as on a Mac */
    personality_prefs_apply();
    if (personality_prefs.volume_feedback && !personality_prefs.muted) (void)mint_audio_play(MINT_SOUND_VOLUME, 1000u);
    if (show_hud) extras_show_volume_hud();
}
void sound_volume_step(int32_t delta) { sound_volume_set((int32_t)personality_prefs.volume + delta, 1); }
void sound_mute_toggle(void) {
    personality_prefs.muted = !personality_prefs.muted;
    personality_prefs_apply();
    extras_show_volume_hud();
}

/* ------------------------------------------------------------ Terminal */
static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int same(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static uint32_t parse_u32(const char **s) {
    uint32_t v = 0;
    while (**s == ' ') ++*s;
    while (**s >= '0' && **s <= '9') { v = v * 10u + (uint32_t)(**s - '0'); if (v > 100000u) v = 100000u; ++*s; }
    return v;
}

static const char *state_text(uint32_t state) {
    switch (state) {
    case MINT_AUDIO_ABSENT: return "no HDA controller";
    case MINT_AUDIO_FOUND: return "detected, not started";
    case MINT_AUDIO_STARTING: return "starting";
    case MINT_AUDIO_RUNNING: return "running";
    case MINT_AUDIO_FAILED: return "failed";
    case MINT_AUDIO_STOPPED: return "stopped";
    default: return "unknown";
    }
}

int personality_terminal_command(const char *cmd, void (*print)(const char *)) {
    char line[160], n[24];
    if (same(cmd, "sound") || same(cmd, "sound status")) {
        MINT_AUDIO_INFO ai;
        if (!sound_info(&ai)) { print("sound: the kernel did not answer"); return 1; }
        copy_text(line, sizeof(line), "Sound: "); str_append(line, sizeof(line), state_text(ai.state));
        str_append(line, sizeof(line), " \xE2\x80\x94 "); str_append(line, sizeof(line), ai.status); print(line);
        if (ai.state != MINT_AUDIO_ABSENT) {
            copy_text(line, sizeof(line), "Controller "); format_hex64(n, sizeof(n), ai.pci_vendor); str_append(line, sizeof(line), n);
            str_append(line, sizeof(line), ":"); format_hex64(n, sizeof(n), ai.pci_device); str_append(line, sizeof(line), n);
            str_append(line, sizeof(line), ai.experimental ? " (experimental)" : " (virtual machine)"); print(line);
        }
        if (ai.state == MINT_AUDIO_RUNNING) {
            copy_text(line, sizeof(line), "Codec: "); str_append(line, sizeof(line), ai.codec_name);
            str_append(line, sizeof(line), ", outputs "); format_u64(n, sizeof(n), ai.output_count); str_append(line, sizeof(line), n);
            str_append(line, sizeof(line), ai.command_path ? ", immediate commands" : ", CORB/RIRB"); print(line);
            copy_text(line, sizeof(line), "Volume "); format_u64(n, sizeof(n), ai.volume); str_append(line, sizeof(line), n);
            str_append(line, sizeof(line), ai.muted ? "% (muted), played " : "%, played ");
            format_u64(n, sizeof(n), ai.frames_played / 48000u); str_append(line, sizeof(line), n);
            str_append(line, sizeof(line), " s, sounds "); format_u64(n, sizeof(n), ai.sounds_started); str_append(line, sizeof(line), n);
            str_append(line, sizeof(line), ", underruns "); format_u64(n, sizeof(n), ai.underruns); str_append(line, sizeof(line), n);
            print(line);
        }
        return 1;
    }
    if (same(cmd, "sound start")) {
        int64_t rc = mint_audio_control(MINT_AUDIO_OP_START, MINT_AUDIO_START_TOKEN);
        print(rc == 0 ? "sound: driver running" : rc == -MINT_ENODEV ? "sound: no HDA controller" : "sound: start failed (see Storage & Logs)");
        return 1;
    }
    if (same(cmd, "sound stop")) {
        print(mint_audio_control(MINT_AUDIO_OP_STOP, 0) == 0 ? "sound: stopped; bus mastering off, DMA released" : "sound: stop failed");
        return 1;
    }
    if (starts(cmd, "volume")) {
        const char *p = cmd + 6;
        while (*p == ' ') ++p;
        if (same(p, "mute")) { personality_prefs.muted = 1u; personality_prefs_apply(); }
        else if (same(p, "unmute")) { personality_prefs.muted = 0u; personality_prefs_apply(); }
        else if (*p) sound_volume_set((int32_t)parse_u32(&p), 1);
        copy_text(line, sizeof(line), "Output volume "); format_u64(n, sizeof(n), personality_prefs.volume);
        str_append(line, sizeof(line), n); str_append(line, sizeof(line), personality_prefs.muted ? "% (muted)" : "%");
        print(line);
        return 1;
    }
    if (starts(cmd, "play")) {
        static const char *const names[MINT_SOUND_COUNT] = {"startup", "trash", "emptytrash", "notification", "volume", "screenshot",
                                                            "mint", "dewdrop", "fern", "ripple", "woodblock"};
        const char *p = cmd + 4;
        while (*p == ' ') ++p;
        for (uint32_t i = 0; i < MINT_SOUND_COUNT; ++i)
            if (same(p, names[i])) {
                int64_t rc = mint_audio_play(i, 1000u);
                print(rc >= 0 ? "Playing." : "sound: the audio driver is not running");
                return 1;
            }
        print("usage: play startup|trash|emptytrash|notification|volume|screenshot|mint|dewdrop|fern|ripple|woodblock");
        return 1;
    }
    if (starts(cmd, "beep")) {
        /* A user-generated tone through MINT_SYS_AUDIO_QUEUE: integer sine, soft edges. */
        static int16_t pcm[48000];
        const char *p = cmd + 4;
        uint32_t hz = parse_u32(&p), ms = parse_u32(&p);
        if (!hz) hz = 880u;
        if (!ms) ms = 250u;
        if (hz < 40u) hz = 40u;
        if (hz > 12000u) hz = 12000u;
        if (ms > 1000u) ms = 1000u;
        uint32_t frames = ms * 48u, phase = 0, step = (uint32_t)(((uint64_t)hz << 32) / 48000u);
        for (uint32_t i = 0; i < frames; ++i) {
            int32_t s = wp_sin((int32_t)(phase >> 22));
            uint32_t edge = i < 480u ? i : frames - i < 480u ? frames - i : 480u;
            pcm[i] = (int16_t)(s * (int32_t)edge / 480 * 3 / 4);
            phase += step;
        }
        int64_t rc = mint_audio_queue(pcm, frames, 1u, 800u);
        print(rc >= 0 ? "Beep." : rc == -MINT_EBUSY ? "sound: too many sounds playing" : "sound: the audio driver is not running");
        return 1;
    }
    if (starts(cmd, "notify ")) {
        notify_post(APP_TERMINAL, "Terminal", cmd + 7);
        print("Posted to Notification Center.");
        return 1;
    }
    return 0;
}
