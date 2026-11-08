#!/usr/bin/env python3
"""Compile and run read-only FAT16 checks against the actual generated disk image."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
image = Path(os.sys.argv[1]) if len(os.sys.argv) > 1 else root / 'build/mintos26-data-512m.img'
with tempfile.TemporaryDirectory(prefix='mint-fat16-ro-') as td:
    exe = Path(td) / 'fat16-ro-test'
    cc = os.environ.get('HOST_CC', 'clang')
    subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_fat16_ro.c'),
                    str(root / 'kernel/fs/fat16.c'),
                    str(root / 'kernel/block/block.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(image)], check=True)
