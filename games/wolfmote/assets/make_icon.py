#!/usr/bin/env python3
"""Launcher icon (60x60) styled after the title screen: darkened brick wall,
red frame, WOLF/MOTE stacked in chunky red caps. Writes ../icon.png; then
`mote bake` regenerates src/icon.h."""
import numpy as np
from PIL import Image

AD = "/home/maustin/thumby-color/mote/games/wolfmote/assets"
S = 60

from PIL import ImageDraw, ImageFont
FONT = ImageFont.truetype(f"{AD}/title.ttf", 22)

def draw_word(img, word, y0, col, shadow):
    d = ImageDraw.Draw(img)
    bb = d.textbbox((0,0), word, font=FONT)
    x0 = (S - (bb[2]-bb[0]))//2 - bb[0]
    d.text((x0+1, y0+1-bb[1]), word, font=FONT, fill=tuple(shadow))
    d.text((x0,   y0-bb[1]),   word, font=FONT, fill=tuple(col))

# backdrop: the brick wall texture, darkened like the title card
wall = np.asarray(Image.open(f"{AD}/wallbrick.png").convert("RGB").resize((S,S), Image.LANCZOS)).astype(int)
px = (wall * 0.30).astype(int)
px[:, :] = px * 0.9 + np.array([8, 4, 6]) * 0.9        # cool it toward the card tint
px[5:47, 4:-4] = px[5:47, 4:-4] * 0.45                 # near-black panel behind the lettering

RED    = np.array([224, 60, 48])
DARKR  = np.array([96, 22, 18])
BLACK  = np.array([12, 8, 10])

# red frame, 2px, with a dark inner line (matches the title card border)
px[:2,:]=RED; px[-2:,:]=RED; px[:,:2]=RED; px[:,-2:]=RED
px[2:3,2:-2]=DARKR; px[-3:-2,2:-2]=DARKR; px[2:-2,2:3]=DARKR; px[2:-2,-3:-2]=DARKR

# WOLF / MOTE stacked in the title blackletter (Pirata One, same as in-game)
img = Image.fromarray(np.clip(px,0,255).astype(np.uint8), "RGB")
draw_word(img, "WOLF", 7,  RED, BLACK)
draw_word(img, "MOTE", 27, RED, BLACK)
px = np.asarray(img).astype(int)

# green "A ROGUE DUNGEON" nod: a tiny exit-arrow chevron centred below
G = np.array([70, 220, 90])
cx, cy = S//2, 51
for r in range(4):
    px[cy+r, cx-1-r:cx+1+r] = G if r<3 else DARKR*0+G*0.45

Image.fromarray(np.clip(px,0,255).astype(np.uint8), "RGB").save(
    "/home/maustin/thumby-color/mote/games/wolfmote/icon.png")
print("wrote icon.png (60x60)")
