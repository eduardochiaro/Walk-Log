#!/usr/bin/env python3
"""Generate 18x18 action-bar icon PNGs for the Pebble app.
Uses only the standard library (struct + zlib) — no Pillow required.
"""

import math
import os
import struct
import zlib


def _chunk(ctype: bytes, data: bytes) -> bytes:
    body = ctype + data
    crc = struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    return struct.pack(">I", len(data)) + body + crc


def make_png(width: int, height: int, pixels: list[list[bool]]) -> bytes:
    """Create a minimal RGBA PNG from a 2-D boolean grid.
    True = black/opaque, False = fully transparent.
    """
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)

    raw = b""
    for y in range(height):
        raw += b"\x00"  # filter: None
        for x in range(width):
            if pixels[y][x]:
                raw += b"\x00\x00\x00\xff"  # black, opaque
            else:
                raw += b"\x00\x00\x00\x00"  # transparent
    compressed = zlib.compress(raw)

    return sig + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", compressed) + _chunk(b"IEND", b"")


# ---------------------------------------------------------------------------
#  Icon definitions  (18 x 18)
# ---------------------------------------------------------------------------

W, H = 18, 18


def _empty():
    return [[False] * W for _ in range(H)]


def make_play():
    """Right-pointing triangle (play)."""
    p = _empty()
    cx, cy = 10, 8
    for y in range(H):
        half = max(0, 6 - abs(y - cy))
        for x in range(cx - half * 2, cx + 1):
            if 0 <= x < W:
                p[y][x] = True
    return p


def make_stop():
    """Filled square (stop)."""
    p = _empty()
    for y in range(4, 14):
        for x in range(4, 14):
            p[y][x] = True
    return p


def make_settings():
    """Simple gear / cog."""
    p = _empty()
    cx, cy = 9, 9
    for y in range(H):
        for x in range(W):
            d2 = (x - cx) ** 2 + (y - cy) ** 2
            # Ring
            if 9 <= d2 <= 30:
                p[y][x] = True
            # Centre dot
            if d2 <= 4:
                p[y][x] = True
    # Six teeth
    for i in range(6):
        angle = i * math.pi / 3
        for r in range(5, 9):
            tx = int(round(cx + r * math.cos(angle)))
            ty = int(round(cy + r * math.sin(angle)))
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    nx, ny = tx + dx, ty + dy
                    if 0 <= nx < W and 0 <= ny < H:
                        p[ny][nx] = True
    return p


def make_logs():
    """Three horizontal bars with bullets (list icon)."""
    p = _empty()
    for bar_y in (4, 9, 14):
        # bullet
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx * dx + dy * dy <= 1:
                    ny, nx = bar_y + dy, 2 + dx
                    if 0 <= ny < H and 0 <= nx < W:
                        p[ny][nx] = True
        # line
        for x in range(5, 16):
            p[bar_y][x] = True
            if bar_y + 1 < H:
                p[bar_y + 1][x] = True
    return p


# ---------------------------------------------------------------------------
#  Write files
# ---------------------------------------------------------------------------

out_dir = os.path.join(os.path.dirname(__file__), "resources", "images")
os.makedirs(out_dir, exist_ok=True)

icons = {
    "icon_play.png":     make_play(),
    "icon_stop.png":     make_stop(),
    "icon_settings.png": make_settings(),
    "icon_logs.png":     make_logs(),
}

for name, pixels in icons.items():
    path = os.path.join(out_dir, name)
    with open(path, "wb") as f:
        f.write(make_png(W, H, pixels))
    print(f"  wrote {path}")

print("Done – icons generated.")
