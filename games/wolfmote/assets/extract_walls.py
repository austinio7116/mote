#!/usr/bin/env python3
"""Extract the 8-tile wall texture set (user-supplied, 4x2 grid on dark gutters)
into individual 64x64 wall textures:
  wallexit (green arrow) / wallstone / wallbrick / walldoor (steel)
  wallcrack / wallmoss / wallmetal / walldoorw (wood)"""
import numpy as np
from PIL import Image
from scipy import ndimage
from collections import Counter

SRC = "/home/maustin/.claude/uploads/28ee9ae3-8030-4cdc-af99-20078e7d27c0/9333b007-file_00000000c86871f4b6b5d0580ebefb1f.png"
AD  = "/home/maustin/thumby-color/mote/games/wolfmote/assets"
NAMES = ["wallexit","wallstone","wallbrick","walldoor",
         "wallcrack","wallmoss","wallmetal","walldoorw"]

im = np.asarray(Image.open(SRC).convert("RGB")).astype(int)
border = np.concatenate([im[:12].reshape(-1,3), im[-12:].reshape(-1,3),
                         im[:, :12].reshape(-1,3), im[:, -12:].reshape(-1,3)])
(bg,_), = Counter(map(tuple,border)).most_common(1)
mask = (np.abs(im - np.array(bg)).sum(axis=2)) > 24
mask = ndimage.binary_opening(mask, structure=np.ones((5,5)))
lab,_ = ndimage.label(mask)
tiles=[]
for sl in ndimage.find_objects(lab):
    h,w = sl[0].stop-sl[0].start, sl[1].stop-sl[1].start
    if h<200 or w<200: continue
    tiles.append((sl[0].start//200, sl[1].start, im[sl]))
tiles.sort(key=lambda t:(t[0], t[1]))
assert len(tiles)==8, f"expected 8 wall tiles, got {len(tiles)}"
for (rb, xs, px), name in zip(tiles, NAMES):
    t = Image.fromarray(px.astype(np.uint8), "RGB").resize((64,64), Image.LANCZOS)
    t.convert("RGBA").save(f"{AD}/{name}.png")
    print(name, "64x64")
