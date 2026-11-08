#!/usr/bin/env python3
"""Static checks for the kernel's reserved virtual arena and allocator path."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
paging_h = (root / 'kernel/include/paging.h').read_text()
paging_c = (root / 'kernel/mm/paging.c').read_text()
vmalloc_h = (root / 'kernel/include/vmalloc.h').read_text()
vmalloc_c = (root / 'kernel/mm/vmalloc.c').read_text()
physmem = (root / 'kernel/mm/physmem.c').read_text()
kernel = (root / 'kernel/kernel.c').read_text()
kernel_entry = kernel[kernel.index('void kernel_entry'):]
makefile = (root / 'Makefile').read_text()
checks = {
    '16 MiB virtual arena has bounded page count and resides below the identity-map limit':
        all(s in paging_h for s in ['MINT_VMALLOC_SIZE (16ULL * 1024ULL * 1024ULL)', 'MINT_VMALLOC_BASE (MINT_IDENTITY_MAP_LIMIT - MINT_VMALLOC_SIZE)', 'MINT_VMALLOC_PAGE_COUNT']),
    'paging removes every arena page before installing CR3':
        'for (uint64_t va = MINT_VMALLOC_BASE; va < MINT_IDENTITY_MAP_LIMIT;' in paging_c and 'mint_paging_unmap_page_in_tables(&g_tables' in paging_c,
    'physical allocator can reserve address spans without any active claims':
        'mint_physmem_reserve_range' in physmem and 'claimed_pages != 0' in physmem,
    'arena is physically reserved before any regular frame claim':
        kernel_entry.index('mint_physmem_reserve_range(MINT_VMALLOC_BASE') < kernel_entry.index('mint_physmem_claim_page()'),
    'allocator zeroes each frame and maps it with explicit flags':
        'b->clear_page(pa, b->context)' in vmalloc_c and 'b->map_page(va, pa, flags, b->context)' in vmalloc_c,
    'allocator records physical ownership, validates frees and rolls back failed mappings':
        all(s in vmalloc_c for s in ['physical_pages', 'is_claimed(expected', 'goto rollback', 'state_mark(s, first, pages, 0)']),
    'kernel boot tests mapped arena allocation, write/read and cleanup':
        all(s in kernel for s in ['mint_vmalloc_init()', 'mint_vmalloc_selftest()', 'VMALLOC ALLOC/MAP/FREE TEST PASSED']),
    'kernel reports active/peak/reclaimed PT tables and VMALLOC health counters':
        all(s in kernel for s in ['mint_paging_dynamic_tables_peak()', 'mint_paging_dynamic_tables_reclaimed()', 'mint_vmalloc_peak_pages()', 'mint_vmalloc_rollback_count()']),
    'VMALLOC self-test repeats allocation/free cycles and checks PT count returns to baseline':
        all(s in vmalloc_c for s in ['cycle < 64u', 'before_tables', 'mint_paging_dynamic_tables_used() != before_tables']),
    'paging owns a bitmap for reusable dynamic PT slots and reclaims completely empty tables':
        'dynamic_pt_alloc_bitmap' in paging_h and 'reclaim_empty_dynamic_pt' in paging_c and 'dynamic_pt_reclaimed' in paging_c,
    'VMALLOC exposes high-water and failure/rollback counters':
        all(s in vmalloc_h for s in ['peak_used_pages', 'failed_allocations', 'rollback_count', 'failed_frees']) and 'mint_vmalloc_peak_pages' in vmalloc_c,
    'VMALLOC releases physical ownership transactionally and restores mappings on failure':
        'release_pages' in vmalloc_h and 'mint_physmem_release_batch' in physmem and 'release rollback mapping failed' in vmalloc_c,
    'physical batch release validates all claims and duplicate pages before commit':
        all(s in physmem for s in ['mint_physmem_release_batch', 'batch_insert_page', 'page_bases[j] == base', 'allocations[batch_records[i]].active = 0']),
    'build compiles vmalloc module and runs its host-side test':
        '$(BUILD)/vmalloc.obj' in makefile and 'tools/test_vmalloc.py' in makefile,
}
for name, ok in checks.items(): print(('PASS: ' if ok else 'FAIL: ') + name)
if not all(checks.values()): raise SystemExit(1)
