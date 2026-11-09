#ifndef MINTOS26_VMALLOC_H
#define MINTOS26_VMALLOC_H
#include <stddef.h>
#include <stdint.h>
#include "paging.h"

#define MINT_VMALLOC_MAX_ALLOCS 256u
#define MINT_VMALLOC_BITMAP_BYTES (MINT_VMALLOC_PAGE_COUNT / 8u)

typedef struct mint_vmalloc_record {
    uint32_t first_page;
    uint32_t page_count;
    uint32_t flags;
    uint8_t active;
} MINT_VMALLOC_RECORD;

typedef struct mint_vmalloc_state {
    uint8_t bitmap[MINT_VMALLOC_BITMAP_BYTES];
    uint64_t physical_pages[MINT_VMALLOC_PAGE_COUNT];
    MINT_VMALLOC_RECORD allocations[MINT_VMALLOC_MAX_ALLOCS];
    uint32_t live_allocations;
    uint32_t used_pages;
    uint32_t peak_used_pages;
    uint32_t total_allocations;
    uint32_t total_frees;
    uint32_t failed_allocations;
    uint32_t rollback_count;
    uint32_t failed_frees;
    uint8_t initialized;
} MINT_VMALLOC_STATE;

typedef struct mint_vmalloc_backend {
    uint64_t (*claim_page)(void *context);
    int (*release_page)(uint64_t physical, void *context);
    /* Optional all-or-nothing release for multiple independently claimed pages. */
    int (*release_pages)(const uint64_t *physical_pages, uint32_t count, void *context);
    int (*is_claimed)(uint64_t physical, void *context);
    int (*map_page)(uint64_t virtual_address, uint64_t physical, uint32_t flags, void *context);
    int (*unmap_page)(uint64_t virtual_address, uint64_t *physical, void *context);
    int (*lookup_page)(uint64_t virtual_address, uint64_t *physical, uint32_t *flags, void *context);
    void (*clear_page)(uint64_t physical, void *context);
    void *context;
} MINT_VMALLOC_BACKEND;

int mint_vmalloc_state_init(MINT_VMALLOC_STATE *state);
int mint_vmalloc_state_find_run(const MINT_VMALLOC_STATE *state, uint32_t arena_pages,
                                uint32_t requested_pages, uint32_t *first_page);
int mint_vmalloc_state_alloc(MINT_VMALLOC_STATE *state, uint64_t arena_base,
                             uint32_t arena_pages, size_t bytes, uint32_t flags,
                             const MINT_VMALLOC_BACKEND *backend, uint64_t *virtual_out);
int mint_vmalloc_state_free(MINT_VMALLOC_STATE *state, uint64_t arena_base,
                            uint32_t arena_pages, uint64_t virtual_address,
                            const MINT_VMALLOC_BACKEND *backend);

int mint_vmalloc_init(void);
void *mint_vmalloc_alloc(size_t bytes, uint32_t flags);
int mint_vmalloc_free(void *address);
int mint_vmalloc_selftest(void);
/* Uncached mapping for device registers anywhere in the physical space. */
void *mint_mmio_map(uint64_t physical, uint64_t bytes);
uint32_t mint_vmalloc_live_allocations(void);
uint32_t mint_vmalloc_used_pages(void);
uint32_t mint_vmalloc_peak_pages(void);
uint32_t mint_vmalloc_total_allocations(void);
uint32_t mint_vmalloc_total_frees(void);
uint32_t mint_vmalloc_failed_allocations(void);
uint32_t mint_vmalloc_rollback_count(void);
uint32_t mint_vmalloc_failed_frees(void);

#endif
