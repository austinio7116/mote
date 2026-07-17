#!/usr/bin/env python3
"""DraftMote launcher icon — a blueprint-blue estate grid with drafted rooms,
the hero standing in one, and a big gold key. Composes from the real art
(tiles.png / hero.png / items.png) so it matches the game.

Run after make_assets.py:  python3 assets/make_icon.py && mote bake games/draftmote
Writes ../icon.png (game root, 60x60); mote bake turns it into src/icon.h.
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

tiles = Image.open(os.path.join(HERE, "tiles.png")).convert("RGBA")
hero  = Image.open(os.path.join(HERE, "hero.png")).convert("RGBA")
items = Image.open(os.path.join(HERE, "items.png")).convert("RGBA")

cell = lambda c, r: tiles.crop((c * 16, r * 16, c * 16 + 16, r * 16 + 16))

W = H = 60
BLUE = (22, 38, 76, 255)
GRID = (46, 70, 122, 255)
LINE = (120, 160, 220, 255)

icon = Image.new("RGBA", (W, H), BLUE)
px = icon.putpixel

# blueprint grid
for v in range(0, W, 10):
    for y in range(H):
        px((v, y), GRID)
    for x in range(W):
        px((x, v), GRID)

# three drafted "rooms" (floor tiles with white blueprint outlines), stairstep
rooms = [(4, 36, cell(0, 0)), (22, 20, cell(2, 0)), (40, 4, cell(6, 0))]
for (x, y, tile) in rooms:
    icon.paste(tile, (x, y))
    for i in range(-1, 17):
        for (ox, oy) in ((x + i, y - 1), (x + i, y + 16), (x - 1, y + i), (x + 16, y + i)):
            if 0 <= ox < W and 0 <= oy < H:
                px((ox, oy), LINE)

# dashed connector lines between rooms (the draft path)
def dash(x0, y0, x1, y1):
    steps = max(abs(x1 - x0), abs(y1 - y0))
    for i in range(steps + 1):
        if (i // 2) % 2 == 0:
            x = x0 + (x1 - x0) * i // steps
            y = y0 + (y1 - y0) * i // steps
            px((x, y), (200, 220, 255, 255))

dash(20, 44, 30, 44); dash(30, 44, 30, 36)
dash(38, 28, 48, 28); dash(48, 28, 48, 20)

# hero in the first room
h = hero.crop((0, 0, 16, 16))
icon.paste(h, (4, 36), h)

# big gold key, upscaled 2x, top-left
key = items.crop((12, 0, 24, 12)).resize((24, 24), Image.NEAREST)
icon.paste(key, (2, 2), key)

out = os.path.join(GAME, "icon.png")
icon.convert("RGB").save(out)
print("wrote", out)
