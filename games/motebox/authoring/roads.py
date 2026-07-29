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


def cell(mask, surface, kerb, stone):
    """One 8x8 road cell for a four-neighbour mask."""
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
    return im


def build(surface, kerb, stone):
    """All sixteen cells in one row, indexed by mask: N | E<<1 | S<<2 | W<<3."""
    sheet = Image.new("RGBA", (TS * 16, TS), (0, 0, 0, 0))
    for m in range(16):
        sheet.paste(cell(m, surface, kerb, stone), (m * TS, 0))
    return sheet
