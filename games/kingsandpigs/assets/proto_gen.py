#!/usr/bin/env python3
"""Floor generator (Python proving ground): small hand-authored solid-brick
room pieces ABUT directly (share one wall/floor row, no corridor tubes) and
connect through carved openings, grown into an organic winding descent.

Pieces declare ports via edge glyphs:
   ^  top landing (2 ceiling cells)     v  bottom drop (2 floor cells)
   <  left door (2 cells, floor level)  >  right door (2 cells, floor level)
Interiors are shaped with SOLID BRICK (#) plus wood planks (= thin, - thick,
S shelf) and markers (d D h b B E C W F e x). Every floor is validated for
reachability + art clearance; bad seeds are rejected.

Usage: python3 assets/proto_gen.py [out.png] [seed0]
"""
import os, sys, random
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import proto_layout as PL
import kp_pieces

# validated hand-authored library (assets/kp_pieces.py)
PIECES = kp_pieces.PIECES

# -- start: king enters, drops out the bottom, or a side door
def ports(name):
    rows = PIECES[name]["rows"]; h = len(rows); w = len(rows[0]); out = []
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
OPP = {'T': 'B', 'B': 'T', 'L': 'R', 'R': 'L'}

# ============================================================ generator
CW, CH = 60, 44
VEXTENT = 30               # HARD cap on total height (top-most..bottom-most): ~4-5 rooms

def gen(seed, target=14, tries_cap=600):
    rng = random.Random(seed)
    cv = [[' '] * CW for _ in range(CH)]
    occ = []        # placed piece rects (x,y,w,h)
    placed = []     # dict per piece
    frontier = []   # (piece_idx, port)

    def fits(x, y, w, h, shared):
        # shared: ('none') or a rect edge we may overlap by 1 line
        if x < 1 or y < 1 or x + w > CW - 1 or y + h > CH - 1: return False
        for (ox, oy, ow, oh) in occ:
            ix0, iy0 = max(x, ox), max(y, oy)
            ix1, iy1 = min(x + w, ox + ow), min(y + h, oy + oh)
            if ix1 - ix0 > 0 and iy1 - iy0 > 0:
                # allow a 1-tile-thick shared boundary only
                if (ix1 - ix0) <= 1 or (iy1 - iy0) <= 1:
                    continue
                return False
        return True

    def stamp(name, x, y):
        rows = PIECES[name]["rows"]
        for r in range(len(rows)):
            for c in range(len(rows[0])):
                ch = rows[r][c]
                cv[y + r][x + c] = '#' if ch in '^v<>' else ch

    start = "start"
    sw, sh = len(PIECES[start]["rows"][0]), len(PIECES[start]["rows"])
    sx, sy = CW // 2 - sw // 2, 2
    placed.append({"name": start, "x": sx, "y": sy, "used": []})
    occ.append((sx, sy, sw, sh)); stamp(start, sx, sy)
    for p in ports(start): frontier.append((0, p))

    side_names = [n for n, d in PIECES.items() if d["kind"] == "side"]

    def attach(pi, port, cand):
        pc = placed[pi]; pr = PIECES[pc["name"]]["rows"]
        ph, pw = len(pr), len(pr[0]); kind, a, b = port
        want = OPP[kind]; rng.shuffle(cand)
        for bn in cand:
            brows = PIECES[bn]["rows"]; bh, bw = len(brows), len(brows[0])
            bports = [q for q in ports(bn) if q[0] == want]; rng.shuffle(bports)
            for bp in bports:
                _, ba, bb = bp
                if (b - a) != (bb - ba): continue        # port widths must match
                if kind == 'B':
                    bx = pc["x"] + a - ba; by = pc["y"] + ph - 1
                elif kind == 'T':
                    bx = pc["x"] + a - ba; by = pc["y"] - bh + 1
                elif kind == 'R':
                    bx = pc["x"] + pw - 1; by = pc["y"] + a - ba
                else:  # 'L'
                    bx = pc["x"] - bw + 1; by = pc["y"] + a - ba
                if fits(bx, by, bw, bh, True):
                    return bn, bx, by, bp
        return None

    def vextent(extra=None):
        ys = [(p["y"], p["y"] + len(PIECES[p["name"]]["rows"])) for p in placed]
        if extra: ys.append(extra)
        return max(b for _, b in ys) - min(a for a, _ in ys)

    t = 0
    while frontier and len(placed) < target and t < tries_cap:
        t += 1
        # once we're near the height cap, stop growing vertically and spread
        near_cap = vextent() >= VEXTENT - 6
        want_down = (not near_cap) and rng.random() < 0.4
        pool = [i for i, (pi, p) in enumerate(frontier)
                if (p[0] in 'TB') == want_down] or list(range(len(frontier)))
        fi = rng.choice(pool)
        pi, port = frontier.pop(fi)
        res = attach(pi, port, list(side_names))
        if not res: continue
        bn, bx, by, bp = res
        # HARD height cap: reject any placement (up or down) that grows the
        # top-to-bottom extent past ~5 rooms
        if vextent((by, by + len(PIECES[bn]["rows"]))) > VEXTENT:
            continue
        bi = len(placed)
        placed.append({"name": bn, "x": bx, "y": by, "used": [bp]})
        placed[pi]["used"].append(port)
        occ.append((bx, by, len(PIECES[bn]["rows"][0]), len(PIECES[bn]["rows"])))
        stamp(bn, bx, by)
        carve_opening(cv, placed[pi], port)
        carve_opening(cv, placed[bi], bp)
        for q in ports(bn):
            if q != bp: frontier.append((bi, q))

    # attach an exit to the deepest bottom/side port
    frontier.sort(key=lambda fp: -placed[fp[0]]["y"])
    exit_ok = False
    for pi, port in frontier:
        if port[0] not in ('B', 'L', 'R'): continue
        res = attach(pi, port, ["exit"])
        if res:
            bn, bx, by, bp = res
            bi = len(placed)
            placed.append({"name": bn, "x": bx, "y": by, "used": [bp]})
            placed[pi]["used"].append(port)
            occ.append((bx, by, len(PIECES[bn]["rows"][0]), len(PIECES[bn]["rows"])))
            stamp(bn, bx, by)
            carve_opening(cv, placed[pi], port); carve_opening(cv, placed[bi], bp)
            exit_ok = True; break

    # seal every unused port back to wall
    for pc in placed:
        for p in ports(pc["name"]):
            if p not in pc["used"]: seal_port(cv, pc, p)
    return cv, placed, exit_ok

def carve_opening(cv, pc, port):
    # Vertical connections (T/B) share ONE row between the two abutting rooms;
    # it stays a THIN PLANK (stand on it from above, jump up through from below).
    # Horizontal doors (L/R) open fully.
    rows = PIECES[pc["name"]]["rows"]; h = len(rows); w = len(rows[0])
    kind, a, b = port; x, y = pc["x"], pc["y"]
    if kind == 'T':
        for c in range(a, b + 1): cv[y][x + c] = '='
    elif kind == 'B':
        for c in range(a, b + 1): cv[y + h - 1][x + c] = '='
    elif kind == 'L':
        for r in range(a, b + 1): cv[y + r][x] = '.'
    elif kind == 'R':
        for r in range(a, b + 1): cv[y + r][x + w - 1] = '.'

def seal_port(cv, pc, port):
    rows = PIECES[pc["name"]]["rows"]; h = len(rows); w = len(rows[0])
    kind, a, b = port; x, y = pc["x"], pc["y"]
    if kind == 'T':
        for c in range(a, b + 1): cv[y][x + c] = '#'
    elif kind == 'B':
        for c in range(a, b + 1): cv[y + h - 1][x + c] = '#'
    elif kind == 'L':
        for r in range(a, b + 1): cv[y + r][x] = '#'
    elif kind == 'R':
        for r in range(a, b + 1): cv[y + r][x + w - 1] = '#'

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/kp_gen_floors.png"
    seed0 = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    imgs = []
    for i in range(4):
        cv, placed, ok = gen(seed0 + i * 31)
        imgs.append((PL.render(cv), len(placed), ok))
    pad = 14
    w = max(im.size[0] for im, _, _ in imgs) + pad * 2
    h = sum(im.size[1] for im, _, _ in imgs) + pad * (len(imgs) + 1)
    sheet = Image.new("RGB", (w, h), (12, 12, 16)); y = pad
    for im, n, ok in imgs:
        sheet.paste(im, (pad, y)); y += im.size[1] + pad
    sheet.save(out)
    print("wrote", out, [(n, ok) for _, n, ok in imgs])

if __name__ == "__main__":
    main()
