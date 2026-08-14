#!/usr/bin/env python3
"""Draw the Podcasts launcher icon in the stock theme's style.

Measured from the shipped theme2 icons rather than guessed: 140x140, an opaque
black field, a grey (87,87,91) rounded square of half-extent 60 and corner
radius 50 centred in the tile, and a single flat-coloured glyph. Each app gets
its own glyph colour — books pink, music orange, streaming green — so podcasts
take violet.

The corner radius matters: at r=50 the tile reads as almost round but keeps
short flat sides, and a plain circle sits visibly narrower than its neighbours.
The figure was fitted against the stock corner profile sampled at 50% coverage,
and agrees with it to under a pixel at every row.

Rendered with 4x supersampling and written via zlib; no image library.
"""
import struct
import sys
import zlib

W = H = 140
SS = 4
HALF = 60                    # half-extent of the tile field
CORNER_R = 50
INSET = HALF - CORNER_R      # corner-arc centre offset from the middle
DISC = (87, 87, 91)
GLYPH = (167, 139, 250)      # violet, unused by the other tiles
BG = (0, 0, 0)


def in_disc(x, y):
    """Rounded square: a rectangle with quarter-circle corners."""
    dx, dy = abs(x - W / 2.0), abs(y - H / 2.0)
    if dx > HALF or dy > HALF:
        return False
    if dx <= INSET or dy <= INSET:
        return True
    return (dx - INSET) ** 2 + (dy - INSET) ** 2 <= CORNER_R * CORNER_R


# The stock glyphs cover 17-21% of the tile field. The microphone was drawn to
# fit the old circular field and came out at 15%, reading thin beside its
# neighbours, so it is scaled about the centre to sit in the same band.
GLYPH_SCALE = 1.12


def in_mic(x, y):
    """Capsule head, cradle arc, stem and base."""
    x = (x - W / 2.0) / GLYPH_SCALE + W / 2.0
    y = (y - H / 2.0) / GLYPH_SCALE + H / 2.0
    cx = W / 2.0

    hw, top, bot = 13.0, 42.0, 74.0          # head capsule
    if top <= y <= bot and abs(x - cx) <= hw:
        return True
    for cy in (top, bot):
        if (x - cx) ** 2 + (y - cy) ** 2 <= hw * hw:
            return True

    ay, ar, th = 70.0, 27.0, 5.5             # cradle: lower half of an annulus
    d = ((x - cx) ** 2 + (y - ay) ** 2) ** 0.5
    if y >= ay and ar - th <= d <= ar:
        return True

    if 97 <= y <= 106 and abs(x - cx) <= 3.0:       # stem
        return True
    if 103 <= y <= 108 and abs(x - cx) <= 16.0:     # base
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
                        c = GLYPH if in_mic(x, y) else disc
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
    write_png(f"{out}/about.png", render(False))
    write_png(f"{out}/about_s.png", render(True))
