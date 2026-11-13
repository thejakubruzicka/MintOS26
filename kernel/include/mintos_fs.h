#ifndef MINTOS26_MINTOS_FS_H
#define MINTOS26_MINTOS_FS_H
/* File API for MintOS disks (MintFS on a GPT partition that MintOS created).
 * Paths are absolute and UTF-8 ("/Users/mintos/Documents/Notes.txt"); "/" is
 * the MintOS HD volume. All calls return negative MINT_E* values on error. */
#include <stdint.h>
#include <stddef.h>

#define MINT_MFS_ABI_VERSION 1u
#define MINT_MFS_PATH_MAX 1024u
#define MINT_MFS_NAME_MAX 255u
#define MINT_MFS_LIST_MAX 32u       /* entries per list call */
#define MINT_MFS_IO_MAX 65536u      /* bytes per read/write call */

/* open flags */
#define MINT_O_READ   0x01u
#define MINT_O_WRITE  0x02u
#define MINT_O_CREATE 0x04u
#define MINT_O_TRUNC  0x08u
#define MINT_O_APPEND 0x10u
#define MINT_O_EXCL   0x20u
enum { MINT_SEEK_SET = 0, MINT_SEEK_CUR = 1, MINT_SEEK_END = 2 };
enum { MINT_MFS_FILE = 1, MINT_MFS_DIR = 2 };

typedef struct mint_mfs_stat {
    uint64_t ino, size;
    uint32_t type, mode;
    int64_t btime, mtime, ctime;
} MINT_MFS_STAT;

typedef struct mint_mfs_dirent {
    uint32_t type, reserved;
    uint64_t size;
    int64_t mtime;
    char name[MINT_MFS_NAME_MAX + 1];
} MINT_MFS_DIRENT;

enum { MINT_MFS_VOLUME_NONE = 0, MINT_MFS_VOLUME_READY = 1, MINT_MFS_VOLUME_READ_ONLY = 2, MINT_MFS_VOLUME_FAILED = 3 };
typedef struct mint_mfs_volume {
    uint32_t version, state, block_index, reserved;
    uint64_t total_bytes, free_bytes, generation, items;
    char label[32];
    char device[16];
    char home[64];          /* e.g. "/Users/mintos" */
    char status[64];
} MINT_MFS_VOLUME;

#define MINT_MFS_HOME_NAME "mintos"          /* default home before an account exists */

/* Accounts (26.5.014.0): the Setup Assistant writes the account to this text
 * file ("key = value" lines, see docs/DATA-SAVING.md). At mount the kernel
 * reads its `home = "/Users/<account>"` line and, if that folder exists, makes
 * it the volume's home; MINT_SYS_MFS_SET_HOME changes it at run time. */
#define MINT_ACCOUNTS_PATH "/Library/Preferences/com.mintos.accounts"
#define MINT_ACCOUNT_NAME_MAX 31u

/* Pure helpers (host-tested, shared rules with MintDE's accounts.c):
 * account names are 1-31 bytes of a-z 0-9 _ - . starting with a letter,
 * and not a reserved name (root, daemon, nobody, guest, shared, system, wheel). */
int mint_account_name_valid(const char *name);
/* Finds `home = "/Users/<valid name>"` in an accounts file; 1 on success. */
int mint_account_home_from_text(const char *text, size_t len, char *home, size_t cap);

#ifndef MINT_MFS_USERSPACE
/* kernel side */
void mint_mintos_disk_probe(void);                 /* mount the first MintOS disk, if any */
int mint_mintos_disk_initialize(uint32_t block_index);   /* blank disk -> GPT + MintFS + layout */
void mint_mfs_task(void *argument);                 /* periodic commits */
int64_t mint_mfs_open(const char *path, uint32_t flags, uint64_t pid);
int64_t mint_mfs_read(int64_t fd, void *buf, uint32_t len, uint64_t pid);
int64_t mint_mfs_write(int64_t fd, const void *buf, uint32_t len, uint64_t pid);
int64_t mint_mfs_seek(int64_t fd, int64_t offset, uint32_t whence, uint64_t pid);
int64_t mint_mfs_close(int64_t fd, uint64_t pid);
int64_t mint_mfs_stat(const char *path, MINT_MFS_STAT *out);
int64_t mint_mfs_list(const char *path, uint32_t start, MINT_MFS_DIRENT *out, uint32_t capacity);   /* count */
int64_t mint_mfs_mkdir(const char *path);
int64_t mint_mfs_rename(const char *from, const char *to);
int64_t mint_mfs_remove(const char *path);
int64_t mint_mfs_sync(void);
int mint_mfs_volume(MINT_MFS_VOLUME *out);
/* Makes "/Users/<account>" (an existing folder) the volume's home. */
int64_t mint_mfs_set_home(const char *path);
/* Commit + device flush before power-off/restart (registered as the power flush hook). */
int mint_mfs_flush_for_power(void);
void mint_mfs_close_all(uint64_t pid);
int64_t mint_mfs_fstat(int64_t fd, uint64_t pid, MINT_MFS_STAT *out);   /* 26.5.014.0 */
/* Pure helpers (host-tested): GPT for a MintOS disk. */
uint32_t mint_gpt_crc32(uint32_t crc, const void *data, size_t len);
int mint_gpt_build(uint64_t disk_sectors, const uint8_t disk_guid[16], const uint8_t part_guid[16],
                   uint8_t mbr[512], uint8_t header[512], uint8_t entries[16384], uint8_t backup_header[512],
                   uint64_t *part_first, uint64_t *part_last);
/* Finds the MintOS partition in a primary GPT; 1 if found. */
int mint_gpt_find_mintos(const uint8_t header[512], const uint8_t entries[16384], uint64_t disk_sectors,
                         uint64_t *part_first, uint64_t *part_last);
extern const uint8_t MINT_GPT_MINTFS_TYPE[16];
#endif
#endif
