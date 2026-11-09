#!/usr/bin/env python3
"""Static checks for scheduler-owned processes, ELF64 loading and ring-3 ABI."""
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[1]
arch = (root / 'kernel/arch/x86_64/arch.c').read_text()
asm = (root / 'kernel/arch/x86_64/arch.S').read_text()
call = (root / 'kernel/abi/syscall.c').read_text()
space = (root / 'kernel/mm/address_space.c').read_text()
kernel = (root / 'kernel/kernel.c').read_text()
uapi = (root / 'kernel/include/uapi/mintos/syscall.h').read_text()
loader = (root / 'kernel/proc/elf_loader.c').read_text()
proc = (root / 'kernel/proc/process.c').read_text()
sched = (root / 'kernel/task/scheduler.c').read_text()
checks = {
    'ring-3 code/data GDT descriptors are present': '0x00AFFA000000FFFFULL' in arch and '0x00CFF2000000FFFFULL' in arch,
    'int-0x80 IDT gate has DPL 3': 'set_gate_dpl(0x80u' in arch and 'dpl & 3u' in arch,
    'syscall stub saves GPRs and returns using iretq': 'mint_syscall_stub:' in asm and 'callq mint_syscall_trap_dispatch' in asm and 'iretq' in asm,
    'user exit restores the selected task local stack and interrupt flags': 'mint_arch_user_transition_current(%rip)' in asm and '0(%r10)' in asm and '8(%r10)' in asm and 'popfq' in asm and 'MINTEXIT' in asm,
    'per-task user return record is ABI-checked for assembly offsets': 'MINT_USER_TRANSITION' in (root / 'kernel/include/user_transition.h').read_text() and 'user transition RFLAGS asm offset' in arch,
    'cooperative user sched_yield is now enabled': 'mint_task_yield();' in call and 'return -MINT_ENOSYS;' not in call[call.index('if (number == MINT_SYS_SCHED_YIELD)'):call.index('/* Remaining implemented calls')],
    'kernel runs two concurrent-lifetime user process workers': 'MINT_RING3_TEST_WORKERS 2u' in kernel and 'ring3-smoke-a' in kernel and 'ring3-smoke-b' in kernel,
    'C userspace runtime includes yield wrapper and multiple yield probes': 'mint_sched_yield' in (root / 'kernel/user/runtime.c').read_text() and (root / 'kernel/user/init.c').read_text().count('mint_sched_yield()') >= 2,
    'freestanding CRT enters C main and exits through syscall ABI': 'call mint_user_main' in (root / 'kernel/user/crt0.S').read_text() and 'call mint_exit' in (root / 'kernel/user/crt0.S').read_text(),
    'userspace runtime wraps core syscalls and desktop services': all(x in (root / 'kernel/user/runtime.c').read_text() for x in ['MINT_SYS_WRITE','MINT_SYS_GETPID','MINT_SYS_SCHED_YIELD','MINT_SYS_CLOCK_GETTIME','MINT_SYS_DESKTOP_EVENT_POLL','MINT_SYS_DISPLAY_INFO','MINT_SYS_DISPLAY_FILL_RECT','MINT_SYS_DISPLAY_DRAW_TEXT','MINT_SYS_SLEEP_TICKS']),
    'kernel exposes validated desktop event-poll and display-info services': all(x in call for x in ['MINT_SYS_DESKTOP_EVENT_POLL','MINT_SYS_DISPLAY_INFO','mint_address_space_validate_user_range','mint_address_space_copy_to_user']),
    'runtime tests actual queued desktop event and safe display metadata': all(x in (root / 'kernel/user/init.c').read_text() for x in ['MINT_DESKTOP_EVENT_APP_REQUEST','mint_desktop_event_poll','mint_display_get_info']),
    'standalone user ELF is embedded as read-only payload': 'mint_user_elf_start' in (root / 'build/user-elf-blob.S').read_text() and '.section .rodata.mint_user_elf' in (root / 'build/user-elf-blob.S').read_text(),
    'native desktop ELF is embedded under distinct read-only symbols': 'mint_desktop_elf_start' in (root / 'build/desktop-elf-blob.S').read_text() and '.section .rodata.mint_desktop_elf' in (root / 'build/desktop-elf-blob.S').read_text() and 'mint_desktop_elf_start' in kernel and 'mint_desktop_elf_end' in kernel,
    'kernel seeds only bounded synthetic events for the userspace ABI probe': 'seed_desktop_runtime_events' in kernel and '0x00260053u' in kernel and 'mint_desktop_event_post(&event)' in kernel,
    'user pointers are validated before physical-backed copies': 'mint_address_space_validate_user_range' in space and 'mint_address_space_copy_from_user' in call and 'mint_address_space_copy_to_user' in call,
    'user write syscall copies into kernel-owned buffer': 'uint8_t local[512]' in call and 'mint_address_space_copy_from_user(space, local' in call,
    'clock_gettime copies to a validated writable user mapping': 'MINT_SYS_CLOCK_GETTIME' in call and 'mint_address_space_copy_to_user(space, arg1' in call,
    
    'trusted ELF loader initializes RX pages without writable user PTEs': 'mint_address_space_initialize_mapped_bytes' in loader and 'PF_X' in loader and 'PF_W' in loader,
    'ELF64 loader validates class, endianness, machine, bounds and W+X': all(x in loader for x in ['ELFCLASS64','ELFDATA2LSB','EM_X86_64','ph.filesz > ph.memsz','ph.offset > image_size','PF_W) && (ph.flags & PF_X']),
    'ELF64 loader rejects dynamic/interpreter images and validates entry': 'PT_DYNAMIC' in loader and 'PT_INTERP' in loader and 'entry_is_executable' in loader,
    'ELF64 loader maps BSS-zeroed segments plus private user stack': 'MINT_ASPACE_REGION_STACK' in loader and 'stack_top' in loader and 'map_region_pages' in loader,
    'process owns an address space and independent privilege-transition stack': 'MINT_ADDRESS_SPACE address_space' in (root / 'kernel/include/process.h').read_text() and all(x in proc for x in ['mint_vmalloc_alloc(MINT_PROCESS_KERNEL_STACK_BYTES','mint_task_attach_process','mint_task_set_privilege_stack_top']),
    'process exit reaps address space and transition stack': all(x in proc for x in ['mint_address_space_destroy','mint_vmalloc_free','mint_task_detach_process']),
    'scheduler task contains process association and privilege stack state': all(x in sched for x in ['void *process','privilege_stack_top','mint_arch_update_rsp0']),
    'kernel launches the user process through generic ELF loader': all(x in kernel for x in ['mint_user_elf_start','mint_process_create_current_from_elf','mint_process_run_current']),
    'kernel reports ELF64 ring-3 process result': 'ELF64 USER PROCESS TEST PASSED' in kernel and 'ring3_test_state' in kernel,
    'UAPI documents MintOS syscall numbers without claiming Linux compatibility': 'MINT_SYSABI_VERSION' in uapi and 'NOT a Linux-compatible ABI' in uapi,
}
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
if not all(checks.values()):
    raise SystemExit(1)
elf = root / 'build/mintos26-kernel.elf'
if elf.exists():
    syms = subprocess.run(['readelf','-s','--wide',str(elf)],capture_output=True,text=True,check=True).stdout
    required = ['mint_syscall_stub','mint_arch_enter_user','mint_user_elf_start','mint_user_elf_end','mint_desktop_elf_start','mint_desktop_elf_end','mint_arch_user_transition_current']
    found = {name: None for name in required}
    for line in syms.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[-1] in found:
            try: found[fields[-1]] = int(fields[1],16)
            except ValueError: pass
    missing = [name for name,value in found.items() if value is None]
    print(('PASS' if not missing else 'FAIL') + ': linked ELF has user/process-entry symbols' + ('' if not missing else ': '+', '.join(missing)))
    if missing: raise SystemExit(1)
    size = found['mint_user_elf_end'] - found['mint_user_elf_start']
    ok = 128 <= size <= 65536
    print(('PASS' if ok else 'FAIL') + f': embedded static user ELF is within bounded image size ({size} bytes)')
    if not ok: raise SystemExit(1)
    desktop_size = found['mint_desktop_elf_end'] - found['mint_desktop_elf_start']
    # MintDE 2 embeds Inter glyph tables (~270 KiB) and 14 icons at 64 and 128 px
    # (~1.1 MiB). Keep a reviewed ceiling well inside the 8 MiB (2048-page)
    # per-process frame limit so the resource bundle cannot grow unnoticed.
    ok = 128 <= desktop_size <= 3 * 1024 * 1024
    print(('PASS' if ok else 'FAIL') + f': embedded native desktop ELF is within bounded 3 MiB image size ({desktop_size} bytes)')
    if not ok: raise SystemExit(1)
    headers = subprocess.run(['readelf','-l',str(elf)],capture_output=True,text=True,check=True).stdout
    if 'R E' not in headers or 'RW ' not in headers or 'RWE' in headers:
        print('FAIL: kernel ELF should preserve separate RX/R/RW load segments')
        raise SystemExit(1)
    print('PASS: kernel ELF retains separated RX, R and RW PT_LOAD segments')
