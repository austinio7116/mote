#!/usr/bin/env python3
"""DraftMote reference-room extractor (step 1 of the art pipeline).

Splits assets/ref/blueprince_sheet.png (AI-generated Blue Prince style
room sheet, top-down, ~32px tiles, 7x7-tile room cards) into individual
room crops under assets/ref/rooms/, trimming the protruding door tabs and
snapping each room to its wall frame. Also extracts the 4 door sprites.

Run:  python3 assets/extract_rooms.py   -> assets/ref/rooms/*.png + contact sheet
Then: python3 assets/build_tiles.py     (step 2: tile dedupe + library)
"""
import os
import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(os.path.dirname(HERE), "art_sources", "ref")
OUT = os.path.join(REF, "rooms")
os.makedirs(OUT, exist_ok=True)

im = Image.open(os.path.join(REF, "blueprince_sheet.png")).convert("RGB")
a = np.asarray(im).astype(int)
bg = a[5, 5]
mask = (np.abs(a - bg).sum(axis=2) > 60)
mask = ndimage.binary_closing(mask, structure=np.ones((5, 5)))
lab, n = ndimage.label(mask)
objs = ndimage.find_objects(lab)

blobs = []
for sl in objs:
    h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
    if w > 80 and h > 80:
        blobs.append((sl[1].start, sl[0].start, w, h))
blobs.sort(key=lambda r: (round(r[1] / 140), r[0]))

def trim_to_frame(x, y, w, h):
    """Trim a blob to its room frame: keep rows/cols that are mostly non-bg
    (door tabs and floating letters are sparse)."""
    sub = mask[y:y + h, x:x + w]
    colocc = sub.sum(axis=0) / float(h)
    rowocc = sub.sum(axis=1) / float(w)
    cols = np.where(colocc > 0.55)[0]
    rows = np.where(rowocc > 0.55)[0]
    if len(cols) < 10 or len(rows) < 10:
        return x, y, w, h
    return x + cols[0], y + rows[0], cols[-1] - cols[0] + 1, rows[-1] - rows[0] + 1

rooms, doors = [], []
for (x, y, w, h) in blobs:
    if y < 200:                       # the 4 door sprites in the header
        doors.append((x, y, w, h))
    else:
        rooms.append(trim_to_frame(x, y, w, h))

print("door sprites:", len(doors), " rooms:", len(rooms))
for i, (x, y, w, h) in enumerate(doors):
    im.crop((x, y, x + w, y + h)).save(os.path.join(REF, "door%d.png" % i))

meta = []
for i, (x, y, w, h) in enumerate(rooms):
    crop = im.crop((x, y, x + w, y + h))
    name = "room%02d" % i
    crop.save(os.path.join(OUT, name + ".png"))
    meta.append((name, x, y, w, h))
    print(name, x, y, w, h)

# contact sheet for review (quarter scale)
cols = 10
cell = 64
sheet = Image.new("RGB", (cols * cell, ((len(rooms) + cols - 1) // cols) * cell), (30, 30, 40))
from PIL import ImageDraw
d = ImageDraw.Draw(sheet)
for i, (name, x, y, w, h) in enumerate(meta):
    t = Image.open(os.path.join(OUT, name + ".png")).resize((cell - 4, cell - 4), Image.LANCZOS)
    sheet.paste(t, ((i % cols) * cell + 2, (i // cols) * cell + 2))
    d.text(((i % cols) * cell + 4, (i // cols) * cell + 2), str(i), fill=(255, 255, 0))
sheet.save(os.path.join(REF, "rooms_contact.png"))
print("wrote", os.path.join(REF, "rooms_contact.png"))
