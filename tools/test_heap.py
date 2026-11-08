#!/usr/bin/env python3
"""Compile and run host-side behavior tests for the production kernel heap."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mintos-heap-test-') as td:
    exe = Path(td) / 'test_heap'
    cc = os.environ.get('HOST_CC', 'clang')
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_heap.c'),
                    str(root / 'kernel/mm/heap.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
