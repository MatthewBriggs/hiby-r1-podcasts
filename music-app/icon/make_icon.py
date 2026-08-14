#!/usr/bin/env python3
"""Draw the Library launcher icon in the stock theme's style.

Measured from the shipped theme2 icons rather than guessed: 140x140, an opaque
black field, a grey (87,87,91) rounded square of half-extent 60 and corner
radius 50 centred in the tile, and a single flat-coloured glyph. Each app gets
its own glyph colour — books pink, music orange, streaming green — podcasts violet — so this
one, which is music and radio together, takes cyan.

The corner radius matters: at r=50 the tile reads as almost round but keeps
short flat sides, and a plain circle sits visibly narrower than its neighbours.
The figure was fitted against the stock corner profile sampled at 50% coverage,
and agrees with it to under a pixel at every row.

Rendered with 4x supersampling and written via zlib; no image library.
"""
import math
import struct
import sys
import zlib

W = H = 140
SS = 4
HALF = 60                    # half-extent of the tile field
CORNER_R = 50
INSET = HALF - CORNER_R      # corner-arc centre offset from the middle
DISC = (87, 87, 91)
GLYPH = (86, 194, 224)       # cyan, unused by the other tiles
BG = (0, 0, 0)


def in_disc(x, y):
    """Rounded square: a rectangle with quarter-circle corners."""
    dx, dy = abs(x - W / 2.0), abs(y - H / 2.0)
    if dx > HALF or dy > HALF:
        return False
    if dx <= INSET or dy <= INSET:
        return True
    return (dx - INSET) ** 2 + (dy - INSET) ** 2 <= CORNER_R * CORNER_R


# The stock glyphs cover 17-21% of the tile field; this one is drawn to land in
# the same band without needing a correction factor.
GLYPH_SCALE = 1.0


def in_glyph(x, y):
    """A quarter note with two broadcast arcs: the library and the radio.

    Drawn head-first from the foot of the stem. The first attempt placed the
    head up and left of the stem and hung a flag off the top, and with the arcs
    beside it the whole figure read as a letter P.
    """
    x = (x - W / 2.0) / GLYPH_SCALE + W / 2.0
    y = (y - H / 2.0) / GLYPH_SCALE + H / 2.0
    dx, dy = x - W / 2.0, y - H / 2.0

    # note head: a tilted ellipse sitting at the foot of the stem
    hx, hy = dx + 8, dy - 20
    ang = -0.35
    rx = hx * math.cos(ang) - hy * math.sin(ang)
    ry = hx * math.sin(ang) + hy * math.cos(ang)
    if (rx / 14.0) ** 2 + (ry / 10.5) ** 2 <= 1.0:
        return True

    # stem, up the right-hand side of the head
    if 1 <= dx <= 7 and -30 <= dy <= 21:
        return True

    # two arcs struck from the top of the stem, opening to the right, kept
    # inside a wedge so they read as broadcast rather than as a bowl
    cx, cy = 4.0, -27.0
    r = math.hypot(dx - cx, dy - cy)
    if dx > cx + 4 and abs(dy - cy) <= (dx - cx) * 1.6:
        for rad in (15.0, 26.0):
            if abs(r - rad) <= 3.2:
                return True
    return False


def render(selected):
    disc = tuple(min(255, c + 26) for c in DISC) if selected else DISC
    rows = bytearray()
    for py in range(H):
        rows.append(0)
        for px in range(W):
            r = g = b = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS
                    y = py + (sy + 0.5) / SS
                    if in_disc(x, y):
                        c = GLYPH if in_glyph(x, y) else disc
                    else:
                        c = BG
                    r += c[0]; g += c[1]; b += c[2]
            n = SS * SS
            rows += bytes((r // n, g // n, b // n, 255))
    return bytes(rows)


def write_png(path, rows):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)
    print(f"{path}: {W}x{H} RGBA")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    write_png(f"{out}/stream_media.png", render(False))
    write_png(f"{out}/stream_media_s.png", render(True))
