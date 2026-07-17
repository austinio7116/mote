#!/usr/bin/env python3
"""ThumbPrince launcher icon — the crowned-thumbprint mark, small:
a white spiral-whorl print on dotted blueprint paper with an orbit ring and
a hand-sketched chalk crown (same concept as the title backdrop).

Run:
    python3 assets/make_icon.py && mote bake games/thumbprince
Writes ../icon.png (game root, 60x60); mote bake turns it into src/icon.h.
"""
import os
import math
import random
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

W = H = 60
BLUE  = (16, 26, 56)
GRID  = (38, 58, 104)
RING  = (110, 145, 200)
CHALK = (238, 244, 252)
CHALK2= (196, 214, 240)

img = Image.new("RGB", (W, H), BLUE)
px = img.putpixel

def hash2(x, y):
    h = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return h ^ (h >> 16)

# dotted drafting grid
for v in range(0, W, 12):
    for k in range(0, H, 3):
        px((v, k), GRID)
        px((k, v), GRID)

# one dotted orbit ring
for i in range(200):
    a = i * 2 * math.pi / 200
    if (i // 3) % 2:
        continue
    x = int(round(30 + 27 * math.cos(a)))
    y = int(round(31 + 26 * math.sin(a)))
    if 0 <= x < W and 0 <= y < H:
        px((x, y), RING)

# spiral-whorl thumbprint
CX, CY = 30.0, 35.0
RX, RY = 12.5, 16.0
TURNS = 5.5
for y in range(H):
    for x in range(W):
        nx = (x - CX) / RX
        ny = (y - CY) / RY
        taper = 1.0 - 0.16 * max(0.0, ny)
        r = math.hypot(nx / taper, ny)
        if r > 1.0:
            continue
        th = math.atan2(ny, nx / taper)
        s = (r * TURNS - th / (2 * math.pi)) % 1.0
        broken = hash2(x * 3 + y, y * 7 - x) % 13 == 0
        if s < 0.44 and not broken:
            px((x, y), CHALK if hash2(x, y) % 5 else CHALK2)

# chalk crown (small: single-pass sketchy strokes)
rng = random.Random(46)
def stroke(x0, y0, x1, y1):
    steps = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
    for i in range(steps):
        if rng.random() < 0.15:
            continue
        t = i / steps
        x = int(round(x0 + (x1 - x0) * t + rng.uniform(-0.5, 0.5)))
        y = int(round(y0 + (y1 - y0) * t + rng.uniform(-0.5, 0.5)))
        if 0 <= x < W and 0 <= y < H:
            px((x, y), CHALK if rng.random() < 0.8 else CHALK2)

BL, BR, BY = 19, 42, 19
pts = [(BL, BY), (18, 9), (25, 14), (30, 5), (36, 14), (43, 8), (BR, BY)]
for i in range(len(pts) - 1):
    stroke(*pts[i], *pts[i + 1])
stroke(BL, BY, BR, BY)
stroke(BL + 1, BY + 2, BR - 1, BY + 2)
for k in range(5):
    x0 = rng.uniform(BL + 2, BR - 6)
    stroke(x0, BY + 1, x0 + 4, BY - 3)

out = os.path.join(GAME, "icon.png")
img.save(out)
print("wrote", out)
