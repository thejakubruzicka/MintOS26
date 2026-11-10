/* mintfs_tool: host-side inspector for MintOS disk images (read-only).
 *
 *   mintfs_tool check <image>          GPT + MintFS consistency check (fsck)
 *   mintfs_tool ls    <image> [path]   list a folder (recursive with -r)
 *   mintfs_tool cat   <image> <path>   print a file
 *   mintfs_tool gpt   <sectors> <out>  write a blank-disk MintOS GPT image (tests)
 *
 * Build: cc -std=c11 -O2 -DMINT_GPT_HOST_TEST tools/mintfs_tool.c kernel/fs/mintfs.c kernel/fs/mintos_disk.c
 * The image is opened read-only; the tool never writes to a disk image it inspects. */
#define _POSIX_C_SOURCE 200809L          /* fseeko/ftello under -std=c11 on glibc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../kernel/include/mintfs.h"
#include "../kernel/include/mintos_fs.h"

typedef struct { FILE *f; uint64_t offset; } IMG;

static int img_read(void *ctx, uint64_t block, void *buf) {
    IMG *im = (IMG *)ctx;
    if (fseeko(im->f, (off_t)(im->offset + block * MINTFS_BLOCK), SEEK_SET)) return 0;
    return fread(buf, 1, MINTFS_BLOCK, im->f) == MINTFS_BLOCK;
}
static int img_write(void *ctx, uint64_t block, const void *buf) { (void)ctx; (void)block; (void)buf; return 0; }
static int img_flush(void *ctx) { (void)ctx; return 1; }
static int64_t img_now(void *ctx) { (void)ctx; return 0; }

static int open_image(const char *path, IMG *im, MINTFS_DEV *dev) {
    static uint8_t header[512], entries[16384];
    im->f = fopen(path, "rb");
    if (!im->f) { perror(path); return 0; }
    fseeko(im->f, 0, SEEK_END);
    uint64_t sectors = (uint64_t)ftello(im->f) / 512u;
    if (fseeko(im->f, 512, SEEK_SET) || fread(header, 1, 512, im->f) != 512 ||
        fseeko(im->f, 1024, SEEK_SET) || fread(entries, 1, sizeof(entries), im->f) != sizeof(entries)) {
        fprintf(stderr, "%s: too small for a GPT\n", path); return 0;
    }
    uint64_t first, last;
    if (!mint_gpt_find_mintos(header, entries, sectors, &first, &last)) {
        fprintf(stderr, "%s: no valid GPT with a MintOS partition\n", path); return 0;
    }
    im->offset = first * 512u;
    *dev = (MINTFS_DEV){im, (last - first + 1u) / 8u, img_read, img_write, img_flush, img_now};
    printf("GPT: MintOS partition LBA %llu-%llu (%llu MiB)\n", (unsigned long long)first, (unsigned long long)last,
           (unsigned long long)((last - first + 1u) / 2048u));
    return 1;
}

static MINTFS fs;
static int mount_image(const char *path, IMG *im) {
    MINTFS_DEV dev;
    if (!open_image(path, im, &dev)) return 0;
    uint64_t bytes = mintfs_mount_bytes(dev.block_count);
    uint8_t *bm = malloc((size_t)bytes);
    int rc = bm ? mintfs_mount(&fs, &dev, bm, (size_t)bytes, 1) : MINTFS_ENOMEM;
    if (rc) { fprintf(stderr, "mount failed: %s\n", mintfs_strerror(rc)); return 0; }
    return 1;
}

static void list_dir(uint64_t ino, const char *path, int recursive, int depth) {
    MINTFS_DIRENT de;
    for (uint32_t i = 0; mintfs_readdir(&fs, ino, i, &de) == 1; ++i) {
        MINTFS_STAT st;
        if (mintfs_stat(&fs, de.ino, &st)) continue;
        printf("%*s%s%s  %llu bytes  mtime=%lld\n", depth * 2, "", de.name, de.type == MINTFS_TYPE_DIR ? "/" : "",
               (unsigned long long)st.size, (long long)st.mtime);
        if (recursive && de.type == MINTFS_TYPE_DIR) {
            char sub[1100];
            snprintf(sub, sizeof(sub), "%s/%s", strcmp(path, "/") ? path : "", de.name);
            list_dir(de.ino, sub, 1, depth + 1);
        }
    }
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "gpt")) {
        uint64_t n = strtoull(argv[2], 0, 0);
        static uint8_t mbr[512], hdr[512], ent[16384], bak[512];
        uint8_t dg[16] = {1, 2, 3, 4, 5, 6, 0x47, 8, 0x89, 10, 11, 12, 13, 14, 15, 16}, pg[16] = {9, 9, 9, 9, 9, 9, 0x49, 9, 0x99, 9, 9, 9, 9, 9, 9, 9};
        uint64_t first, last;
        if (!mint_gpt_build(n, dg, pg, mbr, hdr, ent, bak, &first, &last)) { fprintf(stderr, "disk too small\n"); return 1; }
        FILE *o = fopen(argv[3], "wb");
        if (!o) { perror(argv[3]); return 1; }
        /* Only the GPT areas are written; the rest of the (sparse) file reads as zeros. */
        int ok = fwrite(mbr, 1, 512, o) == 512 && fwrite(hdr, 1, 512, o) == 512 && fwrite(ent, 1, sizeof(ent), o) == sizeof(ent) &&
                 !fseeko(o, (off_t)((n - 33u) * 512u), SEEK_SET) && fwrite(ent, 1, sizeof(ent), o) == sizeof(ent) &&
                 fwrite(bak, 1, 512, o) == 512;
        if (fclose(o) || !ok) { fprintf(stderr, "%s: write failed\n", argv[3]); return 1; }
        printf("%llu %llu\n", (unsigned long long)first, (unsigned long long)last);
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "usage: mintfs_tool check|ls|cat <image> [path] [-r]\n"); return 2; }
    IMG im;
    if (!strcmp(argv[1], "check")) {
        MINTFS_DEV dev;
        if (!open_image(argv[2], &im, &dev)) return 1;
        uint64_t wb = mintfs_check_work_bytes(dev.block_count);
        uint8_t *work = malloc((size_t)wb);
        MINTFS_CHECK_REPORT r;
        int rc = mintfs_check(&dev, &r, work, (size_t)wb);
        printf("fsck: %s\n  inodes=%llu dirs=%llu files=%llu reachable=%llu marked=%llu\n"
               "  bad_checksums=%u bad_pointers=%u double_refs=%u leaked=%u missing=%u bad_dirs=%u bad_links=%u\n",
               rc ? "PROBLEMS" : "clean", (unsigned long long)r.inodes, (unsigned long long)r.directories,
               (unsigned long long)r.files, (unsigned long long)r.blocks_reachable, (unsigned long long)r.blocks_marked_used,
               r.bad_checksums, r.bad_pointers, r.double_references, r.leaked_blocks, r.missing_blocks,
               r.bad_directories, r.bad_links);
        if (rc) printf("  first problem: %s (block %llu)\n", r.first_problem, (unsigned long long)r.problem_block);
        if (!mount_image(argv[2], &im)) return 1;
        MINTFS_INFO info; mintfs_info(&fs, &info);
        printf("volume \"%s\" generation %llu, %llu of %llu blocks free\n", info.label, (unsigned long long)info.generation,
               (unsigned long long)info.free_blocks, (unsigned long long)info.block_count);
        return rc ? 1 : 0;
    }
    if (!mount_image(argv[2], &im)) return 1;
    const char *path = argc > 3 && argv[3][0] == '/' ? argv[3] : "/";
    uint64_t ino;
    int rc = mintfs_resolve(&fs, path, &ino);
    if (rc) { fprintf(stderr, "%s: %s\n", path, mintfs_strerror(rc)); return 1; }
    if (!strcmp(argv[1], "ls")) {
        int rec = argc > 3 && !strcmp(argv[argc - 1], "-r");
        list_dir(ino, path, rec, 0);
        return 0;
    }
    if (!strcmp(argv[1], "cat")) {
        static uint8_t buf[65536];
        uint64_t off = 0;
        for (;;) {
            int64_t n = mintfs_read(&fs, ino, off, buf, sizeof(buf));
            if (n <= 0) { if (n < 0) fprintf(stderr, "read: %s\n", mintfs_strerror((int)n)); break; }
            fwrite(buf, 1, (size_t)n, stdout);
            off += (uint64_t)n;
        }
        return 0;
    }
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
