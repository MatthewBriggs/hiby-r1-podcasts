#!/usr/bin/env python3
"""Draw the Podcasts launcher icon: a microphone on a rounded square.

140x140 RGBA PNG to match the stock launcher icons. Rendered by hand with 4x
supersampling rather than pulling in an image library, and written with zlib —
the same approach as tools/fbgrab.py.
"""
import struct
import sys
import zlib

W = H = 140
SS = 4                      # supersampling factor
RADIUS = 34                 # corner radius of the tile
BG = (139, 92, 246)         # launcher purple
FG = (255, 255, 255)


def rounded_square(x, y):
    """Inside the rounded square?"""
    r = RADIUS
    if x < r and y < r:
        return (x - r) ** 2 + (y - r) ** 2 <= r * r
    if x > W - r and y < r:
        return (x - (W - r)) ** 2 + (y - r) ** 2 <= r * r
    if x < r and y > H - r:
        return (x - r) ** 2 + (y - (H - r)) ** 2 <= r * r
    if x > W - r and y > H - r:
        return (x - (W - r)) ** 2 + (y - (H - r)) ** 2 <= r * r
    return 0 <= x <= W and 0 <= y <= H


def microphone(x, y):
    """Capsule head, arc cradle, stem and base — a mic in silhouette."""
    cx = W / 2.0

    # Head: capsule from y=34 to y=76, half-width 15.
    hw, top, bot = 15.0, 34.0, 76.0
    if top <= y <= bot and abs(x - cx) <= hw:
        return True
    for cy in (top, bot):
        if (x - cx) ** 2 + (y - cy) ** 2 <= hw * hw:
            return True

    # Cradle: lower half of an annulus, outer r=32, thickness 7.
    ay, ar, th = 72.0, 32.0, 7.0
    d = ((x - cx) ** 2 + (y - ay) ** 2) ** 0.5
    if y >= ay and ar - th <= d <= ar:
        return True

    # Stem and base.
    if 104 <= y <= 116 and abs(x - cx) <= 4:
        return True
    if 112 <= y <= 119 and abs(x - cx) <= 20:
        return True
    return False


def render(selected):
    bg = tuple(min(255, c + 40) for c in BG) if selected else BG
    rows = bytearray()
    for py in range(H):
        rows.append(0)                      # PNG filter: none
        for px in range(W):
            r = g = b = a = 0
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS
                    y = py + (sy + 0.5) / SS
                    if not rounded_square(x, y):
                        continue
                    hits += 1
                    if microphone(x, y):
                        r += FG[0]; g += FG[1]; b += FG[2]
                    else:
                        r += bg[0]; g += bg[1]; b += bg[2]
            n = SS * SS
            if hits == 0:
                rows += bytes((0, 0, 0, 0))
            else:
                # Average only over covered samples, then scale alpha by coverage.
                rows += bytes((r // hits, g // hits, b // hits, (hits * 255) // n))
    return bytes(rows)


def write_png(path, rows):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))  # 6 = RGBA
    png += chunk(b"IDAT", zlib.compress(rows, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)
    print(f"{path}: {W}x{H} RGBA")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    write_png(f"{out}/about.png", render(False))
    write_png(f"{out}/about_s.png", render(True))
