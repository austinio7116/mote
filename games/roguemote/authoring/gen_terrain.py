#!/usr/bin/env python3
"""Build complete blob47 autotile sheets + rulesets from the source tileset.

The CC0 sheet only ever gives us a 3x3 nine-slice per terrain. A nine-slice can
express 16 of the 47 blob configurations; the other 31 need inner (concave)
corners, and some need borders on OPPOSITE sides at once (a 1-tile-wide spur
needs a left and a right edge together, which no single nine-slice cell has).

So all 47 cells are synthesised by compositing, per pixel, from the nine source
tiles. Nothing is hand-placed: border depths and corner radius are measured off
the art by diffing each edge tile against the centre, so the same code works for
any terrain added later. See find_nineslice.py for which source regions can
actually back a blob47 set -- most of the sheet's "wall" bands cannot.

    python3 gen_terrain.py            # write tilesets/<name>.png + .tileset
    python3 gen_terrain.py --report   # measurements only, no writes
"""
import os, sys
from PIL import Image

import blob47
from blob47 import N, NE, E, SE, S, SW, W, NW

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
SRC = Image.open(os.path.join(HERE, "source_tileset.png"))
IDX, PAL = SRC.load(), SRC.getpalette()
TS = 8
TRANSPARENT = 0

# Only regions that pass find_nineslice.py belong here. `blocks` lists the
# top-left of each 3x3; extra blocks become variant rows (nvar), which the
# engine picks per cell by position hash so big areas don't visibly repeat.
TERRAINS = {
    "wall_brick": dict(
        blocks=[(3, 35)], edge=1,
        note="dungeon stone-brick walls; seamless at the map border"),
    "hedge": dict(
        blocks=[(3, 29)], edge=0,
        note="garden hedge maze; draws its own rim at the map border"),
}
# No terrain here gets a second variant row, and that is deliberate. The sheet
# offers a near-twin block for both of these -- (0,35) for brick, (0,29) for
# hedge -- and both LOOK like variants (4% and 19% different). Both fail
# fill_is_clean(): each draws a bright post in every corner of its centre tile,
# and the hedge one is see-through as well. Since the engine picks a variant row
# per cell by position hash, either would sprinkle stray bright dots through the
# middle of solid walls. They are separate decorative terrains, not variants.


def tile_px(c, r):
    return [[IDX[c * TS + x, r * TS + y] for x in range(TS)] for y in range(TS)]


NS_KEYS = ["TL", "T", "TR", "L", "C", "R", "BL", "B", "BR"]


def load_nineslice(c0, r0):
    return {k: tile_px(c0 + i % 3, r0 + i // 3) for i, k in enumerate(NS_KEYS)}


def fill_is_clean(ns):
    """Is this block's centre tile usable as solid interior fill?

    A blob47 interior cell (mask 255) is drawn as the bare centre tile, so the
    centre must be pure fill: no border colour, no holes. Two blocks in this
    sheet look like variants but fail here -- both draw a bright post in each
    corner of their centre tile, which the engine would scatter through the
    middle of a solid wall as stray dots. Cheap check, caught a real bug.
    """
    outline = set(ns["T"][0]) - {TRANSPARENT}      # the top border band, by definition
    cols = {v for row in ns["C"] for v in row}
    return not (cols & outline) and TRANSPARENT not in cols, sorted(cols & outline)


def measure(ns):
    """Border depth per side + corner radius, derived from the art itself."""
    C = ns["C"]
    rows_diff = lambda a: [y for y in range(TS) if any(a[y][x] != C[y][x] for x in range(TS))]
    cols_diff = lambda a: [x for x in range(TS) if any(a[y][x] != C[y][x] for y in range(TS))]
    tr, br = rows_diff(ns["T"]), rows_diff(ns["B"])
    lc, rc = cols_diff(ns["L"]), cols_diff(ns["R"])
    m = dict(top=max(tr) + 1, bottom=TS - min(br), left=max(lc) + 1, right=TS - min(rc))
    rad = 0
    for key, (vk, hk) in (("TL", ("T", "L")), ("TR", ("T", "R")),
                          ("BL", ("B", "L")), ("BR", ("B", "R"))):
        a = ns[key]
        for y in range(TS):
            for x in range(TS):
                inV = y < m["top"] if vk == "T" else y >= TS - m["bottom"]
                inH = x < m["left"] if hk == "L" else x >= TS - m["right"]
                ref = ns[vk][y][x] if inV else (ns[hk][y][x] if inH else C[y][x])
                if a[y][x] != ref:
                    dx = x if hk == "L" else TS - 1 - x
                    dy = y if vk == "T" else TS - 1 - y
                    rad = max(rad, dx + 1, dy + 1)
    m["corner"] = rad
    return m


def compose(ns, m, mask, inner_radius):
    """The 8x8 tile for one reduced blob47 mask.

    Per pixel, pick which of the nine source tiles to read:
      convex corner (both its sides open)          -> that corner tile
      concave corner (both sides same, diagonal not) -> that corner tile, but only
                                                        within inner_radius, so it
                                                        reads as a small notch
      inside an open edge's depth                  -> that edge tile
      otherwise                                    -> the centre fill
    Opposite edges may both be open and both get drawn -- the case a nine-slice
    structurally cannot represent.
    """
    C = ns["C"]
    oN, oE, oS, oW = not (mask & N), not (mask & E), not (mask & S), not (mask & W)
    cNW = bool((mask & N) and (mask & W) and not (mask & NW))
    cNE = bool((mask & N) and (mask & E) and not (mask & NE))
    cSW = bool((mask & S) and (mask & W) and not (mask & SW))
    cSE = bool((mask & S) and (mask & E) and not (mask & SE))
    oc, ir = m["corner"], inner_radius

    out = [[0] * TS for _ in range(TS)]
    for y in range(TS):
        for x in range(TS):
            L_, R_ = x < oc, x >= TS - oc
            T_, B_ = y < oc, y >= TS - oc
            l_, r_ = x < ir, x >= TS - ir
            t_, b_ = y < ir, y >= TS - ir
            if   oN and oW and T_ and L_: src = ns["TL"]
            elif oN and oE and T_ and R_: src = ns["TR"]
            elif oS and oW and B_ and L_: src = ns["BL"]
            elif oS and oE and B_ and R_: src = ns["BR"]
            elif cNW and t_ and l_:       src = ns["TL"]
            elif cNE and t_ and r_:       src = ns["TR"]
            elif cSW and b_ and l_:       src = ns["BL"]
            elif cSE and b_ and r_:       src = ns["BR"]
            elif oN and y < m["top"]:            src = ns["T"]
            elif oS and y >= TS - m["bottom"]:   src = ns["B"]
            elif oW and x < m["left"]:           src = ns["L"]
            elif oE and x >= TS - m["right"]:    src = ns["R"]
            else:                                src = C
            out[y][x] = src[y][x]
    return out


def to_rgba(grid):
    im = Image.new("RGBA", (TS, TS))
    px = im.load()
    for y in range(TS):
        for x in range(TS):
            i = grid[y][x]
            px[x, y] = (0, 0, 0, 0) if i == TRANSPARENT else (PAL[i*3], PAL[i*3+1], PAL[i*3+2], 255)
    return im


def build(name, spec, inner_radius=None):
    """-> (sheet image, [per-variant [47 grids]], [per-variant measurements])"""
    variants, mets = [], []
    for blk in spec["blocks"]:
        ns = load_nineslice(*blk)
        ok, bad = fill_is_clean(ns)
        if not ok:
            raise SystemExit(
                f"{name}: block {blk} cannot back a blob47 set -- its centre tile is not "
                f"clean fill (border colour {bad} inside it, or transparent holes). "
                f"Interior cells would show stray marks.")
        m = measure(ns)
        ir = m["corner"] // 2 if inner_radius is None else inner_radius
        ir = max(1, ir)
        m["inner"] = ir
        variants.append([compose(ns, m, mask, ir) for mask in blob47.CANON])
        mets.append(m)
    sheet = Image.new("RGBA", (47 * TS, len(variants) * TS))
    for v, grids in enumerate(variants):
        for i, g in enumerate(grids):
            sheet.paste(to_rgba(g), (i * TS, v * TS))
    return sheet, variants, mets


def write_tileset(name, spec, nvar):
    d = os.path.join(GAME, "tilesets")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".tileset"), "w") as f:
        f.write(f"sheet tilesets/{name}.png\n")
        f.write("tile 8\n")
        f.write(f"edge {spec['edge']}\n")
        f.write(f"nvar {nvar}\n")
        f.write("lut " + " ".join(str(v) for v in blob47.LUT) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        f.write("vweight " + " ".join("1" for _ in range(8)) + "\n")


def main(write=True):
    for name, spec in TERRAINS.items():
        sheet, variants, mets = build(name, spec)
        m = mets[0]
        if write:
            os.makedirs(os.path.join(GAME, "tilesets"), exist_ok=True)
            sheet.save(os.path.join(GAME, "tilesets", name + ".png"))
            write_tileset(name, spec, len(variants))
        print(f"[blob47] {name:11s} 47 cells x {len(variants)} variants  "
              f"borders T{m['top']} B{m['bottom']} L{m['left']} R{m['right']}  "
              f"corner {m['corner']}px  inner notch {m['inner']}px  edge={spec['edge']}")


if __name__ == "__main__":
    main(write="--report" not in sys.argv)
