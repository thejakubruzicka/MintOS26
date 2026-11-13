/* MintDE 26.5.014.0 desktop items: the files and folders in ~/Desktop on
 * MintOS HD shown as icons on the wallpaper, arranged in columns from the
 * top-right corner like a Mac. Click selects, double-click opens (folders in
 * Files, text in Notes, anything else is revealed in Files), Return renames
 * in Files, Command-Delete moves to the Trash, arrow keys move the
 * selection. The listing is refreshed when MintFS reports a change. */
#include "mintde.h"

#define S (&gfx_screen)
#define MENU_H 26
#define CELL_W 104
#define CELL_H 104
#define ICON 60
#define ITEMS_MAX 48u

typedef struct { MINT_MFS_DIRENT e; int32_t x, y; } item;
static item items[ITEMS_MAX];
static uint32_t item_count;
static int32_t selected = -1;
static uint64_t last_poll_ms, last_generation = ~0ull;
static uint32_t signature;
static char desktop_dir[160];

static gfx_rect R(int32_t x, int32_t y, int32_t w, int32_t h) { return gfx_rect_xywh(x, y, w, h); }

static int same_text(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_order(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) { ++a; ++b; }
    return lower(*a) - lower(*b);
}
static const char *extension(const char *name) {
    const char *dot = 0;
    for (const char *p = name; *p; ++p) if (*p == '.') dot = p;
    return dot && dot != name ? dot + 1 : "";
}
static int ext_is(const char *name, const char *e) {
    const char *x = extension(name);
    while (*x && *e && lower(*x) == lower(*e)) { ++x; ++e; }
    return !*x && !*e;
}
static int is_text(const char *name) {
    return ext_is(name, "txt") || ext_is(name, "md") || ext_is(name, "c") || ext_is(name, "h") || ext_is(name, "log") ||
           ext_is(name, "csv") || ext_is(name, "json") || ext_is(name, "html") || ext_is(name, "lua") || !extension(name)[0];
}
static int is_image(const char *name) { return ext_is(name, "bmp") || ext_is(name, "png") || ext_is(name, "jpg") || ext_is(name, "ppm"); }

static void layout(void) {
    int32_t sw = gfx_screen.w, sh = gfx_screen.h;
    int32_t bottom = sh - 110, x = sw - 12 - CELL_W, y = MENU_H + 14;
    for (uint32_t i = 0; i < item_count; ++i) {
        if (y + CELL_H > bottom) { y = MENU_H + 14; x -= CELL_W; }
        items[i].x = x; items[i].y = y;
        y += CELL_H;
    }
}

static uint32_t hash_entry(uint32_t h, const MINT_MFS_DIRENT *e) {
    for (const char *p = e->name; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ (uint32_t)e->size) * 16777619u;
    h = (h ^ (uint32_t)e->mtime) * 16777619u;
    return (h ^ e->type) * 16777619u;
}

void desktop_items_refresh(int force) {
    (void)force;
    static MINT_MFS_DIRENT chunk[MINT_MFS_LIST_MAX];
    char keep[MINT_MFS_NAME_MAX + 1] = {0};
    if (selected >= 0 && (uint32_t)selected < item_count) copy_text(keep, sizeof(keep), items[selected].e.name);
    item_count = 0;
    MINT_MFS_VOLUME v;
    if (mint_fs_get_volume(&v) != 0 || v.state == MINT_MFS_VOLUME_NONE || v.state == MINT_MFS_VOLUME_FAILED) {
        desktop_dir[0] = '\0'; signature = 0; selected = -1; return;
    }
    last_generation = v.generation;
    copy_text(desktop_dir, sizeof(desktop_dir), v.home);
    str_append(desktop_dir, sizeof(desktop_dir), "/Desktop");
    uint32_t sig = 2166136261u;
    for (uint32_t start = 0; item_count < ITEMS_MAX;) {
        int64_t n = mint_dir_list(desktop_dir, start, chunk);
        if (n <= 0) break;
        for (int64_t i = 0; i < n && item_count < ITEMS_MAX; ++i) {
            if (chunk[i].name[0] == '.') continue;                    /* hidden and temporary files */
            items[item_count++].e = chunk[i];
            sig = hash_entry(sig, &chunk[i]);
        }
        start += (uint32_t)n;
        if (n < (int64_t)MINT_MFS_LIST_MAX) break;
    }
    /* folders first, then names, case-insensitively (Finder's "Sort by Kind") */
    for (uint32_t i = 1; i < item_count; ++i) {
        item t = items[i];
        uint32_t j = i;
        while (j && ((items[j - 1u].e.type != MINT_MFS_DIR && t.e.type == MINT_MFS_DIR) ||
                     (items[j - 1u].e.type == t.e.type && name_order(items[j - 1u].e.name, t.e.name) > 0))) {
            items[j] = items[j - 1u]; --j;
        }
        items[j] = t;
    }
    signature = sig;
    selected = -1;
    for (uint32_t i = 0; keep[0] && i < item_count; ++i) if (same_text(items[i].e.name, keep)) selected = (int32_t)i;
    layout();
}

int desktop_items_poll(uint64_t now_ms) {
    if (now_ms - last_poll_ms < 1000u) return 0;
    last_poll_ms = now_ms;
    MINT_MFS_VOLUME v;
    int ready = mint_fs_get_volume(&v) == 0 && (v.state == MINT_MFS_VOLUME_READY || v.state == MINT_MFS_VOLUME_READ_ONLY);
    if (!ready) {
        if (!item_count && !desktop_dir[0]) return 0;
        item_count = 0; desktop_dir[0] = '\0'; selected = -1;
        return 1;
    }
    if (v.generation == last_generation && desktop_dir[0]) return 0;
    uint32_t before = signature, count = item_count;
    desktop_items_refresh(0);
    return signature != before || count != item_count;
}

gfx_rect desktop_items_bounds(void) {
    int32_t sw = gfx_screen.w;
    int32_t cols = 1;
    for (uint32_t i = 0; i < item_count; ++i) { int32_t c = (sw - items[i].x) / CELL_W + 1; if (c > cols) cols = c; }
    return R(sw - (cols + 1) * CELL_W - 20, MENU_H, (cols + 1) * CELL_W + 20, gfx_screen.h - MENU_H);
}

/* ------------------------------------------------------------ icons */
static void folder_icon(int32_t x, int32_t y) {
    uint32_t back = 0x3FBFA0u, front = 0x63D9BBu, edge = 0x2E9E84u;
    gfx_round_rect(S, x + 4, y + 10, 22, 10, 3, OPAQUE(back));                       /* tab */
    gfx_round_rect(S, x + 4, y + 14, ICON - 8, ICON - 22, 5, OPAQUE(back));
    gfx_round_rect_grad(S, x + 4, y + 20, ICON - 8, ICON - 28, 5, OPAQUE(front), OPAQUE(gfx_lerp_rgb(front, edge, 80)));
    gfx_hline(S, x + 8, y + 20, ICON - 16, ARGB(0x90, 0xFF, 0xFF, 0xFF));
}
static void document_icon(int32_t x, int32_t y, const char *name) {
    int32_t px = x + 12, pw = ICON - 24, ph = ICON - 4;
    gfx_shadow(S, px, y + 2, pw, ph, 3, 4, 1, ARGB(0x40, 0, 0, 0));
    gfx_round_rect(S, px, y + 2, pw, ph, 3, OPAQUE(0xFFFFFFu));
    gfx_fill(S, px + pw - 10, y + 2, 10, 10, OPAQUE(0xE4E4E8u));                      /* folded corner */
    gfx_line(S, px + pw - 10, y + 2, px + pw - 1, y + 11, OPAQUE(0xC8C8CEu));
    if (is_image(name)) {
        gfx_round_rect_grad(S, px + 5, y + 18, pw - 10, ph - 30, 2, OPAQUE(0x86C8F0u), OPAQUE(0x3FBFA0u));
        gfx_circle(S, px + pw - 12, y + 25, 3, OPAQUE(0xFFF4C8u));
    } else {
        for (int32_t l = 0; l < 6; ++l) gfx_hline(S, px + 6, y + 18 + l * 6, pw - 12 - (l == 5 ? 10 : 0), OPAQUE(0xC6C6CCu));
    }
    char ext[8]; size_t n = 0;
    for (const char *e = extension(name); *e && n < 4u; ++e) ext[n++] = (char)(*e >= 'a' && *e <= 'z' ? *e - 32 : *e);
    ext[n] = '\0';
    if (n) {
        int32_t tw = gfx_text_width(FONT_CAPTION_BOLD, ext) + 8;
        gfx_round_rect(S, px + pw / 2 - tw / 2, y + ph - 16, tw, 14, 4, OPAQUE(is_image(name) ? 0x2E9E84u : 0x8E8E93u));
        ui_label_center(px + pw / 2, y + ph - 5, FONT_CAPTION_BOLD, ext, 0xFFFFFFu);
    }
}

/* Two centred label lines with a soft shadow; the selection gets an accent pill. */
static void label(const item *it, int sel) {
    char l1[64], l2[64];
    const char *name = it->e.name;
    int32_t max = CELL_W - 10;
    size_t n = text_length(name), cut = n;
    if (gfx_text_width(FONT_SMALL, name) > max) {
        cut = 0;
        for (size_t i = 1; i <= n && i < sizeof(l1); ++i) {
            char t[64]; copy_text(t, i + 1u < sizeof(t) ? i + 1u : sizeof(t), name);
            if (gfx_text_width(FONT_SMALL, t) > max) break;
            if (name[i - 1u] == ' ' || name[i] == ' ' || name[i] == '.' || name[i] == '-') cut = i;
        }
        if (!cut) cut = n > 12u ? 12u : n;
    }
    copy_text(l1, cut + 1u < sizeof(l1) ? cut + 1u : sizeof(l1), name);
    copy_text(l2, sizeof(l2), cut < n ? name + cut + (name[cut] == ' ' ? 1 : 0) : "");
    int32_t cx = it->x + CELL_W / 2, y = it->y + ICON + 20;
    for (int line = 0; line < 2; ++line) {
        const char *t = line ? l2 : l1;
        if (!t[0]) continue;
        int32_t w = gfx_text_width(FONT_SMALL, t);
        if (w > max) w = max;
        int32_t ly = y + line * 15;
        if (sel) gfx_round_rect(S, cx - w / 2 - 5, ly - 12, w + 10, 16, 5, OPAQUE(T.accent));
        else gfx_text_fit(S, FONT_SMALL, cx - w / 2 + 1, ly + 1, t, max, ARGB(0xB0, 0, 0, 0));
        gfx_text_fit(S, FONT_SMALL, cx - w / 2, ly, t, max, OPAQUE(0xFFFFFFu));
    }
}

void desktop_items_paint(gfx_rect clip) {
    for (uint32_t i = 0; i < item_count; ++i) {
        const item *it = &items[i];
        gfx_rect cell = R(it->x, it->y, CELL_W, CELL_H);
        if (!gfx_rect_overlaps(cell, clip)) continue;
        int sel = (int32_t)i == selected;
        int32_t ix = it->x + (CELL_W - ICON) / 2, iy = it->y + 2;
        if (sel) gfx_round_rect(S, ix - 6, iy - 4, ICON + 12, ICON + 8, 8, ARGB(0x50, 0, 0, 0));
        if (it->e.type == MINT_MFS_DIR) folder_icon(ix, iy); else document_icon(ix, iy, it->e.name);
        label(it, sel);
    }
}

void desktop_items_deselect(void) { selected = -1; }

static int32_t item_at(int32_t x, int32_t y) {
    for (uint32_t i = 0; i < item_count; ++i)
        if (gfx_rect_contains(R(items[i].x + 8, items[i].y, CELL_W - 16, CELL_H - 8), x, y)) return (int32_t)i;
    return -1;
}

static void open_item(uint32_t i) {
    static char path[420];
    copy_text(path, sizeof(path), desktop_dir); str_append(path, sizeof(path), "/"); str_append(path, sizeof(path), items[i].e.name);
    if (items[i].e.type == MINT_MFS_DIR) files_open_location(path, 0, 0);
    else if (is_text(items[i].e.name)) notes_open_document(path);
    else files_open_location(desktop_dir, items[i].e.name, 0);
}

static void trash_item(uint32_t i) {
    MINT_MFS_VOLUME v;
    if (mint_fs_get_volume(&v) != 0 || v.state != MINT_MFS_VOLUME_READY) return;
    static char from[420], trash[200], to[480];
    copy_text(trash, sizeof(trash), v.home); str_append(trash, sizeof(trash), "/.Trash");
    (void)mint_dir_create(trash);
    copy_text(from, sizeof(from), desktop_dir); str_append(from, sizeof(from), "/"); str_append(from, sizeof(from), items[i].e.name);
    int64_t rc = -1;
    for (uint32_t k = 1; k < 100u && rc < 0; ++k) {
        copy_text(to, sizeof(to), trash); str_append(to, sizeof(to), "/"); str_append(to, sizeof(to), items[i].e.name);
        if (k > 1u) { char n[12]; format_u64(n, sizeof(n), k); str_append(to, sizeof(to), " "); str_append(to, sizeof(to), n); }
        MINT_MFS_STAT st;
        if (mint_path_stat(to, &st) == 0) continue;
        rc = mint_path_rename(from, to);
        if (rc < 0) break;
    }
    if (rc >= 0) ui_sound(UI_SOUND_TRASH); else ui_sound(UI_SOUND_ALERT);
    selected = -1;
    desktop_items_refresh(1);
}

int desktop_items_click(int32_t x, int32_t y, uint32_t clicks) {
    int32_t hit = item_at(x, y);
    gfx_rect b = desktop_items_bounds();
    if (hit < 0) {
        if (selected >= 0) { selected = -1; shell_damage(b); }
        return 0;
    }
    selected = hit;
    shell_damage(b);
    if (clicks >= 2) open_item((uint32_t)hit);
    return 1;
}

int desktop_items_key(const MINT_DESKTOP_KEY_EVENT *k) {
    if (selected < 0 || (uint32_t)selected >= item_count) return 0;
    int cmd = (k->modifiers & MINT_KEYMOD_GUI) != 0;
    uint32_t i = (uint32_t)selected;
    if (k->codepoint == '\n' && !cmd) { files_open_location(desktop_dir, items[i].e.name, 1); return 1; }
    if (cmd && (k->codepoint == 'o' || k->codepoint == 'O' || k->scancode == 0x150u)) { open_item(i); return 1; }
    if (cmd && k->codepoint == 8u) { trash_item(i); shell_damage(desktop_items_bounds()); return 1; }
    if (k->codepoint == 27u) { selected = -1; shell_damage(desktop_items_bounds()); return 1; }
    int32_t next = selected;
    if (k->scancode == 0x150u) next = selected + 1;                 /* down */
    else if (k->scancode == 0x148u) next = selected - 1;            /* up */
    else if (k->scancode == 0x14Bu || k->scancode == 0x14Du) {      /* left / right: neighbouring column */
        int32_t dx = k->scancode == 0x14Bu ? -CELL_W : CELL_W;
        for (uint32_t j = 0; j < item_count; ++j)
            if (items[j].x == items[i].x + dx && items[j].y == items[i].y) next = (int32_t)j;
    } else return 0;
    if (next >= 0 && (uint32_t)next < item_count) { selected = next; shell_damage(desktop_items_bounds()); }
    return 1;
}
