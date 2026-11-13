/* 26.5.014.0: bitmap page pool for user memory (see frame_pool.h). Pure C
 * over a backend so the host test links the same file. Single core, called
 * only from non-preemptible kernel code. */
#include <stdint.h>
#include <stddef.h>
#include "../include/frame_pool.h"

#define PAGE 4096ULL

typedef struct pool_chunk {
    uint64_t base;
    uint32_t pages;
    uint32_t used;
    uint64_t bits[MINT_FRAME_POOL_WORDS];
} POOL_CHUNK;

static MINT_FRAME_POOL_BACKEND g_backend;
static int g_ready;
static POOL_CHUNK g_chunks[MINT_FRAME_POOL_MAX_CHUNKS];   /* sorted by base */
static uint32_t g_chunk_count;
static uint32_t g_hint;
static MINT_FRAME_POOL_STATS g_stats;

int mint_frame_pool_init(const MINT_FRAME_POOL_BACKEND *backend, uint64_t reserve_pages) {
    if (!backend || !backend->claim || !backend->release || !backend->free_pages) return 0;
    g_backend = *backend;
    g_chunk_count = 0;
    g_hint = 0;
    MINT_FRAME_POOL_STATS zero = {0};
    g_stats = zero;
    g_stats.reserve_pages = reserve_pages;
    g_ready = 1;
    return 1;
}

static int find_chunk(uint64_t pa) {
    uint32_t lo = 0, hi = g_chunk_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        const POOL_CHUNK *c = &g_chunks[mid];
        if (pa < c->base) hi = mid;
        else if (pa >= c->base + (uint64_t)c->pages * PAGE) lo = mid + 1u;
        else return (int)mid;
    }
    return -1;
}

static int grow(void) {
    if (g_chunk_count >= MINT_FRAME_POOL_MAX_CHUNKS) return 0;
    uint64_t avail = g_backend.free_pages(g_backend.context);
    for (uint32_t want = MINT_FRAME_POOL_CHUNK_PAGES; want >= MINT_FRAME_POOL_MIN_CHUNK_PAGES; want /= 4u) {
        if (avail < g_stats.reserve_pages || avail - g_stats.reserve_pages < want) continue;
        uint64_t base = g_backend.claim(g_backend.context, want);
        if (!base) continue;
        if (base & (PAGE - 1u)) { (void)g_backend.release(g_backend.context, base, want); return 0; }
        uint32_t at = 0;
        while (at < g_chunk_count && g_chunks[at].base < base) ++at;
        for (uint32_t i = g_chunk_count; i > at; --i) g_chunks[i] = g_chunks[i - 1u];
        POOL_CHUNK *c = &g_chunks[at];
        c->base = base; c->pages = want; c->used = 0;
        for (uint32_t w = 0; w < MINT_FRAME_POOL_WORDS; ++w) c->bits[w] = 0;
        /* Bits past `pages` in a short chunk are marked used so they are never handed out. */
        for (uint32_t p = want; p < MINT_FRAME_POOL_CHUNK_PAGES; ++p) c->bits[p / 64u] |= 1ULL << (p % 64u);
        ++g_chunk_count;
        g_hint = at;
        g_stats.chunks = g_chunk_count;
        g_stats.pool_pages += want;
        return 1;
    }
    return 0;
}

static uint64_t take_from(POOL_CHUNK *c) {
    for (uint32_t w = 0; w < MINT_FRAME_POOL_WORDS; ++w) {
        uint64_t word = c->bits[w];
        if (word == ~0ULL) continue;
        uint32_t bit = 0;
        while (word & (1ULL << bit)) ++bit;
        c->bits[w] |= 1ULL << bit;
        ++c->used;
        return c->base + ((uint64_t)w * 64u + bit) * PAGE;
    }
    return 0;
}

uint64_t mint_frame_pool_alloc(void) {
    if (!g_ready) return 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (uint32_t n = 0; n < g_chunk_count; ++n) {
            uint32_t i = (g_hint + n) % g_chunk_count;
            POOL_CHUNK *c = &g_chunks[i];
            if (c->used >= c->pages) continue;
            uint64_t pa = take_from(c);
            if (!pa) continue;
            g_hint = i;
            uint64_t *page = g_backend.pointer ? (uint64_t *)g_backend.pointer(g_backend.context, pa) : 0;
            if (page) for (uint32_t k = 0; k < PAGE / sizeof(uint64_t); ++k) page[k] = 0;
            ++g_stats.allocations;
            ++g_stats.used_pages;
            if (g_stats.used_pages > g_stats.peak_used_pages) g_stats.peak_used_pages = g_stats.used_pages;
            return pa;
        }
        if (!grow()) break;
    }
    ++g_stats.failed_allocations;
    return 0;
}

int mint_frame_pool_free(uint64_t pa) {
    if (!g_ready || (pa & (PAGE - 1u))) return 0;
    int i = find_chunk(pa);
    if (i < 0) return 0;
    POOL_CHUNK *c = &g_chunks[i];
    uint64_t index = (pa - c->base) / PAGE;
    uint64_t mask = 1ULL << (index % 64u);
    if (index >= c->pages || !(c->bits[index / 64u] & mask)) return 0;
    c->bits[index / 64u] &= ~mask;
    --c->used;
    ++g_stats.frees;
    if (g_stats.used_pages) --g_stats.used_pages;
    if (c->used == 0) {
        uint32_t empty = 0;
        for (uint32_t k = 0; k < g_chunk_count; ++k) if (g_chunks[k].used == 0) ++empty;
        /* Keep one spare chunk so a process start/exit cycle does not churn physmem. */
        if (empty > 1u && g_backend.release(g_backend.context, c->base, c->pages)) {
            g_stats.pool_pages -= c->pages;
            for (uint32_t k = (uint32_t)i; k + 1u < g_chunk_count; ++k) g_chunks[k] = g_chunks[k + 1u];
            --g_chunk_count;
            g_hint = 0;
            g_stats.chunks = g_chunk_count;
            ++g_stats.chunks_returned;
        }
    }
    return 1;
}

int mint_frame_pool_is_allocated(uint64_t pa) {
    if (!g_ready || (pa & (PAGE - 1u))) return 0;
    int i = find_chunk(pa);
    if (i < 0) return 0;
    uint64_t index = (pa - g_chunks[i].base) / PAGE;
    return index < g_chunks[i].pages && (g_chunks[i].bits[index / 64u] & (1ULL << (index % 64u))) != 0;
}

int mint_frame_pool_overlaps(uint64_t pa, uint64_t pages) {
    if (!g_ready || !pages) return 0;
    uint64_t end = pa + pages * PAGE;
    for (uint32_t i = 0; i < g_chunk_count; ++i) {
        uint64_t cb = g_chunks[i].base, ce = cb + (uint64_t)g_chunks[i].pages * PAGE;
        if (pa < ce && cb < end) return 1;
    }
    return 0;
}

void mint_frame_pool_get_stats(MINT_FRAME_POOL_STATS *out) {
    if (out) *out = g_stats;
}
