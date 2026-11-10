/* MintFS v1 — see kernel/include/mintfs.h for the design.
 *
 * Transaction rules (they are what make power cuts safe):
 *  - A block the committed state uses is never written. Changing it means
 *    copying it to a block allocated in this transaction ("fresh"), updating
 *    the parent pointer, and marking the old block "reserved": free in the
 *    new state, but not reusable until the new state is committed.
 *  - Fresh blocks may be written at any time; nothing committed points at them.
 *  - Commit: relocate changed bitmap blocks, write all dirty metadata and the
 *    bitmap, FLUSH, write the next superblock (generation g into slot g%2),
 *    FLUSH. Mount picks the valid superblock with the highest generation.
 * Single-threaded by design (the MintOS kernel calls it from one task). */
#include "../include/mintfs.h"

/* ------------------------------------------------------------------ on-disk constants */
#define TAG_SUPER 0x4253544Du    /* "MTSB" */
#define TAG_IMAP  0x50414D49u    /* "IMAP" */
#define TAG_BIDX  0x58444942u    /* "BIDX" */
#define TAG_BMAP  0x50414D42u    /* "BMAP" */
#define TAG_INODE 0x444F4E49u    /* "INOD" */
#define TAG_PTRS  0x53525450u    /* "PTRS" */
#define HDR 32u
#define FANOUT 508u              /* (4096 - 32) / 8 */
#define BITS_PER_BMAP (8u * (MINTFS_BLOCK - HDR))   /* 32512 */
#define INODE_DATA 128u
#define INLINE_MAX (MINTFS_BLOCK - INODE_DATA)        /* 3968 */
#define DIRECT 496u                                    /* 3968 / 8 */
#define FLAG_INLINE 1u
#define DIR_MAX (512u * 1024u)
#define MAX_FILE_BLOCKS ((uint64_t)DIRECT + FANOUT + (uint64_t)FANOUT * FANOUT)
static const char SIGNATURE[8] = {'M', 'I', 'N', 'T', 'F', 'S', '0', '1'};

/* superblock field offsets */
#define SB_SIG 32u
#define SB_VERSION 40u
#define SB_BSIZE 44u
#define SB_BLOCKS 48u
#define SB_IMAP 56u
#define SB_BIDX 64u
#define SB_NBMAP 72u
#define SB_FLAGS 76u
#define SB_FREE 80u
#define SB_INODES 88u
#define SB_UUID 96u
#define SB_LABEL 112u
#define SB_CREATED 144u
#define SB_MODIFIED 152u
/* inode field offsets */
#define I_TYPE 32u
#define I_MODE 36u
#define I_NLINK 40u
#define I_FLAGS 44u
#define I_SIZE 48u
#define I_BTIME 56u
#define I_MTIME 64u
#define I_CTIME 72u
#define I_PARENT 80u
#define I_IND 88u
#define I_DIND 96u
#define I_BLOCKS 104u

/* ------------------------------------------------------------------ helpers */
static void mzero(void *p, size_t n) { uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0; }
static void mcopy(void *d, const void *s, size_t n) { uint8_t *o = (uint8_t *)d; const uint8_t *i = (const uint8_t *)s; while (n--) *o++ = *i++; }
static void mmove(void *d, const void *s, size_t n) {
    uint8_t *o = (uint8_t *)d; const uint8_t *i = (const uint8_t *)s;
    if (o < i) while (n--) *o++ = *i++;
    else { o += n; i += n; while (n--) *--o = *--i; }
}
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) | ((uint64_t)g32(p + 4) << 32); }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32)); }

static uint32_t crc_table[256];
static int crc_ready;
uint32_t mintfs_crc32c(uint32_t crc, const void *data, size_t len) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) crc = crc_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
static void seal(uint8_t *b, uint32_t tag, uint64_t gen, uint64_t self, uint64_t owner) {
    p32(b, tag); p32(b + 4, 0); p64(b + 8, gen); p64(b + 16, self); p64(b + 24, owner);
    p32(b + 4, mintfs_crc32c(0, b, MINTFS_BLOCK));
}
static void reseal(uint8_t *b, uint64_t gen, uint64_t self) { seal(b, g32(b), gen, self, g64(b + 24)); }
static int verify(const uint8_t *b, uint32_t tag, uint64_t self, uint64_t max_gen) {
    static const uint8_t zero4[4] = {0, 0, 0, 0};
    if (g32(b) != tag || g64(b + 16) != self || g64(b + 8) > max_gen) return 0;
    uint32_t crc = mintfs_crc32c(0, b, 4);                 /* CRC over the block with the checksum field zeroed */
    crc = mintfs_crc32c(crc, zero4, 4);
    crc = mintfs_crc32c(crc, b + 8, MINTFS_BLOCK - 8u);
    return crc == g32(b + 4);
}
static int name_eq(const char *a, const uint8_t *b, size_t blen) {
    size_t i = 0;
    for (; i < blen; ++i) {
        char x = a[i], y = (char)b[i];
        if (!x) return 0;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return a[i] == '\0';
}
static int name_ok(const char *n, size_t *len) {
    size_t l = 0;
    if (!n || !n[0]) return MINTFS_EINVAL;
    while (n[l]) { if (n[l] == '/' || (uint8_t)n[l] < 32u) return MINTFS_EINVAL; if (++l > MINTFS_NAME_MAX) return MINTFS_ENAMETOOLONG; }
    if ((l == 1 && n[0] == '.') || (l == 2 && n[0] == '.' && n[1] == '.')) return MINTFS_EINVAL;
    *len = l;
    return 0;
}
static int64_t now(MINTFS *fs) { return fs->dev.now ? fs->dev.now(fs->dev.ctx) : 0; }

/* ------------------------------------------------------------------ bitmaps */
uint64_t mintfs_bitmap_bytes(uint64_t blocks) { return (blocks + 7u) / 8u; }
static uint64_t bmap_count(uint64_t blocks) { return (blocks + BITS_PER_BMAP - 1u) / BITS_PER_BMAP; }
uint64_t mintfs_mount_bytes(uint64_t blocks) { return 3u * mintfs_bitmap_bytes(blocks) + bmap_count(blocks); }
static int bit(const uint8_t *m, uint64_t b) { return (m[b >> 3] >> (b & 7u)) & 1u; }
static void set_bit(uint8_t *m, uint64_t b, int v) { if (v) m[b >> 3] |= (uint8_t)(1u << (b & 7u)); else m[b >> 3] &= (uint8_t)~(1u << (b & 7u)); }
#define FRESH(fs) ((fs)->reserved + (fs)->bitmap_bytes)

static uint64_t sb_get(MINTFS *fs, uint32_t off) { return g64(fs->sb + off); }
static void sb_set(MINTFS *fs, uint32_t off, uint64_t v) { p64(fs->sb + off, v); }

static void mark_used(MINTFS *fs, uint64_t b, int used) {
    if (bit(fs->used, b) == used) return;
    set_bit(fs->used, b, used);
    fs->bitmap_dirty[b / BITS_PER_BMAP] = 1;
    sb_set(fs, SB_FREE, sb_get(fs, SB_FREE) + (used ? (uint64_t)-1 : 1u));
}
static int is_fresh(MINTFS *fs, uint64_t b) { return bit(FRESH(fs), b); }

/* Ordinary operations leave room for a commit to relocate every bitmap block. */
static uint64_t commit_reserve(MINTFS *fs) { return bmap_count(fs->dev.block_count) + 8u; }
static uint64_t alloc_block(MINTFS *fs) {
    uint64_t n = fs->dev.block_count;
    if (!sb_get(fs, SB_FREE) || (!fs->in_commit && sb_get(fs, SB_FREE) <= commit_reserve(fs))) return 0;
    for (uint64_t k = 0; k < n; ++k) {
        uint64_t b = fs->alloc_hint + k;
        if (b >= n) b -= n;
        if (b < 2u) continue;
        if (fs->used[b >> 3] == 0xFFu && (b & 7u) == 0u && b + 8u <= n) { k += 7u; continue; }
        if (!bit(fs->used, b) && !bit(fs->reserved, b)) {
            mark_used(fs, b, 1);
            set_bit(FRESH(fs), b, 1);
            fs->alloc_hint = b + 1u;
            return b;
        }
    }
    return 0;
}
static void free_block(MINTFS *fs, uint64_t b) {
    if (b < 2u || b >= fs->dev.block_count || !bit(fs->used, b)) return;
    mark_used(fs, b, 0);
    if (is_fresh(fs, b)) set_bit(FRESH(fs), b, 0);      /* never committed: reusable now */
    else set_bit(fs->reserved, b, 1);                  /* committed state still needs it */
}

/* ------------------------------------------------------------------ block cache */
static MINTFS_CACHE_ENTRY *cache_find(MINTFS *fs, uint64_t addr) {
    for (uint32_t i = 0; i < MINTFS_CACHE_BLOCKS; ++i)
        if (fs->cache[i].valid && fs->cache[i].addr == addr) { fs->cache[i].lru = ++fs->lru_clock; return &fs->cache[i]; }
    return 0;
}
static MINTFS_CACHE_ENTRY *cache_slot(MINTFS *fs) {
    MINTFS_CACHE_ENTRY *best = 0;
    for (uint32_t i = 0; i < MINTFS_CACHE_BLOCKS; ++i) {
        MINTFS_CACHE_ENTRY *e = &fs->cache[i];
        if (!e->valid) return e;
        if (e->dirty || e->pinned) continue;
        if (!best || e->lru < best->lru) best = e;
    }
    return best;
}
/* Loads a committed or fresh metadata block, verifying its header. */
static MINTFS_CACHE_ENTRY *meta_get(MINTFS *fs, uint64_t addr, uint32_t tag) {
    if (addr < 2u || addr >= fs->dev.block_count) return 0;
    MINTFS_CACHE_ENTRY *e = cache_find(fs, addr);
    if (e) return g32(e->data) == tag ? e : 0;
    e = cache_slot(fs);
    if (!e) return 0;
    if (!fs->dev.read(fs->dev.ctx, addr, e->data)) { e->valid = 0; return 0; }
    if (!verify(e->data, tag, addr, fs->generation)) { e->valid = 0; return 0; }
    e->valid = 1; e->dirty = 0; e->addr = addr; e->lru = ++fs->lru_clock;
    return e;
}
static MINTFS_CACHE_ENTRY *meta_new(MINTFS *fs, uint32_t tag, uint64_t owner) {
    MINTFS_CACHE_ENTRY *e = cache_slot(fs);
    if (!e) return 0;
    uint64_t b = alloc_block(fs);
    if (!b) return 0;
    mzero(e->data, MINTFS_BLOCK);
    p32(e->data, tag); p64(e->data + 24, owner);
    e->valid = 1; e->dirty = 1; e->addr = b; e->lru = ++fs->lru_clock;
    ++fs->dirty_blocks;
    return e;
}
/* Makes a cached block writable in this transaction. Returns the entry,
 * whose address may have changed (the caller updates the parent pointer). */
static MINTFS_CACHE_ENTRY *meta_cow(MINTFS *fs, MINTFS_CACHE_ENTRY *e) {
    if (!e) return 0;
    if (!is_fresh(fs, e->addr)) {
        uint64_t nb = alloc_block(fs);
        if (!nb) return 0;
        free_block(fs, e->addr);
        e->addr = nb;
    }
    if (!e->dirty) { e->dirty = 1; ++fs->dirty_blocks; }
    return e;
}

/* ------------------------------------------------------------------ inode map */
static uint64_t imap_lookup(MINTFS *fs, uint64_t ino) {
    if (!ino || ino >= MINTFS_MAX_INODES) return 0;
    MINTFS_CACHE_ENTRY *root = meta_get(fs, sb_get(fs, SB_IMAP), TAG_IMAP);
    if (!root) return 0;
    uint64_t leaf_addr = g64(root->data + HDR + 8u * (ino / FANOUT));
    if (!leaf_addr) return 0;
    MINTFS_CACHE_ENTRY *leaf = meta_get(fs, leaf_addr, TAG_IMAP);
    return leaf ? g64(leaf->data + HDR + 8u * (ino % FANOUT)) : 0;
}
static int imap_set(MINTFS *fs, uint64_t ino, uint64_t addr) {
    MINTFS_CACHE_ENTRY *root = meta_cow(fs, meta_get(fs, sb_get(fs, SB_IMAP), TAG_IMAP));
    if (!root) return MINTFS_EIO;
    sb_set(fs, SB_IMAP, root->addr);
    root->pinned = 1;
    uint64_t li = ino / FANOUT;
    uint64_t leaf_addr = g64(root->data + HDR + 8u * li);
    MINTFS_CACHE_ENTRY *leaf = leaf_addr ? meta_cow(fs, meta_get(fs, leaf_addr, TAG_IMAP)) : meta_new(fs, TAG_IMAP, 0);
    root->pinned = 0;
    if (!leaf) return MINTFS_EIO;
    p64(root->data + HDR + 8u * li, leaf->addr);
    p64(leaf->data + HDR + 8u * (ino % FANOUT), addr);
    return 0;
}
static MINTFS_CACHE_ENTRY *inode_get(MINTFS *fs, uint64_t ino) {
    uint64_t a = imap_lookup(fs, ino);
    MINTFS_CACHE_ENTRY *e = a ? meta_get(fs, a, TAG_INODE) : 0;
    return e && g64(e->data + 24) == ino ? e : 0;
}
static MINTFS_CACHE_ENTRY *inode_cow(MINTFS *fs, uint64_t ino) {
    MINTFS_CACHE_ENTRY *e = inode_get(fs, ino);
    if (!e) return 0;
    uint64_t old = e->addr;
    e = meta_cow(fs, e);
    if (!e) return 0;
    if (e->addr != old) {
        e->pinned = 1;
        int rc = imap_set(fs, ino, e->addr);
        e->pinned = 0;
        if (rc) return 0;
    }
    return e;
}
static uint64_t inode_alloc_number(MINTFS *fs) {
    for (uint64_t k = 0; k < MINTFS_MAX_INODES - 2u; ++k) {
        uint64_t ino = 2u + (fs->inode_hint - 2u + k) % (MINTFS_MAX_INODES - 2u);
        if (!imap_lookup(fs, ino)) { fs->inode_hint = ino + 1u; return ino; }
    }
    return 0;
}

/* ------------------------------------------------------------------ file block map */
/* Returns the disk block of file block fb (0 if a hole). */
static uint64_t fmap_get(MINTFS *fs, const uint8_t *in, uint64_t fb) {
    if (fb < DIRECT) return g64(in + INODE_DATA + 8u * fb);
    fb -= DIRECT;
    if (fb < FANOUT) {
        MINTFS_CACHE_ENTRY *p = meta_get(fs, g64(in + I_IND), TAG_PTRS);
        return p ? g64(p->data + HDR + 8u * fb) : 0;
    }
    fb -= FANOUT;
    if (fb >= (uint64_t)FANOUT * FANOUT) return 0;
    MINTFS_CACHE_ENTRY *p = meta_get(fs, g64(in + I_DIND), TAG_PTRS);
    if (!p) return 0;
    uint64_t mid = g64(p->data + HDR + 8u * (fb / FANOUT));
    MINTFS_CACHE_ENTRY *q = mid ? meta_get(fs, mid, TAG_PTRS) : 0;
    return q ? g64(q->data + HDR + 8u * (fb % FANOUT)) : 0;
}
/* Sets file block fb of a COW'd inode entry to disk block d (may be 0). */
static int fmap_set(MINTFS *fs, MINTFS_CACHE_ENTRY *ie, uint64_t fb, uint64_t d) {
    uint8_t *in = ie->data;
    uint64_t ino = g64(in + 24);
    if (fb < DIRECT) { p64(in + INODE_DATA + 8u * fb, d); return 0; }
    fb -= DIRECT;
    ie->pinned = 1;
    int rc = MINTFS_EIO;
    if (fb < FANOUT) {
        uint64_t pa = g64(in + I_IND);
        MINTFS_CACHE_ENTRY *p = pa ? meta_cow(fs, meta_get(fs, pa, TAG_PTRS)) : (d ? meta_new(fs, TAG_PTRS, ino) : 0);
        if (p) {
            if (!pa) p64(in + I_BLOCKS, g64(in + I_BLOCKS) + 1u);
            p64(in + I_IND, p->addr);
            p64(p->data + HDR + 8u * fb, d);
            rc = 0;
        } else if (!pa && !d) rc = 0;
        ie->pinned = 0;
        return rc;
    }
    fb -= FANOUT;
    if (fb >= (uint64_t)FANOUT * FANOUT) { ie->pinned = 0; return MINTFS_EFBIG; }
    uint64_t pa = g64(in + I_DIND);
    MINTFS_CACHE_ENTRY *p = pa ? meta_cow(fs, meta_get(fs, pa, TAG_PTRS)) : (d ? meta_new(fs, TAG_PTRS, ino) : 0);
    if (!p) { ie->pinned = 0; return (!pa && !d) ? 0 : MINTFS_EIO; }
    if (!pa) p64(in + I_BLOCKS, g64(in + I_BLOCKS) + 1u);
    p64(in + I_DIND, p->addr);
    p->pinned = 1;
    uint64_t ma = g64(p->data + HDR + 8u * (fb / FANOUT));
    MINTFS_CACHE_ENTRY *q = ma ? meta_cow(fs, meta_get(fs, ma, TAG_PTRS)) : (d ? meta_new(fs, TAG_PTRS, ino) : 0);
    if (q) {
        if (!ma) p64(in + I_BLOCKS, g64(in + I_BLOCKS) + 1u);
        p64(p->data + HDR + 8u * (fb / FANOUT), q->addr);
        p64(q->data + HDR + 8u * (fb % FANOUT), d);
        rc = 0;
    } else if (!ma && !d) rc = 0;
    p->pinned = 0; ie->pinned = 0;
    return rc;
}

/* ------------------------------------------------------------------ data I/O */
static int64_t inode_read(MINTFS *fs, const uint8_t *in, uint64_t off, uint8_t *buf, uint32_t len) {
    uint64_t size = g64(in + I_SIZE);
    if (off >= size) return 0;
    if (len > size - off) len = (uint32_t)(size - off);
    if (g32(in + I_FLAGS) & FLAG_INLINE) { mcopy(buf, in + INODE_DATA + off, len); return len; }
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done, fb = pos / MINTFS_BLOCK;
        uint32_t within = (uint32_t)(pos % MINTFS_BLOCK), n = MINTFS_BLOCK - within;
        if (n > len - done) n = len - done;
        uint64_t d = fmap_get(fs, in, fb);
        if (!d) mzero(buf + done, n);
        else {
            if (d >= fs->dev.block_count || !fs->dev.read(fs->dev.ctx, d, fs->scratch)) return MINTFS_EIO;
            mcopy(buf + done, fs->scratch + within, n);
        }
        done += n;
    }
    return done;
}

/* Writes bytes into file block fb (COW for committed blocks). */
static int write_block_part(MINTFS *fs, MINTFS_CACHE_ENTRY *ie, uint64_t fb, uint32_t within, const uint8_t *src, uint32_t n) {
    uint64_t old = fmap_get(fs, ie->data, fb);
    uint64_t target = old;
    if (n < MINTFS_BLOCK) {
        if (old) { if (!fs->dev.read(fs->dev.ctx, old, fs->scratch2)) return MINTFS_EIO; }
        else mzero(fs->scratch2, MINTFS_BLOCK);
    }
    if (!old || !is_fresh(fs, old)) {
        ie->pinned = 1;
        target = alloc_block(fs);
        ie->pinned = 0;
        if (!target) return MINTFS_ENOSPC;
    }
    const uint8_t *out = src;
    if (n < MINTFS_BLOCK) { mcopy(fs->scratch2 + within, src, n); out = fs->scratch2; }
    if (!fs->dev.write(fs->dev.ctx, target, out)) return MINTFS_EIO;
    if (target != old) {
        int rc = fmap_set(fs, ie, fb, target);
        if (rc) return rc;
        if (old) free_block(fs, old); else p64(ie->data + I_BLOCKS, g64(ie->data + I_BLOCKS) + 1u);
    }
    return 0;
}

static int convert_to_blocks(MINTFS *fs, MINTFS_CACHE_ENTRY *ie) {
    uint8_t *in = ie->data;
    if (!(g32(in + I_FLAGS) & FLAG_INLINE)) return 0;
    uint64_t size = g64(in + I_SIZE);
    uint8_t tmp[INLINE_MAX];
    mcopy(tmp, in + INODE_DATA, size);
    mzero(in + INODE_DATA, INLINE_MAX);
    p32(in + I_FLAGS, g32(in + I_FLAGS) & ~FLAG_INLINE);
    if (!size) return 0;
    uint8_t blockbuf[MINTFS_BLOCK];
    mzero(blockbuf, sizeof(blockbuf));
    mcopy(blockbuf, tmp, size);
    return write_block_part(fs, ie, 0, 0, blockbuf, MINTFS_BLOCK);
}

/* Frees every data/pointer block at or beyond file block `from`. */
static void free_from(MINTFS *fs, MINTFS_CACHE_ENTRY *ie, uint64_t from) {
    uint8_t *in = ie->data;
    /* Inline files keep their bytes where the pointer table would be: there is
       nothing to free, and reading those bytes as pointers would free blocks
       that belong to other files. */
    if (g32(in + I_FLAGS) & FLAG_INLINE) return;
    for (uint64_t fb = from; fb < DIRECT; ++fb) {
        uint64_t d = g64(in + INODE_DATA + 8u * fb);
        if (d) { free_block(fs, d); p64(in + INODE_DATA + 8u * fb, 0); p64(in + I_BLOCKS, g64(in + I_BLOCKS) - 1u); }
    }
    uint64_t start = from > DIRECT ? from - DIRECT : 0;
    uint64_t ind = g64(in + I_IND);
    if (ind && start < FANOUT) {
        ie->pinned = 1;
        MINTFS_CACHE_ENTRY *p = start ? meta_cow(fs, meta_get(fs, ind, TAG_PTRS)) : meta_get(fs, ind, TAG_PTRS);
        if (p) {
            for (uint64_t i = start; i < FANOUT; ++i) {
                uint64_t d = g64(p->data + HDR + 8u * i);
                if (d) { free_block(fs, d); if (start) p64(p->data + HDR + 8u * i, 0); p64(in + I_BLOCKS, g64(in + I_BLOCKS) - 1u); }
            }
            if (!start) { free_block(fs, p->addr); if (p->dirty) { p->dirty = 0; --fs->dirty_blocks; } p->valid = 0; p64(in + I_IND, 0); p64(in + I_BLOCKS, g64(in + I_BLOCKS) - 1u); }
            else p64(in + I_IND, p->addr);
        }
        ie->pinned = 0;
    }
    uint64_t dstart = from > DIRECT + FANOUT ? from - DIRECT - FANOUT : 0;
    uint64_t dind = g64(in + I_DIND);
    if (dind) {
        ie->pinned = 1;
        MINTFS_CACHE_ENTRY *p = dstart ? meta_cow(fs, meta_get(fs, dind, TAG_PTRS)) : meta_get(fs, dind, TAG_PTRS);
        if (p) {
            p->pinned = 1;
            for (uint64_t m = dstart / FANOUT; m < FANOUT; ++m) {
                uint64_t ma = g64(p->data + HDR + 8u * m);
                if (!ma) continue;
                uint64_t first = (m == dstart / FANOUT) ? dstart % FANOUT : 0;
                MINTFS_CACHE_ENTRY *q = first ? meta_cow(fs, meta_get(fs, ma, TAG_PTRS)) : meta_get(fs, ma, TAG_PTRS);
                if (!q) continue;
                for (uint64_t i = first; i < FANOUT; ++i) {
                    uint64_t d = g64(q->data + HDR + 8u * i);
                    if (d) { free_block(fs, d); if (first) p64(q->data + HDR + 8u * i, 0); p64(in + I_BLOCKS, g64(in + I_BLOCKS) - 1u); }
                }
                if (!first) {
                    free_block(fs, q->addr); if (q->dirty) { q->dirty = 0; --fs->dirty_blocks; } q->valid = 0;
                    if (dstart) p64(p->data + HDR + 8u * m, 0);
                    p64(in + I_BLOCKS, g64(in + I_BLOCKS) - 1u);
                } else p64(p->data + HDR + 8u * m, q->addr);
            }
            p->pinned = 0;
            if (!dstart) { free_block(fs, p->addr); if (p->dirty) { p->dirty = 0; --fs->dirty_blocks; } p->valid = 0; p64(in + I_DIND, 0); p64(in + I_BLOCKS, g64(in + I_BLOCKS) - 1u); }
            else p64(in + I_DIND, p->addr);
        }
        ie->pinned = 0;
    }
}

static int inode_truncate(MINTFS *fs, MINTFS_CACHE_ENTRY *ie, uint64_t size) {
    uint8_t *in = ie->data;
    uint64_t old = g64(in + I_SIZE);
    if (size > MAX_FILE_BLOCKS * MINTFS_BLOCK) return MINTFS_EFBIG;
    if (g32(in + I_FLAGS) & FLAG_INLINE) {
        if (size <= INLINE_MAX) {
            if (size > old) mzero(in + INODE_DATA + old, size - old);
            p64(in + I_SIZE, size);
            return 0;
        }
        int rc = convert_to_blocks(fs, ie);
        if (rc) return rc;
    }
    if (size < old) {
        free_from(fs, ie, (size + MINTFS_BLOCK - 1u) / MINTFS_BLOCK);
        if (!size) { mzero(in + INODE_DATA, INLINE_MAX); p32(in + I_FLAGS, g32(in + I_FLAGS) | FLAG_INLINE); }
    } else if (size > old && (old % MINTFS_BLOCK)) {
        /* bytes after the old end inside its last block must read as zeros */
        uint64_t fb = old / MINTFS_BLOCK;
        if (fmap_get(fs, in, fb)) {
            uint8_t zeros[MINTFS_BLOCK];
            uint32_t within = (uint32_t)(old % MINTFS_BLOCK);
            mzero(zeros, sizeof(zeros));
            int rc = write_block_part(fs, ie, fb, within, zeros, MINTFS_BLOCK - within);
            if (rc) return rc;
        }
    }
    p64(in + I_SIZE, size);
    return 0;
}

/* ------------------------------------------------------------------ transactions */
static int ensure_room(MINTFS *fs) {
    if (fs->failed) return MINTFS_EIO;
    if (fs->read_only) return MINTFS_EROFS;
    if (fs->dirty_blocks > MINTFS_CACHE_BLOCKS / 2u) return mintfs_commit(fs);
    return 0;
}
static void write_bitmap_block(MINTFS *fs, uint64_t i, uint64_t addr, uint8_t *buf) {
    mzero(buf, MINTFS_BLOCK);
    uint64_t first = i * BITS_PER_BMAP, n = fs->dev.block_count - first;
    if (n > BITS_PER_BMAP) n = BITS_PER_BMAP;
    for (uint64_t b = 0; b < n; ++b) if (bit(fs->used, first + b)) buf[HDR + (b >> 3)] |= (uint8_t)(1u << (b & 7u));
    seal(buf, TAG_BMAP, fs->generation, addr, i);
}

int mintfs_commit(MINTFS *fs) {
    if (!fs->mounted) return MINTFS_EINVAL;
    if (fs->failed) return MINTFS_EIO;
    if (fs->read_only) return 0;
    uint64_t nb = bmap_count(fs->dev.block_count);
    int any_bitmap = 0;
    for (uint64_t i = 0; i < nb; ++i) any_bitmap |= fs->bitmap_dirty[i];
    if (!fs->dirty_blocks && !any_bitmap) return 0;
    /* Relocate changed bitmap blocks and the index (allocating changes bits; loop until stable). */
    fs->in_commit = 1;
    MINTFS_CACHE_ENTRY *idx = meta_get(fs, sb_get(fs, SB_BIDX), TAG_BIDX);
    if (!idx) { fs->in_commit = 0; fs->failed = 1; return MINTFS_EIO; }
    idx->pinned = 1;
    for (int changed = 1, guard = 0; changed && guard < 64; ++guard) {
        changed = 0;
        for (uint64_t i = 0; i < nb; ++i) {
            uint64_t addr = g64(idx->data + HDR + 8u * i);
            if (!fs->bitmap_dirty[i] || is_fresh(fs, addr)) continue;
            uint64_t moved = alloc_block(fs);
            if (!moved || !meta_cow(fs, idx)) { idx->pinned = 0; fs->in_commit = 0; fs->failed = 1; return MINTFS_ENOSPC; }
            free_block(fs, addr);
            p64(idx->data + HDR + 8u * i, moved);
            changed = 1;
        }
    }
    if (!meta_cow(fs, idx)) { idx->pinned = 0; fs->in_commit = 0; fs->failed = 1; return MINTFS_ENOSPC; }
    idx->pinned = 0;
    fs->in_commit = 0;
    sb_set(fs, SB_BIDX, idx->addr);
    /* Write metadata, then bitmap blocks, then flush. */
    for (uint32_t i = 0; i < MINTFS_CACHE_BLOCKS; ++i) {
        MINTFS_CACHE_ENTRY *e = &fs->cache[i];
        if (!e->valid || !e->dirty) continue;
        reseal(e->data, fs->generation, e->addr);
        if (!fs->dev.write(fs->dev.ctx, e->addr, e->data)) { fs->failed = 1; return MINTFS_EIO; }
    }
    for (uint64_t i = 0; i < nb; ++i) {
        if (!fs->bitmap_dirty[i]) continue;
        uint64_t addr = g64(idx->data + HDR + 8u * i);
        write_bitmap_block(fs, i, addr, fs->scratch);
        if (!fs->dev.write(fs->dev.ctx, addr, fs->scratch)) { fs->failed = 1; return MINTFS_EIO; }
    }
    if (!fs->dev.flush(fs->dev.ctx)) { fs->failed = 1; return MINTFS_EIO; }
    sb_set(fs, SB_MODIFIED, (uint64_t)now(fs));
    seal(fs->sb, TAG_SUPER, fs->generation, fs->generation & 1u, 0);
    if (!fs->dev.write(fs->dev.ctx, fs->generation & 1u, fs->sb) || !fs->dev.flush(fs->dev.ctx)) { fs->failed = 1; return MINTFS_EIO; }
    /* The new state is durable: reserved blocks become free, fresh blocks become committed. */
    mzero(fs->reserved, fs->bitmap_bytes);
    mzero(FRESH(fs), fs->bitmap_bytes);
    mzero(fs->bitmap_dirty, nb);
    for (uint32_t i = 0; i < MINTFS_CACHE_BLOCKS; ++i) fs->cache[i].dirty = 0;
    fs->dirty_blocks = 0;
    ++fs->generation;
    return 0;
}

/* ------------------------------------------------------------------ format / mount */
int mintfs_format(const MINTFS_DEV *dev, const char *label, const uint8_t uuid[16]) {
    uint64_t n = dev->block_count;
    if (n < 64u || n > MINTFS_MAX_BLOCKS) return MINTFS_EINVAL;
    uint64_t nb = bmap_count(n);
    /* layout: 0,1 superblocks; 2 bitmap index; 3..3+nb-1 bitmaps; imap root; imap leaf 0; root inode */
    uint64_t bidx = 2, bmap0 = 3, imap_root = 3 + nb, imap_leaf = imap_root + 1, root_ino = imap_leaf + 1, first_free = root_ino + 1;
    static uint8_t b[MINTFS_BLOCK];
    const uint64_t gen = 1;
    int64_t t = dev->now ? dev->now(dev->ctx) : 0;
    mzero(b, sizeof(b));
    if (!dev->write(dev->ctx, 0, b)) return MINTFS_EIO;           /* slot 0 stays invalid until generation 2 */
    for (uint64_t i = 0; i < nb; ++i) {
        mzero(b, sizeof(b));
        uint64_t first = i * BITS_PER_BMAP;
        for (uint64_t k = 0; k < BITS_PER_BMAP && first + k < n; ++k)
            if (first + k < first_free) b[HDR + (k >> 3)] |= (uint8_t)(1u << (k & 7u));
        seal(b, TAG_BMAP, gen, bmap0 + i, i);
        if (!dev->write(dev->ctx, bmap0 + i, b)) return MINTFS_EIO;
    }
    mzero(b, sizeof(b));
    for (uint64_t i = 0; i < nb; ++i) p64(b + HDR + 8u * i, bmap0 + i);
    seal(b, TAG_BIDX, gen, bidx, 0);
    if (!dev->write(dev->ctx, bidx, b)) return MINTFS_EIO;
    mzero(b, sizeof(b));
    p64(b + HDR, imap_leaf);
    seal(b, TAG_IMAP, gen, imap_root, 0);
    if (!dev->write(dev->ctx, imap_root, b)) return MINTFS_EIO;
    mzero(b, sizeof(b));
    p64(b + HDR + 8u * MINTFS_ROOT_INO, root_ino);
    seal(b, TAG_IMAP, gen, imap_leaf, 0);
    if (!dev->write(dev->ctx, imap_leaf, b)) return MINTFS_EIO;
    mzero(b, sizeof(b));
    p32(b + I_TYPE, MINTFS_TYPE_DIR); p32(b + I_MODE, 0755u); p32(b + I_NLINK, 1u); p32(b + I_FLAGS, FLAG_INLINE);
    p64(b + I_BTIME, (uint64_t)t); p64(b + I_MTIME, (uint64_t)t); p64(b + I_CTIME, (uint64_t)t); p64(b + I_PARENT, MINTFS_ROOT_INO);
    seal(b, TAG_INODE, gen, root_ino, MINTFS_ROOT_INO);
    if (!dev->write(dev->ctx, root_ino, b)) return MINTFS_EIO;
    if (!dev->flush(dev->ctx)) return MINTFS_EIO;
    mzero(b, sizeof(b));
    mcopy(b + SB_SIG, SIGNATURE, 8);
    p32(b + SB_VERSION, MINTFS_VERSION); p32(b + SB_BSIZE, MINTFS_BLOCK);
    p64(b + SB_BLOCKS, n); p64(b + SB_IMAP, imap_root); p64(b + SB_BIDX, bidx); p32(b + SB_NBMAP, (uint32_t)nb);
    p64(b + SB_FREE, n - first_free); p64(b + SB_INODES, 1);
    if (uuid) mcopy(b + SB_UUID, uuid, 16);
    for (uint32_t i = 0; label && label[i] && i < 31u; ++i) b[SB_LABEL + i] = (uint8_t)label[i];
    p64(b + SB_CREATED, (uint64_t)t); p64(b + SB_MODIFIED, (uint64_t)t);
    seal(b, TAG_SUPER, gen, 1, 0);
    if (!dev->write(dev->ctx, 1, b) || !dev->flush(dev->ctx)) return MINTFS_EIO;
    return 0;
}

static int sb_valid(const uint8_t *b, uint64_t slot, uint64_t dev_blocks) {
    if (!verify(b, TAG_SUPER, slot, UINT64_MAX)) return 0;
    for (int i = 0; i < 8; ++i) if (b[SB_SIG + i] != (uint8_t)SIGNATURE[i]) return 0;
    if (g32(b + SB_VERSION) != MINTFS_VERSION || g32(b + SB_BSIZE) != MINTFS_BLOCK) return 0;
    uint64_t n = g64(b + SB_BLOCKS);
    if (n < 64u || n > dev_blocks || n > MINTFS_MAX_BLOCKS || g32(b + SB_NBMAP) != bmap_count(n)) return 0;
    if ((g64(b + 8) & 1u) != slot) return 0;
    return 1;
}

int mintfs_mount(MINTFS *fs, const MINTFS_DEV *dev, uint8_t *bitmaps, size_t bitmaps_len, int read_only) {
    mzero(fs, sizeof(*fs));
    fs->dev = *dev;
    fs->device = *dev;
    fs->bitmaps = bitmaps; fs->bitmaps_len = bitmaps_len;
    static uint8_t a[MINTFS_BLOCK], c[MINTFS_BLOCK];
    int va = dev->read(dev->ctx, 0, a) && sb_valid(a, 0, dev->block_count);
    int vc = dev->read(dev->ctx, 1, c) && sb_valid(c, 1, dev->block_count);
    if (!va && !vc) return MINTFS_ECORRUPT;
    const uint8_t *best = (va && (!vc || g64(a + 8) > g64(c + 8))) ? a : c;
    mcopy(fs->sb, best, MINTFS_BLOCK);
    fs->dev.block_count = g64(fs->sb + SB_BLOCKS);
    uint64_t n = fs->dev.block_count, nb = bmap_count(n);
    fs->bitmap_bytes = mintfs_bitmap_bytes(n);
    if (bitmaps_len < 3u * fs->bitmap_bytes + nb) return MINTFS_ENOMEM;
    fs->used = bitmaps; fs->reserved = bitmaps + fs->bitmap_bytes; fs->bitmap_dirty = bitmaps + 3u * fs->bitmap_bytes;
    mzero(bitmaps, 3u * fs->bitmap_bytes + nb);
    fs->generation = g64(fs->sb + 8) + 1u;
    fs->mounted = 1;
    /* Load the free-space bitmap of the committed state. */
    MINTFS_CACHE_ENTRY *idx = meta_get(fs, sb_get(fs, SB_BIDX), TAG_BIDX);
    if (!idx) { fs->mounted = 0; return MINTFS_ECORRUPT; }
    uint64_t free_count = 0;
    for (uint64_t i = 0; i < nb; ++i) {
        uint64_t addr = g64(idx->data + HDR + 8u * i);
        if (addr < 2u || addr >= n || !dev->read(dev->ctx, addr, fs->scratch) || !verify(fs->scratch, TAG_BMAP, addr, fs->generation)) {
            fs->mounted = 0; return MINTFS_ECORRUPT;
        }
        uint64_t first = i * BITS_PER_BMAP;
        for (uint64_t k = 0; k < BITS_PER_BMAP && first + k < n; ++k)
            if ((fs->scratch[HDR + (k >> 3)] >> (k & 7u)) & 1u) set_bit(fs->used, first + k, 1); else ++free_count;
    }
    if (free_count != sb_get(fs, SB_FREE)) { fs->mounted = 0; return MINTFS_ECORRUPT; }
    if (!bit(fs->used, 0) || !bit(fs->used, 1) || !inode_get(fs, MINTFS_ROOT_INO)) { fs->mounted = 0; return MINTFS_ECORRUPT; }
    fs->read_only = read_only;
    fs->alloc_hint = 2; fs->inode_hint = 2;
    return 0;
}

/* Drops the open transaction and returns to the last committed state. */
int mintfs_abort(MINTFS *fs) {
    if (!fs->mounted) return MINTFS_EINVAL;
    MINTFS_DEV dev = fs->device;
    int ro = fs->read_only;
    return mintfs_mount(fs, &dev, fs->bitmaps, fs->bitmaps_len, ro);
}

int mintfs_unmount(MINTFS *fs) {
    int rc = fs->mounted && !fs->read_only && !fs->failed ? mintfs_commit(fs) : 0;
    fs->mounted = 0;
    return rc;
}

/* ------------------------------------------------------------------ directories */
static uint8_t dirbuf[DIR_MAX];
/* Reads a directory's records; returns their total length or an error. */
static int64_t dir_load(MINTFS *fs, uint64_t dir) {
    MINTFS_CACHE_ENTRY *e = inode_get(fs, dir);
    if (!e) return MINTFS_ENOENT;
    if (g32(e->data + I_TYPE) != MINTFS_TYPE_DIR) return MINTFS_ENOTDIR;
    uint64_t size = g64(e->data + I_SIZE);
    if (size > DIR_MAX) return MINTFS_ECORRUPT;
    e->pinned = 1;
    int64_t got = inode_read(fs, e->data, 0, dirbuf, (uint32_t)size);
    e->pinned = 0;
    return got == (int64_t)size ? got : MINTFS_EIO;
}
/* Finds `name` in dirbuf[0..len); returns its offset or -1. */
static int64_t dir_find(int64_t len, const char *name, uint64_t *ino, uint32_t *type) {
    for (int64_t at = 0; at + 10 <= len;) {
        uint8_t nl = dirbuf[at + 9];
        if (at + 10 + nl > len) return -2;
        if (name_eq(name, dirbuf + at + 10, nl)) {
            if (ino) *ino = g64(dirbuf + at);
            if (type) *type = dirbuf[at + 8];
            return at;
        }
        at += 10 + nl;
    }
    return -1;
}
static int dir_store(MINTFS *fs, uint64_t dir, int64_t len) {
    MINTFS_CACHE_ENTRY *e = inode_cow(fs, dir);
    if (!e) return MINTFS_EIO;
    e->pinned = 1;
    int rc = inode_truncate(fs, e, (uint64_t)len);
    if (!rc && len) {
        if (g32(e->data + I_FLAGS) & FLAG_INLINE) mcopy(e->data + INODE_DATA, dirbuf, (size_t)len);
        else {
            for (int64_t at = 0; !rc && at < len; at += MINTFS_BLOCK) {
                uint32_t n = (uint32_t)(len - at < MINTFS_BLOCK ? len - at : MINTFS_BLOCK);
                rc = write_block_part(fs, e, (uint64_t)at / MINTFS_BLOCK, 0, dirbuf + at, n);
            }
        }
    }
    int64_t t = now(fs);
    p64(e->data + I_MTIME, (uint64_t)t); p64(e->data + I_CTIME, (uint64_t)t);
    e->pinned = 0;
    return rc;
}
static int dir_add(MINTFS *fs, uint64_t dir, const char *name, size_t nl, uint64_t ino, uint32_t type) {
    int64_t len = dir_load(fs, dir);
    if (len < 0) return (int)len;
    if (dir_find(len, name, 0, 0) >= 0) return MINTFS_EEXIST;
    if ((uint64_t)len + 10u + nl > DIR_MAX) return MINTFS_ENOSPC;
    p64(dirbuf + len, ino); dirbuf[len + 8] = (uint8_t)type; dirbuf[len + 9] = (uint8_t)nl;
    mcopy(dirbuf + len + 10, name, nl);
    return dir_store(fs, dir, len + 10 + (int64_t)nl);
}
static int dir_remove(MINTFS *fs, uint64_t dir, const char *name) {
    int64_t len = dir_load(fs, dir);
    if (len < 0) return (int)len;
    int64_t at = dir_find(len, name, 0, 0);
    if (at < 0) return MINTFS_ENOENT;
    int64_t rl = 10 + dirbuf[at + 9];
    mmove(dirbuf + at, dirbuf + at + rl, (size_t)(len - at - rl));
    return dir_store(fs, dir, len - rl);
}

/* ------------------------------------------------------------------ public operations */
int mintfs_lookup(MINTFS *fs, uint64_t dir, const char *name, uint64_t *ino) {
    if (!fs->mounted) return MINTFS_EINVAL;
    int64_t len = dir_load(fs, dir);
    if (len < 0) return (int)len;
    if (name[0] == '.' && !name[1]) { *ino = dir; return 0; }
    if (name[0] == '.' && name[1] == '.' && !name[2]) {
        MINTFS_CACHE_ENTRY *e = inode_get(fs, dir);
        *ino = e ? g64(e->data + I_PARENT) : MINTFS_ROOT_INO;
        return 0;
    }
    return dir_find(len, name, ino, 0) >= 0 ? 0 : MINTFS_ENOENT;
}

int mintfs_create(MINTFS *fs, uint64_t dir, const char *name, uint32_t type, uint64_t *ino_out) {
    size_t nl;
    int rc = name_ok(name, &nl);
    if (rc) return rc;
    if (type != MINTFS_TYPE_FILE && type != MINTFS_TYPE_DIR) return MINTFS_EINVAL;
    if ((rc = ensure_room(fs))) return rc;
    int64_t len = dir_load(fs, dir);
    if (len < 0) return (int)len;
    if (dir_find(len, name, 0, 0) >= 0) return MINTFS_EEXIST;
    uint64_t ino = inode_alloc_number(fs);
    if (!ino) return MINTFS_ENOSPC;
    MINTFS_CACHE_ENTRY *e = meta_new(fs, TAG_INODE, ino);
    if (!e) return MINTFS_ENOSPC;
    int64_t t = now(fs);
    p32(e->data + I_TYPE, type); p32(e->data + I_MODE, type == MINTFS_TYPE_DIR ? 0755u : 0644u);
    p32(e->data + I_NLINK, 1u); p32(e->data + I_FLAGS, FLAG_INLINE);
    p64(e->data + I_BTIME, (uint64_t)t); p64(e->data + I_MTIME, (uint64_t)t); p64(e->data + I_CTIME, (uint64_t)t);
    p64(e->data + I_PARENT, dir);
    e->pinned = 1;
    rc = imap_set(fs, ino, e->addr);
    e->pinned = 0;
    if (rc) return rc;
    if ((rc = dir_add(fs, dir, name, nl, ino, type))) return rc;
    sb_set(fs, SB_INODES, sb_get(fs, SB_INODES) + 1u);
    if (ino_out) *ino_out = ino;
    return 0;
}

int64_t mintfs_read(MINTFS *fs, uint64_t ino, uint64_t offset, void *buf, uint32_t len) {
    if (!fs->mounted) return MINTFS_EINVAL;
    MINTFS_CACHE_ENTRY *e = inode_get(fs, ino);
    if (!e) return MINTFS_ENOENT;
    if (g32(e->data + I_TYPE) == MINTFS_TYPE_DIR) return MINTFS_EISDIR;
    e->pinned = 1;
    int64_t rc = inode_read(fs, e->data, offset, (uint8_t *)buf, len);
    e->pinned = 0;
    return rc;
}

int64_t mintfs_write(MINTFS *fs, uint64_t ino, uint64_t offset, const void *data, uint32_t len) {
    int rc = ensure_room(fs);
    if (rc) return rc;
    MINTFS_CACHE_ENTRY *e = inode_get(fs, ino);
    if (!e) return MINTFS_ENOENT;
    if (g32(e->data + I_TYPE) == MINTFS_TYPE_DIR) return MINTFS_EISDIR;
    if (offset + len > MAX_FILE_BLOCKS * MINTFS_BLOCK) return MINTFS_EFBIG;
    e = inode_cow(fs, ino);
    if (!e) return MINTFS_EIO;
    e->pinned = 1;
    uint64_t end = offset + len, size = g64(e->data + I_SIZE);
    const uint8_t *src = (const uint8_t *)data;
    int64_t result = len;
    if (end > size && (rc = inode_truncate(fs, e, end))) { e->pinned = 0; return rc; }
    if (g32(e->data + I_FLAGS) & FLAG_INLINE) mcopy(e->data + INODE_DATA + offset, src, len);
    else {
        uint32_t done = 0;
        while (done < len) {
            uint64_t pos = offset + done;
            uint32_t within = (uint32_t)(pos % MINTFS_BLOCK), n = MINTFS_BLOCK - within;
            if (n > len - done) n = len - done;
            if ((rc = write_block_part(fs, e, pos / MINTFS_BLOCK, within, src + done, n))) { result = rc; break; }
            done += n;
            if (fs->dirty_blocks > MINTFS_CACHE_BLOCKS - 16u) { result = done; break; }   /* caller writes the rest after a commit */
        }
    }
    int64_t t = now(fs);
    p64(e->data + I_MTIME, (uint64_t)t); p64(e->data + I_CTIME, (uint64_t)t);
    e->pinned = 0;
    return result;
}

int mintfs_truncate(MINTFS *fs, uint64_t ino, uint64_t size) {
    int rc = ensure_room(fs);
    if (rc) return rc;
    MINTFS_CACHE_ENTRY *e = inode_get(fs, ino);
    if (!e) return MINTFS_ENOENT;
    if (g32(e->data + I_TYPE) == MINTFS_TYPE_DIR) return MINTFS_EISDIR;
    e = inode_cow(fs, ino);
    if (!e) return MINTFS_EIO;
    e->pinned = 1;
    rc = inode_truncate(fs, e, size);
    int64_t t = now(fs);
    p64(e->data + I_MTIME, (uint64_t)t); p64(e->data + I_CTIME, (uint64_t)t);
    e->pinned = 0;
    return rc;
}

static int inode_destroy(MINTFS *fs, uint64_t ino) {
    MINTFS_CACHE_ENTRY *e = inode_cow(fs, ino);
    if (!e) return MINTFS_EIO;
    e->pinned = 1;
    free_from(fs, e, 0);
    e->pinned = 0;
    free_block(fs, e->addr);
    if (e->dirty) { e->dirty = 0; --fs->dirty_blocks; }
    e->valid = 0;
    sb_set(fs, SB_INODES, sb_get(fs, SB_INODES) - 1u);
    return imap_set(fs, ino, 0);
}

int mintfs_unlink(MINTFS *fs, uint64_t dir, const char *name) {
    size_t nl;
    int rc = name_ok(name, &nl);
    if (rc) return rc;
    if ((rc = ensure_room(fs))) return rc;
    int64_t len = dir_load(fs, dir);
    if (len < 0) return (int)len;
    uint64_t ino; uint32_t type;
    if (dir_find(len, name, &ino, &type) < 0) return MINTFS_ENOENT;
    MINTFS_CACHE_ENTRY *e = inode_get(fs, ino);
    if (!e) return MINTFS_ECORRUPT;
    if (g32(e->data + I_TYPE) == MINTFS_TYPE_DIR && g64(e->data + I_SIZE)) return MINTFS_ENOTEMPTY;
    if ((rc = dir_remove(fs, dir, name))) return rc;
    return inode_destroy(fs, ino);
}

int mintfs_rename(MINTFS *fs, uint64_t sdir, const char *sname, uint64_t ddir, const char *dname) {
    size_t snl, dnl;
    int rc = name_ok(sname, &snl);
    if (rc || (rc = name_ok(dname, &dnl))) return rc;
    if ((rc = ensure_room(fs))) return rc;
    int64_t len = dir_load(fs, sdir);
    if (len < 0) return (int)len;
    uint64_t ino; uint32_t type;
    if (dir_find(len, sname, &ino, &type) < 0) return MINTFS_ENOENT;
    /* A directory may not move into itself or its own subtree. */
    if (type == MINTFS_TYPE_DIR) {
        uint64_t cur = ddir;
        for (int depth = 0; depth < 4096; ++depth) {
            if (cur == ino) return MINTFS_EINVAL;
            if (cur == MINTFS_ROOT_INO) break;
            MINTFS_CACHE_ENTRY *c = inode_get(fs, cur);
            if (!c) return MINTFS_ECORRUPT;
            cur = g64(c->data + I_PARENT);
        }
    }
    int64_t dlen = dir_load(fs, ddir);
    if (dlen < 0) return (int)dlen;
    uint64_t existing; uint32_t etype;
    if (dir_find(dlen, dname, &existing, &etype) >= 0) {
        if (existing == ino) {                               /* same entry: only the case of the name changes */
            if (sdir == ddir) { if ((rc = dir_remove(fs, sdir, sname))) return rc; return dir_add(fs, ddir, dname, dnl, ino, type); }
            return 0;
        }
        MINTFS_CACHE_ENTRY *x = inode_get(fs, existing);
        if (!x) return MINTFS_ECORRUPT;
        if (etype == MINTFS_TYPE_DIR) {
            if (type != MINTFS_TYPE_DIR) return MINTFS_EISDIR;
            if (g64(x->data + I_SIZE)) return MINTFS_ENOTEMPTY;
        } else if (type == MINTFS_TYPE_DIR) return MINTFS_ENOTDIR;
        if ((rc = dir_remove(fs, ddir, dname)) || (rc = inode_destroy(fs, existing))) return rc;
    }
    if ((rc = dir_remove(fs, sdir, sname))) return rc;
    if ((rc = dir_add(fs, ddir, dname, dnl, ino, type))) return rc;
    if (sdir != ddir) {
        MINTFS_CACHE_ENTRY *e = inode_cow(fs, ino);
        if (!e) return MINTFS_EIO;
        p64(e->data + I_PARENT, ddir);
        p64(e->data + I_CTIME, (uint64_t)now(fs));
    }
    return 0;
}

int mintfs_readdir(MINTFS *fs, uint64_t dir, uint32_t index, MINTFS_DIRENT *out) {
    if (!fs->mounted) return MINTFS_EINVAL;
    int64_t len = dir_load(fs, dir);
    if (len < 0) return (int)len;
    uint32_t i = 0;
    for (int64_t at = 0; at + 10 <= len; ++i) {
        uint8_t nl = dirbuf[at + 9];
        if (at + 10 + nl > len) return MINTFS_ECORRUPT;
        if (i == index) {
            out->ino = g64(dirbuf + at); out->type = dirbuf[at + 8];
            mcopy(out->name, dirbuf + at + 10, nl); out->name[nl] = '\0';
            return 1;
        }
        at += 10 + nl;
    }
    return 0;
}

int mintfs_list(MINTFS *fs, uint64_t dir, uint32_t start, MINTFS_DIRENT *out, uint32_t capacity) {
    if (!fs->mounted) return MINTFS_EINVAL;
    int64_t len = dir_load(fs, dir);
    if (len < 0) return (int)len;
    uint32_t i = 0, n = 0;
    for (int64_t at = 0; at + 10 <= len && n < capacity; ++i) {
        uint8_t nl = dirbuf[at + 9];
        if (at + 10 + nl > len) return MINTFS_ECORRUPT;
        if (i >= start) {
            out[n].ino = g64(dirbuf + at); out[n].type = dirbuf[at + 8];
            mcopy(out[n].name, dirbuf + at + 10, nl); out[n].name[nl] = '\0';
            ++n;
        }
        at += 10 + nl;
    }
    return (int)n;
}

int mintfs_stat(MINTFS *fs, uint64_t ino, MINTFS_STAT *st) {
    if (!fs->mounted) return MINTFS_EINVAL;
    MINTFS_CACHE_ENTRY *e = inode_get(fs, ino);
    if (!e) return MINTFS_ENOENT;
    const uint8_t *in = e->data;
    st->ino = ino; st->type = g32(in + I_TYPE); st->mode = g32(in + I_MODE); st->nlink = g32(in + I_NLINK); st->reserved = 0;
    st->size = g64(in + I_SIZE); st->blocks = g64(in + I_BLOCKS);
    st->btime = (int64_t)g64(in + I_BTIME); st->mtime = (int64_t)g64(in + I_MTIME); st->ctime = (int64_t)g64(in + I_CTIME);
    st->parent = g64(in + I_PARENT);
    return 0;
}

int mintfs_info(MINTFS *fs, MINTFS_INFO *out) {
    if (!fs->mounted) return MINTFS_EINVAL;
    out->block_count = fs->dev.block_count; out->free_blocks = sb_get(fs, SB_FREE);
    out->generation = fs->generation - 1u; out->inodes = sb_get(fs, SB_INODES);
    mcopy(out->label, fs->sb + SB_LABEL, 32); out->label[31] = '\0';
    mcopy(out->uuid, fs->sb + SB_UUID, 16);
    out->created = (int64_t)sb_get(fs, SB_CREATED); out->modified = (int64_t)sb_get(fs, SB_MODIFIED);
    return 0;
}

int mintfs_resolve(MINTFS *fs, const char *path, uint64_t *ino) {
    if (!path || path[0] != '/') return MINTFS_EINVAL;
    uint64_t cur = MINTFS_ROOT_INO;
    char part[MINTFS_NAME_MAX + 1];
    const char *p = path;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        size_t n = 0;
        while (p[n] && p[n] != '/') { if (n >= MINTFS_NAME_MAX) return MINTFS_ENAMETOOLONG; part[n] = p[n]; ++n; }
        part[n] = '\0';
        p += n;
        int rc = mintfs_lookup(fs, cur, part, &cur);
        if (rc) return rc;
    }
    *ino = cur;
    return 0;
}

/* ------------------------------------------------------------------ fsck */
static uint64_t al8(uint64_t v) { return (v + 7u) & ~(uint64_t)7u; }
uint64_t mintfs_check_work_bytes(uint64_t blocks) {
    uint64_t bm = al8(mintfs_bitmap_bytes(blocks));
    return 2u * bm + MINTFS_MAX_INODES * 2u + MINTFS_BLOCK + al8(3u * mintfs_bitmap_bytes(blocks) + bmap_count(blocks)) + MINTFS_MAX_INODES / 8u + 8u;
}

typedef struct ck {
    const MINTFS_DEV *dev; MINTFS_CHECK_REPORT *r; uint64_t n, gen;
    uint8_t *reach, *disk; uint16_t *refs; uint8_t *blk;
} CK;
static void problem(CK *k, const char *what) {
    if (!k->r->first_problem[0]) {
        size_t i = 0;
        for (; what[i] && i + 1u < sizeof(k->r->first_problem); ++i) k->r->first_problem[i] = what[i];
        k->r->first_problem[i] = '\0';
    }
}
static int ck_mark(CK *k, uint64_t b) {
    if (b < 2u || b >= k->n) { ++k->r->bad_pointers; problem(k, "pointer outside the filesystem"); return 0; }
    if (bit(k->reach, b)) { ++k->r->double_references; problem(k, "block referenced twice"); return 0; }
    set_bit(k->reach, b, 1);
    ++k->r->blocks_reachable;
    return 1;
}
static int ck_meta(CK *k, uint64_t b, uint32_t tag, uint8_t *buf) {
    if (!ck_mark(k, b)) return 0;
    if (!k->dev->read(k->dev->ctx, b, buf) || !verify(buf, tag, b, k->gen)) { ++k->r->bad_checksums; problem(k, "metadata block failed its checksum/header check"); return 0; }
    return 1;
}
static void ck_ptrs(CK *k, uint64_t b, int depth, uint64_t *count) {
    static uint8_t lvl[2][MINTFS_BLOCK];
    if (!ck_meta(k, b, TAG_PTRS, lvl[depth])) return;
    for (uint32_t i = 0; i < FANOUT; ++i) {
        uint64_t p = g64(lvl[depth] + HDR + 8u * i);
        if (!p) continue;
        if (depth) ck_ptrs(k, p, 0, count); else if (ck_mark(k, p)) ++*count;
    }
}

int mintfs_check(const MINTFS_DEV *dev, MINTFS_CHECK_REPORT *r, uint8_t *work, size_t work_len) {
    static MINTFS fsb;                     /* read-only mount used to read directories */
    mzero(r, sizeof(*r));
    static uint8_t sb[MINTFS_BLOCK], a[MINTFS_BLOCK], c[MINTFS_BLOCK];
    int va = dev->read(dev->ctx, 0, a) && sb_valid(a, 0, dev->block_count);
    int vc = dev->read(dev->ctx, 1, c) && sb_valid(c, 1, dev->block_count);
    if (!va && !vc) { problem(&(CK){.r = r}, "no valid superblock"); return MINTFS_ECORRUPT; }
    mcopy(sb, (va && (!vc || g64(a + 8) > g64(c + 8))) ? a : c, MINTFS_BLOCK);
    uint64_t n = g64(sb + SB_BLOCKS);
    if (work_len < mintfs_check_work_bytes(n)) return MINTFS_ENOMEM;
    uint64_t bm = al8(mintfs_bitmap_bytes(n));
    CK k = {dev, r, n, g64(sb + 8), work, work + bm, (uint16_t *)(void *)(work + 2u * bm), 0};
    k.blk = work + 2u * bm + MINTFS_MAX_INODES * 2u;
    uint8_t *mountbits = k.blk + MINTFS_BLOCK;
    uint64_t mountlen = al8(3u * mintfs_bitmap_bytes(n) + bmap_count(n));
    uint8_t *exist = mountbits + mountlen;
    mzero(work, mintfs_check_work_bytes(n));
    set_bit(k.reach, 0, 1); set_bit(k.reach, 1, 1); r->blocks_reachable = 2;
    /* bitmap index and bitmaps */
    uint64_t nb = bmap_count(n), free_count = 0;
    static uint8_t idx[MINTFS_BLOCK];
    if (ck_meta(&k, g64(sb + SB_BIDX), TAG_BIDX, idx)) {
        for (uint64_t i = 0; i < nb; ++i) {
            uint64_t ba = g64(idx + HDR + 8u * i);
            if (!ck_meta(&k, ba, TAG_BMAP, k.blk)) continue;
            uint64_t first = i * BITS_PER_BMAP;
            for (uint64_t b = 0; b < BITS_PER_BMAP && first + b < n; ++b)
                if ((k.blk[HDR + (b >> 3)] >> (b & 7u)) & 1u) set_bit(k.disk, first + b, 1); else ++free_count;
        }
    }
    if (free_count != g64(sb + SB_FREE)) { ++r->leaked_blocks; problem(&k, "free-block count disagrees with the bitmap"); }
    /* inode map and inodes */
    static uint8_t root[MINTFS_BLOCK], leaf[MINTFS_BLOCK], ino_blk[MINTFS_BLOCK];
    uint64_t inodes = 0;
    if (ck_meta(&k, g64(sb + SB_IMAP), TAG_IMAP, root)) {
        for (uint32_t li = 0; li < FANOUT; ++li) {
            uint64_t la = g64(root + HDR + 8u * li);
            if (!la || !ck_meta(&k, la, TAG_IMAP, leaf)) continue;
            for (uint32_t s = 0; s < FANOUT; ++s) {
                uint64_t ia = g64(leaf + HDR + 8u * s), ino = (uint64_t)li * FANOUT + s;
                if (!ia) continue;
                if (!ino || !ck_meta(&k, ia, TAG_INODE, ino_blk)) continue;
                if (g64(ino_blk + 24) != ino) { ++r->bad_pointers; problem(&k, "inode block owned by another inode"); continue; }
                ++inodes;
                set_bit(exist, ino, 1);
                uint32_t type = g32(ino_blk + I_TYPE);
                if (type == MINTFS_TYPE_DIR) ++r->directories; else if (type == MINTFS_TYPE_FILE) ++r->files;
                else { ++r->bad_directories; problem(&k, "inode with unknown type"); }
                uint64_t size = g64(ino_blk + I_SIZE), count = 0;
                if (g32(ino_blk + I_FLAGS) & FLAG_INLINE) { if (size > INLINE_MAX) { ++r->bad_pointers; problem(&k, "inline inode larger than its inline area"); } continue; }
                for (uint32_t d = 0; d < DIRECT; ++d) { uint64_t b = g64(ino_blk + INODE_DATA + 8u * d); if (b && ck_mark(&k, b)) ++count; }
                uint64_t ind = g64(ino_blk + I_IND), dind = g64(ino_blk + I_DIND), meta = 0;
                if (ind) { ck_ptrs(&k, ind, 0, &count); ++meta; }
                if (dind) {
                    static uint8_t top[MINTFS_BLOCK];
                    if (ck_meta(&k, dind, TAG_PTRS, top)) {
                        ++meta;
                        for (uint32_t m = 0; m < FANOUT; ++m) { uint64_t ma = g64(top + HDR + 8u * m); if (ma) { ck_ptrs(&k, ma, 0, &count); ++meta; } }
                    }
                }
                if (count + meta != g64(ino_blk + I_BLOCKS)) { ++r->bad_pointers; problem(&k, "inode block count is wrong"); }
                if (size > (count + 1u) * (uint64_t)MINTFS_BLOCK * 512u) { /* sparse files allowed; no check */ }
            }
        }
    }
    r->inodes = inodes;
    if (inodes != g64(sb + SB_INODES)) { ++r->bad_links; problem(&k, "inode count disagrees with the inode map"); }
    /* every block in use must be reachable and vice versa */
    for (uint64_t b = 0; b < n; ++b) {
        int on = bit(k.disk, b), re = bit(k.reach, b);
        if (on) ++r->blocks_marked_used;
        if (on && !re) { if (!r->first_problem[0]) r->problem_block = b; ++r->leaked_blocks; problem(&k, "block marked used but unreachable"); }
        if (!on && re) { if (!r->first_problem[0]) r->problem_block = b; ++r->missing_blocks; problem(&k, "reachable block marked free"); }
    }
    /* Names: every inode except the root is named exactly once, by its parent directory,
       and following parent links from any directory reaches the root. */
    if (mintfs_mount(&fsb, dev, mountbits, (size_t)mountlen, 1) == 0) {
        for (uint64_t ino = 1; ino < MINTFS_MAX_INODES; ++ino) {
            if (!bit(exist, ino)) continue;
            MINTFS_STAT st;
            if (mintfs_stat(&fsb, ino, &st) || st.type != MINTFS_TYPE_DIR) continue;
            MINTFS_DIRENT de;
            for (uint32_t i = 0;; ++i) {
                int got = mintfs_readdir(&fsb, ino, i, &de);
                if (got < 0) { ++r->bad_directories; problem(&k, "directory records are malformed"); break; }
                if (!got) break;
                MINTFS_STAT es;
                if (!de.ino || de.ino >= MINTFS_MAX_INODES || !bit(exist, de.ino) || mintfs_stat(&fsb, de.ino, &es)) {
                    ++r->bad_directories; problem(&k, "directory entry points to a missing inode"); continue;
                }
                if (es.type != de.type || es.parent != ino) { ++r->bad_directories; problem(&k, "directory entry type or parent mismatch"); }
                if (k.refs[de.ino] < 0xFFFFu) ++k.refs[de.ino];
            }
        }
        for (uint64_t ino = 2; ino < MINTFS_MAX_INODES; ++ino) {
            if (!bit(exist, ino)) continue;
            if (k.refs[ino] != 1u) { ++r->bad_links; problem(&k, k.refs[ino] ? "inode named more than once" : "orphan inode (no directory entry)"); }
            MINTFS_STAT st;
            if (mintfs_stat(&fsb, ino, &st) || st.type != MINTFS_TYPE_DIR) continue;
            uint64_t cur = ino; uint32_t steps = 0;
            while (cur != MINTFS_ROOT_INO && steps++ < 4096u) { if (mintfs_stat(&fsb, cur, &st)) break; cur = st.parent; }
            if (cur != MINTFS_ROOT_INO) { ++r->bad_links; problem(&k, "directory not connected to the root"); }
        }
        fsb.mounted = 0;
    } else { ++r->bad_directories; problem(&k, "could not read directories (mount failed)"); }
    int rc = 0;
    if (r->bad_checksums || r->bad_pointers || r->double_references || r->leaked_blocks || r->missing_blocks ||
        r->bad_directories || r->bad_links) rc = MINTFS_ECORRUPT;
    return rc;
}

const char *mintfs_strerror(int code) {
    switch (code) {
    case MINTFS_OK: return "ok";
    case MINTFS_ENOENT: return "no such file or folder";
    case MINTFS_EIO: return "disk input/output error";
    case MINTFS_ENOMEM: return "not enough memory";
    case MINTFS_EEXIST: return "an item with that name already exists";
    case MINTFS_ENOTDIR: return "not a folder";
    case MINTFS_EISDIR: return "is a folder";
    case MINTFS_EINVAL: return "invalid name or argument";
    case MINTFS_EFBIG: return "file too large";
    case MINTFS_ENOSPC: return "the disk is full";
    case MINTFS_EROFS: return "the disk is read-only";
    case MINTFS_ENAMETOOLONG: return "name too long";
    case MINTFS_ENOTEMPTY: return "the folder is not empty";
    case MINTFS_ECORRUPT: return "the filesystem is damaged";
    default: return "filesystem error";
    }
}
