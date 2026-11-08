#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../kernel/include/boot_abi.h"
#include "../kernel/include/physmem.h"

struct test_efi_descriptor {
    uint32_t type, padding;
    uint64_t physical_start, virtual_start, number_of_pages, attribute;
};
void mint_serial_write(const char *s) { (void)s; }
void mint_serial_dec64(uint64_t v) { (void)v; }

int main(void) {
    struct test_efi_descriptor map[3] = {
        { 7, 0, 0x80000, 0, 512, 0 },       /* clip first 128 pages below 1 MiB */
        { 6, 0, 0x800000, 0, 999, 0 },     /* reserved: must be ignored */
        { 7, 0, 0x400000, 0, 10, 0 }       /* another conventional range */
    };
    MINT_BOOT_INFO boot = {0};
    boot.magic = MINT_BOOT_INFO_MAGIC;
    boot.abi_version = MINT_BOOT_INFO_ABI;
    boot.struct_size = sizeof(boot);
    boot.memory_map_physical = (uint64_t)(uintptr_t)map;
    boot.memory_map_size = sizeof(map);
    boot.memory_descriptor_size = sizeof(map[0]);
    assert(mint_physmem_init(&boot) == 1);
    assert(mint_physmem_total_pages() == 394);
    assert(mint_physmem_free_pages() == 394);
    uint64_t first = mint_physmem_claim_page();
    assert(first == 0x100000);
    assert((first & (MINT_PAGE_SIZE - 1)) == 0);
    assert(mint_physmem_free_pages() == 393);
    uint64_t second = mint_physmem_claim_page();
    assert(second == 0x101000);
    assert(mint_physmem_free_pages() == 392);
    uint64_t contig = mint_physmem_claim_contiguous(4);
    assert(contig == 0x102000);
    assert(mint_physmem_free_pages() == 388);
    assert((contig & (MINT_PAGE_SIZE - 1)) == 0);
    assert(mint_physmem_claimed_pages() == 6);
    assert(mint_physmem_is_claimed(first, 1));
    assert(mint_physmem_is_claimed(contig, 4));
    assert(!mint_physmem_is_claimed(contig, 1));
    assert(!mint_physmem_release_contiguous(contig, 1)); /* partial release forbidden */
    assert(!mint_physmem_release_contiguous(first + 1, 1)); /* alignment forbidden */
    assert(mint_physmem_release_contiguous(first, 1));
    assert(!mint_physmem_release_contiguous(first, 1)); /* double free forbidden */
    assert(mint_physmem_release_contiguous(contig, 4));
    assert(mint_physmem_release_contiguous(second, 1));
    assert(mint_physmem_claimed_pages() == 0);
    assert(mint_physmem_free_pages() == mint_physmem_total_pages());
    uint64_t whole = mint_physmem_claim_contiguous(384);
    assert(whole == 0x100000);
    assert(mint_physmem_free_pages() == 10);
    assert(mint_physmem_release_contiguous(whole, 384));
    assert(mint_physmem_free_pages() == mint_physmem_total_pages());

    /* Exclude a virtual-only arena that overlaps one available physical range. */
    uint64_t before_reserve = mint_physmem_free_pages();
    assert(mint_physmem_reserve_range(0x102000, 2));
    assert(mint_physmem_total_pages() == before_reserve - 2);
    assert(mint_physmem_free_pages() == before_reserve - 2);
    uint64_t low_run = mint_physmem_claim_contiguous(2);
    assert(low_run == 0x100000);
    uint64_t after_hole = mint_physmem_claim_page();
    assert(after_hole == 0x104000); /* reserved physical pages are never returned */
    assert(mint_physmem_release_contiguous(low_run, 2));
    assert(mint_physmem_release_contiguous(after_hole, 1));
    assert(mint_physmem_free_pages() == mint_physmem_total_pages());
    assert(!mint_physmem_reserve_range(0x100001, 1));

    /* Batch release is atomic: duplicate input must fail without releasing any frame. */
    uint64_t batch[3] = { mint_physmem_claim_page(), mint_physmem_claim_page(), mint_physmem_claim_page() };
    assert(batch[0] && batch[1] && batch[2]);
    uint64_t free_before_batch = mint_physmem_free_pages();
    uint64_t claimed_before_batch = mint_physmem_claimed_pages();
    uint64_t duplicate[2] = { batch[0], batch[0] };
    assert(!mint_physmem_release_batch(duplicate, 2));
    assert(mint_physmem_free_pages() == free_before_batch);
    assert(mint_physmem_claimed_pages() == claimed_before_batch);
    assert(mint_physmem_is_claimed(batch[0], 1) && mint_physmem_is_claimed(batch[1], 1) && mint_physmem_is_claimed(batch[2], 1));
    uint64_t unsorted[3] = { batch[2], batch[0], batch[1] };
    assert(mint_physmem_release_batch(unsorted, 3));
    assert(mint_physmem_free_pages() == free_before_batch + 3);
    assert(mint_physmem_claimed_pages() == claimed_before_batch - 3);
    assert(!mint_physmem_is_claimed(batch[0], 1) && !mint_physmem_is_claimed(batch[1], 1) && !mint_physmem_is_claimed(batch[2], 1));
    assert(!mint_physmem_release_batch(unsorted, 3)); /* all-or-nothing double-release rejection */
    assert(mint_physmem_free_pages() == mint_physmem_total_pages());
    /* Reinitialize with split low/high memory to prove a 32-bit DMA request cannot
       borrow a run above 4 GiB and that failed bounded requests consume nothing. */
    struct test_efi_descriptor dma_map[2] = {
        { 7, 0, 0x400000, 0, 2, 0 },
        { 7, 0, 0x100000000ULL, 0, 8, 0 }
    };
    boot.memory_map_physical=(uint64_t)(uintptr_t)dma_map;
    boot.memory_map_size=sizeof(dma_map);
    boot.memory_descriptor_size=sizeof(dma_map[0]);
    assert(mint_physmem_init(&boot)==1);
    uint64_t free_before_limit=mint_physmem_free_pages();
    assert(mint_physmem_claim_contiguous_below(4,0x100000000ULL)==0);
    assert(mint_physmem_free_pages()==free_before_limit);
    uint64_t low_dma=mint_physmem_claim_contiguous_below(2,0x100000000ULL);
    assert(low_dma==0x400000 && low_dma+2*MINT_PAGE_SIZE<=0x100000000ULL);
    assert(mint_physmem_release_contiguous(low_dma,2));
    uint64_t wide_dma=mint_physmem_claim_contiguous(4);
    assert(wide_dma==0x100000000ULL);
    assert(mint_physmem_release_contiguous(wide_dma,4));
    assert(mint_physmem_free_pages()==mint_physmem_total_pages());
    puts("PASS: physical memory allocator enforces max-address constraints for PCI DMA ranges");
    puts("PASS: C physical-memory parser normalizes ranges, clips low RAM, ignores reserved ranges, tracks claims, rejects partial/double frees, coalesces ranges, reserves spans, and transactionally releases batches");
    return 0;
}
