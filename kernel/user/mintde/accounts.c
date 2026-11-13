/* The MintOS computer account (one account per MintOS HD for now).
 *
 * /Library/Preferences/com.mintos.accounts (same "name = value" format as
 * the preferences) holds account, full_name, home and created. The kernel
 * reads its home line when it mounts MintOS HD, so the account's folder is
 * the home everywhere from the first frame; MINT_SYS_MFS_SET_HOME switches
 * it right after the Setup Assistant creates the account.
 *
 * Account names follow the kernel's rules (mint_account_name_valid): 1-31
 * bytes of a-z 0-9 _ - . starting with a letter, not root/daemon/nobody/
 * guest/shared/system/wheel. Creating an account makes /Users/<account> with
 * the macOS folder layout; the previous home (the default /Users/mintos that
 * MintOS 26.5.013.0 created, or the account's old folder when the Setup
 * Assistant renames it) is renamed rather than copied, so nothing saved
 * before setup is lost. */
#ifdef MINT_PREFS_HOST_TEST
#include "prefs_host.h"
#else
#include "mintde.h"
#endif

mde_account account;
static char acc_buf[2048];
static char acc_path[MINT_MFS_PATH_MAX];

static size_t alen(const char *s) { size_t n = 0; while (s && s[n]) ++n; return n; }
static int aeq(const char *a, const char *b) { size_t i = 0; while (a[i] && a[i] == b[i]) ++i; return a[i] == b[i]; }
static void acopy(char *out, size_t cap, const char *in) {
    size_t i = 0;
    if (!cap) return;
    for (; in && in[i] && i + 1u < cap; ++i) out[i] = in[i];
    out[i] = '\0';
}
static void aappend(char *out, size_t cap, const char *in) { size_t n = alen(out); if (n < cap) acopy(out + n, cap - n, in); }

/* ------------------------------------------------------------ names (pure) */
static const char *const reserved_names[] = {"root", "daemon", "nobody", "guest", "shared", "system", "wheel"};
const char *account_name_problem(const char *name) {
    size_t n = alen(name);
    if (!n) return "Enter an account name.";
    if (n > MINT_ACCOUNT_NAME_MAX) return "The account name can have at most 31 characters.";
    if (!(name[0] >= 'a' && name[0] <= 'z')) return "The account name must start with a lowercase letter.";
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return "Use only lowercase letters, numbers, \xE2\x80\x9C.\xE2\x80\x9D, \xE2\x80\x9C-\xE2\x80\x9D and \xE2\x80\x9C_\xE2\x80\x9D.";
    }
    for (size_t r = 0; r < sizeof(reserved_names) / sizeof(reserved_names[0]); ++r)
        if (aeq(name, reserved_names[r])) return "That name is reserved by MintOS.";
    return 0;
}
int account_name_valid(const char *name) { return name && !account_name_problem(name); }

static uint32_t utf8_decode(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = s[0];
    if (c < 0x80u) { *p += 1; return c; }
    if ((c & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) { *p += 2; return ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu); }
    if ((c & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        *p += 3; return ((c & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    }
    if ((c & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u) { *p += 4; return 0xFFFDu; }
    *p += 1;
    return 0xFFFDu;
}
const char *account_full_name_problem(const char *full) {
    size_t n = alen(full);
    if (!n) return "Enter your full name.";
    if (n >= ACCOUNT_FULL_NAME_MAX) return "The full name is too long.";
    int letters = 0;
    const char *p = full;
    while (*p) {
        uint32_t c = utf8_decode(&p);
        if (c == 0xFFFDu) return "The full name contains an unsupported character.";
        if (c < 0x20u || c == 0x7Fu || c == '"' || c == '\\') return "The full name can\xE2\x80\x99t contain quotes, backslashes or control characters.";
        if (c != ' ') letters = 1;
    }
    return letters ? 0 : "Enter your full name.";
}
int account_full_name_valid(const char *full) { return full && !account_full_name_problem(full); }

/* Latin-1 and Latin Extended-A letters without their accents ('?' = drop). */
static const char latin1[64 + 1] = "aaaaaaaceeeeiiiidnooooo?ouuuuyts" "aaaaaaaceeeeiiiidnooooo?ouuuuyty";
static const char latin_ext[128 + 1] =
    "aaaaaa" "cccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh" "iiiiiiiiii" "ii" "jj" "kkk" "llllllllll"
    "nnnnnnnnn" "oooooo" "oo" "rrrrrr" "ssssssss" "tttttt" "uuuuuuuuuuuu" "ww" "yyy" "zzzzzz" "s";
void account_suggest_name(const char *full, char *out, size_t cap) {
    size_t o = 0, limit = cap ? cap - 1u : 0u;
    if (limit > MINT_ACCOUNT_NAME_MAX) limit = MINT_ACCOUNT_NAME_MAX;
    const char *p = full ? full : "";
    while (*p && o < limit) {
        uint32_t c = utf8_decode(&p);
        char m = 0;
        if (c >= 'A' && c <= 'Z') m = (char)(c + 32u);
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) m = (char)c;
        else if (c >= 0xC0u && c <= 0xFFu) m = latin1[c - 0xC0u];
        else if (c >= 0x100u && c <= 0x17Fu) m = latin_ext[c - 0x100u];
        if (!m || m == '?') continue;
        if (!o && !(m >= 'a' && m <= 'z')) continue;          /* must start with a letter */
        out[o++] = m;
        if (c == 0xDFu && o < limit) out[o++] = 's';           /* ß -> ss */
    }
    if (cap) out[o] = '\0';
    if (!o && cap > 4u) acopy(out, cap, "user");
    if (!account_name_valid(out) && alen(out) + 1u < cap && alen(out) < MINT_ACCOUNT_NAME_MAX) aappend(out, cap, "1");
}
void account_initials(const char *full, char *out, size_t cap) {
    size_t o = 0;
    int start = 1, count = 0;
    const char *p = full ? full : "";
    while (*p && count < 2 && o + 1u < cap) {
        const char *at = p;
        uint32_t c = utf8_decode(&p);
        if (c == ' ') { start = 1; continue; }
        if (start) {
            start = 0;
            if (c >= 'a' && c <= 'z') { out[o++] = (char)(c - 32u); ++count; }
            else if (c < 0x80u) { out[o++] = (char)c; ++count; }
            else {                                             /* keep the UTF-8 letter as it is */
                size_t n = (size_t)(p - at);
                if (o + n + 1u > cap) break;
                for (size_t i = 0; i < n; ++i) out[o++] = at[i];
                ++count;
            }
        }
    }
    if (cap) out[o] = '\0';
    if (!o && cap > 1u) { out[0] = 'M'; out[1] = '\0'; }
}
const char *account_short_name(void) { return account.present && account.account[0] ? account.account : MINT_MFS_HOME_NAME; }
const char *account_display_name(void) { return account.present && account.full_name[0] ? account.full_name : "MintOS"; }

/* ------------------------------------------------------------ the accounts file (pure) */
int account_serialize(const mde_account *a, char *out, size_t cap) {
    if (!account_name_valid(a->account) || !account_full_name_valid(a->full_name) || cap < 64u) return -1;
    out[0] = '\0';
    aappend(out, cap, "# MintOS accounts \xE2\x80\x94 written by the Setup Assistant.\n# The kernel reads \"home\" when it mounts MintOS HD.\nversion = 1\naccount = ");
    (void)prefs_quote(out, cap, a->account);
    aappend(out, cap, "\nfull_name = ");
    (void)prefs_quote(out, cap, a->full_name);
    aappend(out, cap, "\nhome = \"/Users/");
    aappend(out, cap, a->account);
    aappend(out, cap, "\"\ncreated = ");
    char num[24]; size_t n = 0; uint64_t v = a->created > 0 ? (uint64_t)a->created : 0u;
    do { num[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < sizeof(num));
    char rev[24]; for (size_t i = 0; i < n; ++i) rev[i] = num[n - 1u - i]; rev[n] = '\0';
    aappend(out, cap, rev);
    aappend(out, cap, "\n");
    size_t len = alen(out);
    return len + 1u < cap ? (int)len : -1;
}
static void account_line(void *ctx, const char *name, const char *value, int is_int) {
    mde_account *a = (mde_account *)ctx;
    if (aeq(name, "account") && !is_int) acopy(a->account, sizeof(a->account), alen(value) < sizeof(a->account) ? value : "");
    else if (aeq(name, "full_name") && !is_int) acopy(a->full_name, sizeof(a->full_name), value);
    else if (aeq(name, "created") && is_int) {
        int64_t v = 0;
        for (size_t i = 0; value[i] >= '0' && value[i] <= '9'; ++i) v = v * 10 + (value[i] - '0');
        a->created = v;
    }
}
int account_parse(const char *text, size_t len, mde_account *out) {
    mde_account a;
    for (size_t i = 0; i < sizeof(a); ++i) ((uint8_t *)&a)[i] = 0;
    (void)prefs_parse_text(text, len, account_line, &a);
    if (!account_name_valid(a.account)) return 0;
    if (!account_full_name_valid(a.full_name)) acopy(a.full_name, sizeof(a.full_name), a.account);
    acopy(a.home, sizeof(a.home), "/Users/");
    aappend(a.home, sizeof(a.home), a.account);
    a.present = 1; a.on_disk = 1;
    *out = a;
    return 1;
}

/* ------------------------------------------------------------ disk */
static int is_dir(const char *path) { MINT_MFS_STAT st; return mint_path_stat(path, &st) == 0 && st.type == MINT_MFS_DIR; }
static int exists(const char *path) { MINT_MFS_STAT st; return mint_path_stat(path, &st) == 0; }
static int64_t make_dir(const char *path) {
    if (is_dir(path)) return 0;
    int64_t rc = mint_dir_create(path);
    return rc == -MINT_EEXIST ? 0 : rc;
}

int account_load(void) {
    MINT_MFS_VOLUME v;
    if (mint_fs_get_volume(&v) != 0 || v.state != MINT_MFS_VOLUME_READY) return account.present;   /* session account, if any */
    int64_t n = mint_file_load(MINT_ACCOUNTS_PATH, acc_buf, sizeof(acc_buf) - 1u);
    mde_account a;
    if (n > 0 && account_parse(acc_buf, (size_t)n, &a) && is_dir(a.home)) {
        account = a;
        if (!aeq(v.home, a.home)) (void)mint_fs_set_home(a.home);       /* kernel kept the default */
        return 1;
    }
    for (size_t i = 0; i < sizeof(account); ++i) ((uint8_t *)&account)[i] = 0;
    return 0;
}

int64_t account_create(const char *full_name, const char *name, int *migrated) {
    static const char *const layout[] = {"Desktop", "Documents", "Downloads", "Movies", "Music", "Pictures",
                                         "Public", "Library", "Library/Preferences", ".Trash"};
    if (migrated) *migrated = 0;
    if (!account_full_name_valid(full_name) || !account_name_valid(name)) return -MINT_EINVAL;
    mde_account a;
    for (size_t i = 0; i < sizeof(a); ++i) ((uint8_t *)&a)[i] = 0;
    acopy(a.account, sizeof(a.account), name);
    acopy(a.full_name, sizeof(a.full_name), full_name);
    a.created = account.present && account.created ? account.created : 0;
    a.present = 1;
    MINT_MFS_VOLUME v;
    if (mint_fs_get_volume(&v) != 0 || v.state != MINT_MFS_VOLUME_READY) {
        account = a;                                  /* session-only account */
        return 0;
    }
    mint_timespec64 ts = {0, 0};
    if (!a.created && mint_clock_gettime(MINT_CLOCK_REALTIME, &ts) == 0) a.created = ts.tv_sec;
    acopy(a.home, sizeof(a.home), "/Users/");
    aappend(a.home, sizeof(a.home), name);
    int64_t rc;
    if ((rc = make_dir("/Users")) < 0) return rc;
    /* Move the previous home (default /Users/mintos or the old account
       folder) when the new folder does not exist yet. */
    if (!aeq(v.home, a.home) && is_dir(v.home) && !exists(a.home)) {
        rc = mint_path_rename(v.home, a.home);
        if (rc < 0) return rc;
        if (migrated) *migrated = 1;
    }
    if ((rc = make_dir(a.home)) < 0) return rc;
    for (size_t i = 0; i < sizeof(layout) / sizeof(layout[0]); ++i) {
        acopy(acc_path, sizeof(acc_path), a.home);
        aappend(acc_path, sizeof(acc_path), "/");
        aappend(acc_path, sizeof(acc_path), layout[i]);
        if ((rc = make_dir(acc_path)) < 0) return rc;
    }
    if ((rc = make_dir("/Library")) < 0 || (rc = make_dir("/Library/Preferences")) < 0) return rc;
    int len = account_serialize(&a, acc_buf, sizeof(acc_buf));
    if (len < 0) return -MINT_EINVAL;
    if ((rc = mint_file_save(MINT_ACCOUNTS_PATH, acc_buf, (size_t)len)) < 0) return rc;
    if ((rc = mint_fs_set_home(a.home)) < 0) return rc;
    a.on_disk = 1;
    account = a;
    (void)mint_fs_flush();
    return 0;
}
