#!/usr/bin/env python3
"""Generate a minimal ISO9660 image with a UEFI El Torito no-emulation FAT boot image."""
from pathlib import Path
import struct,sys
S=2048

def both16(v): return struct.pack('<H',v)+struct.pack('>H',v)
def both32(v): return struct.pack('<I',v)+struct.pack('>I',v)
def rec(extent,size,flags,name):
    n=name if isinstance(name,bytes) else name.encode('ascii');ln=33+len(n)+(0 if len(n)%2 else 1);d=bytearray(ln);d[0]=ln;d[1]=0;d[2:10]=both32(extent);d[10:18]=both32(size);d[18:25]=bytes([126,10,10,0,0,0,0]);d[25]=flags;d[26]=d[27]=0;d[28:32]=both16(1);d[32]=len(n);d[33:33+len(n)]=n;return bytes(d)
def main():
    if len(sys.argv)!=4: raise SystemExit('usage: mkiso_uefi.py boot-fat.img readme.txt output.iso')
    boot=Path(sys.argv[1]).read_bytes();readme=Path(sys.argv[2]).read_bytes();out=Path(sys.argv[3])
    if len(boot)%S: raise SystemExit('FAT boot image size must be a multiple of 2048')
    cat_lba=20;img_lba=23;img_secs=len(boot)//S;root_lba=img_lba+img_secs;pt_lba=root_lba+1;ptbe_lba=pt_lba+1;readme_lba=ptbe_lba+1;readme_secs=(len(readme)+S-1)//S;total=readme_lba+readme_secs
    iso=bytearray(total*S)
    p=memoryview(iso)[16*S:17*S];p[0]=1;p[1:6]=b'CD001';p[6]=1;p[8:40]=b'MINTOS26'.ljust(32,b' ');p[40:72]=b'MintOS26 26.5.011.0 UEFI'.ljust(32,b' ');p[80:88]=both32(total);p[120:124]=both16(1);p[124:128]=both16(1);p[128:132]=both16(S);p[132:140]=both32(10);p[140:144]=struct.pack('<I',pt_lba);p[144:148]=struct.pack('>I',ptbe_lba);p[156:190]=rec(root_lba,S,2,b'\x00');p[813:830]=b'MINTOS26 UEFI'.ljust(17,b' ')
    b=memoryview(iso)[17*S:18*S];b[0]=0;b[1:6]=b'CD001';b[6]=1;b[7:39]=b'EL TORITO SPECIFICATION'.ljust(32,b' ');b[71:75]=struct.pack('<I',cat_lba)
    t=memoryview(iso)[18*S:19*S];t[0]=255;t[1:6]=b'CD001';t[6]=1
    root=bytearray(S);ents=[rec(root_lba,S,2,b'\x00'),rec(root_lba,S,2,b'\x01'),rec(img_lba,len(boot),0,b'EFI.IMG;1'),rec(readme_lba,len(readme),0,b'README.TXT;1')];off=0
    for e in ents:root[off:off+len(e)]=e;off+=len(e)
    iso[root_lba*S:(root_lba+1)*S]=root
    cat=bytearray(S);cat[0]=1;cat[1]=0xEF;cat[4:28]=b'MINTOS26 UEFI BOOT'.ljust(24,b' ');cat[30:32]=b'\x55\xaa';cat[28:30]=struct.pack('<H',(-sum(struct.unpack('<16H',cat[:32])))&0xffff);cat[32]=0x88;cat[33]=0;cat[34:36]=b'\0\0';cat[36]=0;cat[38:40]=b'\0\0';cat[40:44]=struct.pack('<I',img_lba);iso[cat_lba*S:(cat_lba+1)*S]=cat
    iso[pt_lba*S:pt_lba*S+10]=bytes([1,0])+struct.pack('<I',root_lba)+struct.pack('<H',1)+b'\0\0';iso[ptbe_lba*S:ptbe_lba*S+10]=bytes([1,0])+struct.pack('>I',root_lba)+struct.pack('>H',1)+b'\0\0';iso[img_lba*S:img_lba*S+len(boot)]=boot;iso[readme_lba*S:readme_lba*S+len(readme)]=readme
    out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(iso);print(f'Built {out}: {len(iso):,} bytes; UEFI no-emulation image LBA {img_lba}, sectors {img_secs}')
if __name__=='__main__':main()
