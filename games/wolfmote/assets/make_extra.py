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

# ---- key: chunky gold key, reads at 12px ----
im = Image.new("RGBA", (12*S, 12*S), (0,0,0,0)); d = ImageDraw.Draw(im)
GOLD=(232,190,60,255); DK=(150,110,20,255)
d.ellipse([1*S,1*S,6*S,6*S], outline=GOLD, width=S)                # bow
d.rectangle([5.4*S,3*S,10.6*S,4.4*S], fill=GOLD)                   # shaft
d.rectangle([8.6*S,4.4*S,9.6*S,6.4*S], fill=GOLD)                  # teeth
d.rectangle([10*S,4.4*S,10.8*S,5.6*S], fill=GOLD)
d.line([2*S,2*S,5*S,5*S], fill=DK, width=S//2)
down(im,12,12).save(f"{AD}/key.png")

# ---- treasure: cross / chalice / crown ----
sheet = Image.new("RGBA", (36,12), (0,0,0,0))
def cell():
    return Image.new("RGBA",(12*S,12*S),(0,0,0,0))
c = cell(); d = ImageDraw.Draw(c)                                   # cross
d.rectangle([4.8*S,1*S,7.2*S,11*S], fill=GOLD); d.rectangle([2*S,3.4*S,10*S,5.8*S], fill=GOLD)
d.rectangle([5.4*S,1.6*S,6.0*S,10.2*S], fill=(255,232,150,255))
sheet.paste(down(c,12,12),(0,0))
c = cell(); d = ImageDraw.Draw(c)                                   # chalice
d.polygon([(2*S,1.5*S),(10*S,1.5*S),(8*S,5.5*S),(4*S,5.5*S)], fill=GOLD)
d.rectangle([5.2*S,5.5*S,6.8*S,9*S], fill=GOLD); d.rectangle([3*S,9*S,9*S,10.6*S], fill=GOLD)
d.ellipse([4*S,2*S,6*S,3.6*S], fill=(255,232,150,255))
sheet.paste(down(c,12,12),(12,0))
c = cell(); d = ImageDraw.Draw(c)                                   # crown
d.polygon([(1.5*S,10*S),(1.5*S,3*S),(4*S,6*S),(6*S,2*S),(8*S,6*S),(10.5*S,3*S),(10.5*S,10*S)], fill=GOLD)
for x,y in [(2.6,3.2),(6,2.2),(9.4,3.2)]:
    d.ellipse([(x-0.7)*S,(y-0.7)*S,(x+0.7)*S,(y+0.7)*S], fill=(220,60,70,255))
sheet.paste(down(c,12,12),(24,0))
sheet.save(f"{AD}/treasure.png")

# ---- blood pool ----
im = Image.new("RGBA", (16*S, 8*S), (0,0,0,0)); d = ImageDraw.Draw(im)
d.ellipse([1*S,1*S,15*S,7*S], fill=(112,16,20,255))
d.ellipse([3*S,2*S,10*S,5.5*S], fill=(150,24,26,255))
d.ellipse([10*S,3.5*S,14*S,6.5*S], fill=(88,10,14,255))
down(im,16,8).save(f"{AD}/blood.png")

# ---- double-barrel shotgun (first-person HUD) ----
im = Image.new("RGBA", (36*S, 40*S), (0,0,0,0)); d = ImageDraw.Draw(im)
GM=(96,100,112,255); GMD=(48,50,60,255); WOOD=(122,80,44,255); WOODD=(88,56,30,255)
d.rounded_rectangle([10*S,20*S,26*S,40*S], radius=2*S, fill=WOOD, outline=WOODD, width=S)   # stock/pump
d.rounded_rectangle([11*S,4*S,25*S,24*S], radius=3*S, fill=GM, outline=GMD, width=S)        # receiver
d.ellipse([12.4*S,5*S,17.8*S,10.4*S], fill=(20,20,26,255), outline=GMD, width=S)            # left muzzle
d.ellipse([18.2*S,5*S,23.6*S,10.4*S], fill=(20,20,26,255), outline=GMD, width=S)            # right muzzle
d.ellipse([13.8*S,6.4*S,15.4*S,8.0*S], fill=(70,74,86,255))
d.ellipse([19.6*S,6.4*S,21.2*S,8.0*S], fill=(70,74,86,255))
d.rectangle([17.4*S,11*S,18.6*S,22*S], fill=GMD)                                            # rib
down(im,36,40).save(f"{AD}/gun_shotgun.png")

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
print("wrote key, treasure, blood, gun_shotgun, rusher, boss")
