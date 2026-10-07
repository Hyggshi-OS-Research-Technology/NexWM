#!/usr/bin/env python3
"""login/sddm/tools/make-background.py — draws the background of the HDE login theme.

The theme ships `login/sddm/hde/assets/background.png`, produced by this script (no image library needed: a PNG is
written by hand with zlib, lossless and small: the colours are rounded to even values, which keeps the file under a
couple of hundred KiB in the repository). It is the HDE login look: deep indigo-teal, a soft glow behind the login card, a few
light streaks, and a dark vignette at the edges so white text stays readable. The HDE logo colours (the blue of the
default accent, #3584e4) run through it.

    python3 login/sddm/tools/make-background.py [WIDTH HEIGHT OUT]

The result is deterministic: same command, same file, so a rebuild does not churn the repository.
"""
import math
import struct
import sys
import zlib

WIDTH, HEIGHT, OUT = 2560, 1440, "login/sddm/hde/assets/background.png"
if len(sys.argv) >= 3:
    WIDTH, HEIGHT = int(sys.argv[1]), int(sys.argv[2])
if len(sys.argv) >= 4:
    OUT = sys.argv[3]

# the colours of the theme (see theme.conf): a deep background with HDE's accent glow
TOP = (0x16, 0x1C, 0x2B)          # 161c2b
BOTTOM = (0x0B, 0x0F, 0x18)       # 0b0f18
ACCENT = (0x35, 0x84, 0xE4)       # 3584e4 — HDE's default accent
GLOW = (0x6F, 0xB1, 0xF5)         # lightened accent


def smooth(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def add(c, r, g, b):
    return (min(255, c[0] + int(r)), min(255, c[1] + int(g)), min(255, c[2] + int(b)))


def main():
    rows = []
    cx, cy = WIDTH * 0.5, HEIGHT * 0.46           # where the login card sits (the glow is behind it)
    radius = min(WIDTH, HEIGHT) * 0.62
    for y in range(HEIGHT):
        v = y / max(1, HEIGHT - 1)
        base = mix(TOP, BOTTOM, smooth(v * 1.15))
        row = bytearray()
        for x in range(WIDTH):
            u = x / max(1, WIDTH - 1)
            col = add(base, 6 * u, 4 * u, 10 * u)
            # the glow behind the card (two soft ellipses)
            dx, dy = (x - cx) / (radius * 1.25), (y - cy) / (radius * 0.95)
            d = math.sqrt(dx * dx + dy * dy)
            glow = smooth(1.0 - d) ** 2
            col = mix(col, mix(col, GLOW, 0.55), glow * 0.5)
            # a wide diagonal streak, like a light from the top left
            s = (u * 1.6 - v * 1.1) + 0.35
            streak = smooth(1.0 - abs(s - 0.5) * 3.2) ** 3
            col = add(col, GLOW[0] * 0.06 * streak, GLOW[1] * 0.06 * streak, GLOW[2] * 0.07 * streak)
            # a faint horizon line low on the screen
            h = smooth(1.0 - abs(v - 0.82) * 26)
            col = add(col, 10 * h, 12 * h, 18 * h)
            # vignette: darker at the edges, so the text and the card stand out
            ex = min(1.0, abs(u - 0.5) * 2.35)
            ey = min(1.0, abs(v - 0.5) * 2.35)
            edge = smooth(max(ex, ey) ** 1.6)
            col = mix(col, (0x08, 0x0A, 0x10), edge * 0.55)
            # rounded to even values: smaller PNG (the gradients are smooth, so nothing is lost) and no banding
            col = (col[0] & 0xFE, col[1] & 0xFE, col[2] & 0xFE)
            row += bytes(col)
        rows.append(bytes(row))

    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0))   # 8 bit, truecolour (RGB)
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(OUT, "wb") as f:
        f.write(png)
    print(f"{OUT}: {WIDTH}x{HEIGHT}, {len(png) / 1024:.0f} KiB")


if __name__ == "__main__":
    main()
