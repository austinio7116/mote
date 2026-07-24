#!/usr/bin/env python3
"""Find the real 3x3 nine-slice inside each terrain region, instead of guessing.

A well-formed nine-slice has a very specific signature we can test for:
  * the centre tile is pure fill  - no border colour anywhere on its rim
  * T/B differ from the centre ONLY in a few rows at the top/bottom
  * L/R differ from the centre ONLY in a few columns at the left/right
  * the corner tiles differ only inside a small corner box
Anything that scores badly is decoration, a variant, or a misaligned guess.

    python3 find_nineslice.py            # scan every known terrain band
    python3 find_nineslice.py 0 35 15 39 # scan an arbitrary region
"""
import sys, os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = Image.open(os.path.join(HERE, "source_tileset.png"))
IDX = SRC.load()
TS = 8

# candidate bands worth scanning: (label, c0, r0, c1, r1)
BANDS = [
    ("stone-brick wall", 0, 35, 15, 39),
    ("purple brick wall", 0, 16, 15, 18),
    ("temple / aztec wall", 0, 44, 15, 46),
    ("hedge", 0, 29, 15, 31),
    ("jungle grass", 0, 40, 15, 43),
    ("grey stone wall", 0, 25, 10, 28),
    ("colour blocks", 0, 20, 15, 23),
]


def tile(c, r):
    return [[IDX[c * TS + x, r * TS + y] for x in range(TS)] for y in range(TS)]


def score(c0, r0):
    """Lower is better. None if this can't be a nine-slice at all."""
    keys = ["TL", "T", "TR", "L", "C", "R", "BL", "B", "BR"]
    t = {}
    for i, k in enumerate(keys):
        t[k] = tile(c0 + i % 3, r0 + i // 3)
    C = t["C"]
    if all(all(v == 0 for v in row) for row in C):
        return None                                   # empty centre

    def rows_diff(a):
        return [y for y in range(TS) if any(a[y][x] != C[y][x] for x in range(TS))]

    def cols_diff(a):
        return [x for x in range(TS) if any(a[y][x] != C[y][x] for y in range(TS))]

    tr, br = rows_diff(t["T"]), rows_diff(t["B"])
    lc, rc = cols_diff(t["L"]), cols_diff(t["R"])
    if not (tr and br and lc and rc):
        return None                                   # an edge identical to centre
    # each edge must hug its own side and stay shallow
    if min(tr) != 0 or max(br) != TS - 1 or min(lc) != 0 or max(rc) != TS - 1:
        return None
    depth = max(max(tr) + 1, TS - min(br), max(lc) + 1, TS - min(rc))
    if depth > 3:
        return None                                   # too deep to be a border
    # corners must differ from the plain edge composite only near their corner
    rad = 0
    for key, (vk, hk) in (("TL", ("T", "L")), ("TR", ("T", "R")),
                          ("BL", ("B", "L")), ("BR", ("B", "R"))):
        a = t[key]
        for y in range(TS):
            for x in range(TS):
                inV = (y <= max(tr)) if vk == "T" else (y >= min(br))
                inH = (x <= max(lc)) if hk == "L" else (x >= min(rc))
                ref = t[vk][y][x] if inV else (t[hk][y][x] if inH else C[y][x])
                if a[y][x] != ref:
                    dx = x if hk == "L" else TS - 1 - x
                    dy = y if vk == "T" else TS - 1 - y
                    rad = max(rad, dx + 1, dy + 1)
    if rad > 4:
        return None
    return depth * 10 + rad, dict(depth=depth, corner=rad,
                                  top=max(tr) + 1, bottom=TS - min(br),
                                  left=max(lc) + 1, right=TS - min(rc))


def scan(label, c0, r0, c1, r1):
    hits = []
    for r in range(r0, r1 - 1):
        for c in range(c0, c1 - 1):
            s = score(c, r)
            if s:
                hits.append((s[0], c, r, s[1]))
    hits.sort()
    print(f"\n=== {label}  cols {c0}-{c1} rows {r0}-{r1} ===")
    if not hits:
        print("   no well-formed 3x3 nine-slice found")
        return None
    for sc, c, r, m in hits[:6]:
        print(f"   ({c:>2},{r:>2})  score {sc:>3}  borders T{m['top']} B{m['bottom']} "
              f"L{m['left']} R{m['right']}  corner {m['corner']}px")
    return hits[0]


if __name__ == "__main__":
    if len(sys.argv) == 5:
        a = [int(v) for v in sys.argv[1:5]]
        scan("region", *a)
    else:
        for b in BANDS:
            scan(*b)
