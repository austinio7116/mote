#!/usr/bin/env python3
"""The engine's blob47 contract, reimplemented here so the authoring side and
the runtime cannot drift.

Mirrors sdk/mote_tile.h exactly:
  - neighbour bits, clockwise from North
  - mote__at_reduce():  keep the 4 cardinals; keep a corner ONLY if both of its
    adjacent cardinals are also set. That collapses 256 configs to 47.
  - MOTE_AT_BLOB47 cell order: walk masks 0..255 ascending, and assign the next
    cell index the first time each reduced mask is seen.

Everything downstream (sheet layout, .tileset LUT, previews) derives from
CANON here, so there is exactly one definition of "cell 23".
"""

N, NE, E, SE, S, SW, W, NW = 1, 2, 4, 8, 16, 32, 64, 128
CARD = N | E | S | W
BITNAME = [(N, "N"), (NE, "NE"), (E, "E"), (SE, "SE"),
           (S, "S"), (SW, "SW"), (W, "W"), (NW, "NW")]

# corner bit -> the two cardinals that must both be set for it to survive
CORNERS = {NE: (N, E), SE: (S, E), SW: (S, W), NW: (N, W)}


def reduce_mask(m):
    """sdk/mote_tile.h : mote__at_reduce()"""
    r = m & CARD
    for corner, (a, b) in CORNERS.items():
        if (m & corner) and (r & a) and (r & b):
            r |= corner
    return r


def canonical():
    """-> (list of 47 reduced masks in cell order, lut[256])"""
    idx, order = {}, []
    for m in range(256):
        r = reduce_mask(m)
        if r not in idx:
            idx[r] = len(order)
            order.append(r)
    lut = [idx[reduce_mask(m)] for m in range(256)]
    return order, lut


CANON, LUT = canonical()
assert len(CANON) == 47, len(CANON)


def describe(mask):
    """'N+E+NE' style description of which neighbours are same-terrain."""
    return "+".join(nm for bit, nm in BITNAME if mask & bit) or "isolated"


def edges_open(mask):
    """Which sides face empty space -- i.e. where this cell needs a border drawn."""
    return {"N": not (mask & N), "E": not (mask & E),
            "S": not (mask & S), "W": not (mask & W)}


def inner_corners(mask):
    """Corners needing an INNER (concave) notch: both cardinals are same-terrain
    but the diagonal is not. These are the tiles a plain nine-slice cannot make."""
    out = []
    for corner, (a, b) in CORNERS.items():
        if (mask & a) and (mask & b) and not (mask & corner):
            out.append(dict(CORNERS_NAME)[corner])
    return out


CORNERS_NAME = [(NE, "NE"), (SE, "SE"), (SW, "SW"), (NW, "NW")]


def report():
    order, lut = CANON, LUT
    print(f"blob47: {len(order)} distinct cells, lut has {len(lut)} entries\n")
    # how many raw masks fold into each cell
    from collections import Counter
    fold = Counter(lut)
    print(f"{'cell':>4}  {'mask':>3}  {'neighbours':<26} {'inner corners':<16} raw masks")
    for i, m in enumerate(order):
        ic = ",".join(inner_corners(m)) or "-"
        print(f"{i:>4}  {m:>3}  {describe(m):<26} {ic:<16} {fold[i]}")
    print(f"\ntotal raw masks accounted for: {sum(fold.values())}")
    n_inner = sum(1 for m in order if inner_corners(m))
    print(f"cells needing at least one inner corner: {n_inner}")
    print(f"cells that a 3x3 nine-slice can express:  {len(order) - n_inner}")


if __name__ == "__main__":
    report()
