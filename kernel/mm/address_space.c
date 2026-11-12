#include <stdint.h>
#include <stddef.h>
#include "../include/address_space.h"

#define PAGE_MASK (MINT_PAGING_PAGE_SIZE - 1u)
#define PTE_P   (1ULL << 0)
#define PTE_RW  (1ULL << 1)
#define PTE_US  (1ULL << 2)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PDE_PS  (1ULL << 7)
#define PTE_NX  (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_ALLOWED_FLAGS (MINT_PAGE_MAP_WRITE | MINT_PAGE_MAP_EXEC | MINT_PAGE_MAP_UNCACHED | MINT_PAGE_MAP_WRITE_THROUGH)
#define USER_PDPT_FIRST 64u
#define USER_PDPT_END 512u
#define REGION_ALLOWED_FLAGS (PTE_ALLOWED_FLAGS | MINT_ASPACE_REGION_NOACCESS)
/* PTE software-available bits: 9 = borrowed (shared, never freed by the
 * space), 10 = owned frame (allocated for, and freed with, this space). */
#define PTE_SHARED_MARK (1ULL << 9)
#define PTE_OWNED_MARK  (1ULL << 10)

static MINT_ADDRESS_SPACE_BACKEND g_backend;
static uint64_t g_kernel_root_physical;
static uint64_t g_shared_low_pdpt[MINT_ASPACE_SHARED_PDPT_ENTRIES];
static uint64_t g_shared_direct_pml4_entry;
static int g_nx_supported;
static int g_system_ready;
static MINT_ADDRESS_SPACE *g_current_space;

static void zero_bytes(void *ptr, size_t bytes) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < bytes; ++i) p[i] = 0;
}

static int backend_valid(const MINT_ADDRESS_SPACE_BACKEND *b) {
    return b && b->claim_page && b->release_page && b->physical_to_pointer &&
           b->is_claimed_single_page && b->load_root && b->invalidate_page;
}

static int valid_table_page(uint64_t pa) {
    return pa && !(pa & (MINT_PAGING_PAGE_SIZE - 1u)) &&
           pa < MINT_IDENTITY_MAP_LIMIT &&
           MINT_PAGING_PAGE_SIZE <= MINT_IDENTITY_MAP_LIMIT - pa;
}

int mint_address_space_is_user_address(uint64_t va) {
    return va >= MINT_ASPACE_USER_BASE && va < MINT_ASPACE_USER_END;
}

const MINT_ADDRESS_SPACE *mint_address_space_current(void) {
    return g_current_space;
}

static void *physical_pointer(uint64_t pa) {
    if (!valid_table_page(pa)) return 0;
    return g_backend.physical_to_pointer(g_backend.context, pa);
}

static uint64_t *root_entries(const MINT_ADDRESS_SPACE *space) {
    return space && space->root_physical ? (uint64_t *)physical_pointer(space->root_physical) : 0;
}

static uint64_t *pdpt_entries(const MINT_ADDRESS_SPACE *space) {
    return space && space->pdpt_physical ? (uint64_t *)physical_pointer(space->pdpt_physical) : 0;
}

static uint64_t table_claim(MINT_ADDRESS_SPACE *space) {
    uint64_t pa = g_backend.claim_page(g_backend.context);
    if (!valid_table_page(pa)) {
        if (pa && !(pa & (MINT_PAGING_PAGE_SIZE - 1u)))
            (void)g_backend.release_page(g_backend.context, pa);
        return 0;
    }
    void *ptr = physical_pointer(pa);
    if (!ptr) {
        (void)g_backend.release_page(g_backend.context, pa);
        return 0;
    }
    zero_bytes(ptr, (size_t)MINT_PAGING_PAGE_SIZE);
    if (space) {
        ++space->table_pages_live;
        if (space->table_pages_live > space->table_pages_peak)
            space->table_pages_peak = space->table_pages_live;
    }
    return pa;
}

static int table_release(MINT_ADDRESS_SPACE *space, uint64_t pa) {
    if (!valid_table_page(pa) || !g_backend.release_page(g_backend.context, pa)) return 0;
    if (space) {
        if (space->table_pages_live) --space->table_pages_live;
        ++space->table_pages_reclaimed;
    }
    return 1;
}

static int page_table_empty(const uint64_t *entries) {
    if (!entries) return 0;
    for (unsigned i = 0; i < 512u; ++i) if (entries[i] & PTE_P) return 0;
    return 1;
}

static uint32_t frame_limit_of(const MINT_ADDRESS_SPACE *space) {
    return space->frame_limit ? space->frame_limit : MINT_ASPACE_MAX_USER_FRAMES;
}

/* Owned-but-unmapped frames. */
static int find_frame(MINT_ADDRESS_SPACE *space, uint64_t pa) {
    if (!space) return -1;
    for (uint32_t i = 0; i < space->pending_count; ++i)
        if (space->pending[i] == pa) return (int)i;
    return -1;
}
static void pending_remove(MINT_ADDRESS_SPACE *space, int slot) {
    space->pending[slot] = space->pending[space->pending_count - 1u];
    space->pending[--space->pending_count] = 0;
}

/* Leaf PTE pointer for va if its page table exists (the entry may be 0). */
static uint64_t *leaf_entry(const MINT_ADDRESS_SPACE *space, uint64_t va) {
    uint64_t *pdpt = pdpt_entries(space);
    if (!pdpt) return 0;
    uint32_t di = (uint32_t)((va >> 30) & 0x1FFu);
    uint32_t pi = (uint32_t)((va >> 21) & 0x1FFu);
    if (di < USER_PDPT_FIRST || di >= USER_PDPT_END || !(pdpt[di] & PTE_P) ||
        !(pdpt[di] & PTE_US) || (pdpt[di] & PDE_PS)) return 0;
    uint64_t *pd = (uint64_t *)physical_pointer(pdpt[di] & PTE_ADDR_MASK);
    if (!pd || !(pd[pi] & PTE_P) || !(pd[pi] & PTE_US) || (pd[pi] & PDE_PS)) return 0;
    uint64_t *pt = (uint64_t *)physical_pointer(pd[pi] & PTE_ADDR_MASK);
    return pt ? &pt[(va >> 12) & 0x1FFu] : 0;
}

/* Leaf PTE pointer, creating the PD/PT on the way. On failure every table
 * this call created is released again. */
static uint64_t *leaf_entry_create(MINT_ADDRESS_SPACE *space, uint64_t va) {
    uint64_t *pdpt = pdpt_entries(space);
    if (!pdpt) return 0;
    uint32_t di = (uint32_t)((va >> 30) & 0x1FFu);
    uint32_t pi = (uint32_t)((va >> 21) & 0x1FFu);
    if (di < USER_PDPT_FIRST || di >= USER_PDPT_END) return 0;
    int made_pd = 0;
    uint64_t pd_pa;
    if (!(pdpt[di] & PTE_P)) {
        pd_pa = table_claim(space);
        if (!pd_pa) return 0;
        pdpt[di] = pd_pa | PTE_P | PTE_RW | PTE_US;
        made_pd = 1;
    } else {
        if (!(pdpt[di] & PTE_US) || (pdpt[di] & PDE_PS)) return 0;
        pd_pa = pdpt[di] & PTE_ADDR_MASK;
    }
    uint64_t *pd = (uint64_t *)physical_pointer(pd_pa);
    if (pd && !(pd[pi] & PTE_P)) {
        uint64_t pt_pa = table_claim(space);
        if (pt_pa) pd[pi] = pt_pa | PTE_P | PTE_RW | PTE_US;
    }
    uint64_t *leaf = pd ? leaf_entry(space, va) : 0;
    if (!leaf && made_pd && (!pd || page_table_empty(pd))) {
        pdpt[di] = 0;
        if (!table_release(space, pd_pa)) pdpt[di] = pd_pa | PTE_P | PTE_RW | PTE_US;
    }
    return leaf;
}

/* Release the PT (and PD) holding va if they no longer map anything. */
static void prune_tables(MINT_ADDRESS_SPACE *space, uint64_t va) {
    uint64_t *pdpt = pdpt_entries(space);
    if (!pdpt) return;
    uint32_t di = (uint32_t)((va >> 30) & 0x1FFu);
    uint32_t pi = (uint32_t)((va >> 21) & 0x1FFu);
    if (di < USER_PDPT_FIRST || di >= USER_PDPT_END || !(pdpt[di] & PTE_P)) return;
    uint64_t *pd = (uint64_t *)physical_pointer(pdpt[di] & PTE_ADDR_MASK);
    if (!pd || !(pd[pi] & PTE_P)) return;
    uint64_t *pt = (uint64_t *)physical_pointer(pd[pi] & PTE_ADDR_MASK);
    if (pt && page_table_empty(pt) && table_release(space, pd[pi] & PTE_ADDR_MASK)) {
        pd[pi] = 0;
        if (page_table_empty(pd) && table_release(space, pdpt[di] & PTE_ADDR_MASK)) pdpt[di] = 0;
    }
}

static uint64_t make_pte(const MINT_ADDRESS_SPACE *space, uint64_t pa, uint32_t flags, uint64_t mark) {
    uint64_t pte = (pa & PTE_ADDR_MASK) | PTE_P | PTE_US | mark;
    if (flags & MINT_PAGE_MAP_WRITE) pte |= PTE_RW;
    if (flags & MINT_PAGE_MAP_UNCACHED) pte |= PTE_PCD;
    if (flags & MINT_PAGE_MAP_WRITE_THROUGH) pte |= PTE_PWT;
    if (space->nx_supported && !(flags & MINT_PAGE_MAP_EXEC)) pte |= PTE_NX;
    return pte;
}

/* 1 if any leaf in [base, end) is present. Skips absent PD/PT ranges. */
static int range_has_present(const MINT_ADDRESS_SPACE *space, uint64_t base, uint64_t end) {
    uint64_t va = base;
    while (va < end) {
        uint64_t *leaf = leaf_entry(space, va);
        if (!leaf) { va = (va + 0x200000ULL) & ~0x1FFFFFULL; continue; }
        if (*leaf & PTE_P) return 1;
        va += MINT_PAGING_PAGE_SIZE;
    }
    return 0;
}

/* Unmap and free owned pages in [base, end); borrowed pages are left alone.
 * Returns the number of pages freed. */
static uint64_t free_owned_range(MINT_ADDRESS_SPACE *space, uint64_t base, uint64_t end) {
    uint64_t freed = 0, va = base;
    while (va < end) {
        uint64_t *leaf = leaf_entry(space, va);
        if (!leaf) { va = (va + 0x200000ULL) & ~0x1FFFFFULL; continue; }
        uint64_t pte = *leaf;
        if ((pte & PTE_P) && (pte & PTE_OWNED_MARK)) {
            *leaf = 0;
            g_backend.invalidate_page(g_backend.context, va);
            if (g_backend.release_page(g_backend.context, pte & PTE_ADDR_MASK)) {
                if (space->frame_pages_live) --space->frame_pages_live;
            }
            if (space->mappings_live) --space->mappings_live;
            ++space->mappings_removed;
            ++freed;
            uint64_t next = va + MINT_PAGING_PAGE_SIZE;
            if ((next & 0x1FFFFFULL) == 0 || next >= end) prune_tables(space, va);
        }
        va += MINT_PAGING_PAGE_SIZE;
    }
    /* A table can also become empty when its last entry was below `base`. */
    if (base < end) prune_tables(space, base);
    return freed;
}

static int region_contains(const MINT_ADDRESS_REGION *r, uint64_t base, uint64_t size) {
    if (!r || !r->active || !size || base < r->base || base > UINT64_MAX - size ||
        r->base > UINT64_MAX - r->size) return 0;
    return base + size <= r->base + r->size;
}

static int find_region_slot(const MINT_ADDRESS_SPACE *space) {
    if (!space) return -1;
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i)
        if (!space->regions[i].active) return (int)i;
    return -1;
}

static int find_region_for_range(const MINT_ADDRESS_SPACE *space, uint64_t base, uint64_t size) {
    if (!space) return -1;
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i)
        if (region_contains(&space->regions[i], base, size)) return (int)i;
    return -1;
}

static int ranges_overlap_u64(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
    if (!alen || !blen || a > UINT64_MAX - alen || b > UINT64_MAX - blen) return 1;
    return a < b + blen && b < a + alen;
}

static int region_reserve_checked(MINT_ADDRESS_SPACE *space, uint64_t base,
                                  uint64_t size, uint32_t flags,
                                  MINT_ASPACE_REGION_KIND kind);

int mint_address_space_region_reserve(MINT_ADDRESS_SPACE *space, uint64_t base,
                                      uint64_t size, uint32_t flags,
                                      MINT_ASPACE_REGION_KIND kind) {
    if (!space || space->active) return 0;
    return region_reserve_checked(space, base, size, flags, kind);
}

static int region_reserve_checked(MINT_ADDRESS_SPACE *space, uint64_t base,
                                  uint64_t size, uint32_t flags,
                                  MINT_ASPACE_REGION_KIND kind) {
    if (!g_system_ready || !space || !space->initialized ||
        !size || (base & (MINT_PAGING_PAGE_SIZE - 1u)) ||
        (size & (MINT_PAGING_PAGE_SIZE - 1u)) ||
        base < MINT_ASPACE_USER_BASE || base >= MINT_ASPACE_USER_END ||
        size > MINT_ASPACE_USER_END - base ||
        (flags & ~REGION_ALLOWED_FLAGS) ||
        ((flags & MINT_PAGE_MAP_WRITE) && (flags & MINT_PAGE_MAP_EXEC)) ||
        kind < MINT_ASPACE_REGION_ANONYMOUS || kind > MINT_ASPACE_REGION_GUARD)
        return 0;
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i) {
        const MINT_ADDRESS_REGION *r = &space->regions[i];
        if (r->active && ranges_overlap_u64(base, size, r->base, r->size)) return 0;
    }
    int slot = find_region_slot(space);
    if (slot < 0) return 0;
    space->regions[slot] = (MINT_ADDRESS_REGION){base, size, flags, (uint32_t)kind, 1u, 0u};
    ++space->regions_live;
    return 1;
}

int mint_address_space_region_release(MINT_ADDRESS_SPACE *space, uint64_t base) {
    if (!g_system_ready || !space || !space->initialized || space->active ||
        (base & (MINT_PAGING_PAGE_SIZE - 1u))) return 0;
    int slot = -1;
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i)
        if (space->regions[i].active && space->regions[i].base == base) { slot = (int)i; break; }
    if (slot < 0) return 0;
    const MINT_ADDRESS_REGION *region = &space->regions[slot];
    if (range_has_present(space, region->base, region->base + region->size)) return 0;
    space->regions[slot] = (MINT_ADDRESS_REGION){0, 0, 0, 0, 0, 0};
    if (space->regions_live) --space->regions_live;
    return 1;
}

int mint_address_space_region_query(const MINT_ADDRESS_SPACE *space, uint64_t address,
                                    MINT_ADDRESS_REGION *region_out) {
    if (!space || !space->initialized || !region_out) return 0;
    int slot = find_region_for_range(space, address, 1u);
    if (slot < 0) return 0;
    *region_out = space->regions[slot];
    return 1;
}

int mint_address_space_system_init(const MINT_ADDRESS_SPACE_BACKEND *backend,
                                   uint64_t kernel_root_physical,
                                   const uint64_t shared_low_pdpt[MINT_ASPACE_SHARED_PDPT_ENTRIES],
                                   uint64_t shared_direct_pml4_entry,
                                   int nx_supported) {
    if (g_system_ready || !backend_valid(backend) || !valid_table_page(kernel_root_physical) ||
        !shared_low_pdpt || !(shared_direct_pml4_entry & PTE_P) ||
        (shared_direct_pml4_entry & PTE_US)) return 0;
    for (unsigned i = 0; i < MINT_ASPACE_SHARED_PDPT_ENTRIES; ++i) {
        if (!(shared_low_pdpt[i] & PTE_P) || (shared_low_pdpt[i] & PTE_US)) return 0;
        g_shared_low_pdpt[i] = shared_low_pdpt[i];
    }
    g_backend = *backend;
    g_kernel_root_physical = kernel_root_physical;
    g_shared_direct_pml4_entry = shared_direct_pml4_entry;
    g_nx_supported = nx_supported != 0;
    g_current_space = 0;
    g_system_ready = 1;
    return 1;
}

int mint_address_space_create(MINT_ADDRESS_SPACE *space) {
    if (!g_system_ready || !space || space->initialized || space->root_physical || space->pdpt_physical)
        return 0;
    zero_bytes(space, sizeof(*space));
    space->nx_supported = (uint32_t)g_nx_supported;
    uint64_t root_pa = table_claim(space);
    if (!root_pa) { zero_bytes(space, sizeof(*space)); return 0; }
    uint64_t pdpt_pa = table_claim(space);
    if (!pdpt_pa) {
        (void)table_release(space, root_pa);
        zero_bytes(space, sizeof(*space));
        return 0;
    }
    uint64_t *root = (uint64_t *)physical_pointer(root_pa);
    uint64_t *pdpt = (uint64_t *)physical_pointer(pdpt_pa);
    if (!root || !pdpt) {
        (void)table_release(space, pdpt_pa);
        (void)table_release(space, root_pa);
        zero_bytes(space, sizeof(*space));
        return 0;
    }
    /* Root and PDPT were zeroed by table_claim. Copy only the shared low
       supervisor mappings; PDPT entries 64..511 remain private and empty. */
    for (unsigned i = 0; i < MINT_ASPACE_SHARED_PDPT_ENTRIES; ++i)
        pdpt[i] = g_shared_low_pdpt[i];
    root[0] = (pdpt_pa & PTE_ADDR_MASK) | PTE_P | PTE_RW | PTE_US;
    root[256] = g_shared_direct_pml4_entry & ~PTE_US; /* supervisor-only direct map */
    space->root_physical = root_pa;
    space->pdpt_physical = pdpt_pa;
    space->initialized = 1;
    return 1;
}

uint64_t mint_address_space_root(const MINT_ADDRESS_SPACE *space) {
    return space && space->initialized ? space->root_physical : 0;
}

int mint_address_space_activate(MINT_ADDRESS_SPACE *space) {
    if (!g_system_ready || !space || !space->initialized || !space->root_physical) return 0;
    if (g_current_space == space) return 1;
    if (g_current_space) g_current_space->active = 0;
    g_backend.load_root(g_backend.context, space->root_physical);
    if (g_current_space) g_current_space->active = 0;
    space->active = 1;
    g_current_space = space;
    return 1;
}

int mint_address_space_activate_kernel(void) {
    if (!g_system_ready) return 0;
    g_backend.load_root(g_backend.context, g_kernel_root_physical);
    if (g_current_space) g_current_space->active = 0;
    g_current_space = 0;
    return 1;
}

uint64_t mint_address_space_alloc_frame(MINT_ADDRESS_SPACE *space) {
    if (!g_system_ready || !space || !space->initialized ||
        space->pending_count >= MINT_ASPACE_PENDING_FRAMES ||
        space->frame_pages_live >= frame_limit_of(space)) return 0;
    uint64_t pa = g_backend.claim_page(g_backend.context);
    if (!valid_table_page(pa) || !g_backend.is_claimed_single_page(g_backend.context, pa)) {
        if (pa && valid_table_page(pa)) (void)g_backend.release_page(g_backend.context, pa);
        return 0;
    }
    void *ptr = physical_pointer(pa);
    if (!ptr) { (void)g_backend.release_page(g_backend.context, pa); return 0; }
    zero_bytes(ptr, (size_t)MINT_PAGING_PAGE_SIZE);
    space->pending[space->pending_count++] = pa;
    ++space->frame_pages_live;
    if (space->frame_pages_live > space->frame_pages_peak)
        space->frame_pages_peak = space->frame_pages_live;
    return pa;
}

int mint_address_space_release_frame(MINT_ADDRESS_SPACE *space, uint64_t pa) {
    if (!g_system_ready || !space || !space->initialized || (pa & (MINT_PAGING_PAGE_SIZE - 1u))) return 0;
    int slot = find_frame(space, pa);
    if (slot < 0) return 0;
    if (!g_backend.release_page(g_backend.context, pa)) return 0;
    pending_remove(space, slot);
    if (space->frame_pages_live) --space->frame_pages_live;
    return 1;
}

int mint_address_space_map(MINT_ADDRESS_SPACE *space, uint64_t va, uint64_t pa, uint32_t flags) {
    if (!g_system_ready || !space || !space->initialized || space->active != 0 ||
        !mint_address_space_is_user_address(va) || (va & (MINT_PAGING_PAGE_SIZE - 1u)) ||
        !valid_table_page(pa) || (flags & ~PTE_ALLOWED_FLAGS)) return 0;
    if ((flags & MINT_PAGE_MAP_WRITE) && (flags & MINT_PAGE_MAP_EXEC)) return 0;
    /* Only a frame this space allocated and has not mapped yet. */
    int frame_slot = find_frame(space, pa);
    if (frame_slot < 0) return 0;
    int region_slot = find_region_for_range(space, va, MINT_PAGING_PAGE_SIZE);
    if (region_slot < 0 || (flags & ~space->regions[region_slot].flags) ||
        (space->regions[region_slot].flags & MINT_ASPACE_REGION_NOACCESS)) return 0;
    uint64_t *root = root_entries(space);
    if (!root || !(root[0] & PTE_P) || !(root[0] & PTE_US)) return 0;
    uint64_t *leaf = leaf_entry(space, va);
    if (leaf && (*leaf & PTE_P)) return 0;
    if (!leaf) leaf = leaf_entry_create(space, va);
    if (!leaf) return 0;
    *leaf = make_pte(space, pa, flags, PTE_OWNED_MARK);
    pending_remove(space, frame_slot);
    ++space->mappings_live; ++space->mappings_created;
    g_backend.invalidate_page(g_backend.context, va);
    return 1;
}

int mint_address_space_lookup(const MINT_ADDRESS_SPACE *space, uint64_t va,
                              uint64_t *physical_address, uint32_t *flags) {
    if (!space || !space->initialized || !physical_address || !mint_address_space_is_user_address(va)) return 0;
    uint64_t *root = root_entries(space), *pdpt = pdpt_entries(space);
    if (!root || !pdpt || !(root[0] & PTE_P) || !(root[0] & PTE_US)) return 0;
    uint32_t di = (uint32_t)((va >> 30) & 0x1FFu);
    uint32_t pi = (uint32_t)((va >> 21) & 0x1FFu);
    uint32_t ti = (uint32_t)((va >> 12) & 0x1FFu);
    if (di < USER_PDPT_FIRST || di >= USER_PDPT_END || !(pdpt[di] & PTE_P) ||
        !(pdpt[di] & PTE_US) || (pdpt[di] & PDE_PS)) return 0;
    uint64_t *pd = (uint64_t *)physical_pointer(pdpt[di] & PTE_ADDR_MASK);
    if (!pd || !(pd[pi] & PTE_P) || !(pd[pi] & PTE_US) || (pd[pi] & PDE_PS)) return 0;
    uint64_t *pt = (uint64_t *)physical_pointer(pd[pi] & PTE_ADDR_MASK);
    if (!pt || !(pt[ti] & PTE_P) || !(pt[ti] & PTE_US)) return 0;
    uint64_t pte = pt[ti];
    *physical_address = (pte & PTE_ADDR_MASK) + (va & (MINT_PAGING_PAGE_SIZE - 1u));
    if (flags) {
        uint32_t f = 0;
        if (pte & PTE_RW) f |= MINT_PAGE_MAP_WRITE;
        if (!(pte & PTE_NX) || !space->nx_supported) f |= MINT_PAGE_MAP_EXEC;
        if (pte & PTE_PCD) f |= MINT_PAGE_MAP_UNCACHED;
        if (pte & PTE_PWT) f |= MINT_PAGE_MAP_WRITE_THROUGH;
        *flags = f;
    }
    return 1;
}

int mint_address_space_validate_user_range(const MINT_ADDRESS_SPACE *space,
                                           uint64_t address, size_t length,
                                           int require_write) {
    if (!space || !space->initialized || !space->active) return 0;
    if (length == 0) return address == 0 || mint_address_space_is_user_address(address);
    if (!mint_address_space_is_user_address(address) || address > UINT64_MAX - length ||
        (uint64_t)length > MINT_ASPACE_USER_END - address) return 0;
    uint64_t end = address + (uint64_t)length;
    uint64_t cursor = address;
    while (cursor < end) {
        MINT_ADDRESS_REGION region;
        if (!mint_address_space_region_query(space, cursor, &region)) return 0;
        uint64_t region_end = region.base + region.size;
        if (require_write && !(region.flags & MINT_PAGE_MAP_WRITE)) return 0;
        uint64_t physical = 0;
        uint32_t flags = 0;
        if (!mint_address_space_lookup(space, cursor, &physical, &flags)) {
            /* A never-touched demand-zero page: populate it now, exactly as
             * the user's own first access would have. */
            if (space != g_current_space ||
                mint_address_space_demand_fault((MINT_ADDRESS_SPACE *)space, cursor, require_write, 0) != 1 ||
                !mint_address_space_lookup(space, cursor, &physical, &flags)) return 0;
        }
        if (require_write && !(flags & MINT_PAGE_MAP_WRITE)) return 0;
        uint64_t page_end = (cursor & ~(MINT_PAGING_PAGE_SIZE - 1u)) + MINT_PAGING_PAGE_SIZE;
        uint64_t next = end;
        if (next > region_end) next = region_end;
        if (next > page_end) next = page_end;
        if (next <= cursor) return 0;
        cursor = next;
    }
    return 1;
}

static int copy_user_bytes(const MINT_ADDRESS_SPACE *space, uint64_t user_address,
                           void *kernel_buffer, size_t length, int to_user) {
    if ((!kernel_buffer && length) ||
        !mint_address_space_validate_user_range(space, user_address, length, to_user)) return 0;
    uint8_t *kernel = (uint8_t *)kernel_buffer;
    uint64_t cursor = user_address;
    size_t remaining = length;
    while (remaining) {
        uint64_t physical = 0;
        uint32_t flags = 0;
        if (!mint_address_space_lookup(space, cursor, &physical, &flags)) return 0;
        if (to_user && !(flags & MINT_PAGE_MAP_WRITE)) return 0;
        uint64_t page_pa = physical & ~(MINT_PAGING_PAGE_SIZE - 1u);
        size_t offset = (size_t)(cursor & (MINT_PAGING_PAGE_SIZE - 1u));
        size_t chunk = (size_t)MINT_PAGING_PAGE_SIZE - offset;
        if (chunk > remaining) chunk = remaining;
        uint8_t *page = (uint8_t *)physical_pointer(page_pa);
        if (!page) return 0;
        if (to_user) {
            for (size_t i = 0; i < chunk; ++i) page[offset + i] = kernel[i];
        } else {
            for (size_t i = 0; i < chunk; ++i) kernel[i] = page[offset + i];
        }
        kernel += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return 1;
}

int mint_address_space_copy_from_user(const MINT_ADDRESS_SPACE *space, void *dst,
                                      uint64_t source, size_t length) {
    return copy_user_bytes(space, source, dst, length, 0);
}

int mint_address_space_copy_to_user(const MINT_ADDRESS_SPACE *space, uint64_t destination,
                                    const void *src, size_t length) {
    return copy_user_bytes(space, destination, (void *)src, length, 1);
}

/* Trusted image-loader write path. This does not activate the target address
 * space and intentionally ignores leaf write permission, but every destination
 * page must be mapped to a frame owned by this exact address space. It is not a
 * userspace syscall and must never be exposed directly to ring 3. */
int mint_address_space_initialize_mapped_bytes(MINT_ADDRESS_SPACE *space, uint64_t destination,
                                               const void *src, size_t length) {
    if (!space || !space->initialized || space->active || (!src && length) ||
        !length || !mint_address_space_is_user_address(destination) ||
        destination > UINT64_MAX - length ||
        (uint64_t)length > MINT_ASPACE_USER_END - destination) return 0;
    const uint8_t *source = (const uint8_t *)src;
    uint64_t cursor = destination;
    size_t remaining = length;
    while (remaining) {
        uint64_t physical = 0;
        if (!mint_address_space_lookup(space, cursor, &physical, 0)) return 0;
        uint64_t page_pa = physical & ~(MINT_PAGING_PAGE_SIZE - 1u);
        uint64_t *leaf = leaf_entry(space, cursor);
        if (!leaf || !(*leaf & PTE_P) || !(*leaf & PTE_OWNED_MARK)) return 0;
        uint8_t *page = (uint8_t *)physical_pointer(page_pa);
        if (!page) return 0;
        size_t offset = (size_t)(cursor & PAGE_MASK);
        size_t chunk = (size_t)MINT_PAGING_PAGE_SIZE - offset;
        if (chunk > remaining) chunk = remaining;
        for (size_t i = 0; i < chunk; ++i) page[offset + i] = source[i];
        source += chunk; cursor += chunk; remaining -= chunk;
    }
    return 1;
}

int mint_address_space_unmap(MINT_ADDRESS_SPACE *space, uint64_t va,
                             uint64_t *physical_page, uint32_t *flags) {
    if (!space || !space->initialized || space->active || !mint_address_space_is_user_address(va) ||
        (va & (MINT_PAGING_PAGE_SIZE - 1u))) return 0;
    uint64_t pa = 0; uint32_t old_flags = 0;
    if (!mint_address_space_lookup(space, va, &pa, &old_flags)) return 0;
    pa &= ~(MINT_PAGING_PAGE_SIZE - 1u);
    uint64_t *leaf = leaf_entry(space, va);
    /* Borrowed (shared) pages are refused; the frame returns to the pending
     * set so the caller can release it. */
    if (!leaf || !(*leaf & PTE_OWNED_MARK) || space->pending_count >= MINT_ASPACE_PENDING_FRAMES) return 0;
    *leaf = 0;
    g_backend.invalidate_page(g_backend.context, va);
    space->pending[space->pending_count++] = pa;
    if (space->mappings_live) --space->mappings_live;
    ++space->mappings_removed;
    if (physical_page) *physical_page = pa;
    if (flags) *flags = old_flags;
    prune_tables(space, va);
    return 1;
}

int mint_address_space_destroy(MINT_ADDRESS_SPACE *space) {
    if (!space || !space->initialized || space->active || g_current_space == space) return 0;
    if (space->pdpt_physical) {
        uint64_t *pdpt = (uint64_t *)physical_pointer(space->pdpt_physical);
        if (!pdpt) return 0;
        for (uint32_t di = USER_PDPT_FIRST; di < USER_PDPT_END; ++di) {
            if (!(pdpt[di] & PTE_P)) continue;
            if (pdpt[di] & PDE_PS) return 0;
            uint64_t pd_pa = pdpt[di] & PTE_ADDR_MASK;
            uint64_t *pd = (uint64_t *)physical_pointer(pd_pa);
            if (!pd) return 0;
            for (unsigned pi = 0; pi < 512u; ++pi) {
                if (!(pd[pi] & PTE_P)) continue;
                if (pd[pi] & PDE_PS) return 0;
                uint64_t pt_pa = pd[pi] & PTE_ADDR_MASK;
                uint64_t *pt = (uint64_t *)physical_pointer(pt_pa);
                if (!pt) return 0;
                for (unsigned ti = 0; ti < 512u; ++ti) {
                    uint64_t pte = pt[ti];
                    if (!(pte & PTE_P) || !(pte & PTE_OWNED_MARK)) continue;
                    if (!g_backend.release_page(g_backend.context, pte & PTE_ADDR_MASK)) return 0;
                    pt[ti] = 0;
                    if (space->frame_pages_live) --space->frame_pages_live;
                }
                if (!table_release(space, pt_pa)) return 0;
                pd[pi] = 0;
            }
            if (!table_release(space, pd_pa)) return 0;
            pdpt[di] = 0;
        }
        uint64_t *root = root_entries(space);
        if (!root) return 0;
        root[0] = 0; /* no root may retain a pointer to the private PDPT at teardown */
        if (!table_release(space, space->pdpt_physical)) return 0;
        space->pdpt_physical = 0;
    }
    if (space->root_physical) {
        uint64_t *root = root_entries(space);
        if (!root) return 0;
        root[256] = 0;
        if (!table_release(space, space->root_physical)) return 0;
        space->root_physical = 0;
    }
    while (space->pending_count) {
        if (!g_backend.release_page(g_backend.context, space->pending[space->pending_count - 1u])) return 0;
        space->pending[--space->pending_count] = 0;
        if (space->frame_pages_live) --space->frame_pages_live;
    }
    zero_bytes(space, sizeof(*space));
    return 1;
}

/* Map a physically contiguous, kernel-owned range (e.g. the display canvas)
 * into a user address space as a SHARED region. The pages are not recorded in
 * frames[], so destroy() and unmap() never release them: the kernel keeps
 * ownership and the process only borrows the mapping. Page-table pages are
 * owned by the space as usual. Allowed on the active space (TLB entries are
 * invalidated per page) so a running process can request its own mapping. */
int mint_address_space_map_shared(MINT_ADDRESS_SPACE *space, uint64_t va,
                                  uint64_t pa, uint64_t pages, uint32_t flags) {
    if (!g_system_ready || !space || !space->initialized || !pages ||
        pages > (MINT_ASPACE_USER_END - MINT_ASPACE_USER_BASE) / MINT_PAGING_PAGE_SIZE ||
        (va & PAGE_MASK) || (pa & PAGE_MASK) || (flags & ~PTE_ALLOWED_FLAGS) ||
        ((flags & MINT_PAGE_MAP_WRITE) && (flags & MINT_PAGE_MAP_EXEC))) return 0;
    uint64_t bytes = pages * MINT_PAGING_PAGE_SIZE;
    if (!mint_address_space_is_user_address(va) || bytes > MINT_ASPACE_USER_END - va ||
        pa >= MINT_IDENTITY_MAP_LIMIT || bytes > MINT_IDENTITY_MAP_LIMIT - pa) return 0;
    /* Never alias a frame this space owns (that would let it be freed while
     * mapped): neither a pending frame nor a mapped owned one. */
    for (uint32_t i = 0; i < space->pending_count; ++i)
        if (space->pending[i] >= pa && space->pending[i] < pa + bytes) return 0;
    if (space->frame_pages_live > space->pending_count) {
        uint64_t *walk_pdpt = pdpt_entries(space);
        for (uint32_t di = USER_PDPT_FIRST; walk_pdpt && di < USER_PDPT_END; ++di) {
            if (!(walk_pdpt[di] & PTE_P) || (walk_pdpt[di] & PDE_PS)) continue;
            uint64_t *pd = (uint64_t *)physical_pointer(walk_pdpt[di] & PTE_ADDR_MASK);
            for (unsigned pi = 0; pd && pi < 512u; ++pi) {
                if (!(pd[pi] & PTE_P) || (pd[pi] & PDE_PS)) continue;
                uint64_t *pt = (uint64_t *)physical_pointer(pd[pi] & PTE_ADDR_MASK);
                for (unsigned ti = 0; pt && ti < 512u; ++ti) {
                    uint64_t pte = pt[ti];
                    if ((pte & PTE_P) && (pte & PTE_OWNED_MARK) &&
                        (pte & PTE_ADDR_MASK) >= pa && (pte & PTE_ADDR_MASK) < pa + bytes) return 0;
                }
            }
        }
    }
    if (!region_reserve_checked(space, va, bytes, flags, MINT_ASPACE_REGION_SHARED)) return 0;
    uint64_t *root = root_entries(space);
    uint64_t *pdpt = pdpt_entries(space);
    uint64_t mapped = 0;
    if (!root || !pdpt || !(root[0] & PTE_P) || !(root[0] & PTE_US)) goto fail;
    for (uint64_t page = 0; page < pages; ++page) {
        uint64_t v = va + page * MINT_PAGING_PAGE_SIZE;
        uint32_t di = (uint32_t)((v >> 30) & 0x1FFu);
        uint32_t pi = (uint32_t)((v >> 21) & 0x1FFu);
        uint32_t ti = (uint32_t)((v >> 12) & 0x1FFu);
        if (di < USER_PDPT_FIRST || di >= USER_PDPT_END) goto fail;
        if (!(pdpt[di] & PTE_P)) {
            uint64_t pd_pa = table_claim(space);
            if (!pd_pa) goto fail;
            pdpt[di] = pd_pa | PTE_P | PTE_RW | PTE_US;
        } else if (!(pdpt[di] & PTE_US) || (pdpt[di] & PDE_PS)) goto fail;
        uint64_t *pd = (uint64_t *)physical_pointer(pdpt[di] & PTE_ADDR_MASK);
        if (!pd) goto fail;
        if (!(pd[pi] & PTE_P)) {
            uint64_t pt_pa = table_claim(space);
            if (!pt_pa) goto fail;
            pd[pi] = pt_pa | PTE_P | PTE_RW | PTE_US;
        } else if (!(pd[pi] & PTE_US) || (pd[pi] & PDE_PS)) goto fail;
        uint64_t *pt = (uint64_t *)physical_pointer(pd[pi] & PTE_ADDR_MASK);
        if (!pt || (pt[ti] & PTE_P)) goto fail;
        uint64_t pte = ((pa + page * MINT_PAGING_PAGE_SIZE) & PTE_ADDR_MASK) | PTE_P | PTE_US | PTE_SHARED_MARK;
        if (flags & MINT_PAGE_MAP_WRITE) pte |= PTE_RW;
        if (flags & MINT_PAGE_MAP_UNCACHED) pte |= PTE_PCD;
        if (flags & MINT_PAGE_MAP_WRITE_THROUGH) pte |= PTE_PWT;
        if (space->nx_supported && !(flags & MINT_PAGE_MAP_EXEC)) pte |= PTE_NX;
        pt[ti] = pte;
        ++mapped;
        ++space->mappings_live; ++space->mappings_created;
        if (space->active) g_backend.invalidate_page(g_backend.context, v);
    }
    return 1;

fail:
    /* Clear only the borrowed PTEs this call installed (identified by the
       shared mark) and drop the region; page tables stay owned until destroy. */
    for (uint64_t page = 0; page < mapped && pdpt; ++page) {
        uint64_t v = va + page * MINT_PAGING_PAGE_SIZE;
        uint64_t *pd = (uint64_t *)physical_pointer(pdpt[(v >> 30) & 0x1FFu] & PTE_ADDR_MASK);
        uint64_t *pt = pd ? (uint64_t *)physical_pointer(pd[(v >> 21) & 0x1FFu] & PTE_ADDR_MASK) : 0;
        if (pt && (pt[(v >> 12) & 0x1FFu] & PTE_SHARED_MARK)) {
            pt[(v >> 12) & 0x1FFu] = 0;
            if (space->mappings_live) --space->mappings_live;
            if (space->active) g_backend.invalidate_page(g_backend.context, v);
        }
    }
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i)
        if (space->regions[i].active && space->regions[i].base == va &&
            space->regions[i].kind == MINT_ASPACE_REGION_SHARED) {
            space->regions[i] = (MINT_ADDRESS_REGION){0, 0, 0, 0, 0, 0};
            if (space->regions_live) --space->regions_live;
        }
    return 0;
}

/* ===================== 26.5.014.0: demand paging and process memory ===== */

int mint_address_space_set_frame_limit(MINT_ADDRESS_SPACE *space, uint32_t pages) {
    if (!space || !space->initialized || pages > MINT_ASPACE_FRAME_LIMIT_MAX) return 0;
    if (pages && pages < space->frame_pages_live) return 0;
    space->frame_limit = pages;
    return 1;
}

void mint_address_space_get_usage(const MINT_ADDRESS_SPACE *space, MINT_ASPACE_USAGE *out) {
    if (!out) return;
    zero_bytes(out, sizeof(*out));
    if (!space || !space->initialized) return;
    out->owned_pages = space->frame_pages_live;
    out->peak_owned_pages = space->frame_pages_peak;
    out->frame_limit = frame_limit_of(space);
    out->table_pages = space->table_pages_live;
    out->regions = space->regions_live;
    out->demand_faults = space->demand_faults;
}

static int region_is_demand(const MINT_ADDRESS_REGION *r) {
    return r && r->active && (r->kind == MINT_ASPACE_REGION_ANONYMOUS || r->kind == MINT_ASPACE_REGION_STACK) &&
           !(r->flags & MINT_ASPACE_REGION_NOACCESS);
}

int mint_address_space_demand_fault(MINT_ADDRESS_SPACE *space, uint64_t address, int write, int exec) {
    if (!g_system_ready || !space || !space->initialized || !mint_address_space_is_user_address(address))
        return 0;
    int slot = find_region_for_range(space, address & ~(uint64_t)PAGE_MASK, MINT_PAGING_PAGE_SIZE);
    if (slot < 0) return 0;
    const MINT_ADDRESS_REGION *r = &space->regions[slot];
    if (!region_is_demand(r) || (write && !(r->flags & MINT_PAGE_MAP_WRITE)) ||
        (exec && !(r->flags & MINT_PAGE_MAP_EXEC))) return 0;
    uint64_t va = address & ~(uint64_t)PAGE_MASK;
    uint64_t *leaf = leaf_entry(space, va);
    if (leaf && (*leaf & PTE_P)) return 0;           /* protection fault, not a missing page */
    if (space->frame_pages_live >= frame_limit_of(space)) return -1;
    uint64_t pa = g_backend.claim_page(g_backend.context);
    if (!valid_table_page(pa)) {
        if (pa && !(pa & PAGE_MASK)) (void)g_backend.release_page(g_backend.context, pa);
        return -1;
    }
    void *ptr = physical_pointer(pa);
    if (!ptr) { (void)g_backend.release_page(g_backend.context, pa); return -1; }
    zero_bytes(ptr, (size_t)MINT_PAGING_PAGE_SIZE);
    if (!leaf) leaf = leaf_entry_create(space, va);
    if (!leaf) { (void)g_backend.release_page(g_backend.context, pa); return -1; }
    *leaf = make_pte(space, pa, r->flags & PTE_ALLOWED_FLAGS, PTE_OWNED_MARK);
    g_backend.invalidate_page(g_backend.context, va);
    ++space->frame_pages_live;
    if (space->frame_pages_live > space->frame_pages_peak) space->frame_pages_peak = space->frame_pages_live;
    ++space->mappings_live; ++space->mappings_created;
    ++space->demand_faults;
    return 1;
}

int mint_address_space_region_add(MINT_ADDRESS_SPACE *space, uint64_t base, uint64_t size,
                                  uint32_t flags, MINT_ASPACE_REGION_KIND kind) {
    if (!space) return 0;
    /* Merge with an adjacent anonymous region of identical flags so a series
     * of small mmaps does not exhaust the region table. */
    if (kind == MINT_ASPACE_REGION_ANONYMOUS && size && !(base & PAGE_MASK) && !(size & PAGE_MASK)) {
        for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i) {
            MINT_ADDRESS_REGION *r = &space->regions[i];
            if (!r->active || r->kind != MINT_ASPACE_REGION_ANONYMOUS || r->flags != flags) continue;
            if (r->base + r->size == base || base + size == r->base) {
                uint64_t nb = r->base < base ? r->base : base;
                for (unsigned j = 0; j < MINT_ASPACE_MAX_REGIONS; ++j)
                    if (j != i && space->regions[j].active &&
                        ranges_overlap_u64(base, size, space->regions[j].base, space->regions[j].size)) return 0;
                if (base < MINT_ASPACE_USER_BASE || base >= MINT_ASPACE_USER_END ||
                    size > MINT_ASPACE_USER_END - base) return 0;
                r->size += size;
                r->base = nb;
                return 1;
            }
        }
    }
    return region_reserve_checked(space, base, size, flags, kind);
}

int mint_address_space_find_gap(const MINT_ADDRESS_SPACE *space, uint64_t low, uint64_t high,
                                uint64_t size, uint64_t *out) {
    if (!space || !out || !size || (size & PAGE_MASK) || (low & PAGE_MASK) || high <= low ||
        size > high - low) return 0;
    uint64_t candidate = low;
    for (unsigned rounds = 0; rounds <= MINT_ASPACE_MAX_REGIONS; ++rounds) {
        int moved = 0;
        for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i) {
            const MINT_ADDRESS_REGION *r = &space->regions[i];
            if (r->active && ranges_overlap_u64(candidate, size, r->base, r->size)) {
                candidate = r->base + r->size;
                moved = 1;
            }
        }
        if (candidate > high || size > high - candidate) return 0;
        if (!moved) { *out = candidate; return 1; }
    }
    return 0;
}

/* Remove [base, end) from region slot i, keeping the parts outside. */
static int region_cut(MINT_ADDRESS_SPACE *space, unsigned i, uint64_t base, uint64_t end) {
    MINT_ADDRESS_REGION r = space->regions[i];
    uint64_t r_end = r.base + r.size;
    uint64_t cut_lo = base > r.base ? base : r.base, cut_hi = end < r_end ? end : r_end;
    if (cut_lo >= cut_hi) return 1;
    int keep_low = cut_lo > r.base, keep_high = cut_hi < r_end;
    if (keep_low && keep_high) {
        int slot = find_region_slot(space);
        if (slot < 0) return 0;
        space->regions[i].size = cut_lo - r.base;
        space->regions[slot] = (MINT_ADDRESS_REGION){cut_hi, r_end - cut_hi, r.flags, r.kind, 1u, 0u};
        ++space->regions_live;
    } else if (keep_low) {
        space->regions[i].size = cut_lo - r.base;
    } else if (keep_high) {
        space->regions[i].base = cut_hi;
        space->regions[i].size = r_end - cut_hi;
    } else {
        space->regions[i] = (MINT_ADDRESS_REGION){0, 0, 0, 0, 0, 0};
        if (space->regions_live) --space->regions_live;
    }
    return 1;
}

static int carve_check(const MINT_ADDRESS_SPACE *space, uint64_t base, uint64_t size, uint32_t *splits) {
    *splits = 0;
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i) {
        const MINT_ADDRESS_REGION *r = &space->regions[i];
        if (!r->active || !ranges_overlap_u64(base, size, r->base, r->size)) continue;
        if (r->kind != MINT_ASPACE_REGION_ANONYMOUS && r->kind != MINT_ASPACE_REGION_STACK &&
            r->kind != MINT_ASPACE_REGION_GUARD) return 0;
        if (base > r->base && base + size < r->base + r->size) ++*splits;
    }
    uint32_t free_slots = 0;
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i) if (!space->regions[i].active) ++free_slots;
    return *splits <= free_slots;
}

int mint_address_space_region_carve(MINT_ADDRESS_SPACE *space, uint64_t base, uint64_t size) {
    if (!g_system_ready || !space || !space->initialized || !size || (base & PAGE_MASK) ||
        (size & PAGE_MASK) || !mint_address_space_is_user_address(base) ||
        size > MINT_ASPACE_USER_END - base) return 0;
    uint32_t splits = 0;
    if (!carve_check(space, base, size, &splits)) return 0;
    (void)free_owned_range(space, base, base + size);
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i) {
        const MINT_ADDRESS_REGION *r = &space->regions[i];
        if (r->active && ranges_overlap_u64(base, size, r->base, r->size))
            if (!region_cut(space, i, base, base + size)) return 0;
    }
    return 1;
}

int mint_address_space_region_protect(MINT_ADDRESS_SPACE *space, uint64_t base, uint64_t size,
                                      uint32_t flags) {
    if (!g_system_ready || !space || !space->initialized || !size || (base & PAGE_MASK) ||
        (size & PAGE_MASK) || (flags & ~REGION_ALLOWED_FLAGS) ||
        ((flags & MINT_PAGE_MAP_WRITE) && (flags & MINT_PAGE_MAP_EXEC)) ||
        !mint_address_space_is_user_address(base) || size > MINT_ASPACE_USER_END - base) return 0;
    /* The whole range must be covered by anonymous regions. */
    uint64_t covered = 0;
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i) {
        const MINT_ADDRESS_REGION *r = &space->regions[i];
        if (!r->active || !ranges_overlap_u64(base, size, r->base, r->size)) continue;
        if (r->kind != MINT_ASPACE_REGION_ANONYMOUS) return 0;
        uint64_t lo = base > r->base ? base : r->base;
        uint64_t hi = base + size < r->base + r->size ? base + size : r->base + r->size;
        covered += hi - lo;
    }
    if (covered != size) return 0;
    uint32_t splits = 0;
    if (!carve_check(space, base, size, &splits) || splits + 1u > MINT_ASPACE_MAX_REGIONS - space->regions_live)
        return 0;
    if (flags & MINT_ASPACE_REGION_NOACCESS) {
        (void)free_owned_range(space, base, base + size);
    } else {
        for (uint64_t va = base; va < base + size; va += MINT_PAGING_PAGE_SIZE) {
            uint64_t *leaf = leaf_entry(space, va);
            if (!leaf || !(*leaf & PTE_P)) continue;
            *leaf = make_pte(space, *leaf & PTE_ADDR_MASK, flags & PTE_ALLOWED_FLAGS, *leaf & PTE_OWNED_MARK);
            g_backend.invalidate_page(g_backend.context, va);
        }
    }
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i) {
        const MINT_ADDRESS_REGION *r = &space->regions[i];
        if (r->active && ranges_overlap_u64(base, size, r->base, r->size))
            if (!region_cut(space, i, base, base + size)) return 0;
    }
    return mint_address_space_region_add(space, base, size, flags, MINT_ASPACE_REGION_ANONYMOUS);
}

int mint_address_space_region_resize(MINT_ADDRESS_SPACE *space, uint64_t base, uint64_t new_size) {
    if (!g_system_ready || !space || !space->initialized || (new_size & PAGE_MASK) || !new_size) return 0;
    for (unsigned i = 0; i < MINT_ASPACE_MAX_REGIONS; ++i) {
        MINT_ADDRESS_REGION *r = &space->regions[i];
        if (!r->active || r->base != base) continue;
        if (r->kind != MINT_ASPACE_REGION_ANONYMOUS || base > MINT_ASPACE_USER_END ||
            new_size > MINT_ASPACE_USER_END - base) return 0;
        if (new_size > r->size) {
            for (unsigned j = 0; j < MINT_ASPACE_MAX_REGIONS; ++j)
                if (j != i && space->regions[j].active &&
                    ranges_overlap_u64(base + r->size, new_size - r->size,
                                       space->regions[j].base, space->regions[j].size)) return 0;
        } else if (new_size < r->size) {
            (void)free_owned_range(space, base + new_size, base + r->size);
        }
        r->size = new_size;
        return 1;
    }
    return 0;
}
