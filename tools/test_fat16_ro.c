#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/block.h"
#include "../kernel/include/fat16.h"
#include "../kernel/include/storage_diag.h"

static int file_read_sector(void *context, uint64_t lba, uint8_t out[512]) {
    FILE *f = (FILE *)context;
    if (!f || !out || lba > (uint64_t)LONG_MAX / 512u) return 0;
    if (fseek(f, (long)(lba * 512u), SEEK_SET) != 0) return 0;
    return fread(out, 1, 512, f) == 512;
}
void mint_storage_diag_mark_write_attempt(void) { }

static int checks;
static void check(int ok, const char *name) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", name); exit(1); }
    printf("PASS: %s\n", name); ++checks;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: test_fat16_ro raw-image.img\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 2; }
    long length = ftell(f);
    if (length != 512L * 1024L * 1024L) { fclose(f); fprintf(stderr,"unexpected image size\n"); return 2; }
    MINT_BLOCK_DEVICE d = {0};
    memcpy(d.name, "host-test-disk", 15);
    d.sector_count = (uint64_t)length / 512u;
    d.sector_size = 512u;
    d.flags = MINT_BLOCK_FLAG_READONLY;
    d.context = f;
    d.read_sector = file_read_sector;
    d.write_sector = 0;
    mint_block_init();
    check(mint_block_register(&d), "register real 512 MiB developer image as read-only");
    check(mint_fat16_try_mount(0), "real MBR partition and FAT16 volume mount read-only");
    check(mint_fat16_is_mounted(), "read-only filesystem mount reports active");
    check(mint_fat16_partition_lba() == 2048u, "mount uses the partition starting at LBA 2048");
    char notes[4096], settings[4096];
    int64_t notes_len = mint_fat16_read_file("MINTOS26/DOCUMENTS/NOTES.TXT", notes, sizeof(notes));
    int64_t settings_len = mint_fat16_read_file("MINTOS26/DOCUMENTS/SETTINGS.DAT", settings, sizeof(settings));
    check(notes_len >= 0 && notes_len < (int64_t)sizeof(notes), "read developer NOTES.TXT through FAT16 cluster chain");
    check(settings_len > 0 && settings_len < (int64_t)sizeof(settings), "read seeded SETTINGS.DAT through FAT16 cluster chain");
    check(memmem(settings, (size_t)settings_len, "theme=light", 11) != 0, "settings seed contents match expected text");
    uint8_t sector[512], before[512], after[512];
    check(file_read_sector(f, 0, sector), "host fixture reads LBA 0");
    memcpy(before, sector, sizeof(before));
    check(!mint_block_write(0, 0, sector), "block layer rejects writes to the mounted read-only volume");
    check(file_read_sector(f, 0, after) && memcmp(before, after, sizeof(before)) == 0,
          "read-only mount test leaves the source image unchanged");
    printf("INFO: %d host checks; this tests the FAT16 code with file-backed reads, not live AHCI/DMA.\n", checks);
    mint_fat16_unmount();
    fclose(f);
    return 0;
}
