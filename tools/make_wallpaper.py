#!/usr/bin/env python3
"""Generate MintOS26's original "Mint Waves" wallpapers (light and dark).

The design is procedural and original: a soft sky gradient, a diffuse sun
glow and several translucent flowing wave layers with depth blur. Output is
deterministic. Requires Pillow (only needed when regenerating assets):

    python3 tools/make_wallpaper.py            # writes assets/wallpaper-*.{png,rgb}
"""
from __future__ import annotations

import math
import random
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
W, H = 1920, 1080
SS = 2  # supersample factor for smooth edges

PALETTES = {
    "light": {
        "sky": [(0.00, (118, 178, 238)), (0.40, (178, 212, 244)), (0.72, (246, 214, 204)), (1.00, (252, 232, 214))],
        "sun": ((255, 238, 210), 0.80, (0.74, 0.26)),
        "waves": [
            # (base y, tilt, amplitude, wavelength, phase, colors top->bottom, opacity, blur)
            (0.44, -0.16, 0.090, 1.60, 0.4, ((150, 228, 206), (84, 190, 178)), 0.60, 18),
            (0.55, 0.12, 0.100, 1.25, 2.3, ((84, 200, 196), (36, 140, 170)), 0.74, 9),
            (0.66, -0.10, 0.095, 1.05, 4.2, ((76, 152, 232), (44, 92, 190)), 0.82, 4),
            (0.77, 0.14, 0.085, 0.90, 1.0, ((140, 116, 232), (88, 62, 184)), 0.88, 2),
            (0.88, -0.08, 0.070, 1.20, 5.3, ((48, 186, 160), (18, 110, 120)), 0.95, 1),
        ],
    },
    "dark": {
        "sky": [(0.00, (8, 12, 32)), (0.45, (20, 26, 66)), (0.78, (48, 34, 92)), (1.00, (70, 40, 96))],
        "sun": ((150, 110, 255), 0.50, (0.74, 0.30)),
        "waves": [
            (0.44, -0.16, 0.090, 1.60, 0.4, ((44, 132, 142), (22, 80, 104)), 0.52, 18),
            (0.55, 0.12, 0.100, 1.25, 2.3, ((30, 116, 150), (16, 62, 112)), 0.68, 9),
            (0.66, -0.10, 0.095, 1.05, 4.2, ((48, 80, 182), (26, 42, 130)), 0.78, 4),
            (0.77, 0.14, 0.085, 0.90, 1.0, ((104, 70, 190), (56, 34, 124)), 0.84, 2),
            (0.88, -0.08, 0.070, 1.20, 5.3, ((24, 118, 112), (8, 56, 70)), 0.92, 1),
        ],
    },
}


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def gradient(stops, w, h):
    """Vertical multi-stop gradient, computed on a 1-px column and stretched."""
    col = Image.new("RGB", (1, h))
    px = col.load()
    for y in range(h):
        t = y / (h - 1)
        for i in range(len(stops) - 1):
            t0, c0 = stops[i]
            t1, c1 = stops[i + 1]
            if t0 <= t <= t1:
                px[0, y] = lerp(c0, c1, (t - t0) / (t1 - t0) if t1 > t0 else 0)
                break
    return col.resize((w, h), Image.BILINEAR)


def wave_mask(base, tilt, amp, wavelength, phase, w, h):
    pts = []
    for x in range(0, w + 8, 8):
        u = x / w
        y = base + tilt * (u - 0.5) + amp * (0.62 * math.sin(2 * math.pi * u / wavelength + phase)
                          + 0.28 * math.sin(2 * math.pi * u * 2.3 / wavelength + phase * 1.7)
                          + 0.10 * math.sin(2 * math.pi * u * 5.1 / wavelength + phase * 0.4))
        pts.append((x, y * h))
    pts += [(w, h), (0, h)]
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).polygon(pts, fill=255)
    return m


def render(name: str) -> Image.Image:
    pal = PALETTES[name]
    w, h = W * SS, H * SS
    img = gradient(pal["sky"], w, h)

    sun_rgb, sun_strength, (sx, sy) = pal["sun"]
    glow = Image.new("L", (w, h), 0)
    r = int(0.30 * h)
    ImageDraw.Draw(glow).ellipse((sx * w - r, sy * h - r, sx * w + r, sy * h + r), fill=int(255 * sun_strength))
    glow = glow.filter(ImageFilter.GaussianBlur(0.22 * h))
    img = Image.composite(Image.new("RGB", (w, h), sun_rgb), img, glow)

    for base, tilt, amp, wl, ph, (c_top, c_bot), opacity, blur in pal["waves"]:
        mask = wave_mask(base, tilt, amp, wl, ph, w, h)
        top = max(0, int((base - abs(tilt) / 2 - amp * 1.1) * h))
        layer = Image.new("RGB", (w, h), c_bot)
        grad = gradient([(0.0, c_top), (1.0, c_bot)], w, max(2, h - top))
        layer.paste(grad, (0, top))
        # A brighter crest highlight along the top edge of each wave.
        edge = ImageChops.subtract(mask, mask.filter(ImageFilter.GaussianBlur(10 * SS)))
        mask = mask.filter(ImageFilter.GaussianBlur(blur * SS))
        mask = mask.point(lambda v, o=opacity: int(v * o))
        img = Image.composite(layer, img, mask)
        # The crest highlight is blurred at least as much as its layer so a
        # soft, distant wave never gets a hard bright line.
        hl = edge.filter(ImageFilter.GaussianBlur(max(3, blur) * SS)).point(lambda v, o=opacity: int(v * 0.30 * o))
        img = Image.composite(Image.new("RGB", (w, h), (255, 255, 255)), img, hl)

    img = img.filter(ImageFilter.GaussianBlur(1.2 * SS)).resize((W, H), Image.LANCZOS)
    # Deterministic +/-1 dither against banding in smooth gradients.
    rng = random.Random(0x4D494E54)
    noise = Image.frombytes("L", (W, H), bytes(rng.randrange(126, 131) for _ in range(W * H)))
    img = ImageChops.add(img, Image.merge("RGB", (noise, noise, noise)), 1.0, -128)
    return img


def main() -> int:
    out = ROOT / "assets"
    for name, stem in (("light", "wallpaper-1920x1080"), ("dark", "wallpaper-dark-1920x1080")):
        img = render(name)
        img.save(out / f"{stem}.png", optimize=True)
        (out / f"{stem}.rgb").write_bytes(img.tobytes())
        print(f"wrote assets/{stem}.png and .rgb ({img.width}x{img.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
