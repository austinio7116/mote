"""Motebox — terrain transitions as a TRANSPARENT OVERLAY.

THE PROBLEM THIS EXISTS TO SOLVE. Every version of the terrain so far drew one opaque
tile per cell, and that constraint has a consequence no amount of art can escape: a
cell may only paint its own eight by eight pixels, so the colour boundary between two
terrains lies exactly on the tile grid. Every coastline is therefore a staircase of
straight eight-pixel segments meeting at right angles, however good the tiles are. The
2 px lit rim the last version added made the boundary more visible, not less
rectilinear — it drew a straight white line along the straight edge.

The fix is a second layer. The base pass paints flat colour fields, and this pass
paints the BOUNDARY, in the higher terrain's own colours, INSIDE the lower terrain's
cell — so the visible edge is a ragged, rounded shape that has nothing to do with the
tile grid. Grass creeps over sand, sand creeps into the shallows, rock shoulders over
grass. That is how every tile game with a coast worth looking at does it.

PRECEDENCE, NOT SYMMETRY. Each terrain has a height in a stack (see PRECEDENCE in
extract_box.py). Only the higher one bleeds, and only ever downhill, so a boundary is
drawn exactly once and two neighbours can never both claim the same pixels.

SEAMLESS BY CONSTRUCTION. Each edge profile begins and ends at a fixed depth, so any
two tiles along a run join continuously and a long shore reads as one curve rather
than as eight-pixel scallops repeating. The variation lives in the MIDDLE of the tile,
which is the part that never has to agree with anybody.
"""
from PIL import Image

TS = 8
N, E, S, W = 1, 2, 4, 8

# How far the higher terrain reaches into the lower cell, per column across an edge.
# Both ends are 2 so neighbouring tiles always meet; the middle is what varies. Four
# profiles, picked per cell from its position, so a long coast undulates instead of
# repeating one scallop.
PROFILES = (
    (2, 3, 4, 4, 3, 3, 2, 2),
    (2, 2, 3, 4, 4, 3, 3, 2),
    (2, 3, 3, 2, 3, 4, 3, 2),
    (2, 2, 3, 3, 4, 4, 3, 2),
)


def _depths(profile, flip):
    d = PROFILES[profile % len(PROFILES)]
    return tuple(reversed(d)) if flip else d


def cell(mask, base, edge, profile=0):
    """One overlay cell: the higher terrain reaching in from every side in `mask`.

    `base` is the higher terrain's body colour and `edge` its lighter shore tone, which
    goes on the innermost pixel of the reach — a lit lip along the boundary, so the
    thing that bleeds looks like it has a thickness rather than like a stain.
    """
    im = Image.new("RGBA", (TS, TS), (0, 0, 0, 0))
    px = im.load()
    owned = {}                                  # (x,y) -> depth from its own side

    def claim(x, y, d):
        if 0 <= x < TS and 0 <= y < TS:
            owned[(x, y)] = max(owned.get((x, y), 0), d)

    if mask & N:
        for x, d in enumerate(_depths(profile, 0)):
            for y in range(d):
                claim(x, y, d - y)
    if mask & S:
        for x, d in enumerate(_depths(profile + 1, 1)):
            for y in range(d):
                claim(x, TS - 1 - y, d - y)
    if mask & W:
        for y, d in enumerate(_depths(profile + 2, 0)):
            for x in range(d):
                claim(x, y, d - x)
    if mask & E:
        for y, d in enumerate(_depths(profile + 3, 1)):
            for x in range(d):
                claim(TS - 1 - x, y, d - x)

    for (x, y), d in owned.items():
        px[x, y] = (edge if d == 1 else base) + (255,)
    return im


def build(base, edge, nvar=2):
    """Sixteen cells per variant, indexed by mask: N | E<<1 | S<<2 | W<<3.

    Two variants with different profiles, chosen by cell position at draw time, so the
    same stretch of coast is not the same wiggle repeated.
    """
    sheet = Image.new("RGBA", (TS * 16, TS * nvar), (0, 0, 0, 0))
    for v in range(nvar):
        for m in range(16):
            sheet.paste(cell(m, base, edge, profile=v * 2), (m * TS, v * TS))
    return sheet
