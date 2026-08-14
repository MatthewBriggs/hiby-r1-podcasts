#!/usr/bin/env python3
"""Inject a key press on the R1 over ADB.

Companion to tap.py/swipe.py, which only speak touch. The app grabs the key
nodes with EVIOCGRAB, so writing to the node it grabbed is how a key reaches
it -- there is no other way in from outside.

Codes worth knowing: 116 power (the screen lock), 114/115 volume down/up,
163 next, 165 previous, 164 play/pause.

Usage:  key.py CODE [--node event0] [--hold-ms 40]
"""
import argparse
import base64
import struct
import subprocess
import os

ADB = os.environ.get(
    "ADB", "/opt/homebrew/share/android-commandlinetools/platform-tools/adb"
)


def ev(etype: int, code: int, value: int) -> bytes:
    # struct input_event on 32-bit: two longs of timeval, then type/code/value.
    return struct.pack("<llHHl", 0, 0, etype, code, value)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("code", type=int)
    ap.add_argument("--node", default="event0")
    args = ap.parse_args()

    stream = b"".join([
        ev(1, args.code, 1), ev(0, 0, 0),      # press + SYN_REPORT
        ev(1, args.code, 0), ev(0, 0, 0),      # release + SYN_REPORT
    ])
    blob = base64.b64encode(stream).decode()
    cmd = (f"echo '{blob}' | base64 -d > /tmp/.key.bin && "
           f"cat /tmp/.key.bin > /dev/input/{args.node}")
    r = subprocess.run([ADB, "shell", cmd], capture_output=True, text=True)
    print(f"key {args.code} -> /dev/input/{args.node} "
          f"{r.stdout.strip()}{r.stderr.strip()}")


if __name__ == "__main__":
    main()
