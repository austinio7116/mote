#!/usr/bin/env python3
"""Validator for the piece library and generated floors: king-movement
reachability + art clearance, on an arbitrary char canvas.

Movement model (matches game.c / check_rooms): stand on '#'/plank tops; jump
at most 2 up with launch headroom; flat-jump up to 3 cols with arc clearance;
fall any depth but never through a solid; drops one-way.
"""
SOLID = set("# ")            # void blocks movement too
PLANK = set("=-S")
MARK = set("dDhbBECex")      # reachability-relevant (W/F are decor: art only)

def is_solid(cv, r, c):
    H, W = len(cv), len(cv[0])
    if r < 0 or r >= H or c < 0 or c >= W: return True
    return cv[r][c] in SOLID

def surface(cv, r, c):        # can stand ON TOP of (r,c)?
    ch = cv[r][c] if 0 <= r < len(cv) and 0 <= c < len(cv[0]) else ' '
    if ch not in SOLID and ch not in PLANK: return False
    return not is_solid(cv, r - 1, c)      # head room in the cell above

def flood(cv, seeds):
    H, W = len(cv), len(cv[0])
    stands = {(r, c) for r in range(H) for c in range(W) if surface(cv, r, c)}
    seen = set(s for s in seeds if s in stands)
    todo = list(seen)
    while todo:
        r, c = todo.pop()
        def headroom(n):
            for rr in range(r - 1 - n, r - 1):
                if rr >= 0 and is_solid(cv, rr, c): return False
            return True
        def arc(cc2):
            step = 1 if cc2 > c else -1
            for ci in range(c + step, cc2, step):
                for rr2 in (r - 1, r - 2):
                    if rr2 >= 0 and is_solid(cv, rr2, ci): return False
            return True
        cand = set()
        for dc in range(-2, 3):
            cc = c + dc
            if not (0 <= cc < W): continue
            for rr in range(max(1, r - 2), r + 1):
                if (rr, cc) in stands and headroom(r - rr) and arc(cc): cand.add((rr, cc))
            if dc == 0 or not is_solid(cv, r, cc):
                for rr in range(r + 1, H):
                    if (rr, cc) in stands: cand.add((rr, cc)); break
                    if is_solid(cv, rr, cc): break
        for dc in (-3, 3):
            cc = c + dc
            if not (0 <= cc < W) or not headroom(1) or not arc(cc): continue
            if is_solid(cv, r, cc):
                if (r, cc) in stands: cand.add((r, cc))
                continue
            for rr in range(r, H):
                if (rr, cc) in stands: cand.add((rr, cc)); break
                if is_solid(cv, rr, cc): break
        for s in cand:
            if s not in seen: seen.add(s); todo.append(s)
    return seen

def solid_below(cv, r, c):    # the solid/plank cell a marker at (r,c) rests ON
    H = len(cv)
    for rr in range(r + 1, H):
        if cv[rr][c] in SOLID or cv[rr][c] in PLANK: return (rr, c)
    return (H - 1, c)
def rest(cv, r, c):           # the open feet cell just above that surface
    rr, cc = solid_below(cv, r, c); return (rr - 1, cc)

def art_clear(cv, errs, x0=0, y0=0):
    """W=2x2, F=1x3, door(e/x)=3x2 above feet — none overlap terrain/each other."""
    H, W = len(cv), len(cv[0])
    claimed = {}
    def claim(kind, cells, mr, mc):
        for (rr, cc) in cells:
            if rr < 0 or rr >= H or cc < 0 or cc >= W or cv[rr][cc] in SOLID or cv[rr][cc] in PLANK:
                errs.append(f"{kind}@{mr-y0},{mc-x0}: overlaps wall/terrain"); return
            if (rr, cc) in claimed and claimed[(rr, cc)] != (mr, mc):
                errs.append(f"{kind}@{mr-y0},{mc-x0}: overlaps {claimed[(rr,cc)]}"); return
        for cell in cells: claimed[cell] = (mr, mc)
    for r in range(H):
        for c in range(W):
            ch = cv[r][c]
            if ch == 'W': claim('W', [(r, c), (r, c+1), (r+1, c), (r+1, c+1)], r, c)
            elif ch == 'F': claim('F', [(r, c), (r+1, c), (r+2, c)], r, c)
            elif ch in 'ex':
                fr, _ = rest(cv, r, c)
                claim(ch, [(fr, c-1), (fr, c), (fr, c+1), (fr-1, c-1), (fr-1, c), (fr-1, c+1)], r, c)

def markers(cv):
    return [(r, c, cv[r][c]) for r in range(len(cv)) for c in range(len(cv[0])) if cv[r][c] in MARK]

# ---- piece validation -----------------------------------------------------
def _ports(rows):
    h, w = len(rows), len(rows[0]); out = []
    def runs(idxs):
        r = []
        for i in idxs:
            if r and i == r[-1][1] + 1: r[-1][1] = i
            else: r.append([i, i])
        return [tuple(x) for x in r]
    for a, b in runs([c for c in range(w) if rows[0][c] == '^']):   out.append(('T', a, b))
    for a, b in runs([c for c in range(w) if rows[h-1][c] == 'v']): out.append(('B', a, b))
    for a, b in runs([r for r in range(h) if rows[r][0] == '<']):   out.append(('L', a, b))
    for a, b in runs([r for r in range(h) if rows[r][w-1] == '>']): out.append(('R', a, b))
    return out

def _port_targets(cv, port):
    # standing cells (the solid you stand on) associated with a port
    h, w = len(cv), len(cv[0]); kind, a, b = port
    if kind == 'T': return [solid_below(cv, 0, c) for c in range(a, b+1)]
    if kind == 'B': return [(h-1, c) for c in (a-1, b+1) if 0 <= c < w]
    if kind == 'L': return [(h-1, 1)]
    if kind == 'R': return [(h-1, w-2)]
    return []

def check_piece(rows):
    """rows: list of strings (piece with edge port glyphs). Returns error list.
    Rules: rectangular; solid perimeter except ports; ports 2-wide/tall; decor
    art-clear; from EVERY port you can reach all collectibles + every exit port
    (drops one-way). W/F are decor (art only). Floor must not be dead flat."""
    errs = []
    if not rows or any(len(r) != len(rows[0]) for r in rows):
        return ["ragged rows"]
    h, w = len(rows), len(rows[0])
    if not (5 <= h <= 8 and 7 <= w <= 12): errs.append(f"size {w}x{h} out of 7-12 x 5-8")
    ps = _ports(rows)
    if not ps: errs.append("no ports")
    for k, a, b in ps:
        if b - a != 1: errs.append(f"port {k} not 2 wide/tall")
    def sub(s, to): return [list(r.replace('^',to).replace('v',to).replace('<',to).replace('>',to)) for r in s]
    art = sub(rows, '#')     # ports are wall for art clearance (no decor in a doorway)
    grid = sub(rows, '.')    # ports are OPEN sky/doorway for movement
    art_clear(art, errs)
    # floor variety: the bottom interior row must not be one flat solid line
    floor_profile = [max((rr for rr in range(h) if art[rr][c] in SOLID or art[rr][c] in PLANK),
                          default=h-1) for c in range(1, w-1)]
    if len(set(floor_profile)) == 1 and not any(art[h-2][c] in '#=-S' for c in range(1, w-1)):
        errs.append("floor is dead flat (add steps/blocks/pillars)")
    # every connection is TWO-WAY (agency: you can climb back up). A top hole
    # must have a landing/platform within 2 tiles of the ceiling so you can
    # jump back up through it.
    for (k, a, b) in ps:
        if k == 'T':
            for c in range(a, b + 1):
                if solid_below(grid, 0, c)[0] > 2:
                    errs.append(f"top hole @{c} not climbable (need a platform within 2 of the ceiling)")
                    break
    # all ports are both entry and exit: from each, reach all others + all loot
    mk = markers(grid)
    for p in ps:
        seen = flood(grid, _port_targets(grid, p))
        for (r, c, ch) in mk:
            if solid_below(grid, r, c) not in seen:
                errs.append(f"from {p[0]}: {ch}@{r},{c} unreachable")
        for e in ps:
            if e == p: continue
            if not any(t in seen for t in _port_targets(grid, e)):
                errs.append(f"from {p[0]}: port {e[0]} unreachable")
    return errs
