#!/usr/bin/env python3
"""Compose the 60x60 launcher icon (game root icon.png) from the pack art:
king and pig face off at NATIVE resolution on a castle-brick backdrop with a
bright trim frame — reads as a bold emblem at launcher size."""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
SRC = os.path.join(GAME, "source")

def load(p):
    return Image.open(os.path.join(SRC, p)).convert("RGBA")

def crop_alpha(im):
    return im.crop(im.getchannel("A").getbbox())

terr = load("14-TileSets/Terrain (32x32).png")

icon = Image.new("RGBA", (60, 60), (57, 49, 75, 255))
# pink brick backdrop from the bg wall interior
brick = terr.crop((2 * 32, 8 * 32, 3 * 32, 9 * 32))
for y in range(0, 60, 32):
    for x in range(0, 60, 32):
        icon.alpha_composite(brick, (x, y))
# bright trim frame from the solid wall's beam tiles
top = terr.crop((2 * 32, 5 * 32, 3 * 32, 5 * 32 + 9))       # trim strip
for x in range(0, 60, 32):
    icon.alpha_composite(top.crop((0, 0, 32, 5)), (x, 0))
    icon.alpha_composite(top.crop((0, 0, 32, 5)), (x, 55))

# the duel: king (native) facing a pig (native, flipped to face left->right?)
king = crop_alpha(load("01-King Human/Idle (78x58).png").crop((0, 0, 78, 58)))
pig = crop_alpha(load("03-Pig/Idle (34x28).png").crop((0, 0, 34, 28)))
icon.alpha_composite(king, (2, 59 - 5 - king.size[1]))
icon.alpha_composite(pig, (59 - 2 - pig.size[0], 59 - 5 - pig.size[1]))

icon.save(os.path.join(GAME, "icon.png"))
print("icon.png written", icon.size)
