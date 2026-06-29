#!/usr/bin/env python3
"""ThumbyCraft launcher icon — the classic isometric grass block (60x60).

Recreates the old craft lobby icon: a 3D dimetric cube with a grassy green top,
a grass overhang fringe, and textured dirt sides (lit left, shadowed right).
Writes games/thumbycraft/icon.png; bake with `mote bake games/thumbycraft`.
"""
import os, numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
W = 60
rng = np.random.default_rng(7)

# cube vertices (dimetric / Minecraft-style 2:1)
T  = (30, 4)            # top apex
TL = (4, 17); TR = (56, 17)
C  = (30, 30)           # centre (bottom of the top rhombus)
BL = (4, 43); BR = (56, 43)
B  = (30, 56)           # bottom apex

top   = [T, TR, C, TL]
left  = [TL, C, B, BL]
right = [C, TR, BR, B]

img = np.zeros((W, W, 3), np.uint8)
img[:] = (46, 49, 62)                                   # soft slate background

def mask(pts):
    m = Image.new("L", (W, W), 0); ImageDraw.Draw(m).polygon(pts, fill=255)
    return np.array(m) > 0

def paint(m, col, var):
    n = int(m.sum())
    img[m] = np.clip(np.array(col) + rng.integers(-var, var + 1, size=(n, 3)), 0, 255)

# faces: dirt sides (left lit, right shadowed) then grass top
paint(mask(left),  (122, 84, 50), 11)
paint(mask(right), (96, 64, 38),  9)
paint(mask(top),   (104, 176, 78), 13)

# grass overhang fringe along the top edge of each side face
fr = 6
paint(mask([TL, C, (C[0], C[1] + fr), (TL[0], TL[1] + fr)]), (86, 150, 64), 10)   # left side grass
paint(mask([C, TR, (TR[0], TR[1] + fr), (C[0], C[1] + fr)]), (72, 128, 54), 9)    # right side grass
# a few grass drips down the dirt
for _ in range(7):
    x = rng.integers(8, 52); y = rng.integers(20, 30)
    if mask(left + right)[y, x]:
        img[y, x] = (80, 140, 58); img[min(W-1, y+1), x] = (66, 116, 50)

out = Image.fromarray(img, "RGB")
d = ImageDraw.Draw(out)
edge = (32, 26, 18)
for a, b in [(T, TR), (TR, C), (C, TL), (TL, T),       # top rhombus
             (TL, BL), (BL, B), (B, BR), (BR, TR),     # silhouette sides
             (C, B)]:                                  # centre vertical
    d.line([a, b], fill=edge, width=1)

out.save(os.path.join(ROOT, "icon.png"))
print("wrote", os.path.join(ROOT, "icon.png"))
