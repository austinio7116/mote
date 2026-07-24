#!/usr/bin/env python3
"""Generate games/roguemote/icon.png -- the launcher emblem for the asset-library
verification game. Built straight from the CC0 tileset it catalogues: the '@'
hero glyph standing over an open red chest, on a dark dungeon-floor backdrop.
Edit the palette / composition here and re-run, then `mote bake games/roguemote`
turns icon.png into src/icon.h.
"""
import os
from PIL import Image

S = 60
TS = 8
HERE = os.path.dirname(os.path.abspath(__file__))          # games/roguemote/assets
GAME = os.path.dirname(HERE)                                # games/roguemote
SRC = Image.open(os.path.join(GAME, "authoring", "source_tileset.png"))
IDX = SRC.load()
PAL = SRC.getpalette()
TRANSPARENT_IDX = 0

def tile(c, r, scale):
    """One 8x8 source tile as RGBA, idx0 -> transparent, nearest-neighbour scaled."""
    im = Image.new("RGBA", (TS, TS))
    px = im.load()
    for y in range(TS):
        for x in range(TS):
            ix = IDX[c * TS + x, r * TS + y]
            px[x, y] = (0, 0, 0, 0) if ix == TRANSPARENT_IDX else (PAL[ix*3], PAL[ix*3+1], PAL[ix*3+2], 255)
    return im.resize((TS * scale, TS * scale), Image.NEAREST)

# --- background: dark cobble floor with a faint gold glow behind the hero ---
img = Image.new("RGBA", (S, S), (0, 0, 0, 255))
for y in range(S):
    for x in range(S):
        dx, dy = x - S * 0.5, y - S * 0.4
        r = (dx * dx + dy * dy) ** 0.5
        g = max(0, 1.0 - r / 38.0)
        img.putpixel((x, y), (int(20 * g) + 14, int(16 * g) + 13, int(10 * g) + 16, 255))

# --- the '@' hero glyph, bold and upper-left ---------------------------------
hero = tile(32, 0, 5)                                         # 40x40
img.alpha_composite(hero, (10, 2))

# --- open red chest, lower-right, holding the loot ---------------------------
chest = tile(1, 1, 3)                                          # open chest, red row (24x24)
img.alpha_composite(chest, (36, 40))

# subtle vignette border
from PIL import ImageDraw
d = ImageDraw.Draw(img)
d.rectangle([0, 0, S - 1, S - 1], outline=(60, 55, 45, 255))

out = os.path.join(GAME, "icon.png")
img.convert("RGB").save(out)
print("wrote", out)
