#!/usr/bin/env python3
"""Host tests for the MintOS Web engine (kernel/user/web).

Builds tools/test_web_core.c with the engine sources and checks:
  * URL resolution, requests, form encoding, dates, cookies and the heap (built-in unit tests);
  * inflate against Python's zlib (raw deflate, zlib, gzip, every level and strategy,
    stored blocks, multi-member gzip) plus corrupt, truncated and oversized streams;
  * the incremental HTTP/1.1 response parser fed in random pieces (Content-Length,
    chunked with extensions and trailers, until-close, 1xx, gzip + chunked, Set-Cookie).
"""
import gzip
import os
import random
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "kernel/user/web"
failures = 0


def check(ok, name, detail=""):
    global failures
    print(("PASS: " if ok else "FAIL: ") + name + (f"  [{detail}]" if detail and not ok else ""))
    if not ok:
        failures += 1


def build(tmp, harness, sources, extra=()):
    exe = Path(tmp) / Path(harness).stem
    cmd = [os.environ.get("CC", "clang"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *extra, "-o", str(exe),
           str(ROOT / harness)] + [str(ROOT / s) for s in sources]
    subprocess.run(cmd, check=True)
    return exe


def sample_texts(rng):
    words = ("MintOS web engine inflate deflate gzip chunked <div class=\"x\">&amp; "
             "Praha říčání žluťoučký kůň").split()
    yield b""
    yield b"a"
    yield bytes(range(256)) * 40
    yield (" ".join(rng.choice(words) for _ in range(30000))).encode()
    yield bytes(rng.getrandbits(8) for _ in range(70000))                 # incompressible -> stored blocks
    yield b"\x00" * 300000                                               # long matches, distance 1
    html = (ROOT / "README.md").read_bytes()
    yield html


def png(w, h, rgba, color_type=6, bit_depth=8, palette=None, interlace=0):
    def chunk(t, d):
        return len(d).to_bytes(4, "big") + t + d + zlib.crc32(t + d).to_bytes(4, "big")
    raw = bytearray()
    bpp = {6: 4, 2: 3, 0: 1, 3: 1, 4: 2}[color_type]
    rows = [rgba[y * w * bpp:(y + 1) * w * bpp] for y in range(h)]
    prev = bytes(w * bpp)
    for y, row in enumerate(rows):
        f = y % 5                                   # exercise every PNG filter
        out = bytearray()
        for i, v in enumerate(row):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 0: pred = 0
            elif f == 1: pred = a
            elif f == 2: pred = b
            elif f == 3: pred = (a + b) // 2
            else:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            out.append((v - pred) & 255)
        raw += bytes([f]) + out
        prev = row
    ihdr = w.to_bytes(4, "big") + h.to_bytes(4, "big") + bytes([bit_depth, color_type, 0, 0, interlace])
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
    if palette:
        data += chunk(b"PLTE", palette)
    return data + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")


def gif(w, h, indices, palette):
    """GIF with 'uncompressed' LZW: a clear code before the table grows, so every code is a literal."""
    bits = 8
    clear, end = 1 << bits, (1 << bits) + 1
    codes, out = [], bytearray()
    for i, v in enumerate(indices):
        if i % 250 == 0:
            codes.append(clear)
        codes.append(v)
    codes.append(end)
    acc = nbits = 0
    for c in codes:
        acc |= c << nbits; nbits += bits + 1
        while nbits >= 8:
            out.append(acc & 255); acc >>= 8; nbits -= 8
    if nbits:
        out.append(acc & 255)
    blocks = b"".join(bytes([len(out[i:i + 255])]) + out[i:i + 255] for i in range(0, len(out), 255))
    hdr = b"GIF89a" + w.to_bytes(2, "little") + h.to_bytes(2, "little") + bytes([0xF7, 0, 0]) + palette
    img = b"\x2c" + bytes(4) + w.to_bytes(2, "little") + h.to_bytes(2, "little") + b"\x00" + bytes([bits])
    return hdr + img + blocks + b"\x00\x3b"


def bmp(w, h, rgb, bits=24):
    stride = (w * bits // 8 + 3) & ~3
    pix = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b = rgb[(y * w + x) * 3:(y * w + x) * 3 + 3]
            row += bytes([b, g, r]) + (b"\xff" if bits == 32 else b"")
        pix += row + bytes(stride - len(row))
    info = (40).to_bytes(4, "little") + w.to_bytes(4, "little") + h.to_bytes(4, "little", signed=True) + (1).to_bytes(2, "little") + \
        bits.to_bytes(2, "little") + bytes(4) + len(pix).to_bytes(4, "little") + bytes(16)
    return b"BM" + (54 + len(pix)).to_bytes(4, "little") + bytes(4) + (54).to_bytes(4, "little") + info + bytes(pix)


def read_ppm(path):
    data = Path(path).read_bytes()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j]); i = j
    w, h = int(parts[1]), int(parts[2])
    pix = data[i + 1:]
    ch = 3 if parts[0] == b"P6" else 1
    return w, h, [tuple(pix[k * ch:(k + 1) * ch]) * (3 if ch == 1 else 1) for k in range(w * h)]


def images(tmp, core, rng):
    def decode(blob):
        src, dst = tmp / "img.bin", tmp / "img.out"
        src.write_bytes(blob)
        if dst.exists():
            dst.unlink()
        r = subprocess.run([str(core), "image", str(src), str(dst)], capture_output=True, text=True)
        if not dst.exists():
            return r.stdout.strip(), None, None, None, r.returncode
        raw = dst.read_bytes()
        nl = raw.index(b"\n")
        w, h = map(int, raw[:nl].split())
        return r.stdout.strip(), w, h, raw[nl + 1:], r.returncode

    w, h = 37, 23
    rgba = bytes(rng.getrandbits(8) for _ in range(w * h * 4))
    st, dw, dh, px, rc = decode(png(w, h, rgba))
    check(st == f"OK PNG {w}x{h}" and px == rgba and rc == 0, "PNG RGBA (all five filters) decodes pixel-exact", st)
    rgb = bytes(rng.getrandbits(8) for _ in range(w * h * 3))
    st, _, _, px, _ = decode(png(w, h, rgb, color_type=2))
    want = b"".join(rgb[i * 3:i * 3 + 3] + b"\xff" for i in range(w * h))
    check(px == want, "PNG RGB decodes pixel-exact with opaque alpha", st)
    pal = bytes(rng.getrandbits(8) for _ in range(256 * 3))
    idx = bytes(rng.randrange(256) for _ in range(w * h))
    st, _, _, px, _ = decode(png(w, h, idx, color_type=3, palette=pal))
    want = b"".join(pal[i * 3:i * 3 + 3] + b"\xff" for i in idx)
    check(px == want, "PNG palette image decodes pixel-exact", st)
    gray = bytes(rng.getrandbits(8) for _ in range(w * h))
    st, _, _, px, _ = decode(png(w, h, gray, color_type=0))
    check(px == b"".join(bytes([g, g, g, 255]) for g in gray), "PNG greyscale decodes pixel-exact", st)
    st, _, _, px, _ = decode(gif(w, h, list(idx), pal))
    check(st == f"OK GIF {w}x{h}" and px == want, "GIF (first frame, LZW) decodes pixel-exact", st)
    st, _, _, px, _ = decode(bmp(w, h, rgb))
    want = b"".join(rgb[i * 3:i * 3 + 3] + b"\xff" for i in range(w * h))
    check(st == f"OK BMP {w}x{h}" and px == want, "BMP 24-bit decodes pixel-exact", st)
    st, _, _, px, _ = decode(bmp(w, h, rgb, 32))
    check(st == f"OK BMP {w}x{h}" and px is not None and all(px[i * 4:i * 4 + 3] == rgb[i * 3:i * 3 + 3] for i in range(w * h)),
          "BMP 32-bit decodes with the right colours", st)
    fx = ROOT / "tools/fixtures/web"
    for name, ref in (("baseline.jpg", "baseline.ref.ppm"), ("progressive.jpg", "progressive.ref.ppm"), ("gray.jpg", "gray.ref.pgm")):
        st, jw, jh, px, _ = decode((fx / name).read_bytes())
        rw, rh, refpx = read_ppm(fx / ref)
        if px is None or (jw, jh) != (rw, rh):
            check(False, f"JPEG {name} decodes", st)
            continue
        diffs = [abs(px[k * 4 + c] - refpx[k][c]) for k in range(rw * rh) for c in range(3)]
        mean = sum(diffs) / len(diffs)
        check(mean < 1.0 and max(diffs) <= 8, f"JPEG {name} matches libjpeg-turbo within rounding (mean {mean:.2f}, max {max(diffs)})")
    st, *_ = decode(b"RIFF\x10\x00\x00\x00WEBPVP8 ")
    check(st == "ERR image format not supported (WebP)", "WebP is recognised and reported as unsupported", st)
    st, *_ = decode(b'<?xml version="1.0"?><svg xmlns="http://www.w3.org/2000/svg"/>')
    check(st == "ERR image format not supported (SVG)", "SVG is recognised and reported as unsupported", st)
    hdr = b"\x89PNG\r\n\x1a\n" + (13).to_bytes(4, "big") + b"IHDR" + (5000).to_bytes(4, "big") + (5000).to_bytes(4, "big") + bytes([8, 6, 0, 0, 0])
    hdr += zlib.crc32(hdr[12:29]).to_bytes(4, "big")
    st, *_ = decode(hdr + (100).to_bytes(4, "big") + b"IDAT" + bytes(104))
    check(st.startswith("ERR image too large"), "images over the pixel budget are refused before decoding", st)
    fuzz_ok = True
    base = png(w, h, rgba)
    for k in range(200):
        blob = bytearray(rng.choice([base, (fx / "baseline.jpg").read_bytes(), (fx / "progressive.jpg").read_bytes(), gif(w, h, list(idx), pal)]))
        for _ in range(rng.randint(1, 6)):
            blob[rng.randrange(len(blob))] = rng.getrandbits(8)
        st, *_rest, rc = decode(bytes(blob))
        if rc != 0:
            fuzz_ok = False
            break
    check(fuzz_ok, "200 corrupted PNG/JPEG/GIF files decode or fail without crashing or leaking heap", st)
    # resampling: a 2x2 checker shrunk to 1x1 averages; a solid image enlarged stays solid
    src, dst = tmp / "s.png", tmp / "s.out"
    src.write_bytes(png(2, 2, bytes([255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255, 255, 0, 0, 255])))
    subprocess.run([str(core), "scale", str(src), "1", "1", str(dst)], check=True, capture_output=True)
    raw = dst.read_bytes(); px = raw[raw.index(b"\n") + 1:]
    check(abs(px[2] - 128) <= 1 and px[1] == 0 and abs(px[0] - 128) <= 1 and px[3] == 255, "area averaging when shrinking", px.hex())
    src.write_bytes(png(3, 3, bytes([10, 200, 30, 128] * 9)))
    subprocess.run([str(core), "scale", str(src), "17", "11", str(dst)], check=True, capture_output=True)
    raw = dst.read_bytes(); px = raw[raw.index(b"\n") + 1:]
    check(len(px) == 17 * 11 * 4 and all(px[i:i + 4] == px[0:4] for i in range(0, len(px), 4)) and px[3] == 128 and abs(px[1] - 100) <= 1,
          "bilinear enlarging keeps a flat colour flat, output premultiplied", px[:4].hex())


def layout(tmpd):
    exe = build(tmpd, "tools/test_web_layout.c", [f"kernel/user/web/{n}.c" for n in ("wmem", "whttp", "inflate", "whtml", "wcss", "wlayout")] +
                ["kernel/user/mintde/font_data.c"])
    fx = ROOT / "tools/fixtures/web"
    def run(html, width=1000, *css):
        r = subprocess.run([str(exe), str(html), str(width), *[str(c) for c in css]], capture_output=True, text=True)
        items = []
        for line in r.stdout.splitlines():
            parts = line.split(" ")
            if parts[0] in ("TEXT", "RECT", "IMAGE", "CTL", "BULLET", "BORDER"):
                d = {"kind": parts[0]}
                for p in parts[1:]:
                    if "=" in p and not p.startswith("text="):
                        k, v = p.split("=", 1)
                        d[k] = v
                if 'text="' in line:
                    d["text"] = line.split('text="', 1)[1][:-1]
                items.append(d)
        return r, items
    r, items = run(fx / "article.html", 1000, fx / "site.css")
    out = r.stdout
    texts = [i for i in items if i["kind"] == "TEXT"]
    alltext = " ".join(i["text"] for i in texts)
    check(r.returncode == 0 and "HEAP used=0" in out and "ok=1" in out, "article fixture: parse, style and layout without leaks", r.stderr[-300:])
    check("TITLE Prague \u2013 MintOS test article" in out, "title with entities", out.splitlines()[0] if out else "")
    check("P\u0159\u00edklad \u017elu\u0165ou\u010dk\u00fd k\u016f\u0148" in alltext and "\u2014 diacritics & entities." in alltext,
          "UTF-8 and named/numeric entities reach the text")
    hidden = ["Jump to content", "Screen reader only text", "Wikipedia logo text", "Mobile only", "script output", "Note:"]
    check(not any(h in alltext for h in hidden), "display:none, visually-hidden, image-replacement, scripts and ::before are not shown",
          [h for h in hidden if h in alltext])
    check("JavaScript is disabled" in alltext, "<noscript> content is shown (scripts never run)")
    infobox = [i for i in items if i["kind"] == "RECT" and i.get("color") == "fff8f9fa" and int(i["w"]) == 240]
    first_para = [i for i in texts if i["text"].startswith(") is the capital")]
    check(infobox and int(infobox[0]["x"]) + 240 == 950 and first_para and int(first_para[0]["x"]) + int(first_para[0]["w"]) < int(infobox[0]["x"]),
          "float:right infobox sits at the right edge and paragraph lines wrap beside it")
    check(any(i["kind"] == "IMAGE" and i["w"] == "220" and i["h"] == "147" for i in items), "<img width/height> becomes a 220x147 image box")
    bullets = [i for i in items if i["kind"] == "BULLET"]
    check(len(bullets) == 3 and any(t["text"] == "3." for t in texts) and any(t["text"] == "4." for t in texts),
          "list markers: discs/circles for <ul>, numbers from <ol start=3>")
    check(any(t["text"] == "Climate" and t["font"] == "6" for t in texts) and sum(1 for i in items if i["kind"] == "BORDER" and i["color"] == "ffa2a9b1") >= 8,
          "table with caption, header cells (bold) and collapsed cell borders")
    snow = [t for t in texts if t["text"] == "Snow days"]
    mean = [t for t in texts if t["text"].startswith("Mean")]
    check(snow and mean and snow[0]["x"] == mean[0]["x"], "colspan cells align with the column grid")
    check(any(t["text"].startswith("    printf") and int(t["flags"]) & 8 for t in texts), "<pre> keeps spaces and uses the monospace face")
    link_ids = {t["link"] for t in texts if t["link"] != "-1"}
    check(len(link_ids) >= 6 and all(t["color"] == "ff3366cc" for t in texts if t["link"] in ("3", "4", "5")),
          "links carry their index and the CSS var() colour")
    ctl = [l for l in out.splitlines() if l.startswith("CONTROL")]
    check(len(ctl) == 5 and "type=3 name=search" in ctl[1] and "opts=2 sel=1" in ctl[4] and 'value="Special:Search"' in ctl[0],
          "form controls: hidden, search, submit, checkbox and select (selected option)", ctl)
    footer = [t for t in texts if t["text"].startswith("This page was last edited")]
    check(footer and int(footer[0]["y"]) > max(int(i["y"]) + int(i["h"]) for i in infobox), "clear:both footer goes below the float")
    for width in (320, 600, 1600):
        r2, items2 = run(fx / "article.html", width, fx / "site.css")
        maxx = max((int(i["x"]) + int(i["w"]) for i in items2 if i["kind"] == "TEXT"), default=0)
        check(r2.returncode == 0 and maxx <= width, f"layout at {width}px keeps text inside the viewport", maxx)
    quirks = Path(tmpd) / "q.html"
    quirks.write_text("<html><center><table width=80%><tr><td>left</td></tr></table></center>")
    _, qi = run(quirks, 800)
    std = Path(tmpd) / "s.html"
    std.write_text("<!doctype html><center><table width=80%><tr><td>mid</td></tr></table></center>")
    _, si = run(std, 800)
    check(qi and si and int(qi[0]["x"]) < 120 and int(si[0]["x"]) > 300, "quirks mode: tables do not inherit <center> alignment")
    stress = Path(tmpd) / "stress.html"
    stress.write_text("<div>" * 3000 + "deep" + "</div>" * 3000 + "<table>" * 200 + "<tr><td>x" * 50 + "<p>" + "<b><i>" * 500 +
                      "<style>a{color:red}" + "x{" * 200 + "@media (min-width:1px){" * 50 + "</style>" + "<ul><li>" * 300 + "&#xFFFFFFFF;&bogus;<<<>>>")
    r3, _ = run(stress, 900)
    check(r3.returncode == 0 and "HEAP used=0" in r3.stdout, "deeply nested and malformed HTML/CSS lays out without crashing or leaking", r3.stderr[-200:])


def contracts():
    web = (ROOT / "kernel/user/mintde/web.c").read_text()
    css = (WEB / "wcss.c").read_text()
    lay = (WEB / "wlayout.c").read_text()
    html = (WEB / "whtml.c").read_text()
    checks = {
        "no script is ever executed (no JavaScript engine, <script> text is discarded)":
            "WT_SCRIPT" in html and "never used" in html and "eval" not in web,
        "certificate failures stop the load with no way to continue":
            "This connection is not private" in web and "proceed" not in web.lower().replace("proceeding", ""),
        "every network size is bounded": all(k in web for k in ["DOC_MAX", "CSS_MAX", "IMG_MAX", "IMG_PIXELS_MAX", "WEB_HEAP_BYTES"]),
        "downloads are written only below ~/Downloads with sanitised, unique names":
            'DOWNLOAD_DIR "/"' in web and "whttp_disposition" in web and "mint_path_stat(d->path, &st)" in web,
        "cookies and favorites persist on MintOS HD only when it is present":
            "home_available()" in web and "COOKIE_FILE" in web and "BOOKMARK_FILE" in web,
        "layout recursion is bounded (64 KiB ring-3 stack)": "MAX_DEPTH" in lay and "L->depth >= MAX_DEPTH" in lay,
        "HTML nesting and node counts are bounded": "MAX_DEPTH" in html and "MAX_NODES" in html,
        "dynamic pseudo-classes never match and unknown CSS is skipped": "dynamic states never match" in css and "unknown_props" in css,
        "images are fetched lazily near the viewport and decoded within a memory budget":
            "request_near_images" in web and "keep 1 MiB free" in web,
    }
    for name, ok in checks.items():
        check(ok, "contract: " + name)


def freestanding(tmp):
    flags = "-target x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-builtin -mno-red-zone " \
            "-mno-sse -mno-mmx -mgeneral-regs-only -mcmodel=large -Wall -Wextra -Werror -O2".split()
    objdump = next((p for p in ("llvm-objdump", "/opt/homebrew/opt/llvm/bin/llvm-objdump", "objdump") if subprocess.run(["which", p], capture_output=True).returncode == 0 or Path(p).exists()), None)
    bad = []
    for src in sorted(WEB.glob("*.c")):
        obj = tmp / (src.stem + ".o")
        r = subprocess.run([os.environ.get("CC", "clang"), *flags, f"-I{ROOT}/kernel/include", f"-I{ROOT}/kernel/user/include",
                            f"-I{WEB}/shim", "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        if r.returncode:
            bad.append(f"{src.name}: {r.stderr.strip()[:200]}")
            continue
        if objdump:
            dis = subprocess.run([objdump, "-d", str(obj)], capture_output=True, text=True).stdout
            fp = [l for l in dis.splitlines() if any(k in l for k in ("xmm", "ymm", "%st", "cvtsi", "fld", "fstp", "mulss", "addsd"))]
            if fp:
                bad.append(f"{src.name}: FP instruction {fp[0].strip()}")
    check(not bad, f"every Web engine file (incl. stb_image) builds for ring 3 with -mgeneral-regs-only and has no SSE/x87 code", "; ".join(bad))


def main():
    rng = random.Random(26514)
    with tempfile.TemporaryDirectory(prefix="mint-web-") as tmpd:
        tmp = Path(tmpd)
        core = build(tmpd, "tools/test_web_core.c", ["kernel/user/web/wmem.c", "kernel/user/web/inflate.c", "kernel/user/web/whttp.c", "kernel/user/web/wimage.c"])
        out = subprocess.run([str(core), "unit"], capture_output=True, text=True)
        for line in out.stdout.splitlines():
            if line.startswith("PASS: ") or line.startswith("FAIL: "):
                check(line.startswith("PASS: "), "core: " + line[6:])
        check(out.returncode == 0, "core unit tests exit cleanly", out.stderr)

        # ---- inflate against Python's zlib
        def inflate(fmt, data):
            src, dst = tmp / "in.bin", tmp / "out.bin"
            src.write_bytes(data)
            if dst.exists():
                dst.unlink()
            r = subprocess.run([str(core), "inflate", fmt, str(src), str(dst)], capture_output=True, text=True)
            return r.stdout.strip(), (dst.read_bytes() if dst.exists() else None), r.returncode

        cases = 0
        bad = []
        for i, text in enumerate(sample_texts(rng)):
            for level in (0, 1, 6, 9):
                for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE):
                    for fmt, wbits in (("raw", -15), ("zlib", 15), ("gzip", 31), ("auto", 15), ("auto", -15)):
                        c = zlib.compressobj(level, zlib.DEFLATED, wbits, 8, strategy)
                        comp = c.compress(text) + c.flush()
                        status, data, rc = inflate(fmt, comp)
                        cases += 1
                        if status != f"OK {len(text)}" or data != text or rc != 0:
                            bad.append(f"sample {i} level {level} strategy {strategy} {fmt}: {status}")
        check(not bad, f"inflate matches Python zlib on {cases} streams (raw/zlib/gzip, levels 0-9, all strategies)", "; ".join(bad[:5]))
        text = b"first member\n" * 1000
        two = gzip.compress(text) + gzip.compress(b"second member\n")
        status, data, _ = inflate("gzip", two)
        check(data == text + b"second member\n", "multi-member gzip streams decode completely", status)
        named = bytearray(gzip.compress(text))
        named[3] |= 0x08 | 0x10                                  # FNAME + FCOMMENT
        named[10:10] = b"page.html\x00a comment\x00"
        status, data, _ = inflate("gzip", bytes(named))
        check(data == text, "gzip headers with FNAME/FCOMMENT are skipped", status)
        good = zlib.compress(text * 10)
        status, _, _ = inflate("zlib", good[:-30])
        check(status.startswith("ERR"), "a truncated stream is reported, not crashed on", status)
        corrupt = bytearray(gzip.compress(bytes(rng.getrandbits(8) for _ in range(5000)) + text))
        corrupt[len(corrupt) // 2] ^= 0x55
        status, _, _ = inflate("gzip", bytes(corrupt))
        check(status.startswith("ERR"), "a corrupted gzip stream fails its checks", status)
        status, _, _ = inflate("zlib", zlib.compress(b"x" * 40_000_000, 9))
        check(status.startswith("ERR -3"), "a decompression bomb stops at the output limit", status)
        fuzz_ok = True
        for k in range(300):
            blob = bytearray(zlib.compress(bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 3000))) + text[: rng.randint(0, 5000)]))
            for _ in range(rng.randint(1, 8)):
                blob[rng.randrange(len(blob))] = rng.getrandbits(8)
            status, _, rc = inflate(rng.choice(["raw", "zlib", "gzip", "auto"]), bytes(blob))
            if rc != 0 or not (status.startswith("OK") or status.startswith("ERR")):
                fuzz_ok = False
                break
        check(fuzz_ok, "300 randomly corrupted streams decode or fail cleanly without leaking heap", status)

        # ---- HTTP responses
        def http(raw, seed=1):
            src, dst = tmp / "resp.bin", tmp / "body.bin"
            src.write_bytes(raw)
            r = subprocess.run([str(core), "http", str(src), str(dst), str(seed)], capture_output=True, text=True)
            return r.stdout.strip(), (dst.read_bytes() if dst.exists() else b""), r.returncode

        body = ("<!doctype html><title>t</title>" + "<p>Paragraph é</p>" * 4000).encode()
        def chunked(data, ext=False):
            outb, i = b"", 0
            while i < len(data):
                n = rng.randint(1, 5000)
                piece = data[i:i + n]
                outb += f"{len(piece):x}{';name=val' if ext else ''}\r\n".encode() + piece + b"\r\n"
                i += n
            return outb + b"0\r\nX-Trailer: yes\r\n\r\n"

        resp = (b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: " + str(len(body)).encode() +
                b"\r\nSet-Cookie: a=1; Path=/\r\nSet-Cookie: b=2; Secure\r\n\r\n" + body)
        for seed in (1, 2, 3):
            s, b, rc = http(resp, seed)
            check(s.startswith("OK status=200") and b == body and "type=text/html charset=utf-8" in s and "cookies=[a=1; Path=/],[b=2; Secure]" in s and rc == 0,
                  f"Content-Length body in random pieces (seed {seed}); both Set-Cookie headers kept", s)
        extra = resp + b"HTTP/1.1 200 OK\r\n"
        s, b, _ = http(extra)
        check(f"consumed={len(resp)}" in s and b == body, "parsing stops exactly at the end of a Content-Length body", s)
        s, b, _ = http(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n" + chunked(body, True))
        check(s.startswith("OK status=200") and b == body and "close=0" in s, "chunked body with extensions and a trailer", s)
        gz = gzip.compress(body)
        s, b, _ = http(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Encoding: gzip\r\n\r\n" + chunked(gz))
        check(s.startswith("OK") and b == body, "gzip Content-Encoding inside chunked Transfer-Encoding", s)
        s, b, _ = http(b"HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: %d\r\n\r\n" % len(zlib.compress(body)) + zlib.compress(body))
        check(s.startswith("OK") and b == body, "deflate (zlib-wrapped) Content-Encoding", s)
        raw_deflate = zlib.compressobj(6, zlib.DEFLATED, -15)
        rd = raw_deflate.compress(body) + raw_deflate.flush()
        s, b, _ = http(b"HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\n\r\n" + rd)
        check(s.startswith("OK") and b == body and "close=1" in s, "raw-deflate Content-Encoding read until the connection closes", s)
        s, b, _ = http(b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 302 Found\r\nLocation: /next?x=1\r\nContent-Length: 0\r\n\r\n")
        check(s.startswith("OK status=302") and "location=/next?x=1" in s, "100 Continue is skipped; redirect Location is read", s)
        s, b, _ = http(b"HTTP/1.0 200 OK\r\nContent-Type: application/pdf\r\nContent-Disposition: attachment; filename=\"../../report.pdf\"\r\n\r\n%PDF-1.4")
        check("attachment=1 name=report.pdf" in s and b == b"%PDF-1.4" and "close=1" in s,
              "Content-Disposition file names are sanitised; HTTP/1.0 closes", s)
        s, b, _ = http(b"HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename*=UTF-8''%C5%99%C3%AD.txt\r\nContent-Length: 2\r\n\r\nok")
        check("name=ří.txt" in s, "RFC 5987 filename* is decoded", s)
        s, b, _ = http(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort")
        check(s.startswith("OK") and "truncated=1" in s and b == b"short", "a body cut short keeps the partial data, flagged as truncated", s)
        s, b, _ = http(b"garbage\r\n\r\n")
        check(s.startswith("ERR"), "a non-HTTP reply is rejected", s)
        s, b, _ = http(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n")
        check(s.startswith("ERR") or "truncated" in s, "an invalid chunk size is rejected", s)
        s, b, _ = http(b"HTTP/1.1 200 OK\r\nX-Big: " + b"a" * 70000 + b"\r\n\r\n")
        check(s.startswith("ERR"), "header blocks over 64 KiB are refused", s)
        s, b, _ = http(b"HTTP/1.1 304 Not Modified\r\nETag: x\r\n\r\n")
        check(s.startswith("OK status=304 len=0"), "304 has no body", s)

        images(tmp, core, rng)
        layout(tmpd)
        freestanding(tmp)
        contracts()

    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
