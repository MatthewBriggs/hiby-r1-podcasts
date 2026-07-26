#!/usr/bin/env python3
"""Inject a touch tap on the R1 over ADB.

The R1 accepts raw Linux input_event packets written to its touch node. Event
layout and the multi-frame press pattern follow the audiobook mod's
tools/adb_inject_touch_event.py — a single frame is often missed, so the press
is held for several frames before release.

Usage:  tap.py X Y  [--node event1]
"""
import argparse
import base64
import os
import struct
import subprocess

ADB = os.environ.get(
    "ADB", "/opt/homebrew/share/android-commandlinetools/platform-tools/adb"
)
TOUCH_FRAMES = 8


def ev(etype: int, code: int, value: int) -> bytes:
    # struct input_event on 32-bit: two longs for the timeval, then type/code/value.
    return struct.pack("<llHHl", 0, 0, etype, code, value)


def abs_frame(x: int, y: int, press: bool) -> list[bytes]:
    out = [
        ev(3, 57, 0),    # ABS_MT_TRACKING_ID
        ev(3, 58, 63),   # ABS_MT_PRESSURE
        ev(3, 48, 9),    # ABS_MT_TOUCH_MAJOR
        ev(3, 53, x),    # ABS_MT_POSITION_X
        ev(3, 54, y),    # ABS_MT_POSITION_Y
        ev(0, 2, 0),     # SYN_MT_REPORT
    ]
    if press:
        out.append(ev(1, 330, 1))  # BTN_TOUCH down
    out.append(ev(0, 0, 0))        # SYN_REPORT
    return out


def tap_stream(x: int, y: int) -> bytes:
    events = abs_frame(x, y, press=True)
    for _ in range(TOUCH_FRAMES - 1):
        events += abs_frame(x, y, press=False)
    events += [ev(1, 330, 0), ev(0, 2, 0), ev(0, 0, 0)]  # release
    return b"".join(events)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("x", type=int)
    ap.add_argument("y", type=int)
    ap.add_argument("--node", default="event1")
    args = ap.parse_args()

    blob = base64.b64encode(tap_stream(args.x, args.y)).decode()
    # Stage as base64 and decode on-device: adb shell would mangle raw bytes.
    cmd = f"echo '{blob}' | base64 -d > /tmp/.tap.bin && cat /tmp/.tap.bin > /dev/input/{args.node}"
    r = subprocess.run([ADB, "shell", cmd], capture_output=True, text=True)
    print(f"tap ({args.x},{args.y}) -> /dev/input/{args.node} {r.stdout.strip()}{r.stderr.strip()}")


if __name__ == "__main__":
    main()
