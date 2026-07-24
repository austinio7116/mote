#!/usr/bin/env python3
"""Render a .tileset ruleset over a test terrain map, replicating the engine's
autotile raster (engine/render/mote_2d.c + sdk/mote_tile.h) so we can verify a
ruleset before any C build. Usage: preview_autotile.py <name> [<name> ...]"""
import os, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
NB = dict(N=1<<0, NE=1<<1, E=1<<2, SE=1<<3, S=1<<4, SW=1<<5, W=1<<6, NW=1<<7)

def load_tileset(name):
    conf = {"tile": 8, "edge": 0, "nvar": 1, "sheet": ""}
    lut = [0]*256
    for line in open(os.path.join(GAME, "tilesets", name + ".tileset")):
        t = line.split()
        if not t: continue
        if t[0] == "sheet": conf["sheet"] = t[1]
        elif t[0] in ("tile", "edge", "nvar"): conf[t[0]] = int(t[1])
        elif t[0] == "lut": lut = [int(x) for x in t[1:257]]
    sheet = Image.open(os.path.join(GAME, conf["sheet"])).convert("RGBA")
    return conf, lut, sheet

def mask(terr, cols, rows, c, r, edge):
    m = 0
    for (dx, dy, bit) in [(0,-1,'N'),(1,-1,'NE'),(1,0,'E'),(1,1,'SE'),
                          (0,1,'S'),(-1,1,'SW'),(-1,0,'W'),(-1,-1,'NW')]:
        cc, rr = c+dx, r+dy
        if cc < 0 or cc >= cols or rr < 0 or rr >= rows:
            same = edge
        else:
            same = terr[rr*cols+cc]
        if same: m |= NB[bit]
    return m

def at_hash(x, y):
    h = (x*73856093) ^ (y*19349663); h &= 0xffffffff
    h ^= h >> 13; return (h*1274126177) & 0xffffffff

def variant(nvar, c, r):
    if nvar <= 1: return 0
    return at_hash(c, r) % nvar

# test map: solid rect, L-shape, corridor, hole, single tile, diagonal touch
MAP = [
    "................",
    ".#####...##.....",
    ".#####...##..#..",
    ".#####...##.....",
    ".###.........###",
    ".###.....#####.#",
    ".###.....#...#.#",
    ".........#.#.#.#",
    "..####...#...#.#",
    "..#..#...#####.#",
    "..#..#.........#",
    "..####......####",
]

def render(name):
    conf, lut, sheet = load_tileset(name)
    ts, nvar, edge = conf["tile"], conf["nvar"], conf["edge"]
    tpr = sheet.width // ts
    base_rows = (sheet.height // ts) // max(1, nvar)
    rows = len(MAP); cols = len(MAP[0])
    terr = [1 if ch == '#' else 0 for row in MAP for ch in row]
    out = Image.new("RGBA", (cols*ts, rows*ts), (40, 40, 48, 255))
    for r in range(rows):
        for c in range(cols):
            if not terr[r*cols+c]: continue
            m = mask(terr, cols, rows, c, r, edge)
            cell = lut[m]
            fx = (cell % tpr)*ts; fy = (cell // tpr)*ts
            fy += variant(nvar, c, r)*base_rows*ts
            out.alpha_composite(sheet.crop((fx, fy, fx+ts, fy+ts)), (c*ts, r*ts))
    return out

if __name__ == "__main__":
    names = sys.argv[1:] or ["wall_brick", "hedge", "floor_cobble", "floor_grass", "water"]
    S = 10; pad = 12
    imgs = [(n, render(n)) for n in names]
    w = max(i.width for _, i in imgs)*S + pad*2
    h = sum(i.height*S + pad + 16 for _, i in imgs) + pad
    from PIL import ImageDraw
    canvas = Image.new("RGB", (w, h), (25, 25, 30)); d = ImageDraw.Draw(canvas)
    y = pad
    for n, im in imgs:
        d.text((pad, y), n, fill=(255, 255, 120)); y += 16
        canvas.paste(im.resize((im.width*S, im.height*S), Image.NEAREST).convert("RGB"), (pad, y))
        y += im.height*S + pad
    canvas.save("/tmp/preview_terrain.png"); print("saved /tmp/preview_terrain.png", canvas.size)
