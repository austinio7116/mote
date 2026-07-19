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
        # walk / hop 1 (auto step-up covered by jump)
        for dc in (-1, 1):
            cc = c + dc
            if 0 <= cc < W:
                # same level, or step up 1, or down any (fall straight)
                for rr in range(max(1, r - 2), H):
                    if (rr, cc) in stands:
                        if rr >= r or r - rr <= 2:
                            cand.add((rr, cc))
                        break
        # jump up to 2 tiles, reach up to 2 sideways
        for dc in range(-2, 3):
            cc = c + dc
            if 0 <= cc < W:
                for dr in (1, 2):
                    if (r - dr, cc) in stands:
                        cand.add((r - dr, cc))
        # fall from ledge edges up to 2 sideways
        for dc in range(-2, 3):
            cc = c + dc
            if 0 <= cc < W:
                for rr in range(r + 1, H):
                    if (rr, cc) in stands:
                        cand.add((rr, cc))
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

def check(name, idx, rows):
    errs = []
    H, W = len(rows), 16
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

    # accessibility from the corridor mouths (standing just inside each side)
    seeds = []
    for c in (0, 1, 14, 15):
        seeds.append(marker_pos(rows, H - 2, c))
    seen = reachable_set(rows, seeds)
    for r in range(H):
        for c in range(W):
            ch = rows[r][c]
            if ch in MARKERS:
                p = marker_pos(rows, r, c)
                if p not in seen:
                    errs.append(f"marker '{ch}' at {r},{c} unreachable (feet {p})")
            if ch in PLANK and standable(rows, r, c) and (r, c) not in seen:
                errs.append(f"plank at {r},{c} unreachable")
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
