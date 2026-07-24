#!/usr/bin/env python3
"""Render evidence for the blob47 terrain sets.

Everything drawn here goes through resolve(), which is a line-for-line port of
the engine's draw_autotile(): build the 8-neighbour mask with
mote_autotile_mask(), look up at->lut[mask], then index the atlas with
cell%tpr / cell/tpr plus the variant row. If a picture here looks right, the
device will look the same.

    python3 preview_terrain.py            # write all evidence PNGs to /tmp
"""
import os
from PIL import Image, ImageDraw

import blob47
import gen_terrain as gt

OUT = "/tmp/roguemote_terrain"
TS = 8


# --- engine port ----------------------------------------------------------
def at_hash(x, y):
    h = ((x * 73856093) ^ (y * 19349663)) & 0xFFFFFFFF
    h ^= h >> 13
    return (h * 1274126177) & 0xFFFFFFFF


def variant_of(nvar, vweight, c, r):
    if nvar <= 1:
        return 0
    n = min(nvar, 8)
    total = sum(vweight[v] or 1 for v in range(n))
    pick = at_hash(c, r) % total
    for v in range(n - 1):
        w = vweight[v] or 1
        if pick < w:
            return v
        pick -= w
    return n - 1


def neighbour_mask(terrain, cols, rows, c, r, match, edge_is_solid):
    m = 0
    for dx, dy, bit in ((0, -1, blob47.N), (1, -1, blob47.NE), (1, 0, blob47.E),
                        (1, 1, blob47.SE), (0, 1, blob47.S), (-1, 1, blob47.SW),
                        (-1, 0, blob47.W), (-1, -1, blob47.NW)):
        cc, rr = c + dx, r + dy
        if cc < 0 or cc >= cols or rr < 0 or rr >= rows:
            same = edge_is_solid
        else:
            same = terrain[rr * cols + cc] == match
        if same:
            m |= bit
    return m


def resolve(terrain, cols, rows, sheet, nvar, edge_is_solid, match=1, vweight=None):
    """-> RGBA image, drawn exactly as draw_autotile() would."""
    vweight = vweight or [1] * 8
    tpr = sheet.width // TS
    img = Image.new("RGBA", (cols * TS, rows * TS), (0, 0, 0, 0))
    base_rows = (sheet.height // TS) // max(nvar, 1)
    for r in range(rows):
        for c in range(cols):
            if terrain[r * cols + c] != match:
                continue
            mask = neighbour_mask(terrain, cols, rows, c, r, match, edge_is_solid)
            cell = blob47.LUT[mask]
            fx, fy = (cell % tpr) * TS, (cell // tpr) * TS
            fy += variant_of(nvar, vweight, c, r) * base_rows * TS
            img.alpha_composite(sheet.crop((fx, fy, fx + TS, fy + TS)), (c * TS, r * TS))
    return img


# --- drawing helpers ------------------------------------------------------
BG = (26, 26, 32)
GRID = (58, 58, 70)


def upscale(im, z):
    return im.resize((im.width * z, im.height * z), Image.NEAREST)


def on_bg(im, z, grid=True):
    bg = Image.new("RGBA", (im.width * z, im.height * z), BG + (255,))
    if grid:
        d = ImageDraw.Draw(bg)
        for x in range(0, im.width * z + 1, TS * z):
            d.line([x, 0, x, bg.height], fill=GRID)
        for y in range(0, im.height * z + 1, TS * z):
            d.line([0, y, bg.width, y], fill=GRID)
    bg.alpha_composite(upscale(im, z))
    return bg


# --- 1. the 47-cell atlas, labelled --------------------------------------
def sheet_contact(name, sheet, nvar, z=6):
    cols = 12
    rows = (47 + cols - 1) // cols
    cw, ch = TS * z + 14, TS * z + 26
    W, H = cols * cw + 10, rows * ch * nvar + 30
    im = Image.new("RGBA", (W, H), BG + (255,))
    d = ImageDraw.Draw(im)
    d.text((6, 6), f"{name} - blob47 atlas, all 47 cells"
                   + (f" x {nvar} variant rows" if nvar > 1 else ""), fill=(240, 235, 225))
    tpr = sheet.width // TS
    for v in range(nvar):
        for i in range(47):
            gx, gy = i % cols, i // cols
            x = 8 + gx * cw
            y = 24 + (v * rows + gy) * ch
            cell = sheet.crop(((i % tpr) * TS, ((i // tpr) + v) * TS,
                               (i % tpr) * TS + TS, ((i // tpr) + v) * TS + TS))
            im.alpha_composite(upscale(cell, z), (x, y))
            d.rectangle([x - 1, y - 1, x + TS * z, y + TS * z], outline=(70, 70, 84))
            mask = blob47.CANON[i]
            d.text((x, y + TS * z + 2), f"{i}", fill=(240, 200, 90))
            d.text((x + 16, y + TS * z + 2), f"m{mask}", fill=(130, 130, 150))
    return im


# --- 2. every config shown in its own 3x3 neighbourhood -------------------
def config_matrix(name, sheet, nvar, edge, z=6):
    """For each of the 47 cells, build the 3x3 map that produces it and draw the
    whole neighbourhood, so the cell can be judged in context rather than alone."""
    cols = 8
    rows = (47 + cols - 1) // cols
    pad = 12
    bw, bh = 3 * TS * z, 3 * TS * z
    cw, ch = bw + pad, bh + pad + 22
    im = Image.new("RGBA", (cols * cw + 12, rows * ch + 34), BG + (255,))
    d = ImageDraw.Draw(im)
    d.text((6, 6), f"{name} - each of the 47 configs in its own 3x3 neighbourhood "
                   f"(centre cell is the one under test)", fill=(240, 235, 225))
    for i, mask in enumerate(blob47.CANON):
        gx, gy = i % cols, i // cols
        x, y = 8 + gx * cw, 26 + gy * ch
        # a 3x3 where the centre is terrain and neighbours follow the mask
        t = [0] * 9
        t[4] = 1
        for bit, (dx, dy) in ((blob47.N, (0, -1)), (blob47.NE, (1, -1)), (blob47.E, (1, 0)),
                              (blob47.SE, (1, 1)), (blob47.S, (0, 1)), (blob47.SW, (-1, 1)),
                              (blob47.W, (-1, 0)), (blob47.NW, (-1, -1))):
            if mask & bit:
                t[(1 + dy) * 3 + (1 + dx)] = 1
        # edge=0 here so the 3x3 is judged on its own, not padded by the border rule
        img = resolve(t, 3, 3, sheet, nvar, 0)
        im.alpha_composite(on_bg(img, z), (x, y))
        d.rectangle([x + TS * z - 1, y + TS * z - 1, x + 2 * TS * z, y + 2 * TS * z],
                    outline=(240, 200, 90))
        d.text((x, y + bh + 3), f"cell {i}  mask {mask}", fill=(170, 170, 190))
    return im


# --- 3. fringe-case maps --------------------------------------------------
def parse(art):
    """'#' = terrain, '.' = empty. -> (flat map, cols, rows)"""
    lines = [l for l in art.strip("\n").split("\n") if l.strip()]
    w = max(len(l) for l in lines)
    lines = [l.ljust(w) for l in lines]
    return [1 if ch == '#' else 0 for l in lines for ch in l], w, len(lines)


FRINGE = {
    "single isolated tile": """
....
.#..
....
""",
    "1-wide vertical spur": """
.....
..#..
..#..
..#..
.....
""",
    "1-wide horizontal spur": """
.......
.#####.
.......
""",
    "plus / cross": """
.....
..#..
.###.
..#..
.....
""",
    "diagonal touch only (checkerboard)": """
......
.#.#..
..#.#.
.#.#..
......
""",
    "L-shape (one inner corner)": """
.....
.##..
.#...
.....
""",
    "square with a 1-tile hole": """
.....
.###.
.#.#.
.###.
.....
""",
    "donut, 2-tile hole": """
......
.####.
.#..#.
.####.
......
""",
    "solid block (all inner cells)": """
......
.####.
.####.
.####.
......
""",
    "staircase": """
.......
.#.....
.##....
..##...
...##..
.......
""",
    "T-junction": """
.......
.#####.
...#...
...#...
.......
""",
    "two blocks touching at a corner": """
......
.##...
.##...
...##.
...##.
......
""",
    "comb / teeth": """
........
.#.#.#..
.######.
........
""",
    "hole in a corner of a block": """
......
..###.
.####.
.####.
......
""",
}


def fringe_sheet(name, sheet, nvar, edge, z=7):
    items = list(FRINGE.items())
    cols = 4
    rows = (len(items) + cols - 1) // cols
    cell_w = max(len(parse(a)[1] for a in [])) if False else 0
    # size cells to the widest map
    maxw = max(parse(a)[1] for _, a in items)
    maxh = max(parse(a)[2] for _, a in items)
    cw, ch = maxw * TS * z + 18, maxh * TS * z + 34
    im = Image.new("RGBA", (cols * cw + 12, rows * ch + 34), BG + (255,))
    d = ImageDraw.Draw(im)
    d.text((6, 6), f"{name} - fringe cases (edge_is_solid={edge})", fill=(240, 235, 225))
    for i, (label, art) in enumerate(items):
        t, w, h = parse(art)
        gx, gy = i % cols, i // cols
        x, y = 8 + gx * cw, 26 + gy * ch
        img = resolve(t, w, h, sheet, nvar, edge)
        im.alpha_composite(on_bg(img, z), (x, y))
        d.text((x, y + h * TS * z + 5), label, fill=(190, 190, 210))
    return im


# --- 4. a real dungeon, and the edge_is_solid comparison ------------------
DUNGEON = """
################################
#..............................#
#..####..######..####..........#
#..#..#..#....#..#..#..........#
#..#..####....####..#..#####...#
#..#..................#...#....#
#..#..####..#####..####...#....#
#..####..#..#...#..#......#....#
#........#..#...#..#..#####....#
#..#######..#####..#...........#
#..#.....................#####.#
#..#..###..####..######..#...#.#
#.....#.#..#..#..#....#..#...#.#
#..####.#..#..####....####...#.#
#..........#.................#.#
################################
"""


def dungeon_render(name, sheet, nvar, edge, z=4):
    t, w, h = parse(DUNGEON)
    return on_bg(resolve(t, w, h, sheet, nvar, edge), z, grid=False)


def edge_compare(name, sheet, nvar, z=7):
    t, w, h = parse("""
######
######
######
######
""")
    im = Image.new("RGBA", (2 * (w * TS * z + 16) + 12, h * TS * z + 46), BG + (255,))
    d = ImageDraw.Draw(im)
    d.text((6, 6), f"{name} - a fully solid map, off-map neighbours on/off",
           fill=(240, 235, 225))
    for i, e in enumerate((0, 1)):
        x = 8 + i * (w * TS * z + 16)
        im.alpha_composite(on_bg(resolve(t, w, h, sheet, nvar, e), z), (x, 26))
        d.text((x, 26 + h * TS * z + 5),
               f"edge_is_solid={e}  " + ("rim drawn" if e == 0 else "seamless"),
               fill=(190, 190, 210))
    return im


def main():
    os.makedirs(OUT, exist_ok=True)
    written = []
    for name, spec in gt.TERRAINS.items():
        sheet, variants, mets = gt.build(name, spec)
        nvar, edge = len(variants), spec["edge"]
        jobs = [
            (f"{name}_atlas.png", sheet_contact(name, sheet, nvar)),
            (f"{name}_configs.png", config_matrix(name, sheet, nvar, edge)),
            (f"{name}_fringe.png", fringe_sheet(name, sheet, nvar, edge)),
            (f"{name}_dungeon.png", dungeon_render(name, sheet, nvar, edge)),
            (f"{name}_edge.png", edge_compare(name, sheet, nvar)),
        ]
        for fn, img in jobs:
            p = os.path.join(OUT, fn)
            img.convert("RGB").save(p)
            written.append(p)
            print("  wrote", p)
    return written


if __name__ == "__main__":
    main()
