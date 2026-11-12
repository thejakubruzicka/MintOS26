/* mkmintos_disk: create a NEW, already initialized MintOS disk image on the
 * host for QEMU automation (26.5.014.0 app-platform evidence).
 *
 *   mkmintos_disk <new-image> [MiB]      (default 1024 MiB; refuses an existing file)
 *
 * It runs the real kernel initializer (mintos_disk.c: blank-disk check, GPT,
 * MintFS, standard folder layout) against a RAM disk and writes the result as
 * a sparse file. It never opens an existing image for writing.
 * Build: cc -std=c11 -O2 tools/mkmintos_disk.c kernel/block/block.c kernel/fs/mintos_disk.c kernel/fs/mintfs.c */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "../kernel/include/block.h"
#include "../kernel/include/mintos_fs.h"

static uint64_t ticks = 1000;
uint64_t mint_timer_ticks(void) { return ticks += 3; }
int64_t mint_rtc_boot_epoch(void) { return 1794300000; }
void mint_serial_write(const char *s) { (void)s; }
void mint_serial_dec64(uint64_t v) { (void)v; }
void mint_log_emit(uint32_t severity, uint32_t component, const char *message) { (void)severity; (void)component; (void)message; }
void mint_task_sleep_ticks(uint64_t t) { ticks += t; }
uint64_t mint_physmem_claim_contiguous_below(uint64_t pages, uint64_t limit) {
    (void)limit;
    return (uint64_t)(uintptr_t)calloc((size_t)pages, 4096);
}
int mint_physmem_release_contiguous(uint64_t base, uint64_t pages) { (void)pages; free((void *)(uintptr_t)base); return 1; }

static uint8_t *disk;
static uint64_t sectors;
static int rd_read(void *c, uint64_t lba, uint8_t *out) { (void)c; if (lba >= sectors) return 0; memcpy(out, disk + lba * 512, 512); return 1; }
static int rd_write(void *c, uint64_t lba, const uint8_t *in) { (void)c; if (lba >= sectors) return 0; memcpy(disk + lba * 512, in, 512); return 1; }
static int rd_reads(void *c, uint64_t lba, uint32_t n, uint8_t *out) { (void)c; if (lba + n > sectors) return 0; memcpy(out, disk + lba * 512, n * 512u); return 1; }
static int rd_writes(void *c, uint64_t lba, uint32_t n, const uint8_t *in) { (void)c; if (lba + n > sectors) return 0; memcpy(disk + lba * 512, in, n * 512u); return 1; }
static int rd_flush(void *c) { (void)c; return 1; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: mkmintos_disk <new-image> [MiB]\n"); return 2; }
    uint64_t mib = argc > 2 ? strtoull(argv[2], 0, 10) : 1024u;
    if (mib < 64u || mib > 4096u) { fprintf(stderr, "size must be 64..4096 MiB\n"); return 2; }
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { perror(argv[1]); return 1; }
    sectors = mib * 2048u;
    disk = calloc(sectors, 512);
    if (!disk) { fprintf(stderr, "out of memory\n"); return 1; }
    mint_block_init();
    MINT_BLOCK_DEVICE dev; memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "sda");
    dev.sector_count = sectors; dev.sector_size = 512; dev.flags = MINT_BLOCK_FLAG_READONLY;
    dev.read_sector = rd_read; dev.write_sector = rd_write; dev.flush = rd_flush;
    dev.read_sectors = rd_reads; dev.write_sectors = rd_writes;
    if (!mint_block_register(&dev)) { fprintf(stderr, "block register failed\n"); return 1; }
    mint_mintos_disk_probe();
    int64_t rc = mint_mintos_disk_initialize(0);
    if (rc) { fprintf(stderr, "initialization failed: %lld\n", (long long)rc); return 1; }
    if (ftruncate(fd, (off_t)(sectors * 512u))) { perror("ftruncate"); return 1; }
    for (uint64_t off = 0; off < sectors * 512u; off += 4096u) {
        int nonzero = 0;
        for (int i = 0; i < 4096 && !nonzero; ++i) nonzero = disk[off + (uint64_t)i] != 0;
        if (nonzero && pwrite(fd, disk + off, 4096, (off_t)off) != 4096) { perror("pwrite"); return 1; }
    }
    close(fd);
    printf("created MintOS disk %s (%llu MiB)\n", argv[1], (unsigned long long)mib);
    return 0;
}
