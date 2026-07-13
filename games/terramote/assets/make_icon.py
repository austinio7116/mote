#!/usr/bin/env python3
"""TerraMote launcher icon — composes the game's "ground and trees" starting view
(sky + trees + grass line + dirt) from the real art PNGs, so it matches the game
exactly. Writes ../icon.png (game root); `mote bake` turns it into src/icon.h.

Run after make_tiles.py (needs tiles_dirt / grass_cap / tiles_trunk / canopy):
    python3 assets/make_icon.py && mote bake games/terramote
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

dirt   = Image.open(os.path.join(HERE, "tiles_dirt.png")).convert("RGBA")
grass  = Image.open(os.path.join(HERE, "grass_cap.png")).convert("RGBA")
trunk  = Image.open(os.path.join(HERE, "tiles_trunk.png")).convert("RGBA")
canopy = Image.open(os.path.join(HERE, "canopy.png")).convert("RGBA")

# cells (see make_tiles.py layouts)
dirt_i     = dirt.crop((48, 40, 56, 48))     # blob47 interior cell 46
grass_top  = grass.crop((0, 0, 8, 8))        # grass cap, top edge
trunk_mid  = trunk.crop((8, 8, 16, 16))      # edge16 idx5: N+S (vertical middle)
trunk_base = trunk.crop((8, 0, 16, 8))       # edge16 idx1: N only (root base)
crownA     = canopy.crop((0, 0, 40, 28))     # leafy variant 0
crownB     = canopy.crop((40, 0, 80, 28))    # leafy variant 1

W = H = 60
GROUND_Y = 40            # grass line
icon = Image.new("RGBA", (W, H), (0, 0, 0, 255))

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3)) + (255,)

# sky: the game's day gradient (brighter up high, deeper toward the horizon)
SKY_TOP, SKY_HOR = (98, 174, 250), (66, 140, 228)
for y in range(GROUND_Y):
    c = lerp(SKY_TOP, SKY_HOR, y / GROUND_Y)
    for x in range(W):
        icon.putpixel((x, y), c)

# ground: tiled dirt, grass caps on the top row
for ty in range(GROUND_Y, H, 8):
    for tx in range(0, W, 8):
        icon.paste(dirt_i, (tx, ty))
for tx in range(0, W, 8):
    icon.paste(grass_top, (tx, GROUND_Y), grass_top)

def tree(cx, crown, scale, crown_top):
    """Trunk (8px, centred on cx) rooted at the grass line; the crown is pasted
    LAST over a trunk that rises into the foliage, so there is no trunk/crown gap."""
    cw, ch = int(round(40 * scale)), int(round(28 * scale))
    cr = crown.resize((cw, ch), Image.NEAREST)
    trunk_into = crown_top + ch // 2      # trunk must reach into the crown's lower half
    tx = cx - 4
    y = GROUND_Y - 8
    icon.paste(trunk_base, (tx, y), trunk_base); y -= 8
    while y + 8 > trunk_into:             # extend past the crown bottom (crown hides it)
        icon.paste(trunk_mid, (tx, y), trunk_mid); y -= 8
    icon.paste(cr, (cx - cw // 2, crown_top), cr)   # crown over the junction

# a forest: one prominent tree left-of-centre, a second to the right.
# crown_top is the actual top of the crown (small = tall tree); the trunk rises
# into the crown so the junction is always covered.
tree(20, crownA, 0.82, 2)
tree(45, crownB, 0.62, 11)

icon.convert("RGB").save(os.path.join(GAME, "icon.png"))
print("[icon] wrote", os.path.join(GAME, "icon.png"))
