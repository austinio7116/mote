#!/usr/bin/env python3
"""Build blob47 autotile sheets + rulesets from the source tileset's own art.

The CC0 sheet draws a COMPLETE 47-tile blob set for each of its two real
terrains, laid out as a row of blocks of mixed width (some 3x3, some 2x3) across
16 columns x 3 rows -- 48 cells, one blank, 47 tiles. Every one of the 47
configurations is drawn exactly once, by hand.

We therefore do not synthesise anything. map_blob47.py classifies each source
tile from its own pixels (which edges carry the border, which corners carry a
concave mark), which recovers the mask -> tile mapping without having to parse
the block layout. That mapping is a clean bijection onto the 47 canonical cells,
and the sheet is just those tiles reordered into cell order.

    python3 gen_terrain.py            # write tilesets/<name>.png + .tileset
    python3 gen_terrain.py --report   # mapping summary only, no writes
"""
import os, sys
from PIL import Image

import blob47
import map_blob47 as mp

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
TS = mp.TS
PAL = mp.PAL
TRANSPARENT = mp.TRANSPARENT

TERRAINS = {
    "wall_brick": dict(band="wall_brick", edge=1,
                       note="dungeon stone-brick; seamless at the map border"),
    "hedge":      dict(band="hedge", edge=0,
                       note="garden hedge maze; draws its own rim at the map border"),
}


def to_rgba(grid):
    im = Image.new("RGBA", (TS, TS))
    px = im.load()
    for y in range(TS):
        for x in range(TS):
            i = grid[y][x]
            px[x, y] = (0, 0, 0, 0) if i == TRANSPARENT else (PAL[i*3], PAL[i*3+1], PAL[i*3+2], 255)
    return im


def build(name, spec):
    """-> (sheet image, [47 pixel grids], mapping mask -> (c,r))

    Raises if the band is not a clean bijection onto the 47 cells: a missing
    config would leave a hole in the atlas, and a duplicated one means the
    classifier is misreading the art.
    """
    band = mp.BANDS[spec["band"]]
    found, blanks = mp.map_band(spec["band"], band)
    missing = [m for m in blob47.CANON if m not in found]
    dupes = {m: v for m, v in found.items() if len(v) > 1}
    if missing or dupes:
        raise SystemExit(
            f"{name}: band is not a clean 47-way mapping -- "
            f"{len(missing)} config(s) missing, {len(dupes)} drawn more than once. "
            f"Do not ship this; fix the classifier or the band rect first.")

    where = {m: found[m][0] for m in blob47.CANON}
    grids = [mp.tile(*where[m]) for m in blob47.CANON]
    sheet = Image.new("RGBA", (47 * TS, TS))
    for i, g in enumerate(grids):
        sheet.paste(to_rgba(g), (i * TS, 0))
    return sheet, grids, where


def write_tileset(name, spec, nvar=1):
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
        sheet, grids, where = build(name, spec)
        band = mp.BANDS[spec["band"]]
        c0, r0, c1, r1 = band["rect"]
        if write:
            os.makedirs(os.path.join(GAME, "tilesets"), exist_ok=True)
            sheet.save(os.path.join(GAME, "tilesets", name + ".png"))
            write_tileset(name, spec)
        print(f"[blob47] {name:11s} 47/47 cells from source art "
              f"cols {c0}-{c1} rows {r0}-{r1}  edge={spec['edge']}")


if __name__ == "__main__":
    main(write="--report" not in sys.argv)
