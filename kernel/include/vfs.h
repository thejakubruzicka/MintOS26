#ifndef MINTOS26_VFS_H
#define MINTOS26_VFS_H
#include <stdint.h>

#define MINT_VFS_ABI_VERSION 1u
#define MINT_VFS_MAX_MOUNTS 10u
#define MINT_VFS_PATH_LEN 32u
#define MINT_VFS_SOURCE_LEN 16u
#define MINT_VFS_TYPE_LEN 12u
#define MINT_VFS_FLAG_READ_ONLY 0x01u
#define MINT_VFS_FLAG_VOLATILE  0x02u

typedef struct mint_vfs_mount {
    char path[MINT_VFS_PATH_LEN];
    char source[MINT_VFS_SOURCE_LEN];
    char filesystem[MINT_VFS_TYPE_LEN];
    uint32_t flags;
    uint32_t block_index;
} MINT_VFS_MOUNT;

typedef struct mint_vfs_mount_table {
    uint32_t version;
    uint32_t count;
    uint32_t truncated;
    uint32_t reserved;
    MINT_VFS_MOUNT mounts[MINT_VFS_MAX_MOUNTS];
} MINT_VFS_MOUNT_TABLE;

/* A versioned mount-table abstraction. FAT16 is the first block-backed driver;
 * future filesystems (including a possible ext2/ext4 reader) will register
 * through this layer instead of teaching each app about block devices. */
int mint_vfs_mount_table_snapshot(MINT_VFS_MOUNT_TABLE *out);
#endif
