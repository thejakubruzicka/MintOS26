#!/usr/bin/env python3
"""Drive a headless QEMU session for the 26.5.014.0 personality evidence.

    python3 tools/qemu_personality.py SCRIPT [--wav out.wav] [--raw DIR] [--keep]
    python3 tools/qemu_personality.py SCRIPT --attach [--quit]   (drive a session left running with --keep)

Boots build/ with tools/run_qemu.sh --mintos-disk, an xHCI keyboard and
tablet and an ich9-intel-hda + hda-output codec (captured to a WAV file),
presses 2 at the boot selector and then runs SCRIPT: one command per line,

    wait SECONDS                 sleep
    waitlog TEXT [TIMEOUT]       wait until COM1 contains TEXT
    key KEYS                     HMP sendkey (e.g. key meta_l-shift-3)
    type TEXT                    sendkey per character (a-z, 0-9, space)
    move X Y                     absolute pointer (screen pixels, 1920x1080)
    click X Y [N]                move + left click (N clicks)
    shot NAME                    screendump -> test-evidence/qemu-26.5.014.0-personality-NAME.png (1152x648)
    fullshot NAME                screendump at full size into the scratch dir given by --raw DIR

Ports: HMP 45464, QMP 45474 (personality session). Requires only the
standard library; sips (macOS) resizes the PNG evidence.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HMP_PORT, QMP_PORT = 45464, 45474
W, H = 1920, 1080


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


def shot(hmp: Hmp, name: str, raw_dir: Path | None, full: bool) -> None:
    tmp = (raw_dir or Path("/tmp")) / f"{name}.ppm"
    hmp.cmd(f"screendump {tmp}")
    time.sleep(1.0)
    if full:
        subprocess.run([sys.executable, str(ROOT / "tools/ppm2png.py"), str(tmp), str(tmp.with_suffix(".png"))], check=True)
        return
    out = ROOT / f"test-evidence/qemu-26.5.014.0-personality-{name}.png"
    subprocess.run([sys.executable, str(ROOT / "tools/ppm2png.py"), str(tmp), str(out)], check=True)
    subprocess.run(["sips", "-z", "648", "1152", str(out)], check=False, capture_output=True)
    print("evidence:", out.relative_to(ROOT))


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    script = Path(argv[1]).read_text().splitlines()
    wav = Path(argv[argv.index("--wav") + 1]).resolve() if "--wav" in argv else Path("/tmp/mintos-personality.wav")
    raw_dir = Path(argv[argv.index("--raw") + 1]).resolve() if "--raw" in argv else None
    if raw_dir:
        raw_dir.mkdir(parents=True, exist_ok=True)
    attach = "--attach" in argv          # drive an already running session (started with --keep)
    log = ROOT / "build/com1-qemu.log"
    if log.exists() and not attach:
        log.unlink()
    qemu_args = ["-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0", "-device", "usb-tablet,bus=xhci.0",
                 "-audiodev", f"wav,id=snd0,path={wav},out.frequency=48000", "-device", "ich9-intel-hda", "-device", "hda-output,audiodev=snd0",
                 "-monitor", f"tcp:127.0.0.1:{HMP_PORT},server,nowait", "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait"]
    proc = None
    if not attach:
        proc = subprocess.Popen([str(ROOT / "tools/run_qemu.sh"), "--headless", "--mintos-disk", "--", *qemu_args],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        if not attach:
            time.sleep(3)
        hmp, qmp = Hmp(), Qmp()
        # The UEFI selector shows for 3 s and does not log to COM1: keep
        # pressing 2 until the native kernel announces itself.
        deadline = time.time() + 90
        while not attach and "native kernel entry" not in com1() and time.time() < deadline:
            hmp.cmd("sendkey 2")
            time.sleep(0.4)
        for raw in script:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            op, *rest = line.split(" ", 1)
            arg = rest[0] if rest else ""
            if op == "wait":
                time.sleep(float(arg))
            elif op == "waitlog":
                parts = arg.rsplit(" ", 1)
                text, timeout = (parts[0], float(parts[1])) if len(parts) == 2 and parts[1].replace(".", "").isdigit() else (arg, 120.0)
                end = time.time() + timeout
                while text not in com1() and time.time() < end:
                    time.sleep(0.5)
                print(("found: " if text in com1() else "TIMEOUT: ") + text)
            elif op == "key":
                hmp.cmd(f"sendkey {arg}")
                time.sleep(0.3)
            elif op == "type":
                for ch in arg:
                    hmp.cmd("sendkey " + ("spc" if ch == " " else "minus" if ch == "-" else "dot" if ch == "." else
                                          "slash" if ch == "/" else "shift-dot" if ch == ">" else "shift-semicolon" if ch == ":" else
                                          "shift-1" if ch == "!" else "comma" if ch == "," else
                                          f"shift-{ch.lower()}" if ch.isupper() else ch))
                    time.sleep(0.12)
            elif op == "move":
                x, y = (int(v) for v in arg.split())
                qmp.move(x, y)
                time.sleep(0.3)
            elif op == "click":
                v = [int(t) for t in arg.split()]
                qmp.click(v[0], v[1], v[2] if len(v) > 2 else 1)
                time.sleep(0.4)
            elif op in ("shot", "fullshot"):
                shot(hmp, arg, raw_dir, op == "fullshot")
            else:
                print("unknown command:", line)
        if "--keep" not in argv and "--quit" not in argv and attach:
            pass
        elif "--keep" not in argv:
            hmp.cmd("quit")
    finally:
        if proc and "--keep" not in argv:
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
