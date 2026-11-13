#ifndef MINTOS26_FRAME_POOL_H
#define MINTOS26_FRAME_POOL_H
/* 26.5.014.0: page pool for user memory and user page tables.
 *
 * physmem.c tracks every claim as a separate record (4096 at most) and scans
 * them linearly, which caps all user processes together at ~14 MiB. The pool
 * takes large contiguous chunks from physmem on demand (one record each) and
 * hands out single pages from a per-chunk bitmap. Completely free chunks are
 * returned to physmem once more than one spare chunk exists, and the pool
 * never grows while physmem would drop below the kernel reserve. */
#include <stdint.h>

#define MINT_FRAME_POOL_CHUNK_PAGES 1024u   /* 4 MiB per chunk when available */
#define MINT_FRAME_POOL_MIN_CHUNK_PAGES 16u
#define MINT_FRAME_POOL_MAX_CHUNKS 1024u
#define MINT_FRAME_POOL_WORDS (MINT_FRAME_POOL_CHUNK_PAGES / 64u)

typedef struct mint_frame_pool_backend {
    void *context;
    /* Claim `pages` physically contiguous, page-aligned pages; 0 on failure. */
    uint64_t (*claim)(void *context, uint64_t pages);
    int (*release)(void *context, uint64_t base, uint64_t pages);
    uint64_t (*free_pages)(void *context);
    /* Return a pointer through which the page can be zeroed (may be 0 in tests). */
    void *(*pointer)(void *context, uint64_t physical);
} MINT_FRAME_POOL_BACKEND;

typedef struct mint_frame_pool_stats {
    uint64_t chunks;
    uint64_t pool_pages;      /* pages held from physmem */
    uint64_t used_pages;      /* pages handed out */
    uint64_t peak_used_pages;
    uint64_t allocations;
    uint64_t frees;
    uint64_t failed_allocations;
    uint64_t chunks_returned;
    uint64_t reserve_pages;
} MINT_FRAME_POOL_STATS;

int mint_frame_pool_init(const MINT_FRAME_POOL_BACKEND *backend, uint64_t reserve_pages);
/* One zeroed page, or 0 when the pool cannot grow. */
uint64_t mint_frame_pool_alloc(void);
int mint_frame_pool_free(uint64_t physical);
/* 1 if `physical` is a page currently handed out by the pool. */
int mint_frame_pool_is_allocated(uint64_t physical);
/* 1 if any page of [physical, physical+pages*4096) lies inside a pool chunk. */
int mint_frame_pool_overlaps(uint64_t physical, uint64_t pages);
void mint_frame_pool_get_stats(MINT_FRAME_POOL_STATS *out);

#endif
