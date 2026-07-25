#!/usr/bin/env python3
"""Work out which blob47 configuration each source tile actually draws.

The source lays each terrain out as a row of blocks -- some 3x3, some 2x3 -- so
parsing "blocks" is fragile. Instead every tile is classified independently from
its own pixels, which recovers the mask -> tile mapping whatever the layout is.

Per tile:
  * an edge is CLOSED if that outermost line is mostly fill colour: the terrain
    runs straight into the neighbour, so that neighbour is same-terrain.
  * an edge is OPEN if it is transparent/border instead: the terrain stops here.
  * for a corner whose two cardinals are both CLOSED, border colour inside the
    corner box means the diagonal is NOT same-terrain -- a concave corner.
That is exactly the reduced blob47 mask, so each tile lands on a canonical cell.

    python3 map_blob47.py                 # report coverage for every terrain
"""
import os, sys
from PIL import Image

import blob47
from blob47 import N, NE, E, SE, S, SW, W, NW

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = Image.open(os.path.join(HERE, "source_tileset.png"))
IDX, PAL = SRC.load(), SRC.getpalette()
TS = 8
TRANSPARENT = 0

# Terrain bands to map. fill/border are palette indices, read off the art.
BANDS = {
    "wall_brick": dict(rect=(0, 35, 15, 37), fill=5, border=6, extra_fill=(1,)),
    "hedge":      dict(rect=(0, 29, 15, 31), fill=3, border=11, extra_fill=()),
}


def tile(c, r):
    return [[IDX[c * TS + x, r * TS + y] for x in range(TS)] for y in range(TS)]


def classify(t, fill, border, extra_fill=()):
    """-> (reduced blob47 mask, diagnostics) or None if the tile is blank."""
    flat = [v for row in t for v in row]
    if all(v == TRANSPARENT for v in flat):
        return None, "empty"

    # An edge is OPEN when the border colour runs along it. Testing for *fill*
    # instead does not work: brick uses navy both as interior mortar and as the
    # shadow row under its bottom highlight, so a bottom edge looks "full of
    # fill" and reads as closed. Border presence is unambiguous.
    # Check the outer TWO lines, because the bottom border sits at y=6 (with the
    # shadow at y=7) and the hedge insets its border 1px behind a transparent rim.
    # Sample only the CENTRE of each edge. A concave-corner mark sits at the
    # corners and bleeds into the edge lines -- hedge's marks are 2px wide, so a
    # full-width sample of an interior tile reads 50% border and looks open.
    mid = range(TS // 2 - 1, TS // 2 + 1)

    def frac(px):
        s = [px[i] for i in mid]
        return sum(1 for v in s if v == border) / len(s)

    def openness(lines):
        return max(frac(l) for l in lines)

    col = lambda x: [t[y][x] for y in range(TS)]
    top   = openness([t[0], t[1]])
    bot   = openness([t[TS - 1], t[TS - 2]])
    left  = openness([col(0), col(1)])
    right = openness([col(TS - 1), col(TS - 2)])
    cN, cS = top < 0.5, bot < 0.5          # closed = terrain continues outward
    cW, cE = left < 0.5, right < 0.5

    mask = 0
    if cN: mask |= N
    if cE: mask |= E
    if cS: mask |= S
    if cW: mask |= W

    # A corner bit survives reduction only when both its cardinals are closed.
    # Border colour sitting in the corner box means the diagonal is empty.
    box = 3
    for bit, (a, b), (xs, ys) in (
            (NW, (N, W), (range(box), range(box))),
            (NE, (N, E), (range(TS - box, TS), range(box))),
            (SW, (S, W), (range(box), range(TS - box, TS))),
            (SE, (S, E), (range(TS - box, TS), range(TS - box, TS)))):
        if (mask & a) and (mask & b):
            marked = any(t[y][x] == border or t[y][x] == TRANSPARENT
                         for y in ys for x in xs)
            if not marked:
                mask |= bit          # diagonal is same-terrain too
    return blob47.reduce_mask(mask), f"N{top:.2f} E{right:.2f} S{bot:.2f} W{left:.2f}"


def map_band(name, spec):
    c0, r0, c1, r1 = spec["rect"]
    found = {}           # reduced mask -> list of (c,r)
    blanks = []
    for r in range(r0, r1 + 1):
        for c in range(c0, c1 + 1):
            m, _ = classify(tile(c, r), spec["fill"], spec["border"], spec["extra_fill"])
            if m is None:
                blanks.append((c, r))
                continue
            found.setdefault(m, []).append((c, r))
    return found, blanks


def report():
    for name, spec in BANDS.items():
        found, blanks = map_band(name, spec)
        c0, r0, c1, r1 = spec["rect"]
        total = (c1 - c0 + 1) * (r1 - r0 + 1)
        covered = [m for m in blob47.CANON if m in found]
        missing = [m for m in blob47.CANON if m not in found]
        print(f"\n=== {name}  cols {c0}-{c1} rows {r0}-{r1}  ({total} tiles, {len(blanks)} blank) ===")
        print(f"distinct blob47 configs present in the art: {len(covered)} of 47")
        print(f"missing: {len(missing)}")
        dupes = {m: v for m, v in found.items() if len(v) > 1}
        print(f"configs drawn more than once: {len(dupes)}")
        print("\n cell  mask  tiles")
        for m in blob47.CANON:
            i = blob47.CANON.index(m)
            where = found.get(m)
            if where:
                print(f" {i:>4}  {m:>4}  " + " ".join(f"({c},{r})" for c, r in where[:6])
                      + ("" if len(where) <= 6 else f" +{len(where)-6}"))
            else:
                print(f" {i:>4}  {m:>4}  -- MISSING --   ({blob47.describe(m)})")


if __name__ == "__main__":
    report()


def map_rect(rect, fill, border, extra=()):
    c0, r0, c1, r1 = rect
    found = {}
    for r in range(r0, r1 + 1):
        for c in range(c0, c1 + 1):
            m, _ = classify(tile(c, r), fill, border, extra)
            if m is not None:
                found.setdefault(m, []).append((c, r))
    return found


def autodetect(rect):
    """Try candidate (fill, border) pairs. The correct pair is the one that makes
    the band a clean bijection onto the 47 configs -- a wrong pair collides."""
    from collections import Counter
    c0, r0, c1, r1 = rect
    cnt = Counter()
    for r in range(r0, r1 + 1):
        for c in range(c0, c1 + 1):
            for row in tile(c, r):
                cnt.update(row)
    cands = [v for v, _ in cnt.most_common(9) if v != TRANSPARENT]
    best = None
    for fill in cands:
        for border in cands:
            if fill == border:
                continue
            found = map_rect(rect, fill, border)
            n = len(found)
            dup = sum(len(v) - 1 for v in found.values())
            score = (n, -dup)
            if best is None or score > best[0]:
                best = (score, fill, border, n, dup)
    return best


# --- the canonical band layout -------------------------------------------
# Every terrain in this sheet uses the SAME 16x3 arrangement: relative position
# (col,row) within the band always holds the same blob47 configuration, with
# (15,2) left blank. Verified: the brick and hedge bands agree at all 48
# positions. Deriving the layout once and reusing it means a band does not have
# to be colour-classified at all -- which matters, because some terrains use a
# different border colour per edge (grass has a grassy top, dirt sides and a
# gold bottom) and no single-border-colour test can read them.

def derive_layout():
    """-> {(dc,dr): cell index} from the two independently verified bands."""
    maps = []
    for name in ("wall_brick", "hedge"):
        s = BANDS[name]
        c0, r0, c1, r1 = s["rect"]
        m = {}
        for r in range(r0, r1 + 1):
            for c in range(c0, c1 + 1):
                t = tile(c, r)
                if all(v == 0 for row in t for v in row):
                    continue
                mm, _ = classify(t, s["fill"], s["border"], s["extra_fill"])
                m[(c - c0, r - r0)] = blob47.CANON.index(mm)
        maps.append(m)
    if maps[0] != maps[1]:
        raise SystemExit("the two verified bands disagree on the layout")
    return maps[0]


LAYOUT = derive_layout()
BLANK_SLOT = (15, 2)


def band_tiles(c0, r0):
    """-> {cell index: 8x8 grid} for a band at (c0,r0), using the fixed layout."""
    return {cell: tile(c0 + dc, r0 + dr) for (dc, dr), cell in LAYOUT.items()}


def band_report(c0, r0):
    """-> (n filled cells, list of cell indices whose tile is blank)"""
    t = band_tiles(c0, r0)
    blank = [cell for cell, g in t.items() if all(v == 0 for row in g for v in row)]
    return len(t) - len(blank), blank
