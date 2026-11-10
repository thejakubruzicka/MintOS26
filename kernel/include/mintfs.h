#ifndef MINTOS26_MINTFS_H
#define MINTOS26_MINTFS_H
/* MintFS v1 — MintOS's native read/write filesystem.
 *
 * Copy-on-write and checksummed (inspired by APFS/ZFS): a transaction never
 * overwrites a block the last committed state still uses. New and changed
 * blocks are written to free space, the device is flushed, and only then is
 * the commit made durable by writing the next superblock (two alternating
 * slots with increasing generation). After a power cut, mount picks the newest
 * valid superblock, so the filesystem is always the last committed state.
 *
 * Layout (4 KiB blocks, relative to the partition):
 *   0, 1     superblocks A/B
 *   then     everything else, located through the superblock:
 *            inode map (2-level radix: root -> leaves -> inode blocks),
 *            free-space bitmap (index block -> bitmap blocks), inodes,
 *            directory data, file data and pointer blocks.
 * Every metadata block starts with a header carrying a type tag, CRC32C,
 * its own block address and the generation that wrote it.
 * Inode 1 is the root directory. Names are UTF-8, up to 255 bytes, compared
 * case-insensitively for ASCII and stored case-preserving (like APFS). */
#include <stdint.h>
#include <stddef.h>

#define MINTFS_BLOCK 4096u
#define MINTFS_VERSION 1u
#define MINTFS_ROOT_INO 1u
#define MINTFS_NAME_MAX 255u
#define MINTFS_CACHE_BLOCKS 256u
#define MINTFS_MAX_BLOCKS (508ull * 32512ull)     /* ~63 GiB */
#define MINTFS_INODE_FANOUT 508u
#define MINTFS_MAX_INODES (508u * 508u)

enum { MINTFS_TYPE_FILE = 1, MINTFS_TYPE_DIR = 2 };

/* Error codes (negative returns). */
enum {
    MINTFS_OK = 0, MINTFS_ENOENT = -2, MINTFS_EIO = -5, MINTFS_ENOMEM = -12, MINTFS_EEXIST = -17,
    MINTFS_ENOTDIR = -20, MINTFS_EISDIR = -21, MINTFS_EINVAL = -22, MINTFS_EFBIG = -27, MINTFS_ENOSPC = -28,
    MINTFS_EROFS = -30, MINTFS_ENAMETOOLONG = -36, MINTFS_ENOTEMPTY = -39, MINTFS_ECORRUPT = -117
};

/* Block device in 4 KiB units, relative to the start of the filesystem. */
typedef struct mintfs_dev {
    void *ctx;
    uint64_t block_count;
    int (*read)(void *ctx, uint64_t block, void *buf);
    int (*write)(void *ctx, uint64_t block, const void *buf);
    int (*flush)(void *ctx);
    int64_t (*now)(void *ctx);               /* Unix seconds, may return 0 */
} MINTFS_DEV;

typedef struct mintfs_stat {
    uint64_t ino;
    uint32_t type, mode, nlink, reserved;
    uint64_t size, blocks;
    int64_t btime, mtime, ctime;
    uint64_t parent;
} MINTFS_STAT;

typedef struct mintfs_dirent {
    uint64_t ino;
    uint32_t type;
    char name[MINTFS_NAME_MAX + 1];
} MINTFS_DIRENT;

typedef struct mintfs_info {
    uint64_t block_count, free_blocks, generation, inodes;
    char label[32];
    uint8_t uuid[16];
    int64_t created, modified;
} MINTFS_INFO;

typedef struct mintfs_cache_entry {
    uint64_t addr;
    uint64_t lru;
    uint8_t valid, dirty, pinned, reserved;
    uint8_t data[MINTFS_BLOCK];
} MINTFS_CACHE_ENTRY;

typedef struct mintfs {
    MINTFS_DEV dev, device;           /* dev.block_count = filesystem size; device = as supplied */
    int mounted, read_only, failed, in_commit;
    uint8_t *bitmaps; size_t bitmaps_len;
    uint64_t generation;              /* generation of the open transaction (= committed + 1) */
    uint8_t sb[MINTFS_BLOCK];         /* in-memory superblock of the open transaction */
    uint64_t lru_clock;
    uint32_t dirty_blocks;
    uint64_t bitmap_bytes;
    uint8_t *used;                    /* bit per block: in use in the open transaction */
    uint8_t *reserved;                /* bit per block: freed this transaction, still used by the committed state */
    uint8_t *bitmap_dirty;            /* byte per bitmap block */
    uint64_t alloc_hint, inode_hint;
    MINTFS_CACHE_ENTRY cache[MINTFS_CACHE_BLOCKS];
    uint8_t scratch[MINTFS_BLOCK], scratch2[MINTFS_BLOCK];
} MINTFS;

/* Storage the caller provides for the bitmaps (see mintfs_bitmap_bytes). */
uint64_t mintfs_bitmap_bytes(uint64_t block_count);

int mintfs_format(const MINTFS_DEV *dev, const char *label, const uint8_t uuid[16]);
/* bitmaps: at least mintfs_mount_bytes(dev->block_count) bytes, owned by the caller. */
uint64_t mintfs_mount_bytes(uint64_t block_count);
int mintfs_mount(MINTFS *fs, const MINTFS_DEV *dev, uint8_t *bitmaps, size_t bitmaps_len, int read_only);
int mintfs_commit(MINTFS *fs);
int mintfs_abort(MINTFS *fs);       /* discard the open transaction */
int mintfs_unmount(MINTFS *fs);

int mintfs_lookup(MINTFS *fs, uint64_t dir, const char *name, uint64_t *ino);
int mintfs_create(MINTFS *fs, uint64_t dir, const char *name, uint32_t type, uint64_t *ino);
int64_t mintfs_read(MINTFS *fs, uint64_t ino, uint64_t offset, void *buf, uint32_t len);
int64_t mintfs_write(MINTFS *fs, uint64_t ino, uint64_t offset, const void *buf, uint32_t len);
int mintfs_truncate(MINTFS *fs, uint64_t ino, uint64_t size);
int mintfs_unlink(MINTFS *fs, uint64_t dir, const char *name);
int mintfs_rename(MINTFS *fs, uint64_t src_dir, const char *src_name, uint64_t dst_dir, const char *dst_name);
/* Returns 1 with entry `index` (0-based), 0 past the end, or an error. */
int mintfs_readdir(MINTFS *fs, uint64_t dir, uint32_t index, MINTFS_DIRENT *out);
/* Up to `capacity` entries starting at `start`, loading the directory once; returns the count. */
int mintfs_list(MINTFS *fs, uint64_t dir, uint32_t start, MINTFS_DIRENT *out, uint32_t capacity);
int mintfs_stat(MINTFS *fs, uint64_t ino, MINTFS_STAT *out);
int mintfs_info(MINTFS *fs, MINTFS_INFO *out);
/* Resolves an absolute path ("/Users/me/Documents"); "/" is the root. */
int mintfs_resolve(MINTFS *fs, const char *path, uint64_t *ino);

/* Consistency check of the committed state (fsck). Returns 0 if clean;
 * problems are counted in the report. */
typedef struct mintfs_check_report {
    uint64_t inodes, directories, files, blocks_reachable, blocks_marked_used;
    uint32_t bad_checksums, bad_pointers, double_references, leaked_blocks, missing_blocks, bad_directories, bad_links;
    char first_problem[96];
    uint64_t problem_block;           /* block number involved in the first problem, if any */
} MINTFS_CHECK_REPORT;
int mintfs_check(const MINTFS_DEV *dev, MINTFS_CHECK_REPORT *report, uint8_t *work, size_t work_len);
uint64_t mintfs_check_work_bytes(uint64_t block_count);

uint32_t mintfs_crc32c(uint32_t crc, const void *data, size_t len);
const char *mintfs_strerror(int code);
#endif
