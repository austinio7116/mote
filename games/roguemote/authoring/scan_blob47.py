#!/usr/bin/env python3
"""Find every complete blob47 set in the source sheet.

Slides a 3-row window down the sheet and, for each candidate (fill, border)
palette pair, tests whether the band maps cleanly onto the 47 canonical
configurations. A real set is a bijection: 47 distinct configs, none repeated.
Anything else is decoration or a partial set.

Unlike the earlier nine-slice scanner, this makes no assumption about how the
band is arranged into blocks -- which is exactly what caused two full sets to be
found and the rest missed.

    python3 scan_blob47.py            # scan the whole sheet
    python3 scan_blob47.py 0 41 15 43 # score one band in detail
"""
import sys
from collections import Counter

import blob47
import map_blob47 as mp

TS = mp.TS


def band_colours(rect):
    c0, r0, c1, r1 = rect
    cnt = Counter()
    for r in range(r0, r1 + 1):
        for c in range(c0, c1 + 1):
            for row in mp.tile(c, r):
                cnt.update(row)
    return cnt


def score_band(rect, max_cols=12):
    """-> best (n_configs, dupes, fill, border) over candidate palette pairs."""
    cnt = band_colours(rect)
    if len(cnt) < 2:
        return None
    cands = [v for v, _ in cnt.most_common(max_cols)]
    best = None
    for fill in cands:
        if fill == mp.TRANSPARENT:
            continue
        for border in cands:
            if border == fill:
                continue
            found = mp.map_rect(rect, fill, border)
            n = len(found)
            dup = sum(len(v) - 1 for v in found.values())
            key = (n, -dup)
            if best is None or key > (best[0], -best[1]):
                best = (n, dup, fill, border)
    return best


def scan(cols=(0, 15), rows=(0, 61)):
    hits = []
    for r0 in range(rows[0], rows[1] - 1):
        rect = (cols[0], r0, cols[1], r0 + 2)
        b = score_band(rect)
        if b and b[0] >= 30:
            hits.append((r0, b))
    return hits


def main():
    if len(sys.argv) == 5:
        rect = tuple(int(v) for v in sys.argv[1:5])
        n, dup, fill, border = score_band(rect)
        print(f"{rect}: {n}/47 configs, {dup} duplicated, fill={fill} border={border}")
        found = mp.map_rect(rect, fill, border)
        for m in blob47.CANON:
            i = blob47.CANON.index(m)
            w = found.get(m)
            print(f"  cell {i:>2} mask {m:>3}  " +
                  (" ".join(f"({c},{r})" for c, r in w) if w else "-- MISSING --"))
        return

    print("scanning every 3-row band, cols 0-15, for a clean 47-way mapping\n")
    print(f"{'rows':>9}  {'configs':>8} {'dupes':>6} {'fill':>5} {'border':>7}   verdict")
    for r0, (n, dup, fill, border) in scan():
        v = "COMPLETE blob47 set" if n == 47 and dup == 0 else "partial"
        print(f"{r0:>4}-{r0+2:<4}  {n:>8} {dup:>6} {fill:>5} {border:>7}   {v}")


if __name__ == "__main__":
    main()
