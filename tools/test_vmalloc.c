#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/vmalloc.h"

#define FRAME_COUNT 64u
#define MOCK_MAP_COUNT 64u
struct mock_frame { uint64_t pa; int allocated; uint8_t bytes[MINT_PAGING_PAGE_SIZE]; };
struct mock_mapping { uint64_t va, pa; uint32_t flags; int active; };
struct mock { struct mock_frame frames[FRAME_COUNT]; struct mock_mapping maps[MOCK_MAP_COUNT]; uint32_t map_calls; uint32_t fail_map_call; int fail_release_batch; };

uint64_t mint_physmem_claim_page(void) { return 0; }
int mint_physmem_release_contiguous(uint64_t pa, uint64_t pages) { (void)pa; (void)pages; return 0; }
int mint_physmem_release_batch(const uint64_t *pages, uint32_t count) { (void)pages; (void)count; return 0; }
int mint_physmem_is_claimed(uint64_t pa, uint64_t pages) { (void)pa; (void)pages; return 0; }
int mint_paging_map_page(uint64_t va, uint64_t pa, uint32_t flags) { (void)va; (void)pa; (void)flags; return 0; }
int mint_paging_unmap_page(uint64_t va, uint64_t *pa, uint32_t *flags) { (void)va; (void)pa; (void)flags; return 0; }
int mint_paging_lookup_page(uint64_t va, uint64_t *pa, uint32_t *flags) { (void)va; (void)pa; (void)flags; return 0; }
int mint_paging_is_active(void) { return 0; }
uint32_t mint_paging_dynamic_tables_used(void) { return 0; }
void mint_serial_write(const char *s) { (void)s; }
void mint_serial_dec64(uint64_t n) { (void)n; }

static int frame_index(struct mock *m, uint64_t pa) {
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) if (m->frames[i].pa == pa) return (int)i;
    return -1;
}
static uint64_t mock_claim(void *ctx) {
    struct mock *m = ctx;
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) if (!m->frames[i].allocated) {
        m->frames[i].allocated = 1;
        m->frames[i].pa = 0x200000ULL + (uint64_t)i * MINT_PAGING_PAGE_SIZE;
        return m->frames[i].pa;
    }
    return 0;
}
static int mock_release(uint64_t pa, void *ctx) {
    struct mock *m = ctx; int i = frame_index(m, pa);
    if (i < 0 || !m->frames[i].allocated) return 0;
    m->frames[i].allocated = 0; return 1;
}
static int mock_release_batch(const uint64_t *pages, uint32_t count, void *ctx) {
    struct mock *m = ctx;
    if (!pages || !count || m->fail_release_batch) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        int ix = frame_index(m, pages[i]);
        if (ix < 0 || !m->frames[ix].allocated) return 0;
        for (uint32_t j = 0; j < i; ++j) if (pages[j] == pages[i]) return 0;
    }
    for (uint32_t i = 0; i < count; ++i) m->frames[frame_index(m, pages[i])].allocated = 0;
    return 1;
}
static int mock_claimed(uint64_t pa, void *ctx) {
    struct mock *m = ctx; int i = frame_index(m, pa); return i >= 0 && m->frames[i].allocated;
}
static int mock_map(uint64_t va, uint64_t pa, uint32_t flags, void *ctx) {
    struct mock *m = ctx; ++m->map_calls;
    if (m->fail_map_call && m->map_calls == m->fail_map_call) return 0;
    if (frame_index(m, pa) < 0) return 0;
    for (uint32_t i = 0; i < MOCK_MAP_COUNT; ++i) {
        if (m->maps[i].active && m->maps[i].va == va) return 0;
    }
    for (uint32_t i = 0; i < MOCK_MAP_COUNT; ++i) if (!m->maps[i].active) {
        m->maps[i] = (struct mock_mapping){va, pa, flags, 1}; return 1;
    }
    return 0;
}
static int mock_unmap(uint64_t va, uint64_t *pa, void *ctx) {
    struct mock *m = ctx;
    for (uint32_t i = 0; i < MOCK_MAP_COUNT; ++i) if (m->maps[i].active && m->maps[i].va == va) {
        if (pa) *pa = m->maps[i].pa;
        m->maps[i].active = 0; return 1;
    }
    return 0;
}
static int mock_lookup(uint64_t va, uint64_t *pa, uint32_t *flags, void *ctx) {
    struct mock *m = ctx;
    for (uint32_t i = 0; i < MOCK_MAP_COUNT; ++i) if (m->maps[i].active && m->maps[i].va == va) {
        if (pa) *pa = m->maps[i].pa;
        if (flags) *flags = m->maps[i].flags;
        return 1;
    }
    return 0;
}
static void mock_clear(uint64_t pa, void *ctx) {
    struct mock *m = ctx; int i = frame_index(m, pa); assert(i >= 0);
    memset(m->frames[i].bytes, 0, sizeof(m->frames[i].bytes));
}
static int active_frames(const struct mock *m) {
    int n = 0; for (uint32_t i = 0; i < FRAME_COUNT; ++i) n += m->frames[i].allocated != 0; return n;
}
static int active_maps(const struct mock *m) {
    int n = 0; for (uint32_t i = 0; i < MOCK_MAP_COUNT; ++i) n += m->maps[i].active != 0; return n;
}
static int frame_for_va(struct mock *m, uint64_t va) {
    uint64_t pa = 0; assert(mock_lookup(va, &pa, 0, m)); return frame_index(m, pa);
}

int main(void) {
    MINT_VMALLOC_STATE *state = calloc(1, sizeof(*state)); assert(state);
    struct mock *m = calloc(1, sizeof(*m)); assert(m);
    MINT_VMALLOC_BACKEND backend = { mock_claim, mock_release, mock_release_batch, mock_claimed, mock_map,
                                     mock_unmap, mock_lookup, mock_clear, m };
    const uint64_t base = 0xF0000000ULL;
    assert(mint_vmalloc_state_init(state));
    uint64_t a = 0, b = 0;
    assert(mint_vmalloc_state_alloc(state, base, 5, 1, MINT_PAGE_MAP_WRITE, &backend, &a));
    assert(a == base);
    int ai = frame_for_va(m, a);
    assert(ai >= 0 && m->frames[ai].bytes[0] == 0 && m->frames[ai].bytes[4095] == 0);
    assert(mint_vmalloc_state_alloc(state, base, 5, (size_t)3 * MINT_PAGING_PAGE_SIZE + 37,
                                    MINT_PAGE_MAP_WRITE, &backend, &b));
    assert(b == base + MINT_PAGING_PAGE_SIZE);
    assert(state->used_pages == 5 && state->live_allocations == 2);
    uint32_t run = 0;
    assert(!mint_vmalloc_state_find_run(state, 5, 2, &run)); /* all five slots are occupied */
    assert(!mint_vmalloc_state_free(state, base, 5, a + 1, &backend));
    assert(mint_vmalloc_state_free(state, base, 5, a, &backend));
    assert(!mint_vmalloc_state_free(state, base, 5, a, &backend));
    assert(mint_vmalloc_state_free(state, base, 5, b, &backend));
    assert(state->used_pages == 0 && state->live_allocations == 0);
    assert(active_frames(m) == 0 && active_maps(m) == 0);

    /* Repeated lifecycle test: counters increase monotonically, used pages return
       to zero, and no backing frames or VA mappings accumulate across 1000 cycles. */
    uint32_t allocs_before = state->total_allocations;
    uint32_t frees_before = state->total_frees;
    for (uint32_t cycle = 0; cycle < 1000u; ++cycle) {
        assert(mint_vmalloc_state_alloc(state, base, 16, 1, MINT_PAGE_MAP_WRITE,
                                        &backend, &a));
        int ix = frame_for_va(m, a);
        assert(ix >= 0 && m->frames[ix].bytes[0] == 0);
        m->frames[ix].bytes[0] = (uint8_t)cycle;
        assert(mint_vmalloc_state_free(state, base, 16, a, &backend));
        assert(state->used_pages == 0 && state->live_allocations == 0);
        assert(active_frames(m) == 0 && active_maps(m) == 0);
    }
    assert(state->total_allocations == allocs_before + 1000u);
    assert(state->total_frees == frees_before + 1000u);
    assert(state->peak_used_pages >= 5u);
    assert(state->failed_frees >= 2u); /* partial-base and double-free rejects */

    /* Force the physical batch-release transaction to fail after unmapping.
       The allocator must restore all mappings and preserve every claim/record. */
    assert(mint_vmalloc_state_alloc(state, base, 16, (size_t)3 * MINT_PAGING_PAGE_SIZE,
                                    MINT_PAGE_MAP_WRITE, &backend, &a));
    int before_frames = active_frames(m), before_maps = active_maps(m);
    m->fail_release_batch = 1;
    assert(!mint_vmalloc_state_free(state, base, 16, a, &backend));
    assert(state->used_pages == 3 && state->live_allocations == 1);
    assert(active_frames(m) == before_frames && active_maps(m) == before_maps);
    m->fail_release_batch = 0;
    assert(mint_vmalloc_state_free(state, base, 16, a, &backend));
    assert(active_frames(m) == 0 && active_maps(m) == 0);

    /* Fail the second mapping of a three-page allocation; rollback must leave no leaks. */
    m->fail_map_call = m->map_calls + 2;
    assert(!mint_vmalloc_state_alloc(state, base, 16, (size_t)3 * MINT_PAGING_PAGE_SIZE,
                                     MINT_PAGE_MAP_WRITE, &backend, &a));
    assert(state->used_pages == 0 && state->live_allocations == 0);
    assert(active_frames(m) == 0 && active_maps(m) == 0);
    assert(state->failed_allocations >= 1u && state->rollback_count >= 1u);
    m->fail_map_call = 0;
    assert(mint_vmalloc_state_alloc(state, base, 16, (size_t)2 * MINT_PAGING_PAGE_SIZE,
                                    MINT_PAGE_MAP_WRITE | MINT_PAGE_MAP_EXEC, &backend, &a));
    assert(a == base); /* failed transaction released its virtual slots */
    assert(mint_vmalloc_state_free(state, base, 16, a, &backend));
    assert(state->used_pages == 0 && state->live_allocations == 0);
    assert(active_frames(m) == 0 && active_maps(m) == 0);
    free(m); free(state);
    puts("PASS: virtual arena first-fit, zeroing, exact-base free, fragmentation, rollback, slot reuse, allocator metrics, 1000 allocate/free cycles, transactional free rollback, and no backing-frame/mapping leaks");
    return 0;
}
