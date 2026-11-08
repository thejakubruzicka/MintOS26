#!/usr/bin/env python3
"""Static contract checks for the opt-in native x86-64 kernel core."""
from pathlib import Path
import subprocess
import re
root = Path(__file__).resolve().parents[1]
elf = root / 'build/mintos26-kernel.elf'
if not elf.exists():
    raise SystemExit('FAIL: kernel ELF missing')

def cmd(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout

info = cmd('readelf', '-h', str(elf))
loads = cmd('readelf', '-l', str(elf))
syms = cmd('readelf', '-s', str(elf))
kernel = (root / 'kernel/kernel.c').read_text()
arch_c = (root / 'kernel/arch/x86_64/arch.c').read_text()
arch_s = (root / 'kernel/arch/x86_64/arch.S').read_text()
mm = (root / 'kernel/mm/physmem.c').read_text()
serial = (root / 'kernel/console/serial.c').read_text()
abi = (root / 'kernel/include/boot_abi.h').read_text()
loader = (root / 'uefi/main.c').read_text()
makefile = (root / 'Makefile').read_text()
checks = {
    'freestanding x86-64 ELF': 'ELF64' in info and 'Advanced Micro Devices X86-64' in info,
    'kernel entry remains at reserved 64 MiB physical address': 'Entry point address:               0x4000000' in info and '0x0000000004000000' in loads,
    'ELF permissions separate RX code and RW data segments': len(re.findall(r'^\s*LOAD\s', loads, re.M)) == 3 and 'R E' in loads and 'R  ' in loads and 'RW' in loads and 'RWE' not in loads,
    'IRQ0 vector 32 is present in the kernel IDT': 'set_gate(32, (uintptr_t)mint_irq0_stub)' in arch_c and 'mint_irq0_stub' in arch_s,
    'entry point uses UEFI Microsoft x64 ABI': 'kernel_entry' in syms and 'ms_abi' in kernel,
    'stable handoff includes framebuffer and memory map': all(x in abi for x in ['MINT_BOOT_INFO_MAGIC', 'MINT_BOOT_INFO_ABI', 'MINT_FRAMEBUFFER_INFO', 'memory_map_physical']),
    'kernel has its own GDT and reloads code/data segments': '00AF9A000000FFFFULL' in arch_c and 'mint_load_gdt' in arch_c and 'lgdt (%rdi)' in arch_s and 'lretq' in arch_s,
    'kernel owns IDT and installs CPU exception vectors 0..31': 'static struct idt_gate idt[256]' in arch_c and 'for (unsigned i = 0; i < 32; ++i)' in arch_c and 'lidt (%rdi)' in arch_s,
    'kernel installs 64-bit TSS and dedicated IST stacks for double fault, NMI and machine check': all(x in arch_c for x in ['struct mint_tss64', 'install_tss()', 'mint_load_tss(0x18u)', 'set_gate_ist(8', 'set_gate_ist(2', 'set_gate_ist(18']) and 'ltr %di' in arch_s,
    'page fault panic reports CR2 and decodes error bits': all(x in arch_c for x in ['mint_arch_read_cr2()', 'CR2 FAULT ADDRESS=', 'protection-violation', 'instruction-fetch']),
    'exception stubs cover all 32 vectors with error-code variants': all(f'MINT_ISR_NOERR {i}' in arch_s or f'MINT_ISR_ERR   {i}' in arch_s for i in range(32)),
    'exception panic logs vector, RIP, error and flags': all(x in arch_c for x in ['CPU exception:', 'RIP=', 'ERROR=', 'RFLAGS=']),
    'early COM1 UART logger is bounded and emits CRLF': all(x in serial for x in ['COM1 0x3F8u', '100000u', "if (*s == '\\n')"]),
    'legacy PIC is remapped with timer-only baseline and conditional PS/2 masks': all(x in arch_c for x in ['outb(PIC1_DATA, 0x20u)', 'outb(PIC2_DATA, 0x28u)', 'pic1_mask = 0xFEu', 'pic2_mask = 0xFFu', 'mint_arch_enable_input_irqs']),
    'PIT channel 0 configured near 100 Hz': all(x in arch_c for x in ['PIT_INPUT_HZ 1193182u', 'MINT_TIMER_HZ 100u', 'outb(PIT_COMMAND, 0x36u)', 'mint_timer_init']),
    'IRQ dispatcher increments ticks and sends PIC EOI': all(x in arch_c for x in ['timer_ticks++', 'frame->vector == 32u', 'outb(PIC1_COMMAND, 0x20u)', 'mint_timer_ticks']),
    'IRQ assembly saves GPRs, restores stack, and returns with iretq': all(x in arch_s for x in ['mint_irq_common:', 'andq $-16, %rsp', 'callq mint_irq_dispatch', 'popq %r15', 'popq %rax', 'addq $16, %rsp', 'iretq']),
    'kernel enables interrupts only after PIC/PIT init and updates live timer display': all(x in kernel for x in ['mint_timer_init();', 'draw_runtime_status(0, 0, mint_task_live_count(),', '"sti"', 'mint_timer_ticks()', 'TIMER TICKS:', 'mint_scheduler_run();']),
    'kernel validates UEFI descriptors and counts conventional pages': all(x in mm for x in ['MINT_EFI_CONVENTIONAL_MEMORY 7u', 'memory_descriptor_size', 'number_of_pages', 'mint_physmem_init']),
    'runtime 4 KiB mapping API splits huge pages and invalidates TLB': all(x in (root / 'kernel/mm/paging.c').read_text() for x in ['mint_paging_map_page_in_tables', 'mint_paging_unmap_page_in_tables', 'mint_paging_lookup_page_in_tables', 'MINT_PAGING_DYNAMIC_PT_COUNT', 'invlpg']),
    'kernel validates page-map changes and guard translations before scheduler': 'mint_paging_selftest_page(reserved_page)' in kernel and 'PAGE MAP/UNMAP + HIGH ALIAS TEST PASSED' in kernel and 'higher-half direct-map alias self-test passed' in (root / 'kernel/mm/paging.c').read_text() and 'mint_paging_lookup_page(guards[i]' in (root / 'kernel/mm/paging.c').read_text(),
    'physical page allocator tracks exact claims and supports safe release/coalescing': all(x in (root / 'kernel/include/physmem.h').read_text() for x in ['mint_physmem_claim_page', 'mint_physmem_claim_contiguous', 'mint_physmem_release_contiguous', 'mint_physmem_claimed_pages']) and 'MINT_PAGE_SIZE 4096ULL' in (root / 'kernel/include/physmem.h').read_text() and all(x in mm for x in ['MINT_MAX_ALLOCS', 'mint_physmem_claim_page', 'allocations[record]', 'sort_and_merge_ranges', 'mint_physmem_release_contiguous']),
    'kernel has its own 64 GiB identity map, split kernel pages and guarded NX/WP setup': all(x in (root / 'kernel/mm/paging.c').read_text() for x in ['MINT_IDENTITY_MAP_LIMIT', 'kernel_pt', 'PTE_NX', 'mint_arch_enable_write_protect', 'mint_arch_load_cr3', 'mint_arch_prepare_4level_paging', 'PTE_PCD | PTE_PWT', 'guard_page_count', 'guard_page)']),
    'linker creates guard pages around four kernel/IST stacks': all(x in (root / 'kernel/linker.ld').read_text() for x in ['__guard_rsp0_before', '__guard_rsp0_after', '__guard_df_before', '__guard_df_after', '__guard_nmi_before', '__guard_nmi_after', '__guard_mc_before', '__guard_mc_after']),
    'paging explicitly enables PAE/PSE and refuses unsafe LA57 transition': all(x in arch_c for x in ['mint_arch_prepare_4level_paging', '1ULL << 12', '1ULL << 5', '1ULL << 4', 'five-level paging']),
    'higher-half 64 GiB direct-map alias shares the PDPT and invalidates both aliases': all(x in (root / 'kernel/mm/paging.c').read_text() for x in ['MINT_DIRECT_MAP_BASE', 't->pml4[256] = t->pml4[0]', 'paging_window_offset', 'invalidate_window_aliases', 'mint_paging_physical_to_direct_map', 'mint_paging_direct_map_to_physical']),
    'kernel uses page-backed 256 KiB heap with split/coalesce and free validation': all(x in (root / 'kernel/mm/heap.c').read_text() for x in ['mint_kmalloc', 'mint_kfree', 'HEAP_MAGIC', 'remainder >= sizeof(heap_block_t)', 'Coalesce adjacent free blocks']) and 'heap_pages * MINT_PAGE_SIZE' in kernel,
    'kernel reports paging and heap status independently': all(x in kernel for x in ['64 GIB IDENTITY + HIGH DIRECT MAP ACTIVE', 'FIRMWARE PAGE MAP RETAINED', 'KERNEL HEAP READY', 'KERNEL HEAP UNAVAILABLE']),
    'kernel status screen uses its framebuffer without UEFI services': all(x in kernel for x in ['KERNEL CORE ONLINE', 'OWN GDT IDT INSTALLED', 'CPU EXCEPTION HANDLERS ARMED', 'MINT_FRAMEBUFFER_INFO']),
    'SIMD and red zone disabled for kernel code': '-mno-sse' in makefile and '-mno-mmx' in makefile and '-mno-red-zone' in makefile,
    'kernel boot is opt-in through the selector; UEFI Desktop stays default': 'str_eq(p,"BOOTKERNEL")' in loader and 'wait_for_boot_choice()' in loader and 'Auto-starting UEFI Desktop' in loader and 'normal boot and recovery environment' in loader,
    'loader validates ELF, rejects W+X/overlap and allocates by permissions': all(x in loader for x in ['ELF_PF_X', 'ELF_PF_W', 'overlapping ELF load segments', 'EFI_MEMORY_LOADER_DATA', 'permission-separated ELF segments', 'eh->entry<raw_end']),
    'loader exits boot services before handoff': all(x in loader for x in ['ELF_ET_EXEC', 'ELF_EM_X86_64', 'ELF_PT_LOAD', 'GetMemoryMap', 'ExitBootServices', 'enter_native_kernel']),
}
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
if not all(checks.values()):
    raise SystemExit(1)
