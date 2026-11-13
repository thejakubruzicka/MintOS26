#ifndef MINTDE_USERDATA_H
#define MINTDE_USERDATA_H
/* MintDE data saving (26.5.014.0): preferences (prefs.c) and the computer
 * account (accounts.c). Plain C with no drawing, so tools/test_data_saving.c
 * compiles both on the host against the real MintFS. See docs/DATA-SAVING.md. */
#include <stdint.h>
#include <stddef.h>

/* ---- preferences (prefs.c) — key "<domain>.<name>", see the file comment */
int64_t prefs_get_int(const char *key, int64_t fallback);
const char *prefs_get_str(const char *key, const char *fallback);
int prefs_set_int(const char *key, int64_t value);       /* 1 if stored */
int prefs_set_str(const char *key, const char *value);   /* 1 if stored (values < 256 bytes) */
int prefs_remove(const char *key);
void prefs_load(void);                     /* start of session: volume + home from the kernel */
void prefs_set_home(const char *home);     /* account home changed: rewrite everything there */
int prefs_flush(void);                     /* save changed domains now (0, or <0 on error) */
int prefs_background(uint64_t now_ms);     /* debounced save; 1 if it wrote */
int prefs_pending(void);
int prefs_on_disk(void);
int prefs_save_failed(void);
const char *prefs_home_path(void);
/* Pure helpers (host-tested). */
typedef void (*prefs_line_fn)(void *ctx, const char *name, const char *value, int is_int);
int prefs_key_valid(const char *key);
int prefs_parse_text(const char *text, size_t len, prefs_line_fn fn, void *ctx);
int prefs_parse(const char *domain, const char *text, size_t len);
int64_t prefs_serialize(const char *domain, char *out, size_t cap);
size_t prefs_quote(char *out, size_t cap, const char *value);   /* appends "value" with escapes */
void prefs_reset(void);

/* ---- the computer account (accounts.c) */
#define ACCOUNT_FULL_NAME_MAX 64u
typedef struct mde_account {
    int present;                 /* an account exists (on disk, or for this session) */
    int on_disk;                 /* stored in /Library/Preferences/com.mintos.accounts */
    char account[32];            /* "jakub" (home folder name) */
    char full_name[ACCOUNT_FULL_NAME_MAX];
    char home[64];               /* "/Users/jakub", empty without MintOS HD */
    int64_t created;
} mde_account;
extern mde_account account;

int account_name_valid(const char *name);              /* same rules as the kernel */
const char *account_name_problem(const char *name);    /* why a name is refused, or 0 */
int account_full_name_valid(const char *full);
const char *account_full_name_problem(const char *full);
void account_suggest_name(const char *full, char *out, size_t cap);   /* "Jakub Růžička" -> "jakubruzicka" */
void account_initials(const char *full, char *out, size_t cap);       /* "JR" */
const char *account_short_name(void);                  /* account name, or "mintos" */
const char *account_display_name(void);                /* full name, or "MintOS" */
int account_load(void);                                /* 1 if an account exists */
/* Creates (or renames) the account: home folder layout, migration of the
 * previous home, the accounts file and the kernel's home. 0 or -MINT_E*. */
int64_t account_create(const char *full_name, const char *name, int *migrated);
int account_serialize(const mde_account *a, char *out, size_t cap);
int account_parse(const char *text, size_t len, mde_account *out);
#endif
