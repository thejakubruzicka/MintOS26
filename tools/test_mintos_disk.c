/* Host integration test for MintOS disks: the real block layer (block.c),
 * MintOS-disk module (mintos_disk.c) and MintFS (mintfs.c) on a RAM disk that
 * is registered read-only, exactly like an AHCI disk.
 *
 * Checks the write-safety rules (blank-disk initialization only, writes only
 * inside the granted MintOS partition, generic writes refused) and the file
 * API the syscalls expose (handles, per-process ownership, rename, remove,
 * listing), then remounts from the disk and runs the MintFS checker.
 * Build: cc -std=c11 tools/test_mintos_disk.c kernel/block/block.c kernel/fs/mintos_disk.c kernel/fs/mintfs.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../kernel/include/block.h"
#include "../kernel/include/mintfs.h"
#include "../kernel/include/mintos_fs.h"
#include "../kernel/include/uapi/mintos/syscall.h"

/* ---- kernel services the module uses */
static uint64_t ticks = 1000;
uint64_t mint_timer_ticks(void) { return ticks += 3; }
int64_t mint_rtc_boot_epoch(void) { return 1794300000; }
void mint_serial_write(const char *s) { if (getenv("MINT_VERBOSE")) fputs(s, stdout); }
void mint_serial_dec64(uint64_t v) { if (getenv("MINT_VERBOSE")) printf("%llu", (unsigned long long)v); }
void mint_log_emit(uint32_t severity, uint32_t component, const char *message) { (void)severity; (void)component; (void)message; }
void mint_task_sleep_ticks(uint64_t t) { ticks += t; }
static uint64_t claimed_pages;
uint64_t mint_physmem_claim_contiguous_below(uint64_t pages, uint64_t limit) {
    (void)limit; claimed_pages += pages;
    return (uint64_t)(uintptr_t)calloc((size_t)pages, 4096);
}
int mint_physmem_release_contiguous(uint64_t base, uint64_t pages) { claimed_pages -= pages; free((void *)(uintptr_t)base); return 1; }

/* ---- RAM disk with write logging */
typedef struct { uint8_t *data; uint64_t sectors; uint64_t writes, flushes, lowest_write, highest_write; } RAMDISK;
static int rd_read(void *c, uint64_t lba, uint8_t *out) { RAMDISK *d = c; if (lba >= d->sectors) return 0; memcpy(out, d->data + lba * 512, 512); return 1; }
static void note_write(RAMDISK *d, uint64_t lba, uint32_t n) {
    if (!d->writes || lba < d->lowest_write) d->lowest_write = lba;
    if (lba + n - 1 > d->highest_write) d->highest_write = lba + n - 1;
    d->writes += n;
}
static int rd_write(void *c, uint64_t lba, const uint8_t *in) { RAMDISK *d = c; if (lba >= d->sectors) return 0; note_write(d, lba, 1); memcpy(d->data + lba * 512, in, 512); return 1; }
static int rd_reads(void *c, uint64_t lba, uint32_t n, uint8_t *out) { RAMDISK *d = c; if (lba + n > d->sectors) return 0; memcpy(out, d->data + lba * 512, n * 512u); return 1; }
static int rd_writes(void *c, uint64_t lba, uint32_t n, const uint8_t *in) { RAMDISK *d = c; if (lba + n > d->sectors) return 0; note_write(d, lba, n); memcpy(d->data + lba * 512, in, n * 512u); return 1; }
static int rd_flush(void *c) { ((RAMDISK *)c)->flushes++; return 1; }

static int failures, checks;
#define CHECK(cond, msg) do { ++checks; if (cond) printf("PASS: %s\n", msg); else { ++failures; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static RAMDISK disks[3];
static void add_disk(RAMDISK *d, const char *name, uint64_t sectors) {
    d->sectors = sectors; d->data = calloc(sectors, 512);
    MINT_BLOCK_DEVICE dev; memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "%s", name);
    dev.sector_count = sectors; dev.sector_size = 512; dev.flags = MINT_BLOCK_FLAG_READONLY; dev.context = d;
    dev.read_sector = rd_read; dev.write_sector = rd_write; dev.flush = rd_flush; dev.read_sectors = rd_reads; dev.write_sectors = rd_writes;
    if (!mint_block_register(&dev)) { printf("register failed\n"); exit(2); }
}
static int all_zero(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i]) return 0; return 1; }

/* Opens the MintFS volume on a disk image directly (as the host fsck does). */
static uint64_t part_first;
static int ram_read4k(void *c, uint64_t b, void *buf) { RAMDISK *d = c; memcpy(buf, d->data + (part_first + b * 8) * 512, 4096); return 1; }
static int ram_nowrite(void *c, uint64_t b, const void *buf) { (void)c; (void)b; (void)buf; return 0; }
static int ram_noflush(void *c) { (void)c; return 1; }
static int64_t ram_now(void *c) { (void)c; return 0; }

int main(void) {
    mint_block_init();
    add_disk(&disks[0], "sda", 262144);          /* 128 MiB, has data in sector 0 */
    add_disk(&disks[1], "sdb", 65536);           /* 32 MiB: too small */
    add_disk(&disks[2], "sdc", 262144);          /* 128 MiB blank */
    memcpy(disks[0].data + 446, "\x80\x00\x02\x00\x0b", 5); disks[0].data[510] = 0x55; disks[0].data[511] = 0xAA;

    mint_mintos_disk_probe();
    MINT_MFS_VOLUME v;
    mint_mfs_volume(&v);
    CHECK(v.state == MINT_MFS_VOLUME_NONE, "no MintOS disk is mounted when none exists");
    CHECK(mint_mfs_open("/x", MINT_O_READ, 1) == -MINT_ENODEV, "file calls report ENODEV without a MintOS disk");

    /* --- initialization safety */
    uint8_t sda_before[512]; memcpy(sda_before, disks[0].data, 512);
    CHECK(mint_mintos_disk_initialize(0) == -MINT_EPERM && disks[0].writes == 0 && !memcmp(disks[0].data, sda_before, 512),
          "a disk with an MBR is refused and not written");
    disks[2].data[(262144 - 1) * 512 + 7] = 1;   /* stray byte where a backup GPT would be */
    CHECK(mint_mintos_disk_initialize(2) == -MINT_EPERM && disks[2].writes == 0, "a disk with data in the backup-GPT area is refused");
    disks[2].data[(262144 - 1) * 512 + 7] = 0;
    CHECK(mint_mintos_disk_initialize(1) == -MINT_EINVAL && disks[1].writes == 0, "disks under 64 MiB are refused");
    CHECK(mint_mintos_disk_initialize(7) == -MINT_ENODEV, "an unknown block index is refused");
    CHECK(mint_mintos_disk_initialize(2) == 0, "a blank 128 MiB disk is initialized");
    mint_mfs_volume(&v);
    CHECK(v.state == MINT_MFS_VOLUME_READY && !strcmp(v.label, "MintOS HD") && !strcmp(v.home, "/Users/mintos") &&
          !strcmp(v.device, "sdc") && v.block_index == 2, "the new volume is mounted as MintOS HD with home /Users/mintos");
    CHECK(mint_mintos_disk_initialize(0) == -MINT_EBUSY, "a second initialization is refused while a MintOS disk is mounted");

    /* --- what was written */
    uint64_t first, last;
    CHECK(mint_gpt_find_mintos(disks[2].data + 512, disks[2].data + 1024, 262144, &first, &last) && first == 2048,
          "the disk carries a valid GPT with the MintOS partition at 1 MiB");
    part_first = first;
    CHECK(all_zero(disks[2].data + 34 * 512, (2048 - 34) * 512) && all_zero(disks[2].data + (last + 1) * 512, (262144 - 33 - last - 1) * 512),
          "nothing outside the GPT areas and the partition was written");
    CHECK(disks[0].writes == 0 && disks[1].writes == 0, "other disks were never written");

    /* --- block layer rules after mount */
    static uint8_t junk[4096];
    memset(junk, 0xEE, sizeof(junk));
    uint64_t w0 = disks[2].writes;
    CHECK(!mint_block_write(2, 5000, junk), "generic block writes stay refused on the MintOS disk");
    CHECK(!mint_block_write_granted(2, 0, 1, junk, 0x4D465331u), "the MintFS grant does not cover the GPT (LBA 0)");
    CHECK(!mint_block_write_granted(2, last + 1, 1, junk, 0x4D465331u), "the MintFS grant ends at the partition's last sector");
    CHECK(!mint_block_write_granted(2, last - 3, 8, junk, 0x4D465331u), "a write crossing the partition end is refused");
    CHECK(!mint_block_write_granted(2, first, 1, junk, 0x12345678u), "another owner cannot use the MintFS grant");
    CHECK(!mint_block_grant_writes(2, 0, 34, 0x12345678u), "another owner cannot take a grant on the MintOS disk");
    CHECK(!mint_block_write_granted(0, 0, 1, junk, 0x4D465331u), "disks without a grant cannot be written");
    CHECK(disks[2].writes == w0, "refused writes reached no disk");

    /* --- layout */
    MINT_MFS_STAT st;
    static const char *const dirs[] = {"/Applications", "/Library", "/System", "/Volumes", "/Users/Shared", "/Users/mintos/Desktop",
                                       "/Users/mintos/Documents", "/Users/mintos/Downloads", "/Users/mintos/Movies", "/Users/mintos/Music",
                                       "/Users/mintos/Pictures", "/Users/mintos/.Trash"};
    int layout = 1;
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) layout &= mint_mfs_stat(dirs[i], &st) == 0 && st.type == MINT_MFS_DIR;
    CHECK(layout, "the macOS-style folder layout exists");
    CHECK(mint_mfs_stat("/Users/mintos/Documents/Welcome to MintOS.txt", &st) == 0 && st.type == MINT_MFS_FILE && st.size > 50,
          "the welcome document exists");

    /* --- file API */
    int64_t fd = mint_mfs_open("/Users/mintos/Documents/Big.bin", MINT_O_WRITE | MINT_O_CREATE, 7);
    CHECK(fd >= 0, "create a file");
    static uint8_t big[300000], back[300000];
    for (size_t i = 0; i < sizeof(big); ++i) big[i] = (uint8_t)(i * 131u + (i >> 9));
    int64_t total = 0;
    for (size_t off = 0; off < sizeof(big); off += 65536) {
        uint32_t n = sizeof(big) - off < 65536 ? (uint32_t)(sizeof(big) - off) : 65536u;
        int64_t w = mint_mfs_write(fd, big + off, n, 7);
        if (w > 0) total += w;
    }
    CHECK(total == (int64_t)sizeof(big), "write 300 KB through the descriptor API");
    CHECK(mint_mfs_read(fd, back, 10, 7) == -MINT_EBADF, "a write-only handle cannot read");
    CHECK(mint_mfs_write(fd, big, 10, 8) == -MINT_EBADF && mint_mfs_close(fd, 8) == -MINT_EBADF, "another process cannot use the handle");
    CHECK(mint_mfs_remove("/Users/mintos/Documents/Big.bin") == -MINT_EBUSY, "an open file cannot be removed");
    CHECK(mint_mfs_close(fd, 7) == 0, "close commits");
    fd = mint_mfs_open("/Users/mintos/Documents/Big.bin", MINT_O_READ, 7);
    total = 0;
    for (;;) { int64_t r = mint_mfs_read(fd, back + total, 65536, 7); if (r <= 0) break; total += r; }
    CHECK(total == (int64_t)sizeof(big) && !memcmp(big, back, sizeof(big)), "read it back intact");
    CHECK(mint_mfs_seek(fd, -100, MINT_SEEK_END, 7) == (int64_t)sizeof(big) - 100 && mint_mfs_read(fd, back, 1000, 7) == 100,
          "seek from the end and read the tail");
    CHECK(mint_mfs_seek(fd, -1, MINT_SEEK_SET, 7) == -MINT_EINVAL, "seeking before the start is refused");
    mint_mfs_close(fd, 7);
    CHECK(mint_mfs_open("/Users/mintos/Documents", MINT_O_READ, 7) == -MINT_EISDIR, "folders cannot be opened as files");
    CHECK(mint_mfs_open("/Users/mintos/Documents/Big.bin", MINT_O_WRITE | MINT_O_CREATE | MINT_O_EXCL, 7) == -MINT_EEXIST, "O_EXCL refuses an existing file");
    CHECK(mint_mfs_open("relative.txt", MINT_O_READ, 7) == -MINT_EINVAL, "relative paths are refused");
    fd = mint_mfs_open("/Users/mintos/Documents/Big.bin", MINT_O_WRITE | MINT_O_TRUNC, 7);
    CHECK(fd >= 0 && mint_mfs_close(fd, 7) == 0 && mint_mfs_stat("/Users/mintos/Documents/Big.bin", &st) == 0 && st.size == 0, "O_TRUNC empties a file");
    fd = mint_mfs_open("/Users/mintos/Documents/log.txt", MINT_O_WRITE | MINT_O_CREATE | MINT_O_APPEND, 7);
    mint_mfs_write(fd, "one\n", 4, 7); mint_mfs_seek(fd, 0, MINT_SEEK_SET, 7); mint_mfs_write(fd, "two\n", 4, 7); mint_mfs_close(fd, 7);
    fd = mint_mfs_open("/Users/mintos/Documents/log.txt", MINT_O_READ, 7);
    char txt[16] = {0}; mint_mfs_read(fd, txt, 15, 7); mint_mfs_close(fd, 7);
    CHECK(!strcmp(txt, "one\ntwo\n"), "O_APPEND always writes at the end");
    int64_t handles[40]; int opened = 0, emfile = 0;
    for (int i = 0; i < 40; ++i) { handles[i] = mint_mfs_open("/Users/mintos/Documents/log.txt", MINT_O_READ, 9); if (handles[i] >= 0) ++opened; else if (handles[i] == -MINT_EMFILE) ++emfile; }
    CHECK(opened == 32 && emfile == 8, "the handle table is bounded (32) and reports EMFILE");
    mint_mfs_close_all(9);
    CHECK(mint_mfs_read(handles[0], txt, 1, 9) == -MINT_EBADF && mint_mfs_open("/Users/mintos/Documents/log.txt", MINT_O_READ, 9) >= 0,
          "process exit closes all of its handles");
    mint_mfs_close_all(9);
    CHECK(mint_mfs_mkdir("/Users/mintos/Documents/Projects") == 0 && mint_mfs_mkdir("/Users/mintos/Documents/projects") == -MINT_EEXIST,
          "mkdir, with case-insensitive names");
    CHECK(mint_mfs_rename("/Users/mintos/Documents/log.txt", "/Users/mintos/Documents/Projects/Log.txt") == 0 &&
          mint_mfs_stat("/Users/mintos/Documents/Projects/Log.txt", &st) == 0 && mint_mfs_stat("/Users/mintos/Documents/log.txt", &st) == -MINT_ENOENT,
          "rename moves a file between folders");
    CHECK(mint_mfs_rename("/Users/mintos/Documents", "/Users/mintos/Documents/Projects/Inside") == -MINT_EINVAL, "a folder cannot move into itself");
    CHECK(mint_mfs_remove("/Users/mintos/Documents/Projects") == -MINT_ENOTEMPTY, "non-empty folders are not removed");
    static MINT_MFS_DIRENT list[MINT_MFS_LIST_MAX];
    int64_t n = mint_mfs_list("/Users/mintos/Documents", 0, list, MINT_MFS_LIST_MAX);
    int seen = 0;
    for (int64_t i = 0; i < n; ++i) seen += !strcmp(list[i].name, "Projects") && list[i].type == MINT_MFS_DIR;
    CHECK(n == 3 && seen == 1, "listing returns the folder's entries with types");
    for (int i = 0; i < 70; ++i) { char p[96]; snprintf(p, sizeof(p), "/Users/mintos/Downloads/file %02d.txt", i); fd = mint_mfs_open(p, MINT_O_WRITE | MINT_O_CREATE, 7); mint_mfs_close(fd, 7); }
    int64_t a = mint_mfs_list("/Users/mintos/Downloads", 0, list, MINT_MFS_LIST_MAX), b2 = mint_mfs_list("/Users/mintos/Downloads", 64, list, MINT_MFS_LIST_MAX);
    CHECK(a == 32 && b2 == 6 && !strcmp(list[5].name, "file 69.txt"), "batched listing pages through 70 entries");
    CHECK(mint_mfs_sync() == 0, "sync commits");

    /* --- remount from disk, then fsck */
    uint64_t gen_before = v.generation;
    mint_mfs_volume(&v);
    gen_before = v.generation;
    mint_block_revoke_writes(2, 0x4D465331u);
    mint_mintos_disk_probe();                    /* mounts again from the on-disk state */
    mint_mfs_volume(&v);
    CHECK(v.state == MINT_MFS_VOLUME_READY && v.generation >= gen_before, "the disk is found and mounted again at boot");
    CHECK(mint_mfs_stat("/Users/mintos/Documents/Projects/Log.txt", &st) == 0 && st.size == 8 &&
          mint_mfs_stat("/Users/mintos/Downloads/file 42.txt", &st) == 0, "files survive the remount");
    MINTFS_DEV dev = {&disks[2], (last - first + 1) / 8, ram_read4k, ram_nowrite, ram_noflush, ram_now};
    uint64_t wb = mintfs_check_work_bytes(dev.block_count);
    uint8_t *work = malloc((size_t)wb);
    MINTFS_CHECK_REPORT rep;
    int rc = mintfs_check(&dev, &rep, work, (size_t)wb);
    CHECK(rc == 0 && rep.leaked_blocks == 0 && rep.bad_checksums == 0, "the on-disk volume passes the MintFS checker");
    if (rc) printf("  fsck: %s (block %llu)\n", rep.first_problem, (unsigned long long)rep.problem_block);

    printf("%d checks, %d failures\n", checks, failures);
    if (!failures) printf("PASS: MintOS disk integration test\n");
    return failures ? 1 : 0;
}
