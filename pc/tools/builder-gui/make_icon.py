"""Writes runner.ico: the app's launcher icon (android/res/drawable/ic_launcher_*.xml) as a Windows icon.

  python -I outputs/mirrors-edge-native/pc/tools/builder-gui/make_icon.py
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SKY, WHITE, SHADE, RED = (0xCF, 0xE2, 0xEF), (255, 255, 255), (0xB9, 0xCA, 0xD6), (0xE2, 0x4B, 0x4A)
RECTS = [(18, 60, 18, 48, WHITE), (36, 60, 6, 48, SHADE), (46, 42, 18, 66, WHITE), (64, 42, 6, 66, SHADE), (74, 64, 18, 44, WHITE)]
ROUTE = [(18, 60), (42, 60), (46, 42), (70, 42), (74, 64), (92, 64)]
# The visible part of a 108-unit adaptive icon, as launchers show it.
LEFT, TOP, SPAN = 12, 22, 86


def near_route(x, y):
    for (ax, ay), (bx, by) in zip(ROUTE, ROUTE[1:]):
        dx, dy = bx - ax, by - ay
        t = max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
        if math.hypot(x - ax - t * dx, y - ay - t * dy) <= 2.5:
            return True
    return False


def colour(x, y):
    if near_route(x, y):
        return RED
    for rx, ry, w, h, c in RECTS:
        if rx <= x < rx + w and ry <= y < ry + h:
            return c
    return SKY


def inside(px, py, size):
    radius = size * 0.2
    cx, cy = min(max(px, radius), size - radius), min(max(py, radius), size - radius)
    return math.hypot(px - cx, py - cy) <= radius


def png(size):
    ss = 4 if size < 256 else 2
    rows = []
    for j in range(size):
        row = bytearray(b"\0")
        for i in range(size):
            r = g = b = a = 0
            for v in range(ss):
                for u in range(ss):
                    px, py = i + (u + 0.5) / ss, j + (v + 0.5) / ss
                    if inside(px, py, size):
                        c = colour(LEFT + px * SPAN / size, TOP + py * SPAN / size)
                        r += c[0]; g += c[1]; b += c[2]; a += 1
            n = ss * ss
            row += bytes((r // a, g // a, b // a, a * 255 // n)) if a else b"\0\0\0\0"
        rows.append(bytes(row))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


sizes = (16, 24, 32, 48, 64, 256)
images = [png(s) for s in sizes]
offset = 6 + 16 * len(sizes)
head = struct.pack("<HHH", 0, 1, len(sizes))
for size, image in zip(sizes, images):
    head += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(image), offset)
    offset += len(image)
with open(os.path.join(HERE, "runner.ico"), "wb") as f:
    f.write(head + b"".join(images))
print("runner.ico", offset)
