#!/usr/bin/env python3
"""Build and run the 26.5.014.0 app-platform host tests against production sources."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mintos-app-platform-test-') as td:
    exe = Path(td) / 'test_app_platform'
    cc = os.environ.get('HOST_CC', 'clang')
    subprocess.run([cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-DMINT_PROC_FILES_HOST_TEST', '-DMINT_FPU_HOST_TEST', '-DMINT_LINUX_COMPAT_HOST_TEST',
                    '-I', str(root / 'kernel/include'),
                    str(root / 'tools/test_app_platform.c'), str(root / 'kernel/mm/frame_pool.c'),
                    str(root / 'kernel/abi/linux_table.c'), str(root / 'kernel/proc/proc_files.c'),
                    str(root / 'kernel/arch/x86_64/fpu.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
