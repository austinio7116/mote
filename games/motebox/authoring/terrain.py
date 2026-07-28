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


def _rim_plan(mask):
    """Which pixels of this cell are rim, given who its same-terrain neighbours are.

    Open edge  -> the whole one-pixel line along it.
    Open corner (both adjacent cardinals open) -> already covered by the two lines,
      and the corner pixel is doubled, which is what rounds it.
    INNER corner (both cardinals same, the diagonal different) -> one pixel poked
      into the inside of the join. Those eight cells are the entire reason a
      nine-slice looks wrong and a blob47 does not.
    """
    open_e = blob47.edges_open(mask)
    px = set()
    if open_e["N"]:
        for x in range(TS): px.add((x, 0))
    if open_e["S"]:
        for x in range(TS): px.add((x, TS - 1))
    if open_e["W"]:
        for y in range(TS): px.add((0, y))
    if open_e["E"]:
        for y in range(TS): px.add((TS - 1, y))
    for c in blob47.inner_corners(mask):
        if c == "NE": px.add((TS - 1, 0))
        elif c == "SE": px.add((TS - 1, TS - 1))
        elif c == "SW": px.add((0, TS - 1))
        elif c == "NW": px.add((0, 0))
    return px, open_e


def build(interior_fn, rim, rim_south=None, nvar=2):
    """A whole blob47 sheet.

    interior_fn(variant) -> an 8x8 RGBA tile (the texture vocabulary supplies it)
    rim                  -> the edge colour
    rim_south            -> optional second colour for the bottom edge, which is
                            what turns a flat patch into something with a lit top
                            and a shadowed underside (rock, cliffs, ploughed land)
    """
    sheet = Image.new("RGBA", (COLS * TS, ROWS * TS * nvar))
    for v in range(nvar):
        for cell, mask in enumerate(blob47.CANON):
            tile = interior_fn(v).copy()
            px = tile.load()
            plan, open_e = _rim_plan(mask)
            for (x, y) in plan:
                c = rim
                if rim_south and y == TS - 1 and open_e["S"]:
                    c = rim_south
                px[x, y] = c + (255,)
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
    sheets, nvars = {}, {}
    for n in order:
        p = os.path.join(tsets, n + ".png")
        if not os.path.isfile(p):
            continue
        sheets[n] = Image.open(p).convert("RGBA")
        # nvar comes from the .tileset so the preview cannot disagree with it
        nv = 1
        for line in open(os.path.join(tsets, n + ".tileset")):
            if line.startswith("nvar"):
                nv = int(line.split()[1])
        nvars[n] = nv

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
            nv = nvars[t]
            h = ((x * 73856093) ^ (y * 19349663)) & 0xFFFFFFFF
            h ^= h >> 13
            h = (h * 1274126177) & 0xFFFFFFFF
            v = h % nv
            sx = (cellno % COLS) * TS
            sy = (cellno // COLS) * TS + v * ROWS * TS
            out.paste(sheets[t].crop((sx, sy, sx + TS, sy + TS)), (x * TS, y * TS))
    return out.resize((cols * TS * scale, rows * TS * scale), Image.NEAREST)
