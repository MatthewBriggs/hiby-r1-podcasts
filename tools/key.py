#!/usr/bin/env python3
"""Inject a key press on the R1 over ADB.

Volume and power are on the gpio/adc key nodes, not the touch node, so the
target device differs from tap.py. Same base64 transport: this adbd predates
`exec-out` and plain `shell` corrupts binary.

Usage:  key.py volup|voldown|power|<code> [--node event0]
"""
import argparse
import base64
import os
import struct
import subprocess

ADB = os.environ.get(
    "ADB", "/opt/homebrew/share/android-commandlinetools/platform-tools/adb"
)

NAMES = {
    "volup": 115,      # KEY_VOLUMEUP
    "voldown": 114,    # KEY_VOLUMEDOWN
    "power": 116,      # KEY_POWER
    "playpause": 164,
    "next": 163,
    "prev": 165,
}


def ev(etype, code, value):
    return struct.pack("<llHHl", 0, 0, etype, code, value)


def press(code):
    return b"".join([
        ev(1, code, 1), ev(0, 0, 0),      # down + SYN
        ev(1, code, 0), ev(0, 0, 0),      # up + SYN
    ])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("key")
    ap.add_argument("--node", default="event0")
    args = ap.parse_args()

    code = NAMES.get(args.key, None)
    if code is None:
        code = int(args.key)

    blob = base64.b64encode(press(code)).decode()
    cmd = (f"echo '{blob}' | base64 -d > /tmp/.key.bin && "
           f"cat /tmp/.key.bin > /dev/input/{args.node}")
    subprocess.run([ADB, "shell", cmd], capture_output=True, text=True)
    print(f"key {args.key} (code {code}) -> /dev/input/{args.node}")


if __name__ == "__main__":
    main()
