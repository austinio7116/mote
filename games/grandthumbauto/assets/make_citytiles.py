#!/usr/bin/env python3
"""Author the city ground tilesheets (EDITABLE SOURCES; `mote bake` turns them into headers).

Both sheets are 4x4 cells of 32 px (128x128), cell index = N + 2E + 4S + 8W where a SET
bit means the neighbour continues (open side) and a CLEAR bit gets the edge treatment.
Image top = north, right = east (matches road_uv / the roads.tileset lut).

  roads.png — asphalt with proper KERBS on closed sides: pale kerb stones with per-stone
              seams, a gutter shadow, and an oil-stain wear line down the open middle.
  water.png — city WATER with seawall shorelines: concrete cap + shadow + foam line on
              closed sides, subtle wave dashes in open water.
"""
import random
from PIL import Image, ImageDraw

T = 32
random.seed(1207)

def noise(px, x0, y0, w, h, base, amp):
    for y in range(y0, y0+h):
        for x in range(x0, x0+w):
            n = random.randint(-amp, amp)
            r, g, b = base
            px[x, y] = (max(0,min(255,r+n)), max(0,min(255,g+n)), max(0,min(255,b+n)), 255)

# ------------------------------------------------------------------ roads --
ASPHALT   = (88, 88, 98)
KERB      = (168, 166, 152)     # pale concrete kerb stones
KERB_DK   = (128, 126, 114)     # kerb seam
GUTTER    = (52, 52, 60)        # shadow line inside the kerb

def road_cell(im, cx, cy, k):
    d = ImageDraw.Draw(im)
    px = im.load()
    noise(px, cx, cy, T, T, ASPHALT, 3)                     # calmer asphalt
    # a few faint wear stains only (was speckly)
    for _ in range(6):
        x = cx + random.randint(4, T-5); y = cy + random.randint(4, T-5)
        c = px[x, y]; px[x, y] = (max(0,c[0]-8), max(0,c[1]-8), max(0,c[2]-6), 255)
    openN, openE, openS, openW = k&1, k&2, k&4, k&8
    # CLEAN kerb: solid pale band + single crisp shadow line; sparse subtle seams
    def kerb_h(inward):
        y0 = cy + (0 if not inward else T-2)
        d.rectangle([cx, y0, cx+T-1, y0+1], fill=KERB)
        for sx in range(cx+7, cx+T-1, 8):
            px[sx, y0] = KERB_DK; px[sx, y0+1] = KERB_DK    # 1-px seam notch
        gy = y0+2 if not inward else y0-1
        d.line([cx, gy, cx+T-1, gy], fill=GUTTER)
    def kerb_v(inward):
        x0 = cx + (0 if not inward else T-2)
        d.rectangle([x0, cy, x0+1, cy+T-1], fill=KERB)
        for sy in range(cy+7, cy+T-1, 8):
            px[x0, sy] = KERB_DK; px[x0+1, sy] = KERB_DK
        gx = x0+2 if not inward else x0-1
        d.line([gx, cy, gx, cy+T-1], fill=GUTTER)
    if not openN: kerb_h(False)
    if not openS: kerb_h(True)
    if not openW: kerb_v(False)
    if not openE: kerb_v(True)

# ------------------------------------------------------------------ water --
WATER     = (34, 84, 118)       # deep city river
WAVE      = (58, 118, 150)      # wave dash
SEAWALL   = (158, 156, 148)     # concrete embankment cap
SEAWALL_D = (112, 110, 104)     # cap seam / shadow
FOAM      = (150, 200, 214)     # lapping line against the wall

def water_cell(im, cx, cy, k):
    d = ImageDraw.Draw(im)
    px = im.load()
    noise(px, cx, cy, T, T, WATER, 4)
    for _ in range(10):                                     # wave dashes
        x = cx + random.randint(2, T-8); y = cy + random.randint(2, T-3)
        d.line([x, y, x+random.randint(3,5), y], fill=WAVE)
    openN, openE, openS, openW = k&1, k&2, k&4, k&8
    def wall_h(inward):              # seawall along N or S edge
        y0 = cy + (0 if not inward else T-4)
        d.rectangle([cx, y0, cx+T-1, y0+2], fill=SEAWALL)
        for sx in range(cx, cx+T, 10):
            d.line([sx, y0, sx, y0+2], fill=SEAWALL_D)
        sh = y0+3 if not inward else y0-1                   # waterline shadow
        d.line([cx, sh, cx+T-1, sh], fill=(20, 50, 74))
        fy = y0+4 if not inward else y0-2                   # foam lapping the wall
        for sx in range(cx+1, cx+T-2, 5):
            d.line([sx, fy, sx+2, fy], fill=FOAM)
    def wall_v(inward):
        x0 = cx + (0 if not inward else T-4)
        d.rectangle([x0, cy, x0+2, cy+T-1], fill=SEAWALL)
        for sy in range(cy, cy+T, 10):
            d.line([x0, sy, x0+2, sy], fill=SEAWALL_D)
        sh = x0+3 if not inward else x0-1
        d.line([sh, cy, sh, cy+T-1], fill=(20, 50, 74))
        fx = x0+4 if not inward else x0-2
        for sy in range(cy+1, cy+T-2, 5):
            d.line([fx, sy, fx, sy+2], fill=FOAM)
    if not openN: wall_h(False)
    if not openS: wall_h(True)
    if not openW: wall_v(False)
    if not openE: wall_v(True)

def build(maker, path):
    im = Image.new("RGBA", (T*4, T*4), (0,0,0,255))
    for k in range(16):
        maker(im, (k & 3)*T, (k >> 2)*T, k)
    im.save(path)
    print("wrote", path)

# ------------------------------------------------------------------ ground --
# 8 cells of 16 px (128x16): asphalt dash pave grass water cross edge dirt
# (matches game.c enum G_ASPHALT..G_DIRT; pave/grass/dirt are the live ones)
def build_ground(path):
    G = 16
    im = Image.new("RGBA", (G*8, G), (0,0,0,255)); px = im.load(); d = ImageDraw.Draw(im)
    def cell(i): return i*G
    noise(px, cell(0), 0, G, G, ASPHALT, 3)                            # asphalt
    noise(px, cell(1), 0, G, G, ASPHALT, 3)                            # dash
    d.rectangle([cell(1)+7, 2, cell(1)+8, 13], fill=(214,214,222,255))
    x0=cell(2); noise(px, x0, 0, G, G, (150,148,138), 3)               # PAVE: concrete slabs
    for s in (0, 8):                                                   # slab seams (2x2 slabs/tile)
        d.line([x0+s, 0, x0+s, G-1], fill=(122,120,112,255))
        d.line([x0, s, x0+G-1, s], fill=(122,120,112,255))
    d.line([x0+1, 1, x0+1, G-1], fill=(162,160,150,255))               # slab highlight
    x0=cell(3); noise(px, x0, 0, G, G, (86,124,52), 5)                 # GRASS: mottled green
    for _ in range(9):                                                 # blade tufts
        x=x0+random.randint(1,G-2); y=random.randint(1,G-2)
        px[x,y]=(104,146,62,255)
    for _ in range(5):
        x=x0+random.randint(1,G-3); y=random.randint(1,G-2)
        px[x,y]=(70,104,44,255); px[x+1,y]=(70,104,44,255)             # darker patches
    x0=cell(4); noise(px, x0, 0, G, G, WATER, 4)                       # water (legacy cell)
    x0=cell(5); noise(px, x0, 0, G, G, ASPHALT, 3)                     # cross (legacy)
    x0=cell(6); noise(px, x0, 0, G, G, (150,148,138), 3)               # edge (legacy)
    x0=cell(7); noise(px, x0, 0, G, G, (118,96,64), 6)                 # DIRT: earthy + pebbles
    for _ in range(6):
        x=x0+random.randint(1,G-2); y=random.randint(1,G-2)
        px[x,y]=(140,118,84,255)
    im.save(path); print("wrote", path)

ROOT = "/home/maustin/thumby-color/mote/games/grandthumbauto/assets"
build(road_cell,  f"{ROOT}/roads.png")
build(water_cell, f"{ROOT}/water.png")
build_ground(f"{ROOT}/ground.png")
