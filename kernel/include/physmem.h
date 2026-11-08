#ifndef MINTOS26_PHYSMEM_H
#define MINTOS26_PHYSMEM_H
#include <stdint.h>
#include "boot_abi.h"

#define MINT_PAGE_SIZE 4096ULL
int mint_physmem_init(const MINT_BOOT_INFO *boot);
uint64_t mint_physmem_total_pages(void);
uint64_t mint_physmem_free_pages(void);
uint64_t mint_physmem_claimed_pages(void);
/* Claims are tracked by exact base+length; only the original whole claim may be released. */
uint64_t mint_physmem_claim_page(void);
uint64_t mint_physmem_claim_contiguous(uint64_t pages);
uint64_t mint_physmem_claim_contiguous_high(uint64_t pages);
/* Claim a physically contiguous range wholly below max_address_exclusive. */
uint64_t mint_physmem_claim_contiguous_below(uint64_t pages, uint64_t max_address_exclusive);
int mint_physmem_release_contiguous(uint64_t base, uint64_t pages);
/* Transactionally release a set of individually claimed single pages. Either all pages return to the free ranges or none do. */
int mint_physmem_release_batch(const uint64_t *page_bases, uint32_t count);
int mint_physmem_is_claimed(uint64_t base, uint64_t pages);
/* Exclude a page-aligned physical range from future claims. Call before any claims. */
int mint_physmem_reserve_range(uint64_t base, uint64_t pages);

#endif
