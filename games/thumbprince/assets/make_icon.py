#!/usr/bin/env python3
"""ThumbPrince launcher icon — a crowned THUMBPRINT drafted on blueprint paper
(the whole joke in one mark: Blue Prince -> blueprint, ThumbPrince -> thumbprint).

Run:
    python3 assets/make_icon.py && mote bake games/thumbprince
Writes ../icon.png (game root, 60x60); mote bake turns it into src/icon.h.
"""
import os
import math
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

W = H = 60
BLUE  = (22, 38, 76, 255)      # blueprint paper
GRID  = (46, 70, 122, 255)     # faint drafting grid
RIDGE = (168, 205, 250, 255)   # print ridges: pale drafting ink
RIDGE2= (120, 158, 215, 255)   # alternate ridge tone (depth)
GOLD  = (244, 202, 58, 255)
GOLDD = (176, 138, 26, 255)
GOLDH = (255, 240, 170, 255)

icon = Image.new("RGBA", (W, H), BLUE)
px = icon.putpixel

for v in range(0, W, 10):
    for y in range(H):
        px((v, y), GRID)
    for x in range(W):
        px((x, v), GRID)

# ---- the thumbprint: concentric ridge loops inside a thumb-shaped oval ----
CX, CY = 29.5, 36.0            # print centre
RX, RY = 15.0, 18.5            # thumb half-extents
NRINGS = 6.0                   # ridge loops

def hash2(x, y):
    h = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return h ^ (h >> 16)

for y in range(H):
    for x in range(W):
        dx = (x - CX) / RX
        dy = (y - CY) / RY
        # thumb shape: rounder at the top, gently tapered pad at the bottom
        taper = 1.0 - 0.18 * max(0.0, dy)
        t = math.sqrt((dx / taper) ** 2 + dy ** 2)
        if t > 1.0:
            continue
        # ridge bands with print-like breaks
        band = (t * NRINGS) % 1.0
        broken = hash2(int(x * 3 + y), int(y * 5 - x)) % 11 == 0
        if band < 0.42 and not broken:
            px((x, y), RIDGE if int(t * NRINGS) % 2 == 0 else RIDGE2)
        # core dot
        if t < 0.09:
            px((x, y), RIDGE)

# ---- the crown, resting on top of the print --------------------------------
# base bar
for yy in range(14, 18):
    for xx in range(19, 41):
        px((xx, yy), GOLD if yy < 17 else GOLDD)
# three points
for (tipx, basex0) in ((22, 19), (30, 27), (38, 35)):
    for yy in range(6, 14):
        half = max(0, (yy - 6) * 3 // 8 + 1)
        for xx in range(tipx - half, tipx + half + 1):
            if basex0 <= xx <= basex0 + 6 or abs(xx - tipx) <= half:
                if 19 <= xx <= 40:
                    px((xx, yy), GOLD)
# tip jewels + base highlight
for tipx, col in ((22, (235, 90, 90, 255)), (30, (110, 210, 130, 255)), (38, (120, 160, 245, 255))):
    px((tipx, 6), col)
    px((tipx, 7), col)
for xx in range(19, 41, 3):
    px((xx, 15), GOLDH)

out = os.path.join(GAME, "icon.png")
icon.convert("RGB").save(out)
print("wrote", out)
