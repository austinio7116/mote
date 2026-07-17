#!/usr/bin/env python3
"""ThumbPrince authored structure art — walls and doors, drawn at pixel level
(downscaled swatches turned to mush at band scale, so these are hand-made).

  walls_stone/red/dark.png  3x3 NINESLICE rule tilesets (48x48, 16px cells):
      the 8px band hugs each cell's outer side, interior transparent. Bricks
      8x4 fit the band. tilesets/walls_*.tileset for the Studio Tiles tab.

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

# ------------------------------------------------------------------- walls ---
# NINESLICE rule tilesets (the engine's rule type for rectangular frames):
# 3x3 cells of 16px; the 8px band hugs the outer side of each cell, interior
# is transparent so the floor shows through. Bricks are 8x4 - complete bricks
# at band scale. Editable in the Studio Tiles tab (tilesets/walls_*.tileset).
WALLS = {
    "stone": ((126, 128, 140), (104, 106, 118), (84, 86, 98),  (56, 58, 68)),
    "red":   ((178, 92, 60),   (150, 72, 46),   (122, 56, 38), (78, 40, 30)),
    "dark":  ((96, 88, 100),   (78, 72, 84),    (62, 56, 68),  (40, 36, 46)),
}

def brick_px(x, y, pal, joints):
    li, mid, dk, mortar = pal
    row = y % 4
    if row == 3 or (x % 8) in joints[(y // 4) % 2]:
        return mortar
    if row == 0:
        return li
    return mid if (x * 3 + y * 7) % 11 else dk

def wall_nineslice(pal):
    im = Image.new("RGBA", (48, 48), (0, 0, 0, 0))
    def hband(cx, cy, yoff):
        for y in range(8):
            for x in range(16):
                im.putpixel((cx * 16 + x, cy * 16 + yoff + y), snap(brick_px(x, y, pal, ((0,), (4,)))))
    def vband(cx, cy, xoff):
        for y in range(16):
            for x in range(8):
                im.putpixel((cx * 16 + xoff + x, cy * 16 + y), snap(brick_px(x, y, pal, ((0,), (4,)))))
    hband(0, 0, 0); hband(1, 0, 0); hband(2, 0, 0)      # top row: band on top
    hband(0, 2, 8); hband(1, 2, 8); hband(2, 2, 8)      # bottom row: band below
    vband(0, 0, 0); vband(0, 1, 0); vband(0, 2, 0)      # left col: band left
    vband(2, 0, 8); vband(2, 1, 8); vband(2, 2, 8)      # right col: band right
    return im

TILESETS = os.path.join(GAME, "tilesets")
os.makedirs(TILESETS, exist_ok=True)
for name, pal in WALLS.items():
    wall_nineslice(pal).save(os.path.join(HERE, "walls_%s.png" % name))
    # nineslice rule: row from N/S same-neighbours, column from W/E
    N, E, S, W = 1, 4, 16, 64
    lut = []
    for m in range(256):
        row = 1 if (m & N and m & S) else (0 if not (m & N) else 2)
        col = 1 if (m & W and m & E) else (0 if not (m & W) else 2)
        lut.append(row * 3 + col)
    with open(os.path.join(TILESETS, "walls_%s.tileset" % name), "w") as f:
        f.write("sheet assets/walls_%s.png\n" % name)
        f.write("tile 16\ntype 2\nedge 1\nnvar 1\ncols 3\nrows 3\n")
        f.write("lut " + " ".join(str(v) for v in lut) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        f.write("vweight 1 1 1 1 1 1 1 1\n")
print("wrote walls_*.png nineslice + tilesets")

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
