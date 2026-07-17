#!/usr/bin/env python3
"""DraftMote prop harvester (step 2b of the art pipeline).

For every extracted reference room (assets/ref/rooms/roomNN.png):
  - model the FLOOR as the room-interior's dominant colour bins,
  - mask everything that isn't floor (furniture, rugs, lamps...),
  - split the mask into connected components,
  - save each component as an RGBA sprite under assets/ref/props/
    (floor made transparent, interior holes kept opaque),
  - build a numbered contact sheet (assets/ref/props_contact.png) for curation.

The curated pick list lives in gen_assets.py, which composes the game's
props.png + tiles.png from these harvested pieces.
"""
import os
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(os.path.dirname(HERE), "art_sources", "ref")
ROOMS = os.path.join(REF, "rooms")
OUT = os.path.join(REF, "props")
os.makedirs(OUT, exist_ok=True)

INSET = 16          # wall frame thickness to skip when modelling the floor
MIN_AREA = 120      # ignore specks
DIST = 42           # colour distance to nearest floor colour that still counts as floor

# rooms whose centre is a rug: sample the rug too, so furniture ON the rug
# separates cleanly and the rug itself stays "floor" (harvested as a texture)
RUG_ROOMS = {0, 1, 2, 4, 20, 21, 22}

def sample_colors(a, rects):
    """unique quantised colours from sample rects = the floor palette"""
    px = np.concatenate([a[y0:y1, x0:x1].reshape(-1, 3) for (x0, y0, x1, y1) in rects])
    q = np.unique(px // 20, axis=0)
    return q * 20 + 10

def floor_palette(a, idx):
    h, w = a.shape[:2]
    # floor sample: bottom-centre entry area + two low corners (usually clear)
    rects = [(w // 2 - 22, h - 48, w // 2 + 22, h - 26),
             (INSET + 6, h - 44, INSET + 30, h - 26),
             (w - INSET - 30, h - 44, w - INSET - 6, h - 26)]
    if idx in RUG_ROOMS:
        rects.append((w // 2 - 16, h // 2 - 12, w // 2 + 16, h // 2 + 12))
    return sample_colors(a, rects)

entries = []
for fn in sorted(f for f in os.listdir(ROOMS) if f.endswith(".png")):
    im = Image.open(os.path.join(ROOMS, fn)).convert("RGB")
    a = np.asarray(im).astype(np.int32)
    h, w = a.shape[:2]
    if h < 150 or w < 150:
        continue
    idx = int(fn[4:6])
    inter = a[INSET:h - INSET, INSET:w - INSET]
    cols = floor_palette(a, idx)
    # distance of every interior pixel to its nearest floor colour
    d = np.full(inter.shape[:2], 1e9)
    for c in cols:
        d = np.minimum(d, np.abs(inter - c).sum(axis=2))
    m = d > DIST
    m[-32:, -78:] = False                       # room label text
    m = ndimage.binary_opening(m, np.ones((3, 3)))
    m = ndimage.binary_closing(m, np.ones((5, 5)))
    m = ndimage.binary_fill_holes(m)
    lab, n = ndimage.label(m)
    for sl in ndimage.find_objects(lab):
        bh, bw = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
        blob = (lab[sl] > 0)
        if blob.sum() < MIN_AREA:
            continue
        px = inter[sl]
        alpha = (blob * 255).astype(np.uint8)
        rgba = np.dstack([px.astype(np.uint8), alpha])
        name = "%s_p%02d" % (fn[:-4], len([e for e in entries if e[0].startswith(fn[:-4])]))
        Image.fromarray(rgba, "RGBA").save(os.path.join(OUT, name + ".png"))
        entries.append((name, bw, bh))

print("props:", len(entries))

# contact sheet (half source scale = game scale), numbered
CS = 56
cols_n = 14
rows_n = (len(entries) + cols_n - 1) // cols_n
sheet = Image.new("RGB", (cols_n * CS, rows_n * CS), (24, 28, 44))
d = ImageDraw.Draw(sheet)
for i, (name, bw, bh) in enumerate(entries):
    p = Image.open(os.path.join(OUT, name + ".png"))
    p = p.resize((max(1, bw // 2), max(1, bh // 2)), Image.LANCZOS)
    if p.width > CS - 2 or p.height > CS - 10:
        p.thumbnail((CS - 2, CS - 10))
    sheet.paste(p, ((i % cols_n) * CS + 1, (i // cols_n) * CS + 9), p)
    d.text(((i % cols_n) * CS + 1, (i // cols_n) * CS), str(i) + ":" + name[4:6] + name[-2:], fill=(255, 255, 0))
sheet.save(os.path.join(REF, "props_contact.png"))
print("wrote", os.path.join(REF, "props_contact.png"))
