#!/usr/bin/env python3
"""Generate an assembler include for the per-app RGBA icons (PNG masters ship separately)."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]) if len(sys.argv)>1 else root/'build/icon-assets.S'
icons=['files','launchpad','terminal','settings','system-monitor','disk-utility','database','storage','cpu','mintde','windows','notes']
lines=['.section .rodata','']
for name in icons:
    lines += ['.balign 16',f'.globl mint_icon_{name.replace("-","_")}_rgba',f'mint_icon_{name.replace("-","_")}_rgba:',f'.incbin "assets/icons/{name}.rgba"','']
lines += ['.section .note.GNU-stack,"",@progbits','']
out.parent.mkdir(parents=True,exist_ok=True)
out.write_text('\n'.join(lines))
print(f'generated {out} with {len(icons)} icon payloads')
