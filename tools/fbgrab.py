#!/usr/bin/env python3
"""Grab the R1's framebuffer over ADB and write a PNG.

The panel is 480x800 at 16bpp (RGB565); the virtual height is doubled for
double buffering, so a plain read of /dev/fb0 lands on whichever buffer is
first. Pass --pan to grab the second buffer instead.

Writes PNG with no third-party modules — zlib and struct are enough.
"""
import argparse
import os
import struct
import subprocess
import sys
import zlib

W, H, BPP = 480, 800, 2
FRAME = W * H * BPP
ADB = os.environ.get(
    "ADB", "/opt/homebrew/share/android-commandlinetools/platform-tools/adb"
)


def grab(pan: bool) -> bytes:
    # 768000 = 750 * 1024, so use 1 KiB blocks to keep the skip an exact count.
    blocks = FRAME // 1024
    skip = blocks if pan else 0
    # This adbd predates `exec-out`, and plain `shell` mangles binary with LF
    # translation, so move the frame as base64 and decode here.
    cmd = f"dd if=/dev/fb0 bs=1024 skip={skip} count={blocks} 2>/dev/null | base64"
    out = subprocess.run([ADB, "shell", cmd], capture_output=True).stdout
    import base64
    raw = base64.b64decode(b"".join(out.split()), validate=False)
    if len(raw) > FRAME:
        raw = raw[:FRAME]
    if len(raw) < FRAME:
        sys.exit(f"short read: {len(raw)} of {FRAME} bytes")
    return raw


def rgb565_to_rows(raw: bytes) -> bytes:
    out = bytearray()
    for y in range(H):
        out.append(0)  # PNG filter type: none
        row = raw[y * W * 2:(y + 1) * W * 2]
        for x in range(W):
            v = row[x * 2] | (row[x * 2 + 1] << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            # Replicate high bits so full-scale maps to 255.
            out.append((r << 3) | (r >> 2))
            out.append((g << 2) | (g >> 4))
            out.append((b << 3) | (b >> 2))
    return bytes(out)


def write_png(path: str, rows: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="screen.png")
    ap.add_argument("--pan", action="store_true", help="grab the second buffer")
    args = ap.parse_args()

    raw = grab(args.pan)
    write_png(args.out, rgb565_to_rows(raw))
    nonzero = sum(1 for b in raw if b)
    print(f"{args.out}: {W}x{H}  {nonzero * 100 // len(raw)}% non-zero bytes")


if __name__ == "__main__":
    main()
