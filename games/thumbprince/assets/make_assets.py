#!/usr/bin/env python3
"""ThumbPrince authored structure art — walls and doors, drawn at pixel level
(downscaled swatches turned to mush at band scale, so these are hand-made).

  walls_stone/red/dark.png  8x8-TILE EDGE16 rule tilesheets (4x4 cells,
      32x32): one brick tile shaded per neighbour mask; the wall ring is one
      tile thick. tilesets/walls_*.tileset (tile 8) for the Studio Tiles tab.

  doors.png  16x16 x6: h_closed h_open v_closed v_open gold_closed gold_open
      Content is 16w x 8h anchored to the top of the cell: the door sits FLUSH
      in the 8px wall band (S doors VFLIP, v cells are the CCW rotation = a
      W door; E mirrors).

Hero/items/floors from extract_sheet2.py, furniture from make_props.py.
Bake: `mote bake games/thumbprince`.
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

def snap(c):
    return ((c[0] >> 3) << 3, (c[1] >> 2) << 2, (c[2] >> 3) << 3, 255)

# ------------------------------------------------------------------ floors ---
# Authored 8x8 floor rule tilesets — SAME tile size as the walls. Patterns are
# designed to tessellate at an 8px period (planks 4px courses with staggered
# seams per variant row, 4px checks, speckled grass); the engine's nvar hash
# mixes the variant rows so organic floors don't grid up.
import random
FLOORS = {
    #        base          light          dark          seam         variants
    "wood":          ((150,106,62), (178,132,82), (118,80,46), (92,60,34), 3),
    "wood_dark":     ((104,74,44),  (128,94,58),  (82,56,34),  (60,40,26), 3),
    "stone_tile":    ((108,108,118),(128,128,138),(88,88,98),  (70,70,80), 2),
    "red_carpet":    ((142,48,44),  (162,62,54),  (116,36,36), (104,32,32),2),
    "blue_carpet":   ((58,68,130),  (72,84,150),  (44,52,106), (40,46,96), 2),
    "grass":         ((84,140,62),  (102,160,74), (64,112,50), (56,96,44), 3),
    "white_checker": ((208,204,190),(224,220,208),(160,156,146),(140,136,128),1),
    "grass_leafy":   ((74,128,56),  (96,150,66),  (120,150,60),(52,92,42), 3),
    "autumn":        ((150,132,62), (180,158,80), (120,100,50),(100,84,42),3),
}

def floor_tile(name, pal, v):
    base, li, dk, seam = pal
    rng = random.Random(hash(name) & 0xffff | (v << 16))
    t = Image.new("RGB", (8, 8))
    for y in range(8):
        for x in range(8):
            t.putpixel((x, y), snap(base)[:3])
    if name.startswith("wood"):
        # long planks: thin seam lines between 4px boards, joints only on
        # one variant row so boards run several tiles before breaking
        for x in range(8):
            t.putpixel((x, 3), snap(seam)[:3])
            t.putpixel((x, 7), snap(seam)[:3])
        gx = (v * 5 + 2) % 8
        t.putpixel((gx, 1), snap(dk)[:3])            # grain fleck
        t.putpixel(((gx + 4) % 8, 5), snap(li)[:3])
        if v == 2:                                   # occasional plank end
            end = 5
            for yy in range(0, 3):
                t.putpixel((end, yy), snap(seam)[:3])
    elif name == "stone_tile":
        for x in range(8):
            t.putpixel((x, 7), snap(seam)[:3])
            t.putpixel((x, 0), snap(li)[:3])
        for y in range(8):
            t.putpixel((7, y), snap(seam)[:3])
        if v:
            t.putpixel((3, 4), snap(dk)[:3]); t.putpixel((4, 3), snap(dk)[:3])
    elif name.endswith("carpet"):
        for y in range(8):
            for x in range(8):
                if (x + y) % 2 == 0:
                    t.putpixel((x, y), snap(li if (x ^ y) & 2 else base)[:3])
        for i in range(2 + v):
            t.putpixel((rng.randrange(8), rng.randrange(8)), snap(dk)[:3])
    elif name == "white_checker":
        for y in range(8):
            for x in range(8):
                c = li if ((x // 4) + (y // 4)) % 2 == 0 else dk
                t.putpixel((x, y), snap(c)[:3])
    else:                                            # grasses + autumn
        for i in range(7):
            x, y = rng.randrange(8), rng.randrange(8)
            t.putpixel((x, y), snap(li)[:3])
        for i in range(4):
            x, y = rng.randrange(8), rng.randrange(8)
            t.putpixel((x, y), snap(dk)[:3])
        if name in ("grass_leafy", "autumn"):
            x, y = rng.randrange(8), rng.randrange(8)
            t.putpixel((x, y), snap(seam)[:3])
    return t

for name, spec in FLOORS.items():
    pal, nvar = spec[:4], spec[4]
    sheet = Image.new("RGB", (8, 8 * nvar))
    for v in range(nvar):
        sheet.paste(floor_tile(name, pal, v), (0, v * 8))
    sheet.save(os.path.join(HERE, "floor_%s.png" % name))
    with open(os.path.join(GAME, "tilesets", "floor_%s.tileset" % name), "w") as f:
        f.write("sheet assets/floor_%s.png\n" % name)
        f.write("tile 8\ntype 1\nedge 1\nnvar %d\ncols 1\nrows %d\n" % (nvar, nvar))
        f.write("lut " + " ".join("0" for _ in range(256)) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        f.write("vweight 1 1 1 1 1 1 1 1\n")
print("wrote 8px floor tilesets x", len(FLOORS))

# ------------------------------------------------------------------- walls ---
# A real 8x8 tilesheet: the wall band is 8px thick = ONE tile. EDGE16 rule
# sheet (4x4 grid of 8x8 cells, cell index = N|E<<1|S<<2|W<<3 same-neighbour
# mask): every cell is the same running-bond brick tile, with the exposed
# sides (no neighbour) picking up an outline shade. tile 8 in the .tileset.
WALLS = {
    "stone": ((126, 128, 140), (104, 106, 118), (84, 86, 98),  (56, 58, 68)),
    "red":   ((178, 92, 60),   (150, 72, 46),   (122, 56, 38), (78, 40, 30)),
    "dark":  ((96, 88, 100),   (78, 72, 84),    (62, 56, 68),  (40, 36, 46)),
}

def brick8(pal):
    """8x8 running bond: two 4px courses, joints staggered 0/4"""
    li, mid, dk, mortar = pal
    t = Image.new("RGB", (8, 8))
    for y in range(8):
        for x in range(8):
            row = y % 4
            joint = (x + (4 if (y // 4) else 0)) % 8 == 0
            if row == 3 or joint:
                c = mortar
            elif row == 0:
                c = li
            else:
                c = mid if (x * 3 + y * 7) % 11 else dk
            t.putpixel((x, y), snap(c)[:3])
    return t

def wall_sheet8(pal):
    li, mid, dk, mortar = pal
    base = brick8(pal)
    sheet = Image.new("RGB", (32, 32))
    for m in range(16):                    # cell = neighbour mask N|E<<1|S<<2|W<<3
        cell = base.copy()
        if not (m & 1):                    # N exposed: top catch
            for x in range(8): cell.putpixel((x, 0), snap(li)[:3])
        if not (m & 4):                    # S exposed: shadow
            for x in range(8): cell.putpixel((x, 7), snap(mortar)[:3])
        if not (m & 8):                    # W exposed
            for y in range(8): cell.putpixel((0, y), snap(li)[:3])
        if not (m & 2):                    # E exposed
            for y in range(8): cell.putpixel((7, y), snap(mortar)[:3])
        sheet.paste(cell, ((m % 4) * 8, (m // 4) * 8))
    return sheet

TILESETS = os.path.join(GAME, "tilesets")
os.makedirs(TILESETS, exist_ok=True)
N, E, S, W = 1, 4, 16, 64
lut = []
for m in range(256):
    c = 0
    if m & N: c |= 1
    if m & E: c |= 2
    if m & S: c |= 4
    if m & W: c |= 8
    lut.append(c)
for name, pal in WALLS.items():
    wall_sheet8(pal).save(os.path.join(HERE, "walls_%s.png" % name))
    with open(os.path.join(TILESETS, "walls_%s.tileset" % name), "w") as f:
        f.write("sheet assets/walls_%s.png\n" % name)
        f.write("tile 8\ntype 1\nedge 0\nnvar 1\ncols 4\nrows 4\n")
        f.write("lut " + " ".join(str(v) for v in lut) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        f.write("vweight 1 1 1 1 1 1 1 1\n")
print("wrote walls_*.png as 8x8 EDGE16 tilesheets")

# ------------------------------------------------------------------- doors ---
FRAME  = (52, 40, 34)
FRAME_L = (78, 62, 50)
PLANK  = (146, 96, 50)
PLANK_L = (172, 120, 66)
PLANK_D = (108, 68, 36)
SEAM   = (86, 54, 30)
HANDLE = (222, 186, 92)
DARK   = (16, 13, 12)
GLOW   = (60, 47, 32)

D = Image.new("RGBA", (6 * 16, 16), (0, 0, 0, 0))

def px(cell, x, y, c):
    if 0 <= x < 16 and 0 <= y < 16 and c is not None:
        D.putpixel((cell * 16 + x, y), snap(c))

def rect(cell, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px(cell, x, y, c)

def h_door(cell, opened, gold=False):
    """16w x 8h, top-anchored: FLUSH with the 8px wall band, no sill"""
    pl, pll, pld, sm, hd = PLANK, PLANK_L, PLANK_D, SEAM, HANDLE
    if gold:
        pl, pll, pld, sm = (196, 158, 62), (232, 198, 96), (150, 116, 44), (128, 96, 36)
        hd = (255, 244, 190)
    rect(cell, 0, 0, 1, 7, FRAME); px(cell, 0, 0, FRAME_L); px(cell, 1, 0, FRAME_L)
    rect(cell, 14, 0, 15, 7, FRAME); px(cell, 14, 0, FRAME_L); px(cell, 15, 0, FRAME_L)
    rect(cell, 0, 0, 15, 0, FRAME_L)                       # lintel
    if opened:
        rect(cell, 2, 1, 13, 7, DARK)
        rect(cell, 2, 6, 13, 7, GLOW)                      # lit threshold
    else:
        rect(cell, 2, 1, 13, 7, pl)
        for x in (5, 10):
            for y in range(1, 8):
                px(cell, x, y, sm)                         # plank seams
        rect(cell, 2, 1, 13, 1, pll)
        rect(cell, 2, 7, 13, 7, pld)
        px(cell, 12, 4, hd)

h_door(0, 0)
h_door(1, 1)
h_door(4, 0, gold=True)
h_door(5, 1, gold=True)
for (src, dst) in ((0, 2), (1, 3)):                        # v = CCW rotation (W door)
    cellim = D.crop((src * 16, 0, src * 16 + 16, 16)).transpose(Image.ROTATE_90)
    D.paste(cellim, (dst * 16, 0))

D.save(os.path.join(HERE, "doors.png"))
print("wrote doors.png (12px-deep doors)")
