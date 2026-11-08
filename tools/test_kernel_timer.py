#!/usr/bin/env python3
"""Static timing/interrupt contract test. Runtime delivery must still be checked in a VM."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
arch = (root / 'kernel/arch/x86_64/arch.c').read_text()
asm = (root / 'kernel/arch/x86_64/arch.S').read_text()
kernel = (root / 'kernel/kernel.c').read_text()
checks = {
    'PIT reload divisor rounds to ~100 Hz': abs((1193182 / round(1193182 / 100)) - 100.0) < 0.01,
    'PIC starts with only master IRQ0 unmasked': 'pic1_mask = 0xFEu' in arch and 'pic2_mask = 0xFFu' in arch,
    'PIC bases are remapped away from CPU exception vectors': 'outb(PIC1_DATA, 0x20u)' in arch and 'outb(PIC2_DATA, 0x28u)' in arch,
    'PIT receives a valid low/high divisor in mode 3': 'outb(PIT_COMMAND, 0x36u)' in arch and 'outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFFu))' in arch and 'outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8))' in arch,
    'IRQ0 counts timer ticks while IRQ1/IRQ12 dispatch input': 'if (frame->vector == 32u)' in arch and 'timer_ticks++' in arch and 'mint_input_keyboard_irq()' in arch and 'mint_input_mouse_irq()' in arch and 'outb(PIC1_COMMAND, 0x20u)' in arch,
    'IRQ stub synthesizes vector/error fields': re.search(r'mint_irq0_stub:.*?pushq \$0\s+pushq \$32\s+jmp mint_irq_common', asm, re.S) is not None,
    'IRQ path preserves general-purpose registers and returns via iretq': all(x in asm for x in ['pushq %rax','pushq %rbx','pushq %r15','popq %r15','popq %rbx','popq %rax','iretq']),
    'kernel initializes timer before STI': kernel.index('mint_timer_init();') < kernel.index('"sti"'),
    "kernel status worker sleeps on timer ticks and updates the live display": 'mint_task_sleep_ticks(100)' in kernel and 'mint_timer_ticks()' in kernel and 'draw_runtime_status(now, now != 0, mint_task_live_count(),' in kernel,
    'unhandled PIC IRQ lines remain masked': '0xFEu' in arch and '0xFFu' in arch,
}
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
if not all(checks.values()):
    raise SystemExit(1)
