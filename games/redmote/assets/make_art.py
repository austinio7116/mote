#!/usr/bin/env python3
"""Red Mote art — writes the EDITABLE source PNGs (tiles.png, units.png,
buildings.png + ../icon.png). Run from anywhere; deterministic.

tiles.png      16 cells x 8x8, one row: terrain atlas (opaque).
                 0-3 grass  4-5 water(anim)  6-7 rock  8-9 trees
                 10-12 ore(sparse..dense)  13 crystal  14 concrete  15 black
units.png      16 cols x 8x8, 2 team rows (0 player blue, 1 enemy red):
                 0 rifle A   1 rifle B   2 rifle fire
                 3 rocketeer A  4 rocketeer B
                 5 flamer A     6 flamer B
                 7 light hull   8 light turret
                 9 heavy hull  10 heavy turret
                11 artillery   12 tesla tank
                13 harvester   14 gunship rotorA  15 gunship rotorB
               (vehicle sprites face UP; game rotates with blit_ex)
buildings.png  strip per team (row h=24): see XOFF table below.
icon.png       60x60 launcher emblem.
"""
from PIL import Image, ImageDraw
import os, random

HERE = os.path.dirname(os.path.abspath(__file__))
rng = random.Random(1917)

# ---------------------------------------------------------------- palettes
TEAM = [  # base, light, dark  (player blue / enemy red)
    ((58, 110, 168), (111, 160, 216), (36, 70, 110)),
    ((168, 58, 50), (216, 111, 98), (110, 36, 32)),
]
GUN   = (52, 52, 58)     # gunmetal
GUNL  = (86, 86, 96)
GUND  = (30, 30, 36)
TRACK = (40, 38, 34)
SKIN  = (214, 178, 140)
GRASS = (58, 84, 44)


def px(d, x, y, c):
    d.point((x, y), fill=c + (255,))


def rect(d, x0, y0, x1, y1, c):
    d.rectangle((x0, y0, x1, y1), fill=c + (255,))


def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)


# ====================================================== BLOB47 autotiles
# Canonical 47-cell blob template, exactly matching mote_tile.h's
# mote_autotile_template(MOTE_AT_BLOB47): reduced masks in first-seen order.
NB_N, NB_NE, NB_E, NB_SE, NB_S, NB_SW, NB_W, NB_NW = 1, 2, 4, 8, 16, 32, 64, 128


def at_reduce(m):
    r = m & (NB_N | NB_E | NB_S | NB_W)
    if m & NB_NE and r & NB_N and r & NB_E: r |= NB_NE
    if m & NB_SE and r & NB_S and r & NB_E: r |= NB_SE
    if m & NB_SW and r & NB_S and r & NB_W: r |= NB_SW
    if m & NB_NW and r & NB_N and r & NB_W: r |= NB_NW
    return r


BLOB_ORDER = []
BLOB_LUT = [0] * 256
_seen = {}
for _m in range(256):
    _r = at_reduce(_m)
    if _r not in _seen:
        _seen[_r] = len(BLOB_ORDER)
        BLOB_ORDER.append(_r)
    BLOB_LUT[_m] = _seen[_r]


def write_tileset(name, sheet, nvar, lut, cols):
    with open(os.path.join(HERE, "..", "tilesets", name + ".tileset"), "w") as f:
        f.write("sheet %s\ntile 8\ntype 0\nedge 1\nnvar %d\ncols %d\nrows %d\n" %
                (sheet, nvar, cols, nvar))
        f.write("lut " + " ".join(str(v) for v in lut) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        f.write("vweight 1 1 1 1 1 1 1 1\n")


def blob_sheet(name, nvar, painter):
    """47 cells x nvar variant rows, painted from the reduced neighbour mask."""
    im = Image.new("RGBA", (47 * 8, nvar * 8), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for v in range(nvar):
        for ci, mask in enumerate(BLOB_ORDER):
            painter(d, ci * 8, v * 8, mask, v)
    im.save(os.path.join(HERE, name + ".png"))
    write_tileset(name, "assets/%s.png" % name, nvar, BLOB_LUT, 47)


# side/corner helpers: a cell's missing cardinal = a border on that side
def sides(m):
    return (not m & NB_N, not m & NB_E, not m & NB_S, not m & NB_W)


def cell_rng(name, m, v):
    return random.Random("%s/%d/%d" % (name, m, v))


def cw_water(d, x0, y0, m, v):
    """RA-style water: bright high-frequency shimmer (per-pixel noise, two anim
    frames), edged with a chunky irregular ROCK shoreline, not a foam line."""
    W = [(46, 88, 132), (54, 100, 146), (64, 112, 158), (76, 126, 170)]
    SPARK = (112, 158, 196)
    RK = [(38, 34, 30), (58, 52, 44), (82, 74, 62), (104, 96, 82)]
    r2 = cell_rng("w", m, v)
    def wpx(x, y):
        h = ((x + v * 3) * 7 + (y + v * 5) * 13 + v * 11) % 16
        if h == 0: return SPARK
        return W[h % 4]
    for y in range(8):
        for x in range(8):
            px(d, x0 + x, y0 + y, wpx(x, y))
    mn, me, ms, mw = sides(m)
    # rocky shore band along open sides: jagged 1-2px rubble
    def rock(x, y, big):
        px(d, x0 + x, y0 + y, RK[r2.randrange(3)])
        if big and x < 7 and r2.random() < 0.5:
            px(d, x0 + x + 1, y0 + y, RK[1 + r2.randrange(3)])
    if mn:
        for x in range(8):
            if r2.random() < 0.85: rock(x, 0, x % 3 == 1)
            if r2.random() < 0.45: rock(x, 1, 0)
    if ms:
        for x in range(8):
            if r2.random() < 0.85: rock(x, 7, x % 3 == 2)
            if r2.random() < 0.45: rock(x, 6, 0)
    if mw:
        for y in range(8):
            if r2.random() < 0.85: rock(0, y, 0)
            if r2.random() < 0.45: rock(1, y, 0)
    if me:
        for y in range(8):
            if r2.random() < 0.85: rock(7, y, 0)
            if r2.random() < 0.45: rock(6, y, 0)
    # convex corners: transparent rounding + a rock knuckle
    for (cx, cy, c1, c2) in ((0, 0, mn, mw), (7, 0, mn, me), (0, 7, ms, mw), (7, 7, ms, me)):
        if c1 and c2:
            d.point((x0 + cx, y0 + cy), fill=(0, 0, 0, 0))
            px(d, x0 + (1 if cx == 0 else 6), y0 + cy, RK[0])
            px(d, x0 + cx, y0 + (1 if cy == 0 else 6), RK[1])
    # inner corners: rock fleck where the shore turns
    for (bit, ca, cb, cx, cy) in ((NB_NE, NB_N, NB_E, 7, 0), (NB_SE, NB_S, NB_E, 7, 7),
                                  (NB_SW, NB_S, NB_W, 0, 7), (NB_NW, NB_N, NB_W, 0, 0)):
        if (m & ca) and (m & cb) and not (m & bit):
            px(d, x0 + cx, y0 + cy, RK[0])
            px(d, x0 + cx, y0 + cy + (1 if cy == 0 else -1), RK[2])


def cw_rock(d, x0, y0, m, v):
    base, hi, dk = (88, 86, 92), (108, 106, 114), (58, 56, 62)
    rect(d, x0, y0, x0 + 7, y0 + 7, base)
    r2 = cell_rng("r", m, v)
    for _ in range(6):
        x, y = r2.randrange(7), r2.randrange(7)
        rect(d, x0 + x, y0 + y, x0 + x + 1, y0 + y + 1, hi)
    for _ in range(4):
        px(d, x0 + r2.randrange(8), y0 + r2.randrange(8), dk)
    px(d, x0 + r2.randrange(8), y0 + r2.randrange(8), (128, 126, 134))
    mn, me, ms, mw = sides(m)
    if mn:
        for x in range(8):
            if x % 3 == 1: d.point((x0 + x, y0), fill=(0, 0, 0, 0))
            else: px(d, x0 + x, y0, dk)
    if ms:
        for x in range(8):
            if x % 3 == 2: d.point((x0 + x, y0 + 7), fill=(0, 0, 0, 0))
            else: px(d, x0 + x, y0 + 7, (44, 42, 48))
    if mw:
        for y in range(8):
            if y % 3 == 1: d.point((x0, y0 + y), fill=(0, 0, 0, 0))
            else: px(d, x0, y0 + y, dk)
    if me:
        for y in range(8):
            if y % 3 == 2: d.point((x0 + 7, y0 + y), fill=(0, 0, 0, 0))
            else: px(d, x0 + 7, y0 + y, (44, 42, 48))
    for (cx, cy, c1, c2) in ((0, 0, mn, mw), (7, 0, mn, me), (0, 7, ms, mw), (7, 7, ms, me)):
        if c1 and c2:
            d.point((x0 + cx, y0 + cy), fill=(0, 0, 0, 0))
            d.point((x0 + cx + (1 if cx == 0 else -1), y0 + cy), fill=(0, 0, 0, 0))


TREE_BIG = [        # 5x5 crown: lit top-left, shaded bottom-right rim
    ".kkk.",
    "kmhmk",
    "khHmk",
    "kmmdk",
    ".kdk.",
]
TREE_SML = [
    ".k.",
    "mhk",
    "kdk",
]


def cw_tree(d, x0, y0, m, v):
    """forest canopy: clustered round tree crowns (lit top-left), grass gaps
    between crowns; crowns thin out toward the forest edge."""
    mn, me, ms, mw = sides(m)
    r2 = cell_rng("t", m, v)
    pal = {"k": (18, 38, 16), "d": (26, 50, 22), "m": (38, 72, 30),
           "h": (56, 100, 42), "H": (84, 134, 56)}

    def stamp(tmpl, cx, cy):
        hh, ww = len(tmpl), len(tmpl[0])
        for yy in range(hh):
            for xx in range(ww):
                ch = tmpl[yy][xx]
                if ch == ".":
                    continue
                X, Y = cx - ww // 2 + xx, cy - hh // 2 + yy
                if 0 <= X < 8 and 0 <= Y < 8:
                    px(d, x0 + X, y0 + Y, pal[ch])

    # candidate crowns; variant shifts the layout so the forest doesn't grid up
    if v == 0:
        spots = [(2, 2, TREE_BIG), (6, 5, TREE_BIG), (6, 1, TREE_SML), (1, 6, TREE_SML)]
    else:
        spots = [(5, 2, TREE_BIG), (1, 5, TREE_BIG), (2, 7, TREE_SML), (7, 7, TREE_SML)]
    for (cx, cy, tmpl) in spots:
        # near an open side, crowns thin out (forest edge raggedness)
        drop = 0.0
        if mn and cy < 3: drop += 0.55
        if ms and cy > 4: drop += 0.55
        if mw and cx < 3: drop += 0.55
        if me and cx > 4: drop += 0.55
        if r2.random() < drop:
            continue
        stamp(tmpl, cx, cy)


def cw_ore(d, x0, y0, m, v):
    """dense shimmering ore carpet: 2x2 clumps of layered golds, thinning to
    scattered clusters at the field edge (RA's glittering fields)."""
    G = [(112, 86, 18), (150, 116, 24), (190, 150, 34), (226, 188, 52)]
    SPARK = (252, 230, 110)
    mn, me, ms, mw = sides(m)
    r2 = cell_rng("o", m, v)
    for y in range(8):
        for x in range(8):
            k = 0.86
            if mn and y < 3: k *= 0.30
            if ms and y > 4: k *= 0.30
            if mw and x < 3: k *= 0.30
            if me and x > 4: k *= 0.30
            if r2.random() > k: continue
            # 2x2 clump shading so it reads as nugget heaps, not static
            h = ((x // 2) * 31 + (y // 2) * 17 + v * 7) % 8
            c = G[h % 4]
            if ((x * 13 + y * 7 + v * 5) % 11) == 0: c = SPARK
            px(d, x0 + x, y0 + y, c)


def cw_crys(d, x0, y0, m, v):
    """crystal field: clustered cyan shards, denser than before, dark facets."""
    C = [(30, 110, 128), (52, 160, 178), (90, 208, 222), (150, 240, 248)]
    mn, me, ms, mw = sides(m)
    r2 = cell_rng("c2", m, v)
    for y in range(8):
        for x in range(8):
            k = 0.55
            if mn and y < 3: k *= 0.3
            if ms and y > 4: k *= 0.3
            if mw and x < 3: k *= 0.3
            if me and x > 4: k *= 0.3
            if r2.random() > k: continue
            h = ((x // 2) * 23 + (y // 2) * 29 + v * 13) % 4
            px(d, x0 + x, y0 + y, C[h])


def cw_scorch(d, x0, y0, m, v):
    base = (44, 38, 30)
    mn, me, ms, mw = sides(m)
    r2 = cell_rng("s", m, v)
    for y in range(8):
        for x in range(8):
            edge = ((mn and y < 2) or (ms and y > 5) or (mw and x < 2) or (me and x > 5))
            if edge and r2.random() < 0.55: continue
            px(d, x0 + x, y0 + y, base)
    for _ in range(6):
        px(d, x0 + r2.randrange(8), y0 + r2.randrange(8), (30, 26, 20))
    for _ in range(3):
        px(d, x0 + r2.randrange(8), y0 + r2.randrange(8), (60, 52, 40))
    if r2.random() < 0.5: px(d, x0 + r2.randrange(2, 6), y0 + r2.randrange(2, 6), (90, 60, 30))


def cw_conc(d, x0, y0, m, v):
    rect(d, x0, y0, x0 + 7, y0 + 7, (74, 74, 78))
    r2 = cell_rng("c", m, v)
    for _ in range(4):
        px(d, x0 + r2.randrange(8), y0 + r2.randrange(8), (68, 68, 72))
    mn, me, ms, mw = sides(m)
    if mn:
        for x in range(8): px(d, x0 + x, y0, (94, 94, 100))
    if mw:
        for y in range(8): px(d, x0, y0 + y, (88, 88, 94))
    if ms:
        for x in range(8): px(d, x0 + x, y0 + 7, (54, 54, 58))
    if me:
        for y in range(8): px(d, x0 + 7, y0 + y, (58, 58, 62))


def cw_fog(d, x0, y0, m, v):
    """Shroud: solid black interior, dithered feather toward explored sides.
    Drawn OVER the terrain, so transparent px = terrain peeking through."""
    mn, me, ms, mw = sides(m)
    for y in range(8):
        for x in range(8):
            # distance (px) to the nearest explored side of this cell
            dist = 8
            if mn: dist = min(dist, y)
            if ms: dist = min(dist, 7 - y)
            if mw: dist = min(dist, x)
            if me: dist = min(dist, 7 - x)
            # inner corners feather diagonally too
            for (bit, ca, cb, cx, cy) in ((NB_NE, NB_N, NB_E, 7, 0), (NB_SE, NB_S, NB_E, 7, 7),
                                          (NB_SW, NB_S, NB_W, 0, 7), (NB_NW, NB_N, NB_W, 0, 0)):
                if (m & ca) and (m & cb) and not (m & bit):
                    dist = min(dist, (abs(x - cx) + abs(y - cy)) // 2)
            if dist >= 3:
                px(d, x0 + x, y0 + y, (0, 0, 0))
            elif dist == 2:
                if (x + y * 3 + v) % 4 != 0: px(d, x0 + x, y0 + y, (0, 0, 0))
            elif dist == 1:
                if (x + y) % 2 == v % 2: px(d, x0 + x, y0 + y, (0, 0, 0))
            else:
                if (x * 3 + y + v) % 4 == 0: px(d, x0 + x, y0 + y, (0, 0, 0))


def cw_road(d, x0, y0, m, v):
    """Worn dirt trail, RA-style: ragged mud bed with faint, broken, WAVY wheel
    ruts. Each connected side's rut pair bends through per-variant jittered hub
    points, so straights S-curve, corners arc, and junctions read as worn
    crossings — never tramlines."""
    dirt, dirt2, dirt3 = (100, 83, 53), (88, 72, 46), (108, 92, 62)
    rut, rutd = (74, 60, 40), (60, 48, 32)
    r2 = cell_rng("rd", m, v)
    mn, me, ms, mw = sides(m)
    # ragged mud bed, thinning toward open sides, with noisy edges
    for y in range(8):
        for x in range(8):
            k = 0.58
            if mn and y < 2: k *= 0.22
            if ms and y > 5: k *= 0.22
            if mw and x < 2: k *= 0.22
            if me and x > 5: k *= 0.22
            if r2.random() < k:
                c = (dirt, dirt2, dirt3)[(x * 5 + y * 3 + v) % 3]
                px(d, x0 + x, y0 + y, c)
    # rut polylines: edge points fixed (tiles must join), hubs jittered by variant
    def seg(p0, p1, col, drop):
        steps = max(abs(p1[0] - p0[0]), abs(p1[1] - p0[1]), 1) * 2
        for i in range(steps + 1):
            t = i / steps
            xx = int(round(p0[0] + (p1[0] - p0[0]) * t))
            yy = int(round(p0[1] + (p1[1] - p0[1]) * t))
            if r2.random() > drop:
                px(d, x0 + xx, y0 + yy, col)
    jl = ((v * 7 + 1) % 3) - 1
    jr = ((v * 5 + 2) % 3) - 1
    Lh = (3 + jl, 3 + ((v * 3) % 3) - 1)
    Rh = (4 + jr, 4 + ((v * 11) % 3) - 1)
    EDGES = {"N": ((2, 0), (5, 0)), "S": ((2, 7), (5, 7)),
             "W": ((0, 2), (0, 5)), "E": ((7, 2), (7, 5))}
    conn = [c for c, on in (("N", m & NB_N), ("S", m & NB_S),
                            ("W", m & NB_W), ("E", m & NB_E)) if on]
    for c in conn:
        e = EDGES[c]
        seg(e[0], Lh, rut, 0.30)
        seg(e[1], Rh, rut, 0.30)
    if not conn:      # isolated worn patch: a hint of criss-cross
        seg((1, 2), Rh, rut, 0.35)
        seg((6, 5), Lh, rut, 0.35)
    # sparse darker wear along the trail + odd stone
    for _ in range(3):
        px(d, x0 + r2.randrange(8), y0 + r2.randrange(8), rutd)
    if r2.random() < 0.3:
        px(d, x0 + r2.randrange(2, 6), y0 + r2.randrange(2, 6), (118, 112, 100))


def make_autotiles():
    blob_sheet("road", 4, cw_road)
    blob_sheet("fog", 2, cw_fog)
    blob_sheet("water", 2, cw_water)
    blob_sheet("rock", 2, cw_rock)
    blob_sheet("tree", 2, cw_tree)
    blob_sheet("ore", 2, cw_ore)
    blob_sheet("crys", 1, cw_crys)
    blob_sheet("scorch", 2, cw_scorch)
    blob_sheet("conc", 1, cw_conc)
    # grass base: single cell, 4 densely mottled variant rows (RA-dark)
    im = Image.new("RGBA", (8, 4 * 8), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    GS = [(46, 68, 34), (54, 78, 40), (60, 88, 46), (50, 74, 38)]
    for v in range(4):
        y0 = v * 8
        r2 = random.Random(100 + v)
        for y in range(8):
            for x in range(8):
                px(d, x, y0 + y, GS[(x * 7 + y * 13 + v * 5 + r2.randrange(2)) % 4])
        for _ in range(3):
            px(d, r2.randrange(8), y0 + r2.randrange(8), (74, 104, 56))
        for _ in range(2):
            px(d, r2.randrange(8), y0 + r2.randrange(8), (88, 76, 48))
    im.save(os.path.join(HERE, "grass.png"))
    write_tileset("grass", "assets/grass.png", 4, [0] * 256, 1)


# ================================================================= units
# tiny infantry templates 8x8: . none  k dark  b team base  l team light
# s skin  g gun  o orange
INF = {
    "rifleA": [
        "........",
        "...s....",
        "...b....",
        "..bbbg..",
        "...b.g..",
        "...bb...",
        "...k.k..",
        "........",
    ],
    "rifleB": [
        "........",
        "...s....",
        "...b....",
        "..bbbg..",
        "...b.g..",
        "...bb...",
        "..k...k.",
        "........",
    ],
    "rifleF": [
        "........",
        "...s....",
        "...b....",
        "..bbggo.",
        "...b....",
        "...bb...",
        "...k.k..",
        "........",
    ],
    "rockA": [
        "........",
        "...s.g..",
        "...bgg..",
        "..bbbg..",
        "...b....",
        "...bb...",
        "...k.k..",
        "........",
    ],
    "rockB": [
        "........",
        "...s.g..",
        "...bgg..",
        "..bbbg..",
        "...b....",
        "...bb...",
        "..k...k.",
        "........",
    ],
    "flamA": [
        "........",
        "...s....",
        "..ob....",
        "..obbgg.",
        "..ob.o..",
        "...bb...",
        "...k.k..",
        "........",
    ],
    "flamB": [
        "........",
        "...s....",
        "..ob....",
        "..obbgg.",
        "..ob.o..",
        "...bb...",
        "..k...k.",
        "........",
    ],
}


def draw_template(d, x0, y0, rows, team):
    base, light, dark = TEAM[team]
    m = {"k": (20, 20, 22), "b": base, "l": light, "d": dark,
         "s": SKIN, "g": GUN, "o": (216, 130, 40)}
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch != ".":
                px(d, x0 + x, y0 + y, m[ch])


def draw_light_hull(d, x0, y0, team):
    base, light, dark = TEAM[team]
    rect(d, x0 + 1, y0 + 1, x0 + 2, y0 + 6, TRACK)   # left tread
    rect(d, x0 + 5, y0 + 1, x0 + 6, y0 + 6, TRACK)   # right tread
    px(d, x0 + 1, y0 + 2, (70, 66, 60)); px(d, x0 + 6, y0 + 2, (70, 66, 60))
    rect(d, x0 + 3, y0 + 1, x0 + 4, y0 + 6, base)    # hull
    px(d, x0 + 3, y0 + 1, light); px(d, x0 + 4, y0 + 6, dark)


def draw_heavy_hull(d, x0, y0, team):
    base, light, dark = TEAM[team]
    rect(d, x0 + 0, y0 + 0, x0 + 1, y0 + 7, TRACK)
    rect(d, x0 + 6, y0 + 0, x0 + 7, y0 + 7, TRACK)
    px(d, x0 + 0, y0 + 1, (70, 66, 60)); px(d, x0 + 7, y0 + 1, (70, 66, 60))
    rect(d, x0 + 2, y0 + 0, x0 + 5, y0 + 7, base)
    rect(d, x0 + 2, y0 + 0, x0 + 5, y0 + 0, light)
    rect(d, x0 + 2, y0 + 7, x0 + 5, y0 + 7, dark)
    px(d, x0 + 3, y0 + 6, dark); px(d, x0 + 4, y0 + 6, dark)


def make_units():
    im = Image.new("RGBA", (16 * 8, 2 * 8), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for team in range(2):
        base, light, dark = TEAM[team]
        y0 = team * 8
        for i, key in enumerate(("rifleA", "rifleB", "rifleF", "rockA", "rockB", "flamA", "flamB")):
            draw_template(d, i * 8, y0, INF[key], team)

        # 7: light tank hull (faces up)
        draw_light_hull(d, 7 * 8, y0, team)
        # 8: light turret — small dome + barrel up
        x0 = 8 * 8
        rect(d, x0 + 3, y0 + 3, x0 + 4, y0 + 4, light)
        px(d, x0 + 4, y0 + 4, dark)
        rect(d, x0 + 3, y0 + 0, x0 + 3, y0 + 2, GUN)   # barrel (1px, slightly off-center reads fine)
        px(d, x0 + 3, y0 + 0, GUNL)
        # 9: heavy hull
        draw_heavy_hull(d, 9 * 8, y0, team)
        # 10: heavy turret — wide dome + twin barrels
        x0 = 10 * 8
        rect(d, x0 + 3, y0 + 3, x0 + 5, y0 + 5, light)
        px(d, x0 + 5, y0 + 5, dark); px(d, x0 + 3, y0 + 3, (240, 240, 240))
        rect(d, x0 + 3, y0 + 0, x0 + 3, y0 + 2, GUN)
        rect(d, x0 + 5, y0 + 0, x0 + 5, y0 + 2, GUN)
        # 11: artillery — open chassis, long fixed gun
        x0 = 11 * 8
        rect(d, x0 + 1, y0 + 2, x0 + 2, y0 + 7, TRACK)
        rect(d, x0 + 5, y0 + 2, x0 + 6, y0 + 7, TRACK)
        rect(d, x0 + 3, y0 + 3, x0 + 4, y0 + 6, base)
        px(d, x0 + 3, y0 + 3, light)
        rect(d, x0 + 3, y0 + 0, x0 + 4, y0 + 0, GUNL)   # muzzle brake
        rect(d, x0 + 3, y0 + 1, x0 + 4, y0 + 2, GUN)    # long barrel
        # 12: tesla tank — hull + coil orb
        x0 = 12 * 8
        draw_light_hull(d, x0, y0, team)
        px(d, x0 + 3, y0 + 2, (32, 130, 150))
        rect(d, x0 + 3, y0 + 3, x0 + 4, y0 + 4, (64, 200, 216))
        px(d, x0 + 3, y0 + 3, (170, 240, 248))
        # 13: harvester — chunky ore truck
        x0 = 13 * 8
        rect(d, x0 + 1, y0 + 0, x0 + 2, y0 + 7, TRACK)
        rect(d, x0 + 5, y0 + 0, x0 + 6, y0 + 7, TRACK)
        rect(d, x0 + 3, y0 + 0, x0 + 4, y0 + 7, (150, 116, 24))
        rect(d, x0 + 3, y0 + 0, x0 + 4, y0 + 1, base)     # cab (team)
        px(d, x0 + 3, y0 + 0, light)
        px(d, x0 + 3, y0 + 3, (232, 200, 64))             # ore glint in hopper
        px(d, x0 + 4, y0 + 5, (200, 160, 32))
        # 14/15: gunship, rotor frame A/B
        for f in range(2):
            x0 = (14 + f) * 8
            rect(d, x0 + 3, y0 + 1, x0 + 4, y0 + 6, base)      # fuselage
            px(d, x0 + 3, y0 + 1, light); px(d, x0 + 4, y0 + 6, dark)
            rect(d, x0 + 2, y0 + 5, x0 + 5, y0 + 5, dark)      # tail plane
            px(d, x0 + 3, y0 + 2, (26, 52, 92))                # canopy
            if f == 0:
                rect(d, x0 + 0, y0 + 3, x0 + 7, y0 + 3, (200, 200, 208))
            else:
                rect(d, x0 + 3, y0 + 0, x0 + 3, y0 + 6, (200, 200, 208))
    im.save(os.path.join(HERE, "units.png"))


# ============================================================== buildings
# strip layout, row height 24 per team.  (x, w, h) — game mirrors this table.
BXOFF = {
    "conyard":  (0,   24, 24),
    "power":    (24,  16, 16),
    "refinery": (40,  24, 16),
    "barracks": (64,  16, 16),
    "factory":  (80,  24, 16),
    "radar":    (104, 16, 16),
    "helipad":  (120, 16, 16),
    "tech":     (136, 16, 16),
    "pillbox":  (152, 8,  8),
    "turret":   (160, 8,  8),
    "turgun":   (168, 8,  8),
    "tesla":    (176, 8,  16),
}
ROOF = (96, 96, 104)
ROOFL = (120, 120, 130)
ROOFD = (66, 66, 74)
HAZ = (208, 172, 40)


def bdg_body(d, x0, y0, w, h, team):
    """grey industrial slab with lit top-left edge + team trim strip."""
    base, light, dark = TEAM[team]
    rect(d, x0, y0, x0 + w - 1, y0 + h - 1, ROOF)
    rect(d, x0, y0, x0 + w - 1, y0, ROOFL)
    rect(d, x0, y0, x0, y0 + h - 1, ROOFL)
    rect(d, x0, y0 + h - 1, x0 + w - 1, y0 + h - 1, ROOFD)
    rect(d, x0 + w - 1, y0, x0 + w - 1, y0 + h - 1, ROOFD)
    rect(d, x0 + 1, y0 + 1, x0 + w - 2, y0 + 1, base)  # team trim under the lit edge


def make_buildings():
    im = Image.new("RGBA", (192, 48), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for team in range(2):
        base, light, dark = TEAM[team]
        y0 = team * 24

        # conyard 24x24: big block, crane arm, hazard door
        x0, w, h = BXOFF["conyard"][0], 24, 24
        bdg_body(d, x0, y0, w, h, team)
        rect(d, x0 + 3, y0 + 4, x0 + 12, y0 + 12, ROOFD)          # roof pit
        rect(d, x0 + 4, y0 + 5, x0 + 11, y0 + 11, (52, 52, 58))
        rect(d, x0 + 14, y0 + 3, x0 + 20, y0 + 5, base)           # crane cab
        rect(d, x0 + 16, y0 + 5, x0 + 17, y0 + 14, light)         # crane arm
        for i in range(0, 8, 2):                                   # hazard door
            rect(d, x0 + 6 + i, y0 + 19, x0 + 7 + i, y0 + 22, HAZ if (i // 2) % 2 == 0 else (30, 30, 34))
        rect(d, x0 + 2, y0 + 15, x0 + 4, y0 + 17, light)          # beacon
        # power 16x16: twin stacks + bolt
        x0, w, h = BXOFF["power"][0], 16, 16
        bdg_body(d, x0, y0, w, h, team)
        for sx in (3, 9):
            rect(d, x0 + sx, y0 + 3, x0 + sx + 3, y0 + 8, ROOFL)
            rect(d, x0 + sx + 1, y0 + 3, x0 + sx + 2, y0 + 4, (40, 40, 46))
        rect(d, x0 + 6, y0 + 11, x0 + 7, y0 + 13, HAZ)            # bolt mark
        px(d, x0 + 8, y0 + 10, HAZ); px(d, x0 + 5, y0 + 14, HAZ)
        # refinery 24x16: silo + dock pad + ore chute
        x0, w, h = BXOFF["refinery"][0], 24, 16
        bdg_body(d, x0, y0, w, h, team)
        d.ellipse((x0 + 2, y0 + 2, x0 + 9, y0 + 9), fill=ROOFL + (255,))
        d.ellipse((x0 + 3, y0 + 3, x0 + 7, y0 + 7), fill=ROOF + (255,))
        rect(d, x0 + 14, y0 + 3, x0 + 21, y0 + 12, (44, 44, 50))  # dark dock pad
        rect(d, x0 + 15, y0 + 4, x0 + 20, y0 + 11, (58, 56, 50))
        px(d, x0 + 11, y0 + 11, (232, 200, 64)); px(d, x0 + 12, y0 + 12, (200, 160, 32))
        rect(d, x0 + 3, y0 + 12, x0 + 8, y0 + 13, (200, 160, 32)) # ore chute
        # barracks 16x16: hut + door + flag
        x0, w, h = BXOFF["barracks"][0], 16, 16
        bdg_body(d, x0, y0, w, h, team)
        rect(d, x0 + 3, y0 + 4, x0 + 12, y0 + 7, ROOFD)           # roof ridge
        rect(d, x0 + 6, y0 + 10, x0 + 9, y0 + 14, (36, 36, 42))   # door
        rect(d, x0 + 13, y0 + 2, x0 + 13, y0 + 6, GUNL)           # flag pole
        rect(d, x0 + 11, y0 + 2, x0 + 12, y0 + 3, base)
        # factory 24x16: giant bay door with hazard lip
        x0, w, h = BXOFF["factory"][0], 24, 16
        bdg_body(d, x0, y0, w, h, team)
        rect(d, x0 + 4, y0 + 4, x0 + 19, y0 + 13, (40, 40, 46))   # bay
        for i in range(4, 20, 2):
            rect(d, x0 + i, y0 + 4, x0 + i, y0 + 5, HAZ if (i // 2) % 2 else (30, 30, 34))
        rect(d, x0 + 6, y0 + 7, x0 + 17, y0 + 7, (58, 58, 64))    # door slats
        rect(d, x0 + 6, y0 + 10, x0 + 17, y0 + 10, (58, 58, 64))
        # radar 16x16: dish on block
        x0, w, h = BXOFF["radar"][0], 16, 16
        bdg_body(d, x0, y0, w, h, team)
        d.ellipse((x0 + 3, y0 + 3, x0 + 12, y0 + 12), fill=ROOFL + (255,))
        d.ellipse((x0 + 5, y0 + 5, x0 + 10, y0 + 10), fill=(214, 214, 224, 255))
        px(d, x0 + 7, y0 + 7, (30, 30, 36)); px(d, x0 + 8, y0 + 8, (30, 30, 36))
        # helipad 16x16: pad circle + H
        x0, w, h = BXOFF["helipad"][0], 16, 16
        rect(d, x0, y0, x0 + 15, y0 + 15, (58, 58, 64))
        rect(d, x0, y0, x0 + 15, y0, (74, 74, 80))
        d.ellipse((x0 + 2, y0 + 2, x0 + 13, y0 + 13), outline=HAZ + (255,))
        rect(d, x0 + 6, y0 + 5, x0 + 6, y0 + 10, (214, 214, 224))
        rect(d, x0 + 9, y0 + 5, x0 + 9, y0 + 10, (214, 214, 224))
        rect(d, x0 + 7, y0 + 7, x0 + 8, y0 + 8, (214, 214, 224))
        px(d, x0 + 1, y0 + 1, base); px(d, x0 + 14, y0 + 1, base)  # team corner lights
        # tech 16x16: sleek lab, antenna, glow windows
        x0, w, h = BXOFF["tech"][0], 16, 16
        bdg_body(d, x0, y0, w, h, team)
        rect(d, x0 + 3, y0 + 3, x0 + 12, y0 + 12, (56, 60, 72))
        for wx in (4, 7, 10):
            rect(d, x0 + wx, y0 + 5, x0 + wx + 1, y0 + 6, (64, 200, 216))
            rect(d, x0 + wx, y0 + 9, x0 + wx + 1, y0 + 10, (64, 200, 216))
        rect(d, x0 + 13, y0 + 1, x0 + 13, y0 + 5, GUNL)
        px(d, x0 + 13, y0 + 1, (240, 80, 60))
        # pillbox 8x8: sandbag dome + slit
        x0 = BXOFF["pillbox"][0]
        d.ellipse((x0 + 0, y0 + 0, x0 + 7, y0 + 7), fill=(134, 118, 80, 255))
        d.ellipse((x0 + 1, y0 + 1, x0 + 6, y0 + 6), fill=(158, 140, 96, 255))
        rect(d, x0 + 2, y0 + 4, x0 + 5, y0 + 4, (30, 30, 34))
        px(d, x0 + 2, y0 + 1, base)
        # turret base 8x8 + gun 8x8 (barrel up)
        x0 = BXOFF["turret"][0]
        d.ellipse((x0 + 0, y0 + 0, x0 + 7, y0 + 7), fill=(70, 70, 78, 255))
        d.ellipse((x0 + 1, y0 + 1, x0 + 6, y0 + 6), fill=(96, 96, 104, 255))
        px(d, x0 + 1, y0 + 6, base)
        x0 = BXOFF["turgun"][0]
        rect(d, x0 + 3, y0 + 3, x0 + 4, y0 + 5, GUNL)
        rect(d, x0 + 3, y0 + 0, x0 + 3, y0 + 2, GUND)
        px(d, x0 + 4, y0 + 3, base)
        # tesla coil 8x16: pole + orb (draws 8px above its 1x1 tile)
        x0 = BXOFF["tesla"][0]
        rect(d, x0 + 3, y0 + 6, x0 + 4, y0 + 15, GUN)
        rect(d, x0 + 2, y0 + 14, x0 + 5, y0 + 15, GUNL)
        rect(d, x0 + 2, y0 + 6, x0 + 5, y0 + 7, GUNL)     # crown ring
        rect(d, x0 + 2, y0 + 1, x0 + 5, y0 + 4, (32, 130, 150))
        rect(d, x0 + 3, y0 + 2, x0 + 4, y0 + 3, (64, 200, 216))
        px(d, x0 + 3, y0 + 2, (170, 240, 248))
    im.save(os.path.join(HERE, "buildings.png"))


# ================================================================= heli
def make_heli():
    """gunship sheet, 12x12 cells x 2 team rows:
    col0 body (facing up)  col1 banked body (turning)  col2-5 rotor blades
    at 0/45/90/135 deg (cycle for spin). Body rotates via blit_ex; the rotor
    overlay just cycles frames."""
    import math
    im = Image.new("RGBA", (6 * 12, 2 * 12), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for team in range(2):
        base, light, dark = TEAM[team]
        y0 = team * 12
        for bank in range(2):
            x0 = bank * 12
            w2 = 3 if bank == 0 else 2          # banked body reads narrower
            xl = 6 - w2 // 2 - (1 if bank else 0)
            # skids
            if bank == 0:
                for yy in range(3, 9):
                    px(d, x0 + 3, y0 + yy, (34, 34, 40))
                    px(d, x0 + 8, y0 + yy, (34, 34, 40))
            else:
                for yy in range(3, 9):
                    px(d, x0 + 8, y0 + yy, (34, 34, 40))
            # tail boom + tail rotor
            for yy in range(8, 11):
                px(d, x0 + 6 - (1 if bank else 0), y0 + yy, dark)
            px(d, x0 + 4, y0 + 10, (150, 150, 160))
            px(d, x0 + 5 - (1 if bank else 0), y0 + 10, (190, 190, 200))
            # fuselage
            for yy in range(2, 8):
                for xx in range(xl, xl + w2):
                    px(d, x0 + xx, y0 + yy, base)
            # shading + canopy + engine
            for yy in range(2, 8):
                px(d, x0 + xl, y0 + yy, light)
            px(d, x0 + xl + (1 if w2 == 3 else 0), y0 + 2, (150, 220, 235))   # canopy
            px(d, x0 + xl + (1 if w2 == 3 else 0), y0 + 3, (90, 160, 190))
            px(d, x0 + xl + w2 - 1, y0 + 5, GUND)                              # engine
            px(d, x0 + xl + w2 - 1, y0 + 6, dark)
            # stub wings / rocket pods
            if bank == 0:
                px(d, x0 + 2, y0 + 5, GUN); px(d, x0 + 9, y0 + 5, GUN)
                px(d, x0 + 2, y0 + 6, GUNL); px(d, x0 + 9, y0 + 6, GUNL)
        # rotor frames: 2-blade at 0/45/90/135 degrees
        for f in range(4):
            x0 = (2 + f) * 12
            a = f * math.pi / 4
            for t in range(-5, 6):
                X = 6 + t * math.cos(a)
                Y = 6 + t * math.sin(a)
                c = (222, 222, 230) if abs(t) < 2 else (170, 170, 182) if abs(t) < 4 else (120, 120, 134)
                px(d, x0 + int(round(X)), y0 + int(round(Y)), c)
            px(d, x0 + 6, y0 + 6, (60, 60, 68))     # hub
    im.save(os.path.join(HERE, "heli.png"))


# ================================================================== icon
def make_icon():
    im = Image.new("RGBA", (60, 60), (30, 34, 26, 255))
    d = ImageDraw.Draw(im)
    r2 = random.Random(7)
    # mottled field
    for _ in range(160):
        x, y = r2.randrange(60), r2.randrange(60)
        px(d, x, y, r2.choice(((36, 42, 30), (26, 30, 22), (42, 48, 34))))
    # big red star, black outline
    import math
    cx, cy, R, r = 30, 26, 21, 8.5
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        rad = R if i % 2 == 0 else r
        pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    d.polygon(pts, fill=(178, 44, 36, 255), outline=(16, 12, 10, 255))
    # star shading
    d.line((pts[0], (cx, cy)), fill=(216, 84, 64, 255))
    # tiny tank column along the bottom
    for i, tx in enumerate((6, 22, 38)):
        ty = 47
        rect(d, tx, ty + 2, tx + 9, ty + 6, (52, 58, 46))       # hull
        rect(d, tx, ty + 1, tx + 9, ty + 1, (70, 78, 60))
        rect(d, tx - 1, ty + 5, tx + 10, ty + 7, (34, 36, 30))  # tracks
        rect(d, tx + 3, ty, tx + 6, ty + 2, (70, 78, 60))       # turret
        rect(d, tx + 6, ty + 1, tx + 13, ty + 1, (24, 24, 22))  # barrel
        px(d, tx + 14, ty + 1, (255, 220, 90))                  # muzzle flash
        px(d, tx + 15, ty + 1, (255, 160, 40))
    # tracer streaks
    for x, y in ((50, 14), (46, 20), (52, 24)):
        d.line((x, y, x + 5, y - 2), fill=(255, 220, 90, 255))
    im.convert("RGB").save(os.path.join(HERE, "..", "icon.png"))


make_autotiles()
make_units()
make_heli()
# buildings.png now comes from extract_buildings.py (AI-art sheet)
make_icon()
print("wrote autotile sheets units.png buildings.png ../icon.png")
