#!/usr/bin/env python3
"""Extract the tank art: separate HULL and TURRET (so the turret can rotate/animate).

The turret is padded so its PIVOT (turret-ring centre = widest row of the body) is the
image centre — the game rotates quads about their centre, so it turns in place on the
hull. Prints the metre sizes to wire into game.c (hull is 6.2 m long, matching physics).
"""
import numpy as np
from PIL import Image
from scipy import ndimage

SRC = "/tmp/4ccecfd2-d92d-43a6-9c45-69f52a468dd1.png"
HULL_LEN_M = 6.2            # matches VSTAT[VEH_TANK].len
OUT = "/home/maustin/thumby-color/mote/games/grandthumbauto/assets"

im = np.asarray(Image.open(SRC).convert("RGB")).astype(int)
bg = np.median(im[2:20, 2:20].reshape(-1, 3), axis=0)
mask = np.abs(im - bg).sum(axis=2) > 60
mask = ndimage.binary_fill_holes(ndimage.binary_closing(mask, structure=np.ones((5, 5))))
lab, n = ndimage.label(mask)
parts = []
for sl in ndimage.find_objects(lab):
    h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
    if h < 100 or w < 60: continue
    parts.append(sl)
parts.sort(key=lambda sl: sl[1].start)          # left = hull, right = turret
assert len(parts) == 2, f"expected hull+turret, got {len(parts)}"

def crop_rgba(sl):
    c = im[sl]; m = mask[sl]
    return Image.fromarray(np.dstack([c, np.where(m, 255, 0)]).astype(np.uint8), "RGBA")

hull, tur = crop_rgba(parts[0]), crop_rgba(parts[1])
scale = 62.0 / hull.size[1]                     # hull -> 62 px tall (fits the 40x64 cell)
def rescale(img):
    w, h = max(1, round(img.size[0]*scale)), max(1, round(img.size[1]*scale))
    r = img.resize((w, h), Image.LANCZOS)
    a = np.asarray(r); rr = a.copy(); rr[:, :, 3] = np.where(a[:, :, 3] > 110, 255, 0)
    return Image.fromarray(rr, "RGBA")
hull, tur = rescale(hull), rescale(tur)

# hull -> 40x64 cell, centred
cell = Image.new("RGBA", (40, 64), (0, 0, 0, 0))
cell.paste(hull, ((40 - hull.size[0])//2, (64 - hull.size[1])//2))
cell.save(f"{OUT}/tankhull.png")

# turret pivot = widest row of its silhouette (the ring), pad so pivot == centre
ta = np.asarray(tur)[:, :, 3] > 0
widths = ta.sum(axis=1)
pivot_y = int(np.argmax(widths))
H = 2*max(pivot_y, tur.size[1]-pivot_y) + 2
W = max(tur.size[0] + 2, 24); W += W & 1
tcell = Image.new("RGBA", (W, H), (0, 0, 0, 0))
tcell.paste(tur, ((W - tur.size[0])//2, H//2 - pivot_y))
tcell.save(f"{OUT}/tankturret.png")

mpp = HULL_LEN_M / hull.size[1]
print(f"hull  {hull.size[0]}x{hull.size[1]}px in 40x64 cell -> {hull.size[0]*mpp:.2f} x {HULL_LEN_M} m")
print(f"turret cell {W}x{H}, pivot centred; art {tur.size[0]}x{tur.size[1]}px")
print(f"game constants: TURRET_CW {W}  TURRET_CH {H}  TURRET_LEN_M {H*mpp:.2f}f  TURRET_WID_M {W*mpp:.2f}f")
