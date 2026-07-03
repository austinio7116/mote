#!/usr/bin/env python3
"""Extract the 24-item pickup/scenery sheet (user-supplied) -> props24.png.
Source is a 6x4 grid on a baked-in transparency checkerboard. Cells land
row-major in fixed 28x28 cells (scaled to fit, centred), one sheet:
  0 medkit      1 bigmedkit  2 chalice   3 crown     4 chest    5 goldpile
  6 idol        7 ammocrate  8 ammobox   9 goldcard 10 KEY     11 pillar
 12 crackpillar 13 barrel   14 wbarrel  15 crates   16 hanglamp 17 torch
 18 lamppost   19 candles   20 plant    21 skull    22 knight  23 banner"""
import numpy as np
from PIL import Image
from scipy import ndimage
from collections import Counter

SRC = "/tmp/ChatGPT Image Jul 3, 2026, 07_53_58 AM.png"
OUT = "/home/maustin/thumby-color/mote/games/wolfmote/assets/props24.png"
CS = 28

im = np.asarray(Image.open(SRC).convert("RGB")).astype(int)
border = np.concatenate([im[:20].reshape(-1,3), im[-20:].reshape(-1,3),
                         im[:, :20].reshape(-1,3), im[:, -20:].reshape(-1,3)])
(c1,_),(c2,_) = Counter(map(tuple,border)).most_common(2)
def near(px,c,tol=18): return (np.abs(px-np.array(c)).sum(axis=2))<tol
fg = ~(near(im,c1)|near(im,c2))
fg = ndimage.binary_opening(fg, structure=np.ones((3,3)))
lab,_ = ndimage.label(fg)
items=[]
for sl in ndimage.find_objects(lab):
    h,w = sl[0].stop-sl[0].start, sl[1].stop-sl[1].start
    if h<60 or w<60: continue
    m = ndimage.binary_fill_holes(lab[sl]>0)
    rgba = np.dstack([im[sl], np.where(m,255,0)]).astype(np.uint8)
    items.append((sl[0].start, sl[1].start, Image.fromarray(rgba,"RGBA")))
# row-major ordering by y-centre bands then x
items.sort(key=lambda t:t[0])
rows,cur=[],[items[0]]
for it in items[1:]:
    if abs(it[0]-cur[0][0])<120: cur.append(it)
    else: rows.append(cur); cur=[it]
rows.append(cur)
cells=[]
for r in rows:
    r.sort(key=lambda t:t[1]); cells += [t[2] for t in r]
assert len(cells)==27, f"expected 24 props + 3 guns, got {len(cells)}"
guns, cells = cells[24:], cells[:24]

sheet=Image.new("RGBA",(CS*6,CS*4),(0,0,0,0))
for i,c in enumerate(cells):
    sc=(CS-2)/max(c.size)
    r=c.resize((max(1,round(c.size[0]*sc)),max(1,round(c.size[1]*sc))),Image.LANCZOS)
    a=np.asarray(r).copy(); hard=a[:,:,3]>110
    a[:,:,3]=np.where(hard,255,0); a[~hard]=0
    r=Image.fromarray(a,"RGBA")
    sheet.paste(r,((i%6)*CS+(CS-r.size[0])//2,(i//6)*CS+(CS-r.size[1])//2))
sheet.save(OUT)
print(f"wrote props24.png ({len(cells)} cells of {CS}x{CS})")

# ---- bottom row: gun PICKUP profiles -> wpickup.png (3 cells of 32x16) ----
GW, GH = 32, 16
gs = Image.new("RGBA", (GW*3, GH), (0,0,0,0))
for i, g in enumerate(guns):
    sc = min((GW-2)/g.size[0], (GH-2)/g.size[1])
    r = g.resize((max(1,round(g.size[0]*sc)), max(1,round(g.size[1]*sc))), Image.LANCZOS)
    a = np.asarray(r).copy(); hard = a[:,:,3] > 110
    a[:,:,3] = np.where(hard,255,0); a[~hard] = 0
    r = Image.fromarray(a,"RGBA")
    gs.paste(r, (i*GW+(GW-r.size[0])//2, (GH-r.size[1])//2))
gs.save("/home/maustin/thumby-color/mote/games/wolfmote/assets/wpickup.png")
print("wrote wpickup.png (pistol/shotgun/chaingun profiles, 32x16 cells)")
