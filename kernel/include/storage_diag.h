#ifndef MINTOS26_STORAGE_DIAG_H
#define MINTOS26_STORAGE_DIAG_H
#include <stdint.h>

#define MINT_STORAGE_DIAG_VERSION 5u
#define MINT_STORAGE_DEVELOPER_DISK_ID 0x26300421u
#define MINT_STORAGE_MAX_CONTROLLERS 8u
#define MINT_STORAGE_MAX_DEVICES 8u
#define MINT_STORAGE_MAX_PARTITIONS 4u
#define MINT_STORAGE_REASON_LEN 80u
#define MINT_STORAGE_MODEL_LEN 41u
#define MINT_STORAGE_SERIAL_LEN 21u
#define MINT_STORAGE_FORMAT_REQUEST_VERSION 1u
#define MINT_STORAGE_FORMAT_CONFIRM_LEN 16u

typedef struct mint_storage_format_request {
    uint32_t version;
    uint32_t device_index;
    uint64_t expected_sector_count;
    char expected_serial[MINT_STORAGE_SERIAL_LEN];
    char confirmation[MINT_STORAGE_FORMAT_CONFIRM_LEN];
} MINT_STORAGE_FORMAT_REQUEST;

enum mint_storage_controller_state {
    MINT_STORAGE_CTRL_NOT_PROBED = 0,
    MINT_STORAGE_CTRL_READY = 1,
    MINT_STORAGE_CTRL_FAILED = 2,
    MINT_STORAGE_CTRL_DEVICE_LIMIT = 3
};

enum mint_storage_capacity_source {
    MINT_STORAGE_CAPACITY_UNKNOWN = 0,
    MINT_STORAGE_CAPACITY_LBA28 = 1,
    MINT_STORAGE_CAPACITY_LBA48 = 2
};

enum mint_storage_partition_state {
    MINT_STORAGE_PART_EMPTY = 0,
    MINT_STORAGE_PART_OUT_OF_RANGE = 1,
    MINT_STORAGE_PART_BOOT_READ_FAILED = 2,
    MINT_STORAGE_PART_BAD_SIGNATURE = 3,
    MINT_STORAGE_PART_INVALID_BPB = 4,
    MINT_STORAGE_PART_NOT_FAT16_GEOMETRY = 5,
    MINT_STORAGE_PART_MINTOS_LABEL_MISSING = 6,
    MINT_STORAGE_PART_VALID_MINTOS_FAT16 = 7,
    MINT_STORAGE_PART_VALID_FAT16_OTHER_LABEL = 8,
    MINT_STORAGE_PART_INVALID_ENTRY = 9
};

enum mint_storage_table_state {
    MINT_STORAGE_TABLE_UNKNOWN = 0,
    MINT_STORAGE_TABLE_READ_FAILED = 1,
    MINT_STORAGE_TABLE_BAD_SIGNATURE = 2,
    MINT_STORAGE_TABLE_NO_PARTITIONS = 3,
    MINT_STORAGE_TABLE_PARTITIONS_FOUND = 4,
    MINT_STORAGE_TABLE_SUPERFLOPPY = 5,
    MINT_STORAGE_TABLE_UNSTABLE_READ = 6
};

enum mint_storage_lba0_repeat_status {
    MINT_STORAGE_LBA0_REPEAT_UNKNOWN = 0,
    MINT_STORAGE_LBA0_REPEAT_MATCH = 1,
    MINT_STORAGE_LBA0_REPEAT_DIFFERS = 2,
    MINT_STORAGE_LBA0_REPEAT_FAILED = 3
};

typedef struct mint_storage_controller_info {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t state;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t abar;
    uint32_t port_implemented;
    char reason[MINT_STORAGE_REASON_LEN];
} MINT_STORAGE_CONTROLLER_INFO;

typedef struct mint_storage_partition_info {
    uint8_t table_index;
    uint8_t type;
    uint8_t bootable;
    uint8_t state;
    uint64_t start_lba;
    uint64_t sector_count;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_count;
    char reason[MINT_STORAGE_REASON_LEN];
} MINT_STORAGE_PARTITION_INFO;

typedef struct mint_storage_device_info {
    char name[16];
    char model[MINT_STORAGE_MODEL_LEN];
    char serial[MINT_STORAGE_SERIAL_LEN];
    uint32_t controller_index;
    uint32_t port;
    /* Snapshot slot and block registry slot are not necessarily identical: a
       detected SATA device may fail IDENTIFY/registration before later ports. */
    uint32_t block_index;
    uint32_t block_registered;
    uint32_t sector_size;
    uint32_t identify_ok;
    uint32_t lba48_supported;
    uint32_t capacity_source;
    uint32_t read_lba0_ok;
    uint32_t mbr_signature_ok;
    uint32_t partition_table_state;
    uint32_t partition_count;
    uint32_t valid_mintos_fat16_count;
    uint64_t sector_count;
    uint64_t reported_lba28_sectors;
    uint64_t reported_lba48_sectors;
    uint64_t capacity_bytes;
    MINT_STORAGE_PARTITION_INFO partitions[MINT_STORAGE_MAX_PARTITIONS];
    char read_status[MINT_STORAGE_REASON_LEN];
    char write_status[MINT_STORAGE_REASON_LEN];
    char filesystem_status[MINT_STORAGE_REASON_LEN];
    char mount_status[MINT_STORAGE_REASON_LEN];
    /* Independent evidence about the exact sector the filesystem parser saw. */
    uint8_t lba0_signature[2];
    uint8_t lba0_repeat_status;
    uint8_t developer_disk_id_match;
    uint32_t lba0_crc32;
    uint32_t mbr_disk_id;
} MINT_STORAGE_DEVICE_INFO;

typedef struct mint_storage_snapshot {
    uint32_t version;
    uint32_t probe_complete;
    uint32_t controller_count;
    uint32_t device_count;
    uint32_t controller_overflow;
    uint32_t device_overflow;
    uint32_t writes_attempted;
    uint32_t persistent_mount_active;
    uint32_t readonly_mount_active;
    uint32_t readonly_mount_count;
    uint32_t reserved_mount_count;
    MINT_STORAGE_CONTROLLER_INFO controllers[MINT_STORAGE_MAX_CONTROLLERS];
    MINT_STORAGE_DEVICE_INFO devices[MINT_STORAGE_MAX_DEVICES];
} MINT_STORAGE_SNAPSHOT;

/* Shared callback contract keeps the production parser host-testable. */
typedef int (*MINT_STORAGE_READ_SECTOR)(void *context, uint64_t lba, uint8_t out[512]);

int mint_storage_diag_inspect(uint64_t sector_count, MINT_STORAGE_READ_SECTOR read_sector,
                              void *context, MINT_STORAGE_DEVICE_INFO *out);
void mint_storage_diag_init(void);
void mint_storage_diag_recheck(void);
void mint_storage_diag_mark_write_attempt(void);
const MINT_STORAGE_SNAPSHOT *mint_storage_diag_snapshot(void);

/* Pure ATA IDENTIFY capacity/model helpers are covered by host tests. */
typedef struct mint_ata_capacity_info {
    uint64_t lba28_sectors;
    uint64_t lba48_sectors;
    uint64_t selected_sectors;
    uint32_t lba48_supported;
    uint32_t flush_cache_supported;
    uint32_t flush_cache_ext_supported;
    uint32_t source;
} MINT_ATA_CAPACITY_INFO;
int mint_ata_parse_capacity(const uint16_t words[256], MINT_ATA_CAPACITY_INFO *out);
void mint_ata_copy_identify_string(const uint16_t words[256], uint32_t first_word,
                                   uint32_t word_count, char *out, uint32_t out_size);

#endif
