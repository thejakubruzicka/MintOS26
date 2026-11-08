#!/usr/bin/env python3
"""Build and execute the syscall dispatcher under the host ABI with mocked kernel services."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mint-syscall-test-') as tmp:
    exe = Path(tmp) / 'syscall-dispatch-test'
    subprocess.run([
        'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
        str(root / 'tools/test_syscall_dispatch.c'),
        str(root / 'kernel/abi/syscall.c'),
        str(root / 'kernel/ui/desktop_event.c'),
        str(root / 'kernel/ui/display_surface.c'),
        str(root / 'kernel/ui/desktop_services.c'),
        str(root / 'kernel/fs/ramfs.c'),
        '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
