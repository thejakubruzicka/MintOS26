#!/usr/bin/env python3
"""Static verifier for MintOS26 UEFI application, GPT/ESP, data disk and ISO."""
from pathlib import Path
import struct,sys,zlib
S=512; ISO_SECTOR=2048

def fat_read(volume:bytes, path:str, base_lba=0):
    bps=struct.unpack_from('<H',volume,11)[0]; spc=volume[13]; reserved=struct.unpack_from('<H',volume,14)[0]; nf=volume[16]; roots=struct.unpack_from('<H',volume,17)[0]; fatsz=struct.unpack_from('<H',volume,22)[0]
    assert bps==512 and spc in (1,2,4,8,16,32,64,128), 'bad FAT BPB'
    rootsecs=(roots*32+bps-1)//bps; root_lba=(reserved+nf*fatsz); data_lba=root_lba+rootsecs
    fat=volume[reserved*bps:(reserved+fatsz)*bps]
    def chain_read(cluster,size):
        out=bytearray(); seen=set()
        while cluster>=2 and cluster not in seen:
            seen.add(cluster); off=(data_lba+(cluster-2)*spc)*bps;out+=volume[off:off+spc*bps]
            v=struct.unpack_from('<H',fat,cluster*2)[0]
            if v>=0xfff8 or v==0:break
            cluster=v
        return bytes(out[:size])
    def find_entry(dirdata,name):
        base=name.upper().split('.',1)[0]; ext=name.upper().split('.',1)[1][:3] if '.' in name else ''; alias=(base[:6]+'~1')[:8] if len(base)>8 else base; target=(alias.ljust(8)+ext.ljust(3)).encode('ascii')
        for off in range(0,len(dirdata),32):
            e=dirdata[off:off+32]
            if len(e)<32 or e[0] in (0,0xe5):
                if e and e[0]==0:break
                continue
            if e[11]==0x0f:continue
            if e[:11]==target:return e
        raise AssertionError('FAT path not found: '+name)
    rstart=root_lba*bps;rootdata=volume[rstart:rstart+rootsecs*bps];cluster=0;entry=None
    parts=[p for p in path.replace('\\','/').strip('/').split('/') if p]
    for ix,part in enumerate(parts):
        ddata=rootdata if cluster==0 else chain_read(cluster,spc*bps)
        entry=find_entry(ddata,part);cluster=struct.unpack_from('<H',entry,26)[0]
        if ix<len(parts)-1: assert entry[11]&0x10, 'not a directory: '+part
    size=struct.unpack_from('<I',entry,28)[0]
    return chain_read(cluster,size),size

def verify_pe(data):
    assert data[:2]==b'MZ','missing PE DOS header';off=struct.unpack_from('<I',data,0x3c)[0];assert data[off:off+4]==b'PE\0\0','bad PE signature';machine,sections=struct.unpack_from('<HH',data,off+4);optional=struct.unpack_from('<H',data,off+20)[0];sub=struct.unpack_from('<H',data,off+24+68)[0]
    assert machine==0x8664,'EFI image is not x86_64';assert sub==10,'PE subsystem is not EFI application';assert len(data)>4096

def main():
    if len(sys.argv)!=9:raise SystemExit('usage: verify_uefi.py BOOTX64.EFI ESP.img UEFI-DISK.img DATA.img ISO KERNEL.ELF USERINIT.ELF DESKTOP.ELF')
    efip,espp,discp,datap,isop,kernelp,userp,desktopp=map(Path,sys.argv[1:]);efi=efip.read_bytes();esp=espp.read_bytes();disk=discp.read_bytes();data=datap.read_bytes();iso=isop.read_bytes();kernel=kernelp.read_bytes();userelf=userp.read_bytes();desktopelf=desktopp.read_bytes();verify_pe(efi);print('PASS: PE32+ x86_64 EFI application')
    assert len(esp)%S==0 and esp[510:512]==b'\x55\xaa';wall,n=fat_read(esp,'MINTOS26/WALLPAPE.RGB');assert n==1920*1080*3 and len(wall)==n;boot,n2=fat_read(esp,'EFI/BOOT/BOOTX64.EFI');assert boot==efi;dev,_=fat_read(esp,'MINTOS26/DEVELOP.TXT');hello,_=fat_read(esp,'MINTOS26/HELLO.C');app,_=fat_read(esp,'MINTOS26/APP.TXT');locale,_=fat_read(esp,'MINTOS26/LOCALE.TXT');kernel_in_esp,_=fat_read(esp,'MINTOS26/KERNEL.ELF');user_in_esp,_=fat_read(esp,'MINTOS26/USERINIT.ELF');desktop_in_esp,_=fat_read(esp,'MINTOS26/DESKTOP.ELF');assert kernel_in_esp==kernel and user_in_esp==userelf and desktop_in_esp==desktopelf and userelf.startswith(b'\x7fELF') and desktopelf.startswith(b'\x7fELF') and kernel.startswith(b'\x7fELF') and b'development' in dev.lower() and b'main' in hello.lower() and b'TCP/IP' in app and b'timezone' in locale.lower();print('PASS: FAT16 EFI volume includes UEFI fallback, wallpaper, developer docs, kernel, smoke-test ELF and native desktop ELF')
    for icon_name in ['files.png','launchpad.png','terminal.png','settings.png','system-monitor.png','disk-utility.png','console.png','system-info.png','network.png','about-mintos.png','mintde.png','notes.png','calculator.png','trash.png']:
        icon_data,icon_size=fat_read(esp,'MINTOS26/ICONS/'+icon_name)
        assert icon_size>256 and icon_data[:8]==b'\x89PNG\r\n\x1a\n', 'missing/invalid EFI icon PNG: '+icon_name
    print('PASS: EFI volume contains 12 separate valid PNG app icons')
    release_changelog,release_size=fat_read(esp,'MINTOS26/CHANGELOG.TXT')
    fs_roadmap,fs_size=fat_read(esp,'MINTOS26/FILESYS.TXT')
    assert release_size>500 and b'26.5.009.0' in release_changelog, 'release changelog missing from EFI volume'
    assert fs_size>500 and b'ext4' in fs_roadmap.lower(), 'filesystem roadmap missing from EFI volume'
    print('PASS: release changelog and filesystem roadmap are available as Files-visible EFI volume documents')
    assert disk[512:520]==b'EFI PART';hdr=bytearray(disk[512:1024]);crc=struct.unpack_from('<I',hdr,16)[0];struct.pack_into('<I',hdr,16,0);assert zlib.crc32(hdr[:92])&0xffffffff==crc;elba=struct.unpack_from('<Q',disk,512+72)[0];entry_count,entry_size=struct.unpack_from('<II',disk,512+80);assert elba==2 and entry_count==128 and entry_size==128;esp_start,esp_end=struct.unpack_from('<QQ',disk,2*S+32);assert esp_start==2048 and esp_end>=esp_start;print('PASS: GPT UEFI disk and EFI System Partition')
    assert len(disk)==128*1024*1024 and len(data)==512*1024*1024,'boot disk should be 128 MiB and data disk should be 512 MiB';assert data[510:512]==b'\x55\xaa' and data[450]==0x06;part_start,part_len=struct.unpack_from('<II',data,454);vol=data[part_start*S:(part_start+part_len)*S];welcome,_=fat_read(vol,'MINTOS26/DOCUMENTS/WELCOME.TXT');notes,_=fat_read(vol,'MINTOS26/DOCUMENTS/NOTES.TXT');hello,_=fat_read(vol,'MINTOS26/DEVELOP/HELLO.C');dev,_=fat_read(vol,'MINTOS26/DEVELOP/DEVELOP.TXT');app,_=fat_read(vol,'MINTOS26/DEVELOP/APP.TXT');locale,_=fat_read(vol,'MINTOS26/DEVELOP/LOCALE.TXT');user_on_data,_=fat_read(vol,'MINTOS26/DEVELOP/USERINIT.ELF');desktop_on_data,_=fat_read(vol,'MINTOS26/DEVELOP/DESKTOP.ELF');assert user_on_data==userelf and desktop_on_data==desktopelf and b'MintOS26' in welcome and b'Notes' in notes and b'int main' in hello and b'development' in dev.lower() and b'TCP/IP' in app and b'timezone' in locale.lower();print('PASS: 512 MiB MBR/FAT16 data disk with Documents, developer templates, smoke ELF and desktop ELF')
    for icon_name in ['files.png','launchpad.png','terminal.png','settings.png','system-monitor.png','disk-utility.png','console.png','system-info.png','network.png','about-mintos.png','mintde.png','notes.png','calculator.png','trash.png']:
        icon_data,icon_size=fat_read(vol,'MINTOS26/DEVELOP/ICONS/'+icon_name)
        assert icon_size>256 and icon_data[:8]==b'\x89PNG\r\n\x1a\n', 'missing/invalid data-disk icon PNG: '+icon_name
    print('PASS: developer data volume contains all 12 separate PNG app icons')
    data_changelog,dc_size=fat_read(vol,'MINTOS26/DEVELOP/CHANGELOG.TXT')
    driver_roadmap,dr_size=fat_read(vol,'MINTOS26/DEVELOP/DRIVERS.TXT')
    assert dc_size>500 and b'26.5.009.0' in data_changelog, 'changelog missing from developer volume'
    assert dr_size>500 and b'LBA48' in driver_roadmap, 'driver roadmap missing from developer volume'
    print('PASS: changelog and driver roadmap are available on the developer volume')
    assert len(iso)%ISO_SECTOR==0 and iso[16*ISO_SECTOR+1:16*ISO_SECTOR+6]==b'CD001';br=iso[17*ISO_SECTOR:18*ISO_SECTOR];assert br[1:6]==b'CD001' and b'EL TORITO SPECIFICATION' in br;cat_lba=struct.unpack_from('<I',br,71)[0];cat=iso[cat_lba*ISO_SECTOR:(cat_lba+1)*ISO_SECTOR];assert cat[1]==0xEF and cat[30:32]==b'\x55\xaa';assert sum(struct.unpack('<16H',cat[:32]))&0xffff==0;img_lba=struct.unpack_from('<I',cat,40)[0];assert iso[img_lba*ISO_SECTOR:img_lba*ISO_SECTOR+len(esp)]==esp;print('PASS: El Torito UEFI platform catalog and embedded FAT boot image')
    assert len(kernel)>0 and kernel[0:4]==b'\x7fELF','kernel payload is not ELF';print(f'INFO: EFI {len(efi):,} bytes; ESP {len(esp)//(1024*1024)} MiB; ISO {len(iso)//(1024*1024)} MiB; boot disk {len(disk)//(1024*1024)} MiB; data disk {len(data)//(1024*1024)} MiB; kernel ELF {len(kernel):,} bytes; user ELF {len(userelf):,} bytes')
if __name__=='__main__':main()
