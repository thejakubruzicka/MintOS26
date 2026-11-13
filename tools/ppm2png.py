#!/usr/bin/env python3
"""Convert a binary PPM (P6) to PNG without third-party modules.

    python3 tools/ppm2png.py in.ppm out.png [--crop x y w h] [--scale N]

--scale N box-averages N x N blocks (integer downscale); --crop runs first.
Used for boot-splash and wallpaper previews and for QEMU evidence (PPM
screendumps); macOS `sips -z H W file.png` can resample further.
"""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


def read_ppm(path: Path):
    data = path.read_bytes()
    parts = []
    i = 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    if parts[0] != b"P6" or int(parts[3]) != 255:
        raise SystemExit("only 8-bit P6 PPM is supported")
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]


def write_png(path: Path, w: int, h: int, rgb: bytes) -> None:
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def crop(w, h, rgb, x, y, cw, ch):
    out = bytearray()
    for row in range(y, y + ch):
        out += rgb[(row * w + x) * 3:(row * w + x + cw) * 3]
    return cw, ch, bytes(out)


def downscale(w, h, rgb, n):
    ow, oh = w // n, h // n
    out = bytearray(ow * oh * 3)
    area = n * n
    for oy in range(oh):
        rows = [rgb[((oy * n + k) * w) * 3:((oy * n + k) * w + ow * n) * 3] for k in range(n)]
        for ox in range(ow):
            s = [0, 0, 0]
            base = ox * n * 3
            for r in rows:
                seg = r[base:base + n * 3]
                s[0] += sum(seg[0::3]); s[1] += sum(seg[1::3]); s[2] += sum(seg[2::3])
            o = (oy * ow + ox) * 3
            out[o:o + 3] = bytes((s[0] // area, s[1] // area, s[2] // area))
    return ow, oh, bytes(out)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    w, h, rgb = read_ppm(Path(argv[1]))
    args = argv[3:]
    if "--crop" in args:
        k = args.index("--crop")
        x, y, cw, ch = (int(v) for v in args[k + 1:k + 5])
        w, h, rgb = crop(w, h, rgb, x, y, cw, ch)
    if "--scale" in args:
        n = int(args[args.index("--scale") + 1])
        if n > 1:
            w, h, rgb = downscale(w, h, rgb, n)
    write_png(Path(argv[2]), w, h, rgb)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
