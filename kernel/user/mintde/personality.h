#ifndef MINTDE_PERSONALITY_H
#define MINTDE_PERSONALITY_H
/* MintDE 26.5.014.0 personality: sounds, wallpapers, the boot hand-off,
 * Notification Center and banners, the volume HUD and desktop items.
 * Included at the end of mintde.h; implemented in personality.c, notify.c,
 * extras.c, desktop_items.c, personality_settings.c and wallpapers.c. */
#include "wallpapers.h"
#include "notify.h"

/* ---- persisted preferences. One struct so the prefs store can load and
 * save it: personality_pref_keys[] maps every field to a "domain.name" key
 * (see docs/notes-26.5.014.0-personality.md). */
#define MDE_PERSONALITY_PREFS_VERSION 1u
typedef struct mde_personality_prefs {
    uint32_t version;
    uint32_t volume;              /* output volume 0..100 */
    uint32_t muted;
    uint32_t alert_sound;         /* 0..4: Mint, Dewdrop, Fern, Ripple, Woodblock */
    uint32_t alert_volume;        /* 0..100, relative to the output volume */
    uint32_t startup_sound;       /* play the startup chime */
    uint32_t ui_sounds;           /* Trash, screenshot, ... */
    uint32_t volume_feedback;     /* tick when the volume changes */
    uint32_t wallpaper;           /* WP_* */
    uint32_t banners;             /* show notification banners */
    uint32_t do_not_disturb;
    uint32_t notification_sound;
    uint32_t desktop_items;       /* show the items of ~/Desktop on the desktop */
} mde_personality_prefs;
typedef struct { const char *key; uint32_t *field; uint32_t max; } mde_pref_key;
extern mde_personality_prefs personality_prefs;
extern const mde_pref_key personality_pref_keys[];
extern const uint32_t personality_pref_key_count;
extern uint32_t personality_prefs_generation;   /* bumped on every change: a prefs store can poll it */
void personality_prefs_defaults(mde_personality_prefs *p);
void personality_prefs_apply(void);             /* push to the kernel, restyle, bump the generation */

/* ---- boot hand-off and startup */
int personality_boot_handoff(void);             /* before mapping the canvas: 1 if the boot animation ran */
void personality_startup(int crossfade);        /* after app_init: prefs, chime, crossfade */
void personality_init(void);                    /* after the desktop settings are loaded, before the wallpaper */
int personality_render_wallpaper(void);         /* 1 if the current wallpaper is procedural (drawn here) */
int personality_wallpaper_tick(void);           /* dynamic wallpaper: 1 if redrawn */
void personality_paint_splash(gfx_surface *s, uint32_t alpha);   /* splash frame blended over s */

/* ---- sounds */
enum { UI_SOUND_TRASH = 0, UI_SOUND_EMPTY_TRASH, UI_SOUND_SCREENSHOT, UI_SOUND_ALERT, UI_SOUND_NOTIFICATION };
void ui_sound(uint32_t which);
void ui_sound_alert_preview(uint32_t alert);
void sound_volume_set(int32_t volume, int show_hud);
void sound_volume_step(int32_t delta);
void sound_mute_toggle(void);
int sound_output_ready(void);
int sound_info(MINT_AUDIO_INFO *out);           /* 1 if the kernel answered */

/* ---- notifications: queue in notify.c/notify.h (host-tested); posting,
 * banners and Notification Center in extras.c. Shows a banner unless banners
 * are off or Do Not Disturb is on; plays the notification sound if enabled. */
uint32_t notify_post(uint32_t app, const char *title, const char *body);
uint64_t notify_ms(void);                       /* monotonic milliseconds */

/* ---- shell extras (extras.c) */
void extras_init(int32_t screen_w, int32_t screen_h);
void extras_paint_desktop(gfx_rect clip);       /* desktop items, under windows */
void extras_paint_overlays(void);               /* Notification Center, banners, volume HUD */
uint32_t extras_glass_rects(gfx_rect *out, uint32_t max);
/* desktop_hit: the click landed on the bare desktop (no window, Dock or menu). */
int extras_pointer_pressed(int32_t x, int32_t y, uint32_t clicks, int desktop_hit);
int extras_pointer_moved(int32_t x, int32_t y);
void extras_pointer_released(void);
int extras_key(const MINT_DESKTOP_KEY_EVENT *k, int window_focused);
void extras_tick(void);
void extras_toggle_notification_center(void);
int extras_notification_center_open(void);
void extras_close_overlays(void);
int32_t extras_cc_sound_height(void);
void extras_cc_sound_paint(gfx_rect r);
int extras_cc_sound_click(gfx_rect r, int32_t x, int32_t y);
void extras_cc_dnd_paint(gfx_rect r);
void extras_show_volume_hud(void);
int extras_screenshot(void);                    /* Command-Shift-3 */

/* ---- desktop items (desktop_items.c) */
void desktop_items_refresh(int force);
void desktop_items_paint(gfx_rect clip);
gfx_rect desktop_items_bounds(void);
int desktop_items_click(int32_t x, int32_t y, uint32_t clicks);
int desktop_items_key(const MINT_DESKTOP_KEY_EVENT *k);
int desktop_items_poll(uint64_t now_ms);        /* 1 if the desktop changed */
void desktop_items_deselect(void);

/* ---- Settings panes (personality_settings.c) */
void settings_sound_paint(gfx_rect body);
int settings_sound_click(gfx_rect body, int32_t x, int32_t y);
void settings_notifications_paint(gfx_rect body);
int settings_notifications_click(gfx_rect body, int32_t x, int32_t y);
void settings_wallpaper_paint(gfx_rect body);
int settings_wallpaper_click(gfx_rect body, int32_t x, int32_t y);
void ui_slider(gfx_rect r, uint32_t value, uint32_t max, int enabled);
uint32_t ui_slider_value(gfx_rect r, int32_t x, uint32_t max);
/* Terminal: sound, beep, play, notify. Returns 1 if the command was handled. */
int personality_terminal_command(const char *cmd, void (*print)(const char *));

/* ---- exported from apps.c for desktop items and Notification Center */
void files_open_location(const char *dir, const char *select_name, int rename);
void notes_open_document(const char *path);
#endif
