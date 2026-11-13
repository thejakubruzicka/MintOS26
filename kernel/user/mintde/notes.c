/* Notes — many notes, like macOS Notes.
 *
 * Each note is an ordinary text file in ~/Documents/Notes/ on MintOS HD, so
 * notes show up in Files and Terminal (`cat`) and survive restarts. The
 * first line is the title and, inside the Notes folder, also the file name
 * ("Shopping list.txt"); the file is renamed when the title changes. The list
 * is sorted by last edit and grouped Today / Yesterday / Previous 7 Days /
 * Previous 30 Days / Earlier, with a search field over titles and the start
 * of every note. New Note (Command-N), Delete (Command-Delete, to the Trash),
 * and edits are saved atomically about a second after typing stops, and at
 * once before Shut Down/Restart (notes_flush). Files can open any other text
 * file here; it is edited in place and never renamed. The open note is
 * remembered in the "notes.selected" preference.
 *
 * Migration: the single ~/Documents/Notes.txt of MintOS 26.5.013.0 and a
 * session-RAM note typed before MintOS HD existed become notes the first
 * time the Notes folder is created. Without MintOS HD, Notes keeps one note
 * in the session RAM store as before. */
#include "mintde.h"

#define S (&gfx_screen)
#define NOTES_MAX 128u
#define NOTE_TEXT_MAX 16384u
#define NOTE_RAM_MAX (MINT_RAMFS_FILE_CAPACITY - 1u)
#define NOTE_LINES 1200u
#define ROW_H 56
#define HEAD_H 26
#define SAVE_DELAY_MS 1200u

typedef struct note_item {
    char name[MINT_MFS_NAME_MAX + 1];
    char title[80];
    char snippet[200];
    int64_t mtime;
    uint64_t size;
} note_item;
typedef struct note_line { uint32_t start, len; uint8_t title; } note_line;

enum { MODE_NONE = 0, MODE_DISK, MODE_SESSION };
static note_item notes[NOTES_MAX];
static uint32_t note_count;
static uint32_t notes_mode;
static char notes_dir[MINT_MFS_PATH_MAX];
static char notes_home[64];
static int32_t sel = -1;                 /* open note in notes[], or -1 */
static int open_external;                /* open note is a file outside the Notes folder */
static char open_path[MINT_MFS_PATH_MAX];
static char ext_name[MINT_MFS_NAME_MAX + 1];
static char text[NOTE_TEXT_MAX];
static size_t text_len;
static uint32_t text_rev;
static int dirty, readonly, save_failed, fresh, follow = 1;
static uint64_t edit_ms;
static char search[48];
static size_t search_len;
static int search_focus;
static int32_t list_scroll, edit_scroll;
static note_line lines[NOTE_LINES];
static uint32_t line_count, layout_rev = 0xFFFFFFFFu;
static int32_t layout_width = -1;
static char work[MINT_MFS_PATH_MAX], work2[MINT_MFS_PATH_MAX];
static char prefix_buf[512];
static char message[96];

static gfx_rect R(int32_t x, int32_t y, int32_t w, int32_t h) { return gfx_rect_xywh(x, y, w, h); }
static int32_t W(gfx_rect r) { return r.x1 - r.x0; }
static int32_t H(gfx_rect r) { return r.y1 - r.y0; }
static int in(gfx_rect r, int32_t x, int32_t y) { return gfx_rect_contains(r, x, y); }
static int same(const char *a, const char *b) { size_t i = 0; while (a[i] && a[i] == b[i]) ++i; return a[i] == b[i]; }
static uint64_t now_ms(void) {
    mint_timespec64 t = {0, 0};
    (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int ci_has(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (size_t i = 0; hay[i]; ++i) {
        size_t j = 0;
        while (needle[j] && hay[i + j] && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (!needle[j]) return 1;
    }
    return 0;
}
static int ends_ci(const char *s, const char *suffix) {
    size_t n = text_length(s), m = text_length(suffix);
    if (n <= m) return 0;
    for (size_t i = 0; i < m; ++i) if (lower(s[n - m + i]) != lower(suffix[i])) return 0;
    return 1;
}
static int utf8_put(char *buf, size_t *len, size_t cap, uint32_t cp) {
    char t[4]; size_t n;
    if (cp < 0x80u) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800u) { t[0] = (char)(0xC0u | (cp >> 6)); t[1] = (char)(0x80u | (cp & 0x3Fu)); n = 2; }
    else if (cp < 0x10000u) { t[0] = (char)(0xE0u | (cp >> 12)); t[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); t[2] = (char)(0x80u | (cp & 0x3Fu)); n = 3; }
    else return 0;
    if (*len + n + 1u > cap) return 0;
    for (size_t i = 0; i < n; ++i) buf[(*len)++] = t[i];
    buf[*len] = '\0';
    return 1;
}
static void utf8_back(char *buf, size_t *len) {
    if (!*len) return;
    do --*len; while (*len && ((unsigned char)buf[*len] & 0xC0u) == 0x80u);
    buf[*len] = '\0';
}

/* Drops an incomplete UTF-8 sequence at the end of buf[0..n). */
static size_t utf8_whole(const char *buf, size_t n) {
    size_t lead = n, back = 0;
    while (lead && back < 4u && ((unsigned char)buf[lead - 1u] & 0xC0u) == 0x80u) { --lead; ++back; }
    if (!lead) return n;
    unsigned char c = (unsigned char)buf[lead - 1u];
    size_t need = c < 0x80u ? 1u : (c & 0xE0u) == 0xC0u ? 2u : (c & 0xF0u) == 0xE0u ? 3u : 4u;
    return n - (lead - 1u) < need ? lead - 1u : n;
}
/* Title (first non-empty line) and preview text (what follows). */
static void title_snippet(const char *b, size_t n, char *title, size_t tcap, char *snip, size_t scap) {
    size_t i = 0, t = 0, s = 0;
    while (i < n && (b[i] == '\n' || b[i] == ' ' || b[i] == '\r' || b[i] == '\t')) ++i;
    while (i < n && b[i] != '\n') {
        if (t + 1u < tcap && b[i] != '\r' && (unsigned char)b[i] >= 0x20u) title[t++] = b[i];
        ++i;
    }
    t = utf8_whole(title, t);
    while (t && title[t - 1u] == ' ') --t;
    title[t] = '\0';
    int space = 0;
    for (; i < n && s + 2u < scap; ++i) {
        char c = b[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { space = s > 0; continue; }
        if ((unsigned char)c < 0x20u) continue;
        if (space) { snip[s++] = ' '; space = 0; }
        snip[s++] = c;
    }
    s = utf8_whole(snip, s);
    snip[s] = '\0';
    if (!title[0]) copy_text(title, tcap, "New Note");
}

/* File-name stem from a title: no "/", ":" or "\", no leading dot, at most 60 bytes. */
static void title_stem(const char *title, char *out, size_t cap) {
    size_t o = 0, lim = cap - 1u < 60u ? cap - 1u : 60u;
    for (size_t i = 0; title[i] && o < lim; ++i) {
        char c = title[i];
        if ((unsigned char)c < 0x20u) continue;
        if (!o && (c == '.' || c == ' ')) continue;
        if (c == '/' || c == ':' || c == '\\') c = '-';
        out[o++] = c;
    }
    o = utf8_whole(out, o);                                       /* cut whole characters only */
    while (o && (out[o - 1u] == ' ' || out[o - 1u] == '.')) --o;
    out[o] = '\0';
    if (!o) copy_text(out, cap, "New Note");
}
static const char *ext_of(const char *name) { return ends_ci(name, ".md") ? ".md" : ".txt"; }
/* "stem.txt" or "stem 7.txt" already carries this stem. */
static int name_has_stem(const char *name, const char *stem, const char *ext) {
    size_t n = text_length(name), s = text_length(stem), e = text_length(ext);
    if (n < s + e) return 0;
    for (size_t i = 0; i < s; ++i) if (name[i] != stem[i]) return 0;
    if (n == s + e) return 1;
    if (name[s] != ' ' || n < s + e + 2u) return 0;
    for (size_t i = s + 1u; i < n - e; ++i) if (name[i] < '0' || name[i] > '9') return 0;
    return 1;
}
static int join(char *out, size_t cap, const char *dir, const char *name) {
    if (text_length(dir) + text_length(name) + 2u > cap) return 0;
    copy_text(out, cap, dir); str_append(out, cap, "/"); str_append(out, cap, name);
    return 1;
}
static int path_exists(const char *path) { MINT_MFS_STAT st; return mint_path_stat(path, &st) == 0; }
/* Free name "stem.ext", "stem 2.ext", ... inside dir. */
static int unique_name(const char *dir, const char *stem, const char *ext, char *out, size_t cap) {
    for (uint32_t n = 1; n < 200u; ++n) {
        copy_text(out, cap, stem);
        if (n > 1u) { char num[12]; format_u64(num, sizeof(num), n); str_append(out, cap, " "); str_append(out, cap, num); }
        str_append(out, cap, ext);
        if (!join(work2, sizeof(work2), dir, out)) return 0;
        if (!path_exists(work2)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ list */
static void read_prefix(const char *path, note_item *it) {
    size_t n = 0;
    int64_t fd = mint_file_open(path, MINT_O_READ);
    if (fd >= 0) {
        int64_t r = mint_file_read(fd, prefix_buf, sizeof(prefix_buf) - 1u);
        if (r > 0) n = (size_t)r;
        (void)mint_file_close(fd);
    }
    title_snippet(prefix_buf, n, it->title, sizeof(it->title), it->snippet, sizeof(it->snippet));
}
static int32_t find_note(const char *name);
/* Newest first; the open note stays selected. */
static void sort_notes(void) {
    static char keep[MINT_MFS_NAME_MAX + 1];
    keep[0] = '\0';
    if (sel >= 0 && (uint32_t)sel < note_count) copy_text(keep, sizeof(keep), notes[sel].name);
    for (uint32_t i = 1; i < note_count; ++i) {
        note_item t = notes[i];
        uint32_t j = i;
        while (j && notes[j - 1u].mtime < t.mtime) { notes[j] = notes[j - 1u]; --j; }
        notes[j] = t;
    }
    if (keep[0]) sel = find_note(keep);
}
static void refresh_list(void) {
    static MINT_MFS_DIRENT chunk[MINT_MFS_LIST_MAX];
    note_count = 0;
    for (uint32_t start = 0; note_count < NOTES_MAX;) {
        int64_t n = mint_dir_list(notes_dir, start, chunk);
        if (n <= 0) break;
        for (int64_t i = 0; i < n && note_count < NOTES_MAX; ++i) {
            MINT_MFS_DIRENT *d = &chunk[i];
            if (d->type != MINT_MFS_FILE || d->name[0] == '.' || !(ends_ci(d->name, ".txt") || ends_ci(d->name, ".md"))) continue;
            note_item *it = &notes[note_count++];
            copy_text(it->name, sizeof(it->name), d->name);
            it->mtime = d->mtime; it->size = d->size;
            if (join(work, sizeof(work), notes_dir, d->name)) read_prefix(work, it);
        }
        start += (uint32_t)n;
        if (n < (int64_t)MINT_MFS_LIST_MAX) break;
    }
    sel = -1;
    sort_notes();
}
static int32_t find_note(const char *name) {
    for (uint32_t i = 0; i < note_count; ++i) if (same(notes[i].name, name)) return (int32_t)i;
    return -1;
}

/* ------------------------------------------------------------ open / save */
static void load_text(const char *path) {
    text_len = 0; text[0] = '\0'; readonly = 0; dirty = 0; save_failed = 0; fresh = 0;
    edit_scroll = 0; follow = 1; ++text_rev;
    int64_t n = mint_file_load(path, text, sizeof(text) - 1u);
    if (n < 0) return;
    text_len = (size_t)n; text[text_len] = '\0';
    if (text_len >= sizeof(text) - 1u) readonly = 1;                 /* too large to edit safely */
    for (size_t i = 0; i < text_len; ++i) if (!text[i]) { readonly = 1; break; }
}
static void save_session(void) {
    int64_t h = mint_fs_open("NOTES.TXT", MINT_FS_OPEN_WRITE | MINT_FS_OPEN_TRUNC);
    if (h < 0) { save_failed = 1; return; }
    (void)mint_fs_write((uint64_t)h, text, text_len > NOTE_RAM_MAX ? NOTE_RAM_MAX : text_len);
    (void)mint_fs_close((uint64_t)h);
    dirty = 0; save_failed = 0;
}
/* Renames the open note after its title (Notes folder only). */
static void rename_to_title(void) {
    if (open_external || sel < 0 || readonly) return;
    note_item *it = &notes[sel];
    char title[80], snip[8], stem[64], name[MINT_MFS_NAME_MAX + 1];
    title_snippet(text, text_len < 400u ? text_len : 400u, title, sizeof(title), snip, sizeof(snip));
    title_stem(title, stem, sizeof(stem));
    const char *ext = ext_of(it->name);
    if (name_has_stem(it->name, stem, ext)) return;
    if (!unique_name(notes_dir, stem, ext, name, sizeof(name)) || !join(work2, sizeof(work2), notes_dir, name)) return;
    if (mint_path_rename(open_path, work2) < 0) return;
    copy_text(it->name, sizeof(it->name), name);
    copy_text(open_path, sizeof(open_path), work2);
    (void)prefs_set_str("notes.selected", name);
}
int notes_flush(void) {
    if (!dirty) return 0;
    if (notes_mode == MODE_SESSION) { save_session(); return save_failed ? -1 : 0; }
    if (notes_mode != MODE_DISK || !open_path[0] || readonly) return 0;
    int64_t rc = mint_file_save(open_path, text, text_len);
    if (rc < 0) { save_failed = 1; edit_ms = now_ms() + 5000u; return (int)rc; }
    dirty = 0; save_failed = 0;
    if (!open_external && sel >= 0) {
        rename_to_title();
        note_item *it = &notes[sel];
        it->mtime = shell_now_epoch(); it->size = text_len;
        title_snippet(text, text_len, it->title, sizeof(it->title), it->snippet, sizeof(it->snippet));
        sort_notes();
    }
    return 0;
}
/* A new note left empty disappears, as in macOS Notes. */
static void discard_if_empty(void) {
    if (notes_mode != MODE_DISK || open_external || sel < 0 || !fresh || text_len) return;
    (void)mint_path_remove(open_path);
    for (uint32_t i = (uint32_t)sel; i + 1u < note_count; ++i) notes[i] = notes[i + 1u];
    --note_count;
    sel = -1; open_path[0] = '\0'; fresh = 0; dirty = 0;
}
static void select_note(int32_t i) {
    if (i == sel && !open_external) return;
    (void)notes_flush();
    if (i != sel) {
        int32_t before = sel;
        discard_if_empty();
        if (before >= 0 && sel < 0 && i > before) --i;       /* list shifted up */
    }
    open_external = 0;
    if (i < 0 || (uint32_t)i >= note_count) { sel = -1; open_path[0] = '\0'; text_len = 0; text[0] = '\0'; ++text_rev; return; }
    sel = i;
    if (!join(open_path, sizeof(open_path), notes_dir, notes[i].name)) { open_path[0] = '\0'; return; }
    load_text(open_path);
    (void)prefs_set_str("notes.selected", notes[i].name);
}

/* First use of the Notes folder: bring older notes in. */
static void migrate_old_notes(void) {
    char title[80], snip[8], stem[64], name[MINT_MFS_NAME_MAX + 1];
    copy_text(work, sizeof(work), notes_home); str_append(work, sizeof(work), "/Documents/Notes.txt");
    int64_t n = mint_file_load(work, text, sizeof(text) - 1u);
    if (n > 0) {
        title_snippet(text, (size_t)n, title, sizeof(title), snip, sizeof(snip));
        title_stem(title, stem, sizeof(stem));
        if (unique_name(notes_dir, stem, ".txt", name, sizeof(name)) && join(work2, sizeof(work2), notes_dir, name))
            (void)mint_path_rename(work, work2);
    }
    int64_t h = mint_fs_open("NOTES.TXT", MINT_FS_OPEN_READ);
    if (h >= 0) {
        int64_t r = mint_fs_read((uint64_t)h, text, NOTE_RAM_MAX);
        (void)mint_fs_close((uint64_t)h);
        if (r > 0) {
            title_snippet(text, (size_t)r, title, sizeof(title), snip, sizeof(snip));
            title_stem(title, stem, sizeof(stem));
            if (unique_name(notes_dir, stem, ".txt", name, sizeof(name)) && join(work2, sizeof(work2), notes_dir, name))
                (void)mint_file_save(work2, text, (size_t)r);
        }
    }
    text_len = 0; text[0] = '\0';
}

/* Picks MintOS HD (~/Documents/Notes) or the session store. */
static void attach(void) {
    MINT_MFS_VOLUME v;
    int disk = mint_fs_get_volume(&v) == 0 && v.state == MINT_MFS_VOLUME_READY;
    if (disk) {
        if (notes_mode == MODE_DISK && same(notes_home, v.home)) return;
        notes_mode = MODE_DISK;
        copy_text(notes_home, sizeof(notes_home), v.home);
        copy_text(notes_dir, sizeof(notes_dir), v.home); str_append(notes_dir, sizeof(notes_dir), "/Documents/Notes");
        MINT_MFS_STAT st;
        if (mint_path_stat(notes_dir, &st) != 0) {
            copy_text(work, sizeof(work), v.home); str_append(work, sizeof(work), "/Documents");
            if (mint_path_stat(work, &st) != 0) (void)mint_dir_create(work);
            if (mint_dir_create(notes_dir) == 0) migrate_old_notes();
        }
        sel = -1; open_external = 0; open_path[0] = '\0';
        refresh_list();
        int32_t want = find_note(prefs_get_str("notes.selected", ""));
        select_note(want >= 0 ? want : note_count ? 0 : -1);
        return;
    }
    if (notes_mode == MODE_SESSION) return;
    notes_mode = MODE_SESSION;
    note_count = 0; sel = -1; open_external = 0; open_path[0] = '\0';
    text_len = 0; text[0] = '\0'; readonly = 0; dirty = 0; ++text_rev;
    int64_t h = mint_fs_open("NOTES.TXT", MINT_FS_OPEN_READ);
    if (h >= 0) {
        int64_t n = mint_fs_read((uint64_t)h, text, NOTE_RAM_MAX);
        if (n > 0) text_len = (size_t)n;
        text[text_len] = '\0';
        (void)mint_fs_close((uint64_t)h);
    }
}

void notes_home_changed(void) {
    (void)notes_flush();
    notes_mode = MODE_NONE; sel = -1; open_external = 0; open_path[0] = '\0'; note_count = 0;
}
void notes_opened(void) {
    if (notes_mode == MODE_SESSION) notes_mode = MODE_NONE;    /* MintOS HD may have appeared */
    attach();
    if (notes_mode == MODE_DISK && !open_external) {
        char keep[MINT_MFS_NAME_MAX + 1] = {0};
        if (sel >= 0) copy_text(keep, sizeof(keep), notes[sel].name);
        (void)notes_flush();
        refresh_list();
        sel = keep[0] ? find_note(keep) : -1;
    }
}
void notes_open_file(const char *path) {
    attach();
    if (notes_mode != MODE_DISK) return;
    (void)notes_flush();
    discard_if_empty();
    size_t n = text_length(path), slash = n;
    while (slash && path[slash - 1u] != '/') --slash;
    if (slash > 1u && text_length(notes_dir) == slash - 1u) {
        int inside = 1;
        for (size_t i = 0; i + 1u < slash; ++i) if (path[i] != notes_dir[i]) inside = 0;
        if (inside) {
            refresh_list();
            int32_t i = find_note(path + slash);
            if (i >= 0) { sel = -1; select_note(i); return; }
        }
    }
    sel = -1; open_external = 1;
    copy_text(open_path, sizeof(open_path), path);
    copy_text(ext_name, sizeof(ext_name), path + slash);
    load_text(open_path);
    search[0] = '\0'; search_len = 0;
}

static int new_note(void) {
    attach();
    if (notes_mode != MODE_DISK || note_count >= NOTES_MAX) return 0;
    (void)notes_flush();
    discard_if_empty();
    char name[MINT_MFS_NAME_MAX + 1];
    if (!unique_name(notes_dir, "New Note", ".txt", name, sizeof(name)) || !join(work, sizeof(work), notes_dir, name)) return 0;
    if (mint_file_save(work, "", 0) < 0) { copy_text(message, sizeof(message), "The note couldn\xE2\x80\x99t be created on MintOS HD."); return 0; }
    for (uint32_t i = note_count; i > 0; --i) notes[i] = notes[i - 1u];
    ++note_count;
    note_item *it = &notes[0];
    copy_text(it->name, sizeof(it->name), name);
    copy_text(it->title, sizeof(it->title), "New Note");
    it->snippet[0] = '\0'; it->mtime = shell_now_epoch(); it->size = 0;
    sel = 0; open_external = 0;
    copy_text(open_path, sizeof(open_path), work);
    text_len = 0; text[0] = '\0'; ++text_rev; readonly = 0; dirty = 0; save_failed = 0; fresh = 1;
    search[0] = '\0'; search_len = 0; search_focus = 0; list_scroll = 0; edit_scroll = 0; follow = 1;
    message[0] = '\0';
    (void)prefs_set_str("notes.selected", name);
    return 1;
}
static int delete_note(void) {
    if (notes_mode != MODE_DISK || open_external || sel < 0) return 0;
    if (fresh && !text_len) { discard_if_empty(); select_note(note_count ? 0 : -1); return 1; }
    (void)notes_flush();
    char trash[MINT_MFS_PATH_MAX], name[MINT_MFS_NAME_MAX + 1], stem[MINT_MFS_NAME_MAX + 1];
    copy_text(trash, sizeof(trash), notes_home); str_append(trash, sizeof(trash), "/.Trash");
    if (!path_exists(trash)) (void)mint_dir_create(trash);
    const char *ext = ext_of(notes[sel].name);
    copy_text(stem, sizeof(stem), notes[sel].name);
    stem[text_length(stem) - text_length(ext)] = '\0';
    if (!unique_name(trash, stem, ext, name, sizeof(name)) || !join(work, sizeof(work), trash, name)) return 1;
    int64_t rc = mint_path_rename(open_path, work);
    if (rc < 0) { copy_text(message, sizeof(message), "The note couldn\xE2\x80\x99t be moved to the Trash."); return 1; }
    uint32_t at = (uint32_t)sel;
    for (uint32_t i = at; i + 1u < note_count; ++i) notes[i] = notes[i + 1u];
    --note_count;
    sel = -1; open_path[0] = '\0'; dirty = 0;
    select_note(note_count ? (int32_t)(at < note_count ? at : note_count - 1u) : -1);
    return 1;
}
int notes_command_enabled(uint32_t command) {
    if (command == NOTES_CMD_NEW) return notes_mode == MODE_DISK;
    if (command == NOTES_CMD_DELETE) return notes_mode == MODE_DISK && !open_external && sel >= 0;
    return 0;
}
int notes_command(uint32_t command) {
    if (command == NOTES_CMD_NEW) return new_note();
    if (command == NOTES_CMD_DELETE) return delete_note();
    return 0;
}
int notes_background(uint64_t ms) {
    if (dirty && (int64_t)(ms - edit_ms) >= (int64_t)SAVE_DELAY_MS) { (void)notes_flush(); return 1; }
    return 0;
}

/* ------------------------------------------------------------ geometry */
static gfx_rect search_rect(const mde_layout *l) { return R(l->toolbar.x1 - 196, l->toolbar.y0 + 12, 180, 28); }
static gfx_rect compose_rect(const mde_layout *l) { gfx_rect s = search_rect(l); return R(s.x0 - 44, s.y0, 34, 28); }
static gfx_rect trash_rect(const mde_layout *l) { gfx_rect s = search_rect(l); return R(s.x0 - 84, s.y0, 34, 28); }

/* The sidebar list as headers and rows; returns the entry count. */
typedef struct list_entry { int32_t note; const char *header; int32_t y, h; } list_entry;
static list_entry entries[NOTES_MAX + 8u];
static const char *section_of(int64_t mtime, int64_t now) {
    if (!now) return "Notes";
    int64_t d = now / 86400 - mtime / 86400;
    if (mtime <= 0) return "Earlier";
    if (d <= 0) return "Today";
    if (d == 1) return "Yesterday";
    if (d < 7) return "Previous 7 Days";
    if (d < 30) return "Previous 30 Days";
    return "Earlier";
}
static int note_matches(uint32_t i) {
    if (!search[0]) return 1;
    if ((int32_t)i == sel) return ci_has(text, search);
    return ci_has(notes[i].title, search) || ci_has(notes[i].snippet, search);
}
static uint32_t build_entries(void) {
    uint32_t n = 0;
    int32_t y = 0;
    if (open_external) {
        entries[n++] = (list_entry){-1, "Opened from Files", y, HEAD_H}; y += HEAD_H;
        entries[n++] = (list_entry){-2, 0, y, ROW_H}; y += ROW_H;
    }
    int64_t now = shell_now_epoch();
    const char *last = 0;
    for (uint32_t i = 0; i < note_count; ++i) {
        if (!note_matches(i)) continue;
        const char *sec = search[0] ? "Results" : section_of(notes[i].mtime, now);
        if (!last || !same(last, sec)) { entries[n++] = (list_entry){-1, sec, y, HEAD_H}; y += HEAD_H; last = sec; }
        entries[n++] = (list_entry){(int32_t)i, 0, y, ROW_H}; y += ROW_H;
    }
    return n;
}
static void row_date(int64_t mtime, char *out, size_t cap) {
    static const char *const wd[7] = {"Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday"};
    int64_t now = shell_now_epoch();
    if (mtime <= 0) { copy_text(out, cap, ""); return; }
    int64_t d = now ? now / 86400 - mtime / 86400 : 99;
    if (now && d <= 0) {
        uint32_t s = (uint32_t)(mtime % 86400);
        char t[6] = {(char)('0' + s / 36000u), (char)('0' + (s / 3600u) % 10u), ':', (char)('0' + (s / 60u % 60u) / 10u), (char)('0' + s / 60u % 10u), 0};
        copy_text(out, cap, t);
    } else if (d == 1) copy_text(out, cap, "Yesterday");
    else if (d < 7) copy_text(out, cap, wd[(uint32_t)((mtime / 86400) % 7)]);
    else {
        shell_format_date(out, cap, mtime);              /* "3 Nov 2026 at 18:40" -> "3 Nov 2026" */
        for (size_t i = 0; out[i]; ++i) if (out[i] == ' ' && out[i + 1] == 'a' && out[i + 2] == 't' && out[i + 3] == ' ') { out[i] = '\0'; break; }
    }
}

/* ------------------------------------------------------------ editor layout */
static int32_t glyph_adv64(int font, uint32_t cp) {
    const MINT_FONT *f = &mint_fonts[font];
    int32_t lo = 0, hi = (int32_t)f->glyph_count - 1;
    if (cp >= 32u && cp < 127u && f->glyph_count > cp - 32u && f->glyphs[cp - 32u].codepoint == cp) return f->glyphs[cp - 32u].advance64;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (f->glyphs[mid].codepoint == cp) return f->glyphs[mid].advance64;
        if (f->glyphs[mid].codepoint < cp) lo = mid + 1; else hi = mid - 1;
    }
    return cp == '?' ? 0 : glyph_adv64(font, '?');
}
static uint32_t next_cp(const char *s, size_t n, size_t *i) {
    const uint8_t *p = (const uint8_t *)s + *i;
    uint32_t c = p[0];
    if (c < 0x80u || *i + 1u >= n) { *i += 1; return c < 0x80u ? c : '?'; }
    if ((c & 0xE0u) == 0xC0u) { *i += 2; return ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu); }
    if ((c & 0xF0u) == 0xE0u && *i + 2u < n) { *i += 3; return ((c & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); }
    *i += 1;
    return '?';
}
static void add_line(uint32_t start, uint32_t len, int title) {
    if (line_count < NOTE_LINES) lines[line_count++] = (note_line){start, len, (uint8_t)title};
}
/* Word wrap with the exact advances gfx_text uses. */
static void layout(int32_t maxw) {
    if (layout_rev == text_rev && layout_width == maxw) return;
    layout_rev = text_rev; layout_width = maxw;
    line_count = 0;
    size_t k = 0;
    int first = 1;
    int64_t limit = (int64_t)maxw * 64;
    for (;;) {
        size_t pe = k;
        while (pe < text_len && text[pe] != '\n') ++pe;
        int font = first ? FONT_TITLE : FONT_BODY;
        size_t s = k;
        if (s == pe) add_line((uint32_t)s, 0, first);
        while (s < pe) {
            int64_t pen = 0;
            size_t i = s, brk = 0;
            while (i < pe) {
                size_t at = i;
                uint32_t cp = next_cp(text, pe, &i);
                pen += glyph_adv64(font, cp);
                if (pen > limit && at > s) { i = at; break; }
                if (cp == ' ') brk = i;
            }
            size_t end = i;
            if (i < pe && brk > s) end = brk;                 /* break after the last space */
            add_line((uint32_t)s, (uint32_t)(end - s), first);
            s = end;
            if (line_count >= NOTE_LINES) return;
        }
        first = 0;
        if (pe >= text_len) break;
        k = pe + 1u;
    }
}
static int32_t line_height(const note_line *ln) { return ln->title ? 32 : 21; }

/* ------------------------------------------------------------ painting */
static void compose_glyph(gfx_rect r, uint32_t c) {
    int32_t x = r.x0 + W(r) / 2 - 7, y = r.y0 + H(r) / 2 - 7;
    gfx_round_rect_stroke(S, x, y + 2, 13, 13, 3, OPAQUE(c));
    gfx_line(S, x + 6, y + 8, x + 14, y, OPAQUE(c));
    gfx_line(S, x + 7, y + 8, x + 15, y, OPAQUE(c));
}
void notes_paint(const mde_layout *l) {
    attach();
    gfx_rect sb = l->sidebar, c = l->content;
    gfx_rect saved = gfx_clip_get();
    /* ---- toolbar */
    ui_label(l->toolbar.x0 + 16, l->toolbar.y0 + 33, FONT_HEADLINE, "Notes", T.text);
    char count[32] = "";
    if (notes_mode == MODE_DISK) {
        format_u64(count, sizeof(count), note_count);
        str_append(count, sizeof(count), note_count == 1u ? " note" : " notes");
    } else copy_text(count, sizeof(count), "Session");
    ui_label(l->toolbar.x0 + 16 + gfx_text_width(FONT_HEADLINE, "Notes") + 10, l->toolbar.y0 + 33, FONT_CAPTION, count, T.text3);
    compose_glyph(compose_rect(l), notes_command_enabled(NOTES_CMD_NEW) ? T.text2 : T.text3);
    gfx_rect tr = trash_rect(l);
    ui_symbol_glyph(tr.x0 + W(tr) / 2, tr.y0 + H(tr) / 2, SYM_TRASH, OPAQUE(notes_command_enabled(NOTES_CMD_DELETE) ? T.text2 : T.text3));
    ui_search_field(search_rect(l), search, search_focus && l->focused);

    /* ---- list */
    gfx_clip_set(gfx_rect_intersect(saved, sb));
    if (notes_mode == MODE_SESSION) {
        ui_sidebar_header(sb.x0 + 18, sb.y0 + 18, "Session");
        gfx_rect item = R(sb.x0 + 10, sb.y0 + 28, W(sb) - 20, 52);
        gfx_round_rect(S, item.x0, item.y0, W(item), H(item), 7, OPAQUE(l->focused ? 0xF7C833u : T.selection_inactive));
        char title[80], snip[200];
        title_snippet(text, text_len, title, sizeof(title), snip, sizeof(snip));
        gfx_text_fit(S, FONT_BODY_BOLD, item.x0 + 12, item.y0 + 20, title, W(item) - 24, OPAQUE(l->focused ? 0x1D1D1Fu : T.text));
        gfx_text_fit(S, FONT_CAPTION, item.x0 + 12, item.y0 + 40, snip[0] ? snip : "No additional text", W(item) - 24,
                     OPAQUE(l->focused ? 0x4A4030u : T.text2));
    } else {
        uint32_t n = build_entries();
        int32_t total = n ? entries[n - 1u].y + entries[n - 1u].h : 0;
        int32_t view = H(sb) - 12;
        if (list_scroll > total - view) list_scroll = total - view;
        if (list_scroll < 0) list_scroll = 0;
        int32_t top = sb.y0 + 6 - list_scroll;
        for (uint32_t e = 0; e < n; ++e) {
            int32_t y = top + entries[e].y;
            if (y + entries[e].h < sb.y0 || y > sb.y1) continue;
            if (entries[e].header) { ui_sidebar_header(sb.x0 + 18, y + 19, entries[e].header); continue; }
            int32_t i = entries[e].note;
            int selected = i == -2 ? open_external : (!open_external && i == sel);
            gfx_rect row = R(sb.x0 + 10, y, W(sb) - 20, ROW_H - 4);
            if (selected) gfx_round_rect(S, row.x0, row.y0, W(row), H(row), 7, OPAQUE(l->focused && !search_focus ? 0xF7C833u : T.selection_inactive));
            uint32_t fg = selected && l->focused && !search_focus ? 0x1D1D1Fu : T.text;
            uint32_t fg2 = selected && l->focused && !search_focus ? 0x4A4030u : T.text2;
            char title[80], snip[200], date[40];
            if (i == -2 || i == sel) title_snippet(text, text_len, title, sizeof(title), snip, sizeof(snip));
            else { copy_text(title, sizeof(title), notes[i].title); copy_text(snip, sizeof(snip), notes[i].snippet); }
            if (i == -2) copy_text(date, sizeof(date), ext_name);
            else row_date(i == sel && dirty ? shell_now_epoch() : notes[i].mtime, date, sizeof(date));
            gfx_text_fit(S, FONT_BODY_BOLD, row.x0 + 12, row.y0 + 21, title, W(row) - 24, OPAQUE(fg));
            int32_t dw = gfx_text_fit(S, FONT_CAPTION_BOLD, row.x0 + 12, row.y0 + 40, date, W(row) - 60, OPAQUE(fg));
            gfx_text_fit(S, FONT_CAPTION, row.x0 + 12 + dw + 8, row.y0 + 40, snip[0] ? snip : "No additional text",
                         W(row) - 32 - dw, OPAQUE(fg2));
            if (!selected) gfx_hline(S, row.x0 + 12, row.y1 + 1, W(row) - 24, T.separator);
        }
        if (!n) ui_label_center(sb.x0 + W(sb) / 2, sb.y0 + 60, FONT_BODY, search[0] ? "No Results" : "No Notes", T.text3);
    }
    gfx_clip_set(saved);

    /* ---- editor */
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(T.content));
    gfx_clip_set(gfx_rect_intersect(saved, c));
    int has_note = notes_mode == MODE_SESSION || open_external || sel >= 0;
    if (!has_note) {
        ui_label_center(c.x0 + W(c) / 2, c.y0 + H(c) / 2 - 10, FONT_TITLE, "No Note Selected", T.text3);
        ui_label_center(c.x0 + W(c) / 2, c.y0 + H(c) / 2 + 16, FONT_BODY,
                        "Press \xE2\x8C\x98N or click the compose button to create a note.", T.text3);
        gfx_clip_set(saved);
        return;
    }
    char where[MINT_MFS_NAME_MAX + 64];
    if (notes_mode == MODE_SESSION) copy_text(where, sizeof(where), "Session (RAM) \xE2\x80\x94 cleared at restart");
    else if (readonly) copy_text(where, sizeof(where), "Too large to edit in Notes \xE2\x80\x94 read only");
    else if (save_failed) copy_text(where, sizeof(where), "Not saved \xE2\x80\x94 MintOS HD reported an error");
    else if (message[0]) copy_text(where, sizeof(where), message);
    else if (open_external) {
        copy_text(where, sizeof(where), ext_name);
        str_append(where, sizeof(where), dirty ? " \xE2\x80\x94 Edited" : " \xE2\x80\x94 Saved on MintOS HD");
    } else {
        shell_format_date(where, sizeof(where), dirty || fresh ? shell_now_epoch() : notes[sel].mtime);
        str_append(where, sizeof(where), dirty ? " \xC2\xB7 Edited" : " \xC2\xB7 Saved on MintOS HD");
    }
    ui_label_center(c.x0 + W(c) / 2, c.y0 + 22, FONT_CAPTION, where, T.text3);
    int32_t x = c.x0 + 28, top = c.y0 + 54, maxw = W(c) - 56;
    layout(maxw);
    int32_t total = 0;
    for (uint32_t i = 0; i < line_count; ++i) total += line_height(&lines[i]);
    int32_t view = c.y1 - 16 - top;
    int32_t max_scroll = total - view > 0 ? total - view : 0;
    if (follow) edit_scroll = max_scroll;
    if (edit_scroll > max_scroll) edit_scroll = max_scroll;
    if (edit_scroll < 0) edit_scroll = 0;
    int32_t y = top - edit_scroll, caret_x = x, caret_y = top;
    static char buf[1024];
    gfx_clip_set(gfx_rect_intersect(saved, R(c.x0, c.y0 + 34, W(c), H(c) - 34)));
    for (uint32_t i = 0; i < line_count; ++i) {
        note_line *ln = &lines[i];
        int font = ln->title ? FONT_TITLE : FONT_BODY;
        uint32_t len = ln->len < sizeof(buf) - 1u ? ln->len : (uint32_t)sizeof(buf) - 1u;
        for (uint32_t q = 0; q < len; ++q) buf[q] = text[ln->start + q];
        buf[len] = '\0';
        int32_t lh = line_height(ln);
        int32_t base = y + (ln->title ? 22 : 15);
        if (base > c.y0 + 20 && base - lh < c.y1) ui_label(x, base, font, buf, T.text);
        caret_x = x + gfx_text_width(font, buf); caret_y = base;
        y += lh;
    }
    int title_caret = line_count ? lines[line_count - 1u].title : 1;
    if (l->focused && !search_focus && !readonly && ((now_ms() / 500u) & 1u) == 0u)
        gfx_fill(S, caret_x + 1, caret_y - (title_caret ? 20 : 14), 2, title_caret ? 24 : 17, OPAQUE(0xE0A800u));
    gfx_clip_set(saved);
}

/* ------------------------------------------------------------ input */
int notes_click(const mde_layout *l, int32_t x, int32_t y, uint32_t clicks) {
    (void)clicks;
    attach();
    message[0] = '\0';
    if (in(search_rect(l), x, y)) { search_focus = 1; return 1; }
    if (in(compose_rect(l), x, y)) { search_focus = 0; return new_note() || 1; }
    if (in(trash_rect(l), x, y)) { (void)delete_note(); return 1; }
    if (in(l->sidebar, x, y)) {
        search_focus = 0;
        if (notes_mode != MODE_DISK) return 1;
        uint32_t n = build_entries();
        int32_t top = l->sidebar.y0 + 6 - list_scroll;
        for (uint32_t e = 0; e < n; ++e) {
            if (entries[e].header || y < top + entries[e].y || y >= top + entries[e].y + entries[e].h) continue;
            if (entries[e].note >= 0) select_note(entries[e].note);
            return 1;
        }
        return 1;
    }
    if (in(l->content, x, y)) { search_focus = 0; return 1; }
    return 0;
}
int notes_scroll(const mde_layout *l, int32_t x, int32_t y, int32_t delta) {
    if (in(l->sidebar, x, y)) { list_scroll -= delta * 40; if (list_scroll < 0) list_scroll = 0; return 1; }
    if (in(l->content, x, y)) { follow = 0; edit_scroll -= delta * 42; if (edit_scroll < 0) edit_scroll = 0; return 1; }
    return 0;
}
int notes_key(const MINT_DESKTOP_KEY_EVENT *k) {
    attach();
    uint32_t ch = k->codepoint;
    int cmd = (k->modifiers & MINT_KEYMOD_GUI) != 0;
    if (cmd) {
        if (ch == 'n' || ch == 'N') return new_note() || 1;
        if (ch == 8u || ch == 127u) return delete_note();
        if (ch == 's' || ch == 'S') { (void)notes_flush(); return 1; }
        if (ch == 'f' || ch == 'F') { search_focus = 1; return 1; }
        return 0;
    }
    if (search_focus) {
        if (ch == 27u) { if (search_len) { search_len = 0; search[0] = '\0'; } else search_focus = 0; }
        else if (ch == 8u) utf8_back(search, &search_len);
        else if (ch == '\n' || ch == 9u) {
            search_focus = 0;
            for (uint32_t i = 0; i < note_count; ++i) if (note_matches(i)) { select_note((int32_t)i); break; }
        }
        else if (ch >= 32u && ch != 127u) (void)utf8_put(search, &search_len, sizeof(search), ch);
        else return 0;
        list_scroll = 0;
        return 1;
    }
    if (k->scancode == 0x148u || k->scancode == 0x150u) {      /* Up/Down with an empty note: walk the list */
        if (notes_mode != MODE_DISK || open_external || !note_count) return 0;
        int32_t next = sel < 0 ? 0 : sel + (k->scancode == 0x150u ? 1 : -1);
        if (next >= 0 && (uint32_t)next < note_count && (k->modifiers & MINT_KEYMOD_ALT)) { select_note(next); return 1; }
        return 0;
    }
    if (readonly) return 0;
    if (notes_mode == MODE_DISK && !open_external && sel < 0) {
        if (!(ch == '\n' || (ch >= 32u && ch != 127u))) return 0;
        if (!new_note()) return 1;
    }
    size_t cap = notes_mode == MODE_SESSION ? NOTE_RAM_MAX + 1u : sizeof(text);
    if (ch == 8u) { if (!text_len) return 1; utf8_back(text, &text_len); }
    else if (ch == '\n' || ch == 9u || (ch >= 32u && ch != 127u)) {
        if (ch == 9u) ch = ' ';
        if (!utf8_put(text, &text_len, cap, ch)) { copy_text(message, sizeof(message), "This note is full."); return 1; }
    }
    else return 0;
    message[0] = '\0';
    ++text_rev;
    dirty = 1; follow = 1; edit_ms = now_ms();
    if (text_len) fresh = 0;
    return 1;
}
uint32_t notes_cursor(const mde_layout *l, int32_t x, int32_t y) {
    if (in(search_rect(l), x, y) || in(l->content, x, y)) return MINT_CURSOR_SHAPE_IBEAM;
    return MINT_CURSOR_SHAPE_ARROW;
}
