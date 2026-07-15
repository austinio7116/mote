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


def write_tileset(name, sheet, nvar, lut, cols, weights=None):
    with open(os.path.join(HERE, "..", "tilesets", name + ".tileset"), "w") as f:
        f.write("sheet %s\ntile 8\ntype 0\nedge 1\nnvar %d\ncols %d\nrows %d\n" %
                (sheet, nvar, cols, nvar))
        f.write("lut " + " ".join(str(v) for v in lut) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        w = (list(weights) + [1] * 8)[:8] if weights else [1] * 8
        f.write("vweight " + " ".join(str(x) for x in w) + "\n")


def blob_sheet(name, nvar, painter, weights=None):
    """47 cells x nvar variant rows, painted from the reduced neighbour mask."""
    im = Image.new("RGBA", (47 * 8, nvar * 8), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for v in range(nvar):
        for ci, mask in enumerate(BLOB_ORDER):
            painter(d, ci * 8, v * 8, mask, v)
    im.save(os.path.join(HERE, name + ".png"))
    write_tileset(name, "assets/%s.png" % name, nvar, BLOB_LUT, 47, weights)


# side/corner helpers: a cell's missing cardinal = a border on that side
def sides(m):
    return (not m & NB_N, not m & NB_E, not m & NB_S, not m & NB_W)


def cell_rng(name, m, v):
    return random.Random("%s/%d/%d" % (name, m, v))


def cw_water(d, x0, y0, m, v):
    """RA-style water: bright per-pixel shimmer. Open sides are CARVED — the
    waterline recedes in a wavy bite (transparent = grass beyond the shore),
    with rock rubble scattered along the actual waterline."""
    W = [(46, 88, 132), (54, 100, 146), (64, 112, 158), (76, 126, 170)]
    SPARK = (112, 158, 196)
    RK = [(38, 34, 30), (58, 52, 44), (82, 74, 62)]
    r2 = cell_rng("w", m, v)
    mn, me, ms, mw = sides(m)

    # shoreline depth along each open side: anchored to 1 at cell borders so
    # neighbouring cells join, bulging to 2-3 mid-cell (the wavy bite)
    def depth(i, salt):
        if i == 0 or i == 7:
            return 1
        h = (i * 7 + salt * 13 + v * 5) % 4
        return 1 + (1 if h >= 2 else 0) + (1 if h == 3 else 0)

    for y in range(8):
        for x in range(8):
            din = 8            # distance inside the water from the carved line
            if mn: din = min(din, y - (depth(x, 0) - 1))
            if ms: din = min(din, (7 - y) - (depth(x, 1) - 1))
            if mw: din = min(din, x - (depth(y, 2) - 1))
            if me: din = min(din, (7 - x) - (depth(y, 3) - 1))
            # convex corners bite deeper
            if mn and mw: din = min(din, (x + y) - 3)
            if mn and me: din = min(din, ((7 - x) + y) - 3)
            if ms and mw: din = min(din, (x + (7 - y)) - 3)
            if ms and me: din = min(din, ((7 - x) + (7 - y)) - 3)
            if din < 0:
                continue                       # beyond the shore: grass
            if din == 0:                       # the waterline: rubble, gaps
                if r2.random() < 0.72:
                    px(d, x0 + x, y0 + y, RK[r2.randrange(3)])
                continue
            h = ((x + v * 3) * 7 + (y + v * 5) * 13 + v * 11) % 16
            px(d, x0 + x, y0 + y, SPARK if h == 0 else W[h % 4])
    # inner corners: a rock fleck where the shore turns
    for (bit, ca, cb, cx, cy) in ((NB_NE, NB_N, NB_E, 7, 0), (NB_SE, NB_S, NB_E, 7, 7),
                                  (NB_SW, NB_S, NB_W, 0, 7), (NB_NW, NB_N, NB_W, 0, 0)):
        if (m & ca) and (m & cb) and not (m & bit):
            px(d, x0 + cx, y0 + cy, RK[1])


BOULDER = [
    ".hh.",
    "hmmd",
    "hmmd",
    ".dd.",
]
BOULDER_S = [
    "hm",
    "md",
]


def cw_rock(d, x0, y0, m, v):
    """edge cells: boulder piles with grass between. INTERIOR cells (rock on
    all four sides) fuse into a solid mountain massif with diagonal ridges."""
    mn, me, ms, mw = sides(m)
    r2 = cell_rng("r", m, v)
    pal = {"h": (118, 114, 122), "m": (92, 88, 96), "d": (58, 54, 62)}
    if m == 255:
        # DEEP interior (rock on all 8 sides): solid mountain mass, lit from NW.
        # v0/v1 craggy slopes; v2 is a rare PEAK cell (weighted low in the tileset).
        if v < 2:
            for y in range(8):
                for x in range(8):
                    b = (x + y + v * 3) % 7          # diagonal relief bands
                    c = (92, 88, 96)
                    if b == 0: c = (120, 116, 126)   # lit ridge line
                    elif b == 1: c = (104, 100, 110)
                    elif b == 4: c = (66, 62, 72)    # shadow slope
                    elif b == 5: c = (54, 50, 60)    # crevice floor
                    if ((x * 5 + y * 3 + v * 11) % 17) == 0: c = (44, 42, 50)
                    px(d, x0 + x, y0 + y, c)
        else:
            for y in range(8):
                for x in range(8):
                    dxp, dyp = x - 3, y - 3          # summit at (3,3)
                    r = dxp * dxp + dyp * dyp
                    if r <= 1: c = (168, 166, 176)   # sunlit crest
                    elif r <= 4: c = (132, 128, 138)
                    elif dxp > 0 and dyp > 0: c = (58, 54, 64)   # SE shade
                    else: c = (96, 92, 100)
                    if ((x * 7 + y * 5) % 13) == 0 and r > 4: c = (74, 70, 80)
                    px(d, x0 + x, y0 + y, c)
        return

    ncard = (not mn) + (not me) + (not ms) + (not mw)   # PRESENT rock cardinals
    if ncard >= 2:
        # touching the solid interior: lay a massif backing that fades toward
        # the opening, THEN scatter the usual boulders on top — blends into
        # the centre without losing the soft rocky rim
        for y in range(8):
            for x in range(8):
                sol = 8
                if mn: sol = min(sol, y)
                if ms: sol = min(sol, 7 - y)
                if mw: sol = min(sol, x)
                if me: sol = min(sol, 7 - x)
                for (bit, ca, cb, cx2, cy2) in ((NB_NE, NB_N, NB_E, 7, 0), (NB_SE, NB_S, NB_E, 7, 7),
                                                (NB_SW, NB_S, NB_W, 0, 7), (NB_NW, NB_N, NB_W, 0, 0)):
                    if (m & ca) and (m & cb) and not (m & bit):
                        sol = min(sol, max(abs(x - cx2), abs(y - cy2)) - 1)
                if sol >= 3:
                    b = (x + y + v * 3) % 7
                    c = (92, 88, 96)
                    if b == 0: c = (120, 116, 126)
                    elif b == 4: c = (66, 62, 72)
                    elif b == 5: c = (54, 50, 60)
                    px(d, x0 + x, y0 + y, c)
                elif sol == 2 and r2.random() < 0.7:
                    px(d, x0 + x, y0 + y, (78, 74, 82))

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

    if v == 0:
        spots = [(2, 2, BOULDER), (6, 5, BOULDER), (6, 1, BOULDER_S), (1, 6, BOULDER_S)]
    else:
        spots = [(5, 2, BOULDER), (2, 6, BOULDER), (7, 6, BOULDER_S), (1, 1, BOULDER_S)]
    for (cx, cy, tmpl) in spots:
        drop = 0.0
        if mn and cy < 3: drop += 0.5
        if ms and cy > 4: drop += 0.5
        if mw and cx < 3: drop += 0.5
        if me and cx > 4: drop += 0.5
        if r2.random() < drop:
            continue
        stamp(tmpl, cx, cy)
    # scree between the boulders
    for _ in range(3):
        px(d, x0 + r2.randrange(8), y0 + r2.randrange(8), (74, 70, 78))


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
    """ore heaps: 2x2 nugget clumps with grass showing between them —
    glittering but not a solid carpet."""
    G = [(112, 86, 18), (150, 116, 24), (190, 150, 34), (226, 188, 52)]
    SPARK = (250, 224, 96)
    mn, me, ms, mw = sides(m)
    r2 = cell_rng("o", m, v)
    for by in range(4):
        for bx in range(4):
            cx, cy = bx * 2, by * 2
            k = 0.62
            if mn and cy < 3: k *= 0.30
            if ms and cy > 4: k *= 0.30
            if mw and cx < 3: k *= 0.30
            if me and cx > 4: k *= 0.30
            if r2.random() > k:
                continue
            base = G[(bx * 31 + by * 17 + v * 7) % 4]
            for oy in range(2):
                for ox in range(2):
                    if r2.random() < 0.18:
                        continue               # ragged heap outline
                    c = base
                    if ox == 0 and oy == 0: c = G[min(3, (G.index(base) + 1))]
                    if ((cx + ox) * 13 + (cy + oy) * 7 + v * 5) % 23 == 0: c = SPARK
                    px(d, x0 + cx + ox, y0 + cy + oy, c)


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
    """readable dirt trail: a solid band along the connection axis (distance
    field to the edge-midpoint->hub centreline), twin ruts as clean offset
    lines that arc around corners. Dithered rim, barely any noise."""
    DIRT = [(102, 85, 54), (94, 77, 49)]
    RUT = (66, 53, 35)
    RIM = (84, 70, 45)
    r2 = cell_rng("rd", m, v)
    hubx = 3.5 + (((v * 7) % 3) - 1) * 0.5
    huby = 3.5 + (((v * 5) % 3) - 1) * 0.5
    segs = []
    if m & NB_N: segs.append((3.5, -0.5, hubx, huby))
    if m & NB_S: segs.append((3.5, 7.5, hubx, huby))
    if m & NB_W: segs.append((-0.5, 3.5, hubx, huby))
    if m & NB_E: segs.append((7.5, 3.5, hubx, huby))
    if not segs:
        segs = [(1.0, 3.5, 6.0, 3.5)]

    def sdist(px_, py_, sg):
        x0s, y0s, x1s, y1s = sg
        vx, vy = x1s - x0s, y1s - y0s
        L2 = vx * vx + vy * vy
        t = 0 if L2 == 0 else max(0.0, min(1.0, ((px_ - x0s) * vx + (py_ - y0s) * vy) / L2))
        dx_, dy_ = px_ - (x0s + vx * t), py_ - (y0s + vy * t)
        return (dx_ * dx_ + dy_ * dy_) ** 0.5

    for y in range(8):
        for x in range(8):
            dmin = min(sdist(x + 0.5, y + 0.5, sg) for sg in segs)
            if dmin <= 2.1:
                if abs(dmin - 1.3) < 0.4 and r2.random() > 0.12:
                    px(d, x0 + x, y0 + y, RUT)      # clean offset ruts, arc at corners
                else:
                    px(d, x0 + x, y0 + y, DIRT[(x * 5 + y * 3 + v) % 2])
            elif dmin <= 2.8:
                if ((x + y) & 1) == 0:
                    px(d, x0 + x, y0 + y, RIM)      # dithered rim into the grass
    for _ in range(2):
        px(d, x0 + r2.randrange(8), y0 + r2.randrange(8), (78, 64, 42))


def make_autotiles():
    blob_sheet("road", 4, cw_road)
    blob_sheet("fog", 2, cw_fog)
    blob_sheet("water", 2, cw_water)
    blob_sheet("rock", 3, cw_rock, weights=(3, 3, 1))
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
    """units.png — 14 cols x 10px cells, 2 team rows (0 blue, 1 red):
    0-6 infantry (rifleA/B/fire, rockA/B, flamA/B)
    7 light hull   8 light turret   9 heavy hull  10 heavy turret
    11 artillery  12 tesla tank    13 harvester
    Vehicles face UP (the game rotates with blit_ex). The old gunship cells are
    gone — the chopper lives in heli.png now."""
    im = Image.new("RGBA", (14 * 10, 2 * 10), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    def track(x0, y0, x, ytop, ybot, team):
        """a 1px-wide track column with tread ticks."""
        for y in range(ytop, ybot + 1):
            px(d, x0 + x, y0 + y, TRACK if (y % 2) else (58, 54, 48))

    for team in range(2):
        base, light, dark = TEAM[team]
        y0 = team * 10
        # --- infantry: 8px templates centred in the 10px cell
        for i, key in enumerate(("rifleA", "rifleB", "rifleF", "rockA", "rockB", "flamA", "flamB")):
            draw_template(d, i * 10 + 1, y0 + 1, INF[key], team)

        # --- 7: light tank hull — 6 wide x 9 long, thin tracks
        x0 = 7 * 10
        track(x0, y0, 2, 1, 8, team); track(x0, y0, 7, 1, 8, team)
        rect(d, x0 + 3, y0 + 1, x0 + 6, y0 + 8, base)
        rect(d, x0 + 3, y0 + 1, x0 + 6, y0 + 1, light)      # glacis
        rect(d, x0 + 3, y0 + 8, x0 + 6, y0 + 8, dark)       # rear
        px(d, x0 + 4, y0 + 7, dark); px(d, x0 + 5, y0 + 7, dark)   # engine deck
        # --- 8: light turret — centred dome + 2px barrel
        x0 = 8 * 10
        rect(d, x0 + 3, y0 + 3, x0 + 6, y0 + 6, light)
        rect(d, x0 + 4, y0 + 4, x0 + 5, y0 + 5, base)
        px(d, x0 + 6, y0 + 6, dark)
        rect(d, x0 + 4, y0 + 0, x0 + 5, y0 + 2, GUN)
        rect(d, x0 + 4, y0 + 0, x0 + 5, y0 + 0, GUNL)
        # --- 9: heavy tank hull — 8 wide x 10 long, thin tracks, plated
        x0 = 9 * 10
        track(x0, y0, 1, 0, 9, team); track(x0, y0, 8, 0, 9, team)
        rect(d, x0 + 2, y0 + 0, x0 + 7, y0 + 9, base)
        rect(d, x0 + 2, y0 + 0, x0 + 7, y0 + 0, light)
        rect(d, x0 + 2, y0 + 9, x0 + 7, y0 + 9, dark)
        rect(d, x0 + 2, y0 + 1, x0 + 2, y0 + 8, light)      # side skirt catch-light
        rect(d, x0 + 3, y0 + 7, x0 + 6, y0 + 7, dark)       # engine louvres
        px(d, x0 + 3, y0 + 9, GUND); px(d, x0 + 6, y0 + 9, GUND)   # exhausts
        # --- 10: heavy turret — big dome + twin barrels
        x0 = 10 * 10
        rect(d, x0 + 3, y0 + 3, x0 + 6, y0 + 7, light)
        rect(d, x0 + 4, y0 + 4, x0 + 5, y0 + 6, base)
        px(d, x0 + 3, y0 + 3, (238, 238, 244)); px(d, x0 + 6, y0 + 7, dark)
        rect(d, x0 + 3, y0 + 0, x0 + 3, y0 + 2, GUN)
        rect(d, x0 + 6, y0 + 0, x0 + 6, y0 + 2, GUN)
        px(d, x0 + 3, y0 + 0, GUNL); px(d, x0 + 6, y0 + 0, GUNL)
        # --- 11: artillery — open chassis, very long gun with a muzzle brake
        x0 = 11 * 10
        track(x0, y0, 2, 3, 9, team); track(x0, y0, 7, 3, 9, team)
        rect(d, x0 + 3, y0 + 4, x0 + 6, y0 + 8, base)
        rect(d, x0 + 3, y0 + 4, x0 + 6, y0 + 4, light)
        rect(d, x0 + 3, y0 + 8, x0 + 6, y0 + 8, dark)
        rect(d, x0 + 4, y0 + 1, x0 + 5, y0 + 4, GUN)        # long barrel
        rect(d, x0 + 3, y0 + 0, x0 + 6, y0 + 0, GUNL)       # muzzle brake
        # --- 12: tesla tank — light hull with a glowing coil
        x0 = 12 * 10
        track(x0, y0, 2, 1, 8, team); track(x0, y0, 7, 1, 8, team)
        rect(d, x0 + 3, y0 + 1, x0 + 6, y0 + 8, base)
        rect(d, x0 + 3, y0 + 1, x0 + 6, y0 + 1, light)
        rect(d, x0 + 3, y0 + 8, x0 + 6, y0 + 8, dark)
        rect(d, x0 + 4, y0 + 3, x0 + 5, y0 + 5, (64, 200, 216))
        px(d, x0 + 4, y0 + 3, (190, 245, 250))              # coil hotspot
        px(d, x0 + 4, y0 + 6, (32, 130, 150))
        # --- 13: harvester — LONG ore truck: scoop, cab, big hopper, exhaust
        x0 = 13 * 10
        track(x0, y0, 1, 1, 9, team); track(x0, y0, 8, 1, 9, team)
        for x in range(2, 8):                                # intake scoop teeth
            px(d, x0 + x, y0 + 0, (150, 150, 160) if (x % 2) else (96, 96, 106))
        rect(d, x0 + 2, y0 + 1, x0 + 7, y0 + 1, (52, 52, 58))   # scoop mouth
        rect(d, x0 + 2, y0 + 2, x0 + 7, y0 + 3, base)        # cab (team)
        rect(d, x0 + 2, y0 + 2, x0 + 7, y0 + 2, light)
        px(d, x0 + 4, y0 + 3, (150, 220, 235)); px(d, x0 + 5, y0 + 3, (150, 220, 235))  # windshield
        rect(d, x0 + 2, y0 + 4, x0 + 7, y0 + 8, (70, 66, 60))    # hopper tray
        rect(d, x0 + 3, y0 + 5, x0 + 6, y0 + 7, (150, 116, 24))  # ore load
        px(d, x0 + 4, y0 + 5, (232, 200, 64)); px(d, x0 + 5, y0 + 6, (200, 160, 32))
        px(d, x0 + 3, y0 + 7, (112, 86, 18))
        rect(d, x0 + 2, y0 + 9, x0 + 7, y0 + 9, dark)        # rear plate
        px(d, x0 + 3, y0 + 9, GUND)                           # exhaust
    im.save(os.path.join(HERE, "units.png"))


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
    """Fictional military insignia: a red star inside an industrial gear ring,
    over a tiny tank column. No star-shading stroke, no tracer streaks."""
    import math
    im = Image.new("RGBA", (60, 60), (30, 34, 26, 255))
    d = ImageDraw.Draw(im)
    r2 = random.Random(7)
    for _ in range(160):        # mottled field
        x, y = r2.randrange(60), r2.randrange(60)
        px(d, x, y, r2.choice(((36, 42, 30), (26, 30, 22), (42, 48, 34))))
    cx, cy = 30, 24
    STEEL, STEELL, STEELD = (78, 84, 74), (104, 112, 100), (44, 48, 42)
    # gear teeth
    for i in range(12):
        a = i * math.pi / 6
        x, y = cx + 20 * math.cos(a), cy + 20 * math.sin(a)
        rect(d, x - 2, y - 2, x + 2, y + 2, STEELD)
    d.ellipse((cx - 18, cy - 18, cx + 18, cy + 18), outline=STEEL + (255,), width=3)
    d.ellipse((cx - 15, cy - 15, cx + 15, cy + 15), outline=STEELL + (255,), width=1)
    d.ellipse((cx - 13, cy - 13, cx + 13, cy + 13), fill=(24, 22, 20, 255))
    # red star, flat fill + black outline (no shading line)
    R, r = 12, 5
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        rad = R if i % 2 == 0 else r
        pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    d.polygon(pts, fill=(188, 44, 36, 255), outline=(16, 12, 10, 255))
    # tiny tank column along the bottom (no muzzle flashes / tracers)
    for tx in (6, 22, 38):
        ty = 47
        rect(d, tx, ty + 2, tx + 9, ty + 6, (52, 58, 46))       # hull
        rect(d, tx, ty + 1, tx + 9, ty + 1, (70, 78, 60))
        rect(d, tx - 1, ty + 5, tx + 10, ty + 7, (34, 36, 30))  # tracks
        rect(d, tx + 3, ty, tx + 6, ty + 2, (70, 78, 60))       # turret
        rect(d, tx + 6, ty + 1, tx + 13, ty + 1, (24, 24, 22))  # barrel
    im.convert("RGB").save(os.path.join(HERE, "..", "icon.png"))


make_autotiles()
make_units()
make_heli()
# buildings.png now comes from extract_buildings.py (AI-art sheet)
make_icon()
print("wrote autotile sheets units.png buildings.png ../icon.png")
