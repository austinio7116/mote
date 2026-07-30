"""Motebox — a road network drawn as a transparent OVERLAY.

WHY NOT A BIOME. Roads used to be B_ROAD: a terrain that replaced the ground, so
paving a cell erased the grass, the sand or the field that was there. That is wrong
twice over — a road lies ON the land, and a network needs to know its neighbours to
draw a corner. So roads now live in their own byte per cell and draw as colour-keyed
sprites over the terrain, which is the only layer in this engine that can be
transparent.

WHY 16 CELLS AND NOT 47. A blob47 set answers "which of my eight neighbours match",
which is the right question for an AREA — a lake, a forest, a field. A road is not an
area, it is a GRAPH: what matters is only which of the four cardinals it continues
into. Sixteen cells covers every case exactly — one isolated stone, four dead ends,
two straights, four corners, four T-junctions and a crossroads — with no redundancy
and no guessing.

SUB-TILE SHAPES. The carriageway is four pixels of an eight-pixel tile with a one
pixel kerb either side, so the road is genuinely narrower than the cell it sits in.
That is what lets a corner round properly and a junction have a shoulder, instead of
every road being a full-width block with hard steps — and it is why the ground shows
along both sides of every road in the world.
"""
import os
from PIL import Image

TS = 8
N, E, S, W = 1, 2, 4, 8

# The carriageway occupies the middle four pixels; the kerb is derived, never drawn
# by hand, so it follows whatever shape the arms make.
LO, HI = 2, 5                      # inclusive carriageway bounds


def cell(mask, surface, kerb, stone, line=None):
    """One 8x8 road cell for a four-neighbour mask.

    `line`, if given, paints a dashed centre line down a through-road — the one thing that
    says tarmac rather than stone, and the reason the grades below end where they do. It is a
    SEPARATE colour from `stone` on purpose: `stone` is the paving speckle spread over the
    whole surface, and the first version reused it for the line, so a near-white marking
    colour dusted the entire road and the result read as gravel rather than as lane markings.
    """
    im = Image.new("RGBA", (TS, TS), (0, 0, 0, 0))
    px = im.load()
    road = set()

    # the junction block is always there, so an isolated road is still a paving stone
    for y in range(LO, HI + 1):
        for x in range(LO, HI + 1):
            road.add((x, y))
    # one arm per open direction, out to the tile edge so neighbours meet flush
    if mask & N:
        for y in range(0, LO):
            for x in range(LO, HI + 1): road.add((x, y))
    if mask & S:
        for y in range(HI + 1, TS):
            for x in range(LO, HI + 1): road.add((x, y))
    if mask & W:
        for x in range(0, LO):
            for y in range(LO, HI + 1): road.add((x, y))
    if mask & E:
        for x in range(HI + 1, TS):
            for y in range(LO, HI + 1): road.add((x, y))

    # ROUND THE INSIDE OF A CORNER. Where two arms meet at right angles the inner
    # corner pixel is cut back, which is the whole difference between a road that
    # bends and two roads that collide.
    for a, b, (cx, cy) in ((N, W, (LO, LO)), (N, E, (HI, LO)),
                           (S, W, (LO, HI)), (S, E, (HI, HI))):
        if (mask & a) and (mask & b):
            road.discard((cx, cy))

    # the kerb is every transparent pixel touching the carriageway
    for (x, y) in sorted(road):
        px[x, y] = surface + (255,)
    for y in range(TS):
        for x in range(TS):
            if (x, y) in road:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                if (x + dx, y + dy) in road:
                    px[x, y] = kerb + (255,)
                    break

    # a paving pattern, sparse and regular, so a long road is not a flat ribbon
    for (x, y) in sorted(road):
        if (x * 3 + y * 5) % 7 == 0:
            px[x, y] = stone + (255,)

    # THE CENTRE LINE, dashed, DOWN EVERY ARM — so it bends round a corner and forks at a
    # junction, which is what road markings actually do.
    #
    # Two earlier versions were wrong. The first reused `stone` for the line, so a near-white
    # marking colour dusted the whole surface through the paving speckle and the road read as
    # gravel. The second fixed that by painting markings ONLY on straights — clean, but a town
    # of corners and T-junctions then had almost no markings at all, which is just a different
    # kind of wrong.
    #
    # The line runs from the centre of the tile out along each open arm, on the tile's centre
    # pixel, dashed on a period that divides the tile — so it is continuous across many tiles,
    # turns through a corner as an L, and forks at a tee. Where two arms' lines meet at the
    # centre they simply overlap, which is exactly the join a real junction has painted on it.
    if line:
        # The carriageway is x,y in [2,5], so its centre falls between 3 and 4 and the line
        # has to sit on one side or the other. ABOVE centre on both axes: in this near-top-down
        # view a horizontal line one pixel high reads better sitting up against the far kerb
        # than down against the near one, which is where the eye expects the road's surface to
        # catch the light.
        CX, CY = 3, 3
        if mask & N:
            for y in range(0, CY + 1):
                if (y % 4) < 2: px[CX, y] = line + (255,)
        if mask & S:
            for y in range(CY, TS):
                if (y % 4) < 2: px[CX, y] = line + (255,)
        if mask & W:
            for x in range(0, CX + 1):
                if (x % 4) < 2: px[x, CY] = line + (255,)
        if mask & E:
            for x in range(CX, TS):
                if (x % 4) < 2: px[x, CY] = line + (255,)
    return im


# --- THE FIVE GRADES ----------------------------------------------------------------
# A road should date a town the same way its buildings do. The materials are the real
# sequence — beaten earth, then set cobbles, then dressed flagstones, then asphalt, then
# asphalt with paint on it — and each is a row of the sheet, so the C side picks a row from
# the owning kingdom's tech and every road in the realm modernises at once.
#
# The surfaces get progressively DARKER and less saturated, which is both true and useful:
# an early road has to read against grass and sand, and a late one has to read as a machine-
# made surface, so the contrast flips from warm-on-green to dark-on-anything.
GRADES = [
    # name        surface           kerb              paving speckle    centre line
    ("track",    (196, 150, 104), (150, 104,  66), (168, 126,  86), None),
    ("cobble",   (172, 168, 160), (120, 112, 104), (206, 202, 194), None),
    ("flagstone",(198, 196, 190), (140, 136, 130), (226, 224, 218), None),
    # asphalt is a MADE surface, so its speckle is barely there — the texture that reads as
    # cobbles on stone reads as potholes on tarmac
    ("tarmac",   ( 74,  74,  80), ( 52,  52,  58), ( 84,  84,  90), None),
    ("marked",   ( 70,  70,  76), ( 48,  48,  54), ( 80,  80,  86), (236, 234, 224)),
]


def build():
    """Sixteen cells per row, one row per grade: mask = N | E<<1 | S<<2 | W<<3."""
    sheet = Image.new("RGBA", (TS * 16, TS * len(GRADES)), (0, 0, 0, 0))
    for g, (_n, surface, kerb, stone, line) in enumerate(GRADES):
        for m in range(16):
            sheet.paste(cell(m, surface, kerb, stone, line), (m * TS, g * TS))
    return sheet
