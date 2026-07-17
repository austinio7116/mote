#!/usr/bin/env python3
"""ThumbPrince title backdrop — the crowned-thumbprint mark, full screen:
a white SPIRAL-whorl print on blueprint paper with a dotted drafting grid,
two orbit rings, and a hand-sketched white chalk crown on top (user's
reference concept). Text (title/tagline/stats/prompt) is drawn by the game
over the calm bands this leaves at the top and bottom.

Run:
    python3 assets/make_title.py && mote bake games/thumbprince
Writes assets/title_bg.png -> baked to src/title_bg.h (title_bg_img).
"""
import os
import math
import random
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

W = H = 128
BLUE  = (16, 26, 56)          # blueprint paper
BLUE2 = (13, 21, 46)          # paper shade (vignette)
GRID  = (38, 58, 104)         # dotted drafting grid
RING  = (96, 130, 186)        # orbit rings
RINGH = (150, 185, 235)
CHALK = (238, 244, 252)       # print ridges + crown chalk
CHALK2= (196, 214, 240)

img = Image.new("RGB", (W, H), BLUE)
px = img.putpixel

def hash2(x, y):
    h = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return h ^ (h >> 16)

# paper vignette: corners a shade deeper
for y in range(H):
    for x in range(W):
        d = math.hypot(x - 64, y - 64)
        if d > 78:
            px((x, y), BLUE2)

# dotted grid, 16px pitch (dots, like the reference, not solid rules)
for v in range(0, W, 16):
    for k in range(0, H, 3):
        px((v, k), GRID)
        px((k, v), GRID)

# ---- orbit rings around the mark -------------------------------------------
CXR, CYR = 64, 74
for (rad, col, dotted) in ((40, RING, 0), (50, RINGH, 1)):
    steps = int(rad * 7)
    for i in range(steps):
        a = i * 2 * math.pi / steps
        if dotted and (i // 3) % 2:
            continue
        x = int(round(CXR + rad * math.cos(a)))
        y = int(round(CYR + rad * 0.92 * math.sin(a)))
        if 0 <= x < W and 0 <= y < H:
            px((x, y), col)

# ---- the thumbprint: an Archimedean spiral whorl ----------------------------
CX, CY = 64.0, 76.0
RX, RY = 17.0, 22.0
TURNS = 7.5                    # ridge loops from core to edge

for y in range(H):
    for x in range(W):
        nx = (x - CX) / RX
        ny = (y - CY) / RY
        taper = 1.0 - 0.16 * max(0.0, ny)          # thumb pad: narrower base
        r = math.hypot(nx / taper, ny)
        if r > 1.0:
            continue
        th = math.atan2(ny, nx / taper)
        s = (r * TURNS - th / (2 * math.pi)) % 1.0
        broken = hash2(x * 3 + y, y * 7 - x) % 13 == 0
        if s < 0.46 and not broken:
            px((x, y), CHALK if hash2(x, y) % 5 else CHALK2)

# ---- hand-sketched chalk crown ----------------------------------------------
rng = random.Random(46)        # Room 46, of course

def stroke(x0, y0, x1, y1, passes=2):
    """chalky line: drawn a couple of times with jitter and small gaps"""
    for p in range(passes):
        ox = rng.uniform(-0.8, 0.8)
        oy = rng.uniform(-0.8, 0.8)
        steps = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
        for i in range(steps):
            if rng.random() < 0.18:
                continue                            # chalk skips
            t = i / steps
            x = int(round(x0 + (x1 - x0) * t + ox + rng.uniform(-0.5, 0.5)))
            y = int(round(y0 + (y1 - y0) * t + oy + rng.uniform(-0.5, 0.5)))
            if 0 <= x < W and 0 <= y < H:
                px((x, y), CHALK if rng.random() < 0.75 else CHALK2)

# crown outline (like the reference: three unequal peaks, slightly askew)
BL, BR = 48, 82                # base corners
BY = 53                        # base line sits on the print's crown
pts = [(BL, BY), (46, 40), (56, 47), (64, 34), (72, 47), (83, 38), (BR, BY)]
for i in range(len(pts) - 1):
    stroke(*pts[i], *pts[i + 1])
stroke(BL, BY, BR, BY)                       # base bar
stroke(BL + 1, BY + 3, BR - 1, BY + 3)       # double base line
# scribble shading inside the band + a few in the peaks
for k in range(8):
    x0 = rng.uniform(BL + 2, BR - 8)
    stroke(x0, BY + 2, x0 + 6, BY - 4, passes=1)
stroke(60, 40, 66, 44, passes=1)
stroke(49, 44, 53, 47, passes=1)
stroke(78, 42, 74, 46, passes=1)

out = os.path.join(HERE, "title_bg.png")
img.save(out)
print("wrote", out)
