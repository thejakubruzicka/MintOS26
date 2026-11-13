#!/usr/bin/env python3
"""Checks for the generated MintOS sounds (tools/make_sounds.py).

    python3 tools/test_sounds.py build/sounds

Verifies every built-in sound exists with the format the kernel expects,
is deterministic (a fresh generation is byte-identical), never clips, has a
sensible level and length, and that the tonal sounds contain the pitches
they were designed with (Goertzel detector).
"""
from __future__ import annotations

import math
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RATE = 48000
EXPECT = {  # name: (channels, min seconds, max seconds, peak dBFS window)
    "startup": (2, 2.4, 3.6, (-3.5, -2.5)),
    "trash": (1, 0.25, 0.8, (-8.0, -5.0)),
    "empty_trash": (1, 0.5, 1.2, (-8.0, -5.0)),
    "notification": (2, 0.4, 1.0, (-7.0, -5.0)),
    "volume": (1, 0.03, 0.15, (-10.0, -8.0)),
    "screenshot": (1, 0.2, 0.6, (-9.0, -7.0)),
    "alert_mint": (1, 0.2, 0.8, (-8.0, -6.0)),
    "alert_dewdrop": (1, 0.2, 0.8, (-8.0, -6.0)),
    "alert_fern": (1, 0.2, 0.8, (-8.0, -6.0)),
    "alert_ripple": (1, 0.2, 0.8, (-8.5, -6.5)),
    "alert_woodblock": (1, 0.2, 0.8, (-8.5, -6.5)),
}
failures = 0


def check(cond: bool, msg: str) -> None:
    global failures
    print(("PASS: " if cond else "FAIL: ") + msg)
    if not cond:
        failures += 1


def goertzel(x: list[int], freq: float) -> float:
    w = 2.0 * math.pi * freq / RATE
    c = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + c * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / len(x)


def main(argv: list[str]) -> int:
    out = Path(argv[1]) if len(argv) > 1 else ROOT / "build/sounds"
    sounds = {}
    for name, (channels, lo, hi, (pmin, pmax)) in EXPECT.items():
        data = (out / f"{name}.pcm").read_bytes() if (out / f"{name}.pcm").exists() else b""
        check(len(data) > 0 and len(data) % (2 * channels) == 0, f"{name}: present, whole {channels}-channel frames")
        if not data:
            continue
        s = struct.unpack(f"<{len(data) // 2}h", data)
        sounds[name] = (channels, s)
        secs = len(s) / channels / RATE
        peak = max(abs(v) for v in s)
        peak_db = 20 * math.log10(peak / 32767.0)
        rms = math.sqrt(sum(v * v for v in s) / len(s))
        check(lo <= secs <= hi, f"{name}: {secs:.2f} s within {lo}-{hi} s")
        check(peak < 32767 and pmin <= peak_db <= pmax, f"{name}: peak {peak_db:.1f} dBFS (no clipping)")
        check(rms > 300, f"{name}: audible level (RMS {rms:.0f})")
        tail = s[-channels * 240:]
        check(max(abs(v) for v in tail) < peak * 0.05, f"{name}: fades out (no click at the end)")
        head = s[:channels * 8]
        check(max(abs(v) for v in head) < 4000, f"{name}: soft onset")
    if "alert_mint" in sounds:
        x = list(sounds["alert_mint"][1][:9600])
        check(goertzel(x, 880.0) > 4 * goertzel(x, 660.0), "alert_mint: fundamental at 880 Hz")
    if "notification" in sounds:
        ch, s = sounds["notification"]
        left = list(s[0::2][:12000])
        check(goertzel(left, 1318.51) > 3 * goertzel(left, 1100.0), "notification: E6 present")
        check(goertzel(left[6000:], 1975.53) > 3 * goertzel(left[6000:], 1600.0), "notification: B6 present")
    if "startup" in sounds:
        ch, s = sounds["startup"]
        mono = [(s[i] + s[i + 1]) // 2 for i in range(0, min(len(s), 2 * 48000), 2)]
        ok = all(goertzel(mono, f) > 3 * goertzel(mono, f * 1.06) for f in (293.66, 440.0, 659.26))
        check(ok, "startup chime: D4, A4 and E5 chord tones present")
        left, right = s[0::2], s[1::2]
        diff = sum(abs(a - b) for a, b in zip(left[:48000], right[:48000])) / 48000
        check(diff > 50, "startup chime is genuinely stereo")
    with tempfile.TemporaryDirectory(prefix="mint-sounds-") as td:
        subprocess.run([sys.executable, str(ROOT / "tools/make_sounds.py"), td], check=True, capture_output=True)
        same = all((Path(td) / f"{n}.pcm").read_bytes() == (out / f"{n}.pcm").read_bytes() for n in EXPECT)
        check(same, "generation is deterministic (byte-identical rebuild)")
    total = sum((out / f"{n}.pcm").stat().st_size for n in EXPECT if (out / f"{n}.pcm").exists())
    check(total < 1_200_000, f"all sounds together are compact ({total // 1024} KiB)")
    print(f"{'FAIL' if failures else 'PASS'}: sound checks ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
