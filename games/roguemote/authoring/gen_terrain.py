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
    # The sheet stacks six bands three rows apart, alternating a solid-interior
    # terrain with a hollow-interior one (an outline style whose middle is meant
    # to show through). The hollow ones are 46/47: their interior cell is blank
    # by design, declared here rather than shipped silently.
    "hedge":        dict(at=(0, 29), edge=0, holes=(),
                         note="garden hedge maze; draws its own rim at the map border"),
    "wall_bone":    dict(at=(0, 32), edge=1, holes=(46,),
                         note="cream/bone outline wall; hollow interior"),
    "wall_brick":   dict(at=(0, 35), edge=1, holes=(),
                         note="dungeon stone-brick; seamless at the map border"),
    "wall_marble":  dict(at=(0, 38), edge=1, holes=(46,),
                         note="white marble/temple wall with navy trim; hollow interior"),
    "floor_jungle": dict(at=(0, 41), edge=0, holes=(),
                         note="jungle grass with dirt sides and gold steps"),
    "wall_aztec":   dict(at=(0, 44), edge=1, holes=(46,),
                         note="aztec temple facade; hollow interior"),

    # The monochrome bands are line art, not filled terrain, so they are EDGE16
    # (4-cardinal connections: bars, corners, T-junctions, crosses, ends) rather
    # than blob47. 16 cells on a 4x4 sheet.
    "wall_blueprint": dict(at=(0, 50), edge=0, kind="edge16", holes=(),
                           note="thin white floor-plan lines; 16/16"),
    "wall_plaster":   dict(at=(0, 53), edge=0, kind="edge16", holes=(0,),
                           note="thick white wall with drop shadow; no isolated-tile "
                                "cell in the source, so a lone tile draws nothing"),
}
# The monochrome bands below (rows 46-55: dither, blueprint, white furniture) do
# NOT use this layout -- best fit is 43/47 and they render as scattered
# fragments. They stay raw subsheets.


def to_rgba(grid):
    im = Image.new("RGBA", (TS, TS))
    px = im.load()
    for y in range(TS):
        for x in range(TS):
            i = grid[y][x]
            px[x, y] = (0, 0, 0, 0) if i == TRANSPARENT else (PAL[i*3], PAL[i*3+1], PAL[i*3+2], 255)
    return im


def build_edge16(name, spec):
    """-> (4x4 sheet, {cell: grid}, {cell: (c,r)}) for a line-art band."""
    c0, r0 = spec["at"]
    cells = mp.edge16_tiles(c0, r0)
    missing = sorted(set(range(16)) - set(cells))
    undeclared = sorted(set(missing) - set(spec.get("holes", ())))
    if undeclared:
        raise SystemExit(
            f"{name}: EDGE16 band at {(c0, r0)} is missing cell(s) {undeclared}. "
            f"Either the band start is wrong or these belong in holes=().")
    sheet = Image.new("RGBA", (4 * TS, 4 * TS))
    for v, g in cells.items():
        sheet.paste(to_rgba(g), ((v % 4) * TS, (v // 4) * TS))
    return sheet, cells, missing


def build(name, spec):
    """-> (sheet image, [47 pixel grids], mapping mask -> (c,r))

    Reads the band through the shared layout rather than re-classifying it. Any
    blank cell the terrain has not declared in `holes` is a hard error, so a
    mis-specified band start cannot silently ship an atlas full of gaps.
    """
    c0, r0 = spec["at"]
    tiles = mp.band_tiles(c0, r0)
    blank = [cell for cell, g in tiles.items() if all(v == 0 for row in g for v in row)]
    undeclared = sorted(set(blank) - set(spec.get("holes", ())))
    if undeclared:
        raise SystemExit(
            f"{name}: band at {(c0, r0)} has {len(undeclared)} undeclared blank cell(s) "
            f"{undeclared[:8]}. Either the band start is wrong or these belong in holes=().")
    inv = {cell: pos for pos, cell in mp.LAYOUT.items()}
    where = {m: (c0 + inv[i][0], r0 + inv[i][1]) for i, m in enumerate(blob47.CANON)}
    grids = [tiles[i] for i in range(47)]
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
        lut = mp.EDGE16_LUT if spec.get("kind") == "edge16" else blob47.LUT
        f.write("lut " + " ".join(str(v) for v in lut) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        f.write("vweight " + " ".join("1" for _ in range(8)) + "\n")


def main(write=True):
    for name, spec in TERRAINS.items():
        c0, r0 = spec["at"]
        if spec.get("kind") == "edge16":
            sheet, cells, missing = build_edge16(name, spec)
            if write:
                os.makedirs(os.path.join(GAME, "tilesets"), exist_ok=True)
                sheet.save(os.path.join(GAME, "tilesets", name + ".png"))
                write_tileset(name, spec)
            print(f"[edge16] {name:15s} {len(cells)}/16 cells from source art at "
                  f"cols {c0}-{c0+5} rows {r0}-{r0+2}  edge={spec['edge']}"
                  + (f"  ({len(missing)} declared hole)" if missing else ""))
            continue
        sheet, grids, where = build(name, spec)
        if write:
            os.makedirs(os.path.join(GAME, "tilesets"), exist_ok=True)
            sheet.save(os.path.join(GAME, "tilesets", name + ".png"))
            write_tileset(name, spec)
        holes = spec.get("holes", ())
        print(f"[blob47] {name:13s} {47-len(holes)}/47 cells from source art at "
              f"cols {c0}-{c0+15} rows {r0}-{r0+2}  edge={spec['edge']}"
              + (f"  ({len(holes)} declared hole)" if holes else ""))


if __name__ == "__main__":
    main(write="--report" not in sys.argv)
