/* Bounded heap and arenas for the Web engine — see wmem.h. */
#include "wmem.h"

typedef struct blk {
    size_t prev;                 /* size of the physically previous block, 0 for the first */
    size_t size;                 /* this block incl. header; bit 0 = in use */
} blk;
typedef struct fblk {
    blk h;
    struct fblk *next, *prevf;   /* free list (only while free) */
} fblk;

#define HDR ((size_t)sizeof(blk))
#define MIN_BLOCK ((size_t)sizeof(fblk) + 16u)
#define USED 1u

static uint8_t *base, *limit;    /* limit = sentinel header */
static fblk *free_head;
static size_t used_bytes, peak_bytes, cap_bytes;

static size_t bsize(const blk *b) { return b->size & ~(size_t)USED; }
static blk *next_blk(blk *b) { return (blk *)((uint8_t *)b + bsize(b)); }
static blk *prev_blk(blk *b) { return b->prev ? (blk *)((uint8_t *)b - b->prev) : 0; }

static void list_remove(fblk *f) {
    if (f->prevf) f->prevf->next = f->next; else free_head = f->next;
    if (f->next) f->next->prevf = f->prevf;
    f->next = f->prevf = 0;
}
static void list_push(fblk *f) {
    f->prevf = 0; f->next = free_head;
    if (free_head) free_head->prevf = f;
    free_head = f;
}

void wb_copy(void *d, const void *s, size_t n) {
    uint8_t *o = (uint8_t *)d; const uint8_t *i = (const uint8_t *)s;
    if (o == i || !n) return;
    if (o < i || o >= i + n) { while (n--) *o++ = *i++; }
    else { o += n; i += n; while (n--) *--o = *--i; }
}
void wb_set(void *d, int v, size_t n) { uint8_t *o = (uint8_t *)d; while (n--) *o++ = (uint8_t)v; }
size_t wstrlen(const char *s) { size_t n = 0; if (s) while (s[n]) ++n; return n; }

void wmem_init(void *arena, size_t size) {
    uintptr_t a = ((uintptr_t)arena + 15u) & ~(uintptr_t)15u;
    size_t lost = (size_t)(a - (uintptr_t)arena);
    free_head = 0; used_bytes = peak_bytes = cap_bytes = 0;
    base = limit = 0;
    if (!arena || size < lost + 4u * MIN_BLOCK) return;
    size = (size - lost) & ~(size_t)15u;
    base = (uint8_t *)a;
    limit = base + size - HDR;                       /* sentinel: size 0, in use */
    fblk *f = (fblk *)base;
    f->h.prev = 0; f->h.size = size - HDR;
    ((blk *)limit)->prev = f->h.size; ((blk *)limit)->size = USED;
    list_push(f);
    cap_bytes = size - HDR;
}

size_t wmem_capacity(void) { return cap_bytes; }
size_t wmem_used(void) { return used_bytes; }
size_t wmem_peak(void) { return peak_bytes; }
size_t wmem_largest_free(void) {
    size_t m = 0;
    for (fblk *f = free_head; f; f = f->next) if (bsize(&f->h) > m) m = bsize(&f->h);
    return m > HDR ? m - HDR : 0;
}

void *wmem_alloc(size_t n) {
    if (!base || n > cap_bytes) return 0;
    size_t need = ((n + 15u) & ~(size_t)15u) + HDR;
    if (need < MIN_BLOCK) need = MIN_BLOCK;
    fblk *best = 0;
    uint32_t looked = 0;
    for (fblk *f = free_head; f; f = f->next) {        /* first fit, then a short best-fit look-ahead */
        if (bsize(&f->h) >= need && (!best || bsize(&f->h) < bsize(&best->h))) best = f;
        if (best && ++looked > 8u) break;
    }
    if (!best) return 0;
    list_remove(best);
    size_t have = bsize(&best->h);
    if (have - need >= MIN_BLOCK) {
        blk *rest = (blk *)((uint8_t *)best + need);
        rest->prev = need; rest->size = have - need;
        next_blk(rest)->prev = rest->size;
        best->h.size = need;
        list_push((fblk *)rest);
    }
    best->h.size |= USED;
    used_bytes += bsize(&best->h);
    if (used_bytes > peak_bytes) peak_bytes = used_bytes;
    return (uint8_t *)best + HDR;
}

void *wmem_calloc(size_t n) {
    void *p = wmem_alloc(n);
    if (p) wb_set(p, 0, n);
    return p;
}

static int owns(const void *p) {
    return base && (const uint8_t *)p >= base + HDR && (const uint8_t *)p < limit && !(((uintptr_t)p) & 15u);
}

void wmem_free(void *p) {
    if (!p || !owns(p)) return;
    blk *b = (blk *)((uint8_t *)p - HDR);
    if (!(b->size & USED)) return;                    /* double free: ignore */
    b->size &= ~(size_t)USED;
    used_bytes -= bsize(b);
    blk *n = next_blk(b);
    if (!(n->size & USED)) {                          /* merge with the next block */
        list_remove((fblk *)n);
        b->size += bsize(n);
        next_blk(b)->prev = b->size;
    }
    blk *pv = prev_blk(b);
    if (pv && !(pv->size & USED)) {                   /* merge into the previous block */
        pv->size += b->size;
        next_blk(pv)->prev = pv->size;
        return;                                       /* pv is already on the free list */
    }
    list_push((fblk *)b);
}

void *wmem_realloc(void *p, size_t n) {
    if (!p) return wmem_alloc(n);
    if (!owns(p)) return 0;
    if (!n) { wmem_free(p); return 0; }
    blk *b = (blk *)((uint8_t *)p - HDR);
    size_t have = bsize(b) - HDR;
    size_t need = ((n + 15u) & ~(size_t)15u) + HDR;
    if (need < MIN_BLOCK) need = MIN_BLOCK;
    if (have + HDR >= need) return p;
    blk *nx = next_blk(b);                            /* grow in place into a free neighbour */
    if (!(nx->size & USED) && bsize(b) + bsize(nx) >= need) {
        list_remove((fblk *)nx);
        size_t total = bsize(b) + bsize(nx);
        used_bytes -= bsize(b);
        if (total - need >= MIN_BLOCK) {
            blk *rest = (blk *)((uint8_t *)b + need);
            rest->prev = need; rest->size = total - need;
            next_blk(rest)->prev = rest->size;
            list_push((fblk *)rest);
            b->size = need | USED;
        } else {
            b->size = total | USED;
            next_blk(b)->prev = total;
        }
        used_bytes += bsize(b);
        if (used_bytes > peak_bytes) peak_bytes = used_bytes;
        return p;
    }
    void *q = wmem_alloc(n);
    if (!q) return 0;
    wb_copy(q, p, have < n ? have : n);
    wmem_free(p);
    return q;
}

int wmem_check(void) {
    if (!base) return 1;
    size_t prev = 0, used = 0, free_count = 0;
    blk *b = (blk *)base;
    while ((uint8_t *)b < limit) {
        size_t s = bsize(b);
        if (s < MIN_BLOCK || (s & 15u) || (uint8_t *)b + s > limit || b->prev != prev) return 0;
        if (b->size & USED) used += s; else {
            ++free_count;
            if (!(next_blk(b)->size & USED)) return 0;  /* two adjacent free blocks */
        }
        prev = s; b = next_blk(b);
    }
    if ((uint8_t *)b != limit || b->prev != prev || used != used_bytes) return 0;
    size_t listed = 0;
    for (fblk *f = free_head; f; f = f->next) {
        if (f->h.size & USED || ++listed > free_count) return 0;
        if (f->next && f->next->prevf != f) return 0;
    }
    return listed == free_count;
}

/* ------------------------------------------------------------------ arenas */
struct warena_chunk {
    warena_chunk *next;
    size_t used, cap;
};
#define CHUNK_HDR ((sizeof(warena_chunk) + 15u) & ~(size_t)15u)
#define CHUNK_DEFAULT (128u * 1024u)

void warena_init(warena *a) { a->head = 0; a->total = 0; a->failed = 0; }

void *warena_alloc(warena *a, size_t n) {
    n = (n + 7u) & ~(size_t)7u;
    if (!n) n = 8;
    warena_chunk *c = a->head;
    if (!c || c->cap - c->used < n) {
        size_t cap = n > CHUNK_DEFAULT / 4u ? n : CHUNK_DEFAULT;
        warena_chunk *nc = (warena_chunk *)wmem_alloc(CHUNK_HDR + cap);
        if (!nc) { a->failed = 1; return 0; }
        nc->used = 0; nc->cap = cap;
        if (cap == n && c) {                          /* big one-off: keep filling the current chunk */
            nc->next = c->next; c->next = nc;
            a->total += CHUNK_HDR + cap;
            nc->used = n;
            uint8_t *p = (uint8_t *)nc + CHUNK_HDR;
            wb_set(p, 0, n);
            return p;
        }
        nc->next = c; a->head = nc; c = nc;
        a->total += CHUNK_HDR + cap;
    }
    uint8_t *p = (uint8_t *)c + CHUNK_HDR + c->used;
    c->used += n;
    wb_set(p, 0, n);
    return p;
}

char *warena_strndup(warena *a, const char *s, size_t n) {
    char *d = (char *)warena_alloc(a, n + 1u);
    if (!d) return 0;
    if (n) wb_copy(d, s, n);
    d[n] = '\0';
    return d;
}

void warena_free(warena *a) {
    warena_chunk *c = a->head;
    while (c) { warena_chunk *n = c->next; wmem_free(c); c = n; }
    a->head = 0; a->total = 0; a->failed = 0;
}
