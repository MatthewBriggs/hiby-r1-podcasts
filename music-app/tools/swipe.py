#!/usr/bin/env python3
"""Inject a touch swipe on the R1 over ADB.

Same packet layout as tools/tap.py, but moving the contact between press and
release so the app sees a drag rather than a tap.

Usage:  swipe.py X1 Y1 X2 Y2 [steps]
"""
import base64, os, struct, subprocess, sys

ADB = os.environ.get("ADB", "/opt/homebrew/share/android-commandlinetools/platform-tools/adb")

def ev(t, c, v): return struct.pack("<llHHl", 0, 0, t, c, v)

def frame(x, y, press):
    out = [ev(3,57,0), ev(3,58,63), ev(3,48,9), ev(3,53,x), ev(3,54,y), ev(0,2,0)]
    if press: out.append(ev(1,330,1))
    out.append(ev(0,0,0))
    return out

x1,y1,x2,y2 = (int(a) for a in sys.argv[1:5])
steps = int(sys.argv[5]) if len(sys.argv) > 5 else 12
events = frame(x1, y1, True)
for i in range(1, steps + 1):
    events += frame(x1 + (x2-x1)*i//steps, y1 + (y2-y1)*i//steps, False)
events += [ev(1,330,0), ev(0,2,0), ev(0,0,0)]
blob = base64.b64encode(b"".join(events)).decode()
cmd = f"echo '{blob}' | base64 -d > /tmp/.swipe.bin && cat /tmp/.swipe.bin > /dev/input/event1"
subprocess.run([ADB, "shell", cmd], capture_output=True, text=True)
print(f"  swipe ({x1},{y1}) -> ({x2},{y2})")
