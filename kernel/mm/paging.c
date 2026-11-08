#include <stdint.h>
#include <stddef.h>
#include "../include/paging.h"
#include "../include/arch.h"
#include "../include/serial.h"

#define PTE_P   (1ULL << 0)
#define PTE_RW  (1ULL << 1)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PDE_PS  (1ULL << 7)
#define PTE_NX  (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PDE_LARGE_ADDR_MASK 0x000FFFFFFFE00000ULL
#define PTE_ALLOWED_FLAGS (MINT_PAGE_MAP_WRITE | MINT_PAGE_MAP_EXEC | MINT_PAGE_MAP_UNCACHED | MINT_PAGE_MAP_WRITE_THROUGH)

static MINT_PAGING_TABLES g_tables __attribute__((aligned(4096)));
static int paging_active;
static int nx_active;
static uint32_t dynamic_pt_used;
static uint32_t dynamic_pt_peak;
static uint64_t dynamic_pt_reclaimed;

extern char __kernel_start[];
extern char __kernel_end[];
extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];
extern char __guard_rsp0_before[], __guard_rsp0_after[];
extern char __guard_df_before[], __guard_df_after[];
extern char __guard_nmi_before[], __guard_nmi_after[];
extern char __guard_mc_before[], __guard_mc_after[];

static int ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    if (!as || !bs || a > UINT64_MAX - as || b > UINT64_MAX - bs) return 0;
    return a < b + bs && b < a + as;
}

static int in_range(uint64_t address, uint64_t start, uint64_t end) {
    return start < end && address >= start && address < end;
}

/* Resolve an address into the shared 64 GiB paging window. The high alias at
 * FFFF8000... and the low identity window intentionally share the same PDPT.
 * Addresses outside those two canonical windows are rejected. */
static int paging_window_offset(uint64_t va, uint64_t *offset_out) {
    if (!offset_out) return 0;
    if (va < MINT_IDENTITY_MAP_LIMIT) {
        *offset_out = va;
        return 1;
    }
    if (va >= MINT_DIRECT_MAP_BASE && va < MINT_DIRECT_MAP_END) {
        *offset_out = va - MINT_DIRECT_MAP_BASE;
        return 1;
    }
    return 0;
}

int mint_paging_build_tables(MINT_PAGING_TABLES *t,
                             uint64_t table_physical_base,
                             uint64_t framebuffer_base,
                             uint64_t framebuffer_size,
                             const MINT_PAGING_LAYOUT *l,
                             int nx_supported) {
    if (!t || !l || (table_physical_base & (MINT_PAGING_PAGE_SIZE - 1u)) ||
        table_physical_base >= MINT_IDENTITY_MAP_LIMIT ||
        sizeof(*t) > MINT_IDENTITY_MAP_LIMIT - table_physical_base ||
        l->kernel_start >= l->kernel_end || l->kernel_end > MINT_IDENTITY_MAP_LIMIT ||
        l->kernel_start >= MINT_IDENTITY_MAP_LIMIT ||
        l->text_start < l->kernel_start || l->text_end > l->kernel_end ||
        l->text_start >= l->text_end || l->rodata_start < l->text_end ||
        l->rodata_end < l->rodata_start || l->rodata_end > l->kernel_end ||
        (l->guard_page_count && !l->guard_pages) ||
        (framebuffer_size && (framebuffer_base >= MINT_IDENTITY_MAP_LIMIT ||
         framebuffer_size > MINT_IDENTITY_MAP_LIMIT - framebuffer_base))) return 0;

    for (size_t i = 0; i < l->guard_page_count; ++i) {
        uint64_t guard = l->guard_pages[i];
        if ((guard & (MINT_PAGING_PAGE_SIZE - 1u)) ||
            guard < l->kernel_start || guard >= l->kernel_end ||
            guard >= MINT_IDENTITY_MAP_LIMIT) return 0;
    }
    uint8_t *bytes = (uint8_t *)t;
    for (size_t i = 0; i < sizeof(*t); ++i) bytes[i] = 0;
    const uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    const uint64_t large = MINT_PAGING_LARGE_PAGE_SIZE;
    const uint64_t present_rw = PTE_P | PTE_RW;
    t->pml4[0] = (table_physical_base + offsetof(MINT_PAGING_TABLES, pdpt)) | present_rw;
    /* Higher-half direct-map alias. Both PML4 entries share one PDPT so that
       leaf permission changes and mapping teardown remain coherent. */
    t->pml4[256] = t->pml4[0];

    for (uint64_t g = 0; g < 64; ++g) {
        t->pdpt[g] = (table_physical_base + offsetof(MINT_PAGING_TABLES, pd) +
                      g * sizeof(t->pd[0])) | present_rw;
        for (uint64_t p = 0; p < 512; ++p) {
            uint64_t base = g * gib + p * large;
            uint64_t flags = PTE_P | PTE_RW | PDE_PS;
            if (nx_supported) flags |= PTE_NX;
            if (ranges_overlap(base, large, framebuffer_base, framebuffer_size))
                flags |= PTE_PCD | PTE_PWT; /* conservative uncached framebuffer */
            t->pd[g][p] = base | flags;
        }
    }

    uint32_t pt_index = 0;
    uint64_t first = l->kernel_start & ~(large - 1u);
    uint64_t last = (l->kernel_end + large - 1u) & ~(large - 1u);
    if (last < l->kernel_end || last > MINT_IDENTITY_MAP_LIMIT) return 0;
    for (uint64_t chunk = first; chunk < last; chunk += large) {
        if (pt_index >= MINT_PAGING_KERNEL_PT_COUNT) return 0;
        uint64_t g = chunk / gib;
        uint64_t p = (chunk % gib) / large;
        uint64_t pt_phys = table_physical_base + offsetof(MINT_PAGING_TABLES, kernel_pt) +
                           (uint64_t)pt_index * sizeof(t->kernel_pt[0]);
        t->pd[g][p] = pt_phys | present_rw; /* no PS: use 4 KiB leaves */

        for (uint64_t page = 0; page < 512; ++page) {
            uint64_t addr = chunk + page * MINT_PAGING_PAGE_SIZE;
            uint64_t flags = PTE_P;
            int guard_page = 0;
            for (size_t gi = 0; gi < l->guard_page_count; ++gi)
                if (addr == l->guard_pages[gi]) { guard_page = 1; break; }
            if (guard_page) {
                t->kernel_pt[pt_index][page] = 0; /* not present: guard fault */
                continue;
            }
            int text_page = in_range(addr, l->text_start, l->text_end);
            int ro_page = in_range(addr, l->rodata_start, l->rodata_end);
            if (!text_page && !ro_page) flags |= PTE_RW;
            if (nx_supported && !text_page) flags |= PTE_NX;
            if (ranges_overlap(addr, MINT_PAGING_PAGE_SIZE, framebuffer_base, framebuffer_size))
                flags |= PTE_PCD | PTE_PWT;
            t->kernel_pt[pt_index][page] = addr | flags;
        }
        pt_index++;
    }
    return 1;
}


static uint64_t *owned_table_ptr(MINT_PAGING_TABLES *t, uint64_t table_base, uint64_t physical) {
    if (!t || physical < table_base) return 0;
    uint64_t off = physical - table_base;
    if ((off & (MINT_PAGING_PAGE_SIZE - 1u)) || off > sizeof(*t) - MINT_PAGING_PAGE_SIZE) return 0;
    uint64_t k0 = offsetof(MINT_PAGING_TABLES, kernel_pt);
    uint64_t k1 = k0 + sizeof(t->kernel_pt);
    uint64_t d0 = offsetof(MINT_PAGING_TABLES, dynamic_pt);
    uint64_t d1 = d0 + sizeof(t->dynamic_pt);
    if (!((off >= k0 && off < k1) || (off >= d0 && off < d1))) return 0;
    return (uint64_t *)((uint8_t *)t + off);
}

static uint32_t flags_from_entry(uint64_t entry, int nx_supported) {
    uint32_t flags = 0;
    if (entry & PTE_RW) flags |= MINT_PAGE_MAP_WRITE;
    if (!nx_supported || !(entry & PTE_NX)) flags |= MINT_PAGE_MAP_EXEC;
    if (entry & PTE_PCD) flags |= MINT_PAGE_MAP_UNCACHED;
    else if (entry & PTE_PWT) flags |= MINT_PAGE_MAP_WRITE_THROUGH;
    return flags;
}

static uint64_t entry_from_flags(uint64_t physical, uint32_t flags, int nx_supported) {
    uint64_t entry = (physical & PTE_ADDR_MASK) | PTE_P;
    if (flags & MINT_PAGE_MAP_WRITE) entry |= PTE_RW;
    if (nx_supported && !(flags & MINT_PAGE_MAP_EXEC)) entry |= PTE_NX;
    if (flags & MINT_PAGE_MAP_UNCACHED) entry |= PTE_PCD | PTE_PWT;
    else if (flags & MINT_PAGE_MAP_WRITE_THROUGH) entry |= PTE_PWT;
    return entry;
}

static uint64_t *allocate_dynamic_pt(MINT_PAGING_TABLES *t, uint64_t table_base,
                                     uint32_t *used, uint64_t *physical_out) {
    if (!t || !used || *used >= MINT_PAGING_DYNAMIC_PT_COUNT) return 0;
    /* Reuse any free table slot rather than treating the pool as monotonic.
       This is bounded and makes the active-count invariant easy to audit. */
    for (uint32_t index = 0; index < MINT_PAGING_DYNAMIC_PT_COUNT; ++index) {
        uint64_t bit = 1ULL << index;
        if (t->dynamic_pt_alloc_bitmap & bit) continue;
        uint64_t off = offsetof(MINT_PAGING_TABLES, dynamic_pt) +
                       (uint64_t)index * sizeof(t->dynamic_pt[0]);
        uint8_t *bytes = (uint8_t *)t + off;
        for (size_t i = 0; i < sizeof(t->dynamic_pt[0]); ++i) bytes[i] = 0;
        t->dynamic_pt_alloc_bitmap |= bit;
        ++*used;
        if (t == &g_tables && used == &dynamic_pt_used && *used > dynamic_pt_peak)
            dynamic_pt_peak = *used;
        if (physical_out) *physical_out = table_base + off;
        return t->dynamic_pt[index];
    }
    return 0;
}

static int dynamic_pt_index(MINT_PAGING_TABLES *t, uint64_t table_base,
                            uint64_t physical, uint32_t *index_out) {
    if (!t || physical < table_base) return 0;
    uint64_t off = physical - table_base;
    uint64_t first = offsetof(MINT_PAGING_TABLES, dynamic_pt);
    uint64_t bytes = sizeof(t->dynamic_pt);
    if (off < first || off - first >= bytes ||
        ((off - first) % sizeof(t->dynamic_pt[0])) != 0) return 0;
    if (index_out) *index_out = (uint32_t)((off - first) / sizeof(t->dynamic_pt[0]));
    return 1;
}

static int dynamic_pt_is_empty(const uint64_t *pt) {
    if (!pt) return 0;
    for (uint32_t i = 0; i < 512u; ++i) if (pt[i] != 0) return 0;
    return 1;
}

/* Called after clearing the last leaf. Only reclaim a table from our dynamic
   pool, and only when every entry is exactly zero; kernel image PTs are never
   candidates. Clearing the PDE before making the slot reusable avoids aliasing. */
static void reclaim_empty_dynamic_pt(MINT_PAGING_TABLES *t, uint64_t table_base,
                                    uint64_t *pde, uint32_t *used) {
    if (!t || !pde || !used || !(*pde & PTE_P) || (*pde & PDE_PS)) return;
    uint64_t physical = *pde & PTE_ADDR_MASK;
    uint32_t index = 0;
    if (!dynamic_pt_index(t, table_base, physical, &index)) return;
    uint64_t bit = 1ULL << index;
    if (!(t->dynamic_pt_alloc_bitmap & bit)) return;
    uint64_t *pt = owned_table_ptr(t, table_base, physical);
    if (!dynamic_pt_is_empty(pt)) return;
    *pde = 0;
    for (size_t i = 0; i < sizeof(t->dynamic_pt[index]); ++i)
        ((uint8_t *)t->dynamic_pt[index])[i] = 0;
    t->dynamic_pt_alloc_bitmap &= ~bit;
    if (*used) --*used;
    if (t == &g_tables && used == &dynamic_pt_used) ++dynamic_pt_reclaimed;
}

/* Ensure the target 2 MiB PDE uses an owned 4 KiB page table. When splitting
 * a large identity mapping, preserve the old leaf permissions/cache policy. */
static uint64_t *ensure_pte_table(MINT_PAGING_TABLES *t, uint64_t table_base,
                                  uint64_t va, uint32_t *used) {
    uint64_t window_va = 0;
    if (!t || !used || !paging_window_offset(va, &window_va)) return 0;
    va = window_va;
    uint64_t gi = va >> 30;
    uint64_t pi = (va >> 21) & 0x1FFu;
    uint64_t *pde = &t->pd[gi][pi];
    uint64_t old = *pde;
    if (old & PTE_P) {
        if (old & PDE_PS) {
            uint64_t pt_phys = 0;
            uint64_t *pt = allocate_dynamic_pt(t, table_base, used, &pt_phys);
            if (!pt) return 0;
            uint64_t physical_base = old & PDE_LARGE_ADDR_MASK;
            uint64_t leaf_flags = PTE_P | (old & (PTE_RW | PTE_PWT | PTE_PCD | PTE_NX));
            for (uint64_t i = 0; i < 512; ++i)
                pt[i] = physical_base + i * MINT_PAGING_PAGE_SIZE | leaf_flags;
            /* Keep the intermediate entry permissive; leaf PTEs enforce R/W and NX. */
            *pde = (pt_phys & PTE_ADDR_MASK) | PTE_P | PTE_RW;
            return pt;
        }
        return owned_table_ptr(t, table_base, old & PTE_ADDR_MASK);
    }
    uint64_t pt_phys = 0;
    uint64_t *pt = allocate_dynamic_pt(t, table_base, used, &pt_phys);
    if (!pt) return 0;
    *pde = (pt_phys & PTE_ADDR_MASK) | PTE_P | PTE_RW;
    return pt;
}

static uint64_t *lookup_pte(MINT_PAGING_TABLES *t, uint64_t table_base,
                            uint64_t va, uint32_t *used, int create) {
    uint64_t window_va = 0;
    if (!t || !paging_window_offset(va, &window_va) ||
        (va & (MINT_PAGING_PAGE_SIZE - 1u))) return 0;
    va = window_va;
    uint64_t gi = va >> 30;
    uint64_t pi = (va >> 21) & 0x1FFu;
    uint64_t pde = t->pd[gi][pi];
    if ((pde & PTE_P) && (pde & PDE_PS) && !create) return 0;
    if (create) {
        uint64_t *pt = ensure_pte_table(t, table_base, va, used);
        return pt ? &pt[(va >> 12) & 0x1FFu] : 0;
    }
    if (!(pde & PTE_P) || (pde & PDE_PS)) return 0;
    uint64_t *pt = owned_table_ptr(t, table_base, pde & PTE_ADDR_MASK);
    return pt ? &pt[(va >> 12) & 0x1FFu] : 0;
}

int mint_paging_lookup_page_in_tables(const MINT_PAGING_TABLES *ct, uint64_t table_base,
                                      uint64_t va, uint64_t *physical,
                                      uint32_t *flags, int nx_supported) {
    uint64_t window_va = 0;
    if (!ct || !physical || !flags || !paging_window_offset(va, &window_va) ||
        (va & (MINT_PAGING_PAGE_SIZE - 1u))) return 0;
    va = window_va;
    MINT_PAGING_TABLES *t = (MINT_PAGING_TABLES *)(uintptr_t)ct;
    uint64_t gi = va >> 30, pi = (va >> 21) & 0x1FFu;
    uint64_t pde = t->pd[gi][pi];
    if (!(pde & PTE_P)) return 0;
    if (pde & PDE_PS) {
        *physical = (pde & PDE_LARGE_ADDR_MASK) + (va & (MINT_PAGING_LARGE_PAGE_SIZE - 1u));
        *flags = flags_from_entry(pde, nx_supported);
        return 1;
    }
    uint64_t *pte = lookup_pte(t, table_base, va, 0, 0);
    if (!pte || !(*pte & PTE_P)) return 0;
    *physical = *pte & PTE_ADDR_MASK;
    *flags = flags_from_entry(*pte, nx_supported);
    return 1;
}

int mint_paging_map_page_in_tables(MINT_PAGING_TABLES *t, uint64_t table_base,
                                   uint64_t va, uint64_t pa, uint32_t flags,
                                   uint32_t *used, int nx_supported) {
    uint64_t window_va = 0;
    if (!t || !used || (flags & ~PTE_ALLOWED_FLAGS) ||
        !paging_window_offset(va, &window_va) || pa >= MINT_IDENTITY_MAP_LIMIT ||
        (va & (MINT_PAGING_PAGE_SIZE - 1u)) || (pa & (MINT_PAGING_PAGE_SIZE - 1u))) return 0;
    va = window_va;
    uint64_t pde = t->pd[va >> 30][(va >> 21) & 0x1FFu];
    if ((pde & (PTE_P | PDE_PS)) == (PTE_P | PDE_PS)) return 0;
    uint64_t *pte = lookup_pte(t, table_base, va, used, 1);
    if (!pte || (*pte & PTE_P)) return 0;
    *pte = entry_from_flags(pa, flags, nx_supported);
    return 1;
}

int mint_paging_unmap_page_in_tables(MINT_PAGING_TABLES *t, uint64_t table_base,
                                     uint64_t va, uint64_t *old_physical,
                                     uint32_t *old_flags, uint32_t *used,
                                     int nx_supported) {
    uint64_t window_va = 0;
    if (!t || !used || !paging_window_offset(va, &window_va) ||
        (va & (MINT_PAGING_PAGE_SIZE - 1u))) return 0;
    va = window_va;
    uint64_t pde = t->pd[va >> 30][(va >> 21) & 0x1FFu];
    if (!(pde & PTE_P)) return 0;
    uint64_t *pte = lookup_pte(t, table_base, va, used, 1);
    if (!pte || !(*pte & PTE_P)) return 0;
    if (old_physical) *old_physical = *pte & PTE_ADDR_MASK;
    if (old_flags) *old_flags = flags_from_entry(*pte, nx_supported);
    *pte = 0;
    uint64_t *pde_ptr = &t->pd[va >> 30][(va >> 21) & 0x1FFu];
    reclaim_empty_dynamic_pt(t, table_base, pde_ptr, used);
    return 1;
}

int mint_paging_init(const MINT_BOOT_INFO *boot) {
    if (!boot || boot->framebuffer.base >= MINT_IDENTITY_MAP_LIMIT ||
        !boot->framebuffer.size || boot->framebuffer.size > MINT_IDENTITY_MAP_LIMIT - boot->framebuffer.base) {
        mint_serial_write("[paging] framebuffer is outside the 64 GiB identity-map window\n");
        return 0;
    }
    const uint64_t guard_pages[] = {
        (uint64_t)(uintptr_t)__guard_rsp0_before, (uint64_t)(uintptr_t)__guard_rsp0_after,
        (uint64_t)(uintptr_t)__guard_df_before, (uint64_t)(uintptr_t)__guard_df_after,
        (uint64_t)(uintptr_t)__guard_nmi_before, (uint64_t)(uintptr_t)__guard_nmi_after,
        (uint64_t)(uintptr_t)__guard_mc_before, (uint64_t)(uintptr_t)__guard_mc_after
    };
    MINT_PAGING_LAYOUT l = {
        .kernel_start = (uint64_t)(uintptr_t)__kernel_start,
        .kernel_end = (uint64_t)(uintptr_t)__kernel_end,
        .text_start = (uint64_t)(uintptr_t)__kernel_text_start,
        .text_end = (uint64_t)(uintptr_t)__kernel_text_end,
        .rodata_start = (uint64_t)(uintptr_t)__kernel_rodata_start,
        .rodata_end = (uint64_t)(uintptr_t)__kernel_rodata_end,
        .guard_pages = guard_pages,
        .guard_page_count = sizeof(guard_pages) / sizeof(guard_pages[0])
    };
    nx_active = mint_arch_nx_supported();
    dynamic_pt_used = 0;
    dynamic_pt_peak = 0;
    dynamic_pt_reclaimed = 0;
    uint64_t table_base = (uint64_t)(uintptr_t)&g_tables;
    if (!mint_paging_build_tables(&g_tables, table_base,
                                  boot->framebuffer.base, boot->framebuffer.size,
                                  &l, nx_active)) {
        mint_serial_write("[paging] failed to build safe identity page tables\n");
        nx_active = 0;
        return 0;
    }
    /* Turn the final 16 MiB of the identity window into virtual-only space.
       It is reserved from the physical allocator separately by the boot path. */
    dynamic_pt_used = 0;
    for (uint64_t va = MINT_VMALLOC_BASE; va < MINT_IDENTITY_MAP_LIMIT;
         va += MINT_PAGING_PAGE_SIZE) {
        uint64_t old_pa = 0; uint32_t old_flags = 0;
        if (!mint_paging_unmap_page_in_tables(&g_tables, table_base, va,
                                              &old_pa, &old_flags,
                                              &dynamic_pt_used, nx_active)) {
            mint_serial_write("[paging] failed to reserve virtual mapping arena\n");
            nx_active = 0;
            return 0;
        }
    }
    if (!mint_arch_prepare_4level_paging()) {
        nx_active = 0;
        return 0;
    }
    if (nx_active) mint_arch_enable_nxe();
    mint_arch_enable_write_protect();
    mint_arch_load_cr3(table_base);
    paging_active = 1;
    mint_serial_write("[paging] installed 64 GiB identity map plus FFFF8000 higher-half direct-map alias\n");
    mint_serial_write("[paging] 2 MiB leaves; kernel image split into protected 4 KiB pages\n");
    mint_serial_write(nx_active ? "[paging] NX enabled; text RX, rodata R/NX, data RW/NX\n"
                                : "[paging] NX unavailable; write-protect and read-only text enabled\n");
    mint_serial_write("[paging] framebuffer identity range marked uncached\n");
    mint_serial_write("[paging] eight non-present guard pages installed around RSP0/DF/NMI/MC stacks\n");
    return 1;
}

int mint_paging_is_active(void) { return paging_active; }

/* Change the identity-mapped BAR pages to UC before using device MMIO. This
   splits 2 MiB leaves into bounded 4 KiB PTE tables when needed. */
int mint_paging_mark_uncached_range(uint64_t physical_base, uint64_t byte_size) {
    if (!paging_active || !byte_size || physical_base >= MINT_IDENTITY_MAP_LIMIT ||
        byte_size > MINT_IDENTITY_MAP_LIMIT - physical_base) return 0;
    uint64_t start = physical_base & ~(MINT_PAGING_PAGE_SIZE - 1u);
    uint64_t end = (physical_base + byte_size + MINT_PAGING_PAGE_SIZE - 1u) &
                   ~(MINT_PAGING_PAGE_SIZE - 1u);
    if (end < physical_base || end > MINT_IDENTITY_MAP_LIMIT) return 0;
    uint64_t table_base = (uint64_t)(uintptr_t)&g_tables;
    for (uint64_t va = start; va < end; va += MINT_PAGING_PAGE_SIZE) {
        uint64_t *pte = lookup_pte(&g_tables, table_base, va, &dynamic_pt_used, 1);
        if (!pte || !(*pte & PTE_P)) return 0;
        *pte |= PTE_PCD | PTE_PWT;
        __asm__ volatile("invlpg (%0)" :: "r"((uintptr_t)va) : "memory");
    }
    return 1;
}

int mint_paging_export_address_space_template(uint64_t low_pdpt_entries[64],
                                              uint64_t *shared_direct_pml4_entry,
                                              uint64_t *kernel_root_physical) {
    if (!paging_active || !low_pdpt_entries || !shared_direct_pml4_entry || !kernel_root_physical)
        return 0;
    for (unsigned i = 0; i < 64u; ++i) low_pdpt_entries[i] = g_tables.pdpt[i];
    *shared_direct_pml4_entry = g_tables.pml4[256];
    *kernel_root_physical = (uint64_t)(uintptr_t)&g_tables;
    return 1;
}
int mint_paging_nx_is_active(void) { return nx_active && paging_active; }
uint64_t mint_paging_identity_limit(void) { return MINT_IDENTITY_MAP_LIMIT; }
uint64_t mint_paging_direct_map_base(void) { return MINT_DIRECT_MAP_BASE; }

int mint_paging_is_direct_map_address(uint64_t va) {
    return va >= MINT_DIRECT_MAP_BASE && va < MINT_DIRECT_MAP_END;
}

int mint_paging_physical_to_direct_map(uint64_t physical, uint64_t *virtual_address) {
    if (!paging_active || !virtual_address || physical >= MINT_VMALLOC_BASE) return 0;
    uint64_t va = MINT_DIRECT_MAP_BASE + physical;
    uint64_t mapped = 0; uint32_t flags = 0;
    uint64_t page_va = va & ~(MINT_PAGING_PAGE_SIZE - 1u);
    if (!mint_paging_lookup_page(page_va, &mapped, &flags) ||
        mapped != (physical & ~(MINT_PAGING_PAGE_SIZE - 1u))) return 0;
    *virtual_address = va;
    return 1;
}

int mint_paging_direct_map_to_physical(uint64_t va, uint64_t *physical) {
    if (!paging_active || !physical || !mint_paging_is_direct_map_address(va)) return 0;
    uint64_t mapped = 0; uint32_t flags = 0;
    uint64_t page_va = va & ~(MINT_PAGING_PAGE_SIZE - 1u);
    if (!mint_paging_lookup_page(page_va, &mapped, &flags)) return 0;
    *physical = mapped + (va & (MINT_PAGING_PAGE_SIZE - 1u));
    return 1;
}

uint32_t mint_paging_dynamic_tables_used(void) { return dynamic_pt_used; }
uint32_t mint_paging_dynamic_tables_peak(void) { return dynamic_pt_peak; }
uint64_t mint_paging_dynamic_tables_reclaimed(void) { return dynamic_pt_reclaimed; }

int mint_paging_is_vmalloc_address(uint64_t va) {
    uint64_t offset = 0;
    return paging_window_offset(va, &offset) &&
           offset >= MINT_VMALLOC_BASE && offset < MINT_IDENTITY_MAP_LIMIT;
}

int mint_paging_lookup_page(uint64_t va, uint64_t *physical, uint32_t *flags) {
    if (!paging_active) return 0;
    return mint_paging_lookup_page_in_tables(&g_tables, (uint64_t)(uintptr_t)&g_tables,
                                             va, physical, flags, nx_active);
}

static int protected_kernel_virtual(uint64_t va) {
    uint64_t window_va = 0;
    if (!paging_window_offset(va, &window_va)) return 0;
    return window_va >= (uint64_t)(uintptr_t)__kernel_start &&
           window_va < (uint64_t)(uintptr_t)__kernel_end;
}

static void invalidate_window_aliases(uint64_t va) {
    uint64_t window_va = 0;
    if (!paging_window_offset(va, &window_va)) return;
    __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)window_va) : "memory");
    __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)(MINT_DIRECT_MAP_BASE + window_va)) : "memory");
}

int mint_paging_map_page(uint64_t va, uint64_t pa, uint32_t flags) {
    if (!paging_active || protected_kernel_virtual(va)) return 0;
    if (!mint_paging_map_page_in_tables(&g_tables, (uint64_t)(uintptr_t)&g_tables,
                                        va, pa, flags, &dynamic_pt_used, nx_active)) return 0;
    invalidate_window_aliases(va);
    return 1;
}

int mint_paging_unmap_page(uint64_t va, uint64_t *old_physical, uint32_t *old_flags) {
    if (!paging_active || protected_kernel_virtual(va)) return 0;
    if (!mint_paging_unmap_page_in_tables(&g_tables, (uint64_t)(uintptr_t)&g_tables,
                                          va, old_physical, old_flags,
                                          &dynamic_pt_used, nx_active)) return 0;
    invalidate_window_aliases(va);
    return 1;
}

int mint_paging_selftest_page(uint64_t page) {
    if (!paging_active || !page || (page & (MINT_PAGING_PAGE_SIZE - 1u)) ||
        protected_kernel_virtual(page)) return 0;
    /* Verify the four guarded stacks still have non-present translations. This
     * inspects PTEs only; it does not deliberately access a guard address. */
    const uint64_t guards[] = {
        (uint64_t)(uintptr_t)__guard_rsp0_before, (uint64_t)(uintptr_t)__guard_rsp0_after,
        (uint64_t)(uintptr_t)__guard_df_before, (uint64_t)(uintptr_t)__guard_df_after,
        (uint64_t)(uintptr_t)__guard_nmi_before, (uint64_t)(uintptr_t)__guard_nmi_after,
        (uint64_t)(uintptr_t)__guard_mc_before, (uint64_t)(uintptr_t)__guard_mc_after
    };
    uint64_t guard_phys = 0; uint32_t guard_flags = 0;
    for (size_t i = 0; i < sizeof(guards) / sizeof(guards[0]); ++i)
        if (mint_paging_lookup_page(guards[i], &guard_phys, &guard_flags)) return 0;

    uint64_t physical = 0; uint32_t before = 0;
    if (!mint_paging_lookup_page(page, &physical, &before) || physical != page) return 0;
    uint64_t old_physical = 0; uint32_t old_flags = 0;
    if (!mint_paging_unmap_page(page, &old_physical, &old_flags) || old_physical != page) return 0;
    int pass = 0;
    if (mint_paging_map_page(page, page, MINT_PAGE_MAP_WRITE)) {
        uint64_t mapped = 0; uint32_t flags = 0;
        if (mint_paging_lookup_page(page, &mapped, &flags) && mapped == page &&
            (flags & MINT_PAGE_MAP_WRITE) && (!nx_active || !(flags & MINT_PAGE_MAP_EXEC))) {
            volatile uint64_t *word = (volatile uint64_t *)(uintptr_t)page;
            uint64_t old_value = *word;
            *word = 0x4D494E544D415033ULL; /* MINTMAP3 */
            pass = (*word == 0x4D494E544D415033ULL);
            *word = old_value;
        }
        uint64_t ignored_phys = 0; uint32_t ignored_flags = 0;
        if (!mint_paging_unmap_page(page, &ignored_phys, &ignored_flags)) pass = 0;
    }
    if (!mint_paging_map_page(page, page, before)) {
        /* Best-effort recovery: restore the exact attributes reported by the
         * original lookup if the first policy conversion could not be reused. */
        (void)mint_paging_map_page(page, page, old_flags);
        return 0;
    }
    uint64_t restored = 0; uint32_t restored_flags = 0;
    if (!mint_paging_lookup_page(page, &restored, &restored_flags) ||
        restored != page || restored_flags != before) return 0;

    /* Verify the higher-half alias translates to the same physical frame and
       aliases actual memory, not merely a matching page-table entry. */
    uint64_t high_va = 0, roundtrip_pa = 0;
    if (!mint_paging_physical_to_direct_map(page, &high_va) ||
        !mint_paging_direct_map_to_physical(high_va, &roundtrip_pa) ||
        roundtrip_pa != page || !mint_paging_is_direct_map_address(high_va)) return 0;
    volatile uint64_t *low_word = (volatile uint64_t *)(uintptr_t)page;
    volatile uint64_t *high_word = (volatile uint64_t *)(uintptr_t)high_va;
    uint64_t original = *low_word;
    *high_word = 0x4D494E5448494748ULL; /* MINTHIGH */
    pass &= (*low_word == 0x4D494E5448494748ULL);
    *low_word = original;
    if (!pass) return 0;
    mint_serial_write("[paging] higher-half direct-map alias self-test passed\n");
    return 1;
}
