#!/usr/bin/env python3
"""DraftMote door tiles — original pixel art (the extracted sheet doors read
poorly at 16px, so these are authored to match the thin-wall look).

  doors.png  16x16 x6: h_closed h_open v_closed v_open gold_closed gold_open

An h door hangs from the top wall band: dark frame posts, plank leaf with a
handle, drawn 16px tall so it stands proud of the 8px wall (S doors VFLIP,
E/W use the rotated v cells). Open = dark passage with a lit threshold.

Hero + items + floors + walls come from extract_sheet2.py; furniture from
make_props.py. Bake: `mote bake games/draftmote`.
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

FRAME  = (52, 40, 34)
FRAME_L = (78, 62, 50)
PLANK  = (146, 96, 50)
PLANK_L = (172, 120, 66)
PLANK_D = (108, 68, 36)
SEAM   = (86, 54, 30)
HANDLE = (222, 186, 92)
DARK   = (16, 13, 12)
GLOW   = (64, 50, 34)

def snap(c):
    return ((c[0] >> 3) << 3, (c[1] >> 2) << 2, (c[2] >> 3) << 3, 255)

D = Image.new("RGBA", (6 * 16, 16), (0, 0, 0, 0))

def px(cell, x, y, c):
    if 0 <= x < 16 and 0 <= y < 16 and c is not None:
        D.putpixel((cell * 16 + x, y), snap(c))

def rect(cell, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px(cell, x, y, c)

def h_door(cell, opened, gold=False):
    pl, pll, pld, sm, hd = PLANK, PLANK_L, PLANK_D, SEAM, HANDLE
    if gold:
        pl, pll, pld, sm = (196, 158, 62), (232, 198, 96), (150, 116, 44), (128, 96, 36)
        hd = (255, 244, 190)
    rect(cell, 1, 0, 2, 14, FRAME); px(cell, 1, 0, FRAME_L); px(cell, 2, 0, FRAME_L)
    rect(cell, 13, 0, 14, 14, FRAME); px(cell, 13, 0, FRAME_L); px(cell, 14, 0, FRAME_L)
    rect(cell, 1, 0, 14, 1, FRAME_L)                      # lintel
    if opened:
        rect(cell, 3, 1, 12, 13, DARK)
        rect(cell, 3, 12, 12, 13, GLOW)                    # lit threshold
        px(cell, 5, 10, (40, 32, 24)); px(cell, 10, 9, (40, 32, 24))
    else:
        rect(cell, 3, 1, 12, 13, pl)
        for x in (6, 9):
            for y in range(1, 14):
                px(cell, x, y, sm)                        # plank seams
        rect(cell, 3, 1, 12, 1, pll)                      # top catch
        rect(cell, 3, 13, 12, 13, pld)
        px(cell, 11, 7, hd); px(cell, 11, 8, (150, 116, 44) if not gold else (190, 150, 60))
    # feet shadow
    px(cell, 2, 15, (30, 24, 20)); px(cell, 13, 15, (30, 24, 20))

h_door(0, 0)
h_door(1, 1)
h_door(4, 0, gold=True)
h_door(5, 1, gold=True)
# v doors = rotated h doors (E wall; game HFLIPs for W)
for (src, dst) in ((0, 2), (1, 3)):
    cellim = D.crop((src * 16, 0, src * 16 + 16, 16)).transpose(Image.ROTATE_90)
    D.paste(cellim, (dst * 16, 0))

D.save(os.path.join(HERE, "doors.png"))
print("wrote doors.png (authored)")
