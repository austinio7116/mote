"""Motebox — the terrain as a STACK OF AUTOTILED BANDS, which is what the engine is for.

HOW IT WORKS. `scene2d_set_autotile_layers` takes one byte per cell where each bit is a
layer, and draws layer 0 first. The map is built CUMULATIVELY: a cell in band B sets bits
0..B. So layer L covers every cell at band L or above, the engine autotiles it against
its own bit, and its edge falls exactly where that band's territory ends.

Each band's tile is OPAQUE in the interior and fades to TRANSPARENT through an organic
fringe. That is the whole trick, and it has a property I did not expect until I worked it
through: the fringe automatically reveals THE NEIGHBOUR'S OWN BAND COLOUR.

    a rock cell (bits 0..7) beside grass (bits 0..4)
      layers 0..4  — grass shares those bits, so this cell is INTERIOR to them: opaque
      layers 5..7  — grass lacks them, so this cell is an EDGE: fringe transparent
      => the fringe shows layer 4, which is green. The neighbour's colour, correctly.

    the same rock cell beside deep ocean (no bits at all)
      every layer is an edge here, so the fringe falls all the way through to the
      background — navy. Also correct, and it cost nothing.

Sprites could never do that without looking the neighbour up per cell. Here the engine
does it as a side effect of the stack being a stack.

WHY THE FRINGE IS ORGANIC. A cell may only paint its own eight by eight pixels, so in a
single opaque layer the colour boundary is pinned to the tile grid and every coast is a
staircase of straight segments. The fringe is what unpins it: the transparent part eats
into the cell on a wandering profile, so the visible edge owes nothing to the grid. Each
profile starts and ends at the same depth, so tiles join seamlessly along a run and a
long shore reads as one curve.
"""
import os, sys
from PIL import Image

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
sys.path.insert(0, os.path.abspath(os.path.join(_HERE, "..", "..", "roguemote", "authoring")))
import blob47                                                  # noqa: E402
import palette as P                                            # noqa: E402

TS = 8
COLS, ROWS = 16, 3                 # Studio's grid for a blob47 set at 8 px

# How deep the transparent fringe eats in, per pixel along an edge. Ends equal so
# neighbouring tiles meet; the middle varies. Three is the shallowest that can carry the
# lit lip AND still leave body, which is the same constraint the sprite overlay had.
PROFILES = (
    (3, 4, 5, 5, 4, 4, 3, 3),
    (3, 3, 4, 5, 5, 4, 4, 3),
    (3, 4, 4, 3, 4, 5, 4, 3),
    (3, 3, 4, 4, 5, 5, 4, 3),
)

_CORNERS = (
    ("NW", blob47.N, blob47.W, blob47.NW, (0, 0),           (1, 1)),
    ("NE", blob47.N, blob47.E, blob47.NE, (TS - 1, 0),      (-1, 1)),
    ("SW", blob47.S, blob47.W, blob47.SW, (0, TS - 1),      (1, -1)),
    ("SE", blob47.S, blob47.E, blob47.SE, (TS - 1, TS - 1), (-1, -1)),
)


def _prof(i, flip=False):
    d = PROFILES[i % len(PROFILES)]
    return tuple(reversed(d)) if flip else d


def cell(mask, tones, interior_fn=None, profile=0):
    """One band cell.

    `mask` is the eight-neighbour mask of "the neighbour is in this band too" — so a set
    bit means keep going, and a clear bit means this side is the band's edge and gets a
    fringe.

    `tones` runs body-first: tones[0] fills the interior and the rest are the lit lip,
    laid inward from the fringe so the band brightens as it approaches its own edge.
    """
    im = Image.new("RGBA", (TS, TS), (0, 0, 0, 0))
    px = im.load()
    # start opaque: the interior is the band's own body, textured if it wants to be
    if interior_fn is not None:
        im.paste(interior_fn(profile), (0, 0))
        px = im.load()
    else:
        for y in range(TS):
            for x in range(TS):
                px[x, y] = tones[0] + (255,)

    open_e = blob47.edges_open(mask)          # True where this side is an edge
    cut = {}

    def eat(x, y, d):
        if 0 <= x < TS and 0 <= y < TS:
            cut[(x, y)] = max(cut.get((x, y), 0), d)

    if open_e["N"]:
        for x, d in enumerate(_prof(profile)):
            for y in range(d): eat(x, y, d - y)
    if open_e["S"]:
        for x, d in enumerate(_prof(profile + 1, True)):
            for y in range(d): eat(x, TS - 1 - y, d - y)
    if open_e["W"]:
        for y, d in enumerate(_prof(profile + 2)):
            for x in range(d): eat(x, y, d - x)
    if open_e["E"]:
        for y, d in enumerate(_prof(profile + 3, True)):
            for x in range(d): eat(TS - 1 - x, y, d - x)

    # CORNERS. edges_open() says which cardinals are this band's EDGE (it stops there);
    # `mask & diag` says whether it carries on diagonally.
    for name, _ca, _cb, diag, (cx, cy), (ix, iy) in _CORNERS:
        a, b = name[0], name[1]
        if open_e[a] and open_e[b]:
            # OUTSIDE CORNER: the band ends on both sides, so round it off. Without this
            # every convex corner in the world is a right angle, which is exactly why the
            # boundaries read as rectilinear before any of this was layered.
            for k in range(5):
                for j in range(5 - k):
                    eat(cx + ix * k, cy + iy * j, 5 - max(k, j))
        elif not open_e[a] and not open_e[b] and not (mask & diag):
            # INNER CORNER: it carries on along both cardinals but NOT across the
            # diagonal, so that corner belongs to the neighbour poking in. These eight
            # cells are the entire reason a nine-slice looks wrong and a blob47 does not.
            for k in range(2):
                for j in range(2 - k):
                    eat(cx + ix * k, cy + iy * j, 2 - max(k, j))

    # The lip is three tones deep, not one, which is what the extended palette bought:
    # the band brightens as it approaches its own edge and then goes transparent, so a
    # boundary is a graded shore rather than a body with a rim drawn on it.
    top = len(tones) - 1
    for (x, y), d in cut.items():
        if d >= 4:
            px[x, y] = (0, 0, 0, 0)                       # the fringe: let the band below show
        else:
            px[x, y] = tones[min(top, 4 - d)] + (255,)
    return im


def grain(tones, seed, n=3):
    """An interior with GRAIN: a few pixels of the band's own lighter tones on a fixed
    lattice, so a continent has surface without having a pattern.

    This is what the extended palette bought. The last attempt at grain had to borrow a
    colour from another family — NAVY on DKGREEN grass — because there was no near
    neighbour in the sixteen, and a meadow came out speckled with near-black dots. With
    tones interpolated inside one family the marks are a step and not a drop, so they
    read as light catching a surface.
    """
    def fn(profile):
        im = Image.new("RGBA", (TS, TS), tones[0] + (255,))
        px = im.load()
        h = (seed * 2654435761 + profile * 40503) & 0xFFFFFFFF
        for i in range(n):
            h = (h * 1103515245 + 12345) & 0xFFFFFFFF
            x, y = (h >> 8) % TS, (h >> 16) % TS
            px[x, y] = tones[1 + (i % (len(tones) - 1))] + (255,)
        return im
    return fn


def build(tones, interior_fn=None, nvar=4):
    """A 47-cell band set, 16 x 3 per variant block — Studio's grid, so the editor and
    the game agree on where cell 23 is."""
    sheet = Image.new("RGBA", (COLS * TS, ROWS * TS * nvar), (0, 0, 0, 0))
    for v in range(nvar):
        for c, mask in enumerate(blob47.CANON):
            sheet.paste(cell(mask, tones, interior_fn, profile=v),
                        ((c % COLS) * TS, (c // COLS) * TS + v * ROWS * TS))
    return sheet


def write_tileset(tsets_dir, name, sheet, nvar=2, vweight=None):
    """A .tileset sidecar, because a band IS a Blob 47 rule tile and belongs in Studio."""
    os.makedirs(tsets_dir, exist_ok=True)
    sheet.save(os.path.join(tsets_dir, name + ".png"))
    vw = (list(vweight or []) + [1] * 8)[:8]
    with open(os.path.join(tsets_dir, name + ".tileset"), "w") as f:
        f.write("sheet tilesets/%s.png\n" % name)
        f.write("tile %d\n" % TS)
        f.write("type 0\n")                    # Blob 47, stated rather than defaulted
        f.write("edge 1\n")                    # off-map counts as more of the same band
        f.write("nvar %d\n" % nvar)
        f.write("cols %d\nrows %d\n" % (COLS, ROWS * nvar))
        f.write("lut " + " ".join(str(v) for v in blob47.LUT) + "\n")
        f.write("xform " + " ".join(["0"] * 256) + "\n")
        f.write("vweight " + " ".join(str(v) for v in vw) + "\n")

def stone(tones, seed):
    """An interior for ROCK, which needs more than grain.

    The rock band was flat LTGREY with three lighter dots, and a screenshot of a mountain
    interior came out as grey nothing — the hillshade gives it large-scale light but there
    was no surface for the light to fall on. This lays angular PATCHES, two or three pixels
    on a side, in the band's own tones: a broken plane, seamless because every patch is
    wrapped at the tile edge.

    Angular on purpose. The grass grain can be dots because a meadow is soft; stone breaks
    along planes, and a field of round dots on grey reads as gravel-in-a-driveway rather
    than as mountain.
    """
    def fn(profile):
        im = Image.new("RGBA", (TS, TS), tones[0] + (255,))
        px = im.load()
        h = (seed * 2246822519 + profile * 3266489917) & 0xFFFFFFFF
        for i in range(6):
            h = (h * 1103515245 + 12345) & 0xFFFFFFFF
            x, y = (h >> 9) % TS, (h >> 17) % TS
            w = 2 + ((h >> 25) & 1)
            ht = 1 + ((h >> 27) & 1)
            t = tones[1 + (i % (len(tones) - 1))] if (i & 1) else tones[0]
            # A crack every third patch. It is a STEP and not a drop: the first version
            # went 34 units darker and the mountain came out as grey polka dots, because
            # a high-contrast two-pixel mark on flat ground reads as an object.
            if i % 3 == 2:
                t = tuple(max(0, c - 16) for c in tones[0])
            for dy in range(ht):
                for dx in range(w):
                    px[(x + dx) % TS, (y + dy) % TS] = t + (255,)
        return im
    return fn

def ripples(tones, seed):
    """An interior for WATER: short horizontal dashes rather than dots.

    Once the deep ocean got a real surface (mb_draw_sea_band), the shallow band beside it
    was the flattest thing on screen — a bright blue field with three single-pixel specks.
    A dash reads as a ripple crest because that is the shape a wave front makes seen from
    above; a dot reads as a speck of dirt.
    """
    def fn(profile):
        im = Image.new("RGBA", (TS, TS), tones[0] + (255,))
        px = im.load()
        h = (seed * 1103515245 + profile * 2654435761) & 0xFFFFFFFF
        for i in range(3):
            h = (h * 1103515245 + 12345) & 0xFFFFFFFF
            x, y = (h >> 9) % TS, (h >> 17) % TS
            for k in range(2 + ((h >> 25) & 1)):
                px[(x + k) % TS, y] = tones[1 + (i % (len(tones) - 1))] + (255,)
        return im
    return fn
