#!/usr/bin/env python3
"""Extract the first-person gun art (user-supplied sheet) into weapons.png.
The source has the transparency CHECKERBOARD baked in as pixels — key out its
two flat greys, then fill holes per gun so grey steel inside survives.
Cells stay 72x56 (pistol / shotgun / chaingun), same layout the game uses."""
import numpy as np
from PIL import Image
from scipy import ndimage
from collections import Counter

SRC = "/tmp/ChatGPT Image Jul 3, 2026, 07_42_27 AM.png"
OUT = "/home/maustin/thumby-color/mote/games/wolfmote/assets/weapons.png"
CW, CH = 72, 56

im = np.asarray(Image.open(SRC).convert("RGB")).astype(int)
H, W, _ = im.shape

# the checker's two greys dominate the border region — take the 2 most common colours
border = np.concatenate([im[:24].reshape(-1,3), im[-24:].reshape(-1,3),
                         im[:, :24].reshape(-1,3), im[:, -24:].reshape(-1,3)])
counts = Counter(map(tuple, border))
(c1, _), (c2, _) = counts.most_common(2)
def near(px, c, tol=18):
    return (np.abs(px - np.array(c)).sum(axis=2)) < tol
bg = near(im, c1) | near(im, c2)
fg = ~bg
fg = ndimage.binary_opening(fg, structure=np.ones((3,3)))     # stray checker-matched speckle
lab, n = ndimage.label(fg)
cells = []
for sl in ndimage.find_objects(lab):
    h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
    if h < 200 or w < 120: continue
    m = ndimage.binary_fill_holes(lab[sl] > 0)                # recover grey steel inside
    rgba = np.dstack([im[sl], np.where(m, 255, 0)]).astype(np.uint8)
    cells.append((sl[1].start, Image.fromarray(rgba, "RGBA")))
cells.sort(key=lambda t: t[0])                                # left→right: pistol, shotgun, chaingun
assert len(cells) == 3, f"expected 3 guns, got {len(cells)}"

sheet = Image.new("RGBA", (CW*3, CH), (0,0,0,0))
for i, (_, g) in enumerate(cells):
    scale = (CH-1) / g.size[1]                                # fill the cell height
    w = min(CW, max(1, round(g.size[0]*scale)))
    r = g.resize((w, CH-1), Image.LANCZOS)
    a = np.asarray(r).copy()
    hard = a[:,:,3] > 110
    a[:,:,3] = np.where(hard, 255, 0); a[~hard] = 0           # crisp key
    r = Image.fromarray(a, "RGBA")
    sheet.paste(r, (i*CW + (CW-w)//2, CH-1-r.size[1]))        # bottom-anchored (gun sits at screen base)
    print(f"cell {i}: {w}x{CH-1}")
sheet.save(OUT)
print("wrote weapons.png from the supplied art")
