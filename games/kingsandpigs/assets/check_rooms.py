#!/usr/bin/env python3
"""Room-chunk validator: parses src/kp_rooms.h and checks every template for
format rules AND king-movement accessibility.

Movement model (matches game.c tuning): the king stands on solid '#' or plank
'='/'-'/'S' tops, walks along surfaces, jumps at most 2 tiles up (JUMP_V -215
gives a ~2.6-tile apex; 2 is the safe gameplay bound), and can fall any depth.
A horizontal jump clears a 2-tile gap.

Checks per template:
  - every row exactly 16 chars; row 0 solid; floor row only '#'/'O'
  - corridor mouths (rows H-4..H-2) open at cols 0 and 15
  - every item/marker (d D h b B E C S e x) reachable from the room's mouths
    (and from the entrance door for start rooms)
Exit code 1 if anything fails — run after editing kp_rooms.h.
"""
import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), "src", "kp_rooms.h")

SOLID = set("#")
PLANK = set("=-")
MARKERS = set("dDhbBECSex")

def standable(rows, r, c):
    """Can the king stand with feet on top of cell (r,c)? (cell below feet)"""
    H, W = len(rows), 16
    if r >= H or rows[r][c] in ("O",):
        return False
    if rows[r][c] not in SOLID | PLANK and rows[r][c] != "S":
        return False
    # need the cell above to be walkable space (not solid)
    return r - 1 >= 0 and rows[r - 1][c] not in SOLID

def reachable_set(rows, seeds):
    """Flood over standing positions: (r,c) = feet-on-top-of-cell(r,c)."""
    H, W = len(rows), 16
    stands = {(r, c) for r in range(H) for c in range(W) if standable(rows, r, c)}
    seen = set(s for s in seeds if s in stands)
    todo = list(seen)
    while todo:
        r, c = todo.pop()
        cand = set()
        # headroom above the LAUNCH point: a jump is only possible if the
        # king can actually rise there (standing at (r,c) his head is in
        # r-1; gaining dr tiles needs r-2..r-1-dr open above him)
        def headroom(n):
            for rr in range(r - 1 - n, r - 1):
                if rr >= 0 and rows[rr][c] in SOLID:
                    return False
            return True
        def arc_clear(cc2):
            """the jump arc passes over the intermediate columns ~2 tiles up"""
            step = 1 if cc2 > c else -1
            for ci in range(c + step, cc2, step):
                for rr2 in (r - 1, r - 2):
                    if rr2 >= 0 and rows[rr2][ci] in SOLID:
                        return False
            return True
        for dc in range(-2, 3):
            cc = c + dc
            if not (0 <= cc < W):
                continue
            for rr in range(max(1, r - 2), r + 1):     # same level / step / jump
                if (rr, cc) in stands and headroom(r - rr) and arc_clear(cc):
                    cand.add((rr, cc))
            if dc == 0 or rows[r][cc] not in SOLID:    # can actually step off there
                for rr in range(r + 1, H):             # step off and fall
                    if (rr, cc) in stands:
                        cand.add((rr, cc))
                        break
                    if rows[rr][cc] in SOLID:          # never fall through solid
                        break
        for dc in (-3, 3):     # long flat jump (clears a 2-wide gap)
            cc = c + dc
            if not (0 <= cc < W) or not headroom(1):
                continue
            if not arc_clear(cc):
                continue
            if rows[r][cc] in SOLID:                   # land on it, or it blocks
                if (r, cc) in stands:
                    cand.add((r, cc))
                continue
            for rr in range(r, H):
                if (rr, cc) in stands:
                    cand.add((rr, cc))
                    break
                if rows[rr][cc] in SOLID:
                    break
        for s in cand:
            if s not in seen:
                seen.add(s)
                todo.append(s)
    return seen

def marker_pos(rows, r, c):
    """Feet cell a marker rests on (scan down like the game does)."""
    H = len(rows)
    for rr in range(r + 1, H):
        if rows[rr][c] in SOLID | PLANK or rows[rr][c] == "S":
            return (rr, c)
    return (H - 1, c)

REQUIRED = {"start": "e", "exit": "x", "bossexit": "x", "drop": "O"}

def check(name, idx, rows):
    errs = []
    H, W = len(rows), 16
    req = REQUIRED.get(name)
    if req and sum(row.count(req) for row in rows) < 1:
        errs.append(f"missing required '{req}' marker")
    if name not in ("start",) and any("e" in row for row in rows):
        errs.append("stray 'e' marker")
    if name in ("start", "side", "drop") and any("x" in row for row in rows):
        errs.append("stray 'x' marker")
    if H != 8:
        errs.append(f"{H} rows (want 8)")
        return errs
    for i, row in enumerate(rows):
        if len(row) != 16:
            errs.append(f"row {i} len {len(row)}")
            return errs
    if rows[0] != "#" * 16:
        errs.append("ceiling not solid")
    if set(rows[H - 1]) - set("#O"):
        errs.append(f"floor has {set(rows[H-1]) - set('#O')}")
    for i in (H - 4, H - 3, H - 2):
        if rows[i][0] != "." or rows[i][15] != ".":
            errs.append(f"mouth blocked at row {i}")

    # simulate the game's shelf widening: 'S' becomes a 3-wide beam
    sim = [list(row) for row in rows]
    for r in range(H):
        for c in range(W):
            if rows[r][c] == "S":
                sim[r][c] = "-"
                for cc in (c - 1, c + 1):
                    if 0 <= cc < W and rows[r][cc] == ".":
                        sim[r][cc] = "-"
    sim = ["".join(row) for row in sim]

    # ---- art clearance: windows/banners/doors have real pixel footprints;
    # they must sit on clear background and never overlap walls, platforms,
    # or each other's art ----
    claimed = {}
    def claim(kind, r0, c0, r1, c1, mark_r, mark_c):
        for rr in range(r0, r1 + 1):
            for cc in range(c0, c1 + 1):
                if rr < 1 or rr > H - 2 or cc < 0 or cc >= W:
                    errs.append(f"'{kind}' at {mark_r},{mark_c}: art leaves the room")
                    return
                if sim[rr][cc] in "#=-":
                    errs.append(f"'{kind}' at {mark_r},{mark_c}: art overlaps terrain at {rr},{cc}")
                    return
                if (rr, cc) in claimed:
                    errs.append(f"'{kind}' at {mark_r},{mark_c}: art overlaps '{claimed[(rr,cc)]}'")
                    return
        for rr in range(r0, r1 + 1):
            for cc in range(c0, c1 + 1):
                claimed[(rr, cc)] = kind
    for r in range(H):
        for c in range(W):
            ch = rows[r][c]
            if ch == "W":                       # window frame: ~2x2 tiles
                claim("W", r, c, r + 1, c + 1, r, c)
            elif ch == "F":                     # banner: 1 wide, up to 3 tall
                claim("F", r, c, r + 2, c, r, c)
            elif ch in "ex":                    # door: 3 wide x 2 tall above its feet
                fr = marker_pos(sim, r, c)[0]
                claim(ch, fr - 2, c - 1, fr - 1, c + 1, r, c)

    # accessibility from EACH corridor mouth independently — a floor may
    # connect a room from only one side, so both entries must reach everything
    for side, cols in (("left", (0, 1)), ("right", (14, 15))):
        seen = reachable_set(sim, [marker_pos(sim, H - 2, c) for c in cols])
        for r in range(H):
            for c in range(W):
                ch = rows[r][c]
                if ch in MARKERS:
                    p = (r, c) if ch == "S" else marker_pos(sim, r, c)
                    if p not in seen:
                        errs.append(f"marker '{ch}' at {r},{c} unreachable from {side}")
                if sim[r][c] in PLANK and standable(sim, r, c) and (r, c) not in seen:
                    errs.append(f"plank at {r},{c} unreachable from {side}")
    return errs

def main():
    txt = open(SRC).read()
    groups = re.findall(r"kp_(\w+)\[\] = \{(.*?)\n\};", txt, re.S)
    bad = 0
    total = 0
    for gname, body in groups:
        rooms = re.findall(r"\{\{(.*?)\}\}", body, re.S)
        for i, room in enumerate(rooms):
            rows = re.findall(r'"([^"]*)"', room)
            total += 1
            for e in check(gname, i, rows):
                print(f"kp_{gname}[{i}]: {e}")
                bad += 1
    print(f"{total} rooms checked:", "ALL OK" if not bad else f"{bad} PROBLEMS")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
