#ifndef MINTDE_H
#define MINTDE_H
/* MintDE 2 — shared definitions for the shell (desktop.c), the controls
 * (widgets.c) and the applications (apps.c). */
#include <mintos/runtime.h>
#include "gfx.h"
#include "userdata.h"     /* preferences and the computer account (data saving) */
#include <mintos/personality.h>   /* 26.5.014.0 sound, boot animation */

#define MINTDE_VERSION "2.4.0"
#define MINTOS_VERSION "26.5.014.0"

enum {
    APP_FILES = 0, APP_NOTES, APP_TERMINAL, APP_SETTINGS, APP_MONITOR, APP_STORAGE,
    APP_DRIVERS, APP_NETWORK, APP_DISKS, APP_ABOUT_OS, APP_ABOUT_MINTDE, APP_LAUNCHPAD,
    APP_CALCULATOR, APP_WEB, APP_COUNT
};

/* Window chrome styles. */
enum { CHROME_TITLE = 0,   /* 28 px title bar, centred title */
       CHROME_TOOLBAR = 1, /* 52 px unified toolbar, title left */
       CHROME_PLAIN = 2 }; /* 28 px bar, no title (About, Calculator) */

typedef struct mde_window {
    uint32_t open, minimized, zoomed;
    int32_t x, y, w, h;
    int32_t rx, ry, rw, rh;   /* restore frame while zoomed */
} mde_window;

/* Geometry handed to an app for painting and hit testing (absolute pixels). */
typedef struct mde_layout {
    gfx_rect frame, toolbar, sidebar, content;
    int focused;
} mde_layout;

typedef struct mde_theme {
    int dark;
    uint32_t accent, accent_text;        /* accent and readable text on it */
    uint32_t window, content, sidebar, group, group_bg, field, control;
    uint32_t text, text2, text3;
    uint32_t separator, control_border;  /* ARGB */
    uint32_t row_alt, selection_inactive;
    uint32_t menu_tint;                  /* ARGB tint over blurred backdrop */
    uint32_t sidebar_tint;               /* ARGB tint over the blurred window sidebar */
    uint32_t sidebar_selection;          /* ARGB Finder-style selection pill */
} mde_theme;
extern mde_theme T;

/* ---- shell services used by apps (desktop.c) */
extern MINT_DESKTOP_SETTINGS desktop_settings;
extern MINT_DISPLAY_CANVAS canvas;
extern int32_t pointer_x, pointer_y;
void shell_damage(gfx_rect r);
void shell_damage_window(uint32_t app);
void shell_open_app(uint32_t app);
void shell_close_app(uint32_t app);
void shell_request_power(uint32_t action);
int shell_apply_settings(void);        /* push desktop_settings to the kernel and restyle */
int64_t shell_now_epoch(void);         /* RTC wall clock (0 if unavailable) */
void shell_format_clock(char *out, size_t cap, int64_t epoch, int with_seconds);
/* Finder-style date: "Today at 14:05", "Yesterday at 09:12", "3 Nov 2026 at 18:40". */
void shell_format_date(char *out, size_t cap, int64_t epoch);

/* ---- applications (apps.c) */
const char *app_name(uint32_t app);
const uint8_t *app_icon(uint32_t app, int large);
int app_chrome(uint32_t app);
int32_t app_sidebar_width(uint32_t app);
void app_default_size(uint32_t app, int32_t *w, int32_t *h);
void app_init(void);
void app_opened(uint32_t app);
/* Files commands on MintOS HD (menu bar File menu and shortcuts). */
enum { FILES_CMD_NEW_FOLDER = 1, FILES_CMD_RENAME, FILES_CMD_DUPLICATE, FILES_CMD_TRASH, FILES_CMD_EMPTY_TRASH, FILES_CMD_COUNT };
int files_command(uint32_t command);
int files_command_enabled(uint32_t command);
int files_show_trash(void);                      /* Dock Trash: 1 if Files now shows the Trash */
void app_paint(uint32_t app, const mde_layout *l);
int app_click(uint32_t app, const mde_layout *l, int32_t x, int32_t y, uint32_t clicks);
int app_key(uint32_t app, const mde_layout *l, const MINT_DESKTOP_KEY_EVENT *k);
int app_scroll(uint32_t app, const mde_layout *l, int32_t x, int32_t y, int32_t delta);
int app_tick(uint32_t app);
/* Called every frame for every app; returns a bitmask (1 << app) of windows to repaint. */
uint32_t app_background(void);
uint32_t app_cursor(uint32_t app, const mde_layout *l, int32_t x, int32_t y);
const char *app_window_title(uint32_t app);
void settings_select_pane(uint32_t pane);

/* ---- Web browser (web.c) */
void web_paint(const mde_layout *l);
int web_click(const mde_layout *l, int32_t x, int32_t y);
int web_key(const mde_layout *l, const MINT_DESKTOP_KEY_EVENT *k);
int web_scroll(const mde_layout *l, int32_t delta);
int web_background(void);
uint32_t web_cursor(const mde_layout *l, int32_t x, int32_t y);
const char *web_title(void);
void web_opened(void);
/* Menu commands: 1 add/remove favorite (Cmd-D), 2 show downloads, 3 show favorites. */
int web_command(uint32_t command);

/* ---- controls (widgets.c) */
size_t text_length(const char *s);
size_t copy_text(char *out, size_t capacity, const char *in);
void format_u64(char *out, size_t capacity, uint64_t value);
void format_hex64(char *out, size_t capacity, uint64_t value);
void format_bytes(char *out, size_t capacity, uint64_t bytes);
void str_append(char *dst, size_t capacity, const char *src);
void theme_apply(const MINT_DESKTOP_SETTINGS *s, int night);

void ui_button(gfx_rect r, const char *label, int primary, int enabled);
void ui_popup(gfx_rect r, const char *value, int enabled);
void ui_switch(int32_t x, int32_t y, int on);
void ui_segmented(gfx_rect r, const char *const *labels, uint32_t count, uint32_t selected);
int32_t ui_segment_at(gfx_rect r, uint32_t count, int32_t x);
void ui_search_field(gfx_rect r, const char *text, int focused);
void ui_text_field(gfx_rect r, const char *text, const char *placeholder, int focused);
/* Same, with the caret after `caret` bytes of text. */
void ui_text_field_caret(gfx_rect r, const char *text, const char *placeholder, int focused, size_t caret);
void ui_group(gfx_rect r);
void ui_group_row(gfx_rect r, uint32_t index, uint32_t count, const char *label, const char *value);
void ui_sidebar_item(gfx_rect r, const char *label, const uint8_t *icon64, uint32_t color, int selected, int focused);
/* Finder-style row: gray selection pill, accent-coloured line symbol, regular label. */
void ui_sidebar_row(gfx_rect r, const char *label, uint32_t symbol, int selected);
void ui_sidebar_header(int32_t x, int32_t y, const char *label);
void ui_list_row(gfx_rect r, uint32_t index, int selected, int focused);
void ui_progress(gfx_rect r, uint32_t permille, uint32_t color);
void ui_symbol_glyph(int32_t cx, int32_t cy, uint32_t symbol, uint32_t argb);
void ui_label(int32_t x, int32_t baseline, int font, const char *text, uint32_t rgb);
void ui_label_right(int32_t right, int32_t baseline, int font, const char *text, uint32_t rgb);
void ui_label_center(int32_t cx, int32_t baseline, int font, const char *text, uint32_t rgb);
void ui_badge(int32_t x, int32_t y, const char *text, uint32_t color);

/* Symbols drawn procedurally (sidebar and toolbar). */
enum { SYM_CHEVRON_LEFT = 1, SYM_CHEVRON_RIGHT, SYM_SEARCH, SYM_FOLDER, SYM_DISK, SYM_GEAR,
       SYM_TOGGLES, SYM_PLUS, SYM_MINUS, SYM_INFO, SYM_WARNING, SYM_CHECK, SYM_DOC,
       /* line-style sidebar symbols (macOS SF Symbols look) */
       SYM_DRIVE, SYM_RAM, SYM_CHIP, SYM_USB, SYM_BOLT, SYM_LIST, SYM_GEAR_LINE, SYM_CLOCK, SYM_HOUSE, SYM_FOLDER_LINE,
       SYM_APPS, SYM_DESKTOP, SYM_DOWNLOAD, SYM_TRASH, SYM_FOLDER_PLUS };

/* Icons embedded by tools/embed_icons.py */
#define ICON_DECL(n) extern const uint8_t mint_icon_##n##_128[]; extern const uint8_t mint_icon_##n##_64[];
ICON_DECL(files) ICON_DECL(notes) ICON_DECL(terminal) ICON_DECL(settings) ICON_DECL(system_monitor)
ICON_DECL(console) ICON_DECL(system_info) ICON_DECL(network) ICON_DECL(disk_utility)
ICON_DECL(about_mintos) ICON_DECL(mintde) ICON_DECL(launchpad) ICON_DECL(calculator) ICON_DECL(trash) ICON_DECL(web)
extern const uint8_t mint_icon_mint_leaf_a8[];

/* ---- data saving (26.5.014.0). Preferences and the account: userdata.h. */
/* Notes with many notes in ~/Documents/Notes (notes.c) */
enum { NOTES_CMD_NEW = 1, NOTES_CMD_DELETE, NOTES_CMD_COUNT };
void notes_paint(const mde_layout *l);
int notes_click(const mde_layout *l, int32_t x, int32_t y, uint32_t clicks);
int notes_key(const MINT_DESKTOP_KEY_EVENT *k);
int notes_scroll(const mde_layout *l, int32_t x, int32_t y, int32_t delta);
uint32_t notes_cursor(const mde_layout *l, int32_t x, int32_t y);
void notes_opened(void);
void notes_open_file(const char *path);        /* Files: open a text file in Notes */
int notes_background(uint64_t now_ms);         /* debounced save; 1 = repaint */
int notes_flush(void);                         /* save the open note now */
void notes_home_changed(void);
int notes_command(uint32_t command);
int notes_command_enabled(uint32_t command);
/* Setup Assistant, appearance restore and Users & Groups (setup.c) */
void userdata_start(void);                     /* MintDE start: preferences, account, saved appearance */
void userdata_store_appearance(void);          /* called by shell_apply_settings */
int setup_active(void);
void setup_begin(int rerun);
void setup_boot_check(void);                   /* first boot: no account yet */
void setup_paint(gfx_rect damage);
void setup_click(int32_t x, int32_t y);
void setup_key(const MINT_DESKTOP_KEY_EVENT *k);
int setup_tick(void);
uint32_t setup_cursor(int32_t x, int32_t y);
void account_avatar(int32_t cx, int32_t cy, int32_t r, const char *full_name, int font);
void users_card_paint(gfx_rect r, int selected, int focused);
void users_pane_paint(gfx_rect body);
int users_pane_click(gfx_rect body, int32_t x, int32_t y);
/* apps.c */
void app_save_all(void);                       /* Notes, Terminal history, preferences, MintFS commit */
void app_home_changed(void);                   /* the home moved: Files, Notes and Terminal follow */
#include "personality.h"   /* 26.5.014.0: sounds, wallpapers, notifications, desktop items */
#endif
