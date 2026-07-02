#!/usr/bin/env python3
"""Extract the long vehicles (school bus + limo) into assets/bus.png — 2 cells,
at the SAME world scale as the cars2 sheet, so relative sizes stay true.
Prints the per-cell art sizes + cell size to wire into game.c."""
import numpy as np
from PIL import Image
from scipy import ndimage

SRC   = "/tmp/e6934045-a4d4-4b8c-bcda-0c93d5308013.png"
CARS0 = "/tmp/5bd02cd1-41b6-4fc2-b39c-0c7d66662c38.png"   # to recompute the cars' scale
TARGET_MEDIAN_H = 46.0

def segment(path):
    im = np.asarray(Image.open(path).convert("RGB")).astype(int)
    bg = np.median(im[2:20, 2:20].reshape(-1, 3), axis=0)
    mask = np.abs(im - bg).sum(axis=2) > 60
    mask = ndimage.binary_fill_holes(ndimage.binary_closing(mask, structure=np.ones((5,5))))
    lab, _ = ndimage.label(mask)
    out=[]
    for sl in ndimage.find_objects(lab):
        h,w = sl[0].stop-sl[0].start, sl[1].stop-sl[1].start
        if h<60 or w<30: continue
        c=im[sl]; m=mask[sl]
        out.append((sl[1].start, Image.fromarray(np.dstack([c,np.where(m,255,0)]).astype(np.uint8),"RGBA")))
    out.sort(key=lambda t:t[0])
    return [t[1] for t in out]

# cars' global scale (median car height -> 46 px), recomputed for consistency
hs = sorted(c.size[1] for c in segment(CARS0))
scale = TARGET_MEDIAN_H / hs[len(hs)//2]

vehs = segment(SRC)
assert len(vehs)==2, len(vehs)
scaled=[]
for v in vehs:
    w,h = max(1,round(v.size[0]*scale)), max(1,round(v.size[1]*scale))
    r=v.resize((w,h), Image.LANCZOS)
    a=np.asarray(r); rr=a.copy(); rr[:,:,3]=np.where(a[:,:,3]>110,255,0)
    scaled.append(Image.fromarray(rr,"RGBA"))

CW = max(v.size[0] for v in scaled)+2; CW += CW&1
CH = max(v.size[1] for v in scaled)+2; CH += CH&1
sheet=Image.new("RGBA",(CW*2,CH),(0,0,0,0))
for i,v in enumerate(scaled):
    sheet.paste(v,(i*CW+(CW-v.size[0])//2,(CH-v.size[1])//2))
sheet.save("/home/maustin/thumby-color/mote/games/grandthumbauto/assets/bus.png")
MPP=0.098
for i,v in enumerate(scaled):
    print(f"cell {i}: art {v.size[0]}x{v.size[1]}px -> {v.size[0]*MPP:.2f} x {v.size[1]*MPP:.2f} m")
print(f"game constants: BUS_CW {CW}  BUS_CH {CH}  aw/ah per cell above")
