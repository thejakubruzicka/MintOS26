#!/usr/bin/env python3
"""Generate an assembler include with MintDE's icons (see tools/make_icons.py).

Each icon is embedded as raw RGBA at 128 px and 64 px:
  mint_icon_<name>_128 / mint_icon_<name>_64   (name with '-' -> '_')
plus mint_icon_mint_leaf_a8 (32x32 alpha mask for the menu-bar logo).
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'build/icon-assets.S'
ICONS = ['files', 'notes', 'terminal', 'settings', 'system-monitor', 'console', 'system-info',
         'network', 'disk-utility', 'about-mintos', 'mintde', 'launchpad', 'calculator', 'trash']
lines = ['.section .rodata', '']
for name in ICONS:
    sym = name.replace('-', '_')
    for px in (128, 64):
        path = root / f'assets/icons/{name}-{px}.rgba'
        if path.stat().st_size != px * px * 4:
            raise SystemExit(f'{path} is not {px}x{px} RGBA')
        lines += ['.balign 16', f'.globl mint_icon_{sym}_{px}', f'mint_icon_{sym}_{px}:',
                  f'.incbin "assets/icons/{name}-{px}.rgba"', '']
leaf = root / 'assets/icons/mint-leaf-32.a8'
if leaf.stat().st_size != 32 * 32:
    raise SystemExit('mint-leaf-32.a8 is not 32x32 A8')
lines += ['.balign 16', '.globl mint_icon_mint_leaf_a8', 'mint_icon_mint_leaf_a8:',
          '.incbin "assets/icons/mint-leaf-32.a8"', '']
lines += ['.section .note.GNU-stack,"",@progbits', '']
out.write_text('\n'.join(lines))
