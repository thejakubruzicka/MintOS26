#include <stdint.h>
#include "../include/physmem.h"
#include "../include/serial.h"

#define MINT_EFI_CONVENTIONAL_MEMORY 7u
#define MINT_MAX_RANGES 512u
#define MINT_MAX_ALLOCS 4096u
#define MINT_MIN_USABLE_ADDRESS (1024ULL * 1024ULL)
#define MINT_PHYSICAL_MAP_LIMIT (64ULL * 1024ULL * 1024ULL * 1024ULL)

struct efi_memory_descriptor_prefix {
    uint32_t type;
    uint32_t padding;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};
struct page_range { uint64_t base, pages; };
struct allocation_record { uint64_t base, pages; uint8_t active; };
static struct page_range ranges[MINT_MAX_RANGES];
static struct allocation_record allocations[MINT_MAX_ALLOCS];
/* Static scratch makes batch release transactional without consuming kernel stack. */
static struct page_range batch_ranges[MINT_MAX_RANGES];
static uint16_t batch_records[MINT_MAX_ALLOCS];
static uint32_t batch_range_count;
static uint32_t range_count;
static uint64_t total_pages, free_pages, claimed_pages;

static uint64_t range_end(const struct page_range *r) {
    return r->base + r->pages * MINT_PAGE_SIZE;
}
static void sort_and_merge_ranges(void) {
    for (uint32_t i = 1; i < range_count; ++i) {
        struct page_range key = ranges[i];
        uint32_t j = i;
        while (j && ranges[j - 1].base > key.base) { ranges[j] = ranges[j - 1]; --j; }
        ranges[j] = key;
    }
    uint32_t out = 0;
    for (uint32_t i = 0; i < range_count; ++i) {
        if (!ranges[i].pages) continue;
        if (!out) { ranges[out++] = ranges[i]; continue; }
        struct page_range *prev = &ranges[out - 1];
        uint64_t prev_end = range_end(prev);
        uint64_t curr_end = range_end(&ranges[i]);
        if (ranges[i].base <= prev_end) {
            if (curr_end > prev_end) prev->pages = (curr_end - prev->base) / MINT_PAGE_SIZE;
        } else ranges[out++] = ranges[i];
    }
    range_count = out;
}
static void compact_empty_ranges(void) {
    uint32_t out = 0;
    for (uint32_t i = 0; i < range_count; ++i)
        if (ranges[i].pages) ranges[out++] = ranges[i];
    range_count = out;
}
static int add_range(uint64_t base, uint64_t pages) {
    if (!pages || range_count >= MINT_MAX_RANGES ||
        pages > UINT64_MAX / MINT_PAGE_SIZE) return 0;
    uint64_t bytes = pages * MINT_PAGE_SIZE;
    if ((base & (MINT_PAGE_SIZE - 1u)) || base > UINT64_MAX - bytes) return 0;
    ranges[range_count++] = (struct page_range){base, pages};
    return 1;
}
static int find_free_record(void) {
    for (uint32_t i = 0; i < MINT_MAX_ALLOCS; ++i) if (!allocations[i].active) return (int)i;
    return -1;
}

int mint_physmem_init(const MINT_BOOT_INFO *boot) {
    range_count = 0; total_pages = 0; free_pages = 0; claimed_pages = 0;
    for (uint32_t i = 0; i < MINT_MAX_ALLOCS; ++i) allocations[i].active = 0;
    if (!boot || !boot->memory_map_physical || !boot->memory_map_size ||
        boot->memory_descriptor_size < sizeof(struct efi_memory_descriptor_prefix) ||
        boot->memory_map_size % boot->memory_descriptor_size != 0 ||
        boot->memory_map_size / boot->memory_descriptor_size > 65536ULL) {
        mint_serial_write("[mm] invalid UEFI memory-map bounds/stride\n");
        return 0;
    }
    const uint8_t *map = (const uint8_t *)(uintptr_t)boot->memory_map_physical;
    uint64_t count = boot->memory_map_size / boot->memory_descriptor_size;
    for (uint64_t i = 0; i < count; ++i) {
        const struct efi_memory_descriptor_prefix *d =
            (const struct efi_memory_descriptor_prefix *)(const void *)(map + i * boot->memory_descriptor_size);
        if (d->type != MINT_EFI_CONVENTIONAL_MEMORY || !d->number_of_pages) continue;
        if (d->number_of_pages > UINT64_MAX / MINT_PAGE_SIZE) continue;
        uint64_t bytes = d->number_of_pages * MINT_PAGE_SIZE;
        if (d->physical_start > UINT64_MAX - bytes ||
            (d->physical_start & (MINT_PAGE_SIZE - 1u))) continue;
        uint64_t base = d->physical_start;
        uint64_t pages = d->number_of_pages;
        if (base >= MINT_PHYSICAL_MAP_LIMIT) continue;
        uint64_t map_pages = (MINT_PHYSICAL_MAP_LIMIT - base) / MINT_PAGE_SIZE;
        if (pages > map_pages) pages = map_pages;
        if (!pages) continue;
        if (base < MINT_MIN_USABLE_ADDRESS) {
            uint64_t skip = (MINT_MIN_USABLE_ADDRESS - base + MINT_PAGE_SIZE - 1u) / MINT_PAGE_SIZE;
            if (skip >= pages) continue;
            base += skip * MINT_PAGE_SIZE;
            pages -= skip;
        }
        if (!add_range(base, pages)) break;
    }
    sort_and_merge_ranges();
    total_pages = 0;
    for (uint32_t i = 0; i < range_count; ++i) {
        if (ranges[i].pages > UINT64_MAX - total_pages) { total_pages = 0; range_count = 0; break; }
        total_pages += ranges[i].pages;
    }
    free_pages = total_pages;
    mint_serial_write("[mm] UEFI descriptors: "); mint_serial_dec64(count); mint_serial_write("\n");
    mint_serial_write("[mm] conventional pages: "); mint_serial_dec64(total_pages); mint_serial_write("\n");
    mint_serial_write("[mm] merged free ranges: "); mint_serial_dec64(range_count); mint_serial_write("\n");
    return range_count != 0;
}

uint64_t mint_physmem_total_pages(void) { return total_pages; }
uint64_t mint_physmem_free_pages(void) { return free_pages; }
uint64_t mint_physmem_claimed_pages(void) { return claimed_pages; }


int mint_physmem_reserve_range(uint64_t base, uint64_t pages) {
    if (!pages || claimed_pages != 0 || (base & (MINT_PAGE_SIZE - 1u)) ||
        pages > UINT64_MAX / MINT_PAGE_SIZE) return 0;
    uint64_t bytes = pages * MINT_PAGE_SIZE;
    if (base > UINT64_MAX - bytes) return 0;
    uint64_t end = base + bytes;

    /* A contiguous exclusion can split at most one already-merged free run.
       Check capacity before editing so failure leaves the allocator unchanged. */
    int requires_split = 0;
    for (uint32_t i = 0; i < range_count; ++i) {
        uint64_t lo = ranges[i].base, hi = range_end(&ranges[i]);
        if (lo < base && hi > end) { requires_split = 1; break; }
    }
    if (requires_split && range_count >= MINT_MAX_RANGES) return 0;

    uint64_t removed = 0;
    uint32_t i = 0;
    while (i < range_count) {
        uint64_t lo = ranges[i].base, hi = range_end(&ranges[i]);
        uint64_t cut_lo = lo > base ? lo : base;
        uint64_t cut_hi = hi < end ? hi : end;
        if (cut_lo >= cut_hi) { ++i; continue; }
        uint64_t removed_here = (cut_hi - cut_lo) / MINT_PAGE_SIZE;
        removed += removed_here;
        if (lo < cut_lo && cut_hi < hi) {
            /* Split into the preserved prefix and suffix. */
            for (uint32_t j = range_count; j > i + 1u; --j) ranges[j] = ranges[j - 1u];
            ranges[i].pages = (cut_lo - lo) / MINT_PAGE_SIZE;
            ranges[i + 1u].base = cut_hi;
            ranges[i + 1u].pages = (hi - cut_hi) / MINT_PAGE_SIZE;
            ++range_count;
            i += 2u;
        } else if (lo < cut_lo) {
            ranges[i].pages = (cut_lo - lo) / MINT_PAGE_SIZE;
            ++i;
        } else if (cut_hi < hi) {
            ranges[i].base = cut_hi;
            ranges[i].pages = (hi - cut_hi) / MINT_PAGE_SIZE;
            ++i;
        } else {
            for (uint32_t j = i + 1u; j < range_count; ++j) ranges[j - 1u] = ranges[j];
            --range_count;
        }
    }
    /* Intersections are carved only from free ranges; this bound is an invariant. */
    if (removed > free_pages || removed > total_pages) {
        mint_serial_write("[mm] reserve-range invariant violation\n");
        return 0;
    }
    free_pages -= removed;
    total_pages -= removed;
    mint_serial_write("[mm] excluded physical range pages: ");
    mint_serial_dec64(removed);
    mint_serial_write("\n");
    return 1;
}

uint64_t mint_physmem_claim_contiguous_below(uint64_t pages, uint64_t max_address_exclusive) {
    if (!pages || pages > UINT64_MAX / MINT_PAGE_SIZE || pages > free_pages ||
        !max_address_exclusive) return 0;
    if (max_address_exclusive > MINT_PHYSICAL_MAP_LIMIT)
        max_address_exclusive = MINT_PHYSICAL_MAP_LIMIT;
    uint64_t bytes = pages * MINT_PAGE_SIZE;
    if (bytes > max_address_exclusive) return 0;
    int record = find_free_record();
    if (record < 0) return 0;
    for (uint32_t i = 0; i < range_count; ++i) {
        if (ranges[i].pages < pages) continue;
        uint64_t base = ranges[i].base;
        if ((base & (MINT_PAGE_SIZE - 1u)) || base >= max_address_exclusive ||
            bytes > max_address_exclusive - base) continue;
        allocations[record] = (struct allocation_record){base, pages, 1};
        ranges[i].base += bytes;
        ranges[i].pages -= pages;
        free_pages -= pages;
        claimed_pages += pages;
        compact_empty_ranges();
        return base;
    }
    return 0;
}
/* Claim from the top of the highest range that fits. Large CPU-only buffers
 * (display backbuffers can exceed 30 MiB at 4K) use this so they do not
 * consume the low memory that DMA devices restricted below 4 GiB need.
 * Ranges are clipped to MINT_PHYSICAL_MAP_LIMIT at init, so the result is
 * always inside the identity map. */
uint64_t mint_physmem_claim_contiguous_high(uint64_t pages) {
    if (!pages || pages > UINT64_MAX / MINT_PAGE_SIZE || pages > free_pages) return 0;
    uint64_t bytes = pages * MINT_PAGE_SIZE;
    int record = find_free_record();
    if (record < 0) return 0;
    for (uint32_t k = range_count; k-- > 0;) {
        if (ranges[k].pages < pages) continue;
        uint64_t end = range_end(&ranges[k]);
        if (end > MINT_PHYSICAL_MAP_LIMIT || end < bytes) continue;
        uint64_t base = end - bytes;
        if (base & (MINT_PAGE_SIZE - 1u)) continue;
        allocations[record] = (struct allocation_record){base, pages, 1};
        ranges[k].pages -= pages;
        free_pages -= pages;
        claimed_pages += pages;
        compact_empty_ranges();
        return base;
    }
    return 0;
}

uint64_t mint_physmem_claim_contiguous(uint64_t pages) {
    return mint_physmem_claim_contiguous_below(pages, MINT_PHYSICAL_MAP_LIMIT);
}
uint64_t mint_physmem_claim_page(void) { return mint_physmem_claim_contiguous(1); }

int mint_physmem_is_claimed(uint64_t base, uint64_t pages) {
    if (!pages) return 0;
    for (uint32_t i = 0; i < MINT_MAX_ALLOCS; ++i)
        if (allocations[i].active && allocations[i].base == base && allocations[i].pages == pages) return 1;
    return 0;
}


static int batch_insert_page(uint64_t base) {
    uint64_t bytes = MINT_PAGE_SIZE;
    uint64_t end = base + bytes;
    uint32_t pos = 0;
    while (pos < batch_range_count && batch_ranges[pos].base < base) ++pos;
    if (pos && range_end(&batch_ranges[pos - 1]) > base) return 0;
    if (pos < batch_range_count && batch_ranges[pos].base < end) return 0;
    int left = pos > 0 && range_end(&batch_ranges[pos - 1]) == base;
    int right = pos < batch_range_count && batch_ranges[pos].base == end;
    if (left && right) {
        batch_ranges[pos - 1].pages += 1u + batch_ranges[pos].pages;
        for (uint32_t i = pos + 1u; i < batch_range_count; ++i) batch_ranges[i - 1u] = batch_ranges[i];
        --batch_range_count;
        return 1;
    }
    if (left) { ++batch_ranges[pos - 1].pages; return 1; }
    if (right) { batch_ranges[pos].base = base; ++batch_ranges[pos].pages; return 1; }
    if (batch_range_count >= MINT_MAX_RANGES) return 0;
    for (uint32_t i = batch_range_count; i > pos; --i) batch_ranges[i] = batch_ranges[i - 1u];
    batch_ranges[pos] = (struct page_range){base, 1};
    ++batch_range_count;
    return 1;
}

int mint_physmem_release_batch(const uint64_t *page_bases, uint32_t count) {
    if (!page_bases || !count || count > MINT_MAX_ALLOCS || count > claimed_pages ||
        count > UINT64_MAX - free_pages) return 0;
    /* Validate every claim first; no live allocator state changes until commit. */
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t base = page_bases[i];
        if ((base & (MINT_PAGE_SIZE - 1u)) || base >= MINT_PHYSICAL_MAP_LIMIT ||
            MINT_PAGE_SIZE > MINT_PHYSICAL_MAP_LIMIT - base) return 0;
        int found = -1;
        for (uint32_t r = 0; r < MINT_MAX_ALLOCS; ++r) {
            if (allocations[r].active && allocations[r].base == base && allocations[r].pages == 1u) {
                found = (int)r; break;
            }
        }
        if (found < 0) return 0;
        for (uint32_t j = 0; j < i; ++j) if (page_bases[j] == base) return 0;
        batch_records[i] = (uint16_t)found;
    }
    batch_range_count = range_count;
    for (uint32_t i = 0; i < range_count; ++i) batch_ranges[i] = ranges[i];
    for (uint32_t i = 0; i < count; ++i) {
        if (!batch_insert_page(page_bases[i])) return 0;
    }
    /* Commit is now infallible: every insertion and ownership check succeeded. */
    for (uint32_t i = 0; i < batch_range_count; ++i) ranges[i] = batch_ranges[i];
    for (uint32_t i = batch_range_count; i < range_count; ++i) ranges[i] = (struct page_range){0, 0};
    range_count = batch_range_count;
    for (uint32_t i = 0; i < count; ++i) allocations[batch_records[i]].active = 0;
    free_pages += count;
    claimed_pages -= count;
    return 1;
}

int mint_physmem_release_contiguous(uint64_t base, uint64_t pages) {
    if (!pages || (base & (MINT_PAGE_SIZE - 1u)) || pages > UINT64_MAX / MINT_PAGE_SIZE ||
        base >= MINT_PHYSICAL_MAP_LIMIT || pages * MINT_PAGE_SIZE > MINT_PHYSICAL_MAP_LIMIT - base) return 0;
    int record = -1;
    for (uint32_t i = 0; i < MINT_MAX_ALLOCS; ++i) {
        if (allocations[i].active && allocations[i].base == base && allocations[i].pages == pages) {
            record = (int)i; break;
        }
    }
    if (record < 0 || pages > claimed_pages || pages > UINT64_MAX - free_pages) return 0;
    uint64_t bytes = pages * MINT_PAGE_SIZE;
    /* If adjacent to an existing free run, merge without needing a new slot. */
    int merged = 0;
    for (uint32_t i = 0; i < range_count; ++i) {
        uint64_t end = range_end(&ranges[i]);
        if (end == base) { ranges[i].pages += pages; merged = 1; break; }
        if (base + bytes == ranges[i].base) { ranges[i].base = base; ranges[i].pages += pages; merged = 1; break; }
        /* Any overlap means allocator metadata is corrupt: refuse to double-free. */
        if (base < end && ranges[i].base < base + bytes) return 0;
    }
    if (!merged) {
        if (range_count >= MINT_MAX_RANGES) return 0;
        ranges[range_count++] = (struct page_range){base, pages};
    }
    sort_and_merge_ranges();
    allocations[record].active = 0;
    free_pages += pages;
    claimed_pages -= pages;
    return 1;
}
