#include <stdint.h>
#include <stddef.h>
#include "../include/storage_diag.h"

#define MBR_ENTRY_OFFSET 446u
#define MBR_ENTRY_SIZE 16u
#define FAT16_TYPE_CHS 0x04u
#define FAT16_TYPE_LBA 0x06u
#define FAT16_TYPE_LBA_LARGE 0x0Eu
#define MINT_FAT16_MIN_CLUSTERS 4085u
#define MINT_FAT16_MAX_CLUSTERS 65524u

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void copy_text(char *out, size_t cap, const char *in) {
    if (!out || !cap) return;
    size_t i = 0;
    if (in) while (in[i] && i + 1u < cap) { out[i] = in[i]; ++i; }
    out[i] = '\0';
}
static int is_fat16_type(uint8_t type) {
    return type == FAT16_TYPE_CHS || type == FAT16_TYPE_LBA || type == FAT16_TYPE_LBA_LARGE;
}
static int is_power_of_two(uint32_t n) { return n && !(n & (n - 1u)); }
static uint32_t crc32_sector(const uint8_t *data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < 512u; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}
static int sector_equal(const uint8_t *a, const uint8_t *b) {
    for (uint32_t i = 0; i < 512u; ++i) if (a[i] != b[i]) return 0;
    return 1;
}


static void partition_reason(MINT_STORAGE_PARTITION_INFO *p, uint8_t state,
                             const char *reason) {
    p->state = state;
    copy_text(p->reason, sizeof(p->reason), reason);
}

static uint8_t check_boot_sector(const uint8_t boot[512], uint64_t device_sectors,
                                 uint64_t partition_start, uint64_t partition_sectors,
                                 MINT_STORAGE_PARTITION_INFO *p) {
    if (boot[510] != 0x55u || boot[511] != 0xAAu) {
        partition_reason(p, MINT_STORAGE_PART_BAD_SIGNATURE, "boot-sector signature is not 0x55AA");
        return p->state;
    }
    uint32_t bps = rd16(&boot[11]);
    uint32_t spc = boot[13];
    uint32_t reserved = rd16(&boot[14]);
    uint32_t fats = boot[16];
    uint32_t root_entries = rd16(&boot[17]);
    uint32_t total = rd16(&boot[19]);
    if (!total) total = rd32(&boot[32]);
    uint32_t sectors_per_fat = rd16(&boot[22]);

    if (bps != 512u || !is_power_of_two(spc) || spc > 128u || !reserved ||
        !fats || fats > 2u || !root_entries || !sectors_per_fat || !total) {
        partition_reason(p, MINT_STORAGE_PART_INVALID_BPB,
                         "invalid BPB: expected 512-byte sectors and bounded FAT geometry");
        return p->state;
    }
    if (total > partition_sectors || partition_start > device_sectors ||
        total > device_sectors - partition_start) {
        partition_reason(p, MINT_STORAGE_PART_INVALID_BPB,
                         "BPB total-sector count exceeds the partition or device bounds");
        return p->state;
    }
    uint64_t root_bytes = (uint64_t)root_entries * 32u;
    uint64_t root_sectors = (root_bytes + bps - 1u) / bps;
    uint64_t data_start = (uint64_t)reserved + (uint64_t)fats * sectors_per_fat + root_sectors;
    if (data_start >= total) {
        partition_reason(p, MINT_STORAGE_PART_INVALID_BPB,
                         "invalid BPB: FAT and root-directory regions consume the volume");
        return p->state;
    }
    uint64_t clusters = (total - data_start) / spc;
    uint64_t fat_entries = (uint64_t)sectors_per_fat * bps / 2u;
    if (clusters < MINT_FAT16_MIN_CLUSTERS || clusters > MINT_FAT16_MAX_CLUSTERS ||
        fat_entries < clusters + 2u) {
        p->bytes_per_sector = bps;
        p->sectors_per_cluster = spc;
        p->cluster_count = clusters > UINT32_MAX ? UINT32_MAX : (uint32_t)clusters;
        partition_reason(p, MINT_STORAGE_PART_NOT_FAT16_GEOMETRY,
                         "BPB parsed, but cluster count/FAT capacity is not valid FAT16 geometry");
        return p->state;
    }
    p->bytes_per_sector = bps;
    p->sectors_per_cluster = spc;
    p->cluster_count = (uint32_t)clusters;
    int label = boot[54] == 'F' && boot[55] == 'A' && boot[56] == 'T' &&
                boot[57] == '1' && boot[58] == '6';
    if (!label) {
        partition_reason(p, MINT_STORAGE_PART_VALID_FAT16_OTHER_LABEL,
                         "FAT16 geometry is valid; MintOS FAT16 type label is missing");
        return p->state;
    }
    int mintos_oem = boot[3] == 'M' && boot[4] == 'I' && boot[5] == 'N' &&
                     boot[6] == 'T' && boot[7] == 'O' && boot[8] == 'S' &&
                     boot[9] == '2' && boot[10] == '6';
    if (!mintos_oem) {
        partition_reason(p, MINT_STORAGE_PART_VALID_FAT16_OTHER_LABEL,
                         "FAT16 geometry/label valid; OEM ID is not MINTOS26");
        return p->state;
    }
    partition_reason(p, MINT_STORAGE_PART_VALID_MINTOS_FAT16,
                     "MintOS26 FAT16 BPB and cluster geometry validated read-only");
    return p->state;
}

static void inspect_partition(uint64_t device_sectors, MINT_STORAGE_READ_SECTOR reader,
                              void *context, MINT_STORAGE_PARTITION_INFO *p) {
    if (p->start_lba >= device_sectors || !p->sector_count ||
        p->sector_count > device_sectors - p->start_lba) {
        partition_reason(p, MINT_STORAGE_PART_OUT_OF_RANGE,
                         "partition start/length exceeds identified device capacity");
        return;
    }
    if (!is_fat16_type(p->type)) {
        partition_reason(p, MINT_STORAGE_PART_EMPTY,
                         "partition type is not one of the supported FAT16 types");
        return;
    }
    uint8_t boot[512];
    if (!reader(context, p->start_lba, boot)) {
        partition_reason(p, MINT_STORAGE_PART_BOOT_READ_FAILED,
                         "failed to read partition boot sector");
        return;
    }
    (void)check_boot_sector(boot, device_sectors, p->start_lba, p->sector_count, p);
}

int mint_storage_diag_inspect(uint64_t sector_count, MINT_STORAGE_READ_SECTOR reader,
                              void *context, MINT_STORAGE_DEVICE_INFO *out) {
    if (!sector_count || !reader || !out) return 0;
    out->sector_size = 512u;
    out->sector_count = sector_count;
    out->capacity_bytes = sector_count <= UINT64_MAX / 512u ? sector_count * 512u : UINT64_MAX;
    out->read_lba0_ok = 0u;
    out->mbr_signature_ok = 0u;
    out->lba0_signature[0] = out->lba0_signature[1] = 0u;
    out->lba0_repeat_status = MINT_STORAGE_LBA0_REPEAT_UNKNOWN;
    out->developer_disk_id_match = 0u;
    out->lba0_crc32 = 0u;
    out->mbr_disk_id = 0u;
    out->partition_table_state = MINT_STORAGE_TABLE_UNKNOWN;
    out->partition_count = 0u;
    out->valid_mintos_fat16_count = 0u;
    copy_text(out->write_status, sizeof(out->write_status),
              "DISABLED - read-only diagnostics; no media writes attempted");
    copy_text(out->mount_status, sizeof(out->mount_status),
              "NOT MOUNTED - read-only mount verification pending; persistent writes disabled");
    for (uint32_t i = 0; i < MINT_STORAGE_MAX_PARTITIONS; ++i) {
        uint8_t *raw = (uint8_t *)&out->partitions[i];
        for (size_t j = 0; j < sizeof(out->partitions[i]); ++j) raw[j] = 0u;
    }

    uint8_t sector0[512];
    if (!reader(context, 0u, sector0)) {
        out->partition_table_state = MINT_STORAGE_TABLE_READ_FAILED;
        copy_text(out->read_status, sizeof(out->read_status), "FAILED: could not read LBA 0");
        copy_text(out->filesystem_status, sizeof(out->filesystem_status),
                  "not checked: sector 0 read failed");
        return 1;
    }
    out->read_lba0_ok = 1u;
    out->lba0_signature[0] = sector0[510];
    out->lba0_signature[1] = sector0[511];
    out->lba0_crc32 = crc32_sector(sector0);
    out->mbr_disk_id = rd32(&sector0[440]);

    /* Repeat the same read before trusting a partition table. This is a bounded,
       read-only consistency check for stale images and DMA/buffer corruption. */
    uint8_t verify0[512];
    if (!reader(context, 0u, verify0)) {
        out->lba0_repeat_status = MINT_STORAGE_LBA0_REPEAT_FAILED;
        out->partition_table_state = MINT_STORAGE_TABLE_READ_FAILED;
        copy_text(out->read_status, sizeof(out->read_status),
                  "PARTIAL: first LBA 0 read OK; repeat read failed");
        copy_text(out->filesystem_status, sizeof(out->filesystem_status),
                  "not checked: repeat LBA 0 read failed; I/O path unverified");
        return 1;
    }
    if (!sector_equal(sector0, verify0)) {
        out->lba0_repeat_status = MINT_STORAGE_LBA0_REPEAT_DIFFERS;
        out->partition_table_state = MINT_STORAGE_TABLE_UNSTABLE_READ;
        copy_text(out->read_status, sizeof(out->read_status),
                  "WARNING: repeated LBA 0 reads differ");
        copy_text(out->filesystem_status, sizeof(out->filesystem_status),
                  "failed at LBA 0 stability check; do not trust this read path");
        return 1;
    }
    out->lba0_repeat_status = MINT_STORAGE_LBA0_REPEAT_MATCH;
    out->mbr_signature_ok = sector0[510] == 0x55u && sector0[511] == 0xAAu;
    out->developer_disk_id_match = out->mbr_signature_ok &&
                                   out->mbr_disk_id == MINT_STORAGE_DEVELOPER_DISK_ID;
    copy_text(out->read_status, sizeof(out->read_status),
              "OK: two LBA 0 reads matched");
    if (!out->mbr_signature_ok) {
        out->partition_table_state = MINT_STORAGE_TABLE_BAD_SIGNATURE;
        copy_text(out->filesystem_status, sizeof(out->filesystem_status),
                  "bad MBR signature; inspect LBA0 bytes and verify attached disk image");
        return 1;
    }

    for (uint32_t i = 0; i < MINT_STORAGE_MAX_PARTITIONS; ++i) {
        const uint8_t *entry = &sector0[MBR_ENTRY_OFFSET + i * MBR_ENTRY_SIZE];
        uint8_t type = entry[4];
        uint32_t start = rd32(&entry[8]);
        uint32_t count = rd32(&entry[12]);
        if (!type && !start && !count && entry[0] == 0u) continue;
        if (out->partition_count >= MINT_STORAGE_MAX_PARTITIONS) break;
        MINT_STORAGE_PARTITION_INFO *p = &out->partitions[out->partition_count++];
        p->table_index = (uint8_t)(i + 1u);
        p->type = type;
        p->bootable = entry[0] == 0x80u;
        p->start_lba = start;
        p->sector_count = count;
        if (entry[0] != 0u && entry[0] != 0x80u) {
            partition_reason(p, MINT_STORAGE_PART_INVALID_ENTRY,
                             "invalid MBR entry: status byte is neither inactive nor bootable");
        } else {
            inspect_partition(sector_count, reader, context, p);
        }
        if (p->state == MINT_STORAGE_PART_VALID_MINTOS_FAT16)
            ++out->valid_mintos_fat16_count;
    }

    if (out->partition_count) {
        out->partition_table_state = MINT_STORAGE_TABLE_PARTITIONS_FOUND;
        for (uint32_t i = 0; i < out->partition_count; ++i) {
            if (out->partitions[i].state == MINT_STORAGE_PART_VALID_MINTOS_FAT16) {
                copy_text(out->filesystem_status, sizeof(out->filesystem_status),
                          "MintOS26 FAT16 candidate validated; read-only mount verification pending");
                return 1;
            }
        }
        for (uint32_t i = 0; i < out->partition_count; ++i) {
            MINT_STORAGE_PARTITION_INFO *p = &out->partitions[i];
            if (is_fat16_type(p->type)) {
                copy_text(out->filesystem_status, sizeof(out->filesystem_status), p->reason);
                return 1;
            }
        }
        copy_text(out->filesystem_status, sizeof(out->filesystem_status),
                  "partition table valid, but no supported FAT16 partition type was found");
        return 1;
    }

    /* With no partition entries, try the same sector as a superfloppy BPB. */
    out->partition_table_state = MINT_STORAGE_TABLE_SUPERFLOPPY;
    MINT_STORAGE_PARTITION_INFO *p = &out->partitions[0];
    p->table_index = 0u;
    p->type = 0u;
    p->start_lba = 0u;
    p->sector_count = sector_count;
    (void)check_boot_sector(sector0, sector_count, 0u, sector_count, p);
    if (p->state == MINT_STORAGE_PART_VALID_MINTOS_FAT16) {
        out->partition_count = 1u;
        out->valid_mintos_fat16_count = 1u;
        copy_text(out->filesystem_status, sizeof(out->filesystem_status),
                  "MintOS26 FAT16 superfloppy candidate validated; read-only mount verification pending");
    } else {
        out->partition_count = 0u;
        copy_text(out->filesystem_status, sizeof(out->filesystem_status),
                  p->reason[0] ? p->reason : "no partition entries and no valid FAT16 BPB");
    }
    return 1;
}
