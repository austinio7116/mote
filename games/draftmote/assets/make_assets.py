#!/usr/bin/env python3
"""DraftMote authored structure art — walls and doors, drawn at pixel level
(downscaled swatches turned to mush at band scale, so these are hand-made).

  walls_stone/red/dark.png  4x4 edge16 sheets (16px cells). Every cell carries
      the same authored pattern: 4px brick courses with staggered joints and a
      top highlight, so the game's 8px wall band shows two crisp courses no
      matter which slice it takes. tilesets/walls_*.tileset regenerated too.

  doors.png  16x16 x6: h_closed h_open v_closed v_open gold_closed gold_open
      Content is 16w x 12h anchored to the top of the cell: the door sits IN
      the 8px wall band with a 4px sill standing into the room (S doors VFLIP,
      v cells are the CCW rotation = a W door; E mirrors).

Hero/items/floors from extract_sheet2.py, furniture from make_props.py.
Bake: `mote bake games/draftmote`.
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

def snap(c):
    return ((c[0] >> 3) << 3, (c[1] >> 2) << 2, (c[2] >> 3) << 3, 255)

# ------------------------------------------------------------------- walls ---
# palette per style: brick light / brick mid / brick dark / mortar
WALLS = {
    "stone": ((126, 128, 140), (104, 106, 118), (84, 86, 98),  (56, 58, 68)),
    "red":   ((178, 92, 60),   (150, 72, 46),   (122, 56, 38), (78, 40, 30)),
    "dark":  ((96, 88, 100),   (78, 72, 84),    (62, 56, 68),  (40, 36, 46)),
}

def wall_cell(pal):
    """16x16: four 4px brick courses, joints staggered per course"""
    li, mid, dk, mortar = pal
    im = Image.new("RGBA", (16, 16))
    for y in range(16):
        course = y // 4
        for x in range(16):
            row = y % 4
            joint = (x + (8 if course % 2 else 0)) % 16 == 0
            if row == 3 or joint:
                c = mortar
            elif row == 0:
                c = li
            elif row == 1:
                c = mid
            else:
                c = mid if (x + course * 5) % 7 else dk   # speckle
            im.putpixel((x, y), snap(c))
    return im

for name, pal in WALLS.items():
    cell = wall_cell(pal)
    sheet = Image.new("RGBA", (64, 64))
    for i in range(16):
        sheet.paste(cell, ((i % 4) * 16, (i // 4) * 16))
    sheet.convert("RGB").save(os.path.join(HERE, "walls_%s.png" % name))
    lut = []
    for m in range(256):
        c = 0
        if m & 1: c |= 1
        if m & 4: c |= 2
        if m & 16: c |= 4
        if m & 64: c |= 8
        lut.append(c)
    os.makedirs(os.path.join(GAME, "tilesets"), exist_ok=True)
    with open(os.path.join(GAME, "tilesets", "walls_%s.tileset" % name), "w") as f:
        f.write("sheet assets/walls_%s.png\n" % name)
        f.write("tile 16\ntype 1\nedge 1\nnvar 1\ncols 4\nrows 4\n")
        f.write("lut " + " ".join(str(v) for v in lut) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        f.write("vweight 1 1 1 1 1 1 1 1\n")
print("wrote walls_stone/red/dark.png + tilesets (authored brickwork)")

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
    """16w x 12h, top-anchored: in the 8px band + a 4px sill into the room"""
    pl, pll, pld, sm, hd = PLANK, PLANK_L, PLANK_D, SEAM, HANDLE
    if gold:
        pl, pll, pld, sm = (196, 158, 62), (232, 198, 96), (150, 116, 44), (128, 96, 36)
        hd = (255, 244, 190)
    rect(cell, 0, 0, 1, 10, FRAME); px(cell, 0, 0, FRAME_L); px(cell, 1, 0, FRAME_L)
    rect(cell, 14, 0, 15, 10, FRAME); px(cell, 14, 0, FRAME_L); px(cell, 15, 0, FRAME_L)
    rect(cell, 0, 0, 15, 0, FRAME_L)                       # lintel
    if opened:
        rect(cell, 2, 1, 13, 9, DARK)
        rect(cell, 2, 8, 13, 9, GLOW)                      # lit threshold
    else:
        rect(cell, 2, 1, 13, 9, pl)
        for x in (5, 10):
            for y in range(1, 10):
                px(cell, x, y, sm)                         # plank seams
        rect(cell, 2, 1, 13, 1, pll)
        rect(cell, 2, 9, 13, 9, pld)
        px(cell, 12, 5, hd)
    rect(cell, 0, 11, 1, 11, (30, 24, 20))                 # sill shadow feet
    rect(cell, 14, 11, 15, 11, (30, 24, 20))

h_door(0, 0)
h_door(1, 1)
h_door(4, 0, gold=True)
h_door(5, 1, gold=True)
for (src, dst) in ((0, 2), (1, 3)):                        # v = CCW rotation (W door)
    cellim = D.crop((src * 16, 0, src * 16 + 16, 16)).transpose(Image.ROTATE_90)
    D.paste(cellim, (dst * 16, 0))

D.save(os.path.join(HERE, "doors.png"))
print("wrote doors.png (12px-deep doors)")
