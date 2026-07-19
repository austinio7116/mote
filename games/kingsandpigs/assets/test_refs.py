#!/usr/bin/env python3
"""Golden test: recreate mini versions of the pack's example screens with our
baked tilesets + blob rules (assets/solidt.png + tilesets/solidt.tileset etc.),
mirroring the engine's layered autotile exactly (mask -> lut -> xform blit).

Rooms are authored as bg shapes ('.'); the 1-tile wall ribbon around them is
derived automatically (any empty cell 8-adjacent to bg), exactly like the
game's generator. Output: side-by-side renders for eyeballing against
source screenshots of https://pixelfrog-assets.itch.io/kings-and-pigs.

Usage: python3 assets/test_refs.py <out.png>
"""
import os
import sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

NB = [(0, -1, 1), (1, -1, 2), (1, 0, 4), (1, 1, 8),
      (0, 1, 16), (-1, 1, 32), (-1, 0, 64), (-1, -1, 128)]

def load_tileset(name):
    conf = {"lut": [0] * 256, "xform": [0] * 256, "edge": 0, "sheet": None}
    for line in open(os.path.join(GAME, "tilesets", name + ".tileset")):
        t = line.split()
        if not t: continue
        if t[0] == "sheet": conf["sheet"] = Image.open(os.path.join(GAME, t[1])).convert("RGBA")
        elif t[0] == "edge": conf["edge"] = int(t[1])
        elif t[0] == "lut": conf["lut"] = [int(x) for x in t[1:]]
        elif t[0] == "xform": conf["xform"] = [int(x) for x in t[1:]]
    return conf

def xform_tile(im, xf):
    rot = (xf >> 2) & 3
    if rot == 1: im = im.transpose(Image.ROTATE_270)   # PIL rotates CCW; engine 90 CW
    elif rot == 2: im = im.transpose(Image.ROTATE_180)
    elif rot == 3: im = im.transpose(Image.ROTATE_90)
    if xf & 1: im = im.transpose(Image.FLIP_LEFT_RIGHT)
    if xf & 2: im = im.transpose(Image.FLIP_TOP_BOTTOM)
    return im

def render(rows, tilesets, bits):
    R, C = len(rows), max(len(r) for r in rows)
    grid = [[0] * C for _ in range(R)]
    for r, line in enumerate(rows):
        for c, ch in enumerate(line):
            if ch == '.': grid[r][c] |= bits['bg']
            elif ch == '#': grid[r][c] |= bits['sol']
    # rooms are holes carved in an infinite solid — every non-room cell is wall
    # (matches the pack's example screens: dark fill + scatter beyond the trim)
    for r in range(R):
        for c in range(C):
            if not (grid[r][c] & bits['bg']):
                grid[r][c] |= bits['sol']
    out = Image.new("RGBA", (C * 16, R * 16), (57, 49, 75, 255))   # the pack's void colour
    for layer, bit in (("bgwall", bits['bg']), ("solidt", bits['sol'])):
        ts = tilesets[layer]
        tpr = ts["sheet"].size[0] // 16
        for r in range(R):
            for c in range(C):
                if not (grid[r][c] & bit): continue
                m = 0
                for dx, dy, b in NB:
                    rr, cc = r + dy, c + dx
                    same = ts["edge"] if (rr < 0 or rr >= R or cc < 0 or cc >= C) \
                        else (grid[rr][cc] & bit) != 0
                    if same: m |= b
                cell = ts["lut"][m]
                fx, fy = (cell % tpr) * 16, (cell // tpr) * 16
                tile = xform_tile(ts["sheet"].crop((fx, fy, fx + 16, fy + 16)), ts["xform"][m])
                out.alpha_composite(tile, (c * 16, r * 16))
    return out

# room shapes ('.' = interior), ribbons derived — mimicking ref1 / ref2 / ref3
REF1 = [       # S/Z-shaped hall (ref1)
    "                  ",
    "  ............    ",
    "  ............    ",
    "  ............    ",
    "  .....census.    ".replace("census", "......"),
    "  ............... ",
    "       .......... ",
    "  ..............  ",
    "  ..............  ",
    "                  ",
]
REF2 = [       # L-shaped room (ref2)
    "                ",
    "  ...........   ",
    "  ...........   ",
    "  ...........   ",
    "  ...........   ",
    "  ......        ",
    "  ......        ",
    "  ......        ",
    "                ",
]
REF3 = [       # room with a notched corner + side alcove (ref3)
    "                 ",
    "  ............   ",
    "  ............   ",
    "  ............   ",
    "     .........   ",
    "     .........   ",
    "     .........   ",
    "                 ",
]

def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/kp_refs_test.png"
    tilesets = {n: load_tileset(n) for n in ("bgwall", "solidt")}
    bits = {"bg": 1, "sol": 8}
    imgs = [render(m, tilesets, bits) for m in (REF1, REF2, REF3)]
    W = max(i.size[0] for i in imgs)
    H = sum(i.size[1] + 8 for i in imgs)
    out = Image.new("RGBA", (W, H), (20, 18, 28, 255))
    y = 0
    for i in imgs:
        out.alpha_composite(i, (0, y)); y += i.size[1] + 8
    s = 3
    out = out.resize((out.size[0] * s, out.size[1] * s), Image.NEAREST)
    out.convert("RGB").save(out_path)
    print("wrote", out_path)

if __name__ == "__main__":
    main()
