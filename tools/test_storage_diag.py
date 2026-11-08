#!/usr/bin/env python3
"""Compile production storage parsers for host-side synthetic/image regression tests."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parent.parent
compiler = os.environ.get("HOST_CC", "clang")
source_files = [
    root / "tools/test_storage_diag.c",
    root / "kernel/fs/storage_diag.c",
    root / "kernel/diag/ata_identify.c",
]
with tempfile.TemporaryDirectory(prefix="mintos26-storage-tests-") as tmp:
    exe = Path(tmp) / "test_storage_diag"
    cmd = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(root)]
    cmd += [str(p) for p in source_files]
    cmd += ["-o", str(exe)]
    subprocess.run(cmd, check=True)
    subprocess.run([str(exe), *sys.argv[1:]], check=True)
