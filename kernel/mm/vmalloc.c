#include <stdint.h>
#include <stddef.h>
#include "../include/vmalloc.h"
#include "../include/physmem.h"
#include "../include/serial.h"

#define PAGE_SIZE MINT_PAGING_PAGE_SIZE
#define ALLOWED_FLAGS (MINT_PAGE_MAP_WRITE | MINT_PAGE_MAP_EXEC | MINT_PAGE_MAP_UNCACHED | MINT_PAGE_MAP_WRITE_THROUGH)

static MINT_VMALLOC_STATE g_vmalloc;
static int g_vmalloc_ready;

static int bit_is_set(const MINT_VMALLOC_STATE *s, uint32_t page) {
    return (s->bitmap[page >> 3] & (uint8_t)(1u << (page & 7u))) != 0;
}
static void bit_set(MINT_VMALLOC_STATE *s, uint32_t page, int value) {
    uint8_t mask = (uint8_t)(1u << (page & 7u));
    if (value) s->bitmap[page >> 3] |= mask;
    else s->bitmap[page >> 3] &= (uint8_t)~mask;
}
static void counter_inc(uint32_t *counter) {
    if (counter && *counter != UINT32_MAX) ++*counter;
}
static int alloc_failed(MINT_VMALLOC_STATE *s) {
    if (s && s->initialized) counter_inc(&s->failed_allocations);
    return 0;
}
static int free_failed(MINT_VMALLOC_STATE *s) {
    if (s && s->initialized) counter_inc(&s->failed_frees);
    return 0;
}
static int backend_valid(const MINT_VMALLOC_BACKEND *b) {
    return b && b->claim_page && b->release_page && b->release_pages && b->is_claimed && b->map_page &&
           b->unmap_page && b->lookup_page && b->clear_page;
}

int mint_vmalloc_state_init(MINT_VMALLOC_STATE *s) {
    if (!s) return 0;
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < sizeof(*s); ++i) p[i] = 0;
    s->initialized = 1;
    return 1;
}

int mint_vmalloc_state_find_run(const MINT_VMALLOC_STATE *s, uint32_t arena_pages,
                                uint32_t requested_pages, uint32_t *first_page) {
    if (!s || !s->initialized || !first_page || !requested_pages ||
        arena_pages > MINT_VMALLOC_PAGE_COUNT || requested_pages > arena_pages) return 0;
    uint32_t run = 0, start = 0;
    for (uint32_t i = 0; i < arena_pages; ++i) {
        if (!bit_is_set(s, i)) {
            if (!run) start = i;
            if (++run == requested_pages) { *first_page = start; return 1; }
        } else run = 0;
    }
    return 0;
}

static int state_mark(MINT_VMALLOC_STATE *s, uint32_t first, uint32_t count, int set) {
    if (!s || !s->initialized || !count || first >= MINT_VMALLOC_PAGE_COUNT ||
        count > MINT_VMALLOC_PAGE_COUNT - first) return 0;
    if (set) {
        for (uint32_t i = 0; i < count; ++i) if (bit_is_set(s, first + i)) return 0;
        for (uint32_t i = 0; i < count; ++i) bit_set(s, first + i, 1);
    } else {
        for (uint32_t i = 0; i < count; ++i) if (!bit_is_set(s, first + i)) return 0;
        for (uint32_t i = 0; i < count; ++i) bit_set(s, first + i, 0);
    }
    return 1;
}

static int free_record_slot(MINT_VMALLOC_STATE *s) {
    for (uint32_t i = 0; i < MINT_VMALLOC_MAX_ALLOCS; ++i)
        if (!s->allocations[i].active) return (int)i;
    return -1;
}

int mint_vmalloc_state_alloc(MINT_VMALLOC_STATE *s, uint64_t arena_base,
                             uint32_t arena_pages, size_t bytes, uint32_t flags,
                             const MINT_VMALLOC_BACKEND *b, uint64_t *virtual_out) {
    if (!s || !s->initialized) return 0;
    if (!backend_valid(b) || !virtual_out || !bytes || (flags & ~ALLOWED_FLAGS) ||
        (arena_base & (PAGE_SIZE - 1u)) || arena_pages > MINT_VMALLOC_PAGE_COUNT ||
        arena_base >= MINT_IDENTITY_MAP_LIMIT ||
        (uint64_t)arena_pages * PAGE_SIZE > MINT_IDENTITY_MAP_LIMIT - arena_base ||
        bytes > (size_t)arena_pages * PAGE_SIZE || bytes > SIZE_MAX - (PAGE_SIZE - 1u))
        return alloc_failed(s);
    uint64_t pages64 = ((uint64_t)bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (!pages64 || pages64 > arena_pages) return alloc_failed(s);
    uint32_t pages = (uint32_t)pages64, first = 0;
    int rec = free_record_slot(s);
    if (rec < 0 || !mint_vmalloc_state_find_run(s, arena_pages, pages, &first) ||
        !state_mark(s, first, pages, 1)) return alloc_failed(s);

    uint32_t mapped = 0;
    for (uint32_t i = 0; i < pages; ++i) {
        uint64_t pa = b->claim_page(b->context);
        uint64_t va = arena_base + (uint64_t)(first + i) * PAGE_SIZE;
        if (!pa || (pa & (PAGE_SIZE - 1u)) || !b->is_claimed(pa, b->context)) {
            if (pa && b->is_claimed(pa, b->context)) (void)b->release_page(pa, b->context);
            goto rollback;
        }
        b->clear_page(pa, b->context);
        if (!b->map_page(va, pa, flags, b->context)) {
            (void)b->release_page(pa, b->context);
            goto rollback;
        }
        s->physical_pages[first + i] = pa;
        ++mapped;
    }
    uint32_t effective_flags = 0;
    uint64_t first_pa = 0;
    if (!b->lookup_page(arena_base + (uint64_t)first * PAGE_SIZE, &first_pa,
                        &effective_flags, b->context) || first_pa != s->physical_pages[first])
        goto rollback;
    for (uint32_t i = 1; i < pages; ++i) {
        uint32_t page_flags = 0; uint64_t page_pa = 0;
        uint64_t page_va = arena_base + (uint64_t)(first + i) * PAGE_SIZE;
        if (!b->lookup_page(page_va, &page_pa, &page_flags, b->context) ||
            page_pa != s->physical_pages[first + i] || page_flags != effective_flags)
            goto rollback;
    }
    s->allocations[rec] = (MINT_VMALLOC_RECORD){ first, pages, effective_flags, 1 };
    s->live_allocations++;
    s->used_pages += pages;
    if (s->used_pages > s->peak_used_pages) s->peak_used_pages = s->used_pages;
    counter_inc(&s->total_allocations);
    *virtual_out = arena_base + (uint64_t)first * PAGE_SIZE;
    return 1;

rollback:
    counter_inc(&s->failed_allocations);
    counter_inc(&s->rollback_count);
    for (uint32_t j = 0; j < mapped; ++j) {
        uint64_t va = arena_base + (uint64_t)(first + j) * PAGE_SIZE;
        uint64_t actual = 0;
        if (b->unmap_page(va, &actual, b->context)) {
            (void)b->release_page(actual, b->context);
        }
        s->physical_pages[first + j] = 0;
    }
    (void)state_mark(s, first, pages, 0);
    return 0;
}

int mint_vmalloc_state_free(MINT_VMALLOC_STATE *s, uint64_t arena_base,
                            uint32_t arena_pages, uint64_t va,
                            const MINT_VMALLOC_BACKEND *b) {
    if (!s || !s->initialized) return 0;
    if (!backend_valid(b) || !arena_pages || arena_pages > MINT_VMALLOC_PAGE_COUNT ||
        (arena_base & (PAGE_SIZE - 1u)) || arena_base >= MINT_IDENTITY_MAP_LIMIT ||
        (uint64_t)arena_pages * PAGE_SIZE > MINT_IDENTITY_MAP_LIMIT - arena_base ||
        (va & (PAGE_SIZE - 1u)) || va < arena_base ||
        va >= arena_base + (uint64_t)arena_pages * PAGE_SIZE) return free_failed(s);
    uint64_t offset = va - arena_base;
    if (offset / PAGE_SIZE >= arena_pages) return 0;
    uint32_t first = (uint32_t)(offset / PAGE_SIZE);
    int rec = -1;
    for (uint32_t i = 0; i < MINT_VMALLOC_MAX_ALLOCS; ++i)
        if (s->allocations[i].active && s->allocations[i].first_page == first) { rec = (int)i; break; }
    if (rec < 0) return free_failed(s);
    MINT_VMALLOC_RECORD r = s->allocations[rec];
    if (!r.page_count || r.page_count > arena_pages - first) return free_failed(s);

    /* Validate every mapping and ownership record before making any edits. */
    for (uint32_t i = 0; i < r.page_count; ++i) {
        uint64_t actual = 0; uint32_t found_flags = 0;
        uint64_t expected = s->physical_pages[first + i];
        uint64_t page_va = arena_base + (uint64_t)(first + i) * PAGE_SIZE;
        if (!expected || !b->is_claimed(expected, b->context) ||
            !b->lookup_page(page_va, &actual, &found_flags, b->context) ||
            actual != expected || found_flags != r.flags) return free_failed(s);
    }

    /* Verify bitmap ownership before touching page tables. */
    for (uint32_t i = 0; i < r.page_count; ++i)
        if (!bit_is_set(s, first + i)) return free_failed(s);

    uint32_t unmapped = 0;
    for (; unmapped < r.page_count; ) {
        uint64_t actual = 0;
        uint64_t page_va = arena_base + (uint64_t)(first + unmapped) * PAGE_SIZE;
        if (!b->unmap_page(page_va, &actual, b->context)) break;
        if (actual != s->physical_pages[first + unmapped]) {
            /* The wrong leaf was removed; put that leaf back before rollback. */
            if (!b->map_page(page_va, actual, r.flags, b->context))
                mint_serial_write("[vmalloc] CRITICAL: could not restore unexpected unmap leaf\n");
            break;
        }
        ++unmapped;
    }
    if (unmapped != r.page_count) {
        /* Physical claims remain live; restore mappings removed by this pass. */
        for (uint32_t i = 0; i < unmapped; ++i) {
            uint64_t page_va = arena_base + (uint64_t)(first + i) * PAGE_SIZE;
            if (!b->map_page(page_va, s->physical_pages[first + i], r.flags, b->context))
                mint_serial_write("[vmalloc] CRITICAL: mapping rollback failed\n");
        }
        return free_failed(s);
    }

    /* Physical ownership is released transactionally. On failure, the backend
       guarantees that no page was released, so all mappings can be restored. */
    if (!b->release_pages(&s->physical_pages[first], r.page_count, b->context)) {
        for (uint32_t i = 0; i < r.page_count; ++i) {
            uint64_t page_va = arena_base + (uint64_t)(first + i) * PAGE_SIZE;
            if (!b->map_page(page_va, s->physical_pages[first + i], r.flags, b->context))
                mint_serial_write("[vmalloc] CRITICAL: release rollback mapping failed\n");
        }
        return free_failed(s);
    }
    for (uint32_t i = 0; i < r.page_count; ++i) s->physical_pages[first + i] = 0;
    if (!state_mark(s, first, r.page_count, 0)) {
        mint_serial_write("[vmalloc] CRITICAL: bitmap changed after transactional release\n");
        return free_failed(s);
    }
    s->allocations[rec].active = 0;
    s->live_allocations--;
    s->used_pages -= r.page_count;
    counter_inc(&s->total_frees);
    return 1;
}

static uint64_t real_claim_page(void *context) { (void)context; return mint_physmem_claim_page(); }
static int real_release_page(uint64_t pa, void *context) { (void)context; return mint_physmem_release_contiguous(pa, 1); }
static int real_release_pages(const uint64_t *pages, uint32_t count, void *context) { (void)context; return mint_physmem_release_batch(pages, count); }
static int real_is_claimed(uint64_t pa, void *context) { (void)context; return mint_physmem_is_claimed(pa, 1); }
static int real_map_page(uint64_t va, uint64_t pa, uint32_t flags, void *context) { (void)context; return mint_paging_map_page(va, pa, flags); }
static int real_unmap_page(uint64_t va, uint64_t *pa, void *context) { (void)context; return mint_paging_unmap_page(va, pa, 0); }
static int real_lookup_page(uint64_t va, uint64_t *pa, uint32_t *flags, void *context) { (void)context; return mint_paging_lookup_page(va, pa, flags); }
static void real_clear_page(uint64_t pa, void *context) {
    (void)context;
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)pa;
    for (uint32_t i = 0; i < PAGE_SIZE / sizeof(*p); ++i) p[i] = 0;
}
static const MINT_VMALLOC_BACKEND real_backend = {
    real_claim_page, real_release_page, real_release_pages, real_is_claimed, real_map_page,
    real_unmap_page, real_lookup_page, real_clear_page, 0
};

int mint_vmalloc_init(void) {
    if (!mint_paging_is_active()) return 0;
    /* Do not start until the entire arena is unmapped. */
    for (uint64_t va = MINT_VMALLOC_BASE; va < MINT_IDENTITY_MAP_LIMIT; va += PAGE_SIZE) {
        uint64_t pa = 0; uint32_t flags = 0;
        if (mint_paging_lookup_page(va, &pa, &flags)) {
            mint_serial_write("[vmalloc] arena contains a live mapping; refusing init\n");
            return 0;
        }
    }
    if (!mint_vmalloc_state_init(&g_vmalloc)) return 0;
    g_vmalloc_ready = 1;
    mint_serial_write("[vmalloc] 16 MiB virtual arena initialized (4096 pages)\n");
    return 1;
}

void *mint_vmalloc_alloc(size_t bytes, uint32_t flags) {
    uint64_t va = 0;
    if (!g_vmalloc_ready || !mint_vmalloc_state_alloc(&g_vmalloc, MINT_VMALLOC_BASE,
        MINT_VMALLOC_PAGE_COUNT, bytes, flags, &real_backend, &va)) {
        mint_serial_write("[vmalloc] allocation failed\n");
        return 0;
    }
    return (void *)(uintptr_t)va;
}

int mint_vmalloc_free(void *address) {
    if (!g_vmalloc_ready || !address) return 0;
    int ok = mint_vmalloc_state_free(&g_vmalloc, MINT_VMALLOC_BASE,
        MINT_VMALLOC_PAGE_COUNT, (uint64_t)(uintptr_t)address, &real_backend);
    if (!ok) mint_serial_write("[vmalloc] invalid free or mapping ownership mismatch\n");
    return ok;
}

int mint_vmalloc_selftest(void) {
    if (!g_vmalloc_ready) return 0;
    uint32_t before = g_vmalloc.used_pages;
    uint32_t before_tables = mint_paging_dynamic_tables_used();
    /* Repeatedly create and destroy a one-page mapping. The associated dynamic
       PT should be reclaimed each time, so this catches monotonic pool leaks. */
    for (uint32_t cycle = 0; cycle < 64u; ++cycle) {
        uint8_t *probe = (uint8_t *)mint_vmalloc_alloc(1, MINT_PAGE_MAP_WRITE);
        if (!probe) return 0;
        probe[0] = (uint8_t)(0x31u + (cycle & 0x3Fu));
        if (probe[0] != (uint8_t)(0x31u + (cycle & 0x3Fu)) || !mint_vmalloc_free(probe) ||
            mint_paging_dynamic_tables_used() != before_tables) return 0;
    }
    uint8_t *a = (uint8_t *)mint_vmalloc_alloc(1, MINT_PAGE_MAP_WRITE);
    uint8_t *b = (uint8_t *)mint_vmalloc_alloc((size_t)(3 * PAGE_SIZE + 37), MINT_PAGE_MAP_WRITE);
    if (!a || !b) { if (a) (void)mint_vmalloc_free(a); if (b) (void)mint_vmalloc_free(b); return 0; }
    a[0] = 0xA6u; a[PAGE_SIZE - 1u] = 0x59u;
    b[0] = 0xD3u; b[3 * PAGE_SIZE + 36u] = 0x7Bu;
    int pass = a[0] == 0xA6u && a[PAGE_SIZE - 1u] == 0x59u &&
               b[0] == 0xD3u && b[3 * PAGE_SIZE + 36u] == 0x7Bu;
    pass &= mint_vmalloc_free(a);
    pass &= !mint_vmalloc_free(a); /* double free rejected */
    pass &= mint_vmalloc_free(b);
    pass &= g_vmalloc.used_pages == before && g_vmalloc.live_allocations == 0;
    mint_serial_write(pass ? "[vmalloc] alloc/map/zero/write/unmap/free self-test passed\n"
                           : "[vmalloc] allocation self-test FAILED\n");
    if (pass && mint_paging_dynamic_tables_used() != before_tables) pass = 0;
    mint_serial_write(pass ? "[vmalloc] repeated allocate/free table-reclaim test passed\n"
                           : "[vmalloc] repeated allocate/free table-reclaim test FAILED\n");
    return pass;
}

uint32_t mint_vmalloc_live_allocations(void) { return g_vmalloc_ready ? g_vmalloc.live_allocations : 0; }
uint32_t mint_vmalloc_used_pages(void) { return g_vmalloc_ready ? g_vmalloc.used_pages : 0; }
uint32_t mint_vmalloc_peak_pages(void) { return g_vmalloc_ready ? g_vmalloc.peak_used_pages : 0; }
uint32_t mint_vmalloc_total_allocations(void) { return g_vmalloc_ready ? g_vmalloc.total_allocations : 0; }
uint32_t mint_vmalloc_total_frees(void) { return g_vmalloc_ready ? g_vmalloc.total_frees : 0; }
uint32_t mint_vmalloc_failed_allocations(void) { return g_vmalloc_ready ? g_vmalloc.failed_allocations : 0; }
uint32_t mint_vmalloc_rollback_count(void) { return g_vmalloc_ready ? g_vmalloc.rollback_count : 0; }
uint32_t mint_vmalloc_failed_frees(void) { return g_vmalloc_ready ? g_vmalloc.failed_frees : 0; }

/* Device registers: uncached access for an MMIO range. Inside the identity
 * window the existing mapping is switched to UC; above it, pages are mapped
 * into the shared vmalloc window (visible from every address space because
 * process roots share the kernel's low page directories). These mappings are
 * permanent and carry no allocation record, so mint_vmalloc_free refuses them
 * and no device page is ever handed to the RAM allocator. */
void *mint_mmio_map(uint64_t physical, uint64_t bytes) {
    if (!bytes || physical > UINT64_MAX - bytes) return 0;
    if (physical + bytes <= mint_paging_identity_limit() && physical + bytes <= MINT_VMALLOC_BASE)
        return mint_paging_mark_uncached_range(physical, bytes) ? (void *)(uintptr_t)physical : 0;
    if (!g_vmalloc_ready) return 0;
    uint64_t off = physical & (PAGE_SIZE - 1u), base = physical - off;
    uint64_t span = (off + bytes + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    if (span / PAGE_SIZE > 1024u) return 0;
    uint32_t pages = (uint32_t)(span / PAGE_SIZE), first = 0;
    if (!mint_vmalloc_state_find_run(&g_vmalloc, MINT_VMALLOC_PAGE_COUNT, pages, &first)) return 0;
    if (!state_mark(&g_vmalloc, first, pages, 1)) return 0;
    for (uint32_t i = 0; i < pages; ++i) {
        uint64_t va = MINT_VMALLOC_BASE + (uint64_t)(first + i) * PAGE_SIZE;
        if (!mint_paging_map_mmio_page(va, base + (uint64_t)i * PAGE_SIZE)) {
            for (uint32_t j = 0; j < i; ++j) {
                uint64_t ignored = 0;
                (void)mint_paging_unmap_page(MINT_VMALLOC_BASE + (uint64_t)(first + j) * PAGE_SIZE, &ignored, 0);
            }
            (void)state_mark(&g_vmalloc, first, pages, 0);
            return 0;
        }
    }
    g_vmalloc.used_pages += pages;
    return (void *)(uintptr_t)(MINT_VMALLOC_BASE + (uint64_t)first * PAGE_SIZE + off);
}
