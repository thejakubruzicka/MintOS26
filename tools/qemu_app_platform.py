#!/usr/bin/env python3
"""Drive a headless QEMU session for the 26.5.014.0 app-platform evidence.

    python3 tools/qemu_app_platform.py SCRIPT [--no-disk] [--keep]

Boots build/ with tools/run_qemu.sh (--mintos-disk unless --no-disk), an
xHCI keyboard and tablet, presses 2 at the boot selector and runs SCRIPT,
one command per line:

    wait SECONDS                 sleep
    waitlog TEXT [TIMEOUT]       wait until COM1 contains TEXT (fails the run on timeout)
    key KEYS                     HMP sendkey (e.g. key ctrl-c, key ret)
    type TEXT                    type printable ASCII (US layout)
    line TEXT                    type TEXT and press Enter
    move X Y / click X Y [N]     pointer in screen pixels (1920x1080)
    shot NAME                    test-evidence/qemu-26.5.014.0-app-platform-NAME.png (1152x648)
    expectlog TEXT               fail unless COM1 contains TEXT

Ports: HMP 45463, QMP 45473 (app-platform session). Standard library only;
macOS `sips` downsizes the screenshots.
"""
from __future__ import annotations

import json
import socket
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HMP_PORT, QMP_PORT = 45463, 45473
W, H = 1920, 1080

SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
           '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right',
           '|': 'backslash', ':': 'semicolon', '"': 'apostrophe', '<': 'comma', '>': 'dot',
           '?': 'slash', '~': 'grave_accent'}
PLAIN = {' ': 'spc', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
         '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe', ',': 'comma', '.': 'dot',
         '/': 'slash', '`': 'grave_accent', '\n': 'ret'}


def key_for(ch: str) -> str:
    if 'a' <= ch <= 'z' or '0' <= ch <= '9':
        return ch
    if 'A' <= ch <= 'Z':
        return 'shift-' + ch.lower()
    if ch in PLAIN:
        return PLAIN[ch]
    if ch in SHIFTED:
        return 'shift-' + SHIFTED[ch]
    raise ValueError(f'cannot type {ch!r}')


class Hmp:
    def __init__(self) -> None:
        self.s = socket.create_connection(("127.0.0.1", HMP_PORT), timeout=10)
        self.read_prompt()

    def read_prompt(self) -> str:
        buf = b""
        end = time.time() + 20
        while b"(qemu)" not in buf and time.time() < end:
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
        return buf.decode(errors="replace")

    def cmd(self, line: str) -> str:
        self.s.sendall(line.encode() + b"\n")
        return self.read_prompt()


class Qmp:
    def __init__(self) -> None:
        self.s = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=10)
        self.f = self.s.makefile("rwb")
        self.f.readline()
        self.call("qmp_capabilities")

    def call(self, name: str, args: dict | None = None) -> dict:
        msg = {"execute": name}
        if args is not None:
            msg["arguments"] = args
        self.f.write(json.dumps(msg).encode() + b"\n")
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return {}
            reply = json.loads(line)
            if "return" in reply or "error" in reply:
                return reply

    def move(self, x: int, y: int) -> None:
        ax, ay = int(x * 32767 / (W - 1)), int(y * 32767 / (H - 1))
        self.call("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]})

    def click(self, x: int, y: int, n: int = 1) -> None:
        self.move(x, y)
        time.sleep(0.15)
        for _ in range(n):
            self.call("input-send-event", {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]})
            time.sleep(0.06)
            self.call("input-send-event", {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]})
            time.sleep(0.12)


def com1() -> str:
    p = ROOT / "build/com1-qemu.log"
    return p.read_text(errors="replace") if p.exists() else ""


def ppm_to_png(src: Path, dst: Path) -> None:
    data = src.read_bytes()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    pos += 1
    width, height = int(parts[1]), int(parts[2])
    rgb = data[pos:pos + width * height * 3]
    rows = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    dst.write_bytes(png)


def shot(hmp: Hmp, name: str) -> None:
    tmp = Path("/tmp") / f"mintos-app-platform-{name}.ppm"
    hmp.cmd(f"screendump {tmp}")
    time.sleep(1.0)
    out = ROOT / f"test-evidence/qemu-26.5.014.0-app-platform-{name}.png"
    ppm_to_png(tmp, out)
    subprocess.run(["sips", "-z", "648", "1152", str(out)], check=False, capture_output=True)
    print("evidence:", out.relative_to(ROOT))


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    script = Path(argv[1]).read_text().splitlines()
    log = ROOT / "build/com1-qemu.log"
    if log.exists():
        log.unlink()
    qemu_args = ["-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0", "-device", "usb-tablet,bus=xhci.0",
                 "-monitor", f"tcp:127.0.0.1:{HMP_PORT},server,nowait", "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait"]
    run = [str(ROOT / "tools/run_qemu.sh"), "--headless"]
    if "--no-disk" not in argv:
        run.append("--mintos-disk")
    proc = subprocess.Popen([*run, "--", *qemu_args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    status = 0
    try:
        hmp = qmp = None
        for _ in range(60):
            time.sleep(1)
            try:
                hmp, qmp = Hmp(), Qmp()
                break
            except OSError:
                continue
        if hmp is None:
            print("FAIL: QEMU monitor did not come up")
            return 1
        deadline = time.time() + 90
        while "native kernel entry" not in com1() and time.time() < deadline:
            hmp.cmd("sendkey 2")
            time.sleep(0.4)
        for raw in script:
            line = raw.rstrip("\n")
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            op, _, arg = stripped.partition(" ")
            if op in ("type", "line"):
                arg = line.split(op + " ", 1)[1] if (op + " ") in line else ""
            if op == "wait":
                time.sleep(float(arg))
            elif op in ("waitlog", "expectlog"):
                text, timeout = arg, 60.0
                if op == "waitlog":
                    head, _, tail = arg.rpartition(" ")
                    if head and tail.replace(".", "", 1).isdigit():
                        text, timeout = head, float(tail)
                    end = time.time() + timeout
                    while text not in com1() and time.time() < end:
                        time.sleep(0.5)
                if text not in com1():
                    print(f"FAIL: COM1 never showed {text!r}")
                    status = 1
                    break
                print(f"ok: COM1 shows {text!r}")
            elif op == "key":
                hmp.cmd(f"sendkey {arg}")
                time.sleep(0.15)
            elif op in ("type", "line"):
                for ch in arg:
                    hmp.cmd(f"sendkey {key_for(ch)}")
                    time.sleep(0.06)
                if op == "line":
                    hmp.cmd("sendkey ret")
                    time.sleep(0.2)
            elif op == "move":
                x, y = (int(v) for v in arg.split())
                qmp.move(x, y)
            elif op == "click":
                vals = [int(v) for v in arg.split()]
                qmp.click(vals[0], vals[1], vals[2] if len(vals) > 2 else 1)
            elif op == "shot":
                shot(hmp, arg)
            else:
                print(f"unknown command: {line}")
                status = 2
                break
    finally:
        if "--keep" not in argv:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
