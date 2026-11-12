#!/usr/bin/env python3
"""Drive MintOS in headless QEMU over QMP (web-engine ports 45462 HMP / 45472 QMP).

  qdrive.py boot                 start QEMU headless (background) and pick native MintDE
  qdrive.py shot NAME            screenshot -> SCRATCH/NAME.png (full) and NAME-small.png (1152x648)
  qdrive.py key COMBO...         e.g. meta_l-spc ret a shift-a
  qdrive.py type TEXT            types ASCII text
  qdrive.py click X Y [N]        left click at screen pixel (1920x1080), N clicks
  qdrive.py move X Y
  qdrive.py scroll X Y DY        wheel at position
  qdrive.py quit
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = "/Users/jakubruzicka/Documents/mintOS/worktrees/web-engine"
SCRATCH = os.path.dirname(os.path.abspath(__file__))
QMP_PORT = 45472
HMP_PORT = 45462
SW, SH = 1920, 1080


def qmp(cmds):
    s = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=20)
    f = s.makefile("rw")
    json.loads(f.readline())
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush()
    json.loads(f.readline())
    out = []
    for c in cmds:
        f.write(json.dumps(c) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r:
                out.append(r)
                break
    s.close()
    return out


KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", ":": "shift-semicolon", ";": "semicolon", ",": "comma",
          "?": "shift-slash", "=": "equal", "_": "shift-minus", "&": "shift-7", "'": "apostrophe", "\n": "ret",
          "@": "shift-2", "#": "shift-3", "+": "shift-equal", "(": "shift-9", ")": "shift-0", "!": "shift-1", "%": "shift-5"}


def key(combo):
    keys = [{"type": "qcode", "data": k} for k in combo.split("-")] if combo != "minus" else [{"type": "qcode", "data": "minus"}]
    if combo.startswith("shift-minus"):
        keys = [{"type": "qcode", "data": "shift"}, {"type": "qcode", "data": "minus"}]
    return {"execute": "send-key", "arguments": {"keys": keys, "hold-time": 60}}


def type_text(text):
    cmds = []
    for ch in text:
        if ch.isalpha() and ch.isupper():
            cmds.append(key("shift-" + ch.lower()))
        elif ch.isalnum():
            cmds.append(key(ch))
        else:
            cmds.append(key(KEYMAP[ch]))
    for c in cmds:
        qmp([c])
        time.sleep(0.07)


def abs_move(x, y):
    return {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (SW - 1))}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (SH - 1))}}]}}


def btn(down):
    return {"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}}


def click(x, y, n=1):
    qmp([abs_move(x, y)])
    time.sleep(0.15)
    for _ in range(n):
        qmp([btn(True)]); time.sleep(0.06); qmp([btn(False)]); time.sleep(0.08)


def scroll(x, y, dy):
    qmp([abs_move(x, y)])
    time.sleep(0.1)
    button = "wheel-down" if dy > 0 else "wheel-up"
    for _ in range(abs(dy)):
        qmp([{"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": button}}]}},
             {"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": button}}]}}])
        time.sleep(0.05)


def shot(name):
    path = os.path.join(SCRATCH, name + ".png")
    r = qmp([{"execute": "screendump", "arguments": {"filename": path, "format": "png"}}])
    small = os.path.join(SCRATCH, name + "-small.png")
    subprocess.run(["sips", "-z", "648", "1152", path, "--out", small], capture_output=True)
    print(path, r)


def boot():
    log = open(os.path.join(SCRATCH, "qemu.log"), "w")
    args = [os.path.join(ROOT, "tools/run_qemu.sh"), "--mintos-disk", "--headless", "--",
            "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0", "-device", "usb-tablet,bus=xhci.0",
            "-monitor", f"tcp:127.0.0.1:{HMP_PORT},server,nowait", "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait"]
    p = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT, start_new_session=True)
    print("qemu pid", p.pid)
    for _ in range(100):
        try:
            qmp([])
            break
        except OSError:
            time.sleep(0.2)
    # press 2 until the kernel log shows the native desktop starting
    com1 = os.path.join(ROOT, "build/com1-qemu.log")
    for i in range(80):
        time.sleep(0.5)
        try:
            qmp([key("2")])
        except OSError:
            pass
        try:
            data = open(com1, "rb").read()
        except OSError:
            data = b""
        if b"kernel" in data.lower() and i > 4:
            print("kernel started after", i * 0.5, "s")
            break
    print("pressed 2")


def main():
    cmd = sys.argv[1]
    if cmd == "boot": boot()
    elif cmd == "shot": shot(sys.argv[2])
    elif cmd == "key":
        for k in sys.argv[2:]:
            qmp([key(k)]); time.sleep(0.12)
    elif cmd == "type": type_text(" ".join(sys.argv[2:]))
    elif cmd == "click": click(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    elif cmd == "move": qmp([abs_move(int(sys.argv[2]), int(sys.argv[3]))])
    elif cmd == "scroll": scroll(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
    elif cmd == "quit": qmp([{"execute": "quit"}])


if __name__ == "__main__":
    main()
