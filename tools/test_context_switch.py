#!/usr/bin/env python3
"""Execute the production x86-64 context-switch assembly in a host test process."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mint-context-test-') as tmp:
    obj = Path(tmp) / 'arch.o'
    exe = Path(tmp) / 'context-switch-test'
    subprocess.run(['clang', '-c', str(root / 'kernel/arch/x86_64/arch.S'), '-o', str(obj)], check=True)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-no-pie',
                    str(root / 'tools/test_context_switch.c'), str(obj), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
