#!/usr/bin/env python3
"""Compile and run the production address-space manager against a fake page backend."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mintos-address-space-test-') as td:
    exe = Path(td) / 'test_address_space'
    cc = os.environ.get('HOST_CC', 'clang')
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_address_space.c'),
                    str(root / 'kernel/mm/address_space.c'),
                    str(root / 'kernel/proc/elf_loader.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
