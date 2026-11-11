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


def main():
    rng = random.Random(26514)
    with tempfile.TemporaryDirectory(prefix="mint-web-") as tmpd:
        tmp = Path(tmpd)
        core = build(tmpd, "tools/test_web_core.c", ["kernel/user/web/wmem.c", "kernel/user/web/inflate.c", "kernel/user/web/whttp.c"])
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

    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
