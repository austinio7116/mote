#!/usr/bin/env python3
"""DraftMote asset extractor for the character/texture sheet
(art_sources/ref/sheet2.png — hero walks, texture swatches, items, doors).

Writes the editable game sheets under assets/:
  hero.png    16x20 x8 : front0 front1 back0 back1 left0 left1 (right = HFLIP)
  items.png   12x12 x13: coin key gem food star bigstar masterkey padlock boot
                         potion compass spyglass pouch  (boot/compass/spyglass authored)
  floors.png  32x32 macro-tiles (2x2 game tiles):
              wood wood_dark stone_tile red_carpet blue_carpet grass
              white_checker autumn grass_leafy
  props_sheet.png + prop boxes printed: bush chest campfire shelf_big shelf_small
                         book sack   (authored furniture comes from make_props.py)
"""
import os
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(os.path.dirname(HERE), "art_sources", "ref")
GAME = os.path.dirname(HERE)
SRC = os.path.join(REF, "sheet2.png")

im = Image.open(SRC).convert("RGB")
A = np.asarray(im).astype(np.int32)

def snap565(img):
    a = np.asarray(img).astype(np.uint16)
    a = np.stack(((a[..., 0] >> 3) << 3, (a[..., 1] >> 2) << 2, (a[..., 2] >> 3) << 3), axis=-1)
    return Image.fromarray(a.astype(np.uint8))

def keyed(box, pad=8, thresh=40, margin=12):
    """crop + alpha out the backdrop, keeping the sprite's MAIN ISLAND.
    Every sprite on the sheet is a single connected blob, so: colour-key,
    bridge small gaps, take the largest connected component, then fill its
    holes — grey clothing that matched the grey backdrop comes back."""
    from scipy import ndimage
    x0, y0, x1, y1 = box
    x0 = max(0, x0 - margin); y0 = max(0, y0 - margin)
    x1 = min(A.shape[1], x1 + margin); y1 = min(A.shape[0], y1 + margin)
    a = A[y0:y1, x0:x1]
    h, w = a.shape[:2]
    corners = np.concatenate([a[:pad, :pad].reshape(-1, 3), a[:pad, -pad:].reshape(-1, 3),
                              a[-pad:, :pad].reshape(-1, 3), a[-pad:, -pad:].reshape(-1, 3)])
    pal = np.unique(corners // 10, axis=0) * 10 + 5
    d = np.full((h, w), 1e9)
    for c in pal:
        d = np.minimum(d, np.abs(a - c).sum(axis=2))
    m = d > thresh
    m = ndimage.binary_opening(m, np.ones((2, 2)))
    m = ndimage.binary_closing(m, np.ones((5, 5)))     # bridge outline gaps
    lab, n = ndimage.label(m)
    if n == 0:
        raise SystemExit("empty key crop %s" % (box,))
    sizes = ndimage.sum(m, lab, range(1, n + 1))
    m = lab == (int(np.argmax(sizes)) + 1)
    m = ndimage.binary_fill_holes(m)
    ys, xs = np.where(m)
    bb = (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1)
    rgba = np.dstack([a.astype(np.uint8), (m * 255).astype(np.uint8)])
    return Image.fromarray(rgba, "RGBA").crop(bb)

def fit(img, w, h):
    """scale keeping aspect to fit w x h, centred on a transparent cell"""
    s = min(w / img.width, h / img.height)
    t = img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.LANCZOS)
    cell = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    cell.paste(t, ((w - t.width) // 2, h - t.height), t)   # feet on the baseline
    return cell

# ------------------------------------------------------------------- hero ----
# cells: 0-1 front walk · 2-3 back walk · 4-7 SIDE walk, right-facing
# (game flips for left). The sheet's side frames are a jumble — two of the
# "left" frames face right — so the 4-frame cycle is hand-picked:
#   stand(R3) -> stride A (L1 flipped) -> passing (L0 flipped) -> stride B (R1)
HW, HHH = 277, 326
FRONT = [(23, 57), (598, 57)]
BACK  = [(1270, 56), (1845, 57)]
SIDE  = [((4690, 56), 0),      # R3: standing (also the idle frame)
         ((2797, 57), 0),      # L1: stride A — ALREADY right-facing (flipping it
                               #     made frame 2 walk backwards vs the other 3)
         ((2505, 57), 1),      # L0: passing, faces left -> flip
         ((4102, 56), 0)]      # R1: stride B, already right-facing

hero = Image.new("RGBA", (10 * 16, 20), (0, 0, 0, 0))
def hero_cell(i, x, y, flip):
    c = keyed((x, y, x + HW, y + HHH), pad=24, thresh=40)   # dotted cell backdrop
    if flip:
        c = c.transpose(Image.FLIP_LEFT_RIGHT)
    hero.paste(fit(c, 16, 20), (i * 16, 0))

i = 0
for (x, y) in FRONT + BACK:
    hero_cell(i, x, y, 0); i += 1
for ((x, y), fl) in SIDE:
    hero_cell(i, x, y, fl); i += 1
hero.save(os.path.join(HERE, "hero.png"))
print("wrote hero.png (2 front, 2 back, 4 side)")

# ------------------------------------------------------------------ items ----
IT = Image.new("RGBA", (13 * 12, 12), (0, 0, 0, 0))
def item(cell, box, size=12, thresh=40):
    IT.paste(fit(keyed(box, thresh=thresh), size, size), (cell * 12 + (12 - size) // 2, 0))

item(0, (1712, 563, 1847, 727))            # gold nugget = coin
item(1, (1929, 540, 2025, 750))            # key
item(2, (2110, 569, 2285, 720))            # gem
item(3, (2333, 556, 2532, 730))            # sandwich
item(4, (2587, 547, 2760, 745), size=10)   # star (without the long trail)
item(5, (2587, 547, 2900, 745))            # big star, trail and all
item(7, (2961, 534, 3105, 746))            # padlock
item(9, (3176, 541, 3321, 750))            # potion
item(12, (1530, 720, 1610, 800))           # sack = gold pouch

# master key: the key, gilded
key = np.asarray(IT.crop((12, 0, 24, 12))).astype(np.int32)
mk = key.copy()
mk[..., 0] = np.clip(key[..., 0] * 1.5 + 50, 0, 255)
mk[..., 1] = np.clip(key[..., 1] * 1.2 + 25, 0, 255)
mk[..., 2] = (key[..., 2] * 0.35).astype(np.int32)
IT.paste(Image.fromarray(mk.astype(np.uint8)), (6 * 12, 0))

def px(cell, x, y, c):
    if 0 <= x < 12 and 0 <= y < 12:
        IT.putpixel((cell * 12 + x, y), c + (255,))

# boot (authored)
for (x0, y0, x1, y1, c) in ((4, 1, 7, 6, (150, 100, 55)), (4, 7, 9, 9, (150, 100, 55)),
                            (4, 10, 9, 10, (60, 50, 46))):
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            px(8, xx, yy, c)
for xx in range(4, 8): px(8, xx, 1, (180, 128, 74))
for xx in range(4, 10): px(8, xx, 9, (100, 64, 34))

# compass (authored): gold ring, red/white needle
for a in range(0, 360, 12):
    import math
    x = 5.5 + 4.6 * math.cos(math.radians(a)); y = 5.5 + 4.6 * math.sin(math.radians(a))
    px(10, int(round(x)), int(round(y)), (216, 180, 80))
for i2 in range(4):
    px(10, 5, 2 + i2, (220, 60, 50)); px(10, 6, 5 + i2, (235, 235, 240))
px(10, 5, 6, (235, 235, 240)); px(10, 6, 4, (220, 60, 50))

# spyglass (authored): brass telescope diagonal
for i2 in range(6):
    px(11, 2 + i2, 9 - i2, (190, 150, 70)); px(11, 3 + i2, 9 - i2, (150, 110, 50))
px(11, 8, 2, (110, 170, 210)); px(11, 9, 2, (110, 170, 210)); px(11, 8, 3, (80, 130, 170))
px(11, 1, 10, (90, 70, 40))

IT.save(os.path.join(HERE, "items.png"))
print("wrote items.png")

# ----------------------------------------------------------------- floors ----
# swatch grid: 6 cols x 2 rows at (22,462), cell ~176x178
FLOOR_ORDER = [("wood", 0, 0), ("stone_tile", 1, 0), ("red_carpet", 2, 0),
               ("blue_carpet", 3, 0), ("grass", 4, 0), ("white_checker", 5, 0),
               ("wood_dark", 0, 1), ("grass_leafy", 4, 1), ("autumn", 5, 1)]
def blend_seams(img):
    """cross-fade a macro-tile's borders so it tiles without hard seams"""
    a = np.asarray(img).astype(np.float32)
    n = a.shape[0]
    for i in range(2):
        w = (i + 1) / 3.0
        a[i, :] = a[i, :] * w + a[n - 1 - i, :] * (1 - w)
        a[:, i] = a[:, i] * w + a[:, n - 1 - i] * (1 - w)
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))

# ONE art scale everywhere: a full ~176px swatch is one 16px game tile (11:1),
# floors AND walls alike — planks ~3px, bricks ~4px, checker ~3px.
# Two variants per floor (rows), hash-picked per tile so knots don't grid up.
FS = Image.new("RGB", (len(FLOOR_ORDER) * 16, 32), (0, 0, 0))
for i, (name, cx, cy) in enumerate(FLOOR_ORDER):
    x = 22 + int(cx * 176.3)
    y = 462 + cy * 179
    for v, (ox, oy) in enumerate(((6, 6), (14, 10))):
        sw = im.crop((x + ox, y + oy, x + ox + 160, y + oy + 160)).resize((16, 16), Image.LANCZOS)
        FS.paste(snap565(blend_seams(sw)), (i * 16, v * 16))
FS.save(os.path.join(HERE, "floors.png"))
print("wrote floors.png (16px tiles, 2 variants):", " ".join(n for n, _, _ in FLOOR_ORDER))

# ------------------------------------------------------------- sheet props ---
# lifted straight off the sheet at half-art scale, packed left to right
PROPS = [
    ("bush",        (1080, 465, 1245, 635), 24, 24),
    ("chest",       (1075, 655, 1250, 815), 20, 16),
    ("campfire",    (1255, 650, 1440, 820), 20, 20),
    ("shelf_big",   (1255, 460, 1440, 640), 32, 26),
    ("shelf_small", (1440, 465, 1615, 635), 30, 26),
    ("book",        (1450, 645, 1535, 740), 10, 10),
    ("sack",        (1530, 720, 1610, 800), 12, 12),
]
total_w = sum(w for (_, _, w, _) in PROPS)
PS = Image.new("RGBA", (total_w, 32), (0, 0, 0, 0))
x = 0
meta = []
for (name, box, w, h) in PROPS:
    p = fit(keyed(box, thresh=30), w, h)
    PS.paste(p, (x, 32 - h))
    meta.append((name, x, 32 - h, w, h))
    x += w
PS.save(os.path.join(HERE, "props_sheet.png"))
print("props_sheet.png cells:")
for m in meta:
    print("   %-12s x=%3d y=%2d w=%2d h=%2d" % m)
