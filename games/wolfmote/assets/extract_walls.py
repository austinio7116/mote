#!/usr/bin/env python3
"""Extract the 8-tile wall texture set (user-supplied, 4x2 grid on dark gutters)
into ONE sprite sheet walls.png (128x256 = 2 cols x 4 rows of 64x64 cells):
  0 brick   1 stone
  2 crack   3 moss
  4 metal   5 door (steel)
  6 doorw (wood)   7 exit (green arrow)
The 5 tiling wall faces are made HORIZONTALLY SEAMLESS: roll 32px so the old
cut edges (finished brick bevels) meet mid-texture, then heal that centre seam
with a feathered strip of real content from the best-matching donor spot —
bricks now continue across adjacent blocks instead of ending at every cube."""
import numpy as np
from PIL import Image
from scipy import ndimage
from collections import Counter

SRC = "/home/maustin/.claude/uploads/28ee9ae3-8030-4cdc-af99-20078e7d27c0/9333b007-file_00000000c86871f4b6b5d0580ebefb1f.png"
AD  = "/home/maustin/thumby-color/mote/games/wolfmote/assets"
# source-grid order (row-major in the upload)
NAMES = ["exit","stone","brick","door","crack","moss","metal","doorw"]
# sheet order = game cell index
SHEET = ["brick","stone","crack","moss","metal","door","doorw","exit"]
TILING = {"brick","stone","crack","moss","metal"}   # doors stand alone

im = np.asarray(Image.open(SRC).convert("RGB")).astype(int)
border = np.concatenate([im[:12].reshape(-1,3), im[-12:].reshape(-1,3),
                         im[:, :12].reshape(-1,3), im[:, -12:].reshape(-1,3)])
(bg,_), = Counter(map(tuple,border)).most_common(1)
mask = (np.abs(im - np.array(bg)).sum(axis=2)) > 24
mask = ndimage.binary_opening(mask, structure=np.ones((5,5)))
lab,_ = ndimage.label(mask)
tiles=[]
for sl in ndimage.find_objects(lab):
    h,w = sl[0].stop-sl[0].start, sl[1].stop-sl[1].start
    if h<200 or w<200: continue
    tiles.append((sl[0].start//200, sl[1].start, im[sl]))
tiles.sort(key=lambda t:(t[0], t[1]))
assert len(tiles)==8, f"expected 8 wall tiles, got {len(tiles)}"
tex = {}
for (rb, xs, px), name in zip(tiles, NAMES):
    tex[name] = np.asarray(Image.fromarray(px.astype(np.uint8),"RGB")
                           .resize((64,64), Image.LANCZOS)).astype(float)

def heal_wrap(a):
    """Make `a` (64x64x3) tile horizontally: roll half a tile so the old edges
    meet at x=32 (the new outer edges are genuinely continuous content), then
    paste the best-matching 16px donor strip over the centre seam, feathered."""
    a = np.roll(a, 32, axis=1)
    P0, P1, F = 24, 40, 4            # paste zone cols [24,40), 4px feather each side
    best, bd = None, 1e18
    for d in list(range(2, P0-16)) + list(range(P1+1, 64-17)):
        strip = a[:, d:d+16]
        err = (np.abs(strip[:, :F] - a[:, P0:P0+F]).sum()
             + np.abs(strip[:, -F:] - a[:, P1-F:P1]).sum())
        if err < bd: bd, best = err, strip.copy()
    w = np.ones(16); w[:F] = (np.arange(F)+1)/(F+1); w[-F:] = w[:F][::-1]
    a[:, P0:P1] = a[:, P0:P1]*(1-w[None,:,None]) + best*w[None,:,None]
    return a

sheet = np.zeros((256,128,3))
for i,name in enumerate(SHEET):
    t = tex[name]
    if name in TILING: t = heal_wrap(t)
    if name == "doorw":                                   # lighter wood: lift the midtones so
        t = 255.0*np.power(np.clip(t,0,255)/255.0, 0.62)  # planks read clearly; iron bands stay dark
        t[:,:,0] *= 1.06; t[:,:,1] *= 1.00; t[:,:,2] *= 0.90   # warm it up

    cy, cx = i//2, i%2
    sheet[cy*64:(cy+1)*64, cx*64:(cx+1)*64] = t
Image.fromarray(np.clip(sheet,0,255).astype(np.uint8),"RGB").convert("RGBA").save(f"{AD}/walls.png")
print("wrote walls.png (128x256, cells: " + " ".join(f"{i}={n}" for i,n in enumerate(SHEET)) + ")")
