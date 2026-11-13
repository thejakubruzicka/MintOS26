#!/usr/bin/env python3
"""Analyse a QEMU HDA capture (-audiodev wav) against a MintOS sound.

    python3 tools/analyze_wav.py capture.wav build/sounds/startup.pcm [--channels 2] [--volume 70] [--report out.txt]

Finds where the sound starts in the capture (cross-correlation of the
envelope, then exact sample alignment), and checks that the captured audio
matches the expected PCM scaled by the kernel mixer's volume curve
(quadratic, Q15): correlation, worst sample error, no dropouts (10 ms
windows that should be loud but are silent), no clipping, and that the
output is silent before the sound starts. Handles QEMU's unfinalised WAV
header (data size 0 while QEMU is still running). Standard library only.
"""
from __future__ import annotations

import math
import struct
import sys
from pathlib import Path


def read_wav(path: Path):
    d = path.read_bytes()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise SystemExit("not a WAV file")
    i, fmt, data = 12, None, None
    while i + 8 <= len(d):
        cid, size = d[i:i + 4], struct.unpack_from("<I", d, i + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", d, i + 8)
        if cid == b"data":
            data = d[i + 8:] if size == 0 or i + 8 + size > len(d) else d[i + 8:i + 8 + size]
            break
        i += 8 + size + (size & 1)
    if not fmt or data is None or fmt[0] != 1 or fmt[5] != 16:
        raise SystemExit("expected 16-bit PCM WAV")
    channels, rate = fmt[1], fmt[2]
    n = len(data) // (2 * channels)
    s = struct.unpack(f"<{n * channels}h", data[:n * channels * 2])
    return rate, channels, s


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 2
    rate, ch, cap = read_wav(Path(argv[1]))
    pcm = Path(argv[2]).read_bytes()
    src_ch = int(argv[argv.index("--channels") + 1]) if "--channels" in argv else 2
    volume = int(argv[argv.index("--volume") + 1]) if "--volume" in argv else 70
    src = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    frames = len(src) // src_ch
    gain = (volume * volume * 32768 + 5000) // 10000            # kernel mixer curve
    exp_l = [((src[i * src_ch] * gain) >> 15) for i in range(frames)]
    exp_r = [((src[i * src_ch + (1 if src_ch == 2 else 0)] * gain) >> 15) for i in range(frames)]
    cap_l, cap_r = list(cap[0::ch]), list(cap[1::ch]) if ch > 1 else list(cap[0::ch])
    lines = [f"capture: {rate} Hz, {ch} ch, {len(cap_l)} frames ({len(cap_l) / rate:.2f} s)"]
    if rate != 48000:
        lines.append("FAIL: capture is not 48 kHz (use -audiodev wav,...,out.frequency=48000)")
        print("\n".join(lines))
        return 1
    # coarse: first sample loud enough relative to the expected peak
    peak = max(abs(v) for v in exp_l) or 1
    onset_c = next((i for i, v in enumerate(cap_l) if abs(v) > peak * 0.02), None)
    onset_e = next((i for i, v in enumerate(exp_l) if abs(v) > peak * 0.02), 0)
    if onset_c is None:
        lines.append("FAIL: the capture is silent")
        print("\n".join(lines))
        return 1
    # fine: best alignment within +-64 frames by sum of squared error
    best, best_err = 0, None
    for k in range(-64, 65):
        start = onset_c - onset_e + k
        if start < 0 or start + 4800 > len(cap_l):
            continue
        err = sum((cap_l[start + j] - exp_l[j]) ** 2 for j in range(onset_e, onset_e + 4800))
        if best_err is None or err < best_err:
            best, best_err = start, err
    start = best
    n = min(frames, len(cap_l) - start)
    sxy = sxx = syy = 0.0
    worst = 0
    for j in range(n):
        for a, b in ((cap_l[start + j], exp_l[j]), (cap_r[start + j], exp_r[j])):
            sxy += a * b; sxx += a * a; syy += b * b
            worst = max(worst, abs(a - b))
    corr = sxy / math.sqrt(sxx * syy) if sxx and syy else 0.0
    dropouts = 0
    win = 480
    for w in range(0, n - win, win):
        e = math.sqrt(sum(exp_l[w + j] ** 2 for j in range(win)) / win)
        c = math.sqrt(sum(cap_l[start + w + j] ** 2 for j in range(win)) / win)
        if e > 300 and c < e * 0.25:
            dropouts += 1
    pre = cap_l[max(0, start - 4800):start]
    pre_peak = max((abs(v) for v in pre), default=0)
    clipped = sum(1 for v in cap_l[start:start + n] if abs(v) >= 32767)
    lines += [
        f"sound starts at frame {start} ({start / rate:.3f} s into the capture), {n} frames compared",
        f"correlation with the expected PCM x volume {volume}: {corr:.5f}",
        f"worst sample error: {worst} (of peak {peak})",
        f"dropouts (10 ms windows): {dropouts}",
        f"clipped samples: {clipped}",
        f"peak in the 100 ms before the sound: {pre_peak}",
    ]
    ok = corr > 0.995 and dropouts == 0 and clipped == 0 and n >= frames * 0.95 and pre_peak < 64
    lines.append(("PASS" if ok else "FAIL") + ": captured startup chime matches the generated sound")
    text = "\n".join(lines)
    print(text)
    if "--report" in argv:
        Path(argv[argv.index("--report") + 1]).write_text(text + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
