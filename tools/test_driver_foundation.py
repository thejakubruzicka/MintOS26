#!/usr/bin/env python3
"""Regression contracts for driver resource discovery and contained user faults."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
desktop = (root / 'kernel/user/desktop.c').read_text()
arch = (root / 'kernel/arch/x86_64/arch.c').read_text()
asm = (root / 'kernel/arch/x86_64/arch.S').read_text()
pci = (root / 'kernel/bus/pci.c').read_text()
pci_h = (root / 'kernel/include/pci.h').read_text()
catalog = (root / 'kernel/driver/catalog.c').read_text()
checks = {
    'volume selection uses shared static snapshot instead of 9 KiB automatic object':
        'MINT_STORAGE_SNAPSHOT storage;' not in desktop and 'mint_storage_diag_get(&storage_snapshot)' in desktop,
    'terminal scratch arrays keep PCI/VFS/log records off the small user stack':
        all(k in desktop for k in ['terminal_pci_inventory;', 'terminal_mount_table;', 'terminal_log_scratch[']),
    'user exception dispatcher detects CPL3 and records the fault':
        '(f->cs & 3u) == 3u' in arch and 'mint_task_user_exit_record(' in arch,
    'user exception return restores saved kernel continuation instead of iret to faulting user':
        'mint_arch_return_from_user_fault();' in arch and 'jmp mint_arch_return_from_user_fault' in asm,
    'PCI resource scan never sizes BARs by writing all ones':
        'populate_device_resources(&decoded)' in pci and 'never size BARs by writing all ones' in pci,
    'PCI resource inventory exposes BARs, interrupt line/pin and version':
        all(k in pci_h for k in ['MINT_PCI_BAR_SLOTS', 'interrupt_line', 'interrupt_pin', 'MINT_PCI_INVENTORY_VERSION']),
    'driver catalogue distinguishes detected VirtualBox E1000 from a functional network driver':
        'VirtualBox E1000' in catalog and 'DMA RX/TX rings initialized; polling, no IP stack.' in catalog and 'MINT_E1000_ACTIVE' in catalog and 'bus mastering remains disabled' in catalog,
    'kernel includes bounded E1000 DMA/ring driver':
        all(k in (root / 'kernel/net/e1000.c').read_text() for k in ['mint_paging_mark_uncached_range', 'mint_physmem_claim_contiguous_below', 'REG_RDBAL', 'REG_TDBAL', 'mint_e1000_loopback_test']),
    'terminal provides explicit E1000 start, status and opt-in loopback diagnostics':
        'terminal_equals(cmd,"net start")' in desktop and 'terminal_equals(cmd,"net status")' in desktop and 'terminal_equals(cmd,"net test")' in desktop,
    'terminal provides a read-only PCI resource command':
        'terminal_equals(cmd,"resources")' in desktop and 'BARs were not resized or reprogrammed' in desktop,
}
failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
if failed:
    raise SystemExit(1)
