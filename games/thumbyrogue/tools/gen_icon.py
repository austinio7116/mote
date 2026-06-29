#!/usr/bin/env python3
"""ThumbyRogue launcher icon (60x60) — an isometric stone staircase descending
into shadow under torchlight, with teal crystal motes by the steps. Captures the
game's pillar: an endless descent. Writes games/thumbyrogue/icon.png; bake with
`mote bake games/thumbyrogue`.
"""
import os, numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
W = 60
rng = np.random.default_rng(13)

img = np.zeros((W, W, 3), np.uint8)
img[:] = (18, 16, 26)                                   # near-black dungeon slate

def poly(pts):
    m = Image.new("L", (W, W), 0); ImageDraw.Draw(m).polygon(pts, fill=255)
    return np.array(m) > 0

def paint(m, col, var):
    n = int(m.sum())
    if n:
        img[m] = np.clip(np.array(col) + rng.integers(-var, var + 1, size=(n, 3)), 0, 255)

# A 2-wide staircase descending to the lower-right, each tread darker than the
# last so the eye reads "down into the depths". Dimetric treads (rhombus tops +
# lit risers).
steps = 5
x0, y0 = 8, 8          # top-left of the top tread
dx, dy = 7, 7          # per-step descent offset
tw, th = 30, 10        # tread footprint
for i in range(steps):
    sx = x0 + i * dx
    sy = y0 + i * dy
    shade = 1.0 - i * 0.16                              # fade with depth
    top   = [(sx, sy), (sx + tw, sy + 4), (sx + tw, sy + 4 + th), (sx, sy + th)]
    riser = [(sx, sy + th), (sx + tw, sy + 4 + th), (sx + tw, sy + 4 + th + 5), (sx, sy + th + 5)]
    paint(poly(top),   (int(120 * shade), int(118 * shade), int(132 * shade)), 8)   # lit stone tread
    paint(poly(riser), (int(60 * shade),  int(58 * shade),  int(70 * shade)),  6)   # shadowed riser

# Torch glow at top-left — warm radial light spilling over the first steps.
yy, xx = np.mgrid[0:W, 0:W]
d = np.sqrt((xx - 9) ** 2 + (yy - 7) ** 2)
glow = np.clip(1.0 - d / 26.0, 0, 1) ** 1.6
img[..., 0] = np.clip(img[..., 0] + (glow * 150).astype(int), 0, 255)
img[..., 1] = np.clip(img[..., 1] + (glow * 86).astype(int), 0, 255)
img[..., 2] = np.clip(img[..., 2] + (glow * 26).astype(int), 0, 255)
# torch flame pip
img[5:9, 7:10] = (255, 196, 80)
img[9:12, 8:10] = (210, 110, 40)

# Teal crystal shards flanking the lower steps (the game's stairwell motes).
for cx, cy, h in [(46, 40, 7), (12, 44, 6)]:
    shard = [(cx, cy - h), (cx + 3, cy), (cx, cy + 2), (cx - 3, cy)]
    paint(poly(shard), (60, 200, 196), 18)
    img[max(0, cy - h), cx] = (180, 255, 250)
# a few rising teal motes
for _ in range(9):
    mx = rng.integers(40, 52); my = rng.integers(26, 44)
    img[my, mx] = (120, 230, 220)

out = Image.fromarray(img, "RGB")
d = ImageDraw.Draw(out)
# step edge outlines for crisp readability at 60px
for i in range(steps):
    sx = x0 + i * dx; sy = y0 + i * dy
    d.line([(sx, sy + th), (sx + tw, sy + 4 + th)], fill=(26, 24, 34), width=1)
out.save(os.path.join(ROOT, "icon.png"))
print("wrote", os.path.join(ROOT, "icon.png"))
