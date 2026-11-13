#!/usr/bin/env python3
"""Synthesise MintOS's original system sounds (26.5.014.0).

Everything is generated from scratch with additive synthesis, filtered noise,
a Karplus-Strong string and a small Schroeder reverb -- no samples, no
third-party modules, no imitation of any other platform's sounds. Output is
deterministic (fixed-seed LCG) 48 kHz signed 16-bit little-endian PCM, the
format the kernel mixer plays:

    python3 tools/make_sounds.py build/sounds            # *.pcm + manifest.txt
    python3 tools/make_sounds.py build/sounds --wav DIR  # also WAV files to listen to

The startup chime "Mint Morning" is an airy rising arpeggio (D, A, E, F#, B)
that settles into an open add9/6 chord over a breathy pad; it is mastered to
peak at -3 dBFS so nothing clips. UI sounds peak lower (-6..-9 dBFS).
"""
from __future__ import annotations

import math
import struct
import sys
from pathlib import Path

RATE = 48000
TAU = 2.0 * math.pi


class LCG:
    """Deterministic noise independent of Python's random module."""

    def __init__(self, seed: int) -> None:
        self.s = seed & 0xFFFFFFFF

    def next(self) -> float:
        self.s = (1664525 * self.s + 1013904223) & 0xFFFFFFFF
        return (self.s >> 8) / float(1 << 23) - 1.0


def silence(seconds: float) -> list[float]:
    return [0.0] * int(seconds * RATE)


def add(dst: list[float], src: list[float], at: int = 0, gain: float = 1.0) -> None:
    end = min(len(dst), at + len(src))
    for i in range(max(at, 0), end):
        dst[i] += src[i - at] * gain


def env_adsr(n: int, attack: float, decay_tau: float) -> list[float]:
    a = max(1, int(attack * RATE))
    out = []
    for i in range(n):
        if i < a:
            out.append(0.5 - 0.5 * math.cos(math.pi * i / a))
        else:
            out.append(math.exp(-(i - a) / (decay_tau * RATE)))
    return out


def fade_in(x: list[float], seconds: float) -> None:
    n = min(len(x), max(1, int(seconds * RATE)))
    for k in range(n):
        x[k] *= 0.5 - 0.5 * math.cos(math.pi * k / n)


def fade_out(x: list[float], seconds: float) -> None:
    n = min(len(x), int(seconds * RATE))
    for k in range(n):
        x[len(x) - n + k] *= 0.5 + 0.5 * math.cos(math.pi * k / n)


def bell(freq: float, seconds: float, attack: float = 0.006, decay: float = 0.6,
         partials=((1.0, 1.0, 1.0), (2.0, 0.32, 0.55), (3.0, 0.10, 0.35), (4.17, 0.05, 0.22))) -> list[float]:
    """Soft bell: (ratio, amplitude, decay multiplier) partials."""
    n = int(seconds * RATE)
    out = [0.0] * n
    a = max(1, int(attack * RATE))
    for ratio, amp, dmul in partials:
        f = freq * ratio
        if f >= RATE / 2 - 1000:
            continue
        w = TAU * f / RATE
        tau = decay * dmul * RATE
        for i in range(n):
            e = (0.5 - 0.5 * math.cos(math.pi * i / a)) if i < a else math.exp(-(i - a) / tau)
            out[i] += amp * e * math.sin(w * i)
    return out


def lowpass(x: list[float], cutoff: float) -> list[float]:
    k = 1.0 - math.exp(-TAU * cutoff / RATE)
    y, out = 0.0, []
    for v in x:
        y += k * (v - y)
        out.append(y)
    return out


def highpass(x: list[float], cutoff: float) -> list[float]:
    lp = lowpass(x, cutoff)
    return [a - b for a, b in zip(x, lp)]


def bandpass(x: list[float], low: float, high: float) -> list[float]:
    return lowpass(highpass(x, low), high)


def resonator(x: list[float], freq: float, q: float) -> list[float]:
    """Two-pole resonant filter (biquad bandpass, constant skirt)."""
    w = TAU * freq / RATE
    alpha = math.sin(w) / (2.0 * q)
    b0, a0, a1, a2 = alpha, 1.0 + alpha, -2.0 * math.cos(w), 1.0 - alpha
    x1 = x2 = y1 = y2 = 0.0
    out = []
    for v in x:
        y = (b0 * v - b0 * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1, y2, y1 = x1, v, y1, y
        out.append(y)
    return out


def noise(seconds: float, seed: int) -> list[float]:
    r = LCG(seed)
    return [r.next() for _ in range(int(seconds * RATE))]


def reverb(x: list[float], seed_shift: int, mix: float, tail: float) -> list[float]:
    """Small Schroeder reverb: 4 parallel combs + 2 allpasses."""
    combs = [1557, 1617, 1491, 1422]
    combs = [c + seed_shift for c in combs]
    n = len(x) + int(tail * RATE)
    src = x + [0.0] * (n - len(x))
    wet = [0.0] * n
    for d in combs:
        buf = [0.0] * d
        fb, damp, store, j = 0.80, 0.25, 0.0, 0
        for i in range(n):
            out = buf[j]
            store = out * (1.0 - damp) + store * damp
            buf[j] = src[i] + store * fb
            wet[i] += out
            j += 1
            if j == d:
                j = 0
    for d in (556 + seed_shift, 441 + seed_shift):
        buf = [0.0] * d
        j = 0
        for i in range(n):
            b = buf[j]
            v = wet[i]
            buf[j] = v + b * 0.5
            wet[i] = b - v
            j += 1
            if j == d:
                j = 0
    return [src[i] * (1.0 - mix) + wet[i] * mix * 0.25 for i in range(n)]


def master(chans: list[list[float]], peak_db: float) -> list[list[float]]:
    peak = max(max(abs(v) for v in c) for c in chans) or 1.0
    target = 10.0 ** (peak_db / 20.0)
    g = target / peak
    return [[v * g for v in c] for c in chans]


def to_pcm(chans: list[list[float]]) -> bytes:
    n = min(len(c) for c in chans)
    out = bytearray()
    for i in range(n):
        for c in chans:
            v = int(round(c[i] * 32767.0))
            v = 32767 if v > 32767 else -32768 if v < -32768 else v
            out += struct.pack("<h", v)
    return bytes(out)


# ---------------------------------------------------------------- the sounds
def startup() -> list[list[float]]:
    dur = 2.6
    left, right = silence(dur), silence(dur)
    notes = [(293.66, -0.55), (440.00, -0.25), (659.26, 0.05), (739.99, 0.30), (987.77, 0.55)]
    for k, (f, pan) in enumerate(notes):
        tone = bell(f, dur - k * 0.075, attack=0.012, decay=0.95 - k * 0.08)
        shimmer = bell(f * 2.003, dur - k * 0.075, attack=0.02, decay=0.35, partials=((1.0, 0.12, 1.0),))
        tone = [a + b for a, b in zip(tone, shimmer)]
        at = int(k * 0.075 * RATE)
        add(left, tone, at, 0.22 * math.cos((pan + 1) * math.pi / 4))
        add(right, tone, at, 0.22 * math.sin((pan + 1) * math.pi / 4))
    # warm pad: detuned D3/A3 with a slow swell
    n = len(left)
    for f, det, amp in ((146.83, 0.7, 0.10), (220.00, -0.6, 0.08), (293.66, 0.4, 0.05)):
        for i in range(n):
            t = i / RATE
            e = min(1.0, t / 0.35) * math.exp(-max(0.0, t - 0.35) / 1.1)
            a = amp * e
            left[i] += a * math.sin(TAU * (f - det) * t)
            right[i] += a * math.sin(TAU * (f + det) * t + 0.7)
    # breath of air: band-limited noise swell
    air = bandpass(noise(dur, 0x4D494E54), 1800.0, 6500.0)
    for i in range(len(air)):
        t = i / RATE
        e = math.sin(math.pi * min(1.0, t / 1.6)) ** 2 * 0.035
        left[i] += air[i] * e
        right[i] += air[(i + 211) % len(air)] * e
    left = reverb(left, 0, 0.30, 0.45)
    right = reverb(right, 23, 0.30, 0.45)
    fade_out(left, 0.5)
    fade_out(right, 0.5)
    return master([left, right], -3.0)


def crumple(seconds: float, seed: int, density: float) -> list[float]:
    """Dry-leaf crumple: random crackles through a band-pass, swept down."""
    r = LCG(seed)
    n = int(seconds * RATE)
    x = [0.0] * n
    i = 0
    while i < n:
        gap = int((0.004 + 0.02 * (r.next() + 1.0) / 2.0) / density * RATE)
        burst = int((0.002 + 0.006 * (r.next() + 1.0) / 2.0) * RATE)
        amp = 0.4 + 0.6 * (r.next() + 1.0) / 2.0
        for k in range(burst):
            if i + k < n:
                x[i + k] += amp * r.next() * math.exp(-k / (burst * 0.35 + 1))
        i += gap
    x = bandpass(x, 900.0, 7000.0)
    env = env_adsr(n, 0.01, seconds * 0.45)
    return [a * b for a, b in zip(x, env)]


def trash() -> list[list[float]]:
    x = crumple(0.42, 0x7A54, 1.4)
    whoosh = lowpass(noise(0.42, 0x1EAF), 1400.0)
    for i in range(len(whoosh)):
        t = i / RATE
        x[i] += whoosh[i] * 0.6 * math.sin(math.pi * min(1.0, t / 0.42)) ** 2
    fade_out(x, 0.06)
    return master([x], -7.0)


def empty_trash() -> list[list[float]]:
    x = crumple(0.85, 0xE3B7, 1.0)
    sweep = noise(0.85, 0x5EED)
    y, out = 0.0, []
    for i, v in enumerate(sweep):           # whoosh with a falling low-pass
        t = i / RATE
        k = 1.0 - math.exp(-TAU * (4200.0 * math.exp(-t * 3.0) + 300.0) / RATE)
        y += k * (v - y)
        out.append(y * 0.8 * math.sin(math.pi * min(1.0, t / 0.85)))
    add(x, out)
    thud = bell(92.0, 0.3, attack=0.004, decay=0.07, partials=((1.0, 1.0, 1.0), (2.3, 0.3, 0.5)))
    add(x, thud, int(0.55 * RATE), 0.9)
    fade_out(x, 0.08)
    return master([x], -6.5)


def notification() -> list[list[float]]:
    dur = 0.7
    left, right = silence(dur), silence(dur)
    marimba = ((1.0, 1.0, 1.0), (3.93, 0.18, 0.18), (9.2, 0.04, 0.08))
    a = bell(1318.51, dur, attack=0.003, decay=0.28, partials=marimba)          # E6
    b = bell(1975.53, dur - 0.11, attack=0.003, decay=0.32, partials=marimba)   # B6
    add(left, a, 0, 0.55); add(right, a, 0, 0.42)
    add(left, b, int(0.11 * RATE), 0.40); add(right, b, int(0.11 * RATE), 0.55)
    left = reverb(left, 5, 0.22, 0.15)
    right = reverb(right, 31, 0.22, 0.15)
    fade_out(left, 0.2); fade_out(right, 0.2)
    return master([left, right], -6.0)


def volume_tick() -> list[list[float]]:
    x = bell(930.0, 0.07, attack=0.0015, decay=0.012, partials=((1.0, 1.0, 1.0), (2.7, 0.25, 0.4)))
    click = highpass(noise(0.004, 0x71C), 3000.0)
    add(x, click, 0, 0.25)
    fade_out(x, 0.01)
    return master([x], -9.0)


def screenshot() -> list[list[float]]:
    x = silence(0.36)
    for at, seed, g in ((0.0, 0xC1, 1.0), (0.075, 0xC2, 0.7)):
        click = bandpass(noise(0.03, seed), 1200.0, 9000.0)
        e = env_adsr(len(click), 0.0005, 0.006)
        add(x, [a * b for a, b in zip(click, e)], int(at * RATE), g)
    sparkle = bell(2637.02, 0.25, attack=0.004, decay=0.07, partials=((1.0, 1.0, 1.0), (1.5, 0.4, 0.8)))
    add(x, sparkle, int(0.09 * RATE), 0.18)
    fade_out(x, 0.05)
    return master([x], -8.0)


def alert_mint() -> list[list[float]]:
    x = bell(880.0, 0.5, attack=0.004, decay=0.16, partials=((1.0, 1.0, 1.0), (1.5, 0.45, 0.8), (3.01, 0.12, 0.4)))
    fade_out(x, 0.08)
    return master([x], -7.0)


def alert_dewdrop() -> list[list[float]]:
    n = int(0.38 * RATE)
    x, ph = [], 0.0
    for i in range(n):
        t = i / RATE
        f = 700.0 + 900.0 * math.exp(-t / 0.018)
        ph += TAU * f / RATE
        e = (min(1.0, t / 0.002)) * math.exp(-t / 0.07)
        x.append(math.sin(ph) * e)
    tail = resonator([v * 0.3 for v in x], 1400.0, 18.0)
    x = [a + b * 0.6 for a, b in zip(x, tail)]
    fade_out(x, 0.06)
    return master([x], -7.0)


def alert_fern() -> list[list[float]]:
    freq, n = 392.0, int(0.55 * RATE)
    period = int(RATE / freq)
    r = LCG(0xFE24)
    buf = [r.next() for _ in range(period)]
    buf = lowpass(buf, 3000.0)
    out = []
    for i in range(n):
        j = i % period
        v = buf[j]
        nxt = buf[(j + 1) % period]
        buf[j] = 0.996 * 0.5 * (v + nxt)
        out.append(v)
    out = highpass(out, 120.0)
    fade_in(out, 0.0015)
    fade_out(out, 0.08)
    return master([out], -7.0)


def alert_ripple() -> list[list[float]]:
    x = silence(0.55)
    for k, f in enumerate((660.0, 880.0, 1108.73)):
        blip = bell(f, 0.2, attack=0.002, decay=0.035, partials=((1.0, 1.0, 1.0), (2.0, 0.2, 0.5)))
        add(x, blip, int(k * 0.055 * RATE), 0.8 - k * 0.12)
    echo = list(x)
    add(x, echo, int(0.16 * RATE), 0.28)
    fade_out(x, 0.08)
    return master([x], -7.5)


def alert_woodblock() -> list[list[float]]:
    x = silence(0.36)
    for at, f in ((0.0, 1046.5), (0.12, 784.0)):
        imp = [0.0] * int(0.12 * RATE)
        imp[0] = 1.0
        imp[1] = -0.6
        knock = resonator(imp, f, 22.0)
        knock2 = resonator(imp, f * 2.76, 30.0)
        add(x, [a + b * 0.35 for a, b in zip(knock, knock2)], int(at * RATE))
    fade_in(x, 0.0012)
    fade_out(x, 0.05)
    return master([x], -7.5)


SOUNDS = [
    ("startup", startup), ("trash", trash), ("empty_trash", empty_trash),
    ("notification", notification), ("volume", volume_tick), ("screenshot", screenshot),
    ("alert_mint", alert_mint), ("alert_dewdrop", alert_dewdrop), ("alert_fern", alert_fern),
    ("alert_ripple", alert_ripple), ("alert_woodblock", alert_woodblock),
]


def write_wav(path: Path, pcm: bytes, channels: int) -> None:
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, channels, RATE, RATE * 2 * channels, 2 * channels, 16) + b"data" + struct.pack("<I", len(pcm))
    path.write_bytes(hdr + pcm)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    wav_dir = Path(argv[argv.index("--wav") + 1]) if "--wav" in argv else None
    if wav_dir:
        wav_dir.mkdir(parents=True, exist_ok=True)
    lines = []
    for name, fn in SOUNDS:
        chans = fn()
        pcm = to_pcm(chans)
        (out / f"{name}.pcm").write_bytes(pcm)
        if wav_dir:
            write_wav(wav_dir / f"mintos-{name.replace('_', '-')}.wav", pcm, len(chans))
        samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)
        peak = max(abs(s) for s in samples)
        rms = math.sqrt(sum(s * s for s in samples) / len(samples))
        lines.append(f"{name} channels={len(chans)} frames={len(samples) // len(chans)} peak={peak} rms={rms:.1f}")
    (out / "manifest.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
