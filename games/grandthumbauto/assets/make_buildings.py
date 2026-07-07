#!/usr/bin/env python3
"""Procedural building facade|roof atlases for Grand Thumb Auto.

Each atlas is 64x32: LEFT 32x32 = facade (wall), RIGHT 32x32 = roof (seen
top-down, so it carries the most visible detail — gravel, AC units, water
tanks, skylights, vents, parapet). 8 variants give the city its texture
variety; the game picks one per building from a stable hash.

Deterministic (fixed per-variant seeds) so a given city always looks the same.
Run:  python3 make_buildings.py   then  python3 ../../../tools/mote bake games/grandthumbauto
"""
import random
from PIL import Image

W = H = 32   # per-cell size; atlas is 2*W x H

def clamp(v): return max(0, min(255, int(v)))
def mix(a, b, t): return tuple(clamp(a[i]+(b[i]-a[i])*t) for i in range(3))
def shade(c, f): return tuple(clamp(c[i]*f) for i in range(3))

def facade(px, wall, mortar, win_lit, win_dark, glass, style, rnd,
           floors, cols, storefront=False):
    """Draw a wall (window grid) into px[0..31][0..31]."""
    for y in range(H):
        for x in range(W):
            px[x][y] = wall
    fh = H / floors
    cw = W / cols
    for fy in range(floors):
        base = int(fy*fh); top = int((fy+1)*fh)-1
        # ground-floor storefront band: dark glass with an awning line
        ground = storefront and fy == floors-1
        for cx in range(cols):
            x0 = int(cx*cw)+1; x1 = int((cx+1)*cw)-1
            y0 = base+1; y1 = top-1
            if x1 <= x0 or y1 <= y0: continue
            if ground:
                gcol = shade(glass, 0.65)
            else:
                # some windows lit, most dark glass; glassy styles = big panes
                lit = rnd.random() < 0.22
                gcol = win_lit if lit else (glass if style in ('glass','tower','office') else win_dark)
            for x in range(x0, x1+1):
                for y in range(y0, y1+1):
                    c = gcol
                    if style == 'glass' or style == 'tower':
                        # subtle vertical reflection gradient on the pane
                        c = mix(gcol, shade(gcol, 1.4), (y-y0)/max(1,(y1-y0)))
                    # mullion speckle
                    if rnd.random() < 0.04: c = mortar
                    px[x][y] = c
            # window sill / lintel highlight
            for x in range(x0, x1+1):
                px[x][y0] = mix(px[x][y0], wall, 0.5)
        # floor separator line (spandrel)
        if top < H:
            for x in range(W):
                px[x][top] = mortar
    if storefront:
        # awning stripe above ground floor
        ay = int((floors-1)*fh)-1
        if 0 <= ay < H:
            for x in range(W):
                px[x][ay] = win_lit
    # PARAPET CAP at the roofline (top rows, V~0). Only OUTWARD-facing walls show,
    # so from top-down this lip appears exactly at real building edges — never as an
    # internal grid line across a merged block. Bright cap + shadow reveal below.
    cap = mix(wall, (255,255,255), 0.35)
    for x in range(W):
        px[x][0] = cap
        px[x][1] = mix(cap, wall, 0.4)
        px[x][2] = shade(wall, 0.7)

def roof(px, base, edge, dark, light, accent, rnd, props):
    """SEAMLESS tar/gravel roof + mechanical clutter into px[32..63][0..31].

    NO cell-edge frame: a building is many tiles butted together, so a baked
    border would draw a grid across every big roof (the 'dice' look). Instead the
    base tiles seamlessly and props sit INSET, so a merged block reads as one
    continuous cluttered rooftop. The perimeter parapet lives on the wall-top."""
    OX = W
    # gravel/tar base — two-tone speckle that tiles cleanly (edge-neutral)
    for y in range(H):
        for x in range(W):
            n = rnd.random()
            c = base
            if n < 0.14: c = shade(base, 0.84)
            elif n > 0.86: c = shade(base, 1.14)
            # faint tar-seam lines every 8px read as roofing felt, and tile across cells
            if (x % 8) == 0 or (y % 8) == 0:
                if rnd.random() < 0.5: c = shade(base, 0.9)
            px[OX+x][y] = c

    def box(bx, by, bw, bh, top, side, detail=None, lines=0):
        for x in range(bx, bx+bw):
            for y in range(by, by+bh):
                if 0 <= x < W and 0 <= y < H:
                    px[OX+x][y] = top
        # drop shadow (south/east) for a hint of height
        for x in range(bx, bx+bw+1):
            y = by+bh
            if 0 <= x < W and 0 <= y < H: px[OX+x][y] = shade(base, 0.6)
        for y in range(by, by+bh+1):
            x = bx+bw
            if 0 <= x < W and 0 <= y < H: px[OX+x][y] = shade(base, 0.6)
        # side shade
        for y in range(by, by+bh):
            if 0 <= bx < W: px[OX+bx][y] = side
        # louver/fin lines on top
        for i in range(lines):
            ly = by + 1 + i*2
            if ly < by+bh-1:
                for x in range(bx+1, bx+bw-1):
                    if 0 <= x < W and 0 <= ly < H: px[OX+x][ly] = detail or side

    for p in props:
        kind, bx, by = p[0], p[1], p[2]
        if kind == 'ac':
            box(bx, by, 6, 5, shade(dark,1.25), dark, detail=shade(dark,0.8), lines=2)
        elif kind == 'ac2':
            box(bx, by, 4, 4, shade(dark,1.15), dark, detail=shade(dark,0.75), lines=2)
        elif kind == 'tank':   # water tank: round-ish dark cylinder on legs
            box(bx, by, 7, 7, shade(accent,0.9), shade(accent,0.6))
            for x in range(bx+1, bx+6):
                if 0 <= x < W: px[OX+x][by] = shade(accent,1.2)   # highlit top rim
            # legs shadow
            for lx in (bx+1, bx+5):
                yy = by+7
                if 0 <= lx < W and yy < H: px[OX+lx][yy] = shade(base,0.5)
        elif kind == 'sky':    # skylight / glass rooflight (mullioned)
            box(bx, by, 6, 4, light, shade(light,0.7))
            mx = bx+3
            if 0 <= mx < W:
                for y in range(by, by+4): px[OX+mx][y] = shade(light,0.7)
        elif kind == 'hatch':  # roof access hatch
            box(bx, by, 4, 3, shade(dark,1.1), shade(dark,0.7))
        elif kind == 'vent':   # small vent pipe
            for x in range(bx, bx+2):
                for y in range(by, by+2):
                    if 0 <= x < W and 0 <= y < H: px[OX+x][y] = shade(dark,0.7)
        elif kind == 'sign':   # rooftop signage bar
            box(bx, by, 10, 3, accent, shade(accent,0.6))
        elif kind == 'duct':   # long duct run
            box(bx, by, 12, 3, shade(dark,1.2), dark, detail=shade(dark,0.8), lines=1)
        elif kind == 'heli':   # helipad marking
            for x in range(bx, bx+10):
                for y in range(by, by+10):
                    if (x-bx-4.5)**2+(y-by-4.5)**2 < 22 and 0<=x<W and 0<=y<H:
                        px[OX+x][y] = shade(base,0.75)
            # H mark
            for y in range(by+2, by+8):
                if bx+3<W: px[OX+bx+3][y]=light
                if bx+6<W: px[OX+bx+6][y]=light
            for x in range(bx+3, bx+7):
                if by+4<H: px[OX+x][by+4]=light

# variant: (name, wall, mortar, win_lit, win_dark, glass, style, floors, cols,
#           storefront, roofbase, roofedge, roofdark, rooflight, roofaccent, props)
Y_LIT = (255, 224, 150)
# Props are kept INSET (x,y in ~3..27) so nothing clips a tile seam. A mix of one
# hero item + scattered small clutter (vents/hatches) reads as a busy rooftop even
# when the same cell tiles across a multi-tile building.
V = [
 ("bld_brick", (150,74,54),(96,52,44),Y_LIT,(60,40,44),(70,60,70),'brick',7,5,True,
   (72,60,52),(120,104,92),(120,124,130),(150,190,210),(90,80,70),
   [('ac',4,5),('tank',20,4),('vent',13,19),('hatch',5,20),('vent',24,22),('vent',10,26)]),
 ("bld_office", (96,110,128),(60,70,86),(210,235,255),(52,66,86),(70,96,124),'office',9,4,False,
   (96,100,108),(150,156,164),(120,128,140),(170,205,235),(120,130,145),
   [('ac',4,4),('ac',19,4),('duct',6,17),('sky',20,20),('vent',4,26),('vent',26,14)]),
 ("bld_tower", (52,58,70),(34,38,48),(190,210,240),(40,48,64),(48,60,84),'tower',12,5,False,
   (56,60,70),(96,102,116),(90,98,114),(150,190,230),(70,78,96),
   [('heli',4,4),('tank',20,19),('vent',4,26),('duct',12,26),('hatch',26,4)]),
 ("bld_concrete", (168,162,150),(120,114,104),(250,240,210),(120,120,120),(120,124,128),'concrete',8,6,False,
   (172,166,154),(200,196,186),(130,128,124),(190,210,225),(150,144,132),
   [('ac',4,4),('ac2',22,5),('sky',5,20),('vent',24,22),('hatch',14,13),('vent',15,26)]),
 ("bld_glass", (70,120,140),(40,74,90),(190,240,255),(60,110,130),(90,160,180),'glass',10,4,False,
   (86,120,132),(140,180,190),(100,140,155),(180,235,250),(110,150,165),
   [('sky',4,3),('sky',4,11),('sky',4,19),('duct',17,21),('ac',19,4),('vent',26,26)]),
 ("bld_painted", (206,180,140),(150,124,94),Y_LIT,(120,96,70),(120,110,96),'painted',6,4,True,
   (150,96,72),(200,150,110),(130,120,110),(200,210,220),(150,110,84),
   [('ac',4,5),('tank',20,5),('hatch',5,21),('sign',10,3),('vent',24,22),('vent',13,26)]),
 ("bld_brownstone", (120,60,48),(78,42,36),Y_LIT,(52,34,36),(64,50,52),'brick',6,4,True,
   (86,54,44),(140,90,72),(96,72,64),(170,190,205),(70,54,48),
   [('ac2',5,6),('vent',19,5),('vent',24,7),('hatch',5,20),('duct',15,22),('vent',26,26)]),
 ("bld_panel", (150,150,158),(96,96,104),(230,235,245),(96,100,112),(110,120,140),'office',8,5,False,
   (120,124,132),(178,182,190),(112,118,130),(185,210,230),(120,128,142),
   [('duct',4,4),('ac',20,4),('sky',5,20),('tank',21,18),('hatch',14,12),('vent',26,26)]),
]

for spec in V:
    (name, wall, mortar, win_lit, win_dark, glass, style, floors, cols, storefront,
     rb, re, rd, rl, ra, props) = spec
    rnd = random.Random(hash(name) & 0xffffffff)
    px = [[(0,0,0)]*H for _ in range(2*W)]
    facade(px, wall, mortar, win_lit, win_dark, glass, style, rnd, floors, cols, storefront)
    roof(px, rb, re, rd, rl, ra, rnd, props)
    im = Image.new("RGB", (2*W, H))
    for x in range(2*W):
        for y in range(H):
            im.putpixel((x, y), px[x][y])
    im.save(name + ".png")
    print("wrote", name+".png")
