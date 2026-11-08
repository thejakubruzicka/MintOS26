#ifndef MINTOS26_RAMFS_H
#define MINTOS26_RAMFS_H
#include <stddef.h>
#include <stdint.h>
#include "desktop_abi.h"

#define MINT_RAMFS_MAX_FILES 8u
#define MINT_RAMFS_NAME_MAX 24u
#define MINT_RAMFS_FILE_CAPACITY 4096u
#define MINT_FS_OPEN_READ   0x01u
#define MINT_FS_OPEN_WRITE  0x02u
#define MINT_FS_OPEN_CREATE 0x04u
#define MINT_FS_OPEN_TRUNC  0x08u
#define MINT_FS_DIRENT_READONLY 0x01u

void mint_ramfs_init(void);
int64_t mint_ramfs_open(uint64_t owner, const char *name, uint32_t flags);
int64_t mint_ramfs_read(uint64_t owner, uint64_t handle, void *buffer, size_t length);
int64_t mint_ramfs_write(uint64_t owner, uint64_t handle, const void *buffer, size_t length);
int64_t mint_ramfs_close(uint64_t owner, uint64_t handle);
/* Release all open handles owned by a process when it exits. */
void mint_ramfs_close_owner(uint64_t owner);
int64_t mint_ramfs_list(MINT_FS_DIRENT *out, uint32_t capacity);

#endif
