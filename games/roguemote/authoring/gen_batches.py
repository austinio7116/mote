#!/usr/bin/env python3
"""
Emit high-zoom, coordinate-labelled montages (one per section) + a cell manifest,
for identification agents to read. Sprites sit on a checkerboard; each cell is
labelled with its source (col,row). Output: authoring/batches/
"""
import os, json
from PIL import Image, ImageDraw
import gen_catalogue as gc

HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.join(HERE, "batches"); os.makedirs(OUT, exist_ok=True)
SRC  = gc.SRC; IDX = gc.IDX; PAL = gc.PAL; TS = 8
S    = 13   # px per source pixel

def sid_of(title):
    return (title.split(" (")[0].lower().replace(" ", "-").replace("·","")
            .replace("/","").replace("--","-").strip("-"))

def montage(c0, r0, c1, r1, title):
    cols, rows = c1-c0+1, r1-r0+1
    W, H = cols*TS*S, rows*TS*S
    im = Image.new("RGB", (W, H))
    px = im.load()
    cells = []
    # checkerboard + sprite pixels
    for ty in range(rows):
        for tx in range(cols):
            c, r = c0+tx, r0+ty
            ink = False
            for yy in range(TS):
                for xx in range(TS):
                    ix = IDX[c*TS+xx, r*TS+yy]
                    if ix == 0:
                        chk = 150 if ((xx//2 + yy//2) % 2 == 0) else 120
                        col = (chk, chk, chk)
                    else:
                        col = (PAL[ix*3], PAL[ix*3+1], PAL[ix*3+2]); ink = True
                    x0, y0 = (tx*TS+xx)*S, (ty*TS+yy)*S
                    for dy in range(S):
                        for dx in range(S):
                            px[x0+dx, y0+dy] = col
            if ink: cells.append([c, r])
    d = ImageDraw.Draw(im)
    for gx in range(0, cols+1):
        d.line([(gx*TS*S, 0), (gx*TS*S, H)], fill=(220, 40, 40), width=1)
    for gy in range(0, rows+1):
        d.line([(0, gy*TS*S), (W, gy*TS*S)], fill=(220, 40, 40), width=1)
    for ty in range(rows):
        for tx in range(cols):
            c, r = c0+tx, r0+ty
            lbl = f"{c},{r}"
            x, y = tx*TS*S+2, ty*TS*S+1
            for ox, oy in [(-1,0),(1,0),(0,-1),(0,1)]:
                d.text((x+ox, y+oy), lbl, fill=(0, 0, 0))
            d.text((x, y), lbl, fill=(255, 235, 60))
    return im, cells

# batches: group related/small sections so each agent has a coherent, sized job
BATCHES = {
 "containers_furniture": ["Chests","Stone furniture","Doors · gems · banners","White furniture (top-down)"],
 "keys_runes_symbols":   ["Faces · skulls · keys","Runes","Symbols"],
 "props_structures":     ["Light props & structures"],
 "trinkets_nature":      ["Trinkets & small nature"],
 "weapons":              ["Weapons & potions","Elemental weapons","Guns"],
 "food":                 ["Food"],
 "treasure":             ["Treasure & ore","Loot furniture & bones","Crowns · armour · FX"],
 "tools":                ["Tools & wands"],
 "characters":           ["Large portrait","Characters"],
 "animals":              ["Animals & vermin"],
 "monsters":             ["Monsters"],
 "bosses":               ["Bosses"],
 "ui_a":                 ["Tiny UI icons (magenta strip)","Arrows & gauges","Button prompts"],
 "ui_b":                 ["Status · emotes · elements","Blueprint tiles"],
 "terrain_a":            ["Boulders & mountains","Colour panels (terrain)","Purple brick wall (terrain)",
                          "Stone-brick wall set (terrain)"],
 "terrain_b":            ["Temple / aztec wall (terrain)","Jungle grass & steps (terrain)",
                          "Cobblestone floors (terrain)","Terrain edges (terrain)"],
}
SEC = {t.split(" (")[0]: (c0,r0,c1,r1,t,b) for (c0,r0,c1,r1,t,b,u,l) in gc.SECTIONS}
# also key by full title
SECFULL = {t: (c0,r0,c1,r1,t,b) for (c0,r0,c1,r1,t,b,u,l) in gc.SECTIONS}

def find(title):
    if title in SECFULL: return SECFULL[title]
    k = title.split(" (")[0]
    return SEC.get(k)

def build():
    index = {}
    for batch, titles in BATCHES.items():
        parts = []
        for title in titles:
            s = find(title)
            if not s: print("!! missing", title); continue
            c0,r0,c1,r1,t,b = s
            im, cells = montage(c0,r0,c1,r1,t)
            sid = sid_of(t)
            fn = f"{batch}__{sid}.png"
            im.save(os.path.join(OUT, fn))
            parts.append({"title": t, "blurb": b, "png": fn,
                          "rect": [c0,r0,c1,r1], "cells": cells})
        index[batch] = parts
        n = sum(len(p["cells"]) for p in parts)
        print(f"[batch] {batch:22s} {len(parts)} sheets, {n} cells")
    json.dump(index, open(os.path.join(OUT, "index.json"), "w"), indent=1)
    print("wrote", os.path.join(OUT, "index.json"))

if __name__ == "__main__":
    build()
