#!/usr/bin/env python3
"""Host-side validation of the VirtualBox-template seeding path."""
from __future__ import annotations
import hashlib, struct, sys, zlib
from pathlib import Path

SECTOR = 512
FREE = 0xFFFFFFFF

def check(condition: bool, message: str) -> None:
    if not condition: raise AssertionError(message)
    print('PASS:', message)

def sha256_range(path: Path, start: int, length: int) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        f.seek(start); remain=length
        while remain:
            chunk=f.read(min(remain,1024*1024))
            if not chunk: raise ValueError('short payload')
            h.update(chunk); remain-=len(chunk)
    return h.hexdigest()

def main() -> int:
    if len(sys.argv)!=4:
        print('usage: test_vbox_vdi.py TEMPLATE RAW SEEDED_VDI',file=sys.stderr); return 2
    template,raw,vdi=map(Path,sys.argv[1:])
    check(template.exists() and raw.exists() and vdi.exists(),'template, raw image and seeded VDI exist')
    with template.open('rb') as f: th=f.read(512); f.seek(1024*1024); tm=f.read(512*4)
    ts=struct.unpack_from('<IIIII',th,64)
    meta=struct.unpack_from('<IIIIIIIQIIII',th,340)
    check(ts[0]==0xBEDA107F and ts[1]==0x10001 and ts[3]==1,'uploaded template is VDI 1.1 dynamic format')
    check(meta[0]==1024*1024 and meta[1]==2*1024*1024 and meta[7]==512*1024*1024 and meta[8]==1024*1024 and meta[10]==512,'template has expected 512 MiB / 1 MiB block geometry')
    check(all(v==FREE for (v,) in struct.iter_unpack('<I',tm)),'uploaded VirtualBox VDI is blank: all block-map entries are unallocated')
    with vdi.open('rb') as f:
        head=f.read(512); f.seek(meta[0]); mapping=f.read(meta[10]*4)
        values=struct.unpack('<512I',mapping)
        f.seek(meta[1]); sector=f.read(SECTOR)
        f.seek(meta[1]+510); mbrsig=f.read(2)
        f.seek(meta[1]+440); diskid=f.read(4)
    check(struct.unpack_from('<IIIII',head,64)[3]==1,'seeded output remains a dynamic VDI (not a raw file renamed .vdi)')
    check(values==tuple(range(512)),'all 512 dynamic block-map entries resolve to allocated payload blocks')
    check(vdi.stat().st_size==meta[1]+512*1024*1024,'VDI includes every 1 MiB payload block')
    check(mbrsig==b'\x55\xaa','seeded VDI LBA 0 ends with the MBR 0x55AA signature')
    check(diskid==struct.pack('<I',0x26300421),'seeded VDI has MintOS26 developer disk ID 0x26300421')
    check(sector != bytes(SECTOR) and zlib.crc32(sector)!=0xB2AA7578,'seeded VDI sector zero is not the all-zero sector from the blank VDI')
    check(sha256_range(raw,0,raw.stat().st_size)==sha256_range(vdi,meta[1],raw.stat().st_size),'VDI payload SHA-256 matches raw data image exactly')
    check(head[392:408] != th[392:408],'seeded VDI receives a distinct creation UUID')
    print('INFO: host format/content tests only; live AHCI runtime validation is still required.')
    return 0
if __name__=='__main__': raise SystemExit(main())
