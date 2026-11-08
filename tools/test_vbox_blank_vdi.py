#!/usr/bin/env python3
"""Verify a disposable blank VDI generated from the checked template."""
from __future__ import annotations
import struct, sys
from pathlib import Path
FREE=0xFFFFFFFF

def check(ok: bool, message: str) -> None:
    if not ok: raise AssertionError(message)
    print('PASS:',message)

def main() -> int:
    if len(sys.argv)!=3:
        print('usage: test_vbox_blank_vdi.py TEMPLATE BLANK_VDI',file=sys.stderr); return 2
    template,output=map(Path,sys.argv[1:])
    th=template.read_bytes(); data=output.read_bytes()
    check(len(data)>=512 and b'VirtualBox Disk Image' in data[:64],'blank VDI header is present')
    sig,ver,_,itype,_=struct.unpack_from('<IIIII',data,64)
    check(sig==0xBEDA107F and ver==0x10001 and itype==1,'output is a VirtualBox VDI 1.1 dynamic medium')
    mo,do=struct.unpack_from('<II',data,340); disk=struct.unpack_from('<Q',data,368)[0]
    bs=struct.unpack_from('<I',data,376)[0]; bc=struct.unpack_from('<I',data,384)[0]; allocated=struct.unpack_from('<I',data,388)[0]
    check((disk,bs,bc)==(512*1024*1024,1024*1024,512),'blank VDI has 512 MiB capacity and 512 blocks')
    mapping=data[mo:mo+bc*4]
    check(len(mapping)==bc*4 and all(v==FREE for (v,) in struct.iter_unpack('<I',mapping)),'all 512 map entries are unallocated')
    check(allocated==0 and len(data)==do,'blank VDI has no allocated payload blocks')
    check(data[392:408]!=th[392:408] and data[408:424]!=th[408:424],'blank output uses unique create/modify UUIDs')
    check(data[424:456]==bytes(32),'blank output has no parent/linked medium')
    print('INFO: expected guest LBA 0 is zero-filled until Disk Utility initializes the disk.')
    return 0
if __name__=='__main__': raise SystemExit(main())
