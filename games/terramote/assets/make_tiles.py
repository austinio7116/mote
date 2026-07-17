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

_SLIP = HERE   # furniture source PNGs live in assets/ (was the SlipPixel/ folder)
def slip_cells(fname, cols, rows, x0=0, y0=0):
    """Slice an 8px-cell block out of a hand-drawn SlipPixel PNG into Cells
    (row-major). Alpha < 128 -> transparent (shows the wall behind). Lets the
    community re-edit the source PNGs and just re-run this script."""
    im = Image.open(os.path.join(_SLIP, fname)).convert("RGBA")
    out = []
    for cy in range(rows):
        for cx in range(cols):
            c = Cell()
            for y in range(TS):
                for x in range(TS):
                    px = im.getpixel((x0 + cx * TS + x, y0 + cy * TS + y))
                    if px[3] >= 128:
                        c.put(x, y, (px[0], px[1], px[2]))
            out.append(c)
    return out

def slip_cells_scaled(fname, box, cols, rows):
    """Crop a region of a SlipPixel PNG and nearest-scale it to cols*8 x rows*8,
    then slice into Cells. Used to fit a wider drawing into our tile footprint."""
    im = Image.open(os.path.join(_SLIP, fname)).convert("RGBA")
    im = im.crop(box).resize((cols * TS, rows * TS), Image.NEAREST)
    out = []
    for cy in range(rows):
        for cx in range(cols):
            c = Cell()
            for y in range(TS):
                for x in range(TS):
                    px = im.getpixel((cx * TS + x, cy * TS + y))
                    if px[3] >= 128:
                        c.put(x, y, (px[0], px[1], px[2]))
            out.append(c)
    return out

def stack_variants(sheets):
    w = sheets[0].width; h = sum(s.height for s in sheets)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0)); y = 0
    for s in sheets:
        img.paste(s, (0, y)); y += s.height
    return img

def write_tileset(name, lut, nvar, cols, rows, edge=1, vweight=None, tpl=0):
    # tpl: 0 BLOB47 (corner-aware terrain) · 1 EDGE16 (cardinal) · 2 NINESLICE · 3 WANG16.
    # Custom-LUT furniture uses EDGE16 so Studio shows a 4x4 rule grid (not blob47's 47).
    vw = vweight or [1] * 8
    p = os.path.join(GAME, "tilesets", name + ".tileset")
    with open(p, "w") as f:
        f.write("sheet assets/%s.png\n" % name)
        f.write("tile %d\ntype %d\nedge %d\nnvar %d\ncols %d\nrows %d\n" % (TS, tpl, edge, nvar, cols, rows))
        f.write("lut " + " ".join(str(v) for v in lut) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        f.write("vweight " + " ".join(str(v) for v in vw[:8]) + "\n")

# ---------------------------------------------------------------- materials
RNG = random.Random(1717)

def clumps(cell, rng, col, n, size=3, only_base=None):
    """Organic 1..size-px blobs instead of uniform speckle."""
    for _ in range(n):
        x, y = rng.randint(0, TS - 1), rng.randint(0, TS - 1)
        for _ in range(rng.randint(1, size)):
            if only_base is None or cell.get(x, y) == only_base:
                cell.put(x, y, col)
            x = max(0, min(TS - 1, x + rng.randint(-1, 1)))
            y = max(0, min(TS - 1, y + rng.randint(-1, 1)))

def shade_ramp(cell):
    """(disabled) per-cell ramps read as corduroy banding when tiled."""
    _ = cell

def speckle(cell, base, dark, light, density=0.22, rng=None):
    """v2: clumped two-tone texture + top-lit ramp (name kept for callers)."""
    rng = rng or RNG
    cell.fill(base)
    n = max(2, int(density * 30))
    clumps(cell, rng, dark, n)
    clumps(cell, rng, light, max(1, n - 1))
    shade_ramp(cell)

def apply_edges(cell, mask, rimc, top_hi=None):
    """v2 lit edges: bright sun-catch on exposed tops, deep shadow below,
    medium sides, darkened outer corners, notched inner corners."""
    base = cell.get(3, 3) or rimc
    hi   = top_hi or mix(base, (255, 255, 255), 0.45)
    hi2  = mix(base, (255, 255, 255), 0.2)
    lo   = shade(base, 0.5)
    lo2  = shade(base, 0.72)
    side = shade(base, 0.62)
    if not (mask & N):
        for x in range(TS): cell.put(x, 0, hi)
        for x in range(0, TS, 2): cell.put(x + (x // 2) % 2, 1, hi2)
    if not (mask & S):
        for x in range(TS): cell.put(x, TS - 1, lo)
        for x in range(1, TS, 3): cell.put(x, TS - 2, lo2)
    if not (mask & W):
        for y in range(TS): cell.put(0, y, side)
    if not (mask & E):
        for y in range(TS): cell.put(TS - 1, y, side)
    if not (mask & N) and not (mask & W): cell.put(0, 0, mix(hi, side, 0.5))
    if not (mask & N) and not (mask & E): cell.put(TS - 1, 0, mix(hi, side, 0.5))
    if not (mask & S) and not (mask & W): cell.put(0, TS - 1, shade(base, 0.42))
    if not (mask & S) and not (mask & E): cell.put(TS - 1, TS - 1, shade(base, 0.42))
    for (dm, cm1, cm2, cx, cy) in ((NW, N, W, 0, 0), (NE, N, E, TS - 1, 0),
                                   (SW, S, W, 0, TS - 1), (SE, S, E, TS - 1, TS - 1)):
        if (mask & cm1) and (mask & cm2) and not (mask & dm):
            cell.put(cx, cy, lo)
            cell.put(cx + (1 if cx == 0 else -1), cy, lo2)
            cell.put(cx, cy + (1 if cy == 0 else -1), lo2)

def mat_cell(mask, base, dark, light, rim, density=0.22, top_hi=None, seed=0):
    rng = random.Random((mask * 131 + seed) & 0x7FFFFFFF)
    c = Cell()
    speckle(c, base, dark, light, density, rng)
    apply_edges(c, mask, rim, top_hi)
    return c

def stone_cell(mask, base, dark, light, seed=0, cracks=True):
    """Rocky: clumped tone + short crack polylines + light chips."""
    rng = random.Random((mask * 761 + 5 + seed) & 0x7FFFFFFF)
    c = Cell()
    c.fill(base)
    clumps(c, rng, shade(base, 0.86), 6, 4)
    clumps(c, rng, shade(base, 0.93), 4, 4)
    clumps(c, rng, light, 3, 2)
    if cracks:
        for _ in range(rng.randint(1, 2)):
            x, y = rng.randint(1, 6), rng.randint(1, 6)
            for _ in range(rng.randint(3, 5)):
                c.put(x, y, dark)
                x = max(0, min(TS - 1, x + rng.choice((-1, 0, 1))))
                y = max(0, min(TS - 1, y + rng.choice((0, 1))))
        c.put(rng.randint(1, 6), rng.randint(1, 6), light)
    shade_ramp(c)
    apply_edges(c, mask, dark)
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
    """Stone base + 2x2 metallic nuggets (highlight top-left, shadow bottom-right)."""
    STONE = (118, 118, 126); SDARK = (84, 84, 94); SLITE = (142, 142, 150)
    rng = random.Random((mask * 313 + 11 + seed) & 0x7FFFFFFF)
    c = Cell()
    c.fill(STONE)
    clumps(c, rng, shade(STONE, 0.85), 3, 3)
    fleck_sh = shade(fleck, 0.55)
    for _ in range(2 + (mask % 2)):
        x, y = rng.randint(1, TS - 3), rng.randint(1, TS - 3)
        c.put(x, y, fleck_hi); c.put(x + 1, y, fleck)
        c.put(x, y + 1, fleck); c.put(x + 1, y + 1, fleck_sh)
        if rng.random() < 0.5: c.put(x + 2, y + rng.randint(0, 1), fleck)
    shade_ramp(c)
    apply_edges(c, mask, SDARK)
    return c

def leaf_cell(mask, seed=0):
    LEAF = (46, 126, 50); LDARK = (28, 90, 36); LLITE = (96, 176, 66); LSUN = (140, 210, 90)
    rng = random.Random((mask * 517 + 3 + seed) & 0x7FFFFFFF)
    c = Cell()
    c.fill(LEAF)
    clumps(c, rng, LDARK, 4, 3)
    clumps(c, rng, LLITE, 3, 3)
    apply_edges(c, mask, LDARK, top_hi=LSUN)
    # scalloped exposed edges + sunlit top sprigs
    if not (mask & N):
        for x in (0, 3, 6):
            if rng.random() < 0.6: c.px[0][x] = None
        for x in range(TS):
            if c.get(x, 0) is not None and rng.random() < 0.5: c.put(x, 0, LSUN)
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

def roof_cell(mask, seed=0):
    """Sloped shingles: horizontal shingle courses, and the exposed upper
    corners are cut DIAGONALLY — a staircase of roof tiles renders as a smooth
    45-degree roof face automatically."""
    BASE = (134, 60, 44); DARK = (94, 40, 30); LITE = (170, 90, 58); CAP = (204, 128, 78)
    rng = random.Random((mask * 419 + 9 + seed) & 0x7FFFFFFF)
    c = Cell()
    c.fill(BASE)
    for y in range(TS):                          # shingle courses every 3 rows
        for x in range(TS):
            if y % 3 == 2: c.put(x, y, DARK)
            elif (x + (y // 3) * 4) % 8 == 0: c.put(x, y, DARK)   # staggered joints
            elif y % 3 == 0 and rng.random() < 0.18: c.put(x, y, LITE)
    up, dn = not (mask & N), not (mask & S)
    lf, rt = not (mask & W), not (mask & E)
    if up:                                        # ridge cap rows
        for x in range(TS): c.put(x, 0, CAP)
        for x in range(TS): c.put(x, 1, LITE)
    if dn:                                        # eave shadow
        for x in range(TS): c.put(x, TS - 1, DARK)
    if lf:
        for y in range(TS): c.put(0, y, DARK)
    if rt:
        for y in range(TS): c.put(TS - 1, y, DARK)
    return c

def blob_sheet(name, cellfn, nvar=1, extra=None, classify=None):
    """47-blob sheet; `extra` cells (appended after the 48-slot grid) are
    selected by `classify(mask) -> extra index or None` — raw 256-mask
    overrides, so rules alone can express shapes blob47 can't (slopes)."""
    _, order = blob47_order()
    sheets = []
    for v in range(nvar):
        cells = [cellfn(m, v * 1000) for m in order] + [None]   # 47 + 1 pad
        sheets.append(sheet_from_cells(cells, 8, 6))
    img = stack_variants(sheets)
    lut = blob47_lut()
    rows = 6 * nvar
    if extra:
        assert nvar == 1, "extra cells only supported for nvar=1"
        n = len(extra)
        erows = (n + 7) // 8
        eimg = sheet_from_cells(list(extra) + [None] * (erows * 8 - n), 8, erows)
        both = Image.new("RGBA", (img.width, img.height + eimg.height), (0, 0, 0, 0))
        both.paste(img, (0, 0)); both.paste(eimg, (0, img.height))
        img = both
        for m in range(256):
            k = classify(m)
            if k is not None: lut[m] = 48 + k
        rows = 6 + erows
    img.save(os.path.join(HERE, name + ".png"))
    write_tileset(name, lut, nvar, 8, rows)
    print("[tiles]", name, img.size, "nvar", nvar, "extra", len(extra) if extra else 0)

# ---------------------------------------------------------------- solid tiles
def make_terrain():
    blob_sheet("tiles_dirt",  lambda m, s: mat_cell(m, (120, 80, 46), (88, 56, 30), (146, 100, 60), (78, 50, 28), 0.30, seed=s), nvar=2)
    blob_sheet("tiles_grass", lambda m, s: grass_cell(m, s), nvar=2)
    blob_sheet("tiles_stone", lambda m, s: stone_cell(m, (118, 118, 126), (72, 72, 84), (148, 148, 156), s), nvar=3)
    blob_sheet("tiles_wood",  lambda m, s: wood_cell(m, s))
    blob_sheet("tiles_sand",  lambda m, s: mat_cell(m, (212, 192, 116), (188, 164, 92), (232, 216, 148), (160, 138, 72), 0.18, seed=s), nvar=2)
    blob_sheet("tiles_snow",  lambda m, s: mat_cell(m, (222, 232, 244), (192, 205, 224), (244, 250, 255), (164, 180, 205), 0.15, seed=s), nvar=2)
    blob_sheet("tiles_ebon",  lambda m, s: stone_cell(m, (102, 86, 128), (62, 48, 84), (132, 114, 158), s), nvar=2)
    blob_sheet("tiles_clay",  lambda m, s: clay_cell(m, s), nvar=2)
    blob_sheet("tiles_copper", lambda m, s: ore_cell(m, (198, 112, 42), (238, 160, 84), s))
    blob_sheet("tiles_iron",   lambda m, s: ore_cell(m, (166, 146, 130), (210, 196, 186), s))
    blob_sheet("tiles_gold",   lambda m, s: ore_cell(m, (222, 178, 32), (252, 224, 100), s))
    blob_sheet("tiles_demonite", lambda m, s: ore_cell(m, (108, 62, 178), (162, 112, 235), s))
    blob_sheet("tiles_ash",   lambda m, s: mat_cell(m, (74, 66, 66), (56, 48, 50), (94, 84, 82), (40, 34, 36), 0.25, seed=s), nvar=2)
    blob_sheet("tiles_hellstone", lambda m, s: hellstone_cell(m, s))
    blob_sheet("tiles_obsidian",  lambda m, s: stone_cell(m, (48, 40, 70), (26, 20, 40), (96, 74, 138), s))
    blob_sheet("tiles_leaf",  lambda m, s: leaf_cell(m, s), nvar=2)

def wood_cell(mask, seed=0):
    # warm GOLD planks with strong horizontal grain lines (distinct from dirt/clay)
    BASE = (182, 138, 78); DARK = (138, 98, 50); LINE = (100, 64, 32); LITE = (212, 172, 108)
    rng = random.Random((mask * 71 + seed) & 0x7FFFFFFF)
    c = Cell(); c.fill(BASE)
    for y in range(TS):
        if y % 4 == 3:
            for x in range(TS): c.put(x, y, LINE)              # dark plank seam
        elif y % 4 == 0:
            for x in range(TS): c.put(x, y, LITE)              # sun-catch top of plank
        else:
            for x in range(TS):
                if rng.random() < 0.14: c.put(x, y, DARK)      # grain flecks
    for y0 in (0, 4):                                          # staggered vertical joints
        jx = (3 + (mask + y0) * 2) % TS
        for y in range(y0, min(TS, y0 + 3)): c.put(jx, y, LINE)
    apply_edges(c, mask, (86, 56, 28), (216, 176, 112))
    return c

def clay_cell(mask, seed=0):
    # SMOOTH terracotta-red with horizontal sediment banding — no speckle, so it
    # reads clearly apart from earthy dirt and golden wood
    BASE = (192, 98, 62); DARK = (156, 72, 44); LITE = (216, 126, 90); RIM = (120, 58, 34)
    rng = random.Random((mask * 233 + seed) & 0x7FFFFFFF)
    c = Cell(); c.fill(BASE)
    for y in range(TS):
        if y in (2, 6):
            for x in range(TS): c.put(x, y, DARK)              # strata bands
        elif y in (0, 4):
            for x in range(TS):
                if rng.random() < 0.55: c.put(x, y, LITE)
        elif rng.random() < 0.08:
            c.put(rng.randint(0, TS - 1), y, DARK)
    apply_edges(c, mask, RIM)
    return c

def hellstone_cell(mask, seed=0):
    BASE = (92, 40, 34); DARK = (58, 22, 22); GLOW = (244, 122, 26); GLO2 = (255, 200, 70)
    rng = random.Random((mask * 419 + seed) & 0x7FFFFFFF)
    c = Cell()
    c.fill(BASE)
    clumps(c, rng, DARK, 4, 3)
    clumps(c, rng, (128, 60, 46), 2, 3)
    # a thick wandering magma vein with a hot core + halo
    x, y = rng.randint(1, 6), rng.randint(1, 6)
    for _ in range(6):
        c.put(x, y, GLO2)
        for dx, dy in ((1, 0), (0, 1), (-1, 0), (0, -1)):
            if c.get(x + dx, y + dy) not in (GLO2,):
                if rng.random() < 0.75: c.put(x + dx, y + dy, GLOW)
        x = max(0, min(TS - 1, x + rng.randint(-1, 1)))
        y = max(0, min(TS - 1, y + rng.randint(-1, 1)))
    shade_ramp(c)
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
    write_tileset("tiles_trunk", edge16_lut(), 1, 4, 4, tpl=1)
    print("[tiles] tiles_trunk", img.size)

# ---------------------------------------------------------------- furniture
def custom_sheet(name, cells, cols, lut, edge=0, nvar=1, tpl=1):
    rows = (len(cells) + cols - 1) // cols
    img = sheet_from_cells(cells, cols, rows)
    img.save(os.path.join(HERE, name + ".png"))
    write_tileset(name, lut, nvar, cols, rows, edge=edge, tpl=tpl)
    print("[tiles]", name, img.size)

def lut_from(fn):
    return [fn(m) for m in range(256)]

def torch_cell():
    # SlipPixel Torch.png is a 4-frame flame (8px cells); take frame 0 as the
    # static torch. (Animation would need a sprite overlay, not a static tile.)
    return slip_cells("Torch.png", 1, 1)[0]

def platform_cells():
    # SlipPixel Oak Wood Planks Platform.png: reslice row 1's three cells into the
    # autotile order the lut expects: [solo, left-end, middle, right-end]. The
    # left/right cells carry the plank end-caps; middle is continuous; solo reuses
    # the middle (a lone plank reads fine).
    left, mid, right = slip_cells("Oak Wood Planks Platform.png", 3, 1, y0=8)
    return [mid, left, mid, right]

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
    # SlipPixel anvil.png: 16x8 = [left, right], matches the 2-wide lut2w order.
    return slip_cells("anvil.png", 2, 1)

def furnace_cells():
    # SlipPixel Furnace.png: TWO 24x16 frames (off | on), 24px pitch. Use the
    # ON frame as the placed art — a furnace is lit. 24x16 = 3x2 tiles.
    # (2) so the station reads as hot, like the old art. Row-major 2x2 =
    # TL,TR,BL,BR, matching lut22.
    return slip_cells("Furnace.png", 3, 2, x0=24)

def chest_cells():
    # SlipPixel Oak Wood Chest.png: 16x16, row-major 2x2 = TL,TR,BL,BR (lut22).
    return slip_cells("Oak Wood Chest.png", 2, 2)

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
    # SlipPixel Oak Wood Door.png (40x24): col 0 is the 1-wide OPEN door (edge-on),
    # cols 1-2 are a 2-wide CLOSED door. Our door footprint is 1 tile wide x 3 tall,
    # so use col 0 as-is for open, and nearest-scale the 2-wide closed door down to
    # 8px wide for closed. Both slice into 3 cells (top/mid/bottom) for lut13.
    if open_:
        return slip_cells("Oak Wood Door.png", 1, 3, x0=0)
    return slip_cells_scaled("Oak Wood Door.png", (8, 0, 24, 24), 1, 3)

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

def table_cells():
    # SlipPixel Oak Wood Table.png 24x16 -> 3x2 row-major [TL,TM,TR,BL,BM,BR].
    return slip_cells("Oak Wood Table.png", 3, 2)

def chair_cells():
    # SlipPixel Oak Wood Chair.png 8x32 holds two facings stacked; use the first
    # (rows 0-1) as a 1x2 chair [top, bottom].
    return slip_cells("Oak Wood Chair.png", 1, 2)

def lantern_cells():
    # SlipPixel Lantern.png 8x16 -> 1x2 [top, bottom].
    return slip_cells("Lantern.png", 1, 2)

def fireplace_cells():
    # SlipPixel Fireplace.png: TWO 24x16 frames (off | on), no gap. ON frame —
    # not 3 frames. Rescale the whole thing to 24x16 so it's a 3-wide (3x2)
    # furniture whose left/mid/right columns are distinguishable by lut3x2.
    return slip_cells("Fireplace.png", 3, 2, x0=24)

def chain_cell():
    return slip_cells("Chain.png", 1, 1)[0]


def beam_cells():
    """Wooden support beam (Terraria-style): a warm post with side brackets at
    the top when nothing continues above, a plain shaft, and a footing."""
    POST = (150, 104, 58); DARK = (104, 68, 38); LITE = (182, 134, 78)
    def body(c):
        for y in range(TS):
            for x in range(2, 6):
                c.put(x, y, POST if x in (3, 4) else DARK)
        for y in range(0, TS, 3): c.put(3, y, LITE)      # grain
    cap = Cell(); body(cap)
    for x in range(0, TS):                                # header brace
        cap.put(x, 0, DARK); cap.put(x, 1, POST)
    cap.put(0, 2, DARK); cap.put(TS - 1, 2, DARK)
    shaft = Cell(); body(shaft)
    foot = Cell(); body(foot)
    for x in range(1, 7): foot.put(x, TS - 1, DARK)       # footing
    foot.put(1, TS - 2, DARK); foot.put(6, TS - 2, DARK)
    return [cap, shaft, foot]

def make_furniture():
    single = lut_from(lambda m: 0)
    custom_sheet("tiles_torch", [torch_cell()], 1, single)
    custom_sheet("tiles_platform", platform_cells(), 4,
                 lut_from(lambda m: {0: 0, E: 1, W: 3, E | W: 2}[(m & (E | W))]))
    lut2w = lut_from(lambda m: 1 if (m & W) else 0)
    custom_sheet("tiles_workbench", bench_cells((176, 128, 72), (128, 90, 48)), 2, lut2w)
    custom_sheet("tiles_anvil", anvil_cells(), 2, lut2w)
    lut22 = lut_from(lambda m: (0 if (m & S) else 2) + (1 if (m & W) else 0))
    custom_sheet("tiles_chest", chest_cells(), 2, lut22)
    custom_sheet("tiles_altar", altar_cells(), 2, lut22)
    lut13 = lut_from(lambda m: 1 if (m & N and m & S) else (0 if (m & S) else 2))
    custom_sheet("tiles_door_c", door_cells(False), 1, lut13)
    custom_sheet("tiles_door_o", door_cells(True), 1, lut13)
    custom_sheet("tiles_mush", [mush_cell()], 1, single)
    custom_sheet("tiles_flower", [flower_cell(k) for k in range(4)], 1, single, nvar=4)
    custom_sheet("tiles_sapling", [sapling_cell()], 1, single)
    # SlipPixel furniture -------------------------------------------------------
    lut1x2 = lut_from(lambda m: 0 if (m & S) else 1)               # [top, bottom]
    def _l3x2(m):
        col = 1 if ((m & W) and (m & E)) else (2 if (m & W) else 0)
        return (0 if (m & S) else 1) * 3 + col                     # [TL,TM,TR,BL,BM,BR]
    lut3x2 = lut_from(_l3x2)
    custom_sheet("tiles_table", table_cells(), 3, lut3x2)
    custom_sheet("tiles_chair", chair_cells(), 1, lut1x2)
    custom_sheet("tiles_lantern", lantern_cells(), 1, lut1x2)
    custom_sheet("tiles_furnace", furnace_cells(), 3, lut3x2)
    custom_sheet("tiles_fireplace", fireplace_cells(), 3, lut3x2)
    custom_sheet("tiles_chain", [chain_cell()], 1, single)
    lut_beam = lut_from(lambda m: 0 if not (m & N) else (2 if (m & S) == 0 else 1))
    custom_sheet("tiles_beam", beam_cells(), 1, lut_beam)

# ------------------------------------------------------------ bricks --------
CLAY_BRICK  = ((178, 102, 64), (128, 68, 42), (204, 128, 84))   # base/mortar/light
STONE_BRICK = ((124, 124, 132), (86, 86, 96), (150, 150, 158))

def brick_body(c, rng, pal, f=1.0):
    base, mortar, lite = (shade(p, f) for p in pal)
    for y in range(TS):
        course = y // 3
        for x in range(TS):
            if y % 3 == 2: col = mortar                       # bed joint
            elif (x + course * 4) % 8 == 0: col = mortar      # head joint
            elif rng.random() < 0.10: col = lite
            else: col = base
            c.put(x, y, col)

def brick_cell(mask, pal, f=1.0, seed=0):
    """Coursed bricks + apply_edges bevels. f pre-darkens (walls)."""
    rng = random.Random((mask * 269 + 17 + seed) & 0x7FFFFFFF)
    c = Cell()
    brick_body(c, rng, pal, f)
    apply_edges(c, mask, shade(pal[1], f))
    return c

# ---------------------------------------------------------------- walls
def make_walls():
    def wall(name, base, dark, light, density=0.2):
        # pre-darkened so the background layer needs no runtime shading;
        # the six diag_cut_cells give staircase edges their inner-corner cuts
        f = 0.52
        bs, ds, ls = shade(base, f), shade(dark, f), shade(light, f)
        def body(c, rng): speckle(c, bs, ds, ls, density, rng)
        blob_sheet(name,
                   lambda m, s: mat_cell(m, bs, ds, ls, shade(dark, f * 0.7), density, seed=s),
                   extra=diag_cut_cells(body, ls, ds), classify=classify_wall)
    wall("wall_dirt",  (128, 84, 50), (100, 62, 36), (150, 104, 66))
    wall("wall_stone", (116, 116, 124), (86, 86, 96), (140, 140, 148))
    wall("wall_wood",  (168, 122, 68), (136, 96, 50), (188, 144, 86), 0.12)
    wall("wall_ebon",  (104, 88, 128), (76, 62, 98), (128, 112, 152))
    wall("wall_ash",   (74, 66, 66), (56, 48, 50), (94, 84, 82))
    wall("wall_snow",  (200, 212, 228), (170, 184, 205), (226, 236, 248), 0.15)
    # brick walls (clay + stone) reuse the brick painter, pre-darkened
    blob_sheet("wall_clay_brick", lambda m, s: brick_cell(m, CLAY_BRICK, 0.52, s),
               extra=diag_cut_cells(lambda c, r: brick_body(c, r, CLAY_BRICK, 0.52),
                                    shade(CLAY_BRICK[2], 0.52), shade(CLAY_BRICK[1], 0.52)),
               classify=classify_wall)
    blob_sheet("wall_stone_brick", lambda m, s: brick_cell(m, STONE_BRICK, 0.52, s),
               extra=diag_cut_cells(lambda c, r: brick_body(c, r, STONE_BRICK, 0.52),
                                    shade(STONE_BRICK[2], 0.52), shade(STONE_BRICK[1], 0.52)),
               classify=classify_wall)
    # glass: mostly TRANSPARENT (the sky/backdrop shows through) with a frame
    # on exposed edges and a diagonal sheen
    def glass_cell(mask, seed=0):
        FRAME = (70, 82, 96); SHEEN = (168, 196, 216); SHEEN2 = (120, 150, 176)
        c = Cell()
        for k in range(TS):                       # diagonal sheen streaks
            c.put(k, (k + 2) % TS, SHEEN2 if k % 3 else SHEEN)
        for k in range(0, TS, 3):
            c.put(k, (k + 6) % TS, SHEEN2)
        if not (mask & N):
            for x in range(TS): c.put(x, 0, FRAME)
        if not (mask & S):
            for x in range(TS): c.put(x, TS - 1, FRAME)
        if not (mask & W):
            for y in range(TS): c.put(0, y, FRAME)
        if not (mask & E):
            for y in range(TS): c.put(TS - 1, y, FRAME)
        return c
    def glass_body(c, rng):
        SHEEN = (168, 196, 216); SHEEN2 = (120, 150, 176)
        for k in range(TS):
            c.put(k, (k + 2) % TS, SHEEN2 if k % 3 else SHEEN)
        for k in range(0, TS, 3):
            c.put(k, (k + 6) % TS, SHEEN2)
    blob_sheet("wall_glass", glass_cell,
               extra=diag_cut_cells(glass_body, (70, 82, 96), (70, 82, 96)),
               classify=classify_wall)



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



# ---------------------------------------------------------------- tree crowns
def make_bricks():
    # single-variant so the sheets carry the six diagonal cut cells (extras
    # need nvar=1) — the SOLID bricks slope exactly like the back walls
    blob_sheet("tiles_brick_clay", lambda m, s: brick_cell(m, CLAY_BRICK, 1.0, s),
               extra=diag_cut_cells(lambda c, r: brick_body(c, r, CLAY_BRICK, 1.0),
                                    CLAY_BRICK[2], CLAY_BRICK[1]),
               classify=classify_wall)
    blob_sheet("tiles_brick_stone", lambda m, s: brick_cell(m, STONE_BRICK, 1.0, s),
               extra=diag_cut_cells(lambda c, r: brick_body(c, r, STONE_BRICK, 1.0),
                                    STONE_BRICK[2], STONE_BRICK[1]),
               classify=classify_wall)

ROOF_BASE = (134, 60, 44); ROOF_DARK = (94, 40, 30); ROOF_LITE = (170, 90, 58); ROOF_CAP = (204, 128, 78)

def _shingle_fill(c, rng):
    for y in range(TS):
        for x in range(TS):
            if c.get(x, y) is None: continue
    # (fill happens before cuts; helper kept for clarity)

def _roof_body(c, rng):
    for y in range(TS):
        for x in range(TS):
            if y % 3 == 2: c.put(x, y, ROOF_DARK)
            elif (x + (y // 3) * 4) % 8 == 0: c.put(x, y, ROOF_DARK)
            elif y % 3 == 0 and rng.random() < 0.18: c.put(x, y, ROOF_LITE)
            else: c.put(x, y, ROOF_BASE)

def diag_cut_cells(body_fn, hi, lo, seed=4000):
    """6 slope cells for ANY material: [0] NW-cut '/' face · [1] NE-cut '\'
    face · [2] SE-cut underside · [3] SW-cut underside · [4] thin '/' ·
    [5] thin '\'. body_fn(cell, rng) paints the material; hi = sun-facing cut
    edge colour, lo = shadowed cut edge colour."""
    out = []
    for k in range(6):
        rng = random.Random(seed + k)
        c = Cell(); body_fn(c, rng)
        if k == 0:                                    # '/' top face
            for y in range(TS):
                for x in range(TS):
                    if x + y < TS - 1: c.px[y][x] = None
            for i in range(TS): c.put(i, TS - 1 - i, hi)
        elif k == 1:                                  # '\' top face
            for y in range(TS):
                for x in range(TS):
                    if x > y: c.px[y][x] = None
            for i in range(TS): c.put(i, i, hi)
        elif k == 2:                                  # underside, cut SE
            for y in range(TS):
                for x in range(TS):
                    if x + y > TS - 1: c.px[y][x] = None
            for i in range(TS): c.put(i, TS - 1 - i, lo)
        elif k == 3:                                  # underside, cut SW
            for y in range(TS):
                for x in range(TS):
                    if y > x: c.px[y][x] = None
            for i in range(TS): c.put(i, i, lo)
        elif k == 4:                                  # thin '/' plank
            for y in range(TS):
                for x in range(TS):
                    d = x + y - (TS - 1)
                    if d < -1 or d > 2: c.px[y][x] = None
            for i in range(TS): c.put(i, TS - 1 - i, hi)
            for i in range(TS - 2): c.put(i, TS - i, lo)
        else:                                         # thin '\' plank
            for y in range(TS):
                for x in range(TS):
                    d = y - x
                    if d < -1 or d > 2: c.px[y][x] = None
            for i in range(TS): c.put(i, i, hi)
            for i in range(TS - 2): c.put(i, i + 2, lo)
        out.append(c)
    return out

def classify_roof(m):
    """ROOF: diagonals wherever the geometry allows — every exposed convex
    corner chamfers (no continuation conditions). Apex/ridge and the very
    end cells stay square naturally (they lack the adjacent edges)."""
    n, e, s, w = m & N, m & E, m & S, m & W
    ne, se, sw, nw = m & NE, m & SE, m & SW, m & NW
    if not n and not w and e and s: return 0                  # NW corner -> '/'
    if not n and not e and w and s: return 1                  # NE corner -> '\'
    if not n and not w and not s and e and ne: return 0       # left eave tip: taper the end
    if not n and not e and not s and w and nw: return 1       # right eave tip
    if not s and not e and n and w and sw: return 2  # SE underside (needs SW: solid bases stay square)
    if not s and not w and n and e and se: return 3  # SW underside (needs SE)
    return None

def classify_wall(m):
    """BACK WALL / masonry: ONLY the under-L inner-corner cuts (a cell tucked
    under an L: covered above + one side, open below + the other side) —
    unconditional, nothing else slopes."""
    n, e, s, w = m & N, m & E, m & S, m & W
    sw, se = m & SW, m & SE
    if not s and not e and n and w and sw: return 2  # under-L, cut SE (needs SW below)
    if not s and not w and n and e and se: return 3  # under-L, cut SW (needs SE below)
    return None

def make_roof():
    blob_sheet("tiles_roof", roof_cell, nvar=1,
               extra=diag_cut_cells(_roof_body, ROOF_CAP, ROOF_DARK),
               classify=classify_roof)

def make_canopy():
    """Proper tree crowns (assets/canopy.png): 3 leafy variants + 1 snowy,
    40x28 cells, drawn as sprites on trunk tops. Branch stubs in branch.png
    (16x12 cells: left, right, leafy-left, leafy-right)."""
    import math as _m
    LEAF = (46, 126, 50); LDARK = (30, 92, 38); LDK2 = (22, 70, 30)
    LLITE = (96, 176, 66); LSUN = (140, 210, 90)
    W, H = 40, 28
    img = Image.new("RGBA", (W * 5, H), (0, 0, 0, 0))

    def dead_crown(ox, rng):
        """corruption: a bare, gnarled crown — crooked branches, no foliage"""
        BARK = (96, 74, 104); BDARK = (64, 48, 74)
        def limb(x, y, ang, ln, w):
            for s in range(ln):
                fx = x + _m.cos(ang) * s; fy = y - _m.sin(ang) * s
                for d in range(w):
                    px_, py_ = int(fx), int(fy - d)
                    if 0 <= px_ < W and 0 <= py_ < H:
                        img.putpixel((ox + px_, py_), rgb565_snap(BARK if d == 0 else BDARK) + (255,))
                if ln > 5 and s == ln // 2 and w > 1:
                    limb(int(fx), int(fy), ang + rng.uniform(0.5, 0.9) * (1 if rng.random() < 0.5 else -1),
                         ln // 2, 1)
        limb(20, H - 1, 1.5708, 12, 2)                       # main stem up
        for k in range(4):                                    # crooked limbs
            limb(20, H - 6 - k * 3, 1.5708 + rng.uniform(0.5, 1.1) * (1 if k & 1 else -1),
                 7 + rng.randint(0, 4), 1)

    def crown(ox, rng, snowy=False):
        blobs = [(20, 11, 11), (11, 15, 8), (29, 15, 8), (15, 7, 7), (26, 7, 7),
                 (20, 18, 8), (20, 21, 7), (16, 20, 6), (24, 20, 6)]
        blobs = [(bx + rng.randint(-2, 2), by + rng.randint(-1, 1), r + rng.randint(-1, 1))
                 for bx, by, r in blobs]
        def inside(x, y):
            for bx, by, r in blobs:
                dx, dy = x - bx, (y - by) * 1.25
                if dx * dx + dy * dy <= r * r: return True
            return False
        for y in range(H):
            for x in range(W):
                if not inside(x, y): continue
                # ragged silhouette (but keep the bottom-center solid: the
                # trunk must disappear INTO the foliage, not under a gap)
                edge = not (inside(x - 1, y) and inside(x + 1, y) and inside(x, y - 1) and inside(x, y + 1))
                center_bottom = abs(x - 20) < 7 and y > 16
                if edge and not center_bottom and rng.random() < 0.35: continue
                # depth shading: top-lit, dark belly
                t = y / float(H)
                c = LLITE if t < 0.28 else (LEAF if t < 0.62 else LDARK)
                if rng.random() < 0.14: c = LDARK if t < 0.6 else LDK2
                if rng.random() < 0.10 and t < 0.5: c = LSUN
                if edge: c = LDK2 if t > 0.35 else shade(c, 0.8)
                img.putpixel((ox + x, y), rgb565_snap(c) + (255,))
        # sunlit crown sprigs
        for x in range(2, W - 2):
            if img.getpixel((ox + x, 2))[3] == 0:
                for y in range(3, 8):
                    if img.getpixel((ox + x, y))[3]:
                        if rng.random() < 0.5:
                            img.putpixel((ox + x, y), rgb565_snap(LSUN) + (255,))
                        break
        if snowy:
            SN = (232, 240, 250); SN2 = (198, 212, 230)
            for y in range(H):
                for x in range(W):
                    px = img.getpixel((ox + x, y))
                    if not px[3]: continue
                    above = img.getpixel((ox + x, y - 1))[3] if y else 0
                    if not above or y < 9:
                        img.putpixel((ox + x, y), rgb565_snap(SN if rng.random() < 0.75 else SN2) + (255,))

    for v in range(3):
        crown(v * W, random.Random(9100 + v * 37))
    crown(3 * W, random.Random(9400), snowy=True)
    dead_crown(4 * W, random.Random(9700))
    img.save(os.path.join(HERE, "canopy.png"))
    with open(os.path.join(HERE, "canopy.sheet"), "w") as f:
        f.write("cell %d %d\n" % (W, H))
    print("[tiles] canopy.png (3 leafy + 1 snowy + 1 dead)")

    # branches: 16x12 cells — 0 left small, 1 right small, 2 left full, 3 right full.
    # Solid 2px limbs growing off the trunk with a leaf tuft at every tip
    # (bare sticks read as floating dashes at this scale).
    BW, BH = 16, 12
    bimg = Image.new("RGBA", (BW * 4, BH), (0, 0, 0, 0))
    BARK = (118, 84, 52); BDARK = (86, 60, 38)
    for k in range(4):
        rng = random.Random(700 + k)
        left = (k % 2) == 0
        big = k >= 2
        ox = k * BW
        ln = 8 if big else 6
        for i in range(ln):                      # limb: 2px thick, gentle rise
            x = (BW - 1 - i) if left else i
            y = 8 - (i >= 3) - (i >= 6)
            bimg.putpixel((ox + x, y), rgb565_snap(BARK) + (255,))
            bimg.putpixel((ox + x, y + 1), rgb565_snap(BDARK) + (255,))
        tuft_r = 4.2 if big else 3.2
        cx = (BW - 1 - ln) if left else ln
        cy = 8 - (1 if ln >= 3 else 0) - (1 if ln >= 6 else 0)
        for y in range(BH):
            for x in range(BW):
                dx, dy = x - cx, (y - cy) * 1.2
                d2 = dx * dx + dy * dy
                if d2 > tuft_r * tuft_r: continue
                if d2 > (tuft_r - 1) * (tuft_r - 1) and rng.random() < 0.4: continue
                t = (y - (cy - tuft_r)) / (2.0 * tuft_r)
                c = LLITE if t < 0.3 else (LEAF if t < 0.65 else LDARK)
                if rng.random() < 0.2: c = LDARK
                bimg.putpixel((ox + x, y), rgb565_snap(c) + (255,))
    bimg.save(os.path.join(HERE, "branch.png"))
    with open(os.path.join(HERE, "branch.sheet"), "w") as f:
        f.write("cell %d %d\n" % (BW, BH))
    print("[tiles] branch.png")


def _normalize_sheets():
    """Rewrite every assets/*.sheet to the full Studio format (a `sheet <png>`
    line is REQUIRED for the Studio Sheet tab to find the image)."""
    import glob
    for f in glob.glob(os.path.join(HERE, "*.sheet")):
        nm = os.path.splitext(os.path.basename(f))[0]
        cw = ch = mg = sp = 0
        for ln in open(f):
            t = ln.split()
            if len(t) >= 3 and t[0] == "cell": cw, ch = int(t[1]), int(t[2])
            elif len(t) >= 2 and t[0] == "margin": mg = int(t[1])
            elif len(t) >= 2 and t[0] == "spacing": sp = int(t[1])
        open(f, "w").write("sheet assets/%s.png\ncell %d %d\nmargin %d\nspacing %d\n" % (nm, cw, ch, mg, sp))


if __name__ == "__main__":
    os.makedirs(os.path.join(GAME, "tilesets"), exist_ok=True)
    make_terrain()
    make_trunk()
    make_furniture()
    make_walls()
    make_grass_caps()
    make_roof()
    make_bricks()
    make_canopy()
    print("done")
    _normalize_sheets()
