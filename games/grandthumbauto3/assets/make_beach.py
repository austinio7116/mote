#!/usr/bin/env python3
"""Author the BEACH art: the second ground-atlas row, and a wall-less water sheet.

Run from anywhere: ROOT is derived from this file's location, unlike
make_citytiles.py, whose ROOT is an absolute path on another machine and points
at games/grandthumbauto (not grandthumbauto3). Running that script here
overwrites this game's committed art with a different game's; do not.

Two outputs:

  ground.png      grown from 128x16 to 128x32. Row 0 is PASTED from the existing
                  file, unchanged, because it is committed art and re-running its
                  generator would not reproduce it byte for byte (the noise draws
                  from one shared random stream, so cell content depends on call
                  order). Row 1 cell 0 is dry sand, cell 1 is wet sand.
                  Cell index is row*8 + col; see ground_uv() in game.c.

  water_beach.png a 16-cell copy of the water autotile with NO seawall. The
                  concrete cap, its seam and its shadow are baked into the closed
                  sides of every cell in water.png, so a beach drawn against the
                  normal sheet is a walled sand strip. Here a closed side shelves
                  out through wet shallows into a foam line instead.
                  Same cell layout, so it reuses water_at.lut.
"""
import os, random
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.abspath(__file__))
T = 32
random.seed(4471)

WATER = (34, 84, 118)
WAVE  = (58, 118, 150)
FOAM  = (150, 200, 214)
SAND      = (206, 190, 150)
SAND_DK   = (178, 160, 122)
SAND_WET  = (150, 132, 100)
SHALLOW   = (86, 148, 164)      # water over sand, right at the shore
SHALLOW_2 = (60, 118, 146)      # a little deeper

def noise(px, x0, y0, w, h, base, amp):
    for y in range(y0, y0+h):
        for x in range(x0, x0+w):
            n = random.randint(-amp, amp)
            r, g, b = base
            px[x, y] = (max(0,min(255,r+n)), max(0,min(255,g+n)), max(0,min(255,b+n)), 255)

# ------------------------------------------------------------ water_beach --
def beach_cell(im, cx, cy, k):
    d = ImageDraw.Draw(im)
    px = im.load()
    noise(px, cx, cy, T, T, WATER, 4)
    for _ in range(10):
        x = cx + random.randint(2, T-8); y = cy + random.randint(2, T-3)
        d.line([x, y, x+random.randint(3,5), y], fill=WAVE)
    openN, openE, openS, openW = k&1, k&2, k&4, k&8
    # A closed side is SHORE: bands running from the land edge inward -- wet
    # sand, then shallows lightening over the sandy bottom, then a broken foam
    # line where the water starts to read as water. 8 px of a 32 px cell is 1 m
    # of a 4 m tile, which is what a shelving shore looks like from the chase
    # camera; 1 px bands just read as stripes.
    BANDS = [(0, 2, SAND_WET), (2, 5, SHALLOW), (5, 7, SHALLOW_2)]
    def shore_h(inward):
        for a, b, col in BANDS:
            for off in range(a, b):
                y = cy + (off if not inward else T-1-off)
                d.line([cx, y, cx+T-1, y], fill=col)
        fy = cy + (7 if not inward else T-8)
        for sx in range(cx+1, cx+T-2, 7):
            d.line([sx, fy, sx+random.randint(1,3), fy], fill=FOAM)
    def shore_v(inward):
        for a, b, col in BANDS:
            for off in range(a, b):
                x = cx + (off if not inward else T-1-off)
                d.line([x, cy, x, cy+T-1], fill=col)
        fx = cx + (7 if not inward else T-8)
        for sy in range(cy+1, cy+T-2, 7):
            d.line([fx, sy, fx, sy+random.randint(1,3)], fill=FOAM)
    if not openN: shore_h(False)
    if not openS: shore_h(True)
    if not openW: shore_v(False)
    if not openE: shore_v(True)

def build_water_beach(path):
    im = Image.new("RGBA", (T*4, T*4), (0,0,0,255))
    for k in range(16):
        beach_cell(im, (k & 3)*T, (k >> 2)*T, k)
    im.save(path); print("wrote", path)

# ----------------------------------------------------------------- ground --
def grow_ground(path):
    G = 16
    old = Image.open(path).convert("RGBA")
    if old.size == (G*8, G*2):
        print("ground.png already two rows; rewriting row 1 only")
        old = old.crop((0, 0, G*8, G))
    assert old.size == (G*8, G), "unexpected ground.png size %r" % (old.size,)
    im = Image.new("RGBA", (G*8, G*2), (0,0,0,255))
    im.paste(old, (0, 0))
    px = im.load(); d = ImageDraw.Draw(im)
    # cell 8 = row 1, col 0: DRY sand
    x0, y0 = 0, G
    noise(px, x0, y0, G, G, SAND, 6)
    for _ in range(7):                                  # shell grit / darker pebbles
        x = x0+random.randint(1,G-2); y = y0+random.randint(1,G-2)
        px[x, y] = SAND_DK + (255,)
    for _ in range(3):                                  # wind ripples
        y = y0+random.randint(2,G-3)
        d.line([x0+random.randint(0,4), y, x0+G-1-random.randint(0,4), y], fill=SAND_DK+(255,))
    # cell 9 = row 1, col 1: WET sand (the tile touching the water)
    x0 = G
    noise(px, x0, y0, G, G, SAND_WET, 5)
    for _ in range(5):
        x = x0+random.randint(1,G-2); y = y0+random.randint(1,G-2)
        px[x, y] = (128, 112, 86, 255)
    im.save(path); print("wrote", path)

build_water_beach(f"{ROOT}/water_beach.png")
grow_ground(f"{ROOT}/ground.png")
