#!/usr/bin/env python3
"""Launcher icon (60x60) styled after the title screen: darkened brick wall,
red frame, WOLF/MOTE stacked in chunky red caps. Writes ../icon.png; then
`mote bake` regenerates src/icon.h."""
import numpy as np
from PIL import Image

AD = "/home/maustin/thumby-color/mote/games/wolfmote/assets"
S = 60

F = {  # 5x5 glyphs
 'W':["10001","10001","10101","10101","01010"],
 'O':["01110","10001","10001","10001","01110"],
 'L':["10000","10000","10000","10000","11111"],
 'F':["11111","10000","11110","10000","10000"],
 'M':["10001","11011","10101","10001","10001"],
 'T':["11111","00100","00100","00100","00100"],
 'E':["11111","10000","11110","10000","11111"],
}

def draw_word(px, word, x0, y0, sc, col, shadow):
    for k,(dx,dy,c) in enumerate([(sc//2,sc//2,shadow),(0,0,col)]):
        x = x0+dx
        for ch in word:
            g = F[ch]
            for r in range(5):
                for cc in range(5):
                    if g[r][cc]=='1':
                        px[y0+dy+r*sc:y0+dy+(r+1)*sc, x+cc*sc:x+(cc+1)*sc] = c
            x += 6*sc

# backdrop: the brick wall texture, darkened like the title card
wall = np.asarray(Image.open(f"{AD}/wallbrick.png").convert("RGB").resize((S,S), Image.LANCZOS)).astype(int)
px = (wall * 0.30).astype(int)
px[:, :] = px * 0.9 + np.array([8, 4, 6]) * 0.9        # cool it toward the card tint
px[6:41, 4:-4] = px[6:41, 4:-4] * 0.45                 # near-black panel behind the lettering

RED    = np.array([224, 60, 48])
DARKR  = np.array([96, 22, 18])
BLACK  = np.array([12, 8, 10])

# red frame, 2px, with a dark inner line (matches the title card border)
px[:2,:]=RED; px[-2:,:]=RED; px[:,:2]=RED; px[:,-2:]=RED
px[2:3,2:-2]=DARKR; px[-3:-2,2:-2]=DARKR; px[2:-2,2:3]=DARKR; px[2:-2,-3:-2]=DARKR

# WOLF / MOTE stacked, 2x glyphs (word = 4*10 + 3*2 = 46 wide)
draw_word(px, "WOLF", 7,  9, 2, RED, BLACK)
draw_word(px, "MOTE", 7, 24, 2, RED, BLACK)

# green "A ROGUE DUNGEON" nod: a tiny exit-arrow chevron centred below
G = np.array([70, 220, 90])
cx, cy = S//2, 47
for r in range(4):
    px[cy+r, cx-1-r:cx+1+r] = G if r<3 else DARKR*0+G*0.45

Image.fromarray(np.clip(px,0,255).astype(np.uint8), "RGB").save(
    "/home/maustin/thumby-color/mote/games/wolfmote/icon.png")
print("wrote icon.png (60x60)")
