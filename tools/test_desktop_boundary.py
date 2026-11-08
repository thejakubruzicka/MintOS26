#!/usr/bin/env python3
"""Host-run tests for the kernel/desktop event and display boundary."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mintos-desktop-boundary-') as td:
    exe = Path(td) / 'test_desktop_boundary'
    cc = os.environ.get('HOST_CC', 'clang')
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Ikernel/include', 'tools/test_desktop_boundary.c',
                    'kernel/ui/desktop_event.c', 'kernel/ui/display_surface.c',
                    '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe)], check=True)
