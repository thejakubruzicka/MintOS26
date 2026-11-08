#ifndef MINTOS26_ADDRESS_SPACE_H
#define MINTOS26_ADDRESS_SPACE_H

#include <stdint.h>
#include "paging.h"

#define MINT_ASPACE_SHARED_PDPT_ENTRIES 64u
#define MINT_ASPACE_PML4_ENTRIES 512u
#define MINT_ASPACE_MAX_USER_FRAMES 256u
#define MINT_ASPACE_MAX_REGIONS 32u
#define MINT_ASPACE_USER_BASE MINT_IDENTITY_MAP_LIMIT
#define MINT_ASPACE_USER_END (512ULL * 1024ULL * 1024ULL * 1024ULL)
#define MINT_ASPACE_PTE_USER (1ULL << 2)

typedef struct mint_address_space_backend {
    void *context;
    uint64_t (*claim_page)(void *context);
    int (*release_page)(void *context, uint64_t physical_page);
    void *(*physical_to_pointer)(void *context, uint64_t physical_page);
    int (*is_claimed_single_page)(void *context, uint64_t physical_page);
    void (*load_root)(void *context, uint64_t root_physical);
    void (*invalidate_page)(void *context, uint64_t virtual_address);
} MINT_ADDRESS_SPACE_BACKEND;

typedef struct mint_address_space_frame {
    uint64_t physical;
    uint32_t active;
    uint32_t mapped;
    uint64_t virtual_address;
} MINT_ADDRESS_SPACE_FRAME;

typedef enum mint_address_region_kind {
    MINT_ASPACE_REGION_ANONYMOUS = 1,
    MINT_ASPACE_REGION_STACK = 2,
    MINT_ASPACE_REGION_IMAGE_TEXT = 3,
    MINT_ASPACE_REGION_IMAGE_DATA = 4,
    MINT_ASPACE_REGION_SHARED = 5
} MINT_ASPACE_REGION_KIND;

typedef struct mint_address_region {
    uint64_t base;
    uint64_t size;
    uint32_t flags;
    uint32_t kind;
    uint32_t active;
    uint32_t reserved;
} MINT_ADDRESS_REGION;

typedef struct mint_address_space {
    uint64_t root_physical;
    uint64_t pdpt_physical;
    uint64_t table_pages_live;
    uint64_t table_pages_peak;
    uint64_t table_pages_reclaimed;
    uint64_t mappings_live;
    uint64_t mappings_created;
    uint64_t mappings_removed;
    uint64_t frame_pages_live;
    uint64_t frame_pages_peak;
    uint32_t initialized;
    uint32_t active;
    uint32_t nx_supported;
    uint32_t reserved;
    MINT_ADDRESS_SPACE_FRAME frames[MINT_ASPACE_MAX_USER_FRAMES];
    MINT_ADDRESS_REGION regions[MINT_ASPACE_MAX_REGIONS];
    uint64_t regions_live;
} MINT_ADDRESS_SPACE;

/* Global address-space manager is initialized once from the active kernel root.
 * It copies the low kernel PDPT template, shares the supervisor-only direct
 * map, and exposes a private user range from 64 GiB through 512 GiB. */
int mint_address_space_system_init(const MINT_ADDRESS_SPACE_BACKEND *backend,
                                   uint64_t kernel_root_physical,
                                   const uint64_t shared_low_pdpt[MINT_ASPACE_SHARED_PDPT_ENTRIES],
                                   uint64_t shared_direct_pml4_entry,
                                   int nx_supported);
int mint_address_space_create(MINT_ADDRESS_SPACE *space);
int mint_address_space_destroy(MINT_ADDRESS_SPACE *space);
int mint_address_space_activate(MINT_ADDRESS_SPACE *space);
int mint_address_space_activate_kernel(void);
uint64_t mint_address_space_root(const MINT_ADDRESS_SPACE *space);

/* User pages must first be obtained from the specific address space. A frame
 * cannot be mapped into a different space through this API. */
uint64_t mint_address_space_alloc_frame(MINT_ADDRESS_SPACE *space);
int mint_address_space_release_frame(MINT_ADDRESS_SPACE *space, uint64_t physical_page);
int mint_address_space_map(MINT_ADDRESS_SPACE *space, uint64_t virtual_page,
                           uint64_t physical_page, uint32_t flags);
int mint_address_space_unmap(MINT_ADDRESS_SPACE *space, uint64_t virtual_page,
                             uint64_t *physical_page, uint32_t *flags);
int mint_address_space_lookup(const MINT_ADDRESS_SPACE *space, uint64_t virtual_address,
                              uint64_t *physical_address, uint32_t *flags);
int mint_address_space_is_user_address(uint64_t virtual_address);
const MINT_ADDRESS_SPACE *mint_address_space_current(void);
/* Validated user-memory helpers: reject unmapped, cross-region, overlong, or
 * non-writable ranges before copying via the physical-memory backend. */
int mint_address_space_validate_user_range(const MINT_ADDRESS_SPACE *space,
                                           uint64_t user_address, size_t length,
                                           int require_write);
int mint_address_space_copy_from_user(const MINT_ADDRESS_SPACE *space, void *kernel_dst,
                                      uint64_t user_src, size_t length);
int mint_address_space_copy_to_user(const MINT_ADDRESS_SPACE *space, uint64_t user_dst,
                                    const void *kernel_src, size_t length);
/* Trusted loader-only initialization of already-mapped owned user pages. Unlike
 * copy_to_user, it may populate RX pages while the address space is inactive. */
int mint_address_space_initialize_mapped_bytes(MINT_ADDRESS_SPACE *space, uint64_t user_dst,
                                               const void *kernel_src, size_t length);
/* Address regions (the early VMA layer) define allowed virtual ranges and
 * permissions before individual pages may be mapped. Regions are page-aligned,
 * non-overlapping, and currently single-core/kernel-managed. Writable+executable
 * regions are rejected. */
int mint_address_space_region_reserve(MINT_ADDRESS_SPACE *space, uint64_t base,
                                      uint64_t size, uint32_t flags,
                                      MINT_ASPACE_REGION_KIND kind);
int mint_address_space_region_release(MINT_ADDRESS_SPACE *space, uint64_t base);
int mint_address_space_region_query(const MINT_ADDRESS_SPACE *space, uint64_t address,
                                    MINT_ADDRESS_REGION *region_out);
int mint_address_space_selftest(void);

#endif
