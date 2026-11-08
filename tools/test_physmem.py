#!/usr/bin/env python3
"""Compile and run a host-side behavior test for the freestanding page-range code."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mintos-physmem-test-') as td:
    exe = Path(td) / 'test_physmem'
    compiler = os.environ.get('HOST_CC', 'clang')
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_physmem.c'),
                    str(root / 'kernel/mm/physmem.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
