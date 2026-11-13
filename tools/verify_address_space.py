#!/usr/bin/env python3
"""Static contracts for independent roots and supervisor-only shared mappings."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/mm/address_space.c').read_text()
header = (root / 'kernel/include/address_space.h').read_text()
kernel = (root / 'kernel/kernel.c').read_text()
makefile = (root / 'Makefile').read_text()
checks = {
    'private user window is bounded between 64 and 512 GiB': 'MINT_ASPACE_USER_BASE MINT_IDENTITY_MAP_LIMIT' in header and 'MINT_ASPACE_USER_END (512ULL' in header,
    'each address space receives an independent root and private PDPT': 'space->root_physical = root_pa' in source and 'space->pdpt_physical = pdpt_pa' in source and 'pdpt[i] = g_shared_low_pdpt[i]' in source,
    'shared higher-half direct map remains supervisor-only': 'root[256] = g_shared_direct_pml4_entry & ~PTE_US' in source,
    'private intermediate and leaf entries require user permission': source.count('PTE_US') >= 8 and 'PTE_P | PTE_RW | PTE_US' in source and 'PTE_P | PTE_US' in source,
    'data frames belong to one address space and cannot be cross-mapped': 'find_frame(space, pa)' in source and 'PTE_OWNED_MARK' in source and 'pending_remove(space, frame_slot)' in source,
    'demand-zero regions and guard pages (26.5.014.0)': 'mint_address_space_demand_fault' in source and 'MINT_ASPACE_REGION_GUARD' in header and 'region_is_demand' in source,
    'address-space teardown rejects active roots and frees private subtrees': 'space->active || g_current_space == space' in source and 'USER_PDPT_FIRST; di < USER_PDPT_END' in source and 'table_release(space, pd_pa)' in source,
    'kernel exercises two roots with the same VA mapping to separate frames': 'address_space_isolation_selftest' in kernel and 'mint_address_space_activate(&address_space_test_a)' in kernel and 'mint_address_space_activate(&address_space_test_b)' in kernel,
    'host test covers rollback, address isolation and no leaks': 'simulate partial page-table allocation failure' in (root / 'tools/test_address_space.c').read_text() and 'no physical page leaks' in (root / 'tools/test_address_space.c').read_text(),
    'runtime isolation self-test reports a specific failure stage': 'FAILED stage: ' in kernel,
    'kernel cleanup releases only regions successfully reserved by the runtime test': 'region_a_reserved = mint_address_space_region_reserve' in kernel and 'if (region_a_reserved && !mint_address_space_region_release' in kernel and 'rollback_va' not in kernel[kernel.index('static int address_space_isolation_selftest'):kernel.index('static void draw_early_kernel')],
    'address-space tests are included in release suite': 'tools/test_address_space.py' in makefile and 'tools/verify_address_space.py' in makefile,
}
for name, passed in checks.items():
    print(('PASS: ' if passed else 'FAIL: ') + name)
if not all(checks.values()):
    raise SystemExit(1)
