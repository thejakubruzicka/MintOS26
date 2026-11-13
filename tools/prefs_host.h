/* Host build of MintDE's data-saving modules (prefs.c, accounts.c) for
 * tools/test_data_saving.c: the user-space runtime file API is provided by
 * the test on top of the real kernel MintFS code (mintos_disk.c, mintfs.c). */
#ifndef MINT_PREFS_HOST_H
#define MINT_PREFS_HOST_H
#include <stdint.h>
#include <stddef.h>
#include "../kernel/include/uapi/mintos/syscall.h"
#define MINT_MFS_USERSPACE 1
#include "../kernel/include/mintos_fs.h"
#include "../kernel/user/mintde/userdata.h"

int64_t mint_file_load(const char *path, void *out, size_t capacity);
int64_t mint_file_save(const char *path, const void *data, size_t length);
int64_t mint_path_stat(const char *path, MINT_MFS_STAT *out);
int64_t mint_dir_create(const char *path);
int64_t mint_path_rename(const char *from, const char *to);
int64_t mint_path_remove(const char *path);
int64_t mint_fs_get_volume(MINT_MFS_VOLUME *out);
int64_t mint_fs_set_home(const char *path);
int64_t mint_fs_flush(void);
int64_t mint_clock_gettime(int clock_id, mint_timespec64 *out);
#endif
