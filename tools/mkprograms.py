#!/usr/bin/env python3
"""Generate assembly that bundles programs/examples into the kernel image.

usage: mkprograms.py out.S /dest/path=host/file ...
Each entry becomes a MINT_PROGRAM_FILE {path, data, size}; the bundle id is a
SHA-256 prefix over all destinations and contents (programs.c compares it with
/System/Programs/.bundle-id to decide whether to reinstall)."""
from pathlib import Path
import hashlib, sys

if len(sys.argv) < 2:
    raise SystemExit(__doc__)
out = Path(sys.argv[1])
entries = []
for arg in sys.argv[2:]:
    dest, _, src = arg.partition('=')
    if not dest.startswith('/') or not src or '"' in dest or '\\' in dest:
        raise SystemExit(f'bad entry: {arg}')
    entries.append((dest, Path(src)))
digest = hashlib.sha256()
for dest, src in entries:
    data = src.read_bytes()
    digest.update(dest.encode() + b'\0' + len(data).to_bytes(8, 'little') + data)
bundle_id = digest.hexdigest()[:16]
lines = ['.section .rodata.mint_programs,"a",@progbits', '.p2align 4',
         '.global mint_program_bundle_count', 'mint_program_bundle_count:', f'    .quad {len(entries)}',
         '.global mint_program_bundle_id', 'mint_program_bundle_id:', f'    .asciz "{bundle_id}"',
         '.p2align 3', '.global mint_program_bundle', 'mint_program_bundle:']
for i, (dest, src) in enumerate(entries):
    lines.append(f'    .quad .Lpath{i}, .Ldata{i}, .Ldata{i}_end - .Ldata{i}')
for i, (dest, src) in enumerate(entries):
    lines.append(f'.Lpath{i}: .asciz "{dest}"')
for i, (dest, src) in enumerate(entries):
    lines += ['.p2align 4', f'.Ldata{i}:', f'    .incbin "{src.resolve().as_posix()}"', f'.Ldata{i}_end:']
lines.append('.section .note.GNU-stack,"",@progbits')
out.write_text('\n'.join(lines) + '\n', encoding='utf-8')
