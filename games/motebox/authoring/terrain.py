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

WHAT THE ARTIST ACTUALLY DOES. Three versions of this file guessed at the geometry
and all three looked wrong, so the fourth went and read his pixels. Dumping hedge's
cells as a colour map settles it completely:

    body cell        N open          NW convex       island
    00000000         ........        ........        ........
    00000000         RRRRRRRR        ..RRRRRR        .bRRRRb.
    00000000         RRRRRRRR        .RbRRRRR        .bRbbRb.   . = transparent
    00000000         bbbbbbbb        .RRbbbbb        .bRbbRb.   R = BRIGHT rim
    00000000         bbbbbbbb        .RRbbbbb        .bRbbRb.   b = base
    ...              ...             ...             ...

Three facts, none of which the earlier versions had:

  1. THE INTERIOR IS FLAT. Not textured, not stippled — one solid colour, every
     pixel. Both hedge and floor_jungle are like this. Every gram of character is
     in the edge cells. Earlier versions spent all their effort on interior
     patterns and gave the edge a single-pixel line, which is precisely backwards,
     and it is why large areas read as wallpaper.
  2. THE RIM IS TWO PIXELS THICK AND BRIGHTER THAN THE BASE. A 1 px line reads as
     an outline; a 2 px band of a lighter tone reads as a lit edge, which is what
     makes a patch look like a raised, shaped thing.
  3. CORNERS ROUND WITH A SINGLE PIXEL, and the rim thins to 1 px when opposite
     edges are both open. That is the entire secret of the round islands and the
     shapely coasts: one base-coloured pixel at each convex corner, and no
     three-pixel chamfer anywhere.

The one thing not copied is the transparency: hedge insets itself a pixel on every
open side, because it is an OVERLAY on a floor. Motebox draws one terrain per cell
with nothing underneath, so an inset would put a dark gap around every patch in the
world — and adjacent patches would each contribute one, making it two. So the rim
runs to the tile edge here, and the corner rounding is done in base colour instead
of by cutting away.

The neighbour-mask -> cell-index contract is imported from roguemote's blob47.py
rather than reimplemented, so there is exactly one definition of "cell 23" in the
whole repo.

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


def flat(interior_fn, nvar=2):
    """A ONE-CELL tileset, stacked for nvar.

    The base layer draws no boundaries any more — the transition overlay draws all of
    them — so there is nothing for 47 cells to disagree about, and emitting 47 copies of
    the same flat fill was a blob47 set doing no work at all while costing 47 times the
    flash. One cell per variant, and `write` pairs it with an all-zero LUT so every
    neighbour arrangement maps to it.
    """
    sheet = Image.new("RGBA", (TS, TS * nvar))
    for v in range(nvar):
        sheet.paste(interior_fn(v), (0, v * TS))
    return sheet


def build(base, interior_fn, rim, nvar=2, sides="NSEW"):
    """A whole blob47 sheet, in the artist's own geometry (see the module docstring).

    base        -> the flat interior colour
    interior_fn -> (variant) -> an 8x8 RGBA tile; normally a flat fill of `base`.
                   Only a surface that genuinely has structure should pass anything
                   else: moving water has ripples, a ploughed field has furrows.
    rim         -> the 2 px edge colour. BRIGHTER than the base, or the patch reads
                   as outlined rather than lit. May be None for a biome that should
                   simply stop (its neighbours' rims are then the only edge).
    sides       -> WHICH edges may carry the rim, and this is load-bearing.

                   "NSEW" is for a material with a physically real edge on every
                   side: water has foam all round, rock has a cliff all round.

                   "SE" is for SOFT GROUND, and it exists because rimming all four
                   sides of everything is a trap this file has now fallen into twice.
                   When both neighbours rim the edge they share, that edge carries
                   TWO bright bands, and a continent of grass, savanna and sand reads
                   as a jigsaw of outlined pieces rather than as country. Lighting
                   only the south and east means exactly one of any two neighbours
                   draws a band on their shared edge — the same rule a painter uses,
                   one light direction — so every patch still has a lit side and a
                   shaped corner, and nothing is outlined.
    """
    sheet = Image.new("RGBA", (COLS * TS, ROWS * TS * nvar))
    for v in range(nvar):
        for cell, mask in enumerate(blob47.CANON):
            tile = interior_fn(v).copy()
            px = tile.load()
            open_e = blob47.edges_open(mask)
            if rim is not None:
                lit = {d: (open_e[d] and d in sides) for d in "NSEW"}
                _paint_rim(px, base, rim, lit, mask, open_e)   # open_e sizes the band
            sheet.paste(tile, ((cell % COLS) * TS,
                               (cell // COLS) * TS + v * ROWS * TS))
    return sheet


def _paint_rim(px, base, rim, open_e, mask, all_open=None):
    R, B = rim + (255,), base + (255,)

    # 1. THICKNESS. Two pixels, except where opposite edges are both open — an 8 px
    #    tile cannot carry two 2 px bands and still be a tile, so it thins to one.
    #    This is the artist's own rule and it is what makes a one-tile spur (a river,
    #    an isthmus) still look like the material rather than like its own outline.
    #
    #    It is sized on `all_open` — whether the PATCH is narrow here — and not on
    #    which sides happen to be lit. Reading it off the lit set meant an SE-lit
    #    biome never thinned at all, so a single stray tile of ash left over from an
    #    old fire drew a fat two-pixel band and a town two centuries after its fires
    #    was dotted with hard grey squares.
    wide = all_open if all_open is not None else open_e
    tv = 1 if (wide["N"] and wide["S"]) else 2
    th = 1 if (wide["W"] and wide["E"]) else 2

    if open_e["N"]:
        for y in range(tv):
            for x in range(TS): px[x, y] = R
    if open_e["S"]:
        for y in range(TS - tv, TS):
            for x in range(TS): px[x, y] = R
    if open_e["W"]:
        for x in range(th):
            for y in range(TS): px[x, y] = R
    if open_e["E"]:
        for x in range(TS - th, TS):
            for y in range(TS): px[x, y] = R

    # 2. ROUND THE CONVEX CORNERS — one pixel, in base colour, at the outer corner,
    #    plus one where the two rim bands meet on the inside. Two pixels per corner
    #    is the whole of it: the earlier three-pixel chamfer was both heavier and
    #    less round, and it is why coastlines still read as staircases.
    for name, a, b, (cx, cy) in _CORNERS:
        if not (open_e[a] and open_e[b]):
            continue
        px[cx, cy] = B
        ix = th if cx == 0 else TS - 1 - th
        iy = tv if cy == 0 else TS - 1 - tv
        px[ix, iy] = B

    # 3. INNER CORNERS get a small bright arc poking into the tile. These eight cells
    #    are the entire reason a nine-slice looks wrong and a blob47 does not: without
    #    them, the inside of every bend is square.
    for c in blob47.inner_corners(mask):
        for name, a, b, (cx, cy) in _CORNERS:
            if name != c or not (open_e[a] or open_e[b]):
                continue
            dx = 1 if cx == 0 else -1
            dy = 1 if cy == 0 else -1
            px[cx, cy] = R
            px[cx + dx, cy] = R
            px[cx, cy + dy] = R
            px[cx + dx, cy + dy] = B          # the notch that makes it an arc
            px[cx + 2 * dx, cy] = R
            px[cx, cy + 2 * dy] = R


def write(tsets_dir, name, sheet, nvar, edge=1, vweight=None, lut=None):
    """The .tileset beside it, carrying the canonical LUT — or an all-zero one for a
    flat single-cell set, where every arrangement of neighbours draws the same tile."""
    os.makedirs(tsets_dir, exist_ok=True)
    sheet.save(os.path.join(tsets_dir, name + ".png"))
    vw = (list(vweight or []) + [1] * 8)[:8]
    with open(os.path.join(tsets_dir, name + ".tileset"), "w") as f:
        f.write(f"sheet tilesets/{name}.png\n")
        f.write("tile 8\n")
        # TYPE, STATED. Omitting it let Studio default to Blob 47 and pad the display
        # to 47 slots, which is why a flat one-cell fill showed up in the editor as
        # forty-seven identical brown squares.
        f.write("type %d\n" % (0 if lut is None else 0))
        f.write("cols %d\nrows %d\n" % (sheet.size[0] // 8, sheet.size[1] // 8))
        f.write(f"edge {edge}\n")
        f.write(f"nvar {nvar}\n")
        f.write("lut " + " ".join(str(v) for v in (lut if lut is not None else blob47.LUT)) + "\n")
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
