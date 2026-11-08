#!/usr/bin/env python3
"""Create a fresh-UUID copy of MintOS26's empty VirtualBox dynamic VDI template."""
from __future__ import annotations
import shutil
import struct
import sys
import uuid
from pathlib import Path

SIGNATURE = 0xBEDA107F
VERSION_1_1 = 0x00010001
TYPE_DYNAMIC = 1
FREE_BLOCK = 0xFFFFFFFF
MAP_OFFSET_FIELD = 340
DATA_OFFSET_FIELD = 344
DISK_SIZE_FIELD = 368
BLOCK_SIZE_FIELD = 376
BLOCK_COUNT_FIELD = 384
ALLOCATED_COUNT_FIELD = 388
UUID_CREATE_FIELD = 392
UUID_MODIFY_FIELD = 408
UUID_LINK_FIELD = 424
UUID_PARENT_FIELD = 440


def create_blank(template: Path, output: Path) -> tuple[str, str]:
    if not template.is_file():
        raise FileNotFoundError(template)
    if output.resolve() == template.resolve():
        raise ValueError('output must not overwrite the blank template')
    raw = template.read_bytes()
    if len(raw) < 512 or b'VirtualBox Disk Image' not in raw[:64]:
        raise ValueError('input does not look like a VirtualBox VDI')
    sig, version, _, image_type, _ = struct.unpack_from('<IIIII', raw, 64)
    if sig != SIGNATURE or version != VERSION_1_1 or image_type != TYPE_DYNAMIC:
        raise ValueError('template must be a VDI 1.1 dynamic image')
    map_offset, data_offset = struct.unpack_from('<II', raw, MAP_OFFSET_FIELD)
    disk_size = struct.unpack_from('<Q', raw, DISK_SIZE_FIELD)[0]
    block_size, block_count = struct.unpack_from('<II', raw, BLOCK_SIZE_FIELD)[0], struct.unpack_from('<I', raw, BLOCK_COUNT_FIELD)[0]
    allocated = struct.unpack_from('<I', raw, ALLOCATED_COUNT_FIELD)[0]
    if disk_size != 512 * 1024 * 1024 or block_size != 1024 * 1024 or block_count != 512:
        raise ValueError('template is not the expected 512 MiB / 1 MiB-block geometry')
    if data_offset <= map_offset or len(raw) != data_offset:
        raise ValueError('template contains unexpected payload bytes')
    map_bytes = raw[map_offset:map_offset + block_count * 4]
    if len(map_bytes) != block_count * 4 or any(v != FREE_BLOCK for (v,) in struct.iter_unpack('<I', map_bytes)) or allocated != 0:
        raise ValueError('template contains allocated blocks; refusing to clone as blank')
    output.parent.mkdir(parents=True, exist_ok=True)
    create_uuid, modify_uuid = uuid.uuid4(), uuid.uuid4()
    with output.open('wb') as f:
        f.write(raw[:84])
        comment=b'MintOS26 blank disposable 512 MiB disk for MintDE Disk Utility'
        f.write(comment + bytes(256-len(comment)))
        f.write(raw[340:])
    with output.open('r+b') as f:
        f.seek(ALLOCATED_COUNT_FIELD); f.write(struct.pack('<I',0))
        f.seek(UUID_CREATE_FIELD); f.write(create_uuid.bytes_le)
        f.seek(UUID_MODIFY_FIELD); f.write(modify_uuid.bytes_le)
        f.seek(UUID_LINK_FIELD); f.write(bytes(32))
        f.flush()
    return str(create_uuid), str(modify_uuid)


def main() -> int:
    if len(sys.argv) != 3:
        print('usage: clone_blank_vdi.py TEMPLATE OUTPUT', file=sys.stderr)
        return 2
    create, modify = create_blank(Path(sys.argv[1]), Path(sys.argv[2]))
    print('Created a fresh blank VirtualBox VDI; no sectors are allocated.')
    print(f'  virtual_capacity: {512 * 1024 * 1024} bytes (512 MiB)')
    print(f'  blocks: 512 x 1 MiB; unallocated: 512')
    print(f'  uuid_create: {create}')
    print(f'  uuid_modify: {modify}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
