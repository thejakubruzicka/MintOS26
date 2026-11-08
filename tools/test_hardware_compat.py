#!/usr/bin/env python3
"""26.5.009.0 real-hardware compatibility checks: display buffers for any GOP
mode, PAT write-combining, Intel RAID-mode AHCI and PS/2 9-bit deltas."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = os.environ.get('HOST_CC', 'clang')
with tempfile.TemporaryDirectory(prefix='mint-display-') as tmp:
    exe = Path(tmp) / 'display-backbuffer'
    subprocess.run([cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_display_backbuffer.c'),
                    str(root / 'kernel/ui/display_surface.c'),
                    str(root / 'kernel/ui/desktop_event.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

display = (root / 'kernel/ui/display_surface.c').read_text()
kernel = (root / 'kernel/kernel.c').read_text()
paging = (root / 'kernel/mm/paging.c').read_text()
arch = (root / 'kernel/arch/x86_64/arch.c').read_text()
ahci = (root / 'kernel/block/ahci.c').read_text()
physmem = (root / 'kernel/mm/physmem.c').read_text()
ps2 = (root / 'kernel/input/ps2.c').read_text()

raid_begin = ahci.find('if (is_intel_raid_mode_ahci(d)) {')
raid_vs = ahci.find('ahci_version_plausible(vs)', raid_begin)
busmaster = ahci.find('(1u << 1) | (1u << 2)', raid_begin)
checks = {
    'no fixed 1920x1080 backbuffer arrays remain in the kernel image':
        'MINT_BACKBUFFER_MAX_PIXELS' not in display and 'static uint32_t *display_backbuffer;' in display,
    'kernel sizes backbuffers from the GOP mode and claims them from high memory':
        'mint_display_backbuffer_pixels_required()' in kernel and 'mint_physmem_claim_contiguous_high(pages)' in kernel,
    'high claim allocates from the top of the highest fitting range':
        'for (uint32_t k = range_count; k-- > 0;)' in physmem and 'uint64_t base = end - bytes;' in physmem,
    'PAT is programmed as a whole table with PA4 = write-combining':
        '0x00070406u' in arch and '0x00070400u | PAT_TYPE_WC' in arch and 'wbinvd' in arch,
    'PAT support is checked with CPUID before touching the MSR':
        '(edx & (1u << 16))' in arch,
    'splitting a large page carries the PAT bit to 4 KiB PTEs (never WC -> WB)':
        'if (old & PDE_PAT) leaf_flags |= PTE_PAT;' in paging,
    'only the framebuffer range becomes WC; edge leaves are split':
        'mint_paging_set_framebuffer_write_combining' in paging and 'lookup_pte(&g_tables, table_base, va, &dynamic_pt_used, 1)' in paging,
    'scanout writes are fenced so WC data is visible':
        'display_flush_scanout()' in display and '"sfence"' in display,
    'Intel RAID-mode (class 01:04:00) SATA is matched only for vendor 8086':
        'd->vendor_id == PCI_VENDOR_INTEL' in ahci and 'd->subclass == PCI_SUBCLASS_RAID' in ahci,
    'RAID-mode ABAR is validated as AHCI before bus mastering is enabled':
        raid_begin >= 0 and 0 <= raid_vs < busmaster,
    'PS/2 deltas use the byte-0 sign bits (9-bit two\'s complement)':
        '(mouse_packet[0] & 0x10u) != 0' in ps2 and '(mouse_packet[0] & 0x20u) != 0' in ps2,
}
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
if not all(checks.values()):
    raise SystemExit(1)
