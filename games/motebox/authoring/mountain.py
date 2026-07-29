"""Motebox — the mountain band, built from the artist's rock by STRUCTURE not by chance.

WHAT WENT WRONG THREE TIMES. The master has a composed mountain at (9..10, 25..28): a
snow cap, two slope rows that alternate into diamond peaks, and a base with a snowline —
two columns wide, four rows tall, drawn to FIT TOGETHER. I kept handing pairs of those
cells to the tileset as `nvar` variants, which the engine picks with
`mote__at_hash(x, y)` — a deliberately random per-cell hash. So a composed picture got
shuffled into noise, and a mountain range came out as diagonal confetti.

TWO THINGS ARE NOT AVAILABLE, both checked in the engine rather than assumed:

  - There is no way to place his 2x2 unit in phase. The variant row is chosen by that
    position hash and nothing else; `var_weight` only changes how often a row is picked,
    not where.
  - 16 px tiles are not available either. `draw_autotile_layers` takes its tile size from
    tiles[0], so every layer shares one size, and the terrain grid is 8 px.

WHAT IS AVAILABLE IS THE MASK, and it is the right tool: it tells each cell where it
sits in the mass. Nothing above means this is the top of the mountain, so it gets his
snow cap. Nothing below means it is the foot, so it gets his snowline. Everything between
is slope. That is exactly what his four rows were drawn for — I had been using two of
them as random variants instead of four of them as structure.

The interior facet is derived rather than lifted, because his slope cells are halves of a
16 px triangle and a field of one half repeats into diagonal stripes. This is a period-8
version in his own tones and his own diagonal: a grey plane meeting a navy plane, seamless
left-to-right and top-to-bottom, so it tiles into faceted rock at any size of mass.
"""
import os, sys
from PIL import Image

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(_HERE, "..", "..", "roguemote", "authoring")))
import blob47                                                  # noqa: E402

TS = 8
COLS, ROWS = 16, 3               # Studio's grid for a blob47 set at 8 px

# His rock, sampled from the master so the tones are his and not an approximation.
NAVY   = (29, 43, 83)
LTGREY = (194, 195, 199)
WHITE  = (255, 241, 232)
DKGREY = (95, 87, 79)


def _blank():
    im = Image.new("RGBA", (TS, TS), (0, 0, 0, 0))
    return im, im.load()


def _slope():
    """The interior: a grey plane meeting a navy plane on his diagonal, period 8.

    Seamless in both axes — the diagonal enters at one corner and leaves at the
    opposite one — so a mass of any shape reads as continuous faceted rock instead of
    as a repeating half-triangle.
    """
    im, px = _blank()
    for y in range(TS):
        for x in range(TS):
            # the fold runs corner to corner; light above it, dark below
            px[x, y] = (LTGREY if x + y < TS else NAVY) + (255,)
    # a lit crease along the fold, which is what makes it read as an edge and not a wipe
    for i in range(TS):
        px[i, TS - 1 - i] = WHITE + (255,)
    return im


def _cap():
    """The top of a mass: snow on the light plane, rock below."""
    im, px = _blank()
    for y in range(TS):
        for x in range(TS):
            px[x, y] = (WHITE if y < 3 else (LTGREY if x + y < TS else NAVY)) + (255,)
    for i in range(TS):
        if TS - 1 - i >= 3:
            px[i, TS - 1 - i] = WHITE + (255,)
    return im


def _foot():
    """The bottom of a mass: his snowline, banded, sitting on dark rock."""
    im, px = _blank()
    for y in range(TS):
        for x in range(TS):
            px[x, y] = (NAVY if y > 4 else (LTGREY if x + y < TS else NAVY)) + (255,)
    for x in range(0, TS, 2):                 # the banded snowline
        px[x, 5] = LTGREY + (255,)
        px[x + 1, 6] = DKGREY + (255,)
    return im


def build(nvar=1):
    """A 47-cell blob47 set where the cell's art is chosen by WHERE IT SITS.

    The fringe is left to the band machinery; this is the opaque body of the mountain
    layer, so every cell is fully painted.
    """
    cap, slope, foot = _cap(), _slope(), _foot()
    sheet = Image.new("RGBA", (COLS * TS, ROWS * TS * nvar), (0, 0, 0, 0))
    for v in range(nvar):
        for c, mask in enumerate(blob47.CANON):
            open_e = blob47.edges_open(mask)
            art = cap if open_e["N"] else (foot if open_e["S"] else slope)
            sheet.paste(art, ((c % COLS) * TS, (c // COLS) * TS + v * ROWS * TS))
    return sheet
