#!/usr/bin/env python3
"""Generate assembly that embeds a standalone ELF image in kernel read-only data."""
from pathlib import Path
import sys

if len(sys.argv) not in (3, 4):
    raise SystemExit("usage: embed_elf.py input.elf output.S [symbol_prefix]")
in_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
prefix = sys.argv[3] if len(sys.argv) == 4 else "mint_user_elf"
if not prefix or not prefix.replace("_", "").isalnum():
    raise SystemExit("symbol_prefix must contain only letters, digits and underscores")
out_path.write_text(
    f''' .section .rodata.{prefix},"a",@progbits
.p2align 4
.global {prefix}_start
.global {prefix}_end
{prefix}_start:
.incbin "{in_path.as_posix()}"
{prefix}_end:
.section .note.GNU-stack,"",@progbits
'''.lstrip(),
    encoding="utf-8",
)
