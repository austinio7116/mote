"""
Motebox — proper blob47 terrain.

WHY. The first two attempts at biome art were both FILLS: one tile (or a few
variants) repeated over an area, with no idea what was next to it. That is why they
looked like "rubbish squares" — where grass met sand there was a hard pixel step,
because nothing was drawn at the join. A fill cannot look like terrain at any level
of texture polish, because terrain is mostly EDGES.

roguemote's wall_brick, hedge, floor_jungle and the rest look right precisely
because they are 47-cell blob autotiles: the artist drew a different tile for every
arrangement of neighbours, so every boundary, corner and one-tile spur has art made
for it. That is the standard this has to meet.

So each biome here is a real 47-cell blob47 set, generated: the interior comes from
the texture vocabulary (biomes.py), and every cell draws a RIM on the sides that
face a different terrain, with the inner corners picked out. The neighbour-mask ->
cell-index contract is imported from roguemote's blob47.py rather than reimplemented,
so there is exactly one definition of "cell 23" in the whole repo.

Sheet layout is 8 columns x 6 rows = 48 cells (47 used, one spare) per variant
block, stacked vertically for nvar. That matches how the engine steps variants:
base_rows = (sheet_h / tile_h) / nvar, and cell c sits at (c % 8, c / 8).
"""
import os, sys
from PIL import Image

# The blob47 contract lives in roguemote and is imported, not copied.
_ROGUE_AUTH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "..", "roguemote", "authoring")
sys.path.insert(0, os.path.abspath(_ROGUE_AUTH))
import blob47                                          # noqa: E402

TS = 8
COLS = 8
ROWS = 6                                               # 8 x 6 = 48 >= 47


# The four corners as (name, cardinal A, cardinal B, corner pixel).
_CORNERS = (("NW", "N", "W", (0, 0)),
            ("NE", "N", "E", (TS - 1, 0)),
            ("SW", "S", "W", (0, TS - 1)),
            ("SE", "S", "E", (TS - 1, TS - 1)))


def _rim_plan(mask):
    """Which pixels of this cell are rim, and which are CUT.

    Open edge -> the whole one-pixel line along it.

    OUTSIDE CORNER (both adjacent cardinals open) -> the corner is CHAMFERED: the
    outermost pixel, and the two beside it, are cut back to a darker tone so the
    silhouette reads as a 45-degree bevel instead of a right angle. This is the
    difference between a coastline with shape and a staircase of squares, and it is
    what makes a diagonal river read as a diagonal. Without it every patch in the
    world has four hard corners no matter how good its texture is.

    (The engine draws one terrain per cell with no layer beneath, so a cut cannot be
    transparent — it has to be a colour. A darker tone at the corner reads as cut,
    which is the same trick hand-drawn tilesets use.)

    INNER corner (both cardinals same, the diagonal different) -> one pixel poked
    into the inside of the join. Those eight cells are the entire reason a
    nine-slice looks wrong and a blob47 does not.
    """
    open_e = blob47.edges_open(mask)
    rim, cut = set(), set()
    if open_e["N"]:
        for x in range(TS): rim.add((x, 0))
    if open_e["S"]:
        for x in range(TS): rim.add((x, TS - 1))
    if open_e["W"]:
        for y in range(TS): rim.add((0, y))
    if open_e["E"]:
        for y in range(TS): rim.add((TS - 1, y))

    for name, a, b, (cx, cy) in _CORNERS:
        if open_e[a] and open_e[b]:
            dx = 1 if cx == 0 else -1
            dy = 1 if cy == 0 else -1
            cut.add((cx, cy))                      # the corner itself
            cut.add((cx + dx, cy))                 # and a step along each edge,
            cut.add((cx, cy + dy))                 # which is what makes it a bevel
            rim.add((cx + dx, cy + dy))            # the new corner of the rim

    for c in blob47.inner_corners(mask):
        for name, a, b, p in _CORNERS:
            if name == c:
                rim.add(p)
    return rim, cut, open_e


def build(interior_fn, rim, rim_south=None, nvar=2, cut=None):
    """A whole blob47 sheet.

    interior_fn(variant) -> an 8x8 RGBA tile (the texture vocabulary supplies it)
    rim                  -> the edge colour
    rim_south            -> optional second colour for the bottom edge, which is what
                            turns a flat patch into something with a lit top and a
                            shadowed underside (rock, cliffs, ploughed land)
    cut                  -> the colour of a chamfered outside corner. Defaults to
                            rim_south, then to the rim, but a biome that sits in
                            something (an island in the sea, a river in the grass)
                            looks best cutting toward what it sits in.
    """
    # RIM MAY BE None. Every biome rimming itself means every boundary carries TWO
    # rims — one from each side — and the whole map reads as outlined ribbons. Only
    # materials with a real physical edge should draw one: water has foam, rock has a
    # cliff, lava has a hot line, a ploughed field has a boundary. Soft ground (grass,
    # snow, sand, ash) just stops, and the neighbour's rim is the edge.
    cut = cut or rim_south or rim
    sheet = Image.new("RGBA", (COLS * TS, ROWS * TS * nvar))
    for v in range(nvar):
        for cell, mask in enumerate(blob47.CANON):
            tile = interior_fn(v).copy()
            px = tile.load()
            rimpx, cutpx, open_e = _rim_plan(mask)
            if rim is not None:
                for (x, y) in rimpx:
                    if (x, y) in cutpx:
                        continue
                    c = rim
                    if rim_south and y == TS - 1 and open_e["S"]:
                        c = rim_south
                    px[x, y] = c + (255,)
            if cut is not None:
                for (x, y) in cutpx:
                    px[x % TS, y % TS] = cut + (255,)
            sheet.paste(tile, ((cell % COLS) * TS,
                               (cell // COLS) * TS + v * ROWS * TS))
    return sheet


def write(tsets_dir, name, sheet, nvar, edge=1, vweight=None):
    """The .tileset beside it, carrying the canonical LUT."""
    os.makedirs(tsets_dir, exist_ok=True)
    sheet.save(os.path.join(tsets_dir, name + ".png"))
    vw = (list(vweight or []) + [1] * 8)[:8]
    with open(os.path.join(tsets_dir, name + ".tileset"), "w") as f:
        f.write(f"sheet tilesets/{name}.png\n")
        f.write("tile 8\n")
        f.write(f"edge {edge}\n")
        f.write(f"nvar {nvar}\n")
        f.write("lut " + " ".join(str(v) for v in blob47.LUT) + "\n")
        f.write("xform " + " ".join(["0"] * 256) + "\n")
        f.write("vweight " + " ".join(str(v) for v in vw) + "\n")


# --------------------------------------------------------------- preview ------

def render_map(tsets, ids, order, cols, rows, scale=3):
    """Render a test map through the SAME mask->cell logic the engine uses, so the
    preview shows real transitions rather than a tile repeated. `ids` is a
    cols x rows grid of terrain names (or None for nothing)."""
    # Read the LAYOUT from each sheet exactly as the engine does — tiles per row is
    # sheet_w / tile_w and a variant steps a whole base block — instead of assuming
    # this file's own 8x6. The artist's sheets are 47x1, and assuming otherwise made
    # the comparison preview render two cells and look like a bug in HIS art.
    sheets, nvars, tprs, brows = {}, {}, {}, {}
    for n in order:
        p = os.path.join(tsets, n + ".png")
        if not os.path.isfile(p):
            continue
        im = Image.open(p).convert("RGBA")
        nv = 1
        for line in open(os.path.join(tsets, n + ".tileset")):
            if line.startswith("nvar"):
                nv = int(line.split()[1])
        sheets[n] = im
        nvars[n] = nv
        tprs[n] = max(1, im.width // TS)
        brows[n] = max(1, (im.height // TS) // nv)

    out = Image.new("RGBA", (cols * TS, rows * TS), (10, 10, 16, 255))
    for y in range(rows):
        for x in range(cols):
            t = ids[y][x]
            if not t or t not in sheets:
                continue
            m = 0
            for bit, (dx, dy) in ((blob47.N, (0, -1)), (blob47.NE, (1, -1)),
                                  (blob47.E, (1, 0)),  (blob47.SE, (1, 1)),
                                  (blob47.S, (0, 1)),  (blob47.SW, (-1, 1)),
                                  (blob47.W, (-1, 0)), (blob47.NW, (-1, -1))):
                nx, ny = x + dx, y + dy
                same = (ids[ny][nx] == t) if (0 <= nx < cols and 0 <= ny < rows) else True
                if same:
                    m |= bit
            cellno = blob47.LUT[m]
            nv, tpr, br = nvars[t], tprs[t], brows[t]
            if tpr == 1 and br == 1:
                cellno = 0                      # a plain fill: every config is cell 0
            h = ((x * 73856093) ^ (y * 19349663)) & 0xFFFFFFFF
            h ^= h >> 13
            h = (h * 1274126177) & 0xFFFFFFFF
            v = h % nv
            sx = (cellno % tpr) * TS
            sy = (cellno // tpr) * TS + v * br * TS
            out.paste(sheets[t].crop((sx, sy, sx + TS, sy + TS)), (x * TS, y * TS))
    return out.resize((cols * TS * scale, rows * TS * scale), Image.NEAREST)
