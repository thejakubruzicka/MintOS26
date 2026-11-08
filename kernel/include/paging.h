#ifndef MINTOS26_PAGING_H
#define MINTOS26_PAGING_H
#include <stdint.h>
#include <stddef.h>
#include "boot_abi.h"

#define MINT_IDENTITY_MAP_LIMIT (64ULL * 1024ULL * 1024ULL * 1024ULL)
/* Canonical higher-half alias for the low 64 GiB physical identity window.
 * This is a direct-map alias; the kernel image itself is still linked low. */
#define MINT_DIRECT_MAP_BASE 0xFFFF800000000000ULL
#define MINT_DIRECT_MAP_END (MINT_DIRECT_MAP_BASE + MINT_IDENTITY_MAP_LIMIT)
#define MINT_PAGING_KERNEL_PT_COUNT 16u
#define MINT_PAGING_DYNAMIC_PT_COUNT 64u
#define MINT_PAGE_MAP_WRITE (1u << 0)
#define MINT_PAGE_MAP_EXEC (1u << 1)
#define MINT_PAGE_MAP_UNCACHED (1u << 2)
#define MINT_PAGE_MAP_WRITE_THROUGH (1u << 3)
#define MINT_PAGING_PAGE_SIZE 4096ULL
/* 16 MiB virtual-only arena removed from the top of the identity map. */
#define MINT_VMALLOC_SIZE (16ULL * 1024ULL * 1024ULL)
#define MINT_VMALLOC_BASE (MINT_IDENTITY_MAP_LIMIT - MINT_VMALLOC_SIZE)
#define MINT_VMALLOC_PAGE_COUNT ((uint32_t)(MINT_VMALLOC_SIZE / MINT_PAGING_PAGE_SIZE))
#define MINT_PAGING_LARGE_PAGE_SIZE (2ULL * 1024ULL * 1024ULL)

/* Static, page-aligned paging structures. Identity maps physical addresses
 * below 64 GiB with 2 MiB leaves, splitting kernel image chunks into 4 KiB
 * PTEs to enforce RX/R/RW+NX permissions. */
typedef struct __attribute__((aligned(4096))) mint_paging_tables {
    uint64_t pml4[512] __attribute__((aligned(4096)));
    uint64_t pdpt[512] __attribute__((aligned(4096)));
    uint64_t pd[64][512] __attribute__((aligned(4096)));
    uint64_t kernel_pt[MINT_PAGING_KERNEL_PT_COUNT][512] __attribute__((aligned(4096)));
    /* Bounded pool for splitting huge pages into 4 KiB PTE tables at runtime. */
    uint64_t dynamic_pt[MINT_PAGING_DYNAMIC_PT_COUNT][512] __attribute__((aligned(4096)));
    /* One bit per dynamic page-table frame: set while a PDE owns the table. */
    uint64_t dynamic_pt_alloc_bitmap;
} MINT_PAGING_TABLES;

typedef struct mint_paging_layout {
    uint64_t kernel_start, kernel_end;
    uint64_t text_start, text_end;
    uint64_t rodata_start, rodata_end;
    /* Optional 4 KiB pages to leave non-present inside the kernel image. */
    const uint64_t *guard_pages;
    size_t guard_page_count;
} MINT_PAGING_LAYOUT;

/* Builder is privilege-free and host-testable; table_physical_base must be the
 * identity-mapped physical address represented by `tables` in the kernel. */
int mint_paging_build_tables(MINT_PAGING_TABLES *tables,
                             uint64_t table_physical_base,
                             uint64_t framebuffer_base,
                             uint64_t framebuffer_size,
                             const MINT_PAGING_LAYOUT *layout,
                             int nx_supported);
int mint_paging_init(const MINT_BOOT_INFO *boot);
int mint_paging_is_active(void);
int mint_paging_mark_uncached_range(uint64_t physical_base, uint64_t byte_size);
int mint_paging_set_framebuffer_write_combining(uint64_t physical_base, uint64_t byte_size);
/* Expose the current shared supervisor mapping template for constructing independent roots. */
int mint_paging_export_address_space_template(uint64_t low_pdpt_entries[64],
                                              uint64_t *shared_direct_pml4_entry,
                                              uint64_t *kernel_root_physical);
int mint_paging_nx_is_active(void);
uint64_t mint_paging_identity_limit(void);
uint64_t mint_paging_direct_map_base(void);
int mint_paging_physical_to_direct_map(uint64_t physical, uint64_t *virtual_address);
int mint_paging_direct_map_to_physical(uint64_t virtual_address, uint64_t *physical);
int mint_paging_is_direct_map_address(uint64_t virtual_address);
uint32_t mint_paging_dynamic_tables_used(void);
uint32_t mint_paging_dynamic_tables_peak(void);
uint64_t mint_paging_dynamic_tables_reclaimed(void);
int mint_paging_is_vmalloc_address(uint64_t virtual_address);
/* Map/unmap only within the lower 64 GiB identity-map window. Addresses must
 * be 4 KiB aligned. Kernel-image pages are protected from runtime remapping. */
int mint_paging_map_page(uint64_t virtual_address, uint64_t physical_address, uint32_t flags);
int mint_paging_unmap_page(uint64_t virtual_address, uint64_t *old_physical, uint32_t *old_flags);
int mint_paging_lookup_page(uint64_t virtual_address, uint64_t *physical, uint32_t *flags);
/* Host-testable helpers use a synthetic physical base and never execute privileged instructions. */
int mint_paging_map_page_in_tables(MINT_PAGING_TABLES *tables, uint64_t table_physical_base,
                                   uint64_t virtual_address, uint64_t physical_address,
                                   uint32_t flags, uint32_t *dynamic_tables_used, int nx_supported);
int mint_paging_unmap_page_in_tables(MINT_PAGING_TABLES *tables, uint64_t table_physical_base,
                                     uint64_t virtual_address, uint64_t *old_physical,
                                     uint32_t *old_flags, uint32_t *dynamic_tables_used,
                                     int nx_supported);
int mint_paging_lookup_page_in_tables(const MINT_PAGING_TABLES *tables, uint64_t table_physical_base,
                                      uint64_t virtual_address, uint64_t *physical,
                                      uint32_t *flags, int nx_supported);
int mint_paging_selftest_page(uint64_t physical_page);

#endif
