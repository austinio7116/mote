#!/usr/bin/env python3
"""TerraMote tile ART GENERATOR.

Writes the editable tilesheet PNGs (assets/tiles_*.png, assets/wall_*.png) and
their tilesets/*.tileset ruleset sidecars. `mote bake` turns each pair into a
src/<name>.tiles.h MoteAutotile. Re-run after tweaking; or edit the PNGs / rules
directly in Mote Studio's Tiles tab (this script only ADDS/regenerates).

Sheet layouts (8px tiles, row-major grid, variant = whole block of rows):
  blob47   : 8 cols x 6 rows per variant (cells 0..46 in the engine's canonical
             first-seen reduced-mask order, cell 47 unused)
  edge16   : 4 cols x 4 rows (cell = N|E<<1|S<<2|W<<3 ... engine EDGE16 order)
  custom   : explicit cell list + explicit 256-entry LUT (furniture)
"""
import os, math, random
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
TS   = 8   # tile size

# ---------------------------------------------------------------- rule tables
N, NE, E, SE, S, SW, W, NW = 1, 2, 4, 8, 16, 32, 64, 128

def reduce47(m):
    r = m & (N | E | S | W)
    if (m & NE) and (r & N) and (r & E): r |= NE
    if (m & SE) and (r & S) and (r & E): r |= SE
    if (m & SW) and (r & S) and (r & W): r |= SW
    if (m & NW) and (r & N) and (r & W): r |= NW
    return r

def blob47_order():
    """Canonical cell order: reduced masks, first-seen ascending (mote_tile.h)."""
    idx, order = {}, []
    for m in range(256):
        r = reduce47(m)
        if r not in idx:
            idx[r] = len(order); order.append(r)
    return idx, order          # 47 distinct

def blob47_lut():
    idx, _ = blob47_order()
    return [idx[reduce47(m)] for m in range(256)]

def edge16_lut():
    lut = []
    for m in range(256):
        c = 0
        if m & N: c |= 1
        if m & E: c |= 2
        if m & S: c |= 4
        if m & W: c |= 8
        lut.append(c)
    return lut

# ---------------------------------------------------------------- png helpers
def rgb565_snap(c):
    r, g, b = c
    return ((r >> 3) << 3, (g >> 2) << 2, (b >> 3) << 3)

def shade(c, f):
    return rgb565_snap((max(0, min(255, int(c[0] * f))),
                        max(0, min(255, int(c[1] * f))),
                        max(0, min(255, int(c[2] * f)))))

def mix(a, b, t):
    return rgb565_snap(tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3)))

class Cell:
    """An 8x8 RGBA paint target."""
    def __init__(self):
        self.px = [[None] * TS for _ in range(TS)]
    def put(self, x, y, c):
        if 0 <= x < TS and 0 <= y < TS and c is not None:
            self.px[y][x] = rgb565_snap(c)
    def get(self, x, y):
        return self.px[y][x] if 0 <= x < TS and 0 <= y < TS else None
    def fill(self, c):
        for y in range(TS):
            for x in range(TS):
                self.put(x, y, c)

def sheet_from_cells(cells, cols, rows):
    img = Image.new("RGBA", (cols * TS, rows * TS), (0, 0, 0, 0))
    for i, cell in enumerate(cells):
        if cell is None: continue
        ox, oy = (i % cols) * TS, (i // cols) * TS
        for y in range(TS):
            for x in range(TS):
                c = cell.px[y][x]
                if c is not None:
                    img.putpixel((ox + x, oy + y), (c[0], c[1], c[2], 255))
    return img

def stack_variants(sheets):
    w = sheets[0].width; h = sum(s.height for s in sheets)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0)); y = 0
    for s in sheets:
        img.paste(s, (0, y)); y += s.height
    return img

def write_tileset(name, lut, nvar, cols, rows, edge=1, vweight=None):
    vw = vweight or [1] * 8
    p = os.path.join(GAME, "tilesets", name + ".tileset")
    with open(p, "w") as f:
        f.write("sheet assets/%s.png\n" % name)
        f.write("tile %d\ntype 0\nedge %d\nnvar %d\ncols %d\nrows %d\n" % (TS, edge, nvar, cols, rows))
        f.write("lut " + " ".join(str(v) for v in lut) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        f.write("vweight " + " ".join(str(v) for v in vw[:8]) + "\n")

# ---------------------------------------------------------------- materials
RNG = random.Random(1717)

def speckle(cell, base, dark, light, density=0.22, rng=None):
    rng = rng or RNG
    cell.fill(base)
    for y in range(TS):
        for x in range(TS):
            v = rng.random()
            if v < density * 0.5:   cell.put(x, y, dark)
            elif v < density:       cell.put(x, y, light)

def apply_edges(cell, mask, rimc, top_hi=None):
    """1px darker rim on faces NOT connected; inner-corner notch where the
    diagonal is missing but both cardinals are present. Optional top highlight."""
    if not (mask & N):
        for x in range(TS): cell.put(x, 0, rimc)
        if top_hi:
            for x in range(TS):
                if cell.get(x, 1) is not None: cell.put(x, 1, top_hi)
    if not (mask & S):
        for x in range(TS): cell.put(x, TS - 1, rimc)
    if not (mask & W):
        for y in range(TS): cell.put(0, y, rimc)
    if not (mask & E):
        for y in range(TS): cell.put(TS - 1, y, rimc)
    for (dm, cm1, cm2, cx, cy) in ((NW, N, W, 0, 0), (NE, N, E, TS - 1, 0),
                                   (SW, S, W, 0, TS - 1), (SE, S, E, TS - 1, TS - 1)):
        if (mask & cm1) and (mask & cm2) and not (mask & dm):
            cell.put(cx, cy, rimc)
            cell.put(cx + (1 if cx == 0 else -1), cy, rimc)
            cell.put(cx, cy + (1 if cy == 0 else -1), rimc)

def mat_cell(mask, base, dark, light, rim, density=0.22, top_hi=None, seed=0):
    rng = random.Random((mask * 131 + seed) & 0x7FFFFFFF)
    c = Cell()
    speckle(c, base, dark, light, density, rng)
    apply_edges(c, mask, rim, top_hi)
    return c

def grass_cell(mask, seed=0):
    """Dirt body with a grass cap/fringe on every exposed face."""
    DIRT  = (128, 84, 50); DDARK = (100, 62, 36); DLITE = (150, 104, 66)
    GRASS = (58, 160, 60); GDARK = (34, 116, 42); GLITE = (110, 200, 80)
    rng = random.Random((mask * 977 + 7 + seed) & 0x7FFFFFFF)
    c = Cell()
    speckle(c, DIRT, DDARK, DLITE, 0.22, rng)
    def cap_row(y, depth_var=True):
        for x in range(TS):
            c.put(x, y, GRASS if rng.random() > 0.25 else GDARK)
    def cap_col(x):
        for y in range(TS):
            c.put(x, y, GRASS if rng.random() > 0.3 else GDARK)
    if not (mask & N):
        cap_row(0); cap_row(1)
        for x in range(TS):                      # ragged under-edge + blades
            if rng.random() < 0.5: c.put(x, 2, GDARK)
        for x in range(0, TS, 3):
            if rng.random() < 0.8: c.put(x + rng.randint(0, 1), 0, GLITE)
    if not (mask & S):
        cap_row(TS - 1)
        for x in range(TS):
            if rng.random() < 0.4: c.put(x, TS - 2, GDARK)
    if not (mask & W):
        cap_col(0)
        for y in range(TS):
            if rng.random() < 0.4: c.put(1, y, GDARK)
    if not (mask & E):
        cap_col(TS - 1)
        for y in range(TS):
            if rng.random() < 0.4: c.put(TS - 2, y, GDARK)
    for (dm, cm1, cm2, cx, cy) in ((NW, N, W, 0, 0), (NE, N, E, TS - 1, 0),
                                   (SW, S, W, 0, TS - 1), (SE, S, E, TS - 1, TS - 1)):
        if (mask & cm1) and (mask & cm2) and not (mask & dm):
            c.put(cx, cy, GRASS); c.put(cx + (1 if cx == 0 else -1), cy, GDARK)
            c.put(cx, cy + (1 if cy == 0 else -1), GDARK)
    return c

def ore_cell(mask, fleck, fleck_hi, seed=0):
    """Stone base + clustered ore flecks."""
    STONE = (116, 116, 124); SDARK = (86, 86, 96); SLITE = (140, 140, 148)
    rng = random.Random((mask * 313 + 11 + seed) & 0x7FFFFFFF)
    c = Cell()
    speckle(c, STONE, SDARK, SLITE, 0.2, rng)
    for _ in range(2 + (mask % 2)):              # 2-3 clusters
        cx, cy = rng.randint(1, TS - 2), rng.randint(1, TS - 2)
        for _ in range(4):
            x = max(0, min(TS - 1, cx + rng.randint(-1, 1)))
            y = max(0, min(TS - 1, cy + rng.randint(-1, 1)))
            c.put(x, y, fleck)
        c.put(cx, cy, fleck_hi)
    apply_edges(c, mask, SDARK)
    return c

def leaf_cell(mask, seed=0):
    LEAF = (44, 128, 48); LDARK = (26, 92, 34); LLITE = (92, 172, 66)
    rng = random.Random((mask * 517 + 3 + seed) & 0x7FFFFFFF)
    c = Cell()
    speckle(c, LEAF, LDARK, LLITE, 0.35, rng)
    apply_edges(c, mask, LDARK)
    # scalloped exposed edges: knock out corner pixels for a leafy silhouette
    if not (mask & N):
        for x in (0, 3, 6):
            if rng.random() < 0.6: c.px[0][x] = None
    if not (mask & S):
        for x in (1, 4, 7):
            if rng.random() < 0.6: c.px[TS - 1][x] = None
    if not (mask & W):
        for y in (1, 5):
            if rng.random() < 0.5: c.px[y][0] = None
    if not (mask & E):
        for y in (2, 6):
            if rng.random() < 0.5: c.px[y][TS - 1] = None
    return c

def blob_sheet(name, cellfn, nvar=1):
    _, order = blob47_order()
    sheets = []
    for v in range(nvar):
        cells = [cellfn(m, v * 1000) for m in order] + [None]   # 47 + 1 pad
        sheets.append(sheet_from_cells(cells, 8, 6))
    img = stack_variants(sheets)
    img.save(os.path.join(HERE, name + ".png"))
    write_tileset(name, blob47_lut(), nvar, 8, 6 * nvar)
    print("[tiles]", name, img.size, "nvar", nvar)

# ---------------------------------------------------------------- solid tiles
def make_terrain():
    blob_sheet("tiles_dirt",  lambda m, s: mat_cell(m, (128, 84, 50), (100, 62, 36), (150, 104, 66), (84, 52, 30), seed=s), nvar=2)
    blob_sheet("tiles_grass", lambda m, s: grass_cell(m, s), nvar=2)
    blob_sheet("tiles_stone", lambda m, s: mat_cell(m, (116, 116, 124), (86, 86, 96), (140, 140, 148), (66, 66, 76), 0.2, (150, 150, 158), s), nvar=2)
    blob_sheet("tiles_wood",  lambda m, s: wood_cell(m, s))
    blob_sheet("tiles_sand",  lambda m, s: mat_cell(m, (212, 192, 116), (188, 164, 92), (232, 216, 148), (160, 138, 72), 0.18, seed=s), nvar=2)
    blob_sheet("tiles_snow",  lambda m, s: mat_cell(m, (222, 232, 244), (192, 205, 224), (244, 250, 255), (164, 180, 205), 0.15, seed=s), nvar=2)
    blob_sheet("tiles_ebon",  lambda m, s: mat_cell(m, (104, 88, 128), (76, 62, 98), (128, 112, 152), (54, 42, 74), 0.24, seed=s))
    blob_sheet("tiles_clay",  lambda m, s: mat_cell(m, (172, 106, 74), (146, 84, 56), (194, 128, 92), (118, 66, 44), 0.12, seed=s))
    blob_sheet("tiles_copper", lambda m, s: ore_cell(m, (198, 112, 42), (238, 160, 84), s))
    blob_sheet("tiles_iron",   lambda m, s: ore_cell(m, (166, 146, 130), (210, 196, 186), s))
    blob_sheet("tiles_gold",   lambda m, s: ore_cell(m, (222, 178, 32), (252, 224, 100), s))
    blob_sheet("tiles_demonite", lambda m, s: ore_cell(m, (108, 62, 178), (162, 112, 235), s))
    blob_sheet("tiles_ash",   lambda m, s: mat_cell(m, (74, 66, 66), (56, 48, 50), (94, 84, 82), (40, 34, 36), 0.25, seed=s), nvar=2)
    blob_sheet("tiles_hellstone", lambda m, s: hellstone_cell(m, s))
    blob_sheet("tiles_obsidian",  lambda m, s: mat_cell(m, (46, 38, 66), (30, 24, 46), (84, 66, 120), (20, 16, 32), 0.18, seed=s))
    blob_sheet("tiles_leaf",  lambda m, s: leaf_cell(m, s), nvar=2)

def wood_cell(mask, seed=0):
    BASE = (168, 122, 68); DARK = (136, 96, 50); LINE = (110, 76, 40); LITE = (188, 144, 86)
    rng = random.Random((mask * 71 + seed) & 0x7FFFFFFF)
    c = Cell(); c.fill(BASE)
    for y in range(TS):
        if y % 4 == 3:
            for x in range(TS): c.put(x, y, LINE)
        else:
            for x in range(TS):
                if rng.random() < 0.12: c.put(x, y, DARK if rng.random() < 0.6 else LITE)
    # staggered vertical plank joints
    for y0 in (0, 4):
        jx = (3 + (mask + y0) * 2) % TS
        for y in range(y0, min(TS, y0 + 3)): c.put(jx, y, LINE)
    apply_edges(c, mask, (92, 62, 32), (196, 152, 94))
    return c

def hellstone_cell(mask, seed=0):
    BASE = (96, 42, 36); DARK = (66, 26, 26); GLOW = (240, 120, 24); GLO2 = (255, 190, 60)
    rng = random.Random((mask * 419 + seed) & 0x7FFFFFFF)
    c = Cell()
    speckle(c, BASE, DARK, (128, 62, 48), 0.2, rng)
    x, y = rng.randint(1, 6), rng.randint(1, 6)          # a wandering magma vein
    for _ in range(7):
        c.put(x, y, GLOW)
        if rng.random() < 0.3: c.put(x, y, GLO2)
        x = max(0, min(TS - 1, x + rng.randint(-1, 1)))
        y = max(0, min(TS - 1, y + rng.randint(-1, 1)))
    apply_edges(c, mask, DARK)
    return c

# ---------------------------------------------------------------- tree parts
def trunk_cell(idx, seed=0):
    """edge16 cell: bark column. bit1=N connected, bit4=S connected."""
    BARK = (124, 88, 54); BDARK = (94, 64, 38); BLITE = (150, 112, 72); RIM = (72, 48, 28)
    n = idx & 1; s = (idx >> 2) & 1
    rng = random.Random(idx * 37 + 5 + seed)
    c = Cell()
    for y in range(TS):
        for x in range(1, TS - 1):
            r = rng.random()
            c.put(x, y, BDARK if r < 0.18 else (BLITE if r > 0.88 else BARK))
    for y in range(TS):
        c.put(1, y, BDARK); c.put(0, y, None); c.put(TS - 1, y, None)
        c.put(TS - 2, y, BDARK if rng.random() < 0.7 else BARK)
    # vertical bark grooves
    for x in (3, 5):
        for y in range(TS):
            if rng.random() < 0.45: c.put(x, y, BDARK)
    if not n:                                            # cut top / crown base
        for x in range(1, TS - 1): c.put(x, 0, RIM)
    if not s:                                            # base: root flare
        for x in range(TS): c.put(x, TS - 1, BDARK)
        c.put(0, TS - 1, BARK); c.put(0, TS - 2, BDARK)
        c.put(TS - 1, TS - 1, BARK); c.put(TS - 1, TS - 2, BDARK)
    return c

def make_trunk():
    cells = [trunk_cell(i) for i in range(16)]
    img = sheet_from_cells(cells, 4, 4)
    img.save(os.path.join(HERE, "tiles_trunk.png"))
    write_tileset("tiles_trunk", edge16_lut(), 1, 4, 4)
    print("[tiles] tiles_trunk", img.size)

# ---------------------------------------------------------------- furniture
def custom_sheet(name, cells, cols, lut, edge=0, nvar=1):
    rows = (len(cells) + cols - 1) // cols
    img = sheet_from_cells(cells, cols, rows)
    img.save(os.path.join(HERE, name + ".png"))
    write_tileset(name, lut, nvar, cols, rows, edge=edge)
    print("[tiles]", name, img.size)

def lut_from(fn):
    return [fn(m) for m in range(256)]

def torch_cell():
    c = Cell()
    STICK = (150, 108, 60)
    for y in range(3, 8): c.put(3, y, STICK); c.put(4, y, shade(STICK, 0.8))
    c.put(3, 2, (255, 200, 60)); c.put(4, 2, (255, 160, 30))
    c.put(3, 1, (255, 240, 160)); c.put(4, 1, (255, 200, 60))
    c.put(3, 0, (255, 120, 20))
    return c

def platform_cells():
    PLANK = (172, 126, 70); DARK = (128, 90, 48); LINE = (104, 72, 38)
    out = []
    for kind in range(4):        # 0 solo, 1 left end, 2 middle, 3 right end
        c = Cell()
        for y in (2, 3):
            for x in range(TS):
                c.put(x, y, PLANK if y == 2 else DARK)
        for x in range(TS):
            if x % 3 == 1: c.put(x, 3, LINE)
        if kind in (0, 1):
            c.put(0, 2, LINE); c.put(0, 3, LINE); c.put(0, 4, DARK)
        if kind in (0, 3):
            c.put(TS - 1, 2, LINE); c.put(TS - 1, 3, LINE); c.put(TS - 1, 4, DARK)
        out.append(c)
    return out

def bench_cells(top, leg):
    """2-wide table: left / right halves."""
    out = []
    for half in range(2):
        c = Cell()
        for x in range(TS):
            c.put(x, 1, top); c.put(x, 2, shade(top, 0.75))
        lx = 1 if half == 0 else TS - 2
        for y in range(3, TS):
            c.put(lx, y, leg); c.put(lx + (1 if half == 0 else -1), y, shade(leg, 0.7))
        out.append(c)
    return out

def anvil_cells():
    IRON = (110, 114, 126); DARK = (72, 76, 88); LITE = (152, 156, 168)
    out = []
    for half in range(2):
        c = Cell()
        for x in range(TS):
            c.put(x, 2, LITE); c.put(x, 3, IRON)
        if half == 0:
            c.put(0, 2, DARK); c.put(1, 4, IRON); c.put(1, 5, DARK)
            for x in range(2, TS): c.put(x, 4, DARK)
            for x in range(2, 6): c.put(x, 5, IRON); c.put(x, 6, IRON); c.put(x, 7, DARK)
        else:
            c.put(TS - 1, 2, DARK)
            for x in range(0, 5): c.put(x, 4, DARK)
            for x in range(1, 5): c.put(x, 5, IRON); c.put(x, 6, IRON); c.put(x, 7, DARK)
            c.put(TS - 1, 3, DARK)
        out.append(c)
    return out

def furnace_cells():
    STONE = (104, 100, 100); SDARK = (70, 66, 68); FIRE = (255, 150, 30); FIR2 = (255, 220, 90)
    out = []
    for i in range(4):           # TL TR BL BR
        c = Cell()
        rng = random.Random(900 + i)
        speckle(c, STONE, SDARK, (128, 124, 122), 0.25, rng)
        top, left = i < 2, (i % 2) == 0
        if top:
            for x in range(TS): c.put(x, 0, SDARK)
            if left: c.put(2, 0, (40, 36, 36)); c.put(2, 1, (60, 52, 50))   # chimney hint
        else:
            for x in range(TS): c.put(x, TS - 1, SDARK)
            # mouth with fire spans the two bottom cells' inner halves
            xs = range(3, TS) if left else range(0, 5)
            for x in xs:
                c.put(x, 3, SDARK); c.put(x, 4, FIRE); c.put(x, 5, FIR2 if (x % 2) else FIRE)
                c.put(x, 6, SDARK)
        for y in range(TS):
            if left: c.put(0, y, SDARK)
            else:    c.put(TS - 1, y, SDARK)
        out.append(c)
    return out

def chest_cells():
    WOODC = (178, 126, 64); WDARK = (132, 92, 46); GOLDC = (240, 200, 70); RIM = (96, 66, 34)
    out = []
    for i in range(4):
        c = Cell()
        top, left = i < 2, (i % 2) == 0
        for y in range(TS):
            for x in range(TS):
                c.put(x, y, WOODC if (y % 3) else WDARK)
        if top:
            for x in range(TS): c.put(x, 0, None); c.put(x, 1, RIM)
            for x in range(TS): c.put(x, 4, GOLDC)                       # metal band
            if not left: c.put(0, 5, GOLDC); c.put(0, 6, shade(GOLDC, 0.7))
            if left: c.put(TS - 1, 5, GOLDC); c.put(TS - 1, 6, shade(GOLDC, 0.7))  # clasp center
        else:
            for x in range(TS): c.put(x, TS - 1, RIM)
        for y in range(1 if top else 0, TS):
            if left: c.put(0, y, RIM if y > 0 else None)
            else:    c.put(TS - 1, y, RIM if y > 0 else None)
        out.append(c)
    return out

def altar_cells():
    ROCK = (92, 62, 110); RDARK = (62, 40, 78); GLOW = (196, 90, 220)
    out = []
    for i in range(4):
        c = Cell()
        rng = random.Random(1300 + i)
        top, left = i < 2, (i % 2) == 0
        speckle(c, ROCK, RDARK, (120, 86, 140), 0.3, rng)
        if top:
            for x in range(TS): c.put(x, 0, None)
            for x in range(2 if left else 0, TS if left else 6): c.put(x, 1, RDARK)
            c.put(5 if left else 2, 0, GLOW); c.put(5 if left else 2, 1, GLOW)   # spikes
            c.put(2 if left else 5, 2, GLOW)
        else:
            for x in range(TS): c.put(x, TS - 1, RDARK)
        out.append(c)
    return out

def door_cells(open_):
    WOODC = (170, 122, 66); WDARK = (128, 88, 46); RIM = (92, 62, 32); KNOB = (240, 200, 80)
    out = []
    for i in range(3):           # top / mid / bottom
        c = Cell()
        if not open_:
            for y in range(TS):
                for x in range(1, TS - 1):
                    c.put(x, y, WOODC if ((y + x) % 4) else WDARK)
            for y in range(TS): c.put(1, y, RIM); c.put(TS - 2, y, RIM)
            if i == 0:
                for x in range(1, TS - 1): c.put(x, 0, RIM)
            if i == 2:
                for x in range(1, TS - 1): c.put(x, TS - 1, RIM)
            if i == 1: c.put(TS - 3, 3, KNOB)
        else:                    # open: thin frame at the left edge
            for y in range(TS):
                c.put(0, y, RIM); c.put(1, y, WOODC); c.put(2, y, WDARK)
            if i == 0: c.put(1, 0, RIM); c.put(2, 0, RIM)
            if i == 2: c.put(1, TS - 1, RIM); c.put(2, TS - 1, RIM)
            if i == 1: c.put(2, 3, KNOB)
        out.append(c)
    return out

def mush_cell():
    c = Cell()
    CAP = (94, 220, 255); CAPD = (52, 150, 210); STEM = (200, 214, 230)
    for x in range(1, 7): c.put(x, 3, CAP)
    for x in range(2, 6): c.put(x, 2, CAP)
    c.put(3, 1, CAPD); c.put(4, 1, CAPD); c.put(2, 3, CAPD); c.put(6, 3, CAPD)
    for y in range(4, 8): c.put(3, y, STEM); c.put(4, y, shade(STEM, 0.8))
    return c

def flower_cell(kind):
    c = Cell()
    rng = random.Random(40 + kind)
    STEMC = (52, 140, 54)
    petals = [(240, 90, 90), (250, 210, 80), (110, 140, 250)][kind % 3]
    for y in range(4, 8): c.put(3, y, STEMC)
    c.put(2, 5, STEMC); c.put(4, 6, STEMC)
    for dx, dy in ((0, -1), (-1, 0), (1, 0), (0, 1)):
        c.put(3 + dx, 2 + dy, petals)
    c.put(3, 2, (255, 240, 160))
    if kind == 3:                # tall grass blades instead
        c2 = Cell()
        for x in (1, 3, 5, 7):
            h = rng.randint(3, 6)
            for y in range(TS - h, TS): c2.put(x, y, STEMC if y % 2 else (90, 190, 80))
        return c2
    return c

def sapling_cell():
    c = Cell()
    for y in range(3, 8): c.put(3, y, (124, 88, 54))
    for dx, dy in ((-1, -1), (0, -2), (1, -1), (0, -1)):
        c.put(3 + dx, 3 + dy, (70, 170, 60))
    return c

def make_furniture():
    single = lut_from(lambda m: 0)
    custom_sheet("tiles_torch", [torch_cell()], 1, single)
    custom_sheet("tiles_platform", platform_cells(), 4,
                 lut_from(lambda m: {0: 0, E: 1, W: 3, E | W: 2}[(m & (E | W))]))
    lut2w = lut_from(lambda m: 1 if (m & W) else 0)
    custom_sheet("tiles_workbench", bench_cells((176, 128, 72), (128, 90, 48)), 2, lut2w)
    custom_sheet("tiles_anvil", anvil_cells(), 2, lut2w)
    lut22 = lut_from(lambda m: (0 if (m & S) else 2) + (1 if (m & W) else 0))
    custom_sheet("tiles_furnace", furnace_cells(), 2, lut22)
    custom_sheet("tiles_chest", chest_cells(), 2, lut22)
    custom_sheet("tiles_altar", altar_cells(), 2, lut22)
    lut13 = lut_from(lambda m: 1 if (m & N and m & S) else (0 if (m & S) else 2))
    custom_sheet("tiles_door_c", door_cells(False), 1, lut13)
    custom_sheet("tiles_door_o", door_cells(True), 1, lut13)
    custom_sheet("tiles_mush", [mush_cell()], 1, single)
    custom_sheet("tiles_flower", [flower_cell(k) for k in range(4)], 1, single, nvar=4)
    custom_sheet("tiles_sapling", [sapling_cell()], 1, single)

# ---------------------------------------------------------------- walls
def make_walls():
    def wall(name, base, dark, light, density=0.2):
        # pre-darkened so the background layer needs no runtime shading
        f = 0.52
        blob_sheet(name, lambda m, s: mat_cell(m, shade(base, f), shade(dark, f),
                                               shade(light, f), shade(dark, f * 0.7),
                                               density, seed=s))
    wall("wall_dirt",  (128, 84, 50), (100, 62, 36), (150, 104, 66))
    wall("wall_stone", (116, 116, 124), (86, 86, 96), (140, 140, 148))
    wall("wall_wood",  (168, 122, 68), (136, 96, 50), (188, 144, 86), 0.12)
    wall("wall_ebon",  (104, 88, 128), (76, 62, 98), (128, 112, 152))
    wall("wall_ash",   (74, 66, 66), (56, 48, 50), (94, 84, 82))
    wall("wall_snow",  (200, 212, 228), (170, 184, 205), (226, 236, 248), 0.15)



# ---------------------------------------------------------------- grass caps
def make_grass_caps():
    """Cosmetic caps drawn over exposed dirt (assets/grass_cap.png).
    cells 8x8: 0 top cap, 1 top-left corner, 2 top-right corner, 3 left side,
    4 right side, 5 solo island; row 1 = same in corruption purple."""
    import random as _r
    def build(g, gd, gl):
        cells = []
        for kind in range(6):
            rng = _r.Random(60 + kind)
            c = Cell()
            def top():
                for x in range(TS):
                    c.put(x, 0, g if rng.random() > 0.25 else gd)
                    c.put(x, 1, g if rng.random() > 0.35 else gd)
                    if rng.random() < 0.55: c.put(x, 2, gd)
                for x in range(0, TS, 3):
                    if rng.random() < 0.8: c.put(x + rng.randint(0, 1), 0, gl)
            def left():
                for y in range(TS):
                    c.put(0, y, g if rng.random() > 0.3 else gd)
                    if rng.random() < 0.5: c.put(1, y, gd)
            def right():
                for y in range(TS):
                    c.put(TS - 1, y, g if rng.random() > 0.3 else gd)
                    if rng.random() < 0.5: c.put(TS - 2, y, gd)
            if kind == 0: top()
            elif kind == 1: top(); left()
            elif kind == 2: top(); right()
            elif kind == 3: left()
            elif kind == 4: right()
            else: top(); left(); right()
            cells.append(c)
        return cells
    green = build((58, 160, 60), (34, 116, 42), (110, 200, 80))
    purple = build((150, 90, 190), (108, 58, 140), (190, 140, 230))
    img = stack_variants([sheet_from_cells(green, 6, 1), sheet_from_cells(purple, 6, 1)])
    img.save(os.path.join(HERE, "grass_cap.png"))
    with open(os.path.join(HERE, "grass_cap.sheet"), "w") as f:
        f.write("cell %d %d\n" % (TS, TS))
    print("[tiles] grass_cap.png")

if __name__ == "__main__":
    os.makedirs(os.path.join(GAME, "tilesets"), exist_ok=True)
    make_terrain()
    make_trunk()
    make_furniture()
    make_walls()
    make_grass_caps()
    print("done")
