#!/usr/bin/env python3
"""Host-test the ACPI table parser and check the power syscall/UI wiring."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = os.environ.get('HOST_CC', 'clang')
with tempfile.TemporaryDirectory(prefix='mint-acpi-') as tmp:
    exe = Path(tmp) / 'acpi-test'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-DMINT_ACPI_HOST_TEST',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_acpi.c'), str(root / 'kernel/power/acpi.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

acpi = (root / 'kernel/power/acpi.c').read_text()
abi = (root / 'kernel/abi/syscall.c').read_text()
kernel = (root / 'kernel/kernel.c').read_text()
desktop = (root / 'kernel/user/desktop.c').read_text()
uapi = (root / 'kernel/include/uapi/mintos/syscall.h').read_text()
checks = {
    'power syscalls are numbered in the MintOS-specific range': 'MINT_SYS_POWER_INFO                    0x4D25u' in uapi and 'MINT_SYS_POWER_CONTROL                 0x4D26u' in uapi,
    'power control requires the explicit confirmation token': 'arg1 != MINT_POWER_CONFIRM_TOKEN' in abi,
    'kernel parses ACPI from the UEFI-provided RSDP at boot': 'mint_power_init(boot->acpi_rsdp_physical)' in kernel,
    'shutdown enables ACPI mode with a bounded wait before S5': 'enable_acpi_mode' in acpi and '3000000u' in acpi,
    'VM power-off ports are only used under a hypervisor': 'if (power_info.flags & MINT_POWER_HYPERVISOR)' in acpi,
    'restart falls back from ACPI reset to 0xCF9, 8042 and triple fault': all(x in acpi for x in ['reset_via_acpi()', 'outb(0xCF9u, 0x06u)', 'outb(0x64u, 0xFEu)', 'lidt']),
    'MintDE asks for confirmation before shutdown/restart': 'power_confirm' in desktop and 'mint_power_request' in desktop,
}
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name)
if not all(checks.values()):
    raise SystemExit(1)
