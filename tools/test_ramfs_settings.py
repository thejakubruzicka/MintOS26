#!/usr/bin/env python3
"""Host tests for versioned desktop settings and session RAM filesystem."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mintos-settings-ramfs-') as td:
    exe = Path(td) / 'ramfs-settings-test'
    cc = os.environ.get('HOST_CC', 'clang')
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_ramfs_settings.c'),
                    str(root / 'kernel/ui/desktop_services.c'),
                    str(root / 'kernel/fs/ramfs.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
