#!/usr/bin/env python3
"""Static contracts for the 26.5.014.0 app platform (FPU state, preemption,
processes, Linux-compat subset, SDK, bundled programs, docs and evidence)."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
r = lambda p: (root / p).read_text(errors='replace')
mk, sched, arch, fpu = r('Makefile'), r('kernel/task/scheduler.c'), r('kernel/arch/x86_64/arch.c'), r('kernel/arch/x86_64/fpu.c')
sysc, entry, uapi = r('kernel/abi/syscall.c'), r('kernel/arch/x86_64/linux_entry.S'), r('kernel/include/uapi/mintos/process.h')
app_mk, sdk_mk, loader, mem = r('mk/app-platform.mk'), r('sdk/sdk.mk'), r('kernel/proc/elf_loader.c'), r('kernel/proc/proc_memory.c')
aspace_h, programs, compat = r('kernel/include/address_space.h'), r('kernel/proc/programs.c'), r('kernel/abi/linux_compat.c')
numbers = [int(x, 16) for x in re.findall(r'#define MINT_SYS_\w+\s+0x([0-9A-Fa-f]+)u', uapi)]
checks = {
    'kernel C stays integer-only (-mgeneral-regs-only) including fpu.c':
        '-mgeneral-regs-only' in mk.split('KERNEL_FLAGS :=')[1].split('\n')[0] and '$(KERNEL_FLAGS)' in app_mk.split('fpu.obj:')[1].split('\n')[1],
    'MintDE stays integer-only; only the FPU self-test program uses SSE':
        '-mgeneral-regs-only' in mk.split('USER_C_FLAGS :=')[1].split('\n')[0] and '-mgeneral-regs-only' not in app_mk.split('FPUTEST_C_FLAGS :=')[1].split('$(BUILD)')[0],
    'CR4.OSFXSR/OSXMMEXCPT, OSXSAVE and XCR0 are programmed':
        '(1ULL << 9) | (1ULL << 10)' in fpu and '(1ULL << 18)' in fpu and 'xsetbv' in fpu,
    'scheduler saves user FPU state after every switch-out and restores before switch-in':
        'if (task->fpu_user) mint_fpu_save(&task->fpu);' in sched and 'mint_fpu_restore(&task->fpu);' in sched and 'write_fs_base(task->fs_base)' in sched,
    'new user tasks start from a clean FPU state':
        'mint_fpu_state_make_clean(&t->fpu)' in sched and 'mint_task_user_state_begin();' in r('kernel/proc/process.c'),
    'timer preemption only interrupts ring-3 code (kernel stays non-preemptive)':
        '(frame->cs & 3u) == 3u' in arch.split('mint_irq_dispatch')[1].split('else if')[0] and 'mint_task_timer_tick_from_user' in arch,
    'killed processes leave through the common exit path on every return to ring 3':
        'mint_process_current_killed()' in sysc and 'mint_process_trap_return_check();' in arch and 'MINT_EXIT_KILLED_BIT' in compat,
    'app-platform syscalls stay inside the reserved 0x4D60-0x4D7F range':
        numbers and all(0x4D60 <= n <= 0x4D7F for n in numbers) and 'MINT_SYS_APP_PLATFORM_FIRST && number <= MINT_SYS_APP_PLATFORM_LAST' in sysc,
    'syscall entry switches to RSP0 before pushing and returns with IRETQ (no SYSRET)':
        entry.index('movq mint_arch_rsp0_shadow(%rip), %rsp') < entry.index('pushq $0x33') and 'iretq' in entry and 'sysretq' not in entry,
    'SFMASK clears IF/TF/DF on syscall entry': '(1u << 9) | (1u << 8) | (1u << 10)' in compat,
    'Linux layer documents itself as a subset, not Linux compatibility':
        'NOT Linux' in r('kernel/include/linux_compat.h') and 'not a Linux kernel' in compat,
    'loader rejects interpreters and W+X segments; stacks get a guard region':
        'PT_INTERP) return fail_and_destroy' in loader and '(ph.flags & PF_W) && (ph.flags & PF_X)' in loader and 'MINT_ASPACE_REGION_GUARD' in loader,
    'mmap refuses writable+executable memory (W^X)': '(prot & MINT_MEM_WRITE) && (prot & MINT_MEM_EXEC)' in mem,
    'default per-process limit is 16384 owned pages (64 MiB)': '#define MINT_ASPACE_MAX_USER_FRAMES 16384u' in aspace_h,
    '32 concurrent tasks and 24 process slots': 'MINT_TASK_MAX 32u' in r('kernel/include/task.h') and 'MINT_PROCESS_MAX 24u' in r('kernel/include/process.h'),
    'bundled programs are written only through the MintFS file API':
        'mint_mfs_open' in programs and 'mint_block' not in programs,
    'SDK builds static musl programs at the 64 GiB user base': '--image-base=0x1000000000' in sdk_mk and '-static' in sdk_mk and 'x86_64-unknown-linux-musl' in sdk_mk,
    'vendored sources carry licences': (root / 'vendor/musl/COPYRIGHT').exists() and (root / 'vendor/lua/LICENSE').exists() and (root / 'vendor/README.md').exists(),
    'SDK, window-server design and release notes are documented':
        all((root / p).exists() for p in ['docs/SDK.md', 'docs/WINDOW-SERVER.md', 'docs/notes-26.5.014.0-app-platform.md']),
    'QEMU evidence for Lua, two programs and Ctrl+C is recorded':
        all((root / f'test-evidence/qemu-26.5.014.0-app-platform-{n}.png').exists() for n in ['lua-repl', 'lua-script', 'two-programs', 'ctrlc-tools']),
}
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
if not all(checks.values()):
    raise SystemExit(1)
