/* MintOS disks: GPT with one MintFS partition, created only on verified blank
 * disks after explicit confirmation; mounted at boot as "/" ("MintOS HD").
 * Writes go exclusively through a block-layer write grant limited to the
 * MintOS partition. Also the descriptor-based file API used by the syscalls. */
#include <stdint.h>
#include <stddef.h>
#include "../include/mintos_fs.h"
#include "../include/mintfs.h"
#ifndef MINT_GPT_HOST_TEST
#include "../include/block.h"
#include "../include/physmem.h"
#include "../include/serial.h"
#include "../include/log.h"
#include "../include/rtc.h"
#include "../include/arch.h"
#include "../include/task.h"
#include "../include/uapi/mintos/syscall.h"
#endif

/* GPT partition type for MintFS (on-disk byte order). */
const uint8_t MINT_GPT_MINTFS_TYPE[16] = {0x74, 0x4D, 0x69, 0x6E, 0x4F, 0x53, 0x44, 0x46, 0x8A, 0x3B, 0x9C, 0x2E, 0x5F, 0x71, 0xD0, 0xA4};

static void z(void *p, size_t n) { uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0; }
static void cp(void *d, const void *s, size_t n) { uint8_t *o = (uint8_t *)d; const uint8_t *i = (const uint8_t *)s; while (n--) *o++ = *i++; }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t r64(const uint8_t *p) { return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32); }

uint32_t mint_gpt_crc32(uint32_t crc, const void *data, size_t len) {
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1; table[i] = c; }
        ready = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) crc = table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

static void gpt_header(uint8_t h[512], uint64_t current, uint64_t backup, uint64_t first_usable, uint64_t last_usable,
                       const uint8_t guid[16], uint64_t entries_lba, uint32_t entries_crc) {
    z(h, 512);
    cp(h, "EFI PART", 8);
    w32(h + 8, 0x00010000u); w32(h + 12, 92u);
    w64(h + 24, current); w64(h + 32, backup); w64(h + 40, first_usable); w64(h + 48, last_usable);
    cp(h + 56, guid, 16);
    w64(h + 72, entries_lba); w32(h + 80, 128u); w32(h + 84, 128u); w32(h + 88, entries_crc);
    w32(h + 16, mint_gpt_crc32(0, h, 92));
}

int mint_gpt_build(uint64_t n, const uint8_t disk_guid[16], const uint8_t part_guid[16],
                   uint8_t mbr[512], uint8_t header[512], uint8_t entries[16384], uint8_t backup[512],
                   uint64_t *part_first, uint64_t *part_last) {
    if (n < 2048u + 64u * 8u + 34u) return 0;
    uint64_t first = 2048u, last = n - 34u;
    uint64_t sectors = last - first + 1u;
    uint64_t max_sectors = (uint64_t)MINTFS_MAX_BLOCKS * 8u;
    if (sectors > max_sectors) sectors = max_sectors;
    sectors &= ~(uint64_t)7u;                              /* whole 4 KiB blocks */
    last = first + sectors - 1u;
    z(mbr, 512);                                            /* protective MBR */
    mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00;
    mbr[446 + 4] = 0xEE; mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    w32(mbr + 446 + 8, 1u); w32(mbr + 446 + 12, n - 1u > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(n - 1u));
    mbr[510] = 0x55; mbr[511] = 0xAA;
    z(entries, 16384);
    cp(entries, MINT_GPT_MINTFS_TYPE, 16);
    cp(entries + 16, part_guid, 16);
    w64(entries + 32, first); w64(entries + 40, last); w64(entries + 48, 0);
    static const char name[] = "MintOS HD";
    for (uint32_t i = 0; name[i]; ++i) { entries[56 + 2 * i] = (uint8_t)name[i]; entries[57 + 2 * i] = 0; }
    uint32_t ecrc = mint_gpt_crc32(0, entries, 16384);
    gpt_header(header, 1, n - 1u, 34, n - 34u, disk_guid, 2, ecrc);
    gpt_header(backup, n - 1u, 1, 34, n - 34u, disk_guid, n - 33u, ecrc);
    *part_first = first; *part_last = last;
    return 1;
}

int mint_gpt_find_mintos(const uint8_t h[512], const uint8_t e[16384], uint64_t n, uint64_t *first, uint64_t *last) {
    uint8_t copy[92];
    if (r64(h) != 0x5452415020494645ull || r32(h + 12) != 92u) return 0;      /* "EFI PART" */
    cp(copy, h, 92); w32(copy + 16, 0);
    if (mint_gpt_crc32(0, copy, 92) != r32(h + 16)) return 0;
    if (r64(h + 24) != 1u || r32(h + 80) != 128u || r32(h + 84) != 128u || r64(h + 72) != 2u) return 0;
    if (mint_gpt_crc32(0, e, 16384) != r32(h + 88)) return 0;
    for (uint32_t i = 0; i < 128u; ++i) {
        const uint8_t *p = e + 128u * i;
        int match = 1;
        for (int k = 0; k < 16; ++k) if (p[k] != MINT_GPT_MINTFS_TYPE[k]) match = 0;
        if (!match) continue;
        uint64_t f = r64(p + 32), l = r64(p + 40);
        if (f < 34u || l < f || l >= n || ((l - f + 1u) & 7u)) return 0;
        *first = f; *last = l;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ accounts (pure, host-tested) */
int mint_account_name_valid(const char *name) {
    static const char *const reserved[] = {"root", "daemon", "nobody", "guest", "shared", "system", "wheel"};
    if (!name || !(name[0] >= 'a' && name[0] <= 'z')) return 0;
    for (size_t n = 0; name[n]; ++n) {
        char c = name[n];
        if (n >= MINT_ACCOUNT_NAME_MAX) return 0;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    for (size_t r = 0; r < sizeof(reserved) / sizeof(reserved[0]); ++r) {
        size_t i = 0;
        while (reserved[r][i] && reserved[r][i] == name[i]) ++i;
        if (!reserved[r][i] && !name[i]) return 0;
    }
    return 1;
}
static int blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }
int mint_account_home_from_text(const char *t, size_t len, char *home, size_t cap) {
    static const char key[] = "home", prefix[] = "\"/Users/";
    for (size_t i = 0; i < len;) {
        size_t e = i;
        while (e < len && t[e] != '\n') ++e;
        size_t q = i, k = 0;
        while (q < e && blank(t[q])) ++q;
        while (key[k] && q < e && t[q] == key[k]) { ++q; ++k; }
        if (!key[k]) {
            while (q < e && blank(t[q])) ++q;
            if (q < e && t[q] == '=') {
                ++q;
                while (q < e && blank(t[q])) ++q;
                size_t p = 0;
                while (prefix[p] && q + p < e && t[q + p] == prefix[p]) ++p;
                if (prefix[p]) return 0;
                char name[MINT_ACCOUNT_NAME_MAX + 2];
                size_t a = q + p, nl = 0;
                while (a < e && t[a] != '"' && nl <= MINT_ACCOUNT_NAME_MAX) name[nl++] = t[a++];
                if (a >= e || t[a] != '"' || nl > MINT_ACCOUNT_NAME_MAX) return 0;
                name[nl] = '\0';
                if (!mint_account_name_valid(name) || 7u + nl + 1u > cap) return 0;
                for (size_t u = 0; u < 7u; ++u) home[u] = prefix[1u + u];
                for (size_t u = 0; u <= nl; ++u) home[7u + u] = name[u];
                return 1;
            }
        }
        i = e + 1u;
    }
    return 0;
}

#ifndef MINT_GPT_HOST_TEST
/* ------------------------------------------------------------------ device glue */
#define GRANT_OWNER 0x4D465331u   /* "MFS1" */
typedef struct mdisk { uint32_t index; uint64_t first; } MDISK;
static MDISK disk;
static MINTFS fs;
static MINT_MFS_VOLUME volume;
static uint64_t last_commit_tick;

static int dev_read(void *ctx, uint64_t block, void *buf) {
    MDISK *d = (MDISK *)ctx;
    return mint_block_read_sectors(d->index, d->first + block * 8u, 8u, (uint8_t *)buf);
}
static int dev_write(void *ctx, uint64_t block, const void *buf) {
    MDISK *d = (MDISK *)ctx;
    return mint_block_write_granted(d->index, d->first + block * 8u, 8u, (const uint8_t *)buf, GRANT_OWNER);
}
static int dev_flush(void *ctx) { MDISK *d = (MDISK *)ctx; return mint_block_flush_granted(d->index, GRANT_OWNER); }
static int64_t dev_now(void *ctx) {
    (void)ctx;
    int64_t epoch = mint_rtc_boot_epoch();
    return epoch ? epoch + (int64_t)(mint_timer_ticks() / 100u) : 0;
}
static void set_status(const char *s) {
    size_t i = 0;
    for (; s && s[i] && i + 1u < sizeof(volume.status); ++i) volume.status[i] = s[i];
    volume.status[i] = '\0';
}
static int to_errno(int rc) { return rc == MINTFS_ECORRUPT ? -MINT_EIO : rc; }   /* MintFS codes match MINT_E* */

static int mount_partition(uint32_t index, uint64_t first, uint64_t last) {
    if (!mint_block_grant_writes(index, first, last - first + 1u, GRANT_OWNER)) return 0;
    disk.index = index; disk.first = first;
    MINTFS_DEV dev = {&disk, (last - first + 1u) / 8u, dev_read, dev_write, dev_flush, dev_now};
    uint64_t bytes = mintfs_mount_bytes(dev.block_count), pages = (bytes + 4095u) / 4096u;
    uint64_t mem = mint_physmem_claim_contiguous_below(pages, 0x100000000ull);
    if (!mem) { mint_block_revoke_writes(index, GRANT_OWNER); set_status("Not enough memory to mount the MintOS disk"); return 0; }
    int rc = mintfs_mount(&fs, &dev, (uint8_t *)(uintptr_t)mem, (size_t)(pages * 4096u), 0);
    if (rc) {
        (void)mint_physmem_release_contiguous(mem, pages);
        mint_block_revoke_writes(index, GRANT_OWNER);
        volume.state = MINT_MFS_VOLUME_FAILED;
        set_status(rc == MINTFS_ECORRUPT ? "The MintOS disk is damaged and was not mounted" : "The MintOS disk could not be read");
        return 0;
    }
    const MINT_BLOCK_DEVICE *bd = mint_block_get(index);
    MINTFS_INFO info;
    mintfs_info(&fs, &info);
    volume.version = MINT_MFS_ABI_VERSION;
    volume.state = MINT_MFS_VOLUME_READY;
    volume.block_index = index;
    for (int i = 0; i < 32; ++i) volume.label[i] = info.label[i];
    for (int i = 0; bd && bd->name[i] && i < 15; ++i) volume.device[i] = bd->name[i];
    static const char home[] = "/Users/" MINT_MFS_HOME_NAME;
    for (size_t i = 0; i < sizeof(home); ++i) volume.home[i] = home[i];
    set_status("Mounted read/write");
    last_commit_tick = mint_timer_ticks();
    mint_serial_write("[mintfs] mounted \"");
    mint_serial_write(volume.label);
    mint_serial_write("\" from ");
    mint_serial_write(volume.device);
    mint_serial_write(" read/write, generation ");
    mint_serial_dec64(info.generation);
    mint_serial_write(", ");
    mint_serial_dec64(info.free_blocks * 4u / 1024u);
    mint_serial_write(" MiB free\n");
    mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_MINTFS, "MintOS disk mounted read/write as /");
    return 1;
}

static void set_home_text(const char *home) {
    size_t i = 0;
    for (; home[i] && i + 1u < sizeof(volume.home); ++i) volume.home[i] = home[i];
    volume.home[i] = '\0';
}
static int is_directory(const char *path) {
    uint64_t ino;
    MINTFS_STAT st;
    return !mintfs_resolve(&fs, path, &ino) && !mintfs_stat(&fs, ino, &st) && st.type == MINTFS_TYPE_DIR;
}
/* The account's home replaces the default /Users/mintos once the Setup
 * Assistant has written /Library/Preferences/com.mintos.accounts. */
static void load_account_home(void) {
    static char text[4096];
    char home[64];
    uint64_t ino;
    if (mintfs_resolve(&fs, MINT_ACCOUNTS_PATH, &ino)) return;
    int64_t n = mintfs_read(&fs, ino, 0, text, sizeof(text) - 1u);
    if (n <= 0) return;
    if (!mint_account_home_from_text(text, (size_t)n, home, sizeof(home)) || !is_directory(home)) {
        mint_serial_write("[mintfs] accounts file has no usable home; keeping the default home folder\n");
        mint_log_emit(MINT_LOG_WARN, MINT_LOG_COMPONENT_PREFS, "accounts file ignored: no valid existing home folder");
        return;
    }
    set_home_text(home);
    mint_serial_write("[mintfs] home folder ");
    mint_serial_write(volume.home);
    mint_serial_write(" (from the MintOS account)\n");
    mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_PREFS, "home folder set from the MintOS account");
}

static int read_gpt(uint32_t index, uint64_t n, uint64_t *first, uint64_t *last) {
    static uint8_t header[512], entries[16384];
    if (!mint_block_read(index, 1, header)) return 0;
    for (uint32_t s = 0; s < 32u; s += 8u) if (!mint_block_read_sectors(index, 2u + s, 8u, entries + 512u * s)) return 0;
    return mint_gpt_find_mintos(header, entries, n, first, last);
}

void mint_mintos_disk_probe(void) {
    z(&volume, sizeof(volume));
    volume.version = MINT_MFS_ABI_VERSION;
    set_status("No MintOS disk");
    for (uint32_t i = 0; i < mint_block_count(); ++i) {
        const MINT_BLOCK_DEVICE *bd = mint_block_get(i);
        uint64_t first, last;
        if (!bd || !read_gpt(i, bd->sector_count, &first, &last)) continue;
        mint_serial_write("[mintfs] MintOS partition found on ");
        mint_serial_write(bd->name);
        mint_serial_write("\n");
        if (mount_partition(i, first, last)) { load_account_home(); return; }
    }
    mint_serial_write("[mintfs] no MintOS disk; MintOS HD is unavailable (Disk Utility can initialize a blank disk)\n");
}

/* ------------------------------------------------------------------ initialization of a blank disk */
static int sector_zero(const uint8_t *s) { for (int i = 0; i < 512; ++i) if (s[i]) return 0; return 1; }
static void random_guid(uint8_t g[16], uint32_t salt) {
    uint64_t t = mint_timer_ticks() * 0x9E3779B97F4A7C15ull ^ ((uint64_t)salt << 17);
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    t ^= ((uint64_t)hi << 32) | lo;
    for (int i = 0; i < 16; ++i) { t ^= t << 13; t ^= t >> 7; t ^= t << 17; g[i] = (uint8_t)t; }
    g[6] = (uint8_t)((g[6] & 0x0Fu) | 0x40u); g[8] = (uint8_t)((g[8] & 0x3Fu) | 0x80u);
}
static int mkdirs(uint64_t parent, const char *name, uint64_t *out) {
    int rc = mintfs_create(&fs, parent, name, MINTFS_TYPE_DIR, out);
    return rc == MINTFS_EEXIST ? mintfs_lookup(&fs, parent, name, out) : rc;
}

int mint_mintos_disk_initialize(uint32_t index) {
    const MINT_BLOCK_DEVICE *bd = mint_block_get(index);
    if (!bd || !bd->write_sector || !bd->flush) return -MINT_ENODEV;
    if (volume.state == MINT_MFS_VOLUME_READY) return -MINT_EBUSY;       /* one MintOS disk at a time */
    uint64_t n = bd->sector_count;
    if (n < 131072u) return -MINT_EINVAL;                              /* at least 64 MiB */
    /* The disk must be blank: no MBR/GPT at the start, no backup GPT at the end. */
    static uint8_t s[512];
    const uint64_t probe[] = {0, 1, 2, 33, n - 33u, n - 1u};
    for (uint32_t i = 0; i < 6u; ++i) {
        if (!mint_block_read(index, probe[i], s)) return -MINT_EIO;
        if (!sector_zero(s)) return -MINT_EPERM;
    }
    static uint8_t mbr[512], header[512], entries[16384], backup[512];
    uint8_t disk_guid[16], part_guid[16];
    random_guid(disk_guid, 1); random_guid(part_guid, 2);
    uint64_t first, last;
    if (!mint_gpt_build(n, disk_guid, part_guid, mbr, header, entries, backup, &first, &last)) return -MINT_EINVAL;
    /* GPT areas plus the partition; nothing else on the disk may be written. */
    if (!mint_block_grant_writes(index, 0, n, GRANT_OWNER)) return -MINT_EBUSY;
    int ok = mint_block_write_granted(index, 0, 1, mbr, GRANT_OWNER) && mint_block_write_granted(index, 1, 1, header, GRANT_OWNER);
    for (uint32_t i = 0; ok && i < 32u; i += 8u) ok = mint_block_write_granted(index, 2u + i, 8u, entries + 512u * i, GRANT_OWNER);
    for (uint32_t i = 0; ok && i < 32u; i += 8u) ok = mint_block_write_granted(index, n - 33u + i, 8u, entries + 512u * i, GRANT_OWNER);
    ok = ok && mint_block_write_granted(index, n - 1u, 1, backup, GRANT_OWNER) && mint_block_flush_granted(index, GRANT_OWNER);
    if (!ok) { mint_block_revoke_writes(index, GRANT_OWNER); return -MINT_EIO; }
    disk.index = index; disk.first = first;
    MINTFS_DEV dev = {&disk, (last - first + 1u) / 8u, dev_read, dev_write, dev_flush, dev_now};
    int rc = mintfs_format(&dev, "MintOS HD", part_guid);
    mint_block_revoke_writes(index, GRANT_OWNER);
    if (rc) return to_errno(rc);
    if (!mount_partition(index, first, last)) return -MINT_EIO;
    /* macOS-style folder layout */
    uint64_t users, home, d;
    static const char *const top[] = {"Applications", "Library", "System", "Volumes"};
    static const char *const in_home[] = {"Desktop", "Documents", "Downloads", "Movies", "Music", "Pictures", ".Trash"};
    for (uint32_t i = 0; i < 4u; ++i) if ((rc = mkdirs(MINTFS_ROOT_INO, top[i], &d))) return to_errno(rc);
    if ((rc = mkdirs(MINTFS_ROOT_INO, "Users", &users)) || (rc = mkdirs(users, "Shared", &d)) || (rc = mkdirs(users, MINT_MFS_HOME_NAME, &home)))
        return to_errno(rc);
    for (uint32_t i = 0; i < 7u; ++i) if ((rc = mkdirs(home, in_home[i], &d))) return to_errno(rc);
    uint64_t docs, welcome;
    if (!mintfs_lookup(&fs, home, "Documents", &docs) && !mintfs_create(&fs, docs, "Welcome to MintOS.txt", MINTFS_TYPE_FILE, &welcome)) {
        static const char text[] =
            "Welcome to MintOS!\n\nThis is your MintOS disk. Files you save here stay after a restart.\n"
            "MintFS writes changes copy-on-write, so a power cut never leaves a half-written folder behind.\n";
        (void)mintfs_write(&fs, welcome, 0, text, sizeof(text) - 1u);
    }
    rc = mintfs_commit(&fs);
    mint_serial_write("[mintfs] initialized a MintOS disk with the standard folder layout\n");
    mint_log_emit(MINT_LOG_WARN, MINT_LOG_COMPONENT_MINTFS, "blank disk initialized as a MintOS disk (GPT + MintFS)");
    return to_errno(rc);
}

/* ------------------------------------------------------------------ descriptors */
#define FD_MAX 32u
typedef struct fdesc { uint8_t used, dirty; uint32_t flags; uint64_t pid, ino, pos; } FDESC;
static FDESC fds[FD_MAX];

static int ready(void) { return volume.state == MINT_MFS_VOLUME_READY && fs.mounted && !fs.failed; }
/* Splits "/a/b/name" into the parent inode and the leaf name. */
static int split(const char *path, uint64_t *parent, char leaf[MINT_MFS_NAME_MAX + 1]) {
    size_t n = 0, slash = 0;
    while (path[n]) { if (path[n] == '/') slash = n; if (++n >= MINT_MFS_PATH_MAX) return -MINT_ENAMETOOLONG; }
    if (path[0] != '/' || n < 2u || path[n - 1u] == '/') return -MINT_EINVAL;
    size_t ll = n - slash - 1u;
    if (ll > MINT_MFS_NAME_MAX) return -MINT_ENAMETOOLONG;
    for (size_t i = 0; i < ll; ++i) leaf[i] = path[slash + 1u + i];
    leaf[ll] = '\0';
    static char dir[MINT_MFS_PATH_MAX];
    for (size_t i = 0; i < slash; ++i) dir[i] = path[i];
    dir[slash] = '\0';
    if (!slash) { dir[0] = '/'; dir[1] = '\0'; }
    int rc = mintfs_resolve(&fs, dir, parent);
    return rc ? to_errno(rc) : 0;
}
/* Commits pending work, runs one namespace change, then commits or rolls back. */
static int64_t finish(int rc) {
    if (rc) { (void)mintfs_abort(&fs); return to_errno(rc); }
    rc = mintfs_commit(&fs);
    last_commit_tick = mint_timer_ticks();
    return rc ? to_errno(rc) : 0;
}

int64_t mint_mfs_open(const char *path, uint32_t flags, uint64_t pid) {
    if (!ready()) return -MINT_ENODEV;
    if (!(flags & (MINT_O_READ | MINT_O_WRITE))) return -MINT_EINVAL;
    uint32_t slot = FD_MAX;
    for (uint32_t i = 0; i < FD_MAX; ++i) if (!fds[i].used) { slot = i; break; }
    if (slot == FD_MAX) return -MINT_EMFILE;
    uint64_t parent, ino;
    static char leaf[MINT_MFS_NAME_MAX + 1];
    int rc = split(path, &parent, leaf);
    if (rc) return rc;
    rc = mintfs_lookup(&fs, parent, leaf, &ino);
    if (rc == MINTFS_ENOENT && (flags & MINT_O_CREATE)) {
        (void)mintfs_commit(&fs);
        rc = mintfs_create(&fs, parent, leaf, MINTFS_TYPE_FILE, &ino);
        int64_t f = finish(rc);
        if (f) return f;
    } else if (rc) return to_errno(rc);
    else if ((flags & MINT_O_CREATE) && (flags & MINT_O_EXCL)) return -MINT_EEXIST;
    MINTFS_STAT st;
    if ((rc = mintfs_stat(&fs, ino, &st))) return to_errno(rc);
    if (st.type == MINTFS_TYPE_DIR) return -MINT_EISDIR;
    if ((flags & MINT_O_TRUNC) && (flags & MINT_O_WRITE) && st.size) {
        if ((rc = mintfs_truncate(&fs, ino, 0))) return finish(rc);
        if (finish(0)) return -MINT_EIO;
    }
    fds[slot].used = 1; fds[slot].dirty = 0; fds[slot].flags = flags; fds[slot].pid = pid; fds[slot].ino = ino;
    fds[slot].pos = 0;
    return (int64_t)slot;
}
static FDESC *get_fd(int64_t fd, uint64_t pid) {
    if (fd < 0 || fd >= (int64_t)FD_MAX || !fds[fd].used || fds[fd].pid != pid) return 0;
    return &fds[fd];
}
int64_t mint_mfs_read(int64_t fd, void *buf, uint32_t len, uint64_t pid) {
    FDESC *f = get_fd(fd, pid);
    if (!f || !(f->flags & MINT_O_READ)) return -MINT_EBADF;
    if (!ready()) return -MINT_EIO;
    int64_t got = mintfs_read(&fs, f->ino, f->pos, buf, len);
    if (got > 0) f->pos += (uint64_t)got;
    return got < 0 ? to_errno((int)got) : got;
}
int64_t mint_mfs_write(int64_t fd, const void *buf, uint32_t len, uint64_t pid) {
    FDESC *f = get_fd(fd, pid);
    if (!f || !(f->flags & MINT_O_WRITE)) return -MINT_EBADF;
    if (!ready()) return -MINT_EIO;
    if (f->flags & MINT_O_APPEND) {
        MINTFS_STAT st;
        if (mintfs_stat(&fs, f->ino, &st)) return -MINT_ENOENT;
        f->pos = st.size;
    }
    uint32_t done = 0;
    while (done < len) {
        int64_t w = mintfs_write(&fs, f->ino, f->pos, (const uint8_t *)buf + done, len - done);
        if (w < 0) { if (!done) return to_errno((int)w); break; }
        if (w == 0) { if (!done) return -MINT_EIO; break; }          /* no progress: never spin */
        done += (uint32_t)w; f->pos += (uint64_t)w; f->dirty = 1;
        if (done < len && mintfs_commit(&fs)) return done ? (int64_t)done : -MINT_EIO;
    }
    return done;
}
int64_t mint_mfs_seek(int64_t fd, int64_t offset, uint32_t whence, uint64_t pid) {
    FDESC *f = get_fd(fd, pid);
    if (!f) return -MINT_EBADF;
    MINTFS_STAT st;
    if (mintfs_stat(&fs, f->ino, &st)) return -MINT_ENOENT;
    int64_t base = whence == MINT_SEEK_SET ? 0 : whence == MINT_SEEK_CUR ? (int64_t)f->pos : whence == MINT_SEEK_END ? (int64_t)st.size : -1;
    if (base < 0 || base + offset < 0) return -MINT_EINVAL;
    f->pos = (uint64_t)(base + offset);
    return (int64_t)f->pos;
}
int64_t mint_mfs_close(int64_t fd, uint64_t pid) {
    FDESC *f = get_fd(fd, pid);
    if (!f) return -MINT_EBADF;
    int dirty = f->dirty;
    f->used = 0;
    if (dirty && ready()) { int rc = mintfs_commit(&fs); last_commit_tick = mint_timer_ticks(); if (rc) return to_errno(rc); }
    return 0;
}
void mint_mfs_close_all(uint64_t pid) {
    for (uint32_t i = 0; i < FD_MAX; ++i) if (fds[i].used && fds[i].pid == pid) (void)mint_mfs_close((int64_t)i, pid);
}
int64_t mint_mfs_stat(const char *path, MINT_MFS_STAT *out) {
    if (!ready()) return -MINT_ENODEV;
    uint64_t ino;
    int rc = mintfs_resolve(&fs, path, &ino);
    if (rc) return to_errno(rc);
    MINTFS_STAT st;
    if ((rc = mintfs_stat(&fs, ino, &st))) return to_errno(rc);
    out->ino = st.ino; out->size = st.size; out->type = st.type; out->mode = st.mode;
    out->btime = st.btime; out->mtime = st.mtime; out->ctime = st.ctime;
    return 0;
}
int64_t mint_mfs_list(const char *path, uint32_t start, MINT_MFS_DIRENT *out, uint32_t capacity) {
    if (!ready()) return -MINT_ENODEV;
    if (capacity > MINT_MFS_LIST_MAX) capacity = MINT_MFS_LIST_MAX;
    uint64_t ino;
    int rc = mintfs_resolve(&fs, path, &ino);
    if (rc) return to_errno(rc);
    static MINTFS_DIRENT de[MINT_MFS_LIST_MAX];
    int n = mintfs_list(&fs, ino, start, de, capacity);
    if (n < 0) return to_errno(n);
    for (int k = 0; k < n; ++k) {
        MINTFS_STAT st;
        z(&out[k], sizeof(out[k]));
        out[k].type = de[k].type;
        if (!mintfs_stat(&fs, de[k].ino, &st)) { out[k].size = st.size; out[k].mtime = st.mtime; }
        for (size_t i = 0; i <= MINT_MFS_NAME_MAX && de[k].name[i]; ++i) out[k].name[i] = de[k].name[i];
    }
    return n;
}
int64_t mint_mfs_mkdir(const char *path) {
    if (!ready()) return -MINT_ENODEV;
    uint64_t parent, ino;
    static char leaf[MINT_MFS_NAME_MAX + 1];
    int rc = split(path, &parent, leaf);
    if (rc) return rc;
    (void)mintfs_commit(&fs);
    return finish(mintfs_create(&fs, parent, leaf, MINTFS_TYPE_DIR, &ino));
}
int64_t mint_mfs_rename(const char *from, const char *to) {
    if (!ready()) return -MINT_ENODEV;
    uint64_t sp, dp;
    static char sl[MINT_MFS_NAME_MAX + 1], dl[MINT_MFS_NAME_MAX + 1];
    int rc = split(from, &sp, sl);
    if (rc || (rc = split(to, &dp, dl))) return rc;
    (void)mintfs_commit(&fs);
    return finish(mintfs_rename(&fs, sp, sl, dp, dl));
}
int64_t mint_mfs_remove(const char *path) {
    if (!ready()) return -MINT_ENODEV;
    uint64_t parent, ino;
    static char leaf[MINT_MFS_NAME_MAX + 1];
    int rc = split(path, &parent, leaf);
    if (rc) return rc;
    if (!mintfs_lookup(&fs, parent, leaf, &ino))
        for (uint32_t i = 0; i < FD_MAX; ++i) if (fds[i].used && fds[i].ino == ino) return -MINT_EBUSY;   /* open elsewhere */
    (void)mintfs_commit(&fs);
    return finish(mintfs_unlink(&fs, parent, leaf));
}
int64_t mint_mfs_sync(void) {
    if (!ready()) return -MINT_ENODEV;
    int rc = mintfs_commit(&fs);
    last_commit_tick = mint_timer_ticks();
    return rc ? to_errno(rc) : 0;
}
int64_t mint_mfs_set_home(const char *path) {
    if (!ready()) return -MINT_ENODEV;
    static const char users[] = "/Users/";
    size_t i = 0;
    while (users[i] && path[i] == users[i]) ++i;
    if (users[i] || !mint_account_name_valid(path + i)) return -MINT_EINVAL;
    if (!is_directory(path)) return -MINT_ENOENT;
    set_home_text(path);
    mint_serial_write("[mintfs] home folder is now ");
    mint_serial_write(volume.home);
    mint_serial_write("\n");
    mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_PREFS, "home folder changed by the Setup Assistant");
    return 0;
}
/* Power-off/restart: commit the open transaction and flush the disk cache. */
int mint_mfs_flush_for_power(void) {
    if (!ready()) return 0;
    int rc = mintfs_commit(&fs);
    last_commit_tick = mint_timer_ticks();
    mint_serial_write(rc ? "[mintfs] final commit before power-off FAILED\n" : "[mintfs] final commit before power-off: clean\n");
    return rc ? to_errno(rc) : 0;
}
int mint_mfs_volume(MINT_MFS_VOLUME *out) {
    if (!out) return 0;
    if (ready()) {
        MINTFS_INFO info;
        mintfs_info(&fs, &info);
        volume.total_bytes = info.block_count * 4096u;
        volume.free_bytes = info.free_blocks * 4096u;
        volume.generation = info.generation;
        volume.items = info.inodes;
    } else if (volume.state == MINT_MFS_VOLUME_READY && fs.failed) {
        volume.state = MINT_MFS_VOLUME_FAILED;
        set_status("A disk error stopped writes; restart to recover");
    }
    *out = volume;
    return 1;
}
/* 26.5.014.0: stat an open handle (fstat for the Linux-compat layer). */
int64_t mint_mfs_fstat(int64_t fd, uint64_t pid, MINT_MFS_STAT *out) {
    FDESC *f = get_fd(fd, pid);
    if (!f || !out) return -MINT_EBADF;
    if (!ready()) return -MINT_EIO;
    MINTFS_STAT st;
    int rc = mintfs_stat(&fs, f->ino, &st);
    if (rc) return to_errno(rc);
    out->ino = st.ino; out->size = st.size; out->type = st.type; out->mode = st.mode;
    out->btime = st.btime; out->mtime = st.mtime; out->ctime = st.ctime;
    return 0;
}
/* Background commits: file data written without close/sync becomes durable within ~2 s. */
void mint_mfs_task(void *argument) {
    (void)argument;
    for (;;) {
        mint_task_sleep_ticks(50);
        if (ready() && mint_timer_ticks() - last_commit_tick >= 200u) {
            (void)mintfs_commit(&fs);
            last_commit_tick = mint_timer_ticks();
        }
    }
}
#endif
