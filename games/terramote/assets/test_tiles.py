#!/usr/bin/env python3
"""Tileset RULE HARNESS — renders a tileset against scripted placement
patterns EXACTLY the way the engine does (raw 8-bit neighbour mask -> lut ->
sheet cell), so rules + art are verified offline, zoomed, cell by cell,
before anything ships. Run:  python3 assets/test_tiles.py tiles_roof

Output: assets/_test_<name>.png — one labelled contact sheet, 8x zoom,
sky-blue backdrop so transparent (cut) pixels are obvious.
"""
import os, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

# neighbour bit order — MUST match make_tiles.py and the engine (fx.c nb[])
N, NE, E, SE, S, SW, W, NW = 1, 2, 4, 8, 16, 32, 64, 128
OFFS = { N: (0, -1), NE: (1, -1), E: (1, 0), SE: (1, 1),
         S: (0, 1), SW: (-1, 1), W: (-1, 0), NW: (-1, -1) }

def load_tileset(name):
    ts = {}
    for line in open(os.path.join(GAME, "tilesets", name + ".tileset")):
        k, _, v = line.strip().partition(" ")
        ts[k] = v
    sheet = Image.open(os.path.join(GAME, ts["sheet"])).convert("RGBA")
    lut = [int(x) for x in ts["lut"].split()]
    assert len(lut) == 256, "lut must be the full 256 raw-mask table"
    return sheet, lut, int(ts["tile"]), int(ts["cols"])

PATTERNS = [
    ("solid 3x3", ["###",
                   "###",
                   "###"]),
    ("flat run", ["#####"]),
    ("lone", ["#"]),
    ("stair / 1-thick", ["....#",
                         "...#.",
                         "..#..",
                         ".#...",
                         "#...."]),
    ("stair \\ 1-thick", ["#....",
                          ".#...",
                          "..#..",
                          "...#.",
                          "....#"]),
    ("stair / 2-thick", ["....##",
                         "...##.",
                         "..##..",
                         ".##...",
                         "##...."]),
    ("peak (roof)", ["...##...",
                     "..####..",
                     ".##..##.",
                     "##....##"]),
    ("peak filled", ["...##...",
                     "..####..",
                     ".######.",
                     "########"]),
    ("overhang underside", ["########",
                            "..######",
                            "....####",
                            "......##"]),
    ("gable steps", ["...#...",
                     "..###..",
                     ".#####.",
                     "#######"]),
    ("pointed peak hollow", ["...#...",
                             "..#.#..",
                             ".#...#.",
                             "#.....#"]),
    ("diamond", [".#.",
                 "#.#",
                 ".#."]),
    ("aframe eaves", ["....##....",
                      "...####...",
                      "..##..##..",
                      ".##....##.",
                      "##......##"]),
    ("pavilion", ["...####...",
                  "..######..",
                  ".##....##.",
                  "##########"]),
]

def mask_at(grid, x, y):
    m = 0
    for bit, (dx, dy) in OFFS.items():
        xx, yy = x + dx, y + dy
        if 0 <= yy < len(grid) and 0 <= xx < len(grid[yy]) and grid[yy][xx] == '#':
            m |= bit
    return m

def render(name, zoom=8):
    sheet, lut, tile, cols = load_tileset(name)
    SKY = (100, 160, 240, 255)
    pad = 6
    imgs = []
    for label, grid in PATTERNS:
        gw, gh = max(len(r) for r in grid), len(grid)
        im = Image.new("RGBA", (gw * tile, gh * tile), SKY)
        for y, row in enumerate(grid):
            for x, ch in enumerate(row):
                if ch != '#': continue
                cell = lut[mask_at(grid, x, y)]
                sx, sy = (cell % cols) * tile, (cell // cols) * tile
                piece = sheet.crop((sx, sy, sx + tile, sy + tile))
                im.alpha_composite(piece, (x * tile, y * tile))
        im = im.resize((im.width * zoom, im.height * zoom), Image.NEAREST)
        cap = Image.new("RGBA", (im.width, im.height + 14), (24, 22, 30, 255))
        cap.alpha_composite(im, (0, 14))
        ImageDraw.Draw(cap).text((2, 2), label, fill=(255, 230, 160, 255))
        imgs.append(cap)
    w = max(i.width for i in imgs) + pad * 2
    h = sum(i.height + pad for i in imgs) + pad
    out = Image.new("RGBA", (w, h), (24, 22, 30, 255))
    y = pad
    for i in imgs:
        out.alpha_composite(i, (pad, y))
        y += i.height + pad
    path = os.path.join(HERE, "_test_%s.png" % name)
    out.convert("RGB").save(path)
    print("wrote", path, out.size)

def dump(name, pattern_label):
    """Print each cell's mask + chosen sheet cell for one pattern."""
    _, lut, _, _ = load_tileset(name)
    NAMES = [("N",N),("NE",NE),("E",E),("SE",SE),("S",S),("SW",SW),("W",W),("NW",NW)]
    for label, grid in PATTERNS:
        if label != pattern_label: continue
        for y, row in enumerate(grid):
            for x, ch in enumerate(row):
                if ch != '#': continue
                m = mask_at(grid, x, y)
                bits = "+".join(nm for nm,b in NAMES if m & b) or "-"
                print(f"({x},{y}) mask[{bits}] -> cell {lut[m]}")

if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[2].startswith("dump="):
        dump(sys.argv[1], sys.argv[2][5:])
    else:
        render(sys.argv[1] if len(sys.argv) > 1 else "tiles_roof")
