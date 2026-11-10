#ifndef MINTOS26_BLOCK_H
#define MINTOS26_BLOCK_H
#include <stddef.h>
#include <stdint.h>

#define MINT_BLOCK_SECTOR_SIZE 512u
#define MINT_BLOCK_MAX_DEVICES 8u
#define MINT_BLOCK_DEVICE_NAME 16u
#define MINT_BLOCK_FLAG_READONLY 0x01u
#define MINT_BLOCK_IO_STATS_VERSION 1u

typedef struct mint_block_io_stats {
    uint32_t version;
    uint32_t device_index;
    uint64_t read_calls;
    uint64_t read_success;
    uint64_t read_failures;
    uint64_t write_calls;
    uint64_t writes_blocked;
    uint64_t write_success;
    uint64_t write_failures;
} MINT_BLOCK_IO_STATS;

typedef int (*MINT_BLOCK_READ_SECTOR)(void *context, uint64_t lba,
                                      uint8_t out[MINT_BLOCK_SECTOR_SIZE]);
typedef int (*MINT_BLOCK_WRITE_SECTOR)(void *context, uint64_t lba,
                                       const uint8_t data[MINT_BLOCK_SECTOR_SIZE]);
typedef int (*MINT_BLOCK_FLUSH)(void *context);
typedef int (*MINT_BLOCK_READ_SECTORS)(void *context, uint64_t lba, uint32_t count, uint8_t *out);
typedef int (*MINT_BLOCK_WRITE_SECTORS)(void *context, uint64_t lba, uint32_t count, const uint8_t *data);

typedef struct mint_block_device {
    char name[MINT_BLOCK_DEVICE_NAME];
    uint64_t sector_count;
    uint32_t sector_size;
    uint32_t flags;
    void *context;
    MINT_BLOCK_READ_SECTOR read_sector;
    MINT_BLOCK_WRITE_SECTOR write_sector;
    MINT_BLOCK_FLUSH flush;
    MINT_BLOCK_READ_SECTORS read_sectors;      /* optional: up to 8 sectors per call */
    MINT_BLOCK_WRITE_SECTORS write_sectors;    /* optional; never used without a write grant */
} MINT_BLOCK_DEVICE;

void mint_block_init(void);
int mint_block_register(const MINT_BLOCK_DEVICE *device);
uint32_t mint_block_count(void);
const MINT_BLOCK_DEVICE *mint_block_get(uint32_t index);
int mint_block_read(uint32_t index, uint64_t lba, uint8_t out[MINT_BLOCK_SECTOR_SIZE]);
int mint_block_write(uint32_t index, uint64_t lba, const uint8_t data[MINT_BLOCK_SECTOR_SIZE]);
int mint_block_io_stats(uint32_t index, MINT_BLOCK_IO_STATS *out);
/* Multi-sector read (falls back to single sectors). count <= 8. */
int mint_block_read_sectors(uint32_t index, uint64_t lba, uint32_t count, uint8_t *out);

/* Write grants. Every registered disk stays read-only to mint_block_write.
 * The MintOS-disk code grants itself writes to one LBA range of one device,
 * after it has verified that the range is a MintOS partition (or is creating
 * one on a verified blank disk). Writes outside the granted range fail. */
int mint_block_grant_writes(uint32_t index, uint64_t first_lba, uint64_t sectors, uint32_t owner);
void mint_block_revoke_writes(uint32_t index, uint32_t owner);
int mint_block_write_granted(uint32_t index, uint64_t lba, uint32_t count, const uint8_t *data, uint32_t owner);
int mint_block_flush_granted(uint32_t index, uint32_t owner);

/* The diagnostics release intentionally does not expose a write callback. */
int mint_ahci_init(void);

#endif
