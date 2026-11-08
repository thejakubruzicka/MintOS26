#!/usr/bin/env python3
"""Host tests for safe PCI configuration-function decoding."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="mint-pci-test-") as td:
    exe = Path(td) / "pci-test"
    cc = os.environ.get("HOST_CC", "clang")
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(root / "tools/test_pci.c"), str(root / "kernel/bus/pci.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
