#!/usr/bin/env python3
"""Wolfmote expansion assets (EDITABLE SOURCES; `mote bake` -> src/*.h).
  key.png       gold key pickup            treasure.png  cross/chalice/crown (3 cells)
  blood.png     corpse pool decal          gun_shotgun.png double-barrel HUD gun
(enemy sheets + weapons + props are EXTRACTED from supplied art — see
extract_enemies.py / extract_guns.py / extract_props.py)"""
import numpy as np, colorsys
from PIL import Image, ImageDraw

AD = "/home/maustin/thumby-color/mote/games/wolfmote/assets"
S = 6

def down(im, w, h): return im.resize((w, h), Image.LANCZOS)

# ---- blood pool ----
im = Image.new("RGBA", (16*S, 8*S), (0,0,0,0)); d = ImageDraw.Draw(im)
d.ellipse([1*S,1*S,15*S,7*S], fill=(112,16,20,255))
d.ellipse([3*S,2*S,10*S,5.5*S], fill=(150,24,26,255))
d.ellipse([10*S,3.5*S,14*S,6.5*S], fill=(88,10,14,255))
down(im,16,8).save(f"{AD}/blood.png")

print("wrote blood")
