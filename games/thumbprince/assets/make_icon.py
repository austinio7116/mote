#!/usr/bin/env python3
"""ThumbPrince launcher icon — blueprint estate grid, drafted floor swatches,
the caped hero and a big gold key, all from the real game sheets.

Run after extract_sheet2.py:
    python3 assets/make_icon.py && mote bake games/thumbprince
Writes ../icon.png (game root, 60x60); mote bake turns it into src/icon.h.
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

hero   = Image.open(os.path.join(HERE, "hero.png")).convert("RGBA")
items  = Image.open(os.path.join(HERE, "items.png")).convert("RGBA")
floors = Image.open(os.path.join(HERE, "floors.png")).convert("RGBA")
doors  = Image.open(os.path.join(HERE, "doors.png")).convert("RGBA")

W = H = 60
BLUE = (22, 38, 76, 255)
GRID = (46, 70, 122, 255)
LINE = (150, 190, 245, 255)

icon = Image.new("RGBA", (W, H), BLUE)
px = icon.putpixel

for v in range(0, W, 10):
    for y in range(H):
        px((v, y), GRID)
    for x in range(W):
        px((x, v), GRID)

# three drafted rooms (floor swatches) climbing toward the gold door
rooms = [(4, 38, 0), (22, 21, 3), (40, 4, 2)]     # wood, blue carpet, red carpet
for (x, y, fl) in rooms:
    icon.paste(floors.crop((fl * 16, 0, fl * 16 + 16, 16)), (x, y))
    for i in range(-1, 17):
        for (ox, oy) in ((x + i, y - 1), (x + i, y + 16), (x - 1, y + i), (x + 16, y + i)):
            if 0 <= ox < W and 0 <= oy < H:
                px((ox, oy), LINE)

def dash(x0, y0, x1, y1):
    steps = max(abs(x1 - x0), abs(y1 - y0))
    for i in range(steps + 1):
        if (i // 2) % 2 == 0:
            px((x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps), (210, 230, 255, 255))

dash(21, 46, 30, 46); dash(30, 46, 30, 38)
dash(39, 29, 48, 29); dash(48, 29, 48, 21)

# gold door inside the top room
gd = doors.crop((4 * 16, 0, 5 * 16, 16))
icon.paste(gd, (40, 4), gd)

# hero on the first room
h = hero.crop((0, 0, 16, 20))
icon.paste(h, (4, 33), h)

# big gold key
key = items.crop((6 * 12, 0, 7 * 12, 12)).resize((22, 22), Image.NEAREST)
icon.paste(key, (36, 37), key)

out = os.path.join(GAME, "icon.png")
icon.convert("RGB").save(out)
print("wrote", out)
