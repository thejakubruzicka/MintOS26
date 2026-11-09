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

static int find_frame(MINT_ADDRESS_SPACE *space, uint64_t pa) {
    if (!space) return -1;
    for (unsigned i = 0; i < MINT_ASPACE_MAX_USER_FRAMES; ++i)
        if (space->frames[i].active && space->frames[i].physical == pa) return (int)i;
    return -1;
}

static int find_frame_slot(MINT_ADDRESS_SPACE *space) {
    for (unsigned i = 0; i < MINT_ASPACE_MAX_USER_FRAMES; ++i)
        if (!space->frames[i].active) return (int)i;
    return -1;
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
        (flags & ~PTE_ALLOWED_FLAGS) ||
        ((flags & MINT_PAGE_MAP_WRITE) && (flags & MINT_PAGE_MAP_EXEC)) ||
        kind < MINT_ASPACE_REGION_ANONYMOUS || kind > MINT_ASPACE_REGION_SHARED)
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
    for (unsigned i = 0; i < MINT_ASPACE_MAX_USER_FRAMES; ++i) {
        const MINT_ADDRESS_SPACE_FRAME *frame = &space->frames[i];
        if (frame->active && frame->mapped &&
            region_contains(region, frame->virtual_address, MINT_PAGING_PAGE_SIZE)) return 0;
    }
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
    if (!g_system_ready || !space || !space->initialized) return 0;
    int slot = find_frame_slot(space);
    if (slot < 0) return 0;
    uint64_t pa = g_backend.claim_page(g_backend.context);
    if (!valid_table_page(pa) || !g_backend.is_claimed_single_page(g_backend.context, pa)) {
        if (pa && valid_table_page(pa)) (void)g_backend.release_page(g_backend.context, pa);
        return 0;
    }
    void *ptr = physical_pointer(pa);
    if (!ptr) { (void)g_backend.release_page(g_backend.context, pa); return 0; }
    zero_bytes(ptr, (size_t)MINT_PAGING_PAGE_SIZE);
    space->frames[slot] = (MINT_ADDRESS_SPACE_FRAME){pa, 1u, 0u, 0u};
    ++space->frame_pages_live;
    if (space->frame_pages_live > space->frame_pages_peak)
        space->frame_pages_peak = space->frame_pages_live;
    return pa;
}

int mint_address_space_release_frame(MINT_ADDRESS_SPACE *space, uint64_t pa) {
    if (!g_system_ready || !space || !space->initialized || (pa & (MINT_PAGING_PAGE_SIZE - 1u))) return 0;
    int slot = find_frame(space, pa);
    if (slot < 0 || space->frames[slot].mapped) return 0;
    if (!g_backend.release_page(g_backend.context, pa)) return 0;
    space->frames[slot] = (MINT_ADDRESS_SPACE_FRAME){0, 0, 0, 0};
    if (space->frame_pages_live) --space->frame_pages_live;
    return 1;
}

int mint_address_space_map(MINT_ADDRESS_SPACE *space, uint64_t va, uint64_t pa, uint32_t flags) {
    if (!g_system_ready || !space || !space->initialized || space->active != 0 ||
        !mint_address_space_is_user_address(va) || (va & (MINT_PAGING_PAGE_SIZE - 1u)) ||
        !valid_table_page(pa) || (flags & ~PTE_ALLOWED_FLAGS)) return 0;
    if ((flags & MINT_PAGE_MAP_WRITE) && (flags & MINT_PAGE_MAP_EXEC)) return 0;
    int frame_slot = find_frame(space, pa);
    if (frame_slot < 0 || space->frames[frame_slot].mapped) return 0;
    int region_slot = find_region_for_range(space, va, MINT_PAGING_PAGE_SIZE);
    if (region_slot < 0 || (flags & ~space->regions[region_slot].flags)) return 0;
    uint64_t *root = root_entries(space);
    uint64_t *pdpt = pdpt_entries(space);
    if (!root || !pdpt || !(root[0] & PTE_P) || !(root[0] & PTE_US)) return 0;
    uint32_t pdpt_index = (uint32_t)((va >> 30) & 0x1FFu);
    uint32_t pd_index = (uint32_t)((va >> 21) & 0x1FFu);
    uint32_t pt_index = (uint32_t)((va >> 12) & 0x1FFu);
    if (pdpt_index < USER_PDPT_FIRST || pdpt_index >= USER_PDPT_END) return 0;

    int made_pd = 0, made_pt = 0;
    uint64_t *pd = 0, *pt = 0;
    uint64_t pd_pa = 0, pt_pa = 0;
    if (!(pdpt[pdpt_index] & PTE_P)) {
        pd_pa = table_claim(space);
        if (!pd_pa) return 0;
        pdpt[pdpt_index] = pd_pa | PTE_P | PTE_RW | PTE_US;
        made_pd = 1;
    } else {
        if (!(pdpt[pdpt_index] & PTE_US) || (pdpt[pdpt_index] & PDE_PS)) return 0;
        pd_pa = pdpt[pdpt_index] & PTE_ADDR_MASK;
    }
    pd = (uint64_t *)physical_pointer(pd_pa);
    if (!pd) goto rollback;
    if (!(pd[pd_index] & PTE_P)) {
        pt_pa = table_claim(space);
        if (!pt_pa) goto rollback;
        pd[pd_index] = pt_pa | PTE_P | PTE_RW | PTE_US;
        made_pt = 1;
    } else {
        if (!(pd[pd_index] & PTE_US) || (pd[pd_index] & PDE_PS)) goto rollback;
        pt_pa = pd[pd_index] & PTE_ADDR_MASK;
    }
    pt = (uint64_t *)physical_pointer(pt_pa);
    if (!pt || (pt[pt_index] & PTE_P)) goto rollback;

    uint64_t pte = (pa & PTE_ADDR_MASK) | PTE_P | PTE_US;
    if (flags & MINT_PAGE_MAP_WRITE) pte |= PTE_RW;
    if (flags & MINT_PAGE_MAP_UNCACHED) pte |= PTE_PCD;
    if (flags & MINT_PAGE_MAP_WRITE_THROUGH) pte |= PTE_PWT;
    if (space->nx_supported && !(flags & MINT_PAGE_MAP_EXEC)) pte |= PTE_NX;
    pt[pt_index] = pte;
    space->frames[frame_slot].mapped = 1;
    space->frames[frame_slot].virtual_address = va;
    ++space->mappings_live; ++space->mappings_created;
    g_backend.invalidate_page(g_backend.context, va);
    return 1;

rollback:
    /* Only unwind tables created by this operation. If a release callback
       refuses, retain the empty table as owned state rather than freeing a
       parent that would still reference it. */
    if (made_pt && pd) {
        pd[pd_index] = 0;
        if (!table_release(space, pt_pa)) pd[pd_index] = pt_pa | PTE_P | PTE_RW | PTE_US;
    }
    if (made_pd) {
        if (!pd || page_table_empty(pd)) {
            pdpt[pdpt_index] = 0;
            if (!table_release(space, pd_pa)) pdpt[pdpt_index] = pd_pa | PTE_P | PTE_RW | PTE_US;
        }
    }
    return 0;
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
        if (!mint_address_space_lookup(space, cursor, &physical, &flags)) return 0;
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
        uint64_t page_va = cursor & ~(MINT_PAGING_PAGE_SIZE - 1u);
        int slot = find_frame(space, page_pa);
        if (slot < 0 || !space->frames[slot].active || !space->frames[slot].mapped ||
            space->frames[slot].virtual_address != page_va) return 0;
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
    int frame_slot = find_frame(space, pa);
    if (frame_slot < 0 || !space->frames[frame_slot].mapped) return 0;
    uint64_t *pdpt = pdpt_entries(space);
    uint32_t di = (uint32_t)((va >> 30) & 0x1FFu);
    uint32_t pi = (uint32_t)((va >> 21) & 0x1FFu);
    uint32_t ti = (uint32_t)((va >> 12) & 0x1FFu);
    uint64_t *pd = (uint64_t *)physical_pointer(pdpt[di] & PTE_ADDR_MASK);
    uint64_t *pt = pd ? (uint64_t *)physical_pointer(pd[pi] & PTE_ADDR_MASK) : 0;
    if (!pt) return 0;
    pt[ti] = 0;
    g_backend.invalidate_page(g_backend.context, va);
    space->frames[frame_slot].mapped = 0;
    space->frames[frame_slot].virtual_address = 0;
    if (space->mappings_live) --space->mappings_live;
    ++space->mappings_removed;
    if (physical_page) *physical_page = pa;
    if (flags) *flags = old_flags;

    if (page_table_empty(pt) && table_release(space, pd[pi] & PTE_ADDR_MASK)) {
        pd[pi] = 0;
        if (page_table_empty(pd) && table_release(space, pdpt[di] & PTE_ADDR_MASK)) pdpt[di] = 0;
    }
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
    for (unsigned i = 0; i < MINT_ASPACE_MAX_USER_FRAMES; ++i) {
        if (!space->frames[i].active) continue;
        if (!g_backend.release_page(g_backend.context, space->frames[i].physical)) return 0;
        space->frames[i] = (MINT_ADDRESS_SPACE_FRAME){0, 0, 0, 0};
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
#define PTE_SHARED_MARK (1ULL << 9) /* software-available bit: borrowed page */
int mint_address_space_map_shared(MINT_ADDRESS_SPACE *space, uint64_t va,
                                  uint64_t pa, uint64_t pages, uint32_t flags) {
    if (!g_system_ready || !space || !space->initialized || !pages ||
        pages > (MINT_ASPACE_USER_END - MINT_ASPACE_USER_BASE) / MINT_PAGING_PAGE_SIZE ||
        (va & PAGE_MASK) || (pa & PAGE_MASK) || (flags & ~PTE_ALLOWED_FLAGS) ||
        ((flags & MINT_PAGE_MAP_WRITE) && (flags & MINT_PAGE_MAP_EXEC))) return 0;
    uint64_t bytes = pages * MINT_PAGING_PAGE_SIZE;
    if (!mint_address_space_is_user_address(va) || bytes > MINT_ASPACE_USER_END - va ||
        pa >= MINT_IDENTITY_MAP_LIMIT || bytes > MINT_IDENTITY_MAP_LIMIT - pa) return 0;
    /* Never alias a frame this space owns (that would let it be freed while mapped). */
    for (unsigned i = 0; i < MINT_ASPACE_MAX_USER_FRAMES; ++i)
        if (space->frames[i].active && space->frames[i].physical >= pa &&
            space->frames[i].physical < pa + bytes) return 0;
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
