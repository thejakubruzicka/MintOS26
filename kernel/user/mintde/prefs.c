/* MintDE preferences — macOS-style "defaults" on MintOS HD.
 *
 * Keys are "<domain>.<name>", lowercase a-z 0-9 _ - . (at most 63 bytes).
 * Each domain is one small text file, written atomically with mint_file_save:
 *   system.<name>  -> /Library/Preferences/com.mintos.system.prefs  (whole computer)
 *   <domain>.<name> -> ~/Library/Preferences/com.mintos.<domain>.prefs (the account)
 * File format (UTF-8): "name = value" per line, where value is an integer or
 * a double-quoted string with \" \\ \n \t \r escapes; "#" starts a comment.
 * Unknown or malformed lines are skipped, so files from newer MintOS versions
 * still load. Adding a key is one line at the call site, e.g.
 *     int64_t volume = prefs_get_int("sound.volume", 75);
 *     prefs_set_int("sound.volume", volume);
 * Changes are saved about one second after the last change (prefs_background)
 * or at once with prefs_flush() (Shut Down, Restart, Log Out). Without a
 * MintOS disk the store keeps working in memory for the session.
 * See docs/DATA-SAVING.md. */
#ifdef MINT_PREFS_HOST_TEST
#include "prefs_host.h"
#else
#include "mintde.h"
#endif

#define PREFS_MAX 160u
#define PREFS_KEY_MAX 64u
#define PREFS_VALUE_MAX 256u
#define PREFS_DOMAINS 24u
#define PREFS_DOMAIN_MAX 24u
#define PREFS_FILE_MAX 16384u
#define PREFS_SAVE_DELAY_MS 1000u

typedef struct pref_entry {
    char key[PREFS_KEY_MAX];          /* full key: "desktop.accent" */
    char value[PREFS_VALUE_MAX];
    uint8_t used, is_int;
} pref_entry;
typedef struct pref_domain {
    char name[PREFS_DOMAIN_MAX];
    uint8_t used, loaded, dirty;
} pref_domain;

static pref_entry entries[PREFS_MAX];
static pref_domain domains[PREFS_DOMAINS];
static char prefs_home[MINT_MFS_PATH_MAX];
static int prefs_disk, prefs_changed, prefs_failed;
static uint64_t prefs_last_change_ms;
static char file_buf[PREFS_FILE_MAX];
static char path_buf[MINT_MFS_PATH_MAX];

/* ------------------------------------------------------------ small helpers */
static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) ++n; return n; }
static int seq(const char *a, const char *b) {
    size_t i = 0;
    for (; a[i] && a[i] == b[i]; ++i) {}
    return a[i] == b[i];
}
static void scopy(char *out, size_t cap, const char *in) {
    size_t i = 0;
    if (!cap) return;
    for (; in && in[i] && i + 1u < cap; ++i) out[i] = in[i];
    out[i] = '\0';
}
static void sappend(char *out, size_t cap, const char *in) {
    size_t n = slen(out);
    if (n < cap) scopy(out + n, cap - n, in);
}
static int name_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'; }

int prefs_key_valid(const char *key) {
    size_t n = slen(key), dot = 0;
    if (!n || n >= PREFS_KEY_MAX || !(key[0] >= 'a' && key[0] <= 'z')) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (!name_char(key[i])) return 0;
        if (key[i] == '.' && !dot) dot = i;
    }
    return dot && dot < PREFS_DOMAIN_MAX && dot + 1u < n;
}

/* ------------------------------------------------------------ text format */
size_t prefs_quote(char *out, size_t cap, const char *value) {
    size_t o = slen(out);
    if (o + 3u > cap) return o;
    out[o++] = '"';
    for (size_t i = 0; value && value[i]; ++i) {
        char c = value[i], e = 0;
        if (c == '"' || c == '\\') e = c;
        else if (c == '\n') e = 'n';
        else if (c == '\t') e = 't';
        else if (c == '\r') e = 'r';
        else if ((unsigned char)c < 0x20u) continue;           /* other control bytes are dropped */
        if (o + (e ? 2u : 1u) + 2u > cap) break;
        if (e) { out[o++] = '\\'; out[o++] = e; } else out[o++] = c;
    }
    out[o++] = '"';
    out[o] = '\0';
    return o;
}

static int parse_int(const char *s, size_t n, int64_t *out) {
    size_t i = 0;
    int neg = 0;
    if (i < n && s[i] == '-') { neg = 1; ++i; }
    if (i >= n || n - i > 18u) return 0;
    int64_t v = 0;
    for (; i < n; ++i) { if (s[i] < '0' || s[i] > '9') return 0; v = v * 10 + (s[i] - '0'); }
    *out = neg ? -v : v;
    return 1;
}
static void format_int(char *out, size_t cap, int64_t v) {
    char t[24];
    size_t n = 0;
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while (u && n < sizeof(t));
    size_t o = 0;
    if (v < 0 && o + 1u < cap) out[o++] = '-';
    while (n && o + 1u < cap) out[o++] = t[--n];
    out[o] = '\0';
}

int prefs_parse_text(const char *t, size_t len, prefs_line_fn fn, void *ctx) {
    static char name[PREFS_KEY_MAX], value[PREFS_VALUE_MAX];
    int count = 0;
    for (size_t i = 0; i < len;) {
        size_t e = i;
        while (e < len && t[e] != '\n') ++e;
        size_t q = i;
        while (q < e && (t[q] == ' ' || t[q] == '\t' || t[q] == '\r')) ++q;
        if (q < e && t[q] != '#') {
            size_t nl = 0;
            int ok = 1;
            while (q < e && name_char(t[q])) { if (nl + 1u >= sizeof(name)) { ok = 0; break; } name[nl++] = t[q++]; }
            name[nl] = '\0';
            while (ok && q < e && (t[q] == ' ' || t[q] == '\t')) ++q;
            if (!nl || !(name[0] >= 'a' && name[0] <= 'z') || q >= e || t[q] != '=') ok = 0;
            else ++q;
            while (ok && q < e && (t[q] == ' ' || t[q] == '\t')) ++q;
            int is_int = 0;
            size_t vl = 0;
            if (ok && q < e && t[q] == '"') {
                ++q;
                int closed = 0;
                while (q < e) {
                    char c = t[q++];
                    if (c == '"') { closed = 1; break; }
                    if (c == '\\' && q < e) {
                        char x = t[q++];
                        c = x == 'n' ? '\n' : x == 't' ? '\t' : x == 'r' ? '\r' : x;
                    }
                    if (vl + 1u >= sizeof(value)) { ok = 0; break; }
                    value[vl++] = c;
                }
                if (!closed) ok = 0;
            } else if (ok) {
                size_t s = q;
                while (q < e && t[q] != ' ' && t[q] != '\t' && t[q] != '\r' && t[q] != '#') ++q;
                int64_t v;
                if (q - s == 4u && t[s] == 't' && t[s + 1] == 'r' && t[s + 2] == 'u' && t[s + 3] == 'e') v = 1;
                else if (q - s == 5u && t[s] == 'f' && t[s + 1] == 'a' && t[s + 2] == 'l' && t[s + 3] == 's' && t[s + 4] == 'e') v = 0;
                else if (!parse_int(t + s, q - s, &v)) ok = 0;
                if (ok) { format_int(value, sizeof(value), v); vl = slen(value); is_int = 1; }
            }
            while (ok && q < e && (t[q] == ' ' || t[q] == '\t' || t[q] == '\r')) ++q;
            if (ok && q < e && t[q] != '#') ok = 0;                     /* trailing garbage */
            if (ok) { value[vl] = '\0'; if (fn) fn(ctx, name, value, is_int); ++count; }
        }
        i = e + 1u;
    }
    return count;
}

/* ------------------------------------------------------------ store */
static pref_domain *domain_find(const char *key, int create) {
    char d[PREFS_DOMAIN_MAX];
    size_t n = 0;
    while (key[n] && key[n] != '.' && n + 1u < sizeof(d)) { d[n] = key[n]; ++n; }
    d[n] = '\0';
    pref_domain *free_slot = 0;
    for (uint32_t i = 0; i < PREFS_DOMAINS; ++i) {
        if (domains[i].used && seq(domains[i].name, d)) return &domains[i];
        if (!domains[i].used && !free_slot) free_slot = &domains[i];
    }
    if (!create || !free_slot) return 0;
    scopy(free_slot->name, sizeof(free_slot->name), d);
    free_slot->used = 1; free_slot->loaded = 0; free_slot->dirty = 0;
    return free_slot;
}
static pref_entry *entry_find(const char *key) {
    for (uint32_t i = 0; i < PREFS_MAX; ++i) if (entries[i].used && seq(entries[i].key, key)) return &entries[i];
    return 0;
}
static int entry_store(const char *key, const char *value, int is_int) {
    if (slen(value) >= PREFS_VALUE_MAX) return 0;
    pref_entry *e = entry_find(key);
    if (!e) for (uint32_t i = 0; i < PREFS_MAX && !e; ++i) if (!entries[i].used) e = &entries[i];
    if (!e) return 0;
    if (e->used && e->is_int == (uint8_t)is_int && seq(e->value, value)) return 2;   /* unchanged */
    scopy(e->key, sizeof(e->key), key);
    scopy(e->value, sizeof(e->value), value);
    e->is_int = (uint8_t)is_int; e->used = 1;
    return 1;
}

static void domain_path(const pref_domain *d, char *out, size_t cap) {
    if (seq(d->name, "system")) { scopy(out, cap, "/Library/Preferences/com.mintos.system.prefs"); return; }
    scopy(out, cap, prefs_home);
    sappend(out, cap, "/Library/Preferences/com.mintos.");
    sappend(out, cap, d->name);
    sappend(out, cap, ".prefs");
}

typedef struct { const char *domain; } parse_ctx;
static void load_line(void *ctx, const char *name, const char *value, int is_int) {
    char key[PREFS_KEY_MAX];
    scopy(key, sizeof(key), ((parse_ctx *)ctx)->domain);
    sappend(key, sizeof(key), ".");
    sappend(key, sizeof(key), name);
    if (prefs_key_valid(key) && slen(key) == slen(((parse_ctx *)ctx)->domain) + 1u + slen(name)) (void)entry_store(key, value, is_int);
}
int prefs_parse(const char *domain, const char *text, size_t len) {
    parse_ctx c = {domain};
    char probe[PREFS_KEY_MAX];
    scopy(probe, sizeof(probe), domain); sappend(probe, sizeof(probe), ".x");
    if (!prefs_key_valid(probe)) return 0;
    return prefs_parse_text(text, len, load_line, &c);
}

/* Lazily loads a domain's file the first time one of its keys is used. */
static void domain_load(pref_domain *d) {
    if (d->loaded) return;
    d->loaded = 1;
    if (!prefs_disk || (!prefs_home[0] && !seq(d->name, "system"))) return;
    domain_path(d, path_buf, sizeof(path_buf));
    int64_t n = mint_file_load(path_buf, file_buf, sizeof(file_buf) - 1u);
    if (n <= 0) return;
    /* Values already set this session win over what is on disk. */
    static pref_entry saved[8];
    uint32_t kept = 0;
    for (uint32_t i = 0; i < PREFS_MAX && kept < 8u; ++i)
        if (entries[i].used && domain_find(entries[i].key, 0) == d) saved[kept++] = entries[i];
    (void)prefs_parse(d->name, file_buf, (size_t)n);
    for (uint32_t i = 0; i < kept; ++i) (void)entry_store(saved[i].key, saved[i].value, saved[i].is_int);
}

static int compare_keys(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i]) ++i;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}
int64_t prefs_serialize(const char *domain, char *out, size_t cap) {
    pref_domain *d = 0;
    for (uint32_t i = 0; i < PREFS_DOMAINS; ++i) if (domains[i].used && seq(domains[i].name, domain)) d = &domains[i];
    if (cap < 2u) return -1;
    out[0] = '\0';
    sappend(out, cap, "# com.mintos.");
    sappend(out, cap, domain);
    sappend(out, cap, " \xE2\x80\x94 MintOS preferences, written by MintDE.\n# One \"name = value\" per line; text values are in quotes.\n");
    size_t name_off = slen(domain) + 1u;
    const char *last = "";
    for (;;) {                                         /* alphabetical, one pass per entry (n <= 160) */
        pref_entry *next = 0;
        for (uint32_t i = 0; i < PREFS_MAX; ++i) {
            pref_entry *e = &entries[i];
            if (!e->used || !d || domain_find(e->key, 0) != d) continue;
            if (compare_keys(e->key, last) <= 0) continue;
            if (!next || compare_keys(e->key, next->key) < 0) next = e;
        }
        if (!next) break;
        size_t before = slen(out);
        sappend(out, cap, next->key + name_off);
        sappend(out, cap, " = ");
        if (next->is_int) sappend(out, cap, next->value);
        else (void)prefs_quote(out, cap, next->value);
        sappend(out, cap, "\n");
        if (slen(out) + 1u >= cap || out[slen(out) - 1u] != '\n') { out[before] = '\0'; return -1; }
        last = next->key;
    }
    return (int64_t)slen(out);
}

/* ------------------------------------------------------------ public API */
static pref_entry *lookup(const char *key) {
    if (!prefs_key_valid(key)) return 0;
    pref_domain *d = domain_find(key, 1);
    if (d) domain_load(d);
    return entry_find(key);
}
const char *prefs_get_str(const char *key, const char *fallback) {
    pref_entry *e = lookup(key);
    return e ? e->value : fallback;
}
int64_t prefs_get_int(const char *key, int64_t fallback) {
    pref_entry *e = lookup(key);
    int64_t v;
    return e && parse_int(e->value, slen(e->value), &v) ? v : fallback;
}
static int set_value(const char *key, const char *value, int is_int) {
    if (!prefs_key_valid(key)) return 0;
    pref_domain *d = domain_find(key, 1);
    if (!d) return 0;
    domain_load(d);
    int rc = entry_store(key, value, is_int);
    if (rc == 1) { d->dirty = 1; prefs_changed = 1; }
    return rc != 0;
}
int prefs_set_str(const char *key, const char *value) { return set_value(key, value ? value : "", 0); }
int prefs_set_int(const char *key, int64_t value) {
    char t[24];
    format_int(t, sizeof(t), value);
    return set_value(key, t, 1);
}
int prefs_remove(const char *key) {
    pref_entry *e = lookup(key);
    if (!e) return 0;
    e->used = 0;
    pref_domain *d = domain_find(key, 0);
    if (d) d->dirty = 1;
    prefs_changed = 1;
    return 1;
}

static void ensure_dir(const char *path) {
    MINT_MFS_STAT st;
    if (mint_path_stat(path, &st) != 0) (void)mint_dir_create(path);
}
int prefs_flush(void) {
    if (!prefs_disk) return 0;
    int rc = 0, wrote_user = 0;
    for (uint32_t i = 0; i < PREFS_DOMAINS; ++i) {
        pref_domain *d = &domains[i];
        if (!d->used || !d->dirty) continue;
        int system = seq(d->name, "system");
        if (!system && !prefs_home[0]) continue;
        if (system) { ensure_dir("/Library"); ensure_dir("/Library/Preferences"); }
        else if (!wrote_user) {
            scopy(path_buf, sizeof(path_buf), prefs_home); sappend(path_buf, sizeof(path_buf), "/Library");
            ensure_dir(path_buf);
            sappend(path_buf, sizeof(path_buf), "/Preferences");
            ensure_dir(path_buf);
            wrote_user = 1;
        }
        int64_t n = prefs_serialize(d->name, file_buf, sizeof(file_buf));
        if (n < 0) { rc = -1; continue; }
        domain_path(d, path_buf, sizeof(path_buf));
        int64_t w = mint_file_save(path_buf, file_buf, (size_t)n);
        if (w < 0) rc = (int)w; else d->dirty = 0;
    }
    prefs_failed = rc < 0;
    return rc;
}
int prefs_background(uint64_t now_ms) {
    if (prefs_changed) { prefs_changed = 0; prefs_last_change_ms = now_ms; return 0; }
    int pending = 0;
    for (uint32_t i = 0; i < PREFS_DOMAINS; ++i) if (domains[i].used && domains[i].dirty) pending = 1;
    if (!pending || !prefs_disk || now_ms - prefs_last_change_ms < PREFS_SAVE_DELAY_MS) return 0;
    if (prefs_flush() < 0) prefs_last_change_ms = now_ms + 4000u;       /* retry in ~5 s */
    return 1;
}
int prefs_pending(void) {
    for (uint32_t i = 0; i < PREFS_DOMAINS; ++i) if (domains[i].used && domains[i].dirty) return 1;
    return 0;
}
int prefs_on_disk(void) { return prefs_disk; }
int prefs_save_failed(void) { return prefs_failed; }
const char *prefs_home_path(void) { return prefs_home; }

/* Reads the volume state and home; domains reload lazily from the new place. */
void prefs_load(void) {
    MINT_MFS_VOLUME v;
    prefs_disk = mint_fs_get_volume(&v) == 0 && v.state == MINT_MFS_VOLUME_READY;
    scopy(prefs_home, sizeof(prefs_home), prefs_disk ? v.home : "");
    for (uint32_t i = 0; i < PREFS_MAX; ++i) entries[i].used = 0;
    for (uint32_t i = 0; i < PREFS_DOMAINS; ++i) domains[i].used = 0;
    prefs_changed = 0; prefs_failed = 0;
}
/* The account's home changed (Setup Assistant): everything known now is
 * written to the new place, including settings chosen before the account
 * existed and the system domain. */
void prefs_set_home(const char *home) {
    MINT_MFS_VOLUME v;
    prefs_disk = mint_fs_get_volume(&v) == 0 && v.state == MINT_MFS_VOLUME_READY;
    scopy(prefs_home, sizeof(prefs_home), prefs_disk ? home : "");
    for (uint32_t i = 0; i < PREFS_DOMAINS; ++i)
        if (domains[i].used) { domains[i].loaded = 1; domains[i].dirty = 1; }
    prefs_changed = 1;
}
void prefs_reset(void) {
    for (uint32_t i = 0; i < PREFS_MAX; ++i) entries[i].used = 0;
    for (uint32_t i = 0; i < PREFS_DOMAINS; ++i) domains[i].used = 0;
    prefs_home[0] = '\0'; prefs_disk = 0; prefs_changed = 0; prefs_failed = 0;
}
