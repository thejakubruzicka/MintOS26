#!/usr/bin/env python3
"""Convert a sector-aligned raw disk image to a fixed VirtualBox VDI 1.1."""
import argparse
import os
import struct
import uuid
from pathlib import Path

SECTOR_SIZE = 512
BLOCK_SIZE = 1024 * 1024
VDI_SIGNATURE = 0xBEDA107F
VDI_VERSION_1_1 = 0x00010001
VDI_TYPE_STATIC = 2
VDI_HEADER_SIZE = 512
VDI_HEADER_MAIN_SIZE = 0x180
TEXT = b"<<< Sun VirtualBox Disk Image >>>\n"


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def convert(source: Path, destination: Path) -> None:
    if source.resolve() == destination.resolve():
        raise ValueError("source and destination must be different files")
    if not source.is_file():
        raise FileNotFoundError(source)
    source_bytes = source.stat().st_size
    if source_bytes <= 0 or source_bytes % SECTOR_SIZE:
        raise ValueError("source size must be a positive multiple of 512 bytes")
    block_count = (source_bytes + BLOCK_SIZE - 1) // BLOCK_SIZE
    map_bytes = align_up(block_count * 4, SECTOR_SIZE)
    map_offset = VDI_HEADER_SIZE
    data_offset = map_offset + map_bytes
    output_bytes = data_offset + block_count * BLOCK_SIZE

    header = bytearray(VDI_HEADER_SIZE)
    header[:len(TEXT)] = TEXT
    struct.pack_into("<IIIII", header, 64,
                     VDI_SIGNATURE, VDI_VERSION_1_1, VDI_HEADER_MAIN_SIZE,
                     VDI_TYPE_STATIC, 0)
    description = b"MintOS26 disposable developer data disk - generated from raw image"
    header[84:84 + len(description)] = description
    struct.pack_into("<IIIIIIIQIIII", header, 340,
                     map_offset, data_offset, 0, 0, 0, SECTOR_SIZE, 0,
                     source_bytes, BLOCK_SIZE, 0, block_count, block_count)
    header[392:408] = uuid.uuid4().bytes_le
    header[408:424] = uuid.uuid4().bytes_le
    # Linked and parent UUIDs stay all-zero: this is a standalone base image.

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    try:
        with source.open("rb") as src, temporary.open("wb") as dst:
            dst.write(header)
            # Fixed VDI uses a static block map: logical block N maps to
            # physical payload block N. The map must precede every payload.
            for index in range(block_count):
                dst.write(struct.pack("<I", index))
            map_padding = map_bytes - block_count * 4
            if map_padding:
                dst.write(bytes(map_padding))
            if dst.tell() != data_offset:
                raise IOError("VDI block map did not end at declared data offset")
            for _ in range(block_count):
                chunk = src.read(BLOCK_SIZE)
                if len(chunk) < BLOCK_SIZE:
                    chunk += bytes(BLOCK_SIZE - len(chunk))
                dst.write(chunk)
            dst.truncate(output_bytes)
            dst.flush()
            os.fsync(dst.fileno())
        temporary.replace(destination)
    except Exception:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise
    print(f"Built fixed VDI: {destination} ({destination.stat().st_size} bytes; virtual capacity {source_bytes} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("vdi", type=Path)
    args = parser.parse_args()
    convert(args.raw, args.vdi)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
