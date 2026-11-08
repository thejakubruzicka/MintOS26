#!/usr/bin/env python3
"""Validate the separately linked user init ELF image and static-loader policy."""
from pathlib import Path
import struct, subprocess, sys
if len(sys.argv) != 2:
    raise SystemExit('usage: verify_elf_loader.py USERINIT.ELF')
p = Path(sys.argv[1])
b = p.read_bytes()
assert len(b) >= 64 and b[:4] == b'\x7fELF', 'user image is not ELF'
assert b[4] == 2 and b[5] == 1 and b[6] == 1, 'user image must be ELF64 little-endian v1'
(e_type, machine, version) = struct.unpack_from('<HHI', b, 16)
entry, phoff = struct.unpack_from('<QQ', b, 24)
ehsize, phentsize, phnum = struct.unpack_from('<HHH', b, 52)
assert e_type == 2 and machine == 62 and version == 1, 'user image must be static x86-64 ET_EXEC'
assert ehsize == 64 and phentsize == 56 and 1 <= phnum <= 16
assert phoff + phnum * phentsize <= len(b), 'program header table outside file'
loads=[]
for i in range(phnum):
    off=phoff+i*phentsize
    typ, flags=struct.unpack_from('<II',b,off)
    poff,va,pa,filesz,memsz,align=struct.unpack_from('<QQQQQQ',b,off+8)
    if typ != 1: continue
    assert memsz and filesz <= memsz and poff + filesz <= len(b)
    assert not (flags & 2 and flags & 1), 'W+X segment rejected'
    assert flags & 4, 'all loadable segments must be readable'
    assert (va & 0xfff) == (poff & 0xfff), 'ELF segment offset/vaddr page congruence'
    assert align in (0,1) or (align & (align-1)) == 0
    loads.append((va,va+memsz,flags,poff,filesz,memsz))
assert len(loads) >= 2, 'expect separate RX and RW segments'
assert any((fl & 5)==5 and not (fl&2) and start <= entry < end for start,end,fl,*_ in loads), 'entry must be in RX segment'
assert any((fl & 6)==6 and not (fl&1) for _,_,fl,*_ in loads), 'expect RW+NX data segment'
for i,a in enumerate(loads):
    abase=a[0]&~0xfff; aend=(a[1]+0xfff)&~0xfff
    for c in loads[i+1:]:
        cbase=c[0]&~0xfff; cend=(c[1]+0xfff)&~0xfff
        assert not (abase < cend and cbase < aend), 'load segments must not share virtual pages'
assert b'[user-runtime]' in b and b'mint_user_main' not in b, 'linked init image should contain runtime test messages but be stripped of symbol names'
print(f'PASS: static ELF64 x86-64 user image ({len(b)} bytes), entry=0x{entry:x}, {len(loads)} load segments')
print('PASS: separate RX text and RW data/BSS, no W+X, bounded static image suitable for the kernel loader')
# The generated program is intentionally tiny and uses only the tested int-0x80 ABI.
source=(p.parent.parent/'kernel/user/init.c').read_text()
runtime=(p.parent.parent/'kernel/user/runtime.c').read_text()
for token in ['mint_getpid', 'mint_sched_yield', 'mint_clock_gettime', 'mint_desktop_event_poll', 'mint_display_get_info']:
    if token not in source:
        raise SystemExit(f'FAIL: user-init source missing expected feature: {token}')
if 'int $0x80' not in runtime or 'MINT_SYS_EXIT' not in runtime:
    raise SystemExit('FAIL: freestanding runtime should implement int-0x80 calls and exit')
print('PASS: C user-init program exercises the syscall runtime and desktop service boundary')
