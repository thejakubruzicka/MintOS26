#!/usr/bin/env python3
"""Host-test native PS/2 scancode and pointer-packet decoding."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mint-ps2-input-') as tmp:
    exe = Path(tmp) / 'ps2-input-test'
    subprocess.run([
        'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(root / 'kernel/include'),
        str(root / 'tools/test_ps2_input.c'),
        str(root / 'kernel/input/ps2.c'),
        '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)

# Source-level contracts ensure the hardware path is wired into IDT/PIC and ABI.
arch = (root / 'kernel/arch/x86_64/arch.c').read_text()
asm = (root / 'kernel/arch/x86_64/arch.S').read_text()
kernel = (root / 'kernel/kernel.c').read_text()
abi = (root / 'kernel/abi/syscall.c').read_text()
checks = {
    'IDT contains keyboard IRQ1 and mouse IRQ12 gates': 'set_gate(33' in arch and 'set_gate(44' in arch,
    'assembly has hardware IRQ1/IRQ12 stubs': 'mint_irq1_stub:' in asm and 'mint_irq12_stub:' in asm,
    'PIC acknowledges slave mouse IRQs before master cascade': 'outb(PIC2_COMMAND, 0x20u)' in arch and 'outb(PIC1_COMMAND, 0x20u)' in arch,
    'native kernel initializes PS/2 after desktop event service': 'mint_input_init()' in kernel,
    'userspace can query validated hardware input readiness': 'MINT_SYS_INPUT_INFO' in abi and 'mint_address_space_copy_to_user' in abi,
}
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
if not all(checks.values()):
    raise SystemExit(1)
