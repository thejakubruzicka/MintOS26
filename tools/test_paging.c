#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include "../kernel/include/paging.h"

/* Unused kernel-init path dependencies for the host-side table builder test. */
char kernel_start_test[1] __asm__("__kernel_start");
char kernel_end_test[1] __asm__("__kernel_end");
char kernel_text_start_test[1] __asm__("__kernel_text_start");
char kernel_text_end_test[1] __asm__("__kernel_text_end");
char kernel_rodata_start_test[1] __asm__("__kernel_rodata_start");
char kernel_rodata_end_test[1] __asm__("__kernel_rodata_end");
char guard_rsp0_before_test[1] __asm__("__guard_rsp0_before");
char guard_rsp0_after_test[1] __asm__("__guard_rsp0_after");
char guard_df_before_test[1] __asm__("__guard_df_before");
char guard_df_after_test[1] __asm__("__guard_df_after");
char guard_nmi_before_test[1] __asm__("__guard_nmi_before");
char guard_nmi_after_test[1] __asm__("__guard_nmi_after");
char guard_mc_before_test[1] __asm__("__guard_mc_before");
char guard_mc_after_test[1] __asm__("__guard_mc_after");
int mint_arch_nx_supported(void) { return 1; }
int mint_arch_prepare_4level_paging(void) { return 1; }
void mint_arch_enable_nxe(void) {}
void mint_arch_enable_write_protect(void) {}
void mint_arch_load_cr3(uint64_t root) { (void)root; }
void mint_serial_write(const char *s) { (void)s; }
void mint_serial_hex64(uint64_t n) { (void)n; }

#define P 1ULL
#define RW 2ULL
#define PWT 8ULL
#define PCD 16ULL
#define PS 128ULL
#define NX (1ULL << 63)

int main(void) {
    MINT_PAGING_TABLES *t = aligned_alloc(4096, sizeof(MINT_PAGING_TABLES));
    assert(t);
    uint64_t table_phys = 0x06000000ULL;
    const uint64_t guards[] = { 0x04003000ULL, 0x04004000ULL };
    MINT_PAGING_LAYOUT l = {
        .kernel_start = 0x04000000ULL,
        .kernel_end = 0x04005000ULL,
        .text_start = 0x04000000ULL,
        .text_end = 0x04000800ULL,
        .rodata_start = 0x04001000ULL,
        .rodata_end = 0x04001800ULL,
        .guard_pages = guards,
        .guard_page_count = sizeof(guards) / sizeof(guards[0])
    };
    assert(mint_paging_build_tables(t, table_phys, 0xC0000000ULL, 8ULL*1024*1024, &l, 1));
    assert(t->pml4[0] == ((table_phys + offsetof(MINT_PAGING_TABLES, pdpt)) | P | RW));
    assert(t->pml4[256] == t->pml4[0]); /* higher-half direct-map alias */
    assert(mint_paging_is_direct_map_address(MINT_DIRECT_MAP_BASE));
    assert(mint_paging_is_direct_map_address(MINT_DIRECT_MAP_END - 4096u));
    assert(!mint_paging_is_direct_map_address(MINT_DIRECT_MAP_END));
    assert(t->pdpt[3] == ((table_phys + offsetof(MINT_PAGING_TABLES, pd) + 3*sizeof(t->pd[0])) | P | RW));
    assert((t->pd[0][0] & (P|RW|PS|NX)) == (P|RW|PS|NX));
    assert((t->pd[3][0] & (P|RW|PS|PCD|PWT)) == (P|RW|PS|PCD|PWT));
    assert((t->pd[3][1] & (P|PCD|PWT)) == (P|PCD|PWT));
    uint64_t kernel_pt_phys = table_phys + offsetof(MINT_PAGING_TABLES, kernel_pt);
    assert(t->pd[0][32] == (kernel_pt_phys | P | RW));
    uint64_t text = t->kernel_pt[0][0];
    assert((text & (P|RW|NX)) == P); /* executable, read-only */
    uint64_t ro = t->kernel_pt[0][1];
    assert((ro & (P|RW|NX)) == (P|NX)); /* read-only, non-executable */
    uint64_t data = t->kernel_pt[0][2];
    assert((data & (P|RW|NX)) == (P|RW|NX));
    assert((data & 0x000FFFFFFFFFF000ULL) == 0x04002000ULL);
    assert(t->kernel_pt[0][3] == 0); /* non-present guard page */
    assert(t->kernel_pt[0][4] == 0); /* second non-present guard page */
    assert(!mint_paging_build_tables(t, table_phys, MINT_IDENTITY_MAP_LIMIT-4096, 8192, &l, 1));
    const uint64_t bad_guard[] = { 0x04003001ULL };
    MINT_PAGING_LAYOUT invalid = l;
    invalid.guard_pages = bad_guard;
    invalid.guard_page_count = 1;
    assert(!mint_paging_build_tables(t, table_phys, 0xC0000000ULL, 8ULL*1024*1024, &invalid, 1));
    assert(mint_paging_build_tables(t, table_phys, 0xC0000000ULL, 8ULL*1024*1024, &l, 0));
    assert((t->pd[0][0] & NX) == 0 && (t->kernel_pt[0][2] & NX) == 0);

    /* Dynamic unmap/split/remap path on a synthetic 2 MiB identity mapping. */
    assert(mint_paging_build_tables(t, table_phys, 0, 0, &l, 1));
    uint32_t pt_used = 0, flags = 0, before_flags = 0;
    uint64_t phys = 0, old_phys = 0;
    const uint64_t target = 0x80000000ULL;
    assert(mint_paging_lookup_page_in_tables(t, table_phys, target, &phys, &before_flags, 1));
    assert(phys == target && (before_flags & MINT_PAGE_MAP_WRITE) && !(before_flags & MINT_PAGE_MAP_EXEC));
    uint64_t high_target = MINT_DIRECT_MAP_BASE + target;
    uint64_t high_phys = 0; uint32_t high_flags = 0;
    assert(mint_paging_lookup_page_in_tables(t, table_phys, high_target, &high_phys, &high_flags, 1));
    assert(high_phys == target && high_flags == before_flags);
    assert(mint_paging_unmap_page_in_tables(t, table_phys, high_target, &old_phys, &flags, &pt_used, 1));
    assert(old_phys == target && flags == before_flags && pt_used == 1);
    assert(!mint_paging_lookup_page_in_tables(t, table_phys, target, &phys, &flags, 1));
    assert(!mint_paging_lookup_page_in_tables(t, table_phys, high_target, &phys, &flags, 1));
    assert(mint_paging_map_page_in_tables(t, table_phys, high_target, 0x00100000ULL,
                                          MINT_PAGE_MAP_WRITE, &pt_used, 1));
    assert(mint_paging_lookup_page_in_tables(t, table_phys, target, &phys, &flags, 1));
    assert(phys == 0x00100000ULL && (flags & MINT_PAGE_MAP_WRITE) && !(flags & MINT_PAGE_MAP_EXEC));
    assert(mint_paging_lookup_page_in_tables(t, table_phys, high_target, &high_phys, &high_flags, 1));
    assert(high_phys == phys && high_flags == flags);
    assert(!mint_paging_map_page_in_tables(t, table_phys, target, target,
                                           MINT_PAGE_MAP_WRITE, &pt_used, 1));
    assert(mint_paging_unmap_page_in_tables(t, table_phys, target, &old_phys, &flags, &pt_used, 1));
    assert(old_phys == 0x00100000ULL);
    assert(mint_paging_map_page_in_tables(t, table_phys, target, target,
                                          before_flags, &pt_used, 1));
    assert(mint_paging_lookup_page_in_tables(t, table_phys, target, &phys, &flags, 1));
    assert(phys == target && flags == before_flags);
    assert(!mint_paging_unmap_page_in_tables(t, table_phys, target + 1, &old_phys, &flags, &pt_used, 1));
    assert(!mint_paging_map_page_in_tables(t, table_phys, MINT_IDENTITY_MAP_LIMIT,
                                           0x00100000ULL, 0, &pt_used, 1));

    /* Isolate an absent PDE: its page-table page should be reclaimed every time
       the single mapped leaf is removed, and its slot should be reusable. */
    assert(mint_paging_build_tables(t, table_phys, 0, 0, &l, 1));
    pt_used = 0;
    const uint64_t isolated = 0x500000000ULL;
    uint64_t igi = isolated >> 30, ipi = (isolated >> 21) & 0x1FFu;
    t->pd[igi][ipi] = 0;
    for (uint32_t cycle = 0; cycle < 512u; ++cycle) {
        uint64_t va = isolated + (uint64_t)(cycle % 512u) * MINT_PAGING_PAGE_SIZE;
        assert(mint_paging_map_page_in_tables(t, table_phys, va,
            0x00100000ULL + (uint64_t)(cycle % 512u) * MINT_PAGING_PAGE_SIZE,
            MINT_PAGE_MAP_WRITE, &pt_used, 1));
        assert(pt_used == 1);
        assert(t->dynamic_pt_alloc_bitmap != 0);
        assert(mint_paging_unmap_page_in_tables(t, table_phys, va, &old_phys, &flags, &pt_used, 1));
        assert(pt_used == 0);
        assert(t->pd[igi][ipi] == 0);
        assert(t->dynamic_pt_alloc_bitmap == 0);
    }

    /* Fill all dynamic page-table slots, reject exhaustion cleanly, then free
       each empty branch and ensure the complete pool becomes available again. */
    const uint64_t pool_base = 0x510000000ULL;
    for (uint32_t i = 0; i < MINT_PAGING_DYNAMIC_PT_COUNT; ++i) {
        uint64_t va = pool_base + (uint64_t)i * MINT_PAGING_LARGE_PAGE_SIZE;
        uint64_t gi = va >> 30, pi = (va >> 21) & 0x1FFu;
        t->pd[gi][pi] = 0;
        assert(mint_paging_map_page_in_tables(t, table_phys, va,
            0x01000000ULL + (uint64_t)i * MINT_PAGING_PAGE_SIZE,
            MINT_PAGE_MAP_WRITE, &pt_used, 1));
        assert(pt_used == i + 1u);
    }
    uint64_t exhausted_va = pool_base + (uint64_t)MINT_PAGING_DYNAMIC_PT_COUNT * MINT_PAGING_LARGE_PAGE_SIZE;
    t->pd[exhausted_va >> 30][(exhausted_va >> 21) & 0x1FFu] = 0;
    assert(!mint_paging_map_page_in_tables(t, table_phys, exhausted_va, 0x02000000ULL,
                                            MINT_PAGE_MAP_WRITE, &pt_used, 1));
    assert(pt_used == MINT_PAGING_DYNAMIC_PT_COUNT);
    for (uint32_t i = MINT_PAGING_DYNAMIC_PT_COUNT; i > 0; --i) {
        uint64_t va = pool_base + (uint64_t)(i - 1u) * MINT_PAGING_LARGE_PAGE_SIZE;
        assert(mint_paging_unmap_page_in_tables(t, table_phys, va, &old_phys, &flags, &pt_used, 1));
    }
    assert(pt_used == 0 && t->dynamic_pt_alloc_bitmap == 0);
    free(t);
    puts("PASS: paging protections/cache flags; low+high alias coherence; dynamic PT reclamation/slot reuse; pool exhaustion/recovery; invalid and duplicate mapping rejection");
    return 0;
}
