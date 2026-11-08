#!/usr/bin/env python3
"""Compile and run host-side tests for the production paging-table builder."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mintos-paging-test-') as td:
    exe = Path(td) / 'test_paging'
    cc = os.environ.get('HOST_CC', 'clang')
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_paging.c'),
                    str(root / 'kernel/mm/paging.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
