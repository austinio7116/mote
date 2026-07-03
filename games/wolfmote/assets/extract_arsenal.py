#!/usr/bin/env python3
"""Extract the 6-weapon arsenal (user-supplied, July 3 PM sheets).
  FP sheet   -> weapons2.png : 6 cells 72x56 (knife smg dblshot flamer cannon plasma)
  icon sheet -> wpickup2.png : 6 cells 32x16 (same order, rows 2-3 of the source)
NEVER writes weapons.png / wpickup.png directly — those are the COMBINED 9-cell sheets
(order: knife pist smg shot dbl chain fire cann plas) holding the user's hand-cleaned
art. This script emits weapons2/wpickup2 STAGING sheets; merge cells in manually.
             -> ammo3.png   : 3 cells 20x20 (fuel can / cannonballs / energy cell)
Also bakes the projectile orbs (fireball/cannonball/plasma bolt) as radial sprites."""
import sys, os
if os.environ.get("MOTE_REGEN")!="1":   # REGEN_GUARD
    sys.exit("extract_arsenal: refusing to run — set MOTE_REGEN=1. Outputs may hold HAND-EDITED art.")
import numpy as np
from PIL import Image
from scipy import ndimage
from collections import Counter

AD  = "/home/maustin/thumby-color/mote/games/wolfmote/assets"
FP  = "/tmp/ChatGPT Image Jul 3, 2026, 04_11_11 PM.png"
IC  = "/tmp/ChatGPT Image Jul 3, 2026, 04_15_23 PM.png"

def key_out(path, minsize):
    im = np.asarray(Image.open(path).convert("RGB")).astype(int)
    border = np.concatenate([im[:24].reshape(-1,3), im[-24:].reshape(-1,3),
                             im[:, :24].reshape(-1,3), im[:, -24:].reshape(-1,3)])
    (c1,_),(c2,_) = Counter(map(tuple,border)).most_common(2)
    def near(px,c,tol=18): return (np.abs(px-np.array(c)).sum(axis=2))<tol
    fg = ~(near(im,c1)|near(im,c2))
    fg = ndimage.binary_opening(fg, structure=np.ones((3,3)))
    lab,_ = ndimage.label(fg)
    out=[]
    for sl in ndimage.find_objects(lab):
        h,w = sl[0].stop-sl[0].start, sl[1].stop-sl[1].start
        if h<minsize or w<minsize: continue
        m = ndimage.binary_fill_holes(lab[sl]>0)
        rgba = np.dstack([im[sl], np.where(m,255,0)]).astype(np.uint8)
        out.append((sl[0].start, sl[1].start, Image.fromarray(rgba,"RGBA")))
    return out

def harden(img):
    a=np.asarray(img).copy(); hard=a[:,:,3]>110
    a[:,:,3]=np.where(hard,255,0); a[~hard]=0
    return Image.fromarray(a,"RGBA")

def strip_shadow(img):
    """The icon sheet paints a soft drop shadow under every item; it is fused to
    the art, so colour rules can't isolate it. It lives in the bottom ~9%% of the
    item, and at 32x16 icon scale a straight crop there is invisible — cut it,
    then trim any leftover pale neutral rows."""
    a=np.asarray(img).astype(int)
    a=a[:int(a.shape[0]*0.91)]
    while a.shape[0]>2:
        row=a[-1]; op=row[:,3]>0
        if not op.any(): a=a[:-1]; continue
        px=row[op]
        neutral=((np.abs(px[:,0]-px[:,1])<=14)&(np.abs(px[:,1]-px[:,2])<=14)&(px[:,:3].max(axis=1)<215)).mean()
        if neutral<0.8: break
        a=a[:-1]
    return Image.fromarray(a.astype(np.uint8),"RGBA")

# ---- first-person sheet: 6 guns left-to-right ----
cells = sorted(key_out(FP, 180), key=lambda t:t[1])
assert len(cells)==6, f"FP sheet: got {len(cells)}"
CW,CH = 72,56
sheet = Image.new("RGBA",(CW*6,CH),(0,0,0,0))
for i,(_,_,g) in enumerate(cells):
    # oversize ~1.35x and let the arm crop off the bottom — the GUN should dominate the cell
    sc=min((CH*1.35)/g.size[1], (CW-1)/g.size[0])
    w=max(1,round(g.size[0]*sc)); h=max(1,round(g.size[1]*sc))
    r=harden(g.resize((w,h),Image.LANCZOS))
    if h>CH: r=r.crop((0,0,w,CH)); h=CH
    sheet.paste(r,(i*CW+(CW-w)//2, CH-h))
sheet.save(f"{AD}/weapons2.png")
print("weapons2.png: 6 FP cells (knife smg dblshot flamer cannon plasma)")

# ---- icon sheet 3x3: row-major -> [fuel balls cell | knife smg shotgun | flamer cannon plasma]
items = key_out(IC, 120)
items.sort(key=lambda t:t[0])
rows,cur=[],[items[0]]
for it in items[1:]:
    if abs(it[0]-cur[0][0])<160: cur.append(it)
    else: rows.append(cur); cur=[it]
rows.append(cur)
flat=[]
for r in rows:
    r.sort(key=lambda t:t[1]); flat += [t[2] for t in r]
assert len(flat)==9, f"icon sheet: got {len(flat)}"
ammo, guns = flat[:3], flat[3:]

gs = Image.new("RGBA",(32*6,16),(0,0,0,0))
for i,g in enumerate(guns):
    g = strip_shadow(g)
    sc=min(30/g.size[0], 14/g.size[1])
    r=harden(g.resize((max(1,round(g.size[0]*sc)),max(1,round(g.size[1]*sc))),Image.LANCZOS))
    gs.paste(r,(i*32+(32-r.size[0])//2, 16-r.size[1]))
gs.save(f"{AD}/wpickup2.png")
print("wpickup2.png: 6 pickup profiles")

am = Image.new("RGBA",(20*3,20),(0,0,0,0))
for i,g in enumerate(ammo):
    g = strip_shadow(g)
    sc=18/max(g.size)
    r=harden(g.resize((max(1,round(g.size[0]*sc)),max(1,round(g.size[1]*sc))),Image.LANCZOS))
    am.paste(r,(i*20+(20-r.size[0])//2, 20-r.size[1]))
am.save(f"{AD}/ammo3.png")
print("ammo3.png: fuel / cannonballs / cell")

# ---- projectile orbs (radial glows; these are effects, not organic art) ----
def orb(name, inner, outer, S=12):
    hi=S*6
    im=Image.new("RGBA",(hi,hi),(0,0,0,0))
    px=np.zeros((hi,hi,4))
    yy,xx=np.mgrid[0:hi,0:hi]
    d=np.sqrt((xx-hi/2)**2+(yy-hi/2)**2)/(hi/2)
    t=np.clip(1-d,0,1)**1.4
    for c in range(3):
        px[:,:,c]=inner[c]*t + outer[c]*(1-t)
    px[:,:,3]=np.where(d<0.95,255,0)
    im=Image.fromarray(px.astype(np.uint8),"RGBA").resize((S,S),Image.LANCZOS)
    a=np.asarray(im).copy(); hard=a[:,:,3]>110
    a[:,:,3]=np.where(hard,255,0); a[~hard]=0
    Image.fromarray(a,"RGBA").save(f"{AD}/{name}.png")
def fire_orb(name, S=14):
    """A proper fireball: white-hot core, orange flame body, deep red rim, with
    radial flame licks (angular ripple) — reads as fire even at 12px."""
    hi=S*8
    px=np.zeros((hi,hi,4))
    yy,xx=np.mgrid[0:hi,0:hi]
    dx=(xx-hi/2)/(hi/2); dy=(yy-hi/2)/(hi/2)
    d=np.sqrt(dx*dx+dy*dy)
    ang=np.arctan2(dy,dx)
    edge=1.0+0.10*np.sin(ang*7)+0.06*np.sin(ang*13+1.7)   # licked rim
    t=np.clip(d/edge,0,1)
    core=(t<0.30); body=(t>=0.30)&(t<0.70); rim=(t>=0.70)&(t<1.0)
    px[core]=[255,250,205,255]
    bb=(t[body]-0.30)/0.40
    px[body,0]=255; px[body,1]=190-120*bb; px[body,2]=40-30*bb; px[body,3]=255
    rr=(t[rim]-0.70)/0.30
    px[rim,0]=225-90*rr; px[rim,1]=60-40*rr; px[rim,2]=8; px[rim,3]=255
    im=Image.fromarray(px.astype(np.uint8),"RGBA").resize((S,S),Image.LANCZOS)
    a=np.asarray(im).copy(); hard=a[:,:,3]>110
    a[:,:,3]=np.where(hard,255,0); a[~hard]=0
    Image.fromarray(a,"RGBA").save(f"{AD}/{name}.png")
fire_orb("fireball")
orb("plasmab",  (210,240,255), (30,80,220))
orb("cannonb",  (210,210,225), (60,60,72))
print("orbs baked")
