#!/usr/bin/env python3
"""Append the ARMOUR cell to pickups.png.

The pickup billboard indexes the atlas as kind*16, so a new kind needs a cell
at its own index. PK_ARMOUR takes 7 and PK_PACKAGE moves to 8 -- PACKAGE is
skipped by the draw and has never had art, so the free index belongs to the
thing that needs one.

A blue vest rather than a shield: at 16 px a shield reads as a generic badge,
while a vest with shoulder straps is unmistakably body armour, and blue keeps
it clear of the red medkit two cells along.

ONE-SHOT: re-running widens the sheet again. Check the width first (112 = not
yet applied, 128 = done).
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.abspath(__file__))
p = os.path.join(ROOT, "pickups.png")
im = Image.open(p).convert("RGBA")
if im.size[0] >= 128:
    raise SystemExit("pickups.png is already %d px wide - already applied" % im.size[0])

out = Image.new("RGBA", (im.size[0] + 16, 16), (0, 0, 0, 0))
out.paste(im, (0, 0))
px = out.load()
x0 = im.size[0]
DK, MD, LT = (28, 58, 110, 255), (46, 92, 164, 255), (96, 150, 220, 255)
# torso
for y in range(3, 14):
    for x in range(4, 12):
        px[x0+x, y] = MD
# shoulder straps
for x in range(3, 13):
    px[x0+x, 3] = DK; px[x0+x, 4] = DK
# neck notch
px[x0+7, 3] = (0,0,0,0); px[x0+8, 3] = (0,0,0,0)
px[x0+7, 4] = (0,0,0,0); px[x0+8, 4] = (0,0,0,0)
# highlight down the left panel, shadow down the right
for y in range(5, 13):
    px[x0+5, y] = LT
    px[x0+10, y] = DK
# hem
for x in range(4, 12):
    px[x0+x, 13] = DK
out.save(p)
print("pickups.png -> %d px (%d cells); armour is cell %d"
      % (out.size[0], out.size[0]//16, x0//16))
