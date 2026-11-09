#!/usr/bin/env python3
"""Regression test: terminal storage/device commands must not overflow the 16 KiB user stack."""
from __future__ import annotations
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "kernel/user/mintde/apps.c"  # MintDE 2: Terminal lives in the apps module
FLAGS = [
    "-target", "x86_64-unknown-none-elf", "-ffreestanding", "-fno-stack-protector",
    "-fno-pic", "-fno-pie", "-fno-builtin", "-mno-red-zone", "-mno-sse", "-mno-mmx",
    "-mgeneral-regs-only", "-mcmodel=large", "-Wall", "-Wextra", "-Werror", "-O2",
    "-Ikernel/include", "-Ikernel/user/include", "-fstack-usage", "-c",
]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="mint-stack-usage-") as td:
        obj = pathlib.Path(td) / "desktop.obj"
        subprocess.run(["clang", *FLAGS, str(SOURCE), "-o", str(obj)], cwd=ROOT, check=True)
        usage_file = obj.with_suffix(".su")
        if not usage_file.exists():
            raise SystemExit(f"FAIL: clang did not emit stack usage report: {usage_file}")
        rows = {}
        for line in usage_file.read_text(encoding="utf-8").splitlines():
            match = re.search(r":([0-9]+):([^\t]+)\t([0-9]+)\t", line)
            if match:
                rows[match.group(2)] = int(match.group(3))
        expected = {
            "terminal_select_named_volume": 1024,
            "terminal_run_command": 2048,
        }
        for name, limit in expected.items():
            if name not in rows:
                raise SystemExit(f"FAIL: missing stack-usage entry for {name}")
            used = rows[name]
            if used > limit:
                raise SystemExit(f"FAIL: {name} stack frame {used} bytes exceeds {limit}-byte budget")
            print(f"PASS: {name} compiled stack frame is {used} bytes (limit {limit})")
        if rows["terminal_select_named_volume"] >= 8192:
            raise SystemExit("FAIL: terminal volume selection regressed toward the original page-faulting frame")
        print("PASS: terminal command stack-usage regression checks complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
