#!/usr/bin/env python3
"""Structural release checks for diagnostics-only storage and fixed VDI output.

These checks inspect files and linked artifacts on the host. They do not exercise AHCI/DMA in a VM.
"""
from __future__ import annotations
import hashlib
import struct
import sys
from pathlib import Path

SECTOR = 512
MIB = 1024 * 1024
SIGNATURE = 0xBEDA107F
VERSION = 0x00010001


def check(ok: bool, message: str) -> None:
    print(f"{'PASS' if ok else 'FAIL'}: {message}")
    if not ok:
        raise SystemExit(1)


def u16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf: bytes, off: int) -> int:
    return struct.unpack_from("<Q", buf, off)[0]


def digest_range(path: Path, offset: int, size: int) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        f.seek(offset)
        remain = size
        while remain:
            block = f.read(min(remain, MIB))
            if not block:
                raise OSError(f"unexpected EOF hashing {path}")
            h.update(block)
            remain -= len(block)
    return h.hexdigest()


def main() -> int:
    if len(sys.argv) != 5:
        print("usage: verify_storage_diag.py RAW VDI KERNEL.ELF DESKTOP.ELF", file=sys.stderr)
        return 2
    raw, vdi, kernel, desktop = map(Path, sys.argv[1:])
    for path in (raw, vdi, kernel, desktop):
        check(path.is_file() and path.stat().st_size > 0, f"artifact exists and is non-empty: {path}")

    raw_size = raw.stat().st_size
    check(raw_size == 512 * MIB and raw_size % SECTOR == 0,
          "developer raw image is exactly 512 MiB and sector-aligned")
    with raw.open("rb") as f:
        mbr = f.read(SECTOR)
        check(mbr[510:512] == b"\x55\xaa", "developer raw image has valid MBR signature")
        check(u32(mbr, 440) == 0x26300421, "developer raw image has the MintOS26 identity marker")
        entries = []
        for i in range(4):
            e = mbr[446 + i * 16:446 + (i + 1) * 16]
            ptype = e[4]
            start, count = struct.unpack_from("<II", e, 8)
            if ptype or start or count:
                entries.append((i + 1, e[0], ptype, start, count))
        check(len(entries) == 1 and entries[0][2:] == (0x06, 2048, 1046528),
              "single FAT16 MBR partition has type 0x06, start LBA 2048, length 1,046,528 sectors")
        _, bootable, _, start, count = entries[0]
        check(bootable in (0, 0x80) and start + count <= raw_size // SECTOR,
              "partition status byte and device bounds are valid")
        f.seek(start * SECTOR)
        bpb = f.read(SECTOR)
    check(bpb[510:512] == b"\x55\xaa", "partition boot sector has 0x55AA signature")
    check(bpb[3:11] == b"MINTOS26" and bpb[54:62] == b"FAT16   ",
          "partition carries MINTOS26 OEM identifier and FAT16 type label")
    bps, spc, reserved, fats, roots, total16, fatsz = (
        u16(bpb, 11), bpb[13], u16(bpb, 14), bpb[16], u16(bpb, 17), u16(bpb, 19), u16(bpb, 22))
    total = total16 or u32(bpb, 32)
    root_sectors = (roots * 32 + bps - 1) // bps if bps else 0
    data_start = reserved + fats * fatsz + root_sectors
    clusters = (total - data_start) // spc if spc and total >= data_start else 0
    check((bps, spc, reserved, fats, roots, total, fatsz) == (512, 32, 1, 2, 512, 1046528, 128),
          "FAT16 BPB fields match expected developer disk geometry")
    check(4085 <= clusters <= 65524 and clusters == 32694,
          "FAT16 cluster count falls within the FAT16 range (32,694 clusters)")

    vsize = vdi.stat().st_size
    with vdi.open("rb") as f:
        header = f.read(SECTOR)
        signature, version, header_size, image_type, flags = struct.unpack_from("<IIIII", header, 64)
        map_offset, data_offset, _cyl, _heads, _spt, sector_size, _unused, disk_size, block_size, extra, block_count, allocated = struct.unpack_from("<IIIIIIIQIIII", header, 340)
        check(header[:len(b"<<< Sun VirtualBox Disk Image >>>\n")] == b"<<< Sun VirtualBox Disk Image >>>\n",
              "VDI contains VirtualBox disk image signature text")
        check((signature, version, header_size, image_type) == (SIGNATURE, VERSION, 0x180, 2),
              "VDI header is version 1.1, fixed/static image type")
        check(map_offset == 512 and data_offset == 2560 and sector_size == 512 and
              disk_size == raw_size and block_size == MIB and block_count == 512 and allocated == 512,
              "VDI disk geometry, block map offset and declared raw capacity are internally consistent")
        check(extra == 0 and flags == 0, "VDI has zero block-extra/flags fields")
        check(vsize == data_offset + block_count * block_size,
              "VDI file length equals header/map plus fixed 1 MiB data blocks")
        f.seek(map_offset)
        mapping = f.read(block_count * 4)
    entries_map = struct.unpack(f"<{block_count}I", mapping)
    check(entries_map == tuple(range(block_count)),
          "fixed VDI block map is complete and maps each logical block to its payload block")
    raw_hash = digest_range(raw, 0, raw_size)
    vdi_hash = digest_range(vdi, data_offset, raw_size)
    check(raw_hash == vdi_hash, "VDI payload SHA-256 matches the raw developer image byte-for-byte")

    for label, path in (("kernel", kernel), ("desktop", desktop)):
        data = path.read_bytes()[:64]
        check(len(data) >= 64 and data[:4] == b"\x7fELF" and data[4] == 2 and data[5] == 1 and
              u16(data, 16) == 2 and u16(data, 18) == 62,
              f"{label} is a little-endian x86-64 ELF executable")

    root = Path(__file__).resolve().parents[1]
    ahci = (root / "kernel/block/ahci.c").read_text()
    identify = (root / "kernel/diag/ata_identify.c").read_text()
    parser = (root / "kernel/fs/storage_diag.c").read_text()
    block = (root / "kernel/block/block.c").read_text()
    diag = (root / "kernel/diag/storage_diag.c").read_text()
    desktop_src = (root / "kernel/user/desktop.c").read_text() + "".join(f.read_text() for f in sorted((root / "kernel/user/mintde").glob("*.c")) if f.name != "font_data.c")
    abi = (root / "kernel/abi/syscall.c").read_text()
    log_src = (root / "kernel/console/log.c").read_text()
    checks = [
        ("bounded AHCI polling with LBA28 fallback and LBA48 DMA EXT commands", "AHCI_WAIT_LIMIT" in ahci and "AHCI_CMD_READ_DMA" in ahci and "AHCI_CMD_READ_DMA_EXT" in ahci and "ATA_LBA48_LIMIT" in ahci),
        ("COM1 reports raw IDENTIFY sector counts and each partition validation reason", "lba28_raw=" in ahci and "lba48_raw=" in ahci and "part->reason" in ahci),
        ("IDENTIFY checks LBA48 capability before selecting words 100-103", "0xC000u" in identify and "0x0400u" in identify and "words[100]" in identify),
        ("MBR and BPB parser bounds partition offset/count and cluster geometry", "MINT_STORAGE_PART_OUT_OF_RANGE" in parser and "MINT_STORAGE_PART_INVALID_BPB" in parser and "MINT_FAT16_MIN_CLUSTERS" in parser),
        ("LBA0 is read twice, compared, and fingerprinted before parsing", "MINT_STORAGE_LBA0_REPEAT_DIFFERS" in parser and "crc32_sector" in parser and "LBA 0 stability check" in parser),
        ("AHCI controller port-implemented bitmap is logged after MMIO read", "log_port_implemented(ci)" in ahci and "ci->port_implemented = mmio_read32(hba, AHCI_PI)" in ahci),
        ("block write API rejects generic writes to flagged read-only devices and records denied writes", "MINT_BLOCK_FLAG_READONLY" in block and "!d->write_sector" in block and "writes_blocked" in block),
        ("storage probe keeps persistent writes disabled and adds a read-only mount state", "snapshot.persistent_mount_active = 0u" in diag and "snapshot.writes_attempted = 0u" in diag and "snapshot.readonly_mount_active" in diag),
        ("storage snapshot exposes read-only mount count for multiple volumes", "readonly_mount_count" in (root / "kernel/include/storage_diag.h").read_text() and "snapshot.readonly_mount_count = mounted_list.count" in diag),
        ("generic VFS mount table lists each FAT16 volume separately", "MINT_VFS_MAX_MOUNTS" in (root / "kernel/include/vfs.h").read_text() and '"/Volumes/"' in (root / "kernel/fs/vfs.c").read_text()),
        ("FAT16 exposes bounded readonly reads plus an explicit blank-disk formatter, not general file writes", "mint_fat16_read_file" in (root / "kernel/fs/fat16.c").read_text() and "mint_fat16_format_blank" in (root / "kernel/fs/fat16.c").read_text() and "mint_fat16_write_file" not in (root / "kernel/fs/fat16.c").read_text()),
        ("driver catalog is exposed through validated ring-3 syscall", "MINT_SYS_DRIVER_CATALOG" in abi and "mint_driver_catalog_snapshot" in abi),
        ("kernel exposes validated storage diagnostics and log syscalls", "MINT_SYS_STORAGE_DIAGNOSTICS" in abi and "MINT_SYS_LOG_READ" in abi),
        ("MintDE Storage & Logs UI includes export and raw IDENTIFY fields", "Storage & Logs" in desktop_src and "export_storage_report" in desktop_src and "reported_lba48_sectors" in desktop_src),
        ("log ring is bounded and supports severity filtering", "MINT_LOG_RING_CAPACITY" in log_src and "minimum_severity" in log_src),
        ("release source labels this build 26.5.012.0", "VERSION := 26.5.012.0" in (root / "Makefile").read_text()),
        ("blank-disk initialization is bounded and writes MBR last after repeated zero-sector validation", "mint_fat16_format_blank" in (root / "kernel/fs/fat16.c").read_text() and "sector_equal_local(mbr,verify_mbr)" in (root / "kernel/fs/fat16.c").read_text() and "MINT_STORAGE_DEVELOPER_DISK_ID" in (root / "kernel/fs/fat16.c").read_text()),
        ("native UI exports LBA0 signature, repeat status, CRC32 and disk marker", "lba0_repeat_status" in desktop_src and "developer_disk_id_match" in desktop_src and "CRC32=" in desktop_src),
        ("MintDE includes driver inventory and explicit network status apps", all(k in desktop_src for k in ["Hardware & Drivers", "Network", "MintDE", "APP_DRIVERS", "APP_NETWORK"])),
    ]
    for label, ok in checks:
        check(ok, label)

    header_text = (root / "kernel/include/storage_diag.h").read_text()
    ahci_text = (root / "kernel/block/ahci.c").read_text()
    diag_text = (root / "kernel/diag/storage_diag.c").read_text()
    syscall_text = (root / "kernel/abi/syscall.c").read_text()
    check("diagnostics snapshot maps each row to its registered block index", "block_index" in header_text and "block_registered" in header_text and "device_for_block_index" in diag_text)
    check("Disk Utility resolves the selected diagnostics row before formatting", "info->block_index" in syscall_text and "mint_fat16_format_blank(info->block_index)" in syscall_text)
    check("AHCI records registry mapping only after successful registration", "di->block_index = mint_block_count()" in ahci_text and "di->block_registered = 1u" in ahci_text)
    check("format failures identify the target block before rechecking other volumes", "Disk Utility format failed for block index" in syscall_text and "per-device read-only revalidation follows" in syscall_text)
    check("read-only revalidation records each actual block index, not a generic post-format success", "read-only revalidation passed for block index" in diag_text and "generic FAT16 revalidation passed for block index" in diag_text and "after disk operation" not in diag_text)

    print("INFO: these are host artifact/source checks only; AHCI/DMA requires VirtualBox runtime validation.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
