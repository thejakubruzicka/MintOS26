#!/usr/bin/env python3
"""Static checks for cooperative task switching and the initial Unix-like syscall ABI."""
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[1]
k = (root / 'kernel/kernel.c').read_text()
s = (root / 'kernel/task/scheduler.c').read_text()
a = (root / 'kernel/arch/x86_64/arch.S').read_text()
h = (root / 'kernel/include/task.h').read_text()
u = (root / 'kernel/include/uapi/mintos/syscall.h').read_text()
sys = (root / 'kernel/abi/syscall.c').read_text()
make = (root / 'Makefile').read_text()
checks = {
    'context-switch assembly saves/restores SysV callee-saved registers': all(x in a for x in ['mint_context_switch:', 'pushq %rbp', 'pushq %rbx', 'pushq %r12', 'pushq %r13', 'pushq %r14', 'pushq %r15', 'popq %r15', 'popq %r14', 'popq %r13', 'popq %r12', 'popq %rbx', 'popq %rbp', 'retq']),
    'task bootstrap fabricates six-register, RFLAGS and return-address frame': all(x in s for x in ['top -= sizeof(uint64_t)', 'top - 8u * sizeof(uint64_t)', 'sp[6] = 0x202u', 'sp[7] = (uint64_t)(uintptr_t)task_bootstrap']),
    'bounded static task slots and dedicated stacks': 'MINT_TASK_MAX 12u' in h and 'MINT_TASK_STACK_SIZE (16u * 1024u)' in h and 'tasks[MINT_TASK_MAX]' in s,
    'round-robin dispatch rotates its candidate and switches contexts': all(x in s for x in ['next_candidate', 'MINT_TASK_READY', 'mint_context_switch(&scheduler_rsp', 'task->state = MINT_TASK_RUNNING']),
    'sleep blocks a task until the PIT deadline': all(x in s for x in ['MINT_TASK_SLEEPING', 'wake_tick', 'now >= tasks[i].wake_tick', 'mint_task_sleep_ticks']),
    'task return marks a slot dead and returns to scheduler': all(x in s for x in ['t->state = MINT_TASK_DEAD', 'mint_context_switch(&t->rsp, scheduler_rsp)']),
    'dead slots can be reused instead of limiting lifetime task creation': all(x in s for x in ['tasks[j].state == MINT_TASK_UNUSED || tasks[j].state == MINT_TASK_DEAD', 'MINT_TASK_MAX']),
    'task lifecycle metrics distinguish live, created and exited tasks': all(x in h for x in ['mint_task_total_created', 'mint_task_total_exited']) and all(x in s for x in ['total_created++', 'total_exited++']) and 'CREATED: ' in k and 'EXITED: ' in k,
    'idle scheduler halts with interrupts enabled when no work is ready': '__asm__ volatile("sti; hlt"' in s,
    'timer IRQ only updates tick count and never context-switches': 'timer_ticks++' in (root / 'kernel/arch/x86_64/arch.c').read_text() and 'mint_context_switch' not in (root / 'kernel/arch/x86_64/arch.c').read_text(),
    'kernel starts status, heartbeat, ABI smoke and two ring-3 process workers': all(x in k for x in ['mint_task_create("desktop-status"', 'mint_task_create("heartbeat"', 'mint_task_create("abi-smoke"', 'mint_task_create("ring3-smoke-a"', 'mint_task_create("ring3-smoke-b"', 'mint_scheduler_run();']),
    'context switching preserves RFLAGS across user/kernel IF states': 'pushfq' in a and 'popfq' in a and 'RFLAGS' in a,
    'scheduler activates kernel root before dispatch and each task process root before resume': all(x in s for x in ['mint_address_space_activate_kernel()', 'mint_address_space_activate(&process->address_space)', 'mint_arch_set_user_transition(&task->user_transition)']),
    'scheduler stores per-task user transition and exit state': all(x in s for x in ['MINT_USER_TRANSITION user_transition', 'mint_task_user_exit_record', 'mint_task_user_exit_seen']),
    'Linux-inspired syscall-number subset is explicitly versioned': 'MINT_SYSABI_VERSION 1u' in u and all(x in u for x in ['MINT_SYS_WRITE         1u', 'MINT_SYS_SCHED_YIELD  24u', 'MINT_SYS_GETPID       39u', 'MINT_SYS_CLOCK_GETTIME 228u']),
    'syscall dispatcher handles write, yield, getpid, monotonic time and exit': all(x in sys for x in ['case MINT_SYS_WRITE:', 'case MINT_SYS_SCHED_YIELD:', 'case MINT_SYS_GETPID:', 'case MINT_SYS_CLOCK_GETTIME:', 'case MINT_SYS_EXIT:', 'return -MINT_ENOSYS']),
    'kernel flags preserve freestanding no-red-zone/no-SIMD assumptions': all(x in make for x in ['-ffreestanding', '-mno-red-zone', '-mno-sse', '-mno-mmx', '-mgeneral-regs-only']),
    'kernel exposes a generic ELF64 process and ring-3 syscall path': all(x in a for x in ['mint_syscall_stub:', 'mint_arch_enter_user:']) and all(x in sys for x in ['mint_syscall_dispatch_user', 'mint_address_space_copy_from_user', 'mint_address_space_copy_to_user']) and all(x in (root / 'kernel/proc/process.c').read_text() for x in ['mint_process_create_current_from_elf', 'mint_process_run_current']) and all(x in make for x in ['user-init.elf', 'elf-loader.obj', 'process.obj']),
}
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
if not all(checks.values()):
    raise SystemExit(1)
# Verify the assembled context switch has the expected instructions.
elf = root / 'build/mintos26-kernel.elf'
out = subprocess.run(['objdump', '-d', str(elf)], check=True, capture_output=True, text=True).stdout
pos = out.find('<mint_context_switch>:')
checks2 = {
    'context switch symbol is present in linked kernel': pos >= 0,
    'linked context switch includes stack store/load and ret': pos >= 0 and all(x in out[pos:pos+1000] for x in ['(%rdi)', '%rsp', 'ret']),
}
for name, ok in checks2.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
if not all(checks2.values()): raise SystemExit(1)
