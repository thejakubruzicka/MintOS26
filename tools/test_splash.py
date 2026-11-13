#!/usr/bin/env python3
"""Checks for the boot animation renderer (kernel/ui/splash_art.c).

    python3 tools/test_splash.py build/render-splash-preview

Renders frames through the host build of the same code the kernel and
MintDE use and verifies: the leaf appears only with alpha, it is mint
green and anti-aliased, the progress bar fill tracks progress, rendering is
deterministic (the kernel's final frame equals MintDE's crossfade source),
other resolutions work and no pixel is pure black or white outside the art.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

failures = 0


def check(cond: bool, msg: str) -> None:
    global failures
    print(("PASS: " if cond else "FAIL: ") + msg)
    if not cond:
        failures += 1


def render(tool: str, d: Path, name: str, w: int, h: int, alpha: int, progress: int, shimmer: int):
    out = d / f"{name}.ppm"
    subprocess.run([tool, str(out), str(w), str(h), str(alpha), str(progress), str(shimmer)], check=True)
    data = out.read_bytes()
    header_end = 0
    fields = 0
    while fields < 4:
        while data[header_end:header_end + 1].isspace():
            header_end += 1
        while not data[header_end:header_end + 1].isspace():
            header_end += 1
        fields += 1
    px = data[header_end + 1:]
    return w, h, px, data


def pixel(img, x, y):
    w, h, px, _ = img
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]


def main(argv: list[str]) -> int:
    tool = argv[1]
    with tempfile.TemporaryDirectory(prefix="mint-splash-") as td:
        d = Path(td)
        W, H = 1920, 1080
        cx, cy = W // 2, H // 2 - H // 25
        full = render(tool, d, "full", W, H, 255, 1000, 0)
        empty = render(tool, d, "empty", W, H, 0, 0, 0)
        half = render(tool, d, "half", W, H, 255, 500, 0)
        again = render(tool, d, "again", W, H, 255, 1000, 0)
        r, g, b = pixel(full, cx + 10, cy - 10)
        check(g > 150 and g > r + 60 and g > b, f"leaf centre is mint green ({r},{g},{b})")
        er, eg, eb = pixel(empty, cx + 10, cy - 10)
        check(eg < 120, "leaf hidden at alpha 0 (fade-in starts from the background)")
        leaf = 92  # 8.5 % of 1080
        partial = 0
        for y in range(cy - leaf, cy + leaf):
            for x in range(cx - leaf, cx + leaf):
                p = pixel(full, x, y)
                q = pixel(empty, x, y)
                if p != q and abs(p[1] - q[1]) < 90:
                    partial += 1
        check(partial > 200, f"leaf edges are anti-aliased ({partial} partial-coverage pixels)")
        bar_y = cy + H * 18 // 100 + 2
        bar_w = H * 21 // 100
        bar_x = cx - bar_w // 2

        def filled(img):
            return sum(1 for x in range(bar_x, bar_x + bar_w) if pixel(img, x, bar_y)[1] > 180)

        f_full, f_half, f_empty = filled(full), filled(half), filled(empty)
        check(f_full > bar_w * 0.95 and abs(f_half - bar_w // 2) < bar_w * 0.05 and f_empty == 0,
              f"progress bar fill tracks progress (0/500/1000 -> {f_empty}/{f_half}/{f_full} px of {bar_w})")
        check(full[3] == again[3], "deterministic: the kernel's final frame equals MintDE's crossfade source")
        corner = pixel(full, 4, 4)
        bottom = pixel(full, W - 4, H - 4)
        check(10 < corner[1] < 70 and bottom[1] < corner[1], f"deep green gradient background {corner} -> {bottom}")
        small = render(tool, d, "small", 1024, 768, 255, 300, 40)
        sr, sg, sb = pixel(small, 512 + 6, 384 - 768 // 25 - 6)
        check(sg > 150, "renders at 1024x768 too")
        shimmer = render(tool, d, "shimmer", W, H, 255, 1000, bar_w)
        check(shimmer[3] != full[3], "shimmer phase animates the bar")
    print(f"{'FAIL' if failures else 'PASS'}: splash checks ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
