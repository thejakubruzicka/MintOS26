#!/usr/bin/env python3
"""MintOS-disk GPT checks against an independent parser (struct + zlib.crc32).

Builds tools/mintfs_tool.c, asks it to write the GPT that Disk Utility would
create for several disk sizes, and checks every field a UEFI firmware or
another OS relies on. Also checks that the kernel's recognizer
(mint_gpt_find_mintos) refuses corrupted tables."""
from pathlib import Path
import os, struct, subprocess, sys, tempfile, zlib

root = Path(__file__).resolve().parents[1]
results = []
def check(name, ok):
    results.append((name, bool(ok)))
    print(("PASS: " if ok else "FAIL: ") + name)

MINT_TYPE = bytes([0x74, 0x4D, 0x69, 0x6E, 0x4F, 0x53, 0x44, 0x46, 0x8A, 0x3B, 0x9C, 0x2E, 0x5F, 0x71, 0xD0, 0xA4])
CAP_SECTORS = 508 * 32512 * 8                      # MINTFS_MAX_BLOCKS * 8

def parse_header(b):
    sig, rev, hsize, crc, _r, my, alt, fu, lu = struct.unpack_from("<8sIIIIQQQQ", b, 0)
    guid = b[56:72]
    elba, num, esize, ecrc = struct.unpack_from("<QIII", b, 72)
    z = bytearray(b[:hsize]); z[16:20] = b"\0\0\0\0"
    return dict(sig=sig, rev=rev, hsize=hsize, crc=crc, crc_ok=(zlib.crc32(bytes(z)) & 0xFFFFFFFF) == crc, my=my, alt=alt,
                fu=fu, lu=lu, guid=guid, elba=elba, num=num, esize=esize, ecrc=ecrc)

with tempfile.TemporaryDirectory(prefix="mint-gpt-test-") as td:
    td = Path(td)
    tool = td / "mintfs-tool"
    cc = os.environ.get("HOST_CC", "clang")
    subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-DMINT_GPT_HOST_TEST",
                    str(root / "tools/mintfs_tool.c"), str(root / "kernel/fs/mintfs.c"), str(root / "kernel/fs/mintos_disk.c"),
                    "-o", str(tool)], check=True)
    for n in (131072, 131072 + 7, 2097152, 2097152 + 5, CAP_SECTORS + 50000):
        img = td / f"gpt-{n}.img"
        out = subprocess.run([str(tool), "gpt", str(n), str(img)], capture_output=True, text=True)
        if out.returncode != 0:
            check(f"{n} sectors: GPT built", False); continue
        first, last = map(int, out.stdout.split())
        with open(img, "rb") as f:
            f.seek(0, 2); size = f.tell()
            f.seek(0); mbr = f.read(512); hdr = f.read(512); ent = f.read(16384)
            f.seek((n - 33) * 512); bent = f.read(16384); bhdr = f.read(512)
        label = f"{n} sectors"
        check(f"{label}: image is exactly the disk size", size == n * 512)
        pe = mbr[446:462]
        check(f"{label}: protective MBR (0xEE from LBA 1, signature 55AA, one entry)",
              mbr[510:512] == b"\x55\xAA" and pe[4] == 0xEE and struct.unpack_from("<I", pe, 8)[0] == 1 and
              struct.unpack_from("<I", pe, 12)[0] == min(n - 1, 0xFFFFFFFF) and mbr[462:510] == bytes(48))
        h = parse_header(hdr); b = parse_header(bhdr)
        check(f"{label}: primary header fields and CRC", h["sig"] == b"EFI PART" and h["rev"] == 0x00010000 and h["hsize"] == 92 and
              h["crc_ok"] and h["my"] == 1 and h["alt"] == n - 1 and h["fu"] == 34 and h["lu"] == n - 34 and h["elba"] == 2 and
              h["num"] == 128 and h["esize"] == 128)
        check(f"{label}: partition array CRC", (zlib.crc32(ent) & 0xFFFFFFFF) == h["ecrc"])
        check(f"{label}: backup header mirrors the primary", b["sig"] == b"EFI PART" and b["crc_ok"] and b["my"] == n - 1 and
              b["alt"] == 1 and b["elba"] == n - 33 and b["guid"] == h["guid"] and b["ecrc"] == h["ecrc"] and bent == ent)
        e0 = ent[:128]
        pfirst, plast = struct.unpack_from("<QQ", e0, 32)
        name = e0[56:128].decode("utf-16le").rstrip("\0")
        check(f"{label}: one MintFS partition at 1 MiB, whole 4 KiB blocks, inside the usable area",
              e0[:16] == MINT_TYPE and (pfirst, plast) == (first, last) and pfirst == 2048 and (plast - pfirst + 1) % 8 == 0 and
              plast <= n - 34 and plast - pfirst + 1 <= CAP_SECTORS and name == "MintOS HD" and ent[128:] == bytes(16384 - 128))
        check(f"{label}: disk and partition GUIDs are set and distinct",
              h["guid"] != bytes(16) and e0[16:32] != bytes(16) and h["guid"] != e0[16:32])
        if n > CAP_SECTORS:
            check(f"{label}: partition capped at the MintFS maximum (~63 GiB)", plast - pfirst + 1 == CAP_SECTORS)
        else:
            check(f"{label}: partition uses the disk up to the backup GPT", n - 34 - plast < 8)
        # The kernel's recognizer accepts it (mintfs_tool check prints the partition) ...
        r = subprocess.run([str(tool), "check", str(img)], capture_output=True, text=True)
        check(f"{label}: kernel recognizer finds the MintOS partition", f"LBA {first}-{last}" in r.stdout)
        img.unlink()
    # ... and refuses damaged tables.
    img = td / "bad.img"
    for what, offset in (("header", 512 + 40), ("partition array", 1024 + 32)):
        subprocess.run([str(tool), "gpt", "131072", str(img)], check=True, capture_output=True)
        with open(img, "r+b") as f:
            f.seek(offset); v = f.read(1); f.seek(offset); f.write(bytes([v[0] ^ 0x5A]))
        r = subprocess.run([str(tool), "check", str(img)], capture_output=True, text=True)
        check(f"recognizer refuses a GPT with a corrupted {what}", r.returncode != 0 and "no valid GPT" in r.stderr)
    r = subprocess.run([str(tool), "gpt", str(2048 + 64 * 8 + 33), str(img)], capture_output=True, text=True)
    check("disks too small for a MintOS partition are refused", r.returncode != 0)

failed = [n for n, ok in results if not ok]
print(f"{len(results)} GPT checks, {len(failed)} failures")
sys.exit(1 if failed else 0)
