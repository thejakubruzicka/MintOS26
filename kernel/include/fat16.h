#ifndef MINTOS26_FAT16_H
#define MINTOS26_FAT16_H
#include <stddef.h>
#include <stdint.h>

/* Classic FAT16 volume interface: read operations are bounded and remain
 * read-only; formatting is a separate, explicit blank-disk operation. */
#define MINT_FAT16_DIRENT_NAME_MAX 64u
#define MINT_FAT16_LIST_MAX 64u /* 26.5.010.0: was 16; folders like CHANGELOGS exceed it */
#define MINT_FAT16_MAX_VOLUMES 8u
#define MINT_FAT16_DIRENT_DIRECTORY 0x01u
typedef struct mint_fat16_dirent {
    char name[MINT_FAT16_DIRENT_NAME_MAX];
    uint32_t size;
    uint32_t flags;
} MINT_FAT16_DIRENT;

/* Read-only mount registry returned through a pointer-validated syscall.
 * Entries refer to registered block-device indexes, not UI rows or SATA ports. */
typedef struct mint_fat16_volume_list {
    uint32_t count;
    uint32_t selected_ordinal;
    uint32_t block_indices[MINT_FAT16_MAX_VOLUMES];
} MINT_FAT16_VOLUME_LIST;
int mint_fat16_try_mount(uint32_t block_device_index);
void mint_fat16_unmount(void);
int mint_fat16_is_mounted(void);
uint32_t mint_fat16_volume_count(void);
int mint_fat16_volume_select(uint32_t ordinal);
int mint_fat16_volume_list(MINT_FAT16_VOLUME_LIST *out);
int mint_fat16_is_device_mounted(uint32_t block_device_index);
int64_t mint_fat16_read_file(const char *path, void *buffer, size_t capacity);
int64_t mint_fat16_read_prefix(const char *path, void *buffer, size_t capacity);
#define MINT_FAT16_READ_PREFIX 1u /* MINT_SYS_FAT16_READ arg3 flag */
int64_t mint_fat16_list_dir(const char *path, MINT_FAT16_DIRENT *entries, uint32_t capacity);
/* Initialize only an unpartitioned/zero-LBA0 developer disk between 32 MiB
 * and 2 GiB. The block registry remains read-only; this private formatter is
 * the only production path allowed to invoke the write callback. */
int mint_fat16_format_blank(uint32_t block_device_index);
uint32_t mint_fat16_block_device_index(void);
uint64_t mint_fat16_partition_lba(void);
uint32_t mint_fat16_cluster_size_bytes(void);

#endif
