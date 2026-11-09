#!/usr/bin/env python3
"""Generate MintDE's original application icons and the menu-bar leaf logo.

All artwork is drawn procedurally here (no third-party images): a superellipse
"squircle" tile with a vertical gradient, a soft drop shadow, and a simple
glyph per app. Outputs per icon:
  assets/icons/<name>.png         256 px master (shipped on the volumes)
  assets/icons/<name>-128.rgba    raw RGBA for MintDE (Launchpad, About, large)
  assets/icons/<name>-64.rgba     raw RGBA for MintDE (Dock, lists)
and assets/icons/mint-leaf-32.a8 (alpha mask for the menu-bar logo).
Requires Pillow; outputs are committed.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets/icons"
N = 1024          # working canvas
TILE = 824        # squircle size within canvas (macOS-style margin)


def squircle_points(cx, cy, size, n=5.0, steps=720):
    r = size / 2
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        x = r * math.copysign(abs(c) ** (2 / n), c)
        y = r * math.copysign(abs(s) ** (2 / n), s)
        pts.append((cx + x, cy + y))
    return pts


def vgrad(size, top, bottom):
    col = Image.new("RGBA", (1, size[1]))
    for y in range(size[1]):
        t = y / max(1, size[1] - 1)
        col.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return col.resize(size, Image.BILINEAR)


def tile(top, bottom, glyph_fn, *, plate=None):
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    cx = cy = N / 2
    mask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(mask).polygon(squircle_points(cx, cy, TILE), fill=255)
    shadow = mask.filter(ImageFilter.GaussianBlur(22))
    shadow = ImageChops.offset(shadow, 0, 18).point(lambda v: int(v * 0.42))
    img.paste((0, 0, 0, 255), (0, 0), shadow)
    img.putalpha(shadow)
    face = vgrad((N, N), top, bottom)
    img = Image.composite(face, img, mask)
    # Subtle top sheen and inner edge for depth.
    sheen = Image.new("L", (N, N), 0)
    ImageDraw.Draw(sheen).ellipse((cx - TILE * 0.9, cy - TILE * 1.32, cx + TILE * 0.9, cy - TILE * 0.05), fill=46)
    sheen = ImageChops.multiply(sheen.filter(ImageFilter.GaussianBlur(40)), mask)
    img = Image.composite(Image.new("RGBA", (N, N), (255, 255, 255, 255)), img, sheen)
    layer = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    glyph_fn(ImageDraw.Draw(layer), layer, cx, cy)
    layer.putalpha(ImageChops.multiply(layer.split()[3], mask))  # clip glyphs to the tile
    img.alpha_composite(layer)
    edge = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(5)))
    img = Image.composite(Image.new("RGBA", (N, N), (0, 0, 0, 60)), img, edge.point(lambda v: v // 3))
    return img


def rr(d, box, r, fill):
    d.rounded_rectangle(box, r, fill=fill)


# ---------------------------------------------------------------- glyphs
def g_files(d, layer, cx, cy):
    rr(d, (cx - 300, cy - 190, cx - 40, cy - 90), 40, (186, 226, 255, 255))
    rr(d, (cx - 300, cy - 150, cx + 300, cy + 230), 46, (172, 218, 255, 255))
    rr(d, (cx - 300, cy - 90, cx + 300, cy + 230), 46, (226, 243, 255, 255))
    d.rectangle((cx - 300, cy - 60, cx + 300, cy - 50), fill=(150, 200, 245, 255))


def g_notes(d, layer, cx, cy):
    d.rectangle((0, 0, N, cy - 210), fill=(255, 205, 64, 255))
    d.rectangle((0, cy - 214, N, cy - 196), fill=(230, 176, 40, 255))
    for i in range(6):
        y = cy - 120 + i * 70
        d.line((cx - 300, y, cx + 300, y), fill=(206, 206, 210, 255), width=8)


def g_terminal(d, layer, cx, cy):
    d.line((cx - 270, cy - 150, cx - 120, cy - 30), fill=(240, 240, 244, 255), width=46)
    d.line((cx - 120, cy - 30, cx - 270, cy + 90), fill=(240, 240, 244, 255), width=46)
    rr(d, (cx - 60, cy + 70, cx + 180, cy + 112), 14, (240, 240, 244, 255))


def gear(d, cx, cy, r_out, r_in, teeth, color, hole):
    for i in range(teeth):
        a = 2 * math.pi * i / teeth
        w = math.pi / teeth * 0.55
        pts = [(cx + r_in * math.cos(a - w * 1.6), cy + r_in * math.sin(a - w * 1.6)),
               (cx + r_out * math.cos(a - w), cy + r_out * math.sin(a - w)),
               (cx + r_out * math.cos(a + w), cy + r_out * math.sin(a + w)),
               (cx + r_in * math.cos(a + w * 1.6), cy + r_in * math.sin(a + w * 1.6))]
        d.polygon(pts, fill=color)
    d.ellipse((cx - r_in, cy - r_in, cx + r_in, cy + r_in), fill=color)
    d.ellipse((cx - hole, cy - hole, cx + hole, cy + hole), fill=(0, 0, 0, 0))


def g_settings(d, layer, cx, cy):
    gear(d, cx, cy, 330, 262, 14, (72, 74, 82, 255), 96)
    d.ellipse((cx - 150, cy - 150, cx + 150, cy + 150), outline=(110, 112, 120, 255), width=34)


def g_monitor(d, layer, cx, cy):
    rr(d, (cx - 310, cy - 230, cx + 310, cy + 230), 40, (20, 22, 26, 255))
    for i in range(1, 4):
        d.line((cx - 290, cy - 230 + i * 115, cx + 290, cy - 230 + i * 115), fill=(50, 56, 60, 255), width=4)
    pts = [(cx - 290, cy + 100), (cx - 190, cy + 60), (cx - 120, cy + 130), (cx - 30, cy - 120),
           (cx + 40, cy + 40), (cx + 120, cy - 40), (cx + 200, cy + 10), (cx + 290, cy - 150)]
    d.line(pts, fill=(64, 232, 120, 255), width=26, joint="curve")


def g_console(d, layer, cx, cy):
    rr(d, (cx - 310, cy - 250, cx + 310, cy + 250), 40, (248, 248, 250, 255))
    for i, w in enumerate((520, 420, 470, 300, 440)):
        y = cy - 170 + i * 72
        d.line((cx - 250, y, cx - 250 + w, y), fill=(120, 124, 132, 255), width=22)
    d.polygon([(cx + 170, cy + 40), (cx + 300, cy + 250), (cx + 40, cy + 250)], fill=(255, 196, 40, 255))
    d.rectangle((cx + 160, cy + 110, cx + 180, cy + 190), fill=(60, 50, 20, 255))
    d.ellipse((cx + 158, cy + 205, cx + 182, cy + 229), fill=(60, 50, 20, 255))


def g_sysinfo(d, layer, cx, cy):
    rr(d, (cx - 200, cy - 200, cx + 200, cy + 200), 40, (40, 48, 60, 255))
    rr(d, (cx - 120, cy - 120, cx + 120, cy + 120), 22, (110, 190, 255, 255))
    for i in range(5):
        o = -160 + i * 80
        for (x0, y0, x1, y1) in ((cx + o - 14, cy - 290, cx + o + 14, cy - 200), (cx + o - 14, cy + 200, cx + o + 14, cy + 290),
                                 (cx - 290, cy + o - 14, cx - 200, cy + o + 14), (cx + 200, cy + o - 14, cx + 290, cy + o + 14)):
            rr(d, (x0, y0, x1, y1), 8, (210, 216, 226, 255))


def g_network(d, layer, cx, cy):
    r = 300
    d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(255, 255, 255, 255), width=34)
    d.ellipse((cx - 130, cy - r, cx + 130, cy + r), outline=(255, 255, 255, 255), width=28)
    d.line((cx, cy - r, cx, cy + r), fill=(255, 255, 255, 255), width=28)
    for y in (-150, 0, 150):
        half = int(math.sqrt(max(0, r * r - y * y)))
        d.line((cx - half + 20, cy + y, cx + half - 20, cy + y), fill=(255, 255, 255, 255), width=26)


def g_web(d, layer, cx, cy):
    """Web: a planet with meridians, ringed by a mint orbit carrying a leaf."""
    r = 270
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(40, 132, 246, 255))
    d.ellipse((cx - r + 40, cy - r + 30, cx + r - 120, cy + r - 160), fill=(78, 160, 255, 255))
    w = 20
    d.ellipse((cx - 110, cy - r, cx + 110, cy + r), outline=(255, 255, 255, 215), width=w)
    d.line((cx, cy - r, cx, cy + r), fill=(255, 255, 255, 215), width=w)
    for y in (-135, 0, 135):
        half = int(math.sqrt(max(0, r * r - y * y)))
        d.line((cx - half + 12, cy + y, cx + half - 12, cy + y), fill=(255, 255, 255, 215), width=w)
    ring = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    rd.ellipse((cx - 390, cy - 110, cx + 390, cy + 110), outline=(70, 226, 168, 255), width=34)
    ring = ring.rotate(24, center=(cx, cy), resample=Image.BICUBIC)
    layer.alpha_composite(ring)
    d2 = ImageDraw.Draw(layer)
    lx, ly = cx + 300, cy - 190
    d2.polygon(leaf_polygon(lx, ly, 120), fill=(70, 226, 168, 255))


def g_disks(d, layer, cx, cy):
    rr(d, (cx - 300, cy - 200, cx + 300, cy + 200), 60, (226, 230, 236, 255))
    rr(d, (cx - 300, cy + 60, cx + 300, cy + 200), 60, (190, 196, 206, 255))
    d.rectangle((cx - 300, cy + 60, cx + 300, cy + 110), fill=(190, 196, 206, 255))
    d.ellipse((cx + 190, cy + 110, cx + 240, cy + 160), fill=(64, 210, 120, 255))
    d.line([(cx - 230, cy - 60), (cx - 120, cy - 60), (cx - 70, cy - 140), (cx, cy + 10), (cx + 60, cy - 90),
            (cx + 100, cy - 60), (cx + 230, cy - 60)], fill=(232, 72, 80, 255), width=26, joint="curve")


def leaf_polygon(cx, cy, s):
    pts = []
    for i in range(181):
        t = math.pi * i / 180
        x = math.sin(t) * 0.52
        y = -math.cos(t)
        pts.append((cx + (x - 0.06 * y) * s, cy + y * s))
    for i in range(180, -1, -1):
        t = math.pi * i / 180
        x = -math.sin(t) * 0.42
        y = -math.cos(t)
        pts.append((cx + (x - 0.06 * y) * s, cy + y * s))
    return pts


def g_leaf(d, layer, cx, cy, color=(255, 255, 255, 255), s=290):
    sub = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sub)
    sd.polygon(leaf_polygon(cx, cy, s), fill=color)
    sd.line((cx - 0.04 * s, cy + 0.95 * s, cx + 0.02 * s, cy - 0.7 * s), fill=(0, 0, 0, 0), width=int(s * 0.07))
    sub = sub.rotate(-28, resample=Image.BICUBIC, center=(cx, cy))
    layer.alpha_composite(sub)


def g_about_de(d, layer, cx, cy):
    rr(d, (cx - 300, cy - 220, cx + 300, cy + 220), 48, (255, 255, 255, 255))
    rr(d, (cx - 300, cy - 220, cx + 300, cy - 120), 48, (232, 234, 240, 255))
    d.rectangle((cx - 300, cy - 150, cx + 300, cy - 120), fill=(232, 234, 240, 255))
    for i, c in enumerate(((255, 95, 87), (254, 188, 46), (40, 200, 64))):
        d.ellipse((cx - 260 + i * 70, cy - 196, cx - 214 + i * 70, cy - 150), fill=c + (255,))
    for i in range(3):
        y = cy - 60 + i * 80
        d.line((cx - 230, y, cx + (230 if i < 2 else 60), y), fill=(150, 160, 190, 255), width=30)


def g_launchpad(d, layer, cx, cy):
    colors = [(255, 95, 87), (254, 188, 46), (40, 200, 64), (90, 200, 250), (0, 122, 255), (175, 82, 222),
              (255, 45, 85), (255, 149, 0), (52, 199, 89)]
    for i, c in enumerate(colors):
        x = cx - 220 + (i % 3) * 220
        y = cy - 220 + (i // 3) * 220
        rr(d, (x - 78, y - 78, x + 78, y + 78), 40, c + (255,))


def g_calculator(d, layer, cx, cy):
    rr(d, (cx - 280, cy - 300, cx + 280, cy - 150), 30, (90, 96, 104, 255))
    for r in range(3):
        for c in range(4):
            x = cx - 210 + c * 140
            y = cy - 50 + r * 130
            col = (255, 159, 10, 255) if c == 3 else ((200, 200, 204, 255) if r == 0 else (100, 104, 110, 255))
            d.ellipse((x - 52, y - 52, x + 52, y + 52), fill=col)


def g_trash(d, layer, cx, cy):
    pass


def trash_icon():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = N / 2
    body = [(cx - 270, cy - 250), (cx + 270, cy - 250), (cx + 220, cy + 370), (cx - 220, cy + 370)]
    shadow = Image.new("L", (N, N), 0)
    ImageDraw.Draw(shadow).polygon(body, fill=255)
    shadow = ImageChops.offset(shadow.filter(ImageFilter.GaussianBlur(20)), 0, 16).point(lambda v: int(v * 0.35))
    img.putalpha(shadow)
    d.polygon(body, fill=(228, 232, 238, 235))
    for i in range(7):
        x = cx - 200 + i * 66
        d.line((x, cy - 210, x + (i - 3) * -10, cy + 330), fill=(176, 184, 196, 255), width=14)
    rr(d, (cx - 310, cy - 330, cx + 310, cy - 250), 30, (206, 212, 220, 255))
    rr(d, (cx - 80, cy - 380, cx + 80, cy - 330), 20, (190, 196, 206, 255))
    return img


ICONS = {
    "files":         ((84, 178, 255), (24, 112, 232), g_files),
    "notes":         ((255, 255, 255), (238, 238, 240), g_notes),
    "terminal":      ((70, 72, 78), (28, 28, 32), g_terminal),
    "settings":      ((214, 216, 222), (150, 154, 162), g_settings),
    "system-monitor": ((70, 74, 80), (34, 36, 40), g_monitor),
    "console":       ((96, 100, 108), (52, 54, 60), g_console),
    "system-info":   ((120, 140, 168), (60, 76, 104), g_sysinfo),
    "network":       ((92, 172, 255), (30, 96, 220), g_network),
    "disk-utility":  ((240, 242, 246), (190, 196, 206), g_disks),
    "about-mintos":  ((88, 222, 178), (16, 150, 122), lambda d, l, x, y: g_leaf(d, l, x, y)),
    "mintde":        ((150, 140, 255), (88, 64, 220), g_about_de),
    "launchpad":     ((60, 62, 70), (24, 24, 28), g_launchpad),
    "calculator":    ((70, 72, 78), (30, 30, 34), g_calculator),
    "web":           ((255, 255, 255), (226, 232, 240), g_web),
}


def save(name, img):
    master = img.resize((256, 256), Image.LANCZOS)
    master.save(OUT / f"{name}.png", optimize=True)
    for px in (128, 64):
        (OUT / f"{name}-{px}.rgba").write_bytes(img.resize((px, px), Image.LANCZOS).tobytes())


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, (top, bottom, fn) in ICONS.items():
        save(name, tile(top, bottom, fn))
    save("trash", trash_icon())
    leaf = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    g_leaf(ImageDraw.Draw(leaf), leaf, N / 2, N / 2, (0, 0, 0, 255), s=440)
    a = leaf.split()[3].resize((32, 32), Image.LANCZOS)
    (OUT / "mint-leaf-32.a8").write_bytes(a.tobytes())
    if len(sys.argv) > 1:
        names = list(ICONS) + ["trash"]
        sheet = Image.new("RGBA", (128 * 7, 128 * 2 + 40), (200, 210, 222, 255))
        for i, n in enumerate(names):
            im = Image.open(OUT / f"{n}.png").resize((128, 128), Image.LANCZOS)
            sheet.alpha_composite(im, ((i % 7) * 128, (i // 7) * 128))
        sheet.paste(Image.merge("RGBA", (a, a, a, a)).resize((32, 32)), (8, 256))
        sheet.save(sys.argv[1])
    print(f"wrote {len(ICONS) + 1} icons and the menu-bar leaf mask")
    return 0


if __name__ == "__main__":
    sys.exit(main())
