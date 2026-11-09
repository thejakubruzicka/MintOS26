#!/usr/bin/env python3
"""Small deterministic FAT16 volume/disk builder for the MintOS26 UEFI media."""
from __future__ import annotations
import math, struct, sys
from pathlib import Path

SECTOR=512
EOC=0xFFFF

def short_name(name: str) -> bytes:
    name=name.upper()
    if name in ('.','..'):
        return name.encode('ascii').ljust(11,b' ')
    parts=name.split('.',1);base=parts[0];ext=parts[1] if len(parts)>1 else ''
    valid="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$~!#%&-{}()@'`"
    if len(base)>8 or len(ext)>3 or not base or any(c not in valid for c in base+ext):
        # Deterministic collision-resistant-enough alias for this curated image.
        alias=(base[:6]+'~1')[:8].ljust(8,' ')+(ext[:3].ljust(3,' '))
        return alias.encode('ascii')
    return (base.ljust(8,' ')+ext.ljust(3,' ')).encode('ascii')

def directory_entries(name,attr,cluster,size=0):
    """Emit VFAT long-name records when an on-disk name needs an 8.3 alias."""
    sfn=short_name(name)
    simple=name in ('.','..') or (len(name.split('.',1)[0])<=8 and len(name.split('.',1)[1] if '.' in name else '')<=3 and sfn[:len(name.split('.',1)[0])]==name.split('.',1)[0].upper().encode('ascii'))
    result=[]
    if not simple:
        raw=name.encode('utf-16le'); units=list(struct.unpack('<'+'H'*(len(raw)//2),raw)); chunks=[units[i:i+13] for i in range(0,len(units),13)]
        checksum=0
        for b in sfn: checksum=(((checksum&1)<<7)+(checksum>>1)+b)&0xff
        slots=[1,3,5,7,9,14,16,18,20,22,24,28,30]
        for idx in range(len(chunks)-1,-1,-1):
            ch=chunks[idx]; ordinal=idx+1 | (0x40 if idx==len(chunks)-1 else 0)
            e=bytearray(32);e[0]=ordinal;e[11]=0x0f;e[12]=0;e[13]=checksum;e[26:28]=b'\0\0'
            padded=ch+[0] if len(ch)<13 else ch
            padded=padded+[0xffff]*(13-len(padded))
            for slot,unit in zip(slots,padded):struct.pack_into('<H',e,slot,unit)
            result.append(bytes(e))
    e=bytearray(32);e[:11]=sfn;e[11]=attr;struct.pack_into('<H',e,20,0);struct.pack_into('<H',e,26,cluster&0xffff);struct.pack_into('<I',e,28,size);result.append(bytes(e));return result

class Node:
    def __init__(self,name,isdir):
        self.name=name; self.isdir=isdir; self.children={}; self.data=b''; self.cluster=0; self.chain=[]

def make_tree(files):
    root=Node('',True)
    for path,data in files.items():
        parts=[p.upper() for p in path.replace('\\','/').strip('/').split('/') if p]
        cur=root
        for part in parts[:-1]:
            short_name(part)
            if part not in cur.children: cur.children[part]=Node(part,True)
            if not cur.children[part].isdir: raise ValueError('file used as directory')
            cur=cur.children[part]
        if not parts: continue
        name=parts[-1]; short_name(name)
        if name in cur.children: raise ValueError('duplicate path: '+path)
        n=Node(name,False); n.data=data; cur.children[name]=n
    return root

def fat16_volume(total_sectors:int, files:dict[str,bytes], label='MINTOS26') -> bytes:
    bps=512; spc=4; reserved=1; nfats=2; root_entries=512; root_secs=(root_entries*32+bps-1)//bps
    # Scale clusters for larger developer/data disks while remaining within FAT16's cluster-count limit.
    while (total_sectors-reserved-nfats-root_secs)//spc >= 60000: spc*=2
    fatsz=1
    for _ in range(32):
        clusters=(total_sectors-reserved-nfats*fatsz-root_secs)//spc
        nxt=math.ceil((clusters+2)*2/bps)
        if nxt==fatsz: break
        fatsz=nxt
    data_start=reserved+nfats*fatsz+root_secs
    cluster_count=(total_sectors-data_start)//spc
    if not 4085 <= cluster_count < 65525:
        raise ValueError(f'FAT16 cluster count out of range: {cluster_count}')
    root=make_tree(files); nextcluster=2; fat=[0]*(cluster_count+2); fat[0]=0xFFF8; fat[1]=EOC
    def alloc(node,data_len):
        nonlocal nextcluster
        count=max(1, math.ceil(data_len/(spc*bps)))
        if nextcluster+count > cluster_count+2: raise ValueError('volume full')
        node.chain=list(range(nextcluster,nextcluster+count));node.cluster=node.chain[0];nextcluster+=count
        for i,c in enumerate(node.chain): fat[c]=node.chain[i+1] if i+1<len(node.chain) else EOC
    def dir_bytes(node):
        # '.' and '..' plus every child's 8.3 entry and VFAT long-name records;
        # directories may span a cluster chain like any FAT16 file.
        n=len(directory_entries('.',0x10,0))+len(directory_entries('..',0x10,0))
        for child in node.children.values(): n+=len(directory_entries(child.name,0x10 if child.isdir else 0x20,0))
        return max(spc*bps, n*32)
    def dirs(node):
        for child in node.children.values():
            if child.isdir: alloc(child,dir_bytes(child)); dirs(child)
            else: alloc(child,len(child.data))
    dirs(root)
    image=bytearray(total_sectors*bps)
    # FAT16 BPB + EBPB
    boot=bytearray(512);boot[0:3]=b'\xeb\x3c\x90';boot[3:11]=b'MINTOS26';struct.pack_into('<HBHBHHBHHHII',boot,11,bps,spc,reserved,nfats,root_entries,0,0xF8,fatsz,63,255,0,total_sectors if total_sectors>65535 else 0)
    # TotSec16 is used for smaller volumes; otherwise TotSec32 at offset 32.
    if total_sectors<=65535: struct.pack_into('<H',boot,19,total_sectors)
    else: struct.pack_into('<H',boot,19,0)
    boot[36]=0x80;boot[37]=0;boot[38]=0x29;struct.pack_into('<I',boot,39,0x26310001);boot[43:54]=label.upper().ljust(11)[:11].encode('ascii');boot[54:62]=b'FAT16   ';boot[510:512]=b'\x55\xaa'
    image[:512]=boot
    # Allocate and write directory records.
    def entry(name,attr,cluster,size=0):
        e=bytearray(32);e[:11]=short_name(name);e[11]=attr;struct.pack_into('<H',e,20,0);struct.pack_into('<H',e,26,cluster&0xffff);struct.pack_into('<I',e,28,size);return e
    def write_cluster(c,data):
        off=(data_start+(c-2)*spc)*bps; image[off:off+len(data)]=data
    def build_dir(node,parent_cluster=0,isroot=False):
        entries=[]
        if not isroot:
            entries.extend(directory_entries('.',0x10,node.cluster));entries.extend(directory_entries('..',0x10,parent_cluster))
        for child in sorted(node.children.values(),key=lambda x:(not x.isdir,x.name)):
            entries.extend(directory_entries(child.name,0x10 if child.isdir else 0x20,child.cluster,0 if child.isdir else len(child.data)))
        data=bytearray(root_secs*bps if isroot else len(node.chain)*spc*bps)
        off=0
        for e in entries:
            if off+32>len(data): raise ValueError('directory exceeds allotted entries')
            data[off:off+32]=e;off+=32
        if isroot:
            start=(reserved+nfats*fatsz)*bps; image[start:start+len(data)]=data
        else:
            for idx,c in enumerate(node.chain): write_cluster(c,data[idx*spc*bps:(idx+1)*spc*bps])
        for child in node.children.values():
            if child.isdir: build_dir(child,node.cluster,False)
            else:
                payload=child.data
                if not payload: payload=b''
                for idx,c in enumerate(child.chain):
                    part=payload[idx*spc*bps:(idx+1)*spc*bps]
                    write_cluster(c,part)
    build_dir(root,0,True)
    fatbytes=bytearray(fatsz*bps)
    for i,v in enumerate(fat): struct.pack_into('<H',fatbytes,i*2,v)
    for f in range(nfats):
        off=(reserved+f*fatsz)*bps;image[off:off+len(fatbytes)]=fatbytes
    return bytes(image)

def mbr_disk(total_sectors:int, part_start:int, part_sectors:int, vol:bytes) -> bytes:
    if len(vol)!=part_sectors*SECTOR: raise ValueError('volume size mismatch')
    disk=bytearray(total_sectors*SECTOR);mbr=bytearray(512)
    # Protective-free DOS MBR: one FAT16 LBA partition, type 0x06.
    e=446;mbr[e]=0;mbr[e+1:e+4]=b'\xfe\xff\xff';mbr[e+4]=0x06;mbr[e+5:e+8]=b'\xfe\xff\xff';struct.pack_into('<II',mbr,e+8,part_start,part_sectors);struct.pack_into('<I',mbr,440,0x26300421);mbr[510:512]=b'\x55\xaa';disk[:512]=mbr
    off=part_start*SECTOR;disk[off:off+len(vol)]=vol;return bytes(disk)

def main():
    if len(sys.argv)<3: raise SystemExit('usage: mkfat16.py volume sectors output [path=hostfile ...]')
    kind=sys.argv[1];sectors=int(sys.argv[2]);out=Path(sys.argv[3]);files={}
    for pair in sys.argv[4:]:
        path,source=pair.split('=',1);files[path]=Path(source).read_bytes()
    if kind=='volume': data=fat16_volume(sectors,files)
    elif kind=='disk':
        start=2048; count=sectors-start;vol=fat16_volume(count,files);data=mbr_disk(sectors,start,count,vol)
    else: raise SystemExit('kind must be volume or disk')
    out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(data);print(f'Built {out}: {len(data):,} bytes ({kind})')
if __name__=='__main__':main()
