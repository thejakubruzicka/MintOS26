#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/storage_diag.h"

#define SYNTH_SECTORS 32768u
static uint8_t synthetic[SYNTH_SECTORS][512];
static FILE *active_file;

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static int mem_read(void *ctx, uint64_t lba, uint8_t out[512]) {
    uint64_t sectors = *(uint64_t *)ctx;
    if (!out || lba >= sectors || sectors > SYNTH_SECTORS) return 0;
    memcpy(out, synthetic[lba], 512);
    return 1;
}
static void make_synthetic(void) {
    memset(synthetic, 0, sizeof(synthetic));
    uint8_t *mbr = synthetic[0];
    wr32(&mbr[440], MINT_STORAGE_DEVELOPER_DISK_ID);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    uint8_t *e = &mbr[446];
    e[0] = 0u; e[4] = 0x06u; wr32(&e[8], 2048u); wr32(&e[12], SYNTH_SECTORS - 2048u);
    uint8_t *b = synthetic[2048];
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(&b[3], "MINTOS26", 8);
    wr16(&b[11], 512u); b[13] = 2u; wr16(&b[14], 1u); b[16] = 2u;
    wr16(&b[17], 512u); wr16(&b[19], SYNTH_SECTORS - 2048u);
    b[21] = 0xF8u; wr16(&b[22], 120u); wr16(&b[24], 32u); wr16(&b[26], 64u);
    b[510] = 0x55; b[511] = 0xAA;
    memcpy(&b[54], "FAT16   ", 8);
}
static int expect(int ok, const char *label) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    return ok;
}

static int test_ata_capacity(void) {
    uint16_t words[256] = {0};
    MINT_ATA_CAPACITY_INFO cap = {0};
    int good = 1;
    words[49] = 1u << 9;
    words[60] = 0u; words[61] = 0x0010u; /* 1,048,576 sectors */
    words[100] = 0x1000u; words[101] = 1u; /* bogus non-zero unsupported LBA48 count */
    words[83] = 0x0400u; /* bit 14 missing: word 83 invalid */
    good &= expect(mint_ata_parse_capacity(words, &cap), "ATA IDENTIFY accepts valid LBA28 fallback");
    good &= expect(cap.selected_sectors == 1048576u && cap.source == MINT_STORAGE_CAPACITY_LBA28,
                   "invalid word 83 cannot override LBA28 count with bogus words 100-103");
    words[83] = 0x4400u; /* validity pattern 01 and LBA48 support bit 10 */
    words[60] = 0x0010u; words[61] = 0x0010u; /* conflicting 1,048,592 sectors */
    words[100] = 0u; words[101] = 0x0010u; words[102] = 0u; words[103] = 0u;
    good &= expect(mint_ata_parse_capacity(words, &cap), "ATA IDENTIFY accepts supported LBA48 count");
    good &= expect(cap.selected_sectors == 1048576u && cap.source == MINT_STORAGE_CAPACITY_LBA48,
                   "valid LBA48 count wins when supported");
    words[49] = 0u;
    good &= expect(!mint_ata_parse_capacity(words, &cap), "IDENTIFY without LBA support is rejected");
    return good;
}

static int test_synthetic(void) {
    uint64_t sectors = SYNTH_SECTORS;
    MINT_STORAGE_DEVICE_INFO out;
    memset(&out, 0xA5, sizeof(out));
    make_synthetic();
    int good = 1;
    good &= expect(mint_storage_diag_inspect(sectors, mem_read, &sectors, &out), "synthetic image inspection returns");
    good &= expect(out.mbr_signature_ok && out.partition_count == 1u &&
                   out.partitions[0].start_lba == 2048u &&
                   out.partitions[0].sector_count == SYNTH_SECTORS - 2048u,
                   "synthetic MBR signature, offset and length decoded");
    good &= expect(out.lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_MATCH &&
                   out.developer_disk_id_match && out.lba0_crc32 != 0u,
                   "LBA 0 repeat-read, CRC32 and developer image identity are captured");
    wr32(&synthetic[0][440], 0x12345678u);
    memset(&out, 0, sizeof(out));
    (void)mint_storage_diag_inspect(sectors, mem_read, &sectors, &out);
    good &= expect(out.mbr_signature_ok && !out.developer_disk_id_match &&
                   out.valid_mintos_fat16_count == 1u,
                   "unknown disk marker is reported but does not reject an otherwise valid FAT16 volume");
    good &= expect(out.valid_mintos_fat16_count == 1u &&
                   out.partitions[0].state == MINT_STORAGE_PART_VALID_MINTOS_FAT16,
                   "synthetic MintOS26 FAT16 BPB passes geometry validation");
    good &= expect(strstr(out.mount_status, "NOT MOUNTED") != NULL &&
                   strstr(out.write_status, "DISABLED") != NULL,
                   "diagnostics label mount unavailable and never enable media writes");
    /* Invalid BPB values must not be mislabeled as a filesystem mismatch. */
    make_synthetic();
    wr16(&synthetic[2048][11], 4096u);
    memset(&out, 0, sizeof(out));
    (void)mint_storage_diag_inspect(sectors, mem_read, &sectors, &out);
    good &= expect(out.partition_count == 1u && out.partitions[0].state == MINT_STORAGE_PART_INVALID_BPB &&
                   strstr(out.partitions[0].reason, "invalid BPB") != NULL,
                   "invalid BPB is reported as the exact FAT validation stage");
    /* Partition outside reported capacity fails at the exact bounds stage. */
    make_synthetic();
    wr32(&synthetic[0][446 + 8], SYNTH_SECTORS - 1u);
    wr32(&synthetic[0][446 + 12], 2048u);
    memset(&out, 0, sizeof(out));
    (void)mint_storage_diag_inspect(sectors, mem_read, &sectors, &out);
    good &= expect(out.partition_count == 1u && out.partitions[0].state == MINT_STORAGE_PART_OUT_OF_RANGE,
                   "out-of-range partition rejected before boot-sector read");
    /* Invalid partition status flags are not mislabeled as an LBA bounds issue. */
    make_synthetic();
    synthetic[0][446] = 0x7Fu;
    memset(&out, 0, sizeof(out));
    (void)mint_storage_diag_inspect(sectors, mem_read, &sectors, &out);
    good &= expect(out.partition_count == 1u && out.partitions[0].state == MINT_STORAGE_PART_INVALID_ENTRY,
                   "invalid MBR status flag has a distinct partition-entry failure stage");
    /* Bad MBR signature is diagnosed before entries are trusted. */
    make_synthetic();
    synthetic[0][510] = synthetic[0][511] = 0;
    memset(&out, 0, sizeof(out));
    (void)mint_storage_diag_inspect(sectors, mem_read, &sectors, &out);
    good &= expect(out.partition_table_state == MINT_STORAGE_TABLE_BAD_SIGNATURE &&
                   !out.mbr_signature_ok, "invalid MBR signature has a distinct failure stage");
    return good;
}

static int always_fail_read(void *opaque, uint64_t lba, uint8_t out[512]) {
    (void)opaque; (void)lba; (void)out;
    return 0;
}

static int test_read_failure_stage(void) {
    MINT_STORAGE_DEVICE_INFO out;
    memset(&out, 0, sizeof(out));
    int ok = mint_storage_diag_inspect(32768u, always_fail_read, NULL, &out) &&
             out.partition_table_state == MINT_STORAGE_TABLE_READ_FAILED &&
             !out.read_lba0_ok && strstr(out.read_status, "FAILED") != NULL;
    return expect(ok, "sector-zero I/O failure has a distinct validation stage");
}

static uint32_t unstable_lba0_calls;
static int unstable_lba0_read(void *opaque, uint64_t lba, uint8_t out[512]) {
    uint64_t sectors = *(uint64_t *)opaque;
    if (!out || lba >= sectors || sectors > SYNTH_SECTORS) return 0;
    memcpy(out, synthetic[lba], 512);
    if (lba == 0u && ++unstable_lba0_calls == 2u) out[20] ^= 0x01u;
    return 1;
}
static int test_unstable_repeat_read(void) {
    uint64_t sectors = SYNTH_SECTORS;
    MINT_STORAGE_DEVICE_INFO out;
    make_synthetic(); memset(&out, 0, sizeof(out)); unstable_lba0_calls = 0;
    int ok = mint_storage_diag_inspect(sectors, unstable_lba0_read, &sectors, &out) &&
             out.lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_DIFFERS &&
             out.partition_table_state == MINT_STORAGE_TABLE_UNSTABLE_READ &&
             out.partition_count == 0u;
    return expect(ok, "inconsistent LBA 0 reads stop partition parsing at the I/O-stability stage");
}

static int file_read(void *opaque, uint64_t lba, uint8_t out[512]) {
    uint64_t count = *(uint64_t *)opaque;
    if (lba >= count || !active_file || fseek(active_file, (long)(lba * 512u), SEEK_SET) != 0) return 0;
    return fread(out, 1, 512, active_file) == 512;
}

static int test_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open image: %s\n", path); return 0; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long bytes = ftell(f);
    if (bytes < 512 || (bytes % 512) != 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    active_file = f;
    uint64_t count = (uint64_t)bytes / 512u;
    MINT_STORAGE_DEVICE_INFO out;
    memset(&out, 0, sizeof(out));
    int ok = mint_storage_diag_inspect(count, file_read, &count, &out);
    printf("IMAGE %s: bytes=%ld sectors=%llu partition-count=%u MBR=%s sig=%02X%02X repeat=%u crc32=%08X disk-id=%08X marker=%s FAT=%s mount=%s\n",
           path, bytes, (unsigned long long)count, out.partition_count,
           out.mbr_signature_ok ? "valid" : "invalid", out.lba0_signature[0], out.lba0_signature[1],
           out.lba0_repeat_status, out.lba0_crc32, out.mbr_disk_id,
           out.developer_disk_id_match ? "match" : "mismatch/unknown", out.filesystem_status, out.mount_status);
    int pass = ok && out.read_lba0_ok && out.mbr_signature_ok && out.partition_count > 0u &&
               out.partition_table_state == MINT_STORAGE_TABLE_PARTITIONS_FOUND &&
               out.sector_count == count && out.capacity_bytes == (uint64_t)bytes;
    if (strstr(path, "data-512m") || strstr(path, "data-512MiB"))
        pass = pass && count == 1048576u && out.partitions[0].start_lba == 2048u &&
               out.partitions[0].sector_count == 1046528u && out.developer_disk_id_match;
    if (strstr(path, "128MiB"))
        pass = pass && count == 262144u && out.partitions[0].start_lba == 2048u &&
               out.partitions[0].sector_count == 260096u;
    if (pass) {
        printf("  partition 1: type=0x%02x start=%llu sectors=%llu state=%u reason=%s\n",
               out.partitions[0].type, (unsigned long long)out.partitions[0].start_lba,
               (unsigned long long)out.partitions[0].sector_count,
               out.partitions[0].state, out.partitions[0].reason);
    }
    fclose(f);
    return expect(pass, "real disk image MBR and partition bounds inspected read-only");
}

int main(int argc, char **argv) {
    int good = test_ata_capacity();
    good &= test_synthetic();
    good &= test_read_failure_stage();
    good &= test_unstable_repeat_read();
    for (int i = 1; i < argc; ++i) good &= test_image(argv[i]);
    return good ? 0 : 1;
}
