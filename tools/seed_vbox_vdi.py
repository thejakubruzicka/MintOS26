#!/usr/bin/env python3
"""Seed a blank VirtualBox dynamic VDI template from MintOS26's raw data image.

This intentionally creates a *new* VDI. It never modifies the supplied template
or the raw source. The VDI metadata is retained from the VirtualBox-generated
empty template; its dynamic block map is populated and its UUIDs are refreshed.
"""
from __future__ import annotations
import argparse
import os
import shutil
import struct
import uuid
from pathlib import Path

SIGNATURE = 0xBEDA107F
VERSION_1_1 = 0x00010001
TYPE_DYNAMIC = 1
SECTOR_SIZE = 512
EXPECTED_CAPACITY = 512 * 1024 * 1024
OFF_BLOCKS_FIELD = 340  # file header offset for dynamic VDI 1.1
OFF_DATA_FIELD = 344
GEOMETRY_SECTOR_FIELD = 360
DISK_SIZE_FIELD = 368
BLOCK_SIZE_FIELD = 376
BLOCK_EXTRA_FIELD = 380
BLOCK_COUNT_FIELD = 384
ALLOCATED_COUNT_FIELD = 388
UUID_CREATE_FIELD = 392
UUID_MODIFY_FIELD = 408
UUID_LINK_FIELD = 424
UUID_PARENT_FIELD = 440
FREE_BLOCK = 0xFFFFFFFF


def read_header(path: Path) -> tuple[bytes, dict[str, int]]:
    with path.open('rb') as f:
        head = f.read(512)
    if len(head) < 512 or b'VirtualBox Disk Image' not in head[:64]:
        raise ValueError('template is not a recognizable VDI file')
    sig, version, cb_header, image_type, flags = struct.unpack_from('<IIIII', head, 64)
    if sig != SIGNATURE or version != VERSION_1_1 or image_type != TYPE_DYNAMIC:
        raise ValueError('template must be a VirtualBox VDI 1.1 dynamic image')
    values = struct.unpack_from('<IIIIIIIQIIII', head, OFF_BLOCKS_FIELD)
    keys = ('map_offset', 'data_offset', 'cylinders', 'heads', 'sectors_per_track',
            'sector_size', 'reserved', 'disk_size', 'block_size', 'block_extra',
            'block_count', 'allocated_count')
    meta = dict(zip(keys, values))
    meta.update({'cb_header': cb_header, 'flags': flags, 'image_type': image_type})
    return head, meta


def seed(template: Path, raw: Path, output: Path) -> dict[str, int | str]:
    if not template.is_file() or not raw.is_file():
        raise FileNotFoundError('template and raw image must both exist')
    if output.resolve() in (template.resolve(), raw.resolve()):
        raise ValueError('output must be a new path; input files are never overwritten')
    if raw.stat().st_size <= 0 or raw.stat().st_size % SECTOR_SIZE:
        raise ValueError('raw image size must be a positive multiple of 512 bytes')
    head, m = read_header(template)
    raw_size = raw.stat().st_size
    if raw_size != m['disk_size']:
        raise ValueError(f'raw image capacity {raw_size} differs from template {m["disk_size"]}')
    if raw_size != EXPECTED_CAPACITY:
        raise ValueError('this release expects exactly a 512 MiB developer image')
    if m['sector_size'] != SECTOR_SIZE or m['block_size'] <= 0 or m['block_extra'] != 0:
        raise ValueError('unsupported dynamic VDI sector/block geometry')
    expected_blocks = (raw_size + m['block_size'] - 1) // m['block_size']
    if m['block_count'] != expected_blocks:
        raise ValueError('template block count does not cover the declared virtual capacity')
    if m['map_offset'] < 512 or m['data_offset'] <= m['map_offset']:
        raise ValueError('invalid VDI block map/data offsets')
    if template.stat().st_size != m['data_offset']:
        raise ValueError('template is not an empty, unallocated VDI (unexpected trailing data)')
    with template.open('rb') as f:
        f.seek(m['map_offset'])
        existing_map = f.read(m['block_count'] * 4)
    if len(existing_map) != m['block_count'] * 4 or any(
        value != FREE_BLOCK for (value,) in struct.iter_unpack('<I', existing_map)
    ):
        raise ValueError('template has allocated blocks; refusing to overwrite existing VDI data')

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + '.tmp')
    try:
        with template.open('rb') as src, temporary.open('wb') as dst:
            shutil.copyfileobj(src, dst, length=1024 * 1024)
            dst.seek(m['map_offset'])
            for block in range(m['block_count']):
                dst.write(struct.pack('<I', block))
            # Rewrite diagnostic metadata in the copied VDI header.
            dst.seek(84)
            comment = b'MintOS26 initialized from raw data image using VirtualBox VDI template'
            dst.write(comment + bytes(256 - len(comment)))
            dst.seek(ALLOCATED_COUNT_FIELD)
            dst.write(struct.pack('<I', m['block_count']))
            create_uuid, modify_uuid = uuid.uuid4(), uuid.uuid4()
            dst.seek(UUID_CREATE_FIELD); dst.write(create_uuid.bytes_le)
            dst.seek(UUID_MODIFY_FIELD); dst.write(modify_uuid.bytes_le)
            dst.seek(UUID_LINK_FIELD); dst.write(bytes(16))
            dst.seek(UUID_PARENT_FIELD); dst.write(bytes(16))
            dst.seek(m['data_offset'])
            with raw.open('rb') as image:
                while True:
                    chunk = image.read(1024 * 1024)
                    if not chunk:
                        break
                    dst.write(chunk)
            expected_file_size = m['data_offset'] + m['block_count'] * (m['block_size'] + m['block_extra'])
            dst.truncate(expected_file_size)
            dst.flush(); os.fsync(dst.fileno())
        temporary.replace(output)
    except Exception:
        try: temporary.unlink()
        except FileNotFoundError: pass
        raise

    return {
        'virtual_bytes': raw_size,
        'physical_bytes': output.stat().st_size,
        'blocks': m['block_count'],
        'block_size': m['block_size'],
        'map_offset': m['map_offset'],
        'data_offset': m['data_offset'],
        'uuid_create': str(create_uuid),
        'uuid_modify': str(modify_uuid),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('template', type=Path)
    parser.add_argument('raw', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = seed(args.template, args.raw, args.output)
    print('Built populated VirtualBox dynamic VDI from blank template:')
    for key, value in result.items(): print(f'  {key}: {value}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
