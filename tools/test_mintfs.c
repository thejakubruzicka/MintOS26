/* Host torture test for MintFS (kernel/fs/mintfs.c).
 *
 * The RAM "disk" models a drive with a volatile write cache: writes since
 * the last FLUSH are pending; a simulated power cut persists a random
 * subset of them (in any order), some torn at sector granularity. After
 * every cut the filesystem must mount, pass fsck, and match the model
 * either before or after the commit that was in flight. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/mintfs.h"

static int failures, checks;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------ simulated disk */
#define MAXBLK 8192u
typedef struct disk {
    uint64_t blocks;
    uint8_t *durable, *current;
    uint8_t pending[MAXBLK];
    uint64_t writes, flushes;
    int64_t clock;
    uint64_t crash_at;        /* from this write on, the "power is gone": nothing more reaches the disk */
    int frozen;
} DISK;
static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint32_t rn(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

static int d_read(void *c, uint64_t b, void *buf) { DISK *d = c; if (b >= d->blocks) return 0; memcpy(buf, d->current + b * MINTFS_BLOCK, MINTFS_BLOCK); return 1; }
static int d_write(void *c, uint64_t b, const void *buf) {
    DISK *d = c; if (b >= d->blocks) return 0;
    if (d->writes >= d->crash_at) d->frozen = 1;
    if (d->frozen) { ++d->writes; return 1; }
    memcpy(d->current + b * MINTFS_BLOCK, buf, MINTFS_BLOCK); d->pending[b] = 1; ++d->writes; return 1;
}
static int d_flush(void *c) {
    DISK *d = c;
    if (d->frozen) return 1;
    for (uint64_t b = 0; b < d->blocks; ++b) if (d->pending[b]) { memcpy(d->durable + b * MINTFS_BLOCK, d->current + b * MINTFS_BLOCK, MINTFS_BLOCK); d->pending[b] = 0; }
    ++d->flushes; return 1;
}
static int64_t d_now(void *c) { DISK *d = c; return ++d->clock; }
/* Power cut: persist a random subset of pending writes, some torn. */
static void power_cut(DISK *d) {
    for (uint64_t b = 0; b < d->blocks; ++b) {
        if (!d->pending[b]) continue;
        uint32_t r = rn(10);
        if (r < 4) { /* lost */ }
        else if (r < 8) memcpy(d->durable + b * MINTFS_BLOCK, d->current + b * MINTFS_BLOCK, MINTFS_BLOCK);
        else { uint32_t sectors = 1 + rn(7); memcpy(d->durable + b * MINTFS_BLOCK, d->current + b * MINTFS_BLOCK, sectors * 512u); }
        d->pending[b] = 0;
    }
    memcpy(d->current, d->durable, d->blocks * MINTFS_BLOCK);
    d->frozen = 0; d->crash_at = UINT64_MAX;
}
static void disk_init(DISK *d, uint64_t blocks) {
    memset(d, 0, sizeof(*d));
    d->blocks = blocks; d->crash_at = UINT64_MAX;
    d->durable = calloc(blocks, MINTFS_BLOCK); d->current = calloc(blocks, MINTFS_BLOCK);
}
static MINTFS_DEV dev_of(DISK *d) { MINTFS_DEV v = {d, d->blocks, d_read, d_write, d_flush, d_now}; return v; }

/* ------------------------------------------------------------ model */
#define MAXN 600
#define MAXDATA (300u * 1024u)
typedef struct node { int used, dir, parent; char name[64]; uint8_t *data; uint32_t size; } NODE;
typedef struct model { NODE n[MAXN]; } MODEL;
static void model_init(MODEL *m) { memset(m, 0, sizeof(*m)); m->n[0].used = 1; m->n[0].dir = 1; m->n[0].parent = 0; }
static void model_free(MODEL *m) { for (int i = 0; i < MAXN; ++i) free(m->n[i].data); memset(m, 0, sizeof(*m)); }
static void model_copy(MODEL *dst, const MODEL *src) {
    model_free(dst);
    *dst = *src;
    for (int i = 0; i < MAXN; ++i) if (src->n[i].data) { dst->n[i].data = malloc(MAXDATA); memcpy(dst->n[i].data, src->n[i].data, MAXDATA); }
}
static int ieq(const char *a, const char *b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int model_child(MODEL *m, int dir, const char *name) {
    for (int i = 1; i < MAXN; ++i) if (m->n[i].used && m->n[i].parent == dir && ieq(m->n[i].name, name)) return i;
    return -1;
}
static int model_children(MODEL *m, int dir) { int c = 0; for (int i = 1; i < MAXN; ++i) if (m->n[i].used && m->n[i].parent == dir) ++c; return c; }

/* path of a model node into buf, resolved in MintFS */
static int resolve_node(MINTFS *fs, MODEL *m, int idx, uint64_t *ino) {
    int chain[MAXN], depth = 0;                      /* random nesting can get deep */
    for (int i = idx; i != 0 && depth < MAXN; i = m->n[i].parent) chain[depth++] = i;
    uint64_t cur = MINTFS_ROOT_INO;
    for (int k = depth - 1; k >= 0; --k) if (mintfs_lookup(fs, cur, m->n[chain[k]].name, &cur)) return -1;
    *ino = cur;
    return 0;
}

/* Full comparison of the mounted filesystem with the model. */
static int compare(MINTFS *fs, MODEL *m, int idx, uint64_t ino, char *why) {
    MINTFS_DIRENT de;
    int count = 0;
    static uint8_t buf[MAXDATA];
    MINTFS_DIRENT chunk[7];                          /* mintfs_list must agree with readdir */
    int chunk_n = 0;
    for (uint32_t i = 0;; ++i) {
        int r = mintfs_readdir(fs, ino, i, &de);
        if (r < 0) { sprintf(why, "readdir error %d", r); return 0; }
        if (i % 7u == 0) {
            chunk_n = mintfs_list(fs, ino, i, chunk, 7);
            if (chunk_n < 0) { sprintf(why, "list error %d", chunk_n); return 0; }
        }
        if (!r) { if (chunk_n != (int)(i % 7u)) { sprintf(why, "list returned extra entries"); return 0; } break; }
        if ((int)(i % 7u) >= chunk_n || chunk[i % 7u].ino != de.ino || strcmp(chunk[i % 7u].name, de.name) || chunk[i % 7u].type != de.type) {
            sprintf(why, "list disagrees with readdir at %u", i); return 0;
        }
        ++count;
        int c = model_child(m, idx, de.name);
        if (c < 0) { sprintf(why, "extra entry %s", de.name); return 0; }
        if (strcmp(m->n[c].name, de.name)) { sprintf(why, "name case %s vs %s", de.name, m->n[c].name); return 0; }
        if (m->n[c].dir != (de.type == MINTFS_TYPE_DIR)) { sprintf(why, "type of %s", de.name); return 0; }
        if (m->n[c].dir) { if (!compare(fs, m, c, de.ino, why)) return 0; }
        else {
            MINTFS_STAT st;
            if (mintfs_stat(fs, de.ino, &st) || st.size != m->n[c].size) { sprintf(why, "size of %s: %llu vs %u", de.name, (unsigned long long)st.size, m->n[c].size); return 0; }
            int64_t got = mintfs_read(fs, de.ino, 0, buf, m->n[c].size);
            if (got != (int64_t)m->n[c].size || (m->n[c].size && memcmp(buf, m->n[c].data, m->n[c].size))) { sprintf(why, "content of %s", de.name); return 0; }
        }
    }
    if (count != model_children(m, idx)) { sprintf(why, "missing entries in dir %d", idx); return 0; }
    return 1;
}

static void random_name(char *out) {
    static const char *words[] = {"Notes", "report", "photo", "Draft", "budget", "todo", "Readme", "music", "plan", "x", "Data", "archive"};
    sprintf(out, "%s%u%s", words[rn(12)], rn(40), rn(4) ? "" : ".txt");
}
static int pick(MODEL *m, int want_dir) {
    int c[MAXN], k = 0;
    for (int i = 0; i < MAXN; ++i) if (m->n[i].used && (want_dir < 0 || m->n[i].dir == want_dir)) c[k++] = i;
    return k ? c[rn(k)] : -1;
}
static int is_ancestor(MODEL *m, int a, int b) { for (int i = b; ; i = m->n[i].parent) { if (i == a) return 1; if (i == 0) return 0; } }

/* One random operation applied to both. Returns 0 if the filesystem disagreed. */
static int random_op(MINTFS *fs, MODEL *m, char *why) {
    static uint8_t tmp[MAXDATA];
    uint32_t op = rn(100);
    char name[64];
    if (op < 22) {                                   /* create file or folder */
        int d = pick(m, 1); uint64_t dino; random_name(name);
        if (resolve_node(fs, m, d, &dino)) { strcpy(why, "resolve dir"); return 0; }
        int dir = rn(4) == 0;
        int exists = model_child(m, d, name) >= 0;
        uint64_t ino;
        int rc = mintfs_create(fs, dino, name, dir ? MINTFS_TYPE_DIR : MINTFS_TYPE_FILE, &ino);
        if (exists) { if (rc != MINTFS_EEXIST) { sprintf(why, "create existing gave %d", rc); return 0; } return 1; }
        int slot = -1; for (int i = 1; i < MAXN; ++i) if (!m->n[i].used) { slot = i; break; }
        if (rc == MINTFS_ENOSPC || slot < 0) { if (!rc) { mintfs_unlink(fs, dino, name); } return 1; }
        if (rc) { sprintf(why, "create %s failed %d", name, rc); return 0; }
        NODE *nn = &m->n[slot]; memset(nn, 0, sizeof(*nn)); nn->used = 1; nn->dir = dir; nn->parent = d; strcpy(nn->name, name);
        if (!dir) nn->data = calloc(1, MAXDATA);
        return 1;
    }
    if (op < 55) {                                   /* write */
        int f = pick(m, 0); if (f < 0) return 1;
        uint64_t ino; if (resolve_node(fs, m, f, &ino)) { strcpy(why, "resolve file"); return 0; }
        uint32_t off = rn(4) == 0 ? m->n[f].size : rn(m->n[f].size + 9000), len = rn(4) ? rn(6000) : rn(40000);
        if (rn(20) == 0) { off = rn(MAXDATA - 70000); len = rn(65536); }
        if (off + len > MAXDATA) len = MAXDATA - off;
        for (uint32_t i = 0; i < len; ++i) tmp[i] = (uint8_t)rnd();
        uint32_t done = 0;
        while (done < len) {
            int64_t w = mintfs_write(fs, ino, off + done, tmp + done, len - done);
            if (w == MINTFS_ENOSPC) break;
            if (w < 0) { sprintf(why, "write failed %lld", (long long)w); return 0; }
            if (w == 0 || (uint32_t)w < len - done) { if (mintfs_commit(fs)) { strcpy(why, "commit in write"); return 0; } }
            done += (uint32_t)w;
        }
        if (off > m->n[f].size && done) memset(m->n[f].data + m->n[f].size, 0, off - m->n[f].size);
        memcpy(m->n[f].data + off, tmp, done);
        if (done && off + done > m->n[f].size) m->n[f].size = off + done;
        if (done < len) return mintfs_abort(fs) == 0 ? 2 : 0;   /* disk full: the model can't express partial writes */
        return 1;
    }
    if (op < 63) {                                   /* truncate */
        int f = pick(m, 0); if (f < 0) return 1;
        uint64_t ino; if (resolve_node(fs, m, f, &ino)) { sprintf(why, "resolve failed at site %d", 1); return 0; }
        uint32_t size = rn(3) ? rn(m->n[f].size + 1) : rn(MAXDATA / 2);
        int rc = mintfs_truncate(fs, ino, size);
        if (rc == MINTFS_ENOSPC) return mintfs_abort(fs) == 0 ? 2 : 0;
        if (rc) { sprintf(why, "truncate %d", rc); return 0; }
        if (size > m->n[f].size) memset(m->n[f].data + m->n[f].size, 0, size - m->n[f].size);
        m->n[f].size = size;
        return 1;
    }
    if (op < 75) {                                   /* unlink */
        int x = pick(m, -1); if (x <= 0) return 1;
        uint64_t pino; if (resolve_node(fs, m, m->n[x].parent, &pino)) { sprintf(why, "resolve failed at site %d", 2); return 0; }
        int rc = mintfs_unlink(fs, pino, m->n[x].name);
        if (m->n[x].dir && model_children(m, x)) { if (rc != MINTFS_ENOTEMPTY) { sprintf(why, "rmdir non-empty gave %d", rc); return 0; } return 1; }
        if (rc == MINTFS_ENOSPC) return mintfs_abort(fs) == 0 ? 2 : 0;
        if (rc) { sprintf(why, "unlink %s gave %d", m->n[x].name, rc); return 0; }
        free(m->n[x].data); memset(&m->n[x], 0, sizeof(m->n[x]));
        return 1;
    }
    if (op < 90) {                                   /* rename / move */
        int x = pick(m, -1), d = pick(m, 1); if (x <= 0 || d < 0) return 1;
        if (rn(3)) random_name(name); else { strcpy(name, m->n[x].name); for (char *p = name; *p; ++p) if (*p >= 'a' && *p <= 'z' && rn(2)) *p -= 32; }
        uint64_t sino, dino;
        if (resolve_node(fs, m, m->n[x].parent, &sino) || resolve_node(fs, m, d, &dino)) { sprintf(why, "resolve failed at site %d", 3); return 0; }
        int rc = mintfs_rename(fs, sino, m->n[x].name, dino, name);
        int target = model_child(m, d, name);
        int expect = 0;
        if (m->n[x].dir && is_ancestor(m, x, d)) expect = MINTFS_EINVAL;
        else if (target >= 0 && target != x) {
            if (m->n[target].dir && !m->n[x].dir) expect = MINTFS_EISDIR;
            else if (!m->n[target].dir && m->n[x].dir) expect = MINTFS_ENOTDIR;
            else if (m->n[target].dir && model_children(m, target)) expect = MINTFS_ENOTEMPTY;
        }
        if (rc == MINTFS_ENOSPC) return mintfs_abort(fs) == 0 ? 2 : 0;
        if (rc != expect) { sprintf(why, "rename %s -> %s gave %d expected %d", m->n[x].name, name, rc, expect); return 0; }
        if (rc) return 1;
        if (target >= 0 && target != x) { free(m->n[target].data); memset(&m->n[target], 0, sizeof(m->n[target])); }
        if (target == x && m->n[x].parent != d) return 1;
        m->n[x].parent = d; strcpy(m->n[x].name, name);
        return 1;
    }
    /* read back a random slice and compare */
    int f = pick(m, 0); if (f < 0) return 1;
    uint64_t ino; if (resolve_node(fs, m, f, &ino)) { sprintf(why, "resolve failed at site %d", 4); return 0; }
    uint32_t off = rn(m->n[f].size + 100), len = rn(20000);
    int64_t got = mintfs_read(fs, ino, off, tmp, len);
    int64_t expect = off >= m->n[f].size ? 0 : (int64_t)(len < m->n[f].size - off ? len : m->n[f].size - off);
    if (got != expect || (expect > 0 && memcmp(tmp, m->n[f].data + off, (size_t)expect))) { sprintf(why, "read slice mismatch"); return 0; }
    return 1;
}

/* Keeps the random workload from filling the disk: delete the largest files and commit. */
static int make_room(MINTFS *fs, MODEL *m, uint64_t want_free) {
    MINTFS_INFO info;
    for (int guard = 0; guard < 50; ++guard) {
        mintfs_info(fs, &info);
        if (info.free_blocks >= want_free) return 1;
        int big = -1;
        for (int i = 1; i < MAXN; ++i) if (m->n[i].used && !m->n[i].dir && (big < 0 || m->n[i].size > m->n[big].size)) big = i;
        if (big < 0) return 1;
        uint64_t pino;
        if (resolve_node(fs, m, m->n[big].parent, &pino) || mintfs_unlink(fs, pino, m->n[big].name) || mintfs_commit(fs)) return 0;
        free(m->n[big].data); memset(&m->n[big], 0, sizeof(m->n[big]));
    }
    return 1;
}

static uint8_t *bitmaps, *work;
static int fsck_clean(DISK *d, char *why) {
    MINTFS_DEV dv = dev_of(d);
    MINTFS_CHECK_REPORT r;
    int rc = mintfs_check(&dv, &r, work, mintfs_check_work_bytes(d->blocks));
    if (rc) sprintf(why, "fsck: %s [block %llu] (csum %u ptr %u dbl %u leak %u miss %u dir %u link %u)", r.first_problem, (unsigned long long)r.problem_block,
                    r.bad_checksums, r.bad_pointers, r.double_references, r.leaked_blocks, r.missing_blocks, r.bad_directories, r.bad_links);
    return rc == 0;
}

static MINTFS fs;

int main(void) {
    char why[256] = "";
    if (getenv("MINTFS_SEED")) rng = strtoull(getenv("MINTFS_SEED"), 0, 10) * 2654435761ull + 1u;
    bitmaps = malloc(mintfs_mount_bytes(MAXBLK));
    work = malloc(mintfs_check_work_bytes(MAXBLK));
    DISK d; disk_init(&d, 4096);
    MINTFS_DEV dv = dev_of(&d);
    uint8_t uuid[16] = {1, 2, 3};

    /* --- format and empty mount */
    CHECK(mintfs_format(&dv, "MintOS HD", uuid) == 0, "format");
    CHECK(mintfs_mount(&fs, &dv, bitmaps, mintfs_mount_bytes(d.blocks), 0) == 0, "mount fresh filesystem");
    CHECK(fsck_clean(&d, why), "fresh filesystem is clean: %s", why);
    MINTFS_INFO info; mintfs_info(&fs, &info);
    CHECK(!strcmp(info.label, "MintOS HD") && info.inodes == 1 && info.free_blocks > 4000, "label, inode count and free space after format");

    /* --- names */
    uint64_t a, b;
    CHECK(mintfs_create(&fs, MINTFS_ROOT_INO, "Documents", MINTFS_TYPE_DIR, &a) == 0, "mkdir Documents");
    CHECK(mintfs_lookup(&fs, MINTFS_ROOT_INO, "documents", &b) == 0 && a == b, "lookups are case-insensitive");
    CHECK(mintfs_create(&fs, MINTFS_ROOT_INO, "DOCUMENTS", MINTFS_TYPE_FILE, &b) == MINTFS_EEXIST, "names differing only by case collide");
    CHECK(mintfs_create(&fs, a, "a/b", MINTFS_TYPE_FILE, &b) == MINTFS_EINVAL && mintfs_create(&fs, a, "..", MINTFS_TYPE_FILE, &b) == MINTFS_EINVAL &&
          mintfs_create(&fs, a, "", MINTFS_TYPE_FILE, &b) == MINTFS_EINVAL, "slashes, '..' and empty names are rejected");
    char longname[300]; memset(longname, 'n', 256); longname[256] = 0;
    CHECK(mintfs_create(&fs, a, longname, MINTFS_TYPE_FILE, &b) == MINTFS_ENAMETOOLONG, "256-byte names are rejected");
    longname[255] = 0;
    CHECK(mintfs_create(&fs, a, longname, MINTFS_TYPE_FILE, &b) == 0, "255-byte names are accepted");
    CHECK(mintfs_create(&fs, a, "P\xC5\x99\xC3\xADli\xC5\xA1 \xC5\xBElu\xC5\xA5ou\xC4\x8Dk\xC3\xBD k\xC5\xAF\xC5\x88.txt", MINTFS_TYPE_FILE, &b) == 0,
          "UTF-8 (Czech) names are accepted");
    uint64_t r2;
    CHECK(mintfs_resolve(&fs, "/Documents", &r2) == 0 && r2 == a && mintfs_resolve(&fs, "/", &r2) == 0 && r2 == MINTFS_ROOT_INO, "absolute path resolution");
    CHECK(mintfs_commit(&fs) == 0 && fsck_clean(&d, why), "commit and fsck after name tests: %s", why);

    /* --- large and sparse files through every pointer level */
    uint64_t big;
    CHECK(mintfs_create(&fs, MINTFS_ROOT_INO, "big.bin", MINTFS_TYPE_FILE, &big) == 0, "create big.bin");
    static uint8_t blk[65536], back[65536];
    /* inline -> direct, end of direct, first indirect, end of indirect, double indirect (several mid blocks) */
    uint64_t offsets[] = {0, 20000, 4096 * 493 + 5, 4096 * 497, 4096 * 1000 + 7, 4096 * 1006 + 1, 4096 * 2600 + 33, 4096 * 9000};
    int okbig = 1;
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 9000; ++j) blk[j] = (uint8_t)(j * 7 + i);
        uint32_t done = 0;
        while (done < 9000) { int64_t w = mintfs_write(&fs, big, offsets[i] + done, blk + done, 9000 - done); if (w < 0) { okbig = 0; break; } done += (uint32_t)w; if (done < 9000) mintfs_commit(&fs); }
    }
    CHECK(okbig && mintfs_commit(&fs) == 0, "sparse writes across inline, direct, indirect and double-indirect ranges");
    for (int i = 0; i < 8 && okbig; ++i) {
        for (int j = 0; j < 9000; ++j) blk[j] = (uint8_t)(j * 7 + i);
        if (mintfs_read(&fs, big, offsets[i], back, 9000) != 9000 || memcmp(back, blk, 9000)) okbig = 0;
    }
    CHECK(okbig, "sparse data reads back at every level");
    CHECK(mintfs_read(&fs, big, 4096 * 1500, back, 4096) == 4096 && back[0] == 0 && back[4095] == 0, "holes read as zeros");
    CHECK(fsck_clean(&d, why), "fsck after sparse file: %s", why);
    CHECK(mintfs_truncate(&fs, big, 5000) == 0 && mintfs_commit(&fs) == 0 && fsck_clean(&d, why), "shrinking frees the pointer blocks: %s", why);
    CHECK(mintfs_truncate(&fs, big, 9000) == 0 && mintfs_read(&fs, big, 5000, back, 4000) == 4000 && back[0] == 0 && back[3999] == 0,
          "regrowing after a shrink reads zeros, not old bytes");
    CHECK(mintfs_unlink(&fs, MINTFS_ROOT_INO, "big.bin") == 0 && mintfs_commit(&fs) == 0 && fsck_clean(&d, why), "deleting a big file: %s", why);
    mintfs_unmount(&fs);

    /* --- random operations against the model, verified after each commit and remount */
    DISK d2; disk_init(&d2, 4096);
    MINTFS_DEV dv2 = dev_of(&d2);
    mintfs_format(&dv2, "Torture", uuid);
    mintfs_mount(&fs, &dv2, bitmaps, mintfs_mount_bytes(d2.blocks), 0);
    static MODEL m; model_init(&m);
    int ok = 1, ops = 0, commits = 0, remounts = 0;
    for (int round = 0; round < 400 && ok; ++round) {
        if (!make_room(&fs, &m, 1400)) { ok = 0; strcpy(why, "make_room"); break; }
        int n = 5 + rn(40);
        for (int i = 0; i < n && ok; ++i, ++ops) {
            int r = random_op(&fs, &m, why);
            if (!r) { ok = 0; printf("  op %d: %s\n", ops, why); break; }
            if (r == 2) {                        /* aborted on a full disk: reload the model from the filesystem state */
                ok = 0; strcpy(why, "unexpected ENOSPC in a roomy filesystem"); break;
            }
        }
        if (!ok) break;
        if (mintfs_commit(&fs)) { ok = 0; strcpy(why, "commit"); break; }
        ++commits;
        if (!compare(&fs, &m, 0, MINTFS_ROOT_INO, why)) { ok = 0; break; }
        if ((getenv("MINTFS_FSCK_EVERY") || round % 25 == 0) && !fsck_clean(&d2, why)) { ok = 0; printf("  round %d\n", round); break; }
        if (round % 40 == 0) {
            mintfs_unmount(&fs);
            if (mintfs_mount(&fs, &dv2, bitmaps, mintfs_mount_bytes(d2.blocks), 0) || !compare(&fs, &m, 0, MINTFS_ROOT_INO, why)) { ok = 0; strcat(why, " (after remount)"); break; }
            ++remounts;
        }
    }
    if (!ok && getenv("MINTFS_DEBUG")) {
        uint64_t target = strtoull(getenv("MINTFS_DEBUG"), 0, 10);
        uint8_t *tb = d2.durable + target * MINTFS_BLOCK;
        printf("  block %llu: tag %.4s gen %llu self %llu owner %llu\n", (unsigned long long)target, (char *)tb,
               (unsigned long long)(*(uint64_t *)(tb + 8)), (unsigned long long)(*(uint64_t *)(tb + 16)), (unsigned long long)(*(uint64_t *)(tb + 24)));
        for (uint64_t b = 0; b < d2.blocks; ++b) {
            uint8_t *p = d2.durable + b * MINTFS_BLOCK;
            for (uint32_t o = 32; o + 8 <= MINTFS_BLOCK; o += 8)
                if (*(uint64_t *)(p + o) == target) printf("  referenced from block %llu (tag %.4s gen %llu owner %llu) offset %u\n",
                    (unsigned long long)b, (char *)p, (unsigned long long)(*(uint64_t *)(p + 8)), (unsigned long long)(*(uint64_t *)(p + 24)), o);
        }
    }
    CHECK(ok, "random operations match the model (%d ops, %d commits, %d remounts): %s", ops, commits, remounts, why);
    CHECK(fsck_clean(&d2, why), "fsck after the random run: %s", why);
    mintfs_info(&fs, &info);
    printf("  random run: %d operations, %d commits, %d remounts, %llu inodes, %llu free blocks\n", ops, commits, remounts,
           (unsigned long long)info.inodes, (unsigned long long)info.free_blocks);

    /* --- power cuts at random points, including in the middle of commits */
    static MODEL before, after;
    int cut_ok = 1, cuts = 0, saw_before = 0, saw_after = 0;
    for (int trial = 0; trial < 300 && cut_ok; ++trial) {
        if (!make_room(&fs, &m, 1400)) { cut_ok = 0; strcpy(why, "make_room"); break; }
        model_copy(&before, &m);
        /* apply a batch of operations, then commit with a cut at a random write */
        int n = 1 + rn(25);
        for (int i = 0; i < n; ++i) {
            uint64_t rng_before = rng;
            int r = random_op(&fs, &m, why);
            if (r != 1) { cut_ok = 0; if (r == 2) strcpy(why, "unexpected ENOSPC"); break; }
            if (getenv("MINTFS_PARANOID") && !compare(&fs, &m, 0, MINTFS_ROOT_INO, why)) {
                printf("  diverged after op %d of trial %d (rng %llu)\n", i, trial, (unsigned long long)rng_before); cut_ok = 0; break;
            }
        }
        if (!cut_ok) break;
        model_copy(&after, &m);
        uint64_t gen_before = fs.generation - 1;
        int cut_mode = rn(4);                      /* 0: cut before committing; otherwise at a random write inside the commit */
        if (cut_mode) { d2.crash_at = d2.writes + rn(40); mintfs_commit(&fs); }
        power_cut(&d2);
        ++cuts;
        if (mintfs_mount(&fs, &dv2, bitmaps, mintfs_mount_bytes(d2.blocks), 0)) { cut_ok = 0; strcpy(why, "mount after cut failed"); break; }
        if (!fsck_clean(&d2, why)) { cut_ok = 0; break; }
        uint64_t gen_now = fs.generation - 1;
        MODEL *expect = gen_now == gen_before ? &before : &after;
        if (gen_now != gen_before && gen_now != gen_before + 1) { cut_ok = 0; sprintf(why, "unexpected generation %llu", (unsigned long long)gen_now); break; }
        if (gen_now == gen_before) ++saw_before; else ++saw_after;
        if (!compare(&fs, expect, 0, MINTFS_ROOT_INO, why)) { cut_ok = 0; strcat(why, gen_now == gen_before ? " (expected pre-commit state)" : " (expected committed state)"); break; }
        model_copy(&m, expect);
    }
    CHECK(cut_ok, "%d power cuts: always mountable, fsck-clean, and equal to the state before or after the interrupted commit: %s", cuts, why);
    CHECK(saw_before > 20 && saw_after > 20, "power cuts landed both before and after commit points (%d before, %d after)", saw_before, saw_after);
    printf("  power cuts: %d (rolled back %d, committed %d)\n", cuts, saw_before, saw_after);
    mintfs_unmount(&fs);

    /* --- a full disk */
    DISK d3; disk_init(&d3, 256);
    MINTFS_DEV dv3 = dev_of(&d3);
    mintfs_format(&dv3, "Tiny", uuid);
    mintfs_mount(&fs, &dv3, bitmaps, mintfs_mount_bytes(d3.blocks), 0);
    uint64_t f1; mintfs_create(&fs, MINTFS_ROOT_INO, "fill", MINTFS_TYPE_FILE, &f1);
    mintfs_commit(&fs);
    int64_t w = 0; uint64_t off = 0; int saw_full = 0;
    for (int i = 0; i < 400; ++i) {
        w = mintfs_write(&fs, f1, off, blk, 4096);
        if (w == MINTFS_ENOSPC) { saw_full = 1; break; }
        if (w > 0) off += (uint64_t)w;
        if (i % 8 == 0) mintfs_commit(&fs);
    }
    CHECK(saw_full, "a full disk reports ENOSPC");
    CHECK(mintfs_abort(&fs) == 0 && mintfs_commit(&fs) == 0, "after ENOSPC the open transaction can be dropped and the filesystem stays usable");
    CHECK(fsck_clean(&d3, why), "full disk is still consistent: %s", why);
    CHECK(mintfs_unlink(&fs, MINTFS_ROOT_INO, "fill") == 0 && mintfs_commit(&fs) == 0 && fsck_clean(&d3, why), "space comes back after deleting: %s", why);
    mintfs_info(&fs, &info);
    CHECK(info.free_blocks > 200, "free space restored (%llu free)", (unsigned long long)info.free_blocks);

    /* --- damaged metadata is detected */
    DISK d4; disk_init(&d4, 512);
    MINTFS_DEV dv4 = dev_of(&d4);
    mintfs_format(&dv4, "Damage", uuid);
    mintfs_mount(&fs, &dv4, bitmaps, mintfs_mount_bytes(d4.blocks), 0);
    uint64_t x; mintfs_create(&fs, MINTFS_ROOT_INO, "x", MINTFS_TYPE_DIR, &x); mintfs_commit(&fs);
    mintfs_unmount(&fs);
    int flipped = 0;                        /* flip a bit in every inode block, live or not */
    for (uint64_t blkno = 2; blkno < 64; ++blkno) {
        if (!memcmp(d4.durable + blkno * MINTFS_BLOCK, "INOD", 4)) { d4.durable[blkno * MINTFS_BLOCK + 200] ^= 0x40; d4.current[blkno * MINTFS_BLOCK + 200] ^= 0x40; flipped = 1; }
    }
    MINTFS_CHECK_REPORT rep;
    CHECK(flipped && mintfs_check(&dv4, &rep, work, mintfs_check_work_bytes(d4.blocks)) == MINTFS_ECORRUPT && rep.bad_checksums > 0,
          "a flipped bit in an inode is caught by its checksum");
    memset(d4.durable, 0x55, 2 * MINTFS_BLOCK); memset(d4.current, 0x55, 2 * MINTFS_BLOCK);
    CHECK(mintfs_mount(&fs, &dv4, bitmaps, mintfs_mount_bytes(d4.blocks), 0) == MINTFS_ECORRUPT, "a disk without a valid superblock is refused");

    printf("%d checks, %d failures\n", checks, failures);
    if (!failures) printf("PASS: MintFS host torture test\n");
    return failures ? 1 : 0;
}
