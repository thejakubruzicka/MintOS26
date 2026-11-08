#!/usr/bin/env python3
"""Wrap a FAT16 ESP volume in a small GPT disk image for UEFI hard-disk boot."""
from pathlib import Path
import struct,sys,zlib,uuid
SECTOR=512
ESP_GUID=uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b')
def main():
    if len(sys.argv)!=3: raise SystemExit('usage: mkuefi_disk.py esp-volume.img output.img')
    esp=Path(sys.argv[1]).read_bytes();out=Path(sys.argv[2]);total=262144
    if len(esp)%SECTOR: raise SystemExit('ESP must be sector aligned')
    start=2048;length=len(esp)//SECTOR;last=total-1
    if start+length>last-33: raise SystemExit('ESP is too large for target disk')
    disk=bytearray(total*SECTOR)
    # Protective MBR.
    mbr=bytearray(SECTOR);mbr[446]=0;mbr[447:450]=b'\0\2\0';mbr[450]=0xEE;mbr[451:454]=b'\xff\xff\xff';struct.pack_into('<II',mbr,454,1,min(total-1,0xffffffff));mbr[510:512]=b'\x55\xaa';disk[:SECTOR]=mbr
    dg=uuid.UUID('6b3e4b6f-2d26-4d3c-9f26-000000000262').bytes_le
    pg=uuid.UUID('26320001-2600-4d26-8a26-000000000001').bytes_le
    entries=bytearray(128*128);e=entries[:128];e[0:16]=ESP_GUID.bytes_le;e[16:32]=pg;struct.pack_into('<QQQ',e,32,start,start+length-1,0);e[56:56+24]='MintOS26 EFI'.encode('utf-16le')
    entries[:128]=e;entry_crc=zlib.crc32(entries)&0xffffffff;entry_sectors=len(entries)//SECTOR;backup_entries=total-1-entry_sectors
    disk[2*SECTOR:2*SECTOR+len(entries)]=entries;disk[backup_entries*SECTOR:backup_entries*SECTOR+len(entries)]=entries
    def header(current,backup,entries_lba):
        h=bytearray(SECTOR);h[:8]=b'EFI PART';struct.pack_into('<IIIIQQQQ',h,8,0x00010000,92,0,0,current,backup,34,total-34);h[56:72]=dg;struct.pack_into('<QIII',h,72,entries_lba,128,128,entry_crc);struct.pack_into('<I',h,16,zlib.crc32(h[:92])&0xffffffff);return h
    disk[SECTOR:2*SECTOR]=header(1,last,2);disk[last*SECTOR:(last+1)*SECTOR]=header(last,1,backup_entries)
    off=start*SECTOR;disk[off:off+len(esp)]=esp
    out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(disk);print(f'Built {out}: {len(disk):,} bytes, GPT ESP LBA {start}-{start+length-1}')
if __name__=='__main__':main()
