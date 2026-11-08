#!/usr/bin/env python3
"""Compile and run production virtual-arena allocation code with a mock paging backend."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mintos-vmalloc-test-') as td:
    exe = Path(td) / 'test_vmalloc'
    cc = os.environ.get('HOST_CC', 'clang')
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_vmalloc.c'),
                    str(root / 'kernel/mm/vmalloc.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
