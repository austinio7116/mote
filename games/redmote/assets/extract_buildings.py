#!/usr/bin/env python3
"""Extract the AI-generated building sheet (gemini_buildings.png, 4096x1024,
left half = blue team, right half = red team) into the game's buildings.png.

Pipeline: background-key (border colour) -> connected components -> assign each
component to the nearest expected sprite ANCHOR (handles hovering parts like the
tesla ring and the yard crane) -> alpha-aware LANCZOS downscale to each
building's footprint width (sprites may stand taller than the footprint; the
game anchors them to the footprint bottom) -> pack a 2-team-row sheet and emit
src/buildings_meta.h with per-type slot x / w / h / y-offset.

Slot order matches game.c's building enum:
  YARD POW REF RAX FACT RDR PAD TECH PILL GUN COIL
"""
import numpy as np
from PIL import Image
from scipy import ndimage
from collections import Counter
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "gemini_buildings.png")
HALF = 2048

# expected sprite centres in the RED (right) half, relative to x=2048.
# NOTE: the factory (conveyor) and the chimney building touch — they land in one
# component assigned to "convey"; crop_sprite splits it at the mask valley.
ANCHORS = {
    "yard":   (275, 260),
    "silos":  (943, 217),    # -> REF
    "convey": (1714, 214),   # conveyor + chimneys merged -> FACT is the LEFT part
    "pow":    (180, 645),    # lightning-bolt towers
    "rax":    (768, 645),
    "radar":  (1254, 622),
    "tech":   (1836, 622),
    "pad":    (179, 900),
    "pill0":  (625, 900),    # spare (plain dome)
    "pill":   (1073, 900),   # dome with MG
    "gun":    (1518, 900),
    "coil":   (1931, 890),
}
# game slots: (anchor, footprint w tiles, footprint h tiles, max sprite h px)
SLOTS = [
    ("yard",   "yard", 3, 3, 26),
    ("pow",    "pow",  2, 2, 18),
    ("ref",    "silos", 3, 2, 22),
    ("rax",    "rax",  2, 2, 18),
    ("fact",   "convey", 3, 2, 22),
    ("rdr",    "radar", 2, 2, 18),
    ("pad",    "pad",  2, 2, 17),
    ("tech",   "tech", 2, 2, 20),
    ("pill",   "pill", 1, 1, 9),
    ("gun",    "gun",  1, 1, 10),
    ("coil",   "coil", 1, 1, 18),
]
ROW_H = 32

im = Image.open(SRC).convert("RGB")
a = np.asarray(im).astype(int)
edge = np.concatenate([a[0], a[-1], a[:, 0], a[:, -1]])
bg = np.array(Counter(map(tuple, edge)).most_common(1)[0][0])
mask = np.abs(a - bg).sum(axis=2) > 60
lab, n = ndimage.label(ndimage.binary_dilation(mask, iterations=2))

# gather per-half component boxes and assign to nearest anchor
half_boxes = [dict(), dict()]      # name -> [x0,y0,x1,y1]
for i, sl in enumerate(ndimage.find_objects(lab)):
    if sl is None:
        continue
    if (lab[sl] == i + 1).sum() < 500:
        continue
    y0, y1, x0, x1 = sl[0].start, sl[0].stop, sl[1].start, sl[1].stop
    if (x1 - x0) > 1900 or (y1 - y0) > 700:   # the divider lines
        continue
    team = 0 if (x0 + x1) / 2 < HALF else 1
    cx, cy = (x0 + x1) / 2 - team * HALF, (y0 + y1) / 2
    name = min(ANCHORS, key=lambda k: (ANCHORS[k][0] - cx) ** 2 + (ANCHORS[k][1] - cy) ** 2)
    b = half_boxes[team].get(name)
    half_boxes[team][name] = ([min(b[0], x0), min(b[1], y0), max(b[2], x1), max(b[3], y1)]
                              if b else [x0, y0, x1, y1])

rgba = np.dstack([a, np.where(mask, 255, 0)]).astype(np.uint8)


def recolor_blue(img):
    """team recolour: rotate the RED accent hues to blue, leave greys/golds."""
    import colorsys
    px = np.asarray(img).astype(float) / 255.0
    r, g, b = px[:, :, 0], px[:, :, 1], px[:, :, 2]
    mx, mn = px[:, :, :3].max(axis=2), px[:, :, :3].min(axis=2)
    v = mx
    s = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0)
    # hue in [0,1): red ~0 (wrap), gold ~0.12+, khaki ~0.1
    d = np.maximum(mx - mn, 1e-6)
    h = np.zeros_like(v)
    sel = mx == r
    h[sel] = ((g - b)[sel] / d[sel]) % 6
    sel = mx == g
    h[sel] = ((b - r)[sel] / d[sel]) + 2
    sel = mx == b
    h[sel] = ((r - g)[sel] / d[sel]) + 4
    h /= 6.0
    is_red = (s > 0.22) & ((h < 0.055) | (h > 0.93))
    h2 = np.where(is_red, 0.60, h)               # -> blue
    s2 = np.where(is_red, np.minimum(s * 1.0, 1), s)
    # hsv -> rgb (vectorised)
    i = np.floor(h2 * 6).astype(int) % 6
    f = h2 * 6 - np.floor(h2 * 6)
    p, q, t = v * (1 - s2), v * (1 - f * s2), v * (1 - (1 - f) * s2)
    r2 = np.choose(i, [v, q, p, p, t, v])
    g2 = np.choose(i, [t, v, v, q, p, p])
    b2 = np.choose(i, [p, p, t, v, v, q])
    out = np.dstack([r2, g2, b2, px[:, :, 3]])
    return Image.fromarray((out * 255).astype(np.uint8))


def crop_sprite(team, name):
    b = list(half_boxes[1][name])  # RED half only (blue half art is off)
    if name == "convey":           # split factory | chimneys at the mask valley
        sub = mask[b[1]:b[3], b[0]:b[2]]
        cols = sub.sum(axis=0)
        w2 = len(cols)
        lo, hi = int(w2 * 0.40), int(w2 * 0.80)
        b[2] = b[0] + lo + int(np.argmin(cols[lo:hi]))
    # alpha = the FILLED silhouette: dark interiors (yard floors, bays, shadows)
    # sit near the background grey — fill_holes keeps everything the sprite
    # encloses, only true outside-background stays transparent
    m = ndimage.binary_fill_holes(mask[b[1]:b[3], b[0]:b[2]])
    sub = a[b[1]:b[3], b[0]:b[2]]
    sp = Image.fromarray(np.dstack([sub, np.where(m, 255, 0)]).astype(np.uint8))
    return recolor_blue(sp) if team == 0 else sp


# pack the sheet
xs, meta = [], []
x = 0
for slot, anchor, fw, fh, maxh in SLOTS:
    xs.append(x)
    x += fw * 8
sheet = Image.new("RGBA", (x, ROW_H * 2), (0, 0, 0, 0))
for team in range(2):
    for (slot, anchor, fw, fh, maxh), sx in zip(SLOTS, xs):
        sp = crop_sprite(team, anchor)
        # bleed sprite colours into the transparent surround so the downscale
        # doesn't average the dark background into every edge (halo/mush)
        arr = np.asarray(sp).copy()
        alpha = arr[:, :, 3] > 0
        ind = ndimage.distance_transform_edt(~alpha, return_distances=False,
                                             return_indices=True)
        arr[:, :, :3] = arr[:, :, :3][tuple(ind)]
        sp = Image.fromarray(arr)
        tw = fw * 8
        th = max(1, round(sp.height * tw / sp.width))
        if th > maxh:
            th = maxh
            tw = max(1, round(sp.width * th / sp.height))
        # two-stage: smooth to 3x, then NEAREST to final — keeps pixel-art crispness
        mid = sp.resize((tw * 3, th * 3), Image.LANCZOS)
        small = mid.resize((tw, th), Image.NEAREST)
        px = np.asarray(small).astype(float)
        a2 = px[:, :, 3]
        rgb = px[:, :, :3]
        rgb = np.clip((rgb - 128) * 1.28 + 140, 0, 255)   # restore contrast + lift
        px[:, :, :3] = rgb
        px[:, :, 3] = np.where(a2 > 110, 255, 0)
        small = Image.fromarray(px.astype(np.uint8))
        ox = sx + (fw * 8 - tw) // 2
        oy = team * ROW_H + ROW_H - th          # anchor at slot bottom
        sheet.alpha_composite(small, (ox, oy))
        if team == 0:
            meta.append((slot, sx, tw, th, fh * 8 - th, ox - sx))
sheet.save(os.path.join(HERE, "buildings.png"))

with open(os.path.join(HERE, "..", "src", "buildings_meta.h"), "w") as f:
    f.write("/* GENERATED by assets/extract_buildings.py — per-building sprite slots\n"
            " * in buildings.png. Sprites anchor to the BOTTOM of the footprint;\n"
            " * BM_YOFF < 0 means the art stands taller than the footprint. */\n")
    f.write("#define BM_ROW_H %d\n" % ROW_H)
    for arr, idx, cast in (("BM_X", 1, "uint8_t"), ("BM_W", 2, "uint8_t"),
                           ("BM_H", 3, "uint8_t"), ("BM_YOFF", 4, "int8_t"),
                           ("BM_XOFF", 5, "int8_t")):
        f.write("static const %s %s[%d] = { %s };\n"
                % (cast, arr, len(meta), ", ".join(str(m[idx]) for m in meta)))
print("packed", sheet.size, "->", [m[0] for m in meta])
for m in meta:
    print("  %-5s x=%3d w=%2d h=%2d yoff=%3d" % (m[0], m[1], m[2], m[3], m[4]))
