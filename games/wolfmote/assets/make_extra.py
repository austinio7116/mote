#!/usr/bin/env python3
"""Wolfmote expansion assets (EDITABLE SOURCES; `mote bake` -> src/*.h).
  key.png       gold key pickup            treasure.png  cross/chalice/crown (3 cells)
  blood.png     corpse pool decal          gun_shotgun.png double-barrel HUD gun
  rusher.png    guard recoloured red       boss.png      brute recoloured, menacing
Recolours derive from the existing guard/brute art (hue-shift, not redrawn)."""
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

# ---- enemy recolours: hue-shifted derivatives of the existing art ----
def hueshift(src, dst, dh, sat=1.0, val=1.0, sel_h=None, sel_w=0.16):
    """recolour; sel_h limits the shift to hues near sel_h (e.g. just the uniform),
    leaving skin/blood/steel untouched"""
    im = Image.open(f"{AD}/{src}").convert("RGBA")
    a = np.asarray(im).astype(np.float32)/255.0
    out = a.copy()
    for y in range(a.shape[0]):
        for x in range(a.shape[1]):
            r,g,b,al = a[y,x]
            if al < 0.5: continue
            h,s,v = colorsys.rgb_to_hsv(r,g,b)
            if sel_h is not None:
                dd = abs(((h - sel_h + 0.5) % 1.0) - 0.5)
                if dd > sel_w or s < 0.25: continue      # keep skin + greys
            r2,g2,b2 = colorsys.hsv_to_rgb((h+dh)%1.0, min(1,s*sat), min(1,v*val))
            out[y,x] = (r2,g2,b2,al)
    Image.fromarray((out*255).astype(np.uint8),"RGBA").save(f"{AD}/{dst}")

hueshift("guard.png", "rusher.png",   dh=+0.40, sat=1.2, val=1.02, sel_h=0.62)  # uniform blue -> RED
hueshift("guard.png", "commando.png", dh=-0.28, sat=1.1, val=0.95, sel_h=0.62)  # uniform blue -> GREEN
hueshift("brute.png", "boss.png",     dh=+0.18, sat=1.15, val=0.82)             # sickly dark: the boss
print("wrote blood, rusher, commando, boss")
