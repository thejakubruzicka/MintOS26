#!/usr/bin/env python3
"""Host-run tests for production user-memory copying and ring-3 syscall dispatch."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mint-user-syscall-test-') as td:
    exe = Path(td) / 'user-syscall-test'
    cc = os.environ.get('HOST_CC', 'clang')
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_user_syscalls.c'),
                    str(root / 'kernel/mm/address_space.c'),
                    str(root / 'kernel/abi/syscall.c'),
                    str(root / 'kernel/ui/desktop_event.c'),
                    str(root / 'kernel/ui/display_surface.c'),
                    str(root / 'kernel/ui/desktop_services.c'),
                    str(root / 'kernel/fs/ramfs.c'),
                    str(root / 'kernel/driver/catalog.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
