#!/usr/bin/env python3
"""GrandThumbAuto launcher icon (60x60) — police chase built from the game's REAL
assets: the red racer (cars2 cell 34) fleeing a squad car (cell 26, lightbar lit)
on the current road art (asphalt, kerb, white double median). Lives in the game
ROOT; `mote bake` -> src/icon.h."""
import random
from PIL import Image, ImageDraw

random.seed(7)
ROOT = "/home/maustin/thumby-color/mote/games/grandthumbauto"
S = 60
im = Image.new("RGBA", (S, S), (88, 88, 98, 255))
d = ImageDraw.Draw(im)
px = im.load()

# asphalt noise
for y in range(S):
    for x in range(S):
        n = random.randint(-4, 4)
        r, g, b, a = px[x, y]
        px[x, y] = (r+n, g+n, b+n, 255)

# pavement strip down the left + kerb (matches the tile art)
for y in range(S):
    for x in range(0, 11):
        n = random.randint(-3, 3)
        px[x, y] = (150+n, 148+n, 138+n, 255)
for yy in range(0, S, 8):                       # slab seams
    d.line([0, yy, 10, yy], fill=(122, 120, 112, 255))
d.rectangle([11, 0, 12, S-1], fill=(168, 166, 152, 255))     # kerb
d.line([13, 0, 13, S-1], fill=(52, 52, 60, 255))             # gutter

# white double median, slightly right of centre
d.rectangle([40, 0, 40, S-1], fill=(210, 210, 218, 255))
d.rectangle([43, 0, 43, S-1], fill=(210, 210, 218, 255))

def car(cell, h_px, rot):
    sheet = Image.open(f"{ROOT}/assets/cars2.png")
    CW, CH, COLS = 28, 60, 8
    c = sheet.crop(((cell % COLS)*CW, (cell//COLS)*CH, (cell % COLS)*CW+CW, (cell//COLS)*CH+CH))
    # crop to opaque art
    bbox = c.getbbox()
    c = c.crop(bbox)
    sc = h_px / c.size[1]
    c = c.resize((max(1, round(c.size[0]*sc)), h_px), Image.LANCZOS)
    return c.rotate(rot, expand=True, resample=Image.BICUBIC)

def shadow(cx, cy, w, h, ang):
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([cx-w//2, cy-h//2, cx+w//2, cy+h//2],
                                         radius=3, fill=(30, 28, 40, 130))
    im.alpha_composite(sh.rotate(ang, center=(cx, cy), resample=Image.BICUBIC))

# the racer tearing away up-right; the law close behind, offset to its lane
racer  = car(34, 36, -16)
police = car(26, 32, -10)
shadow(41, 17, 20, 36, -16)
shadow(22, 44, 18, 32, -10)
im.alpha_composite(police, (22 - police.size[0]//2, 44 - police.size[1]//2 + 2))
im.alpha_composite(racer,  (41 - racer.size[0]//2,  17 - racer.size[1]//2 + 2))

# subtle red/blue strobe glow beside the squad car's own lightbar
d.point([(17, 37), (18, 37)], fill=(255, 80, 70, 255))
d.point([(26, 35), (27, 35)], fill=(90, 130, 255, 255))

# punchy border
d.rectangle([0, 0, S-1, S-1], outline=(10, 10, 16, 255), width=2)
im.convert("RGB").save(f"{ROOT}/icon.png")
print("wrote icon.png 60x60")
