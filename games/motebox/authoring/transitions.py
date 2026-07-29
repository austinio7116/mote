"""Motebox — terrain transitions as a TRANSPARENT BLOB47 OVERLAY.

THE PROBLEM THIS EXISTS TO SOLVE. A cell may only paint its own eight by eight pixels,
so in a single opaque layer the colour boundary between two terrains lies exactly on the
tile grid: every coastline is a staircase of straight eight-pixel segments meeting at
right angles, however good the tiles are. The fix is a second layer — the base pass
paints flat colour fields and this pass paints the BOUNDARY, in the higher terrain's own
colours, INSIDE the lower terrain's cell, so the visible edge is a ragged rounded shape
that owes nothing to the grid.

AND THE BLOB47 BELONGS HERE, NOT ON THE BASE. The first attempt at layering kept 47-cell
sets for the base and gave the overlay a 16-cell FOUR-NEIGHBOUR mask, which got both
halves wrong at once. With no rim left to draw, all 47 base cells were the same flat
fill — a blob47 set doing nothing at all — while the layer that actually draws every
boundary in the world could not tell apart the two cases that matter most:

    higher terrain on N, E and NE            higher terrain on N and E, but NOT NE
    -----------------------------            -------------------------------------
    it is WRAPPING this cell, so the         it stops short, so the corner is a
    corner fills with a convex arc           NOTCH and has to be left open

A four-neighbour mask fills both of those identically, which is precisely the hard right
angle the whole exercise exists to remove. Eight neighbours reduced to blob47's 47
distinct cases is the smallest set that can draw a boundary correctly, and it is what a
blob47 set is FOR: the diagonals are the entire point.

The mask is imported from roguemote's blob47.py and INVERTED IN MEANING: a bit here says
"the higher terrain is at that neighbour", where roguemote's sets say "that neighbour
matches me". Same combinatorics, same reduction, one definition of cell 23 in the repo.

ONE CASE THE REDUCTION WILL NOT GIVE US. A lone diagonal — the higher terrain touching
only at a corner, with neither adjacent cardinal — reduces to cell 0, the same cell as
"no higher terrain anywhere". So a corner nub is not drawable: putting one in cell 0
would stamp a stray pixel into every interior cell in the world. Checked before relying
on it, which is the only reason this file does not try.

SEAMLESS BY CONSTRUCTION. Each edge profile begins and ends at the same depth, so any
two tiles along a run join continuously and a long shore reads as one curve rather than
as eight-pixel scallops repeating. The variation lives in the MIDDLE of the tile, which
is the part that never has to agree with anybody.
"""
import os, sys
from PIL import Image

_ROGUE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "..", "roguemote", "authoring")
sys.path.insert(0, os.path.abspath(_ROGUE))
import blob47                                              # noqa: E402

TS = 8
# 16 x 3 = 48 >= 47. This is STUDIO'S OWN GRID, not a free choice: the editor computes
# cols = min(128 / tile, ncell) and rows = ceil(ncell / cols), so a blob47 set at 8 px
# is 16 by 3 per variant block. Emitting 8 by 6 made a sheet the game could read and
# the editor could not, which is half the point of shipping a sidecar at all.
COLS, ROWS = 16, 3

# How far the higher terrain reaches in, per pixel along an edge. Both ends are 2 so
# neighbouring tiles always meet; the middle is what varies.
PROFILES = (
    (2, 3, 4, 4, 3, 3, 2, 2),
    (2, 2, 3, 4, 4, 3, 3, 2),
    (2, 3, 3, 2, 3, 4, 3, 2),
    (2, 2, 3, 3, 4, 4, 3, 2),
)

# (name, cardinal A, cardinal B, diagonal, corner pixel, inward step)
_CORNERS = (
    ("NW", blob47.N, blob47.W, blob47.NW, (0, 0),           (1, 1)),
    ("NE", blob47.N, blob47.E, blob47.NE, (TS - 1, 0),      (-1, 1)),
    ("SW", blob47.S, blob47.W, blob47.SW, (0, TS - 1),      (1, -1)),
    ("SE", blob47.S, blob47.E, blob47.SE, (TS - 1, TS - 1), (-1, -1)),
)


def _prof(i, flip=False):
    d = PROFILES[i % len(PROFILES)]
    return tuple(reversed(d)) if flip else d


def cell(mask, base, edge, profile=0):
    """One overlay cell, for an eight-neighbour mask of where the higher terrain is.

    `base` is the higher terrain's body colour and `edge` its lighter shore tone, which
    goes on the innermost pixel of the reach — a lit lip along the boundary, so the
    thing creeping over its neighbour looks like it has a thickness rather than like a
    stain.
    """
    im = Image.new("RGBA", (TS, TS), (0, 0, 0, 0))
    px = im.load()
    depth = {}

    def claim(x, y, d):
        if 0 <= x < TS and 0 <= y < TS:
            depth[(x, y)] = max(depth.get((x, y), 0), d)

    if mask & blob47.N:
        for x, d in enumerate(_prof(profile)):
            for y in range(d):
                claim(x, y, d - y)
    if mask & blob47.S:
        for x, d in enumerate(_prof(profile + 1, True)):
            for y in range(d):
                claim(x, TS - 1 - y, d - y)
    if mask & blob47.W:
        for y, d in enumerate(_prof(profile + 2)):
            for x in range(d):
                claim(x, y, d - x)
    if mask & blob47.E:
        for y, d in enumerate(_prof(profile + 3, True)):
            for x in range(d):
                claim(TS - 1 - x, y, d - x)

    for _n, ca, cb, diag, (cx, cy), (ix, iy) in _CORNERS:
        both = (mask & ca) and (mask & cb)
        if both and (mask & diag):
            # WRAPPING. The higher terrain continues round this corner, so fill it with
            # a convex quarter-arc. Without this the two straight bands merely cross and
            # the OUTSIDE of every bend in the world is a right angle — which is the
            # whole complaint, moved from the base layer to the overlay.
            for k in range(4):
                for j in range(4 - k):
                    claim(cx + ix * k, cy + iy * j, 4 - max(k, j))
        elif both:
            # A NOTCH. It is on both sides but does NOT continue round, so the lower
            # terrain has a real little peninsula poking out here and the corner must be
            # left OPEN. A four-neighbour mask cannot express this and fills it solid.
            for k in range(2):
                for j in range(2 - k):
                    depth.pop((cx + ix * k, cy + iy * j), None)

    for (x, y), d in depth.items():
        px[x, y] = (edge if d == 1 else base) + (255,)
    return im


def build(base, edge, nvar=2):
    """A real 47-cell blob47 overlay set, 16 x 3 per variant block, stacked for nvar —
    Studio's own grid for a blob47 set at 8 px, so the editor and the game agree."""
    sheet = Image.new("RGBA", (COLS * TS, ROWS * TS * nvar), (0, 0, 0, 0))
    for v in range(nvar):
        for c, mask in enumerate(blob47.CANON):
            sheet.paste(cell(mask, base, edge, profile=v * 2),
                        ((c % COLS) * TS, (c // COLS) * TS + v * ROWS * TS))
    return sheet


def write_tileset(tsets_dir, name, sheet, nvar=2, vweight=None):
    """Ship the overlay as an editable .tileset sidecar, because it IS a rule tile.

    `type 0` says Blob 47 explicitly. The generator used to omit `type` altogether and
    Studio defaulted it — which is how the flat base sets came to be displayed as
    forty-seven identical cells: nothing in the file said what they were.
    """
    os.makedirs(tsets_dir, exist_ok=True)
    sheet.save(os.path.join(tsets_dir, name + ".png"))
    vw = (list(vweight or []) + [1] * 8)[:8]
    with open(os.path.join(tsets_dir, name + ".tileset"), "w") as f:
        f.write("sheet tilesets/%s.png\n" % name)
        f.write("tile %d\n" % TS)
        f.write("type 0\n")                       # Blob 47, stated rather than assumed
        f.write("edge 0\n")                       # an overlay: the world outside is open
        f.write("nvar %d\n" % nvar)
        f.write("cols %d\nrows %d\n" % (COLS, ROWS * nvar))
        f.write("lut " + " ".join(str(v) for v in blob47.LUT) + "\n")
        f.write("xform " + " ".join(["0"] * 256) + "\n")
        f.write("vweight " + " ".join(str(v) for v in vw) + "\n")


def write_lut_header(path):
    """The mask -> cell table, for the C draw pass.

    Generated from the same blob47.LUT the sheets are built from, so the table the game
    indexes with and the art it indexes into cannot drift apart.
    """
    with open(path, "w") as f:
        f.write("/* GENERATED by authoring/transitions.py from roguemote's blob47.py"
                " — do not edit.\n"
                " *\n"
                " * Mask bits: N=1 NE=2 E=4 SE=8 S=16 SW=32 W=64 NW=128, where a set bit\n"
                " * means THE HIGHER TERRAIN IS AT THAT NEIGHBOUR (the inverse of the\n"
                " * sense roguemote's own sets use). The value is a cell index 0..46 in\n"
                " * an 8 x 6 block. */\n")
        f.write("#ifndef MB_BLOB47_LUT_H\n#define MB_BLOB47_LUT_H\n\n")
        f.write("#define MB_TR_COLS %d\n#define MB_TR_ROWS %d\n\n" % (COLS, ROWS))
        f.write("static const uint8_t MB_BLOB47[256] = {\n")
        for i in range(0, 256, 16):
            f.write("    " + ", ".join("%2d" % v for v in blob47.LUT[i:i + 16]) + ",\n")
        f.write("};\n\n#endif\n")
