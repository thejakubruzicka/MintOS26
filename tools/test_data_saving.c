/* Host test for MintOS data saving (26.5.014.0).
 *
 * Pure parts: the preferences parser/serializer (kernel/user/mintde/prefs.c),
 * account-name rules and name suggestions (kernel/user/mintde/accounts.c) and
 * the kernel's accounts-file parser (kernel/fs/mintos_disk.c), which must agree
 * with MintDE on every name.
 *
 * Integration: prefs.c and accounts.c run unchanged on top of the real block
 * layer, MintOS-disk module and MintFS on a RAM disk (the user-space file API
 * is mapped onto mint_mfs_*). It covers debounced saving, the system and user
 * preference files, account creation with migration of the default home, the
 * kernel picking the account's home at the next mount, the safe-shutdown flush
 * hook (uncommitted data is lost without it and kept with it), and ends with
 * the MintFS checker.
 * Build: cc -std=c11 -Itools tools/test_data_saving.c kernel/user/mintde/prefs.c
 *        kernel/user/mintde/accounts.c kernel/block/block.c kernel/fs/mintos_disk.c kernel/fs/mintfs.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../kernel/include/block.h"
#include "../kernel/include/mintfs.h"
#include "../kernel/include/mintos_fs.h"
#include "../kernel/include/uapi/mintos/syscall.h"
#include "../kernel/include/log.h"
#include "../kernel/user/mintde/userdata.h"

/* ---- kernel services used by mintos_disk.c */
static uint64_t ticks = 1000;
uint64_t mint_timer_ticks(void) { return ticks += 3; }
int64_t mint_rtc_boot_epoch(void) { return 1794300000; }
void mint_serial_write(const char *s) { if (getenv("MINT_VERBOSE")) fputs(s, stdout); }
void mint_serial_dec64(uint64_t v) { if (getenv("MINT_VERBOSE")) printf("%llu", (unsigned long long)v); }
static int prefs_log_events;
void mint_log_emit(uint32_t severity, uint32_t component, const char *message) {
    (void)severity; (void)message;
    if (component == MINT_LOG_COMPONENT_PREFS) ++prefs_log_events;
}
void mint_task_sleep_ticks(uint64_t t) { ticks += t; }
uint64_t mint_physmem_claim_contiguous_below(uint64_t pages, uint64_t limit) { (void)limit; return (uint64_t)(uintptr_t)calloc((size_t)pages, 4096); }
int mint_physmem_release_contiguous(uint64_t base, uint64_t pages) { (void)pages; free((void *)(uintptr_t)base); return 1; }

/* ---- the user-space runtime file API on top of the kernel functions */
#define PID 7u
int64_t mint_file_open(const char *path, uint32_t flags) { return mint_mfs_open(path, flags, PID); }
int64_t mint_file_load(const char *path, void *out, size_t cap) {
    int64_t fd = mint_mfs_open(path, MINT_O_READ, PID);
    if (fd < 0) return fd;
    size_t got = 0;
    while (got < cap) {
        int64_t r = mint_mfs_read(fd, (uint8_t *)out + got, (uint32_t)(cap - got > 65536u ? 65536u : cap - got), PID);
        if (r < 0) { mint_mfs_close(fd, PID); return r; }
        if (!r) break;
        got += (size_t)r;
    }
    mint_mfs_close(fd, PID);
    return (int64_t)got;
}
static int64_t write_whole(const char *path, const void *data, size_t len) {
    int64_t fd = mint_mfs_open(path, MINT_O_WRITE | MINT_O_CREATE | MINT_O_TRUNC, PID);
    if (fd < 0) return fd;
    int64_t w = len ? mint_mfs_write(fd, data, (uint32_t)len, PID) : 0;
    int64_t c = mint_mfs_close(fd, PID);
    if (w < 0) return w;
    if (c < 0) return c;
    return (size_t)w == len ? (int64_t)len : -MINT_EIO;
}
static int saves;
int64_t mint_file_save(const char *path, const void *data, size_t len) {      /* same as runtime.c */
    char tmp[MINT_MFS_PATH_MAX];
    size_t n = strlen(path), slash = 0;
    for (size_t i = 0; i < n; ++i) if (path[i] == '/') slash = i;
    memcpy(tmp, path, slash + 1); tmp[slash + 1] = '.'; tmp[slash + 2] = '~';
    memcpy(tmp + slash + 3, path + slash + 1, n - slash);
    ++saves;
    int64_t rc = write_whole(tmp, data, len);
    if (rc < 0) { mint_mfs_remove(tmp); return rc; }
    int64_t r = mint_mfs_rename(tmp, path);
    if (r < 0) { mint_mfs_remove(tmp); return r; }
    return rc;
}
int64_t mint_path_stat(const char *path, MINT_MFS_STAT *out) { return mint_mfs_stat(path, out); }
int64_t mint_dir_create(const char *path) { return mint_mfs_mkdir(path); }
int64_t mint_path_rename(const char *a, const char *b) { return mint_mfs_rename(a, b); }
int64_t mint_path_remove(const char *path) { return mint_mfs_remove(path); }
int64_t mint_fs_get_volume(MINT_MFS_VOLUME *out) { mint_mfs_volume(out); return 0; }
int64_t mint_fs_set_home(const char *path) { return mint_mfs_set_home(path); }
int64_t mint_fs_flush(void) { return mint_mfs_sync(); }
int64_t mint_clock_gettime(int clock_id, mint_timespec64 *out) { (void)clock_id; out->tv_sec = 1794300000; out->tv_nsec = 0; return 0; }

/* ---- RAM disk */
typedef struct { uint8_t *data; uint64_t sectors; uint64_t writes; } RAMDISK;
static int rd_read(void *c, uint64_t lba, uint8_t *out) { RAMDISK *d = c; if (lba >= d->sectors) return 0; memcpy(out, d->data + lba * 512, 512); return 1; }
static int rd_write(void *c, uint64_t lba, const uint8_t *in) { RAMDISK *d = c; if (lba >= d->sectors) return 0; d->writes++; memcpy(d->data + lba * 512, in, 512); return 1; }
static int rd_reads(void *c, uint64_t lba, uint32_t n, uint8_t *out) { RAMDISK *d = c; if (lba + n > d->sectors) return 0; memcpy(out, d->data + lba * 512, n * 512u); return 1; }
static int rd_writes(void *c, uint64_t lba, uint32_t n, const uint8_t *in) { RAMDISK *d = c; if (lba + n > d->sectors) return 0; d->writes += n; memcpy(d->data + lba * 512, in, n * 512u); return 1; }
static int rd_flush(void *c) { (void)c; return 1; }
static RAMDISK disk;
static uint64_t part_first;
static int ram_read4k(void *c, uint64_t b, void *buf) { RAMDISK *d = c; memcpy(buf, d->data + (part_first + b * 8) * 512, 4096); return 1; }
static int ram_nowrite(void *c, uint64_t b, const void *buf) { (void)c; (void)b; (void)buf; return 0; }
static int ram_noflush(void *c) { (void)c; return 1; }
static int64_t ram_now(void *c) { (void)c; return 0; }

static int failures, checks;
#define CHECK(cond, msg) do { ++checks; if (cond) printf("PASS: %s\n", msg); else { ++failures; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static void remount(void) {                     /* "restart": mount again from what is on the disk */
    mint_block_revoke_writes(0, 0x4D465331u);
    mint_mintos_disk_probe();
}
static int file_has(const char *path, const char *needle) {
    static char buf[16384];
    int64_t n = mint_file_load(path, buf, sizeof(buf) - 1);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}
static int is_dir(const char *path) { MINT_MFS_STAT st; return mint_mfs_stat(path, &st) == 0 && st.type == MINT_MFS_DIR; }

/* ---------------------------------------------------------------- pure */
typedef struct { int count; char names[16][64]; char values[16][256]; int ints[16]; } collected;
static void collect(void *ctx, const char *name, const char *value, int is_int) {
    collected *c = ctx;
    if (c->count < 16) { snprintf(c->names[c->count], 64, "%s", name); snprintf(c->values[c->count], 256, "%s", value); c->ints[c->count] = is_int; }
    c->count++;
}

static void test_pure(void) {
    CHECK(prefs_key_valid("desktop.accent") && prefs_key_valid("system.network_autoconnect") && prefs_key_valid("web.home.page"),
          "well-formed preference keys are accepted");
    CHECK(!prefs_key_valid("Desktop.accent") && !prefs_key_valid("desktop") && !prefs_key_valid("desktop.") &&
          !prefs_key_valid(".accent") && !prefs_key_valid("desk top.x") && !prefs_key_valid("1desk.x") &&
          !prefs_key_valid("aaaaaaaaaaaaaaaaaaaaaaaaa.x") &&
          !prefs_key_valid("desktop.xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"),
          "malformed keys (case, no domain, empty parts, spaces, long) are refused");

    static const char text[] =
        "# comment\n"
        "  accent = 3\n"
        "theme=-2\r\n"
        "wallpaper = \"Mint \\\"Waves\\\"\\n2\\\\x\"   # trailing comment\n"
        "flag = true\n"
        "off = false\n"
        "\n"
        "broken line without equals\n"
        "Upper = 1\n"
        "quote = \"never closed\n"
        "junk = 12 34\n"
        "word = maybe\n"
        "big = 1234567890123456789012\n"
        "last = \"ok\"";
    collected c; memset(&c, 0, sizeof(c));
    int n = prefs_parse_text(text, sizeof(text) - 1, collect, &c);
    CHECK(n == 6 && c.count == 6, "the parser keeps the six valid lines and skips seven malformed ones");
    CHECK(!strcmp(c.names[0], "accent") && !strcmp(c.values[0], "3") && c.ints[0] &&
          !strcmp(c.names[1], "theme") && !strcmp(c.values[1], "-2") &&
          !strcmp(c.values[2], "Mint \"Waves\"\n2\\x") && !c.ints[2] &&
          !strcmp(c.values[3], "1") && !strcmp(c.values[4], "0") && !strcmp(c.values[5], "ok"),
          "integers, true/false, CRLF and escaped strings parse to the right values");

    prefs_reset();
    CHECK(prefs_get_int("desktop.accent", 9) == 9 && !strcmp(prefs_get_str("desktop.name", "fallback"), "fallback"),
          "missing keys return the caller's fallback");
    CHECK(prefs_set_int("desktop.accent", 2) && prefs_set_int("desktop.theme", -1) && prefs_set_str("desktop.label", "Tab\there \"q\" \\ end") &&
          prefs_set_int("desktop.big", -900000000000000000LL) && prefs_set_str("files.last_folder", "/Users/mintos/Documents"),
          "values of several domains are stored");
    CHECK(prefs_get_int("desktop.accent", 0) == 2 && prefs_get_int("desktop.theme", 0) == -1 &&
          prefs_get_int("desktop.big", 0) == -900000000000000000LL && prefs_get_int("desktop.label", 77) == 77,
          "integers read back exactly; a text value is not an integer");
    static char out[4096];
    int64_t len = prefs_serialize("desktop", out, sizeof(out));
    CHECK(len > 0 && strstr(out, "# com.mintos.desktop") && strstr(out, "accent = 2\n") &&
          strstr(out, "label = \"Tab\\there \\\"q\\\" \\\\ end\"\n") && !strstr(out, "last_folder"),
          "a domain serializes with its header, quoted text and only its own keys");
    CHECK(strstr(out, "accent") < strstr(out, "big") && strstr(out, "big") < strstr(out, "label") && strstr(out, "label") < strstr(out, "theme"),
          "keys are written in alphabetical order");
    prefs_reset();
    CHECK(prefs_parse("desktop", out, (size_t)len) == 4 && !strcmp(prefs_get_str("desktop.label", ""), "Tab\there \"q\" \\ end") &&
          prefs_get_int("desktop.big", 0) == -900000000000000000LL, "serialize and parse round-trip every value");
    CHECK(prefs_serialize("desktop", out, 60) == -1, "serializing into a too-small buffer fails instead of truncating");
    char longv[300]; memset(longv, 'x', sizeof(longv)); longv[255] = '\0';
    CHECK(prefs_set_str("desktop.long", longv), "a 255-byte value is stored");
    longv[255] = 'x'; longv[256] = '\0';
    CHECK(!prefs_set_str("desktop.long2", longv) && !prefs_set_str("Bad.key", "x"), "a 256-byte value or a bad key is refused");
    char q[32] = "";
    prefs_quote(q, sizeof(q), "a\"b\x01" "c");
    CHECK(!strcmp(q, "\"a\\\"bc\""), "quoting escapes quotes and drops control bytes");

    /* account names: MintDE and the kernel agree */
    static const char *const good[] = {"jakub", "a", "mintos", "j.r-u_2", "abcdefghijklmnopqrstuvwxyz01234"};
    static const char *const bad[] = {"", "Jakub", "1abc", "_x", "-x", ".x", "a b", "a/b", "root", "shared", "guest",
                                      "abcdefghijklmnopqrstuvwxyz012345", ("jak\xC3\xBA" "b")};
    int agree = 1, ok = 1;
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        ok &= account_name_valid(good[i]);
        agree &= account_name_valid(good[i]) == mint_account_name_valid(good[i]);
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        ok &= !account_name_valid(bad[i]);
        agree &= account_name_valid(bad[i]) == mint_account_name_valid(bad[i]);
    }
    CHECK(ok, "account names: lowercase start, a-z 0-9 . - _, at most 31 bytes, reserved names refused");
    CHECK(agree, "the kernel and MintDE apply the same account-name rules");
    CHECK(account_name_problem("Jakub") && account_name_problem("") && account_name_problem("root") && !account_name_problem("jakub"),
          "every refused account name comes with an explanation");

    char s[40];
    struct { const char *full, *want; } sug[] = {
        {"Jakub R\xC5\xAF\xC5\xBEi\xC4\x8Dka", "jakubruzicka"}, {"  Anna-Marie O'Neil ", "annamarieoneil"},
        {"Stra\xC3\x9F" "e", "strasse"}, {"123 Go", "go"}, {"", "user"}, {"Root", "root1"},
        {"\xC3\x9Cnal \xC3\x87" "elik", "unalcelik"}, {"Bartholomew Montgomery-Smithson III", "bartholomewmontgomerysmithsonii"},
    };
    int sug_ok = 1;
    for (size_t i = 0; i < sizeof(sug) / sizeof(sug[0]); ++i) {
        account_suggest_name(sug[i].full, s, sizeof(s));
        if (strcmp(s, sug[i].want) || !account_name_valid(s)) { sug_ok = 0; printf("  suggest(\"%s\") = \"%s\"\n", sug[i].full, s); }
    }
    CHECK(sug_ok, "account names are suggested from full names (accents removed, ß -> ss, valid, at most 31 bytes)");
    char ini[16];
    account_initials("Jakub Ruzicka", ini, sizeof(ini)); int i_ok = !strcmp(ini, "JR");
    account_initials("anna", ini, sizeof(ini)); i_ok &= !strcmp(ini, "A");
    account_initials("\xC3\x89lise  Durand Smith", ini, sizeof(ini)); i_ok &= !strcmp(ini, "\xC3\x89" "D");
    account_initials("", ini, sizeof(ini)); i_ok &= !strcmp(ini, "M");
    CHECK(i_ok, "avatar initials take the first letters of the first two words");
    CHECK(account_full_name_valid("Jakub R\xC5\xAF\xC5\xBEi\xC4\x8Dka") && !account_full_name_valid("") && !account_full_name_valid("   ") &&
          !account_full_name_valid("Say \"hi\"") && !account_full_name_valid("bad\xC3"), "full names: UTF-8 text, not empty, no quotes");

    mde_account a; memset(&a, 0, sizeof(a));
    strcpy(a.account, "jakub"); strcpy(a.full_name, "Jakub R\xC5\xAF\xC5\xBEi\xC4\x8Dka"); a.created = 1794300000;
    char file[1024];
    int flen = account_serialize(&a, file, sizeof(file));
    mde_account b;
    char home[64];
    CHECK(flen > 0 && account_parse(file, (size_t)flen, &b) && !strcmp(b.account, "jakub") && !strcmp(b.full_name, a.full_name) &&
          !strcmp(b.home, "/Users/jakub") && b.created == 1794300000, "the accounts file round-trips");
    CHECK(mint_account_home_from_text(file, (size_t)flen, home, sizeof(home)) && !strcmp(home, "/Users/jakub"),
          "the kernel finds the home line MintDE writes");
    static const char *const tampered[] = {"home = \"/Users/root\"\n", "home = \"/Users/../etc\"\n", "home = \"/Library\"\n",
                                           "home = /Users/jakub\n", "home = \"/Users/jakub\n", "homes = \"/Users/jakub\"\n",
                                           "home = \"/Users/Jakub\"\n", "home = \"/Users/jakub/Documents\"\n"};
    int refused = 1;
    for (size_t i = 0; i < sizeof(tampered) / sizeof(tampered[0]); ++i)
        refused &= !mint_account_home_from_text(tampered[i], strlen(tampered[i]), home, sizeof(home));
    CHECK(refused, "the kernel refuses reserved, relative, non-/Users, unquoted and nested homes");
    CHECK(!mint_account_home_from_text("home = \"/Users/jakub\"\n", 21, home, 10), "the kernel never overflows the home buffer");
}

/* ---------------------------------------------------------------- on a MintOS disk */
static void test_disk(void) {
    mint_block_init();
    disk.sectors = 262144; disk.data = calloc(disk.sectors, 512);
    MINT_BLOCK_DEVICE dev; memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "sda");
    dev.sector_count = disk.sectors; dev.sector_size = 512; dev.flags = MINT_BLOCK_FLAG_READONLY; dev.context = &disk;
    dev.read_sector = rd_read; dev.write_sector = rd_write; dev.flush = rd_flush; dev.read_sectors = rd_reads; dev.write_sectors = rd_writes;
    mint_block_register(&dev);
    mint_mintos_disk_probe();

    /* no MintOS disk: session-only */
    prefs_load();
    CHECK(!prefs_on_disk() && prefs_set_int("desktop.accent", 1) && prefs_get_int("desktop.accent", 0) == 1 &&
          prefs_flush() == 0 && prefs_background(100000) == 0 && disk.writes == 0,
          "without a MintOS disk preferences work in memory and nothing is written");
    memset(&account, 0, sizeof(account));
    int migrated = -1;
    CHECK(account_create("Session User", "session", &migrated) == 0 && account.present && !account.on_disk && !account.home[0] &&
          disk.writes == 0, "without a MintOS disk the account exists for the session only");
    memset(&account, 0, sizeof(account));

    CHECK(mint_mintos_disk_initialize(0) == 0, "a blank disk is initialized as MintOS HD");
    MINT_MFS_VOLUME v;
    mint_mfs_volume(&v);
    uint64_t last;
    mint_gpt_find_mintos(disk.data + 512, disk.data + 1024, disk.sectors, &part_first, &last);
    CHECK(!strcmp(v.home, "/Users/mintos") && account_load() == 0, "a new disk has the default home and no account");

    prefs_load();
    saves = 0;
    CHECK(prefs_on_disk() && !strcmp(prefs_home_path(), "/Users/mintos"), "preferences follow MintOS HD and its home");
    prefs_set_int("desktop.accent", 3); prefs_set_int("desktop.theme", 1); prefs_set_str("files.last_folder", "/Users/mintos/Documents");
    prefs_set_int("system.network_autoconnect", 0);
    CHECK(prefs_background(5000) == 0 && saves == 0 && prefs_background(5500) == 0 && saves == 0,
          "a change is not written immediately (debounce)");
    CHECK(prefs_background(6100) == 1 && saves == 3 && !prefs_pending(), "about one second after the last change every changed domain is saved once");
    CHECK(file_has("/Users/mintos/Library/Preferences/com.mintos.desktop.prefs", "accent = 3\n") &&
          file_has("/Users/mintos/Library/Preferences/com.mintos.files.prefs", "last_folder = \"/Users/mintos/Documents\"") &&
          file_has("/Library/Preferences/com.mintos.system.prefs", "network_autoconnect = 0"),
          "user domains go to ~/Library/Preferences and system keys to /Library/Preferences");
    prefs_set_int("desktop.accent", 3);
    CHECK(!prefs_pending(), "setting an unchanged value does not schedule a save");

    /* something saved before setup, in the default home */
    CHECK(mint_file_save("/Users/mintos/Documents/before setup.txt", "kept", 4) == 4, "a document is saved before the account exists");
    migrated = 0;
    CHECK(account_create("Jakub R\xC5\xAF\xC5\xBEi\xC4\x8Dka", "jakub", &migrated) == 0 && migrated == 1, "the Setup Assistant creates the account");
    mint_mfs_volume(&v);
    CHECK(!strcmp(v.home, "/Users/jakub") && !strcmp(account.home, "/Users/jakub") && account.on_disk,
          "the kernel's home is the account's folder at once");
    CHECK(file_has("/Users/jakub/Documents/before setup.txt", "kept") && !is_dir("/Users/mintos"),
          "the default home was moved, documents included, not copied");
    int layout = 1;
    static const char *const sub[] = {"Desktop", "Documents", "Downloads", "Movies", "Music", "Pictures", "Public", "Library", "Library/Preferences", ".Trash"};
    for (size_t i = 0; i < sizeof(sub) / sizeof(sub[0]); ++i) { char p[128]; snprintf(p, sizeof(p), "/Users/jakub/%s", sub[i]); layout &= is_dir(p); }
    CHECK(layout && is_dir("/Users/Shared"), "the home has the macOS folder layout");
    CHECK(file_has(MINT_ACCOUNTS_PATH, "home = \"/Users/jakub\"") && file_has(MINT_ACCOUNTS_PATH, "full_name = \"Jakub R\xC5\xAF\xC5\xBEi\xC4\x8Dka\""),
          "the accounts file is in /Library/Preferences");
    prefs_set_home("/Users/jakub");
    prefs_set_int("desktop.wallpaper", 2);
    CHECK(prefs_flush() == 0 && file_has("/Users/jakub/Library/Preferences/com.mintos.desktop.prefs", "wallpaper = 2") &&
          file_has("/Users/jakub/Library/Preferences/com.mintos.desktop.prefs", "accent = 3"),
          "after setup the preferences are written in the account's home");

    /* restart */
    remount();
    mint_mfs_volume(&v);
    CHECK(v.state == MINT_MFS_VOLUME_READY && !strcmp(v.home, "/Users/jakub"), "after a restart the kernel mounts with the account's home");
    memset(&account, 0, sizeof(account));
    prefs_load();
    CHECK(account_load() == 1 && !strcmp(account.account, "jakub") && !strcmp(account_display_name(), "Jakub R\xC5\xAF\xC5\xBEi\xC4\x8Dka"),
          "the account is loaded again");
    CHECK(prefs_get_int("desktop.accent", 0) == 3 && prefs_get_int("desktop.theme", 0) == 1 && prefs_get_int("desktop.wallpaper", 0) == 2 &&
          prefs_get_int("system.network_autoconnect", 1) == 0 && !strcmp(prefs_get_str("files.last_folder", ""), "/Users/mintos/Documents"),
          "every preference is restored after the restart");

    /* the set-home call validates */
    CHECK(mint_mfs_set_home("/Users/nobodyhere") == -MINT_ENOENT && mint_mfs_set_home("/Library") == -MINT_EINVAL &&
          mint_mfs_set_home("/Users/root") == -MINT_EINVAL && mint_mfs_set_home("/Users/jakub/Documents") == -MINT_EINVAL,
          "the kernel only accepts an existing /Users/<valid account> folder as home");

    /* rerun setup with a new account name: the folder is renamed */
    CHECK(account_create("Jakub R", "jakubr", &migrated) == 0 && migrated == 1 && is_dir("/Users/jakubr") && !is_dir("/Users/jakub") &&
          file_has("/Users/jakubr/Documents/before setup.txt", "kept"), "renaming the account renames its home folder");

    /* a damaged accounts file falls back to the default home */
    mint_file_save(MINT_ACCOUNTS_PATH, "home = \"/Users/root\"\n", 21);
    int events = prefs_log_events;
    remount();
    mint_mfs_volume(&v);
    CHECK(!strcmp(v.home, "/Users/mintos") && prefs_log_events > events, "an invalid accounts file is ignored (logged) and the default home is kept");

    /* safe shutdown: uncommitted data is lost without the flush and kept with it */
    int64_t fd = mint_mfs_open("/Users/Shared/unsaved.txt", MINT_O_WRITE | MINT_O_CREATE, PID);
    mint_mfs_write(fd, "lost", 4, PID);
    remount();                                         /* power cut before any commit */
    MINT_MFS_STAT st;
    int lost = mint_mfs_stat("/Users/Shared/unsaved.txt", &st) != 0 || st.size == 0;
    fd = mint_mfs_open("/Users/Shared/flushed.txt", MINT_O_WRITE | MINT_O_CREATE, PID);
    mint_mfs_write(fd, "kept!", 5, PID);
    CHECK(mint_mfs_flush_for_power() == 0, "the power-off flush hook commits");
    remount();
    CHECK(lost && mint_mfs_stat("/Users/Shared/flushed.txt", &st) == 0 && st.size == 5,
          "data written without close survives power-off only because of the flush hook");

    MINTFS_DEV fdev = {&disk, (last - part_first + 1) / 8, ram_read4k, ram_nowrite, ram_noflush, ram_now};
    uint64_t wb = mintfs_check_work_bytes(fdev.block_count);
    uint8_t *work = malloc((size_t)wb);
    MINTFS_CHECK_REPORT rep;
    int rc = mintfs_check(&fdev, &rep, work, (size_t)wb);
    CHECK(rc == 0 && rep.leaked_blocks == 0 && rep.bad_checksums == 0, "the volume passes the MintFS checker afterwards");
    if (rc) printf("  fsck: %s\n", rep.first_problem);
}

int main(void) {
    test_pure();
    test_disk();
    printf("%d checks, %d failures\n", checks, failures);
    if (!failures) printf("PASS: data saving (preferences, accounts, safe shutdown)\n");
    return failures ? 1 : 0;
}
