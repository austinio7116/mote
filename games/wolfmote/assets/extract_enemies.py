#!/usr/bin/env python3
"""Extract the 5 supplied enemy sheets -> guard/rusher/commando/brute/boss.png.
Each source has 4 poses (idle / fire / pain / dead) on a baked-in checkerboard.
Components are clustered into the 4 frames by x-gap (keeps detached blood spray
with its pose), standing frames share one scale, corpses fit by width, and
everything bottom-anchors so feet sit on the floor.
Cells match the game: soldiers 24x40 (96x40 sheets), heavies 28x44 (112x44)."""
import glob, os
import numpy as np
from PIL import Image
from scipy import ndimage
from collections import Counter

SRC = sorted(glob.glob("/tmp/ChatGPT Image Jul 3, 2026, 08_02_1*.png"), key=os.path.getmtime)
assert len(SRC) == 5, f"expected 5 sheets, got {len(SRC)}"
#          file                 out            cell
PLAN = [ (SRC[0], "commando.png", (24,40)),   # green soldier
         (SRC[1], "guard.png",    (24,40)),   # blue
         (SRC[2], "rusher.png",   (24,40)),   # red
         (SRC[3], "boss.png",     (28,44)),   # green heavy
         (SRC[4], "brute.png",    (28,44)) ]  # tan heavy
AD = "/home/maustin/thumby-color/mote/games/wolfmote/assets"

for path, out, (CW, CH) in PLAN:
    im = np.asarray(Image.open(path).convert("RGB")).astype(int)
    border = np.concatenate([im[:20].reshape(-1,3), im[-20:].reshape(-1,3),
                             im[:, :20].reshape(-1,3), im[:, -20:].reshape(-1,3)])
    (c1,_),(c2,_) = Counter(map(tuple,border)).most_common(2)
    def near(px,c,tol=18): return (np.abs(px-np.array(c)).sum(axis=2))<tol
    fg = ~(near(im,c1)|near(im,c2))
    fg = ndimage.binary_opening(fg, structure=np.ones((3,3)))
    lab,_ = ndimage.label(fg)
    comps=[]
    for sl in ndimage.find_objects(lab):
        h,w = sl[0].stop-sl[0].start, sl[1].stop-sl[1].start
        if h<12 or w<12: continue
        comps.append([sl[1].start, sl[1].stop, sl[0].start, sl[0].stop])
    comps.sort(key=lambda c:c[0])
    # merge overlapping bboxes first, then split at the 3 LARGEST gaps -> exactly 4 poses
    merged=[comps[0][:]]
    for c in comps[1:]:
        if c[0] <= merged[-1][1]:
            cl=merged[-1]
            cl[1]=max(cl[1],c[1]); cl[2]=min(cl[2],c[2]); cl[3]=max(cl[3],c[3]); cl[0]=min(cl[0],c[0])
        else: merged.append(c[:])
    assert len(merged)>=4, f"{out}: only {len(merged)} groups"
    gaps = sorted(range(1,len(merged)), key=lambda i: merged[i][0]-merged[i-1][1], reverse=True)[:3]
    cuts = sorted(gaps)
    clusters=[]; start=0
    for cut in cuts+[len(merged)]:
        grp = merged[start:cut]; start=cut
        cl = grp[0][:]
        for c in grp[1:]:
            cl[0]=min(cl[0],c[0]); cl[1]=max(cl[1],c[1]); cl[2]=min(cl[2],c[2]); cl[3]=max(cl[3],c[3])
        clusters.append(cl)
    assert len(clusters)==4, f"{out}: expected 4 poses, got {len(clusters)}"
    frames=[]
    for x0,x1,y0,y1 in clusters:
        m = ndimage.binary_fill_holes(lab[y0:y1, x0:x1]>0)
        frames.append(Image.fromarray(np.dstack([im[y0:y1,x0:x1], np.where(m,255,0)]).astype(np.uint8),"RGBA"))
    stand_scale = (CH-1)/max(f.size[1] for f in frames[:3])   # one scale for the standing poses
    sheet = Image.new("RGBA",(CW*4,CH),(0,0,0,0))
    for i,f in enumerate(frames):
        sc = stand_scale if i<3 else min((CW-1)/f.size[0], (CH-1)/f.size[1])
        w = min(CW, max(1, round(f.size[0]*sc))); h = min(CH, max(1, round(f.size[1]*sc)))
        r = f.resize((w,h), Image.LANCZOS)
        a = np.asarray(r).copy(); hard=a[:,:,3]>110
        a[:,:,3]=np.where(hard,255,0); a[~hard]=0
        r = Image.fromarray(a,"RGBA")
        sheet.paste(r, (i*CW+(CW-w)//2, CH-h))               # feet on the floor
    sheet.save(f"{AD}/{out}")
    print(f"{out}: 4 poses in {CW}x{CH} cells")
print("done")
