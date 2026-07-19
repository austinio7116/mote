#!/usr/bin/env python3
"""PROTOTYPE of the new floor architecture: variable-size hand-authored room
sections joined into an organic castle by a port-graph generator, rendered
with the real tilesets/sprites so we can judge the look before porting to C.

Rooms are enclosed brick chambers of varied W x H. Each declares PORTS on its
perimeter:
   <  left doorway    >  right doorway   (walk-through, at floor level)
   ^  top shaft mouth  v  bottom shaft mouth (fall-through descent)
Doorways sit on the two rows just above the room floor, so horizontally
chained rooms share a continuous floor (optionally stepped +/-1). Shafts drop
the king from a v-port down to a ^-port. Everything off a used port is walled,
so interiors are free to pack multi-level terrain.

Usage: python3 assets/proto_layout.py [out.png] [seed0]
"""
import os, sys, random
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import render_rooms as RR
TS = 32

# ---------------------------------------------------------------- room library
# Each room: enclosed by '#', ports on edges, interiors packed. Doors '<' '>'
# occupy the two rows above the floor; shafts '^'/'v' are 2 wide on ceiling/floor.
R = {}
def room(name, rows):
    w = len(rows[0])
    assert all(len(r) == w for r in rows), (name, [len(r) for r in rows])
    R[name] = rows

# -- entrance --
room("start_hall", [
    "################",
    "#..F.......d...#",
    "#..====.....==.#",
    "#e...E........>#",
    "#..d......E..d.>",
    "######vv########",
])
# -- horizontal halls (L+R) --
room("wide_hall", [
    "####################",
    "#..F..........F....#",
    "#..====....====....#",
    "<....E...dd....E...>",
    "<..d....BB........d>",
    "####################",
])
room("beam_gallery", [
    "##################",
    "#...F........d...#",
    "#..===...E..===..#",
    "#.......===......#",
    "<...d....E.....B.>",
    "<..B..........d..>",
    "##################",
])
room("arch_hall", [
    "##################",
    "#....F....F......#",
    "#...###..###.....#",
    "<...#......#...E.>",
    "<.d.#..dd..#....d>",
    "##################",
])
room("closet", [
    "##########",
    "#..d...W.#",
    "#..====..#",
    "<...E...d>",
    "<..d.....>",
    "##########",
])
room("tall_keep", [
    "##############",
    "#..F.....d...#",
    "#..===..===..#",
    "#.E.......E..#",
    "#...====.....#",
    "<..d....d..B.>",
    "<..B........d>",
    "##############",
])
# -- treasure dead-end (single door) --
room("vault", [
    "############",
    "#.W....dd..#",
    "#..S....S..#",
    "<...dd....d>",
    "<..hd...BB.>",
    "############",
])
# -- guard room with cannon, L+R+bottom drop --
room("guardroom", [
    "################",
    "#...F......d...#",
    "#..===.....==..#",
    "<....E........E>",
    "<.C........dd..>",
    "#####vv#########",
])
# -- vertical: shaft with platforms, T + B + R door --
room("shaft", [
    "####^^######",
    "#....==....#",
    "#..==....E.#",
    "#......==..>",
    "#.E.==....d>",
    "#..==....B.#",
    "#....==..d.#",
    "######vv####",
])
# -- landing hall: shaft drops in the top, exits via R door --
room("landing_hall", [
    "###^^###########",
    "#....d.....F...#",
    "#..==....===...#",
    "<....E.......E.>",
    "<..d....BB...d.>",
    "################",
])
# -- crossroads L+R+T --
room("crossroads", [
    "#####^^#######",
    "#...d....d...#",
    "#..===..===..#",
    "<.....EE.....>",
    "<..B.......d.>",
    "##############",
])
# -- terrace stepping down to a drop, L + bottom --
room("terrace", [
    "###############",
    "#..F.....d....#",
    "<...===.......#",
    "<......===..E.#",
    "#..E......dd..#",
    "######vv#######",
])
# -- exit chamber, L door + T landing + 'x' --
room("throne_exit", [
    "####^^########",
    "#...F.....d..#",
    "#..===.......#",
    "<....E...d...#",
    "<..d.......x.#",
    "##############",
])

# ---------------------------------------------------------------- ports
def ports_of(name):
    """Return list of ports: (kind, a, b) where kind in L,R,T,B.
       L/R: a,b = row span (inclusive). T/B: a,b = col span."""
    rows = R[name]; h = len(rows); w = len(rows[0]); ps = []
    # left / right doorways
    col_runs = lambda col, ch: _runs([r for r in range(h) if rows[r][col] == ch])
    for r0, r1 in col_runs(0, '<'):      ps.append(('L', r0, r1))
    for r0, r1 in col_runs(w - 1, '>'):  ps.append(('R', r0, r1))
    row_runs = lambda row, ch: _runs([c for c in range(w) if rows[row][c] == ch])
    for c0, c1 in row_runs(0, '^'):      ps.append(('T', c0, c1))
    for c0, c1 in row_runs(h - 1, 'v'):  ps.append(('B', c0, c1))
    return ps

def _runs(idxs):
    out = []
    for i in idxs:
        if out and i == out[-1][1] + 1: out[-1][1] = i
        else: out.append([i, i])
    return [tuple(x) for x in out]

OPP = {'L': 'R', 'R': 'L', 'T': 'B', 'B': 'T'}

# ---------------------------------------------------------------- generator
CW, CH = 116, 60           # canvas tiles (generous; used bounds cropped)

def blank():
    return [[' '] * CW for _ in range(CH)]

def rect_free(occ, x, y, w, h, margin=1):
    if x < 1 or y < 1 or x + w > CW - 1 or y + h > CH - 1: return False
    for (ox, oy, ow, oh) in occ:
        if (x - margin < ox + ow and x + w + margin > ox and
            y - margin < oy + oh and y + h + margin > oy):
            return False
    return True

def stamp_room(cv, name, x, y, used_ports):
    rows = R[name]; h = len(rows); w = len(rows[0])
    for r in range(h):
        for c in range(w):
            ch = rows[r][c]
            if ch in '<>^v':                    # port cell: wall unless carved
                ch = '#'
            cv[y + r][x + c] = ch
    # carve the ports we actually connected
    for (kind, a, b) in used_ports:
        if kind == 'L':
            for r in range(a, b + 1): cv[y + r][x] = '.'
        elif kind == 'R':
            for r in range(a, b + 1): cv[y + r][x + w - 1] = '.'
        elif kind == 'T':
            for c in range(a, b + 1): cv[y][x + c] = '.'
        elif kind == 'B':
            for c in range(a, b + 1): cv[y + h - 1][x + c] = '.'

def floor_row(name):
    return len(R[name]) - 1

def gen(seed, target=16):
    rng = random.Random(seed)
    cv = blank(); occ = []
    placed = []          # (name, x, y)
    frontier = []        # (placed_idx, port)

    # place entrance near top-centre
    x0 = CW // 2 - len(R["start_hall"][0]) // 2
    y0 = 2
    placed.append(["start_hall", x0, y0, []])
    occ.append((x0, y0, len(R["start_hall"][0]), len(R["start_hall"])))
    for p in ports_of("start_hall"):
        frontier.append((0, p))

    names = [n for n in R if n not in ("start_hall", "throne_exit")]
    have_exit = False

    def try_attach(pi, port, cand_names):
        (an, ax, ay, aused) = placed[pi]
        arows = R[an]; ah = len(arows); aw = len(arows[0])
        kind, a, b = port
        want = OPP[kind]
        rng.shuffle(cand_names)
        for bn in cand_names:
            bports = [q for q in ports_of(bn) if q[0] == want]
            rng.shuffle(bports)
            brows = R[bn]; bh = len(brows); bw = len(brows[0])
            for bp in bports:
                _, ba, bb = bp
                if kind in 'LR':
                    span = b - a
                    if bb - ba != span: continue          # door heights must match
                    for L in (rng.randint(2, 6), 3, 2, 4, 5):
                        step = rng.choice((0, 0, 0, -1, 1))  # small floor step
                        if kind == 'R':
                            bx = ax + aw - 1 + L + 1
                            by = ay + a - ba + step
                            cx, cw_ = ax + aw, L
                        else:
                            bx = ax - L - 1 - (bw - 1)
                            by = ay + a - ba + step
                            cx, cw_ = ax - L, L
                        # corridor rows span the door rows on the A side
                        cy, ch_ = ay + min(a, a + step), (b - a) + 1 + abs(step)
                        if not rect_free(occ, bx, by, bw, bh): continue
                        if not rect_free(occ, cx, cy - 1, cw_, ch_ + 2, margin=0): continue
                        return bn, bx, by, bp, ('H', cx, cy, cw_, ch_, a, b, aw, bw, kind)
                else:  # vertical shaft
                    span = b - a
                    if bb - ba != span: continue
                    for L in (rng.randint(2, 7), 3, 4, 2):
                        if kind == 'B':
                            bx = ax + a - ba
                            by = ay + ah - 1 + L + 1
                            cy, ch_ = ay + ah, L
                        else:
                            bx = ax + a - ba
                            by = ay - L - 1 - (bh - 1)
                            cy, ch_ = ay - L, L
                        cx = ax + a
                        if not rect_free(occ, bx, by, bw, bh): continue
                        if not rect_free(occ, cx - 1, cy, (b - a) + 1 + 2, ch_, margin=0): continue
                        return bn, bx, by, bp, ('V', cx, cy, (b - a) + 1, ch_)
        return None

    tries = 0
    while frontier and len(placed) < target and tries < 400:
        tries += 1
        fi = rng.randrange(len(frontier))
        pi, port = frontier.pop(fi)
        # bias: exits when deep; verticals prefer descent rooms
        cand = list(names)
        res = try_attach(pi, port, cand)
        if not res:
            continue
        bn, bx, by, bp, corr = res
        # stamp corridor
        if corr[0] == 'H':
            _, cx, cy, cw_, ch_, a, b, aw, bw, kind = corr
            # floor-aligned tube: carve bg between, wall top & bottom
            afloor = placed[pi][2] + len(R[placed[pi][0]]) - 1
            bfloor = by + len(R[bn]) - 1
            for xx in range(cx, cx + cw_):
                # step the floor linearly
                t = (xx - cx + 1) / (cw_ + 1)
                fl = int(round(afloor + (bfloor - afloor) * t))
                for yy in range(fl - (b - a) - 1, fl + 1):
                    if 0 <= yy < CH: cv[yy][xx] = '.'
                if 0 <= fl + 1 < CH: cv[fl + 1][xx] = '#'
                yy = fl - (b - a) - 2
                if 0 <= yy < CH: cv[yy][xx] = '#'
            occ.append((cx, min(afloor, bfloor) - (b - a) - 2, cw_, abs(afloor - bfloor) + (b - a) + 4))
        else:
            _, cx, cy, cwd, ch_ = corr
            for yy in range(cy, cy + ch_):
                for xx in range(cx, cx + cwd): cv[yy][xx] = '.'
                if cx - 1 >= 0: cv[yy][cx - 1] = '#'
                if cx + cwd < CW: cv[yy][cx + cwd] = '#'
            occ.append((cx - 1, cy, cwd + 2, ch_))
        # stamp room, mark ports used on both sides
        bi = len(placed)
        placed.append([bn, bx, by, [bp]])
        placed[pi][3].append(port)
        occ.append((bx, by, len(R[bn][0]), len(R[bn])))
        stamp_room(cv, bn, bx, by, [bp])
        for q in ports_of(bn):
            if q != bp: frontier.append((bi, q))
        if bn == "vault": pass

    # place an exit: attach throne_exit to the deepest open L/T port
    frontier.sort(key=lambda fp: -(placed[fp[0]][2]))
    for pi, port in frontier:
        res = try_attach(pi, port, ["throne_exit"])
        if res:
            bn, bx, by, bp, corr = res
            if corr[0] == 'V':
                _, cx, cy, cwd, ch_ = corr
                for yy in range(cy, cy + ch_):
                    for xx in range(cx, cx + cwd): cv[yy][xx] = '.'
                    if cx-1>=0: cv[yy][cx-1] = '#'
                    if cx+cwd<CW: cv[yy][cx+cwd] = '#'
            else:
                _, cx, cy, cw_, ch_, a, b, aw, bw, kind = corr
                afloor = placed[pi][2] + len(R[placed[pi][0]]) - 1
                bfloor = by + len(R[bn]) - 1
                for xx in range(cx, cx + cw_):
                    t = (xx - cx + 1) / (cw_ + 1)
                    fl = int(round(afloor + (bfloor - afloor) * t))
                    for yy in range(fl - (b - a) - 1, fl + 1): cv[yy][xx] = '.'
                    cv[fl + 1][xx] = '#'; cv[fl - (b - a) - 2][xx] = '#'
            placed.append([bn, bx, by, [bp]])
            placed[pi][3].append(port)
            stamp_room(cv, bn, bx, by, [bp])
            have_exit = True
            break

    # re-wall every port left open (no holes into the void)
    for (nm, x, y, used) in placed:
        for p in ports_of(nm):
            if p in used: continue
            kind, a, b = p; rows = R[nm]; h = len(rows); w = len(rows[0])
            if kind == 'L':
                for r in range(a, b+1): cv[y+r][x] = '#'
            elif kind == 'R':
                for r in range(a, b+1): cv[y+r][x+w-1] = '#'
            elif kind == 'T':
                for c in range(a, b+1): cv[y][x+c] = '#'
            elif kind == 'B':
                for c in range(a, b+1): cv[y+h-1][x+c] = '#'
    return cv, placed

# ---------------------------------------------------------------- render
def bits_of(ch):
    if ch == '#': return 8
    if ch == '=': return 1 | 2
    if ch in '-S': return 1 | 4
    if ch == ' ': return 0
    if ch == 'O': return 0
    return 1

def render(cv):
    ts = {n: RR.load_tileset(n) for n in ("bgwall", "platthin", "platthick", "solidt")}
    import re
    m = open(os.path.join(GAME, "src", "kp_meta.h")).read()
    rdx = int(re.search(r"KP_WRAY_DX (\-?\d+)", m).group(1))
    rdy = int(re.search(r"KP_WRAY_DY (\-?\d+)", m).group(1))
    spr = {
        "window": RR.sprite("assets/window.png"),
        "wray": Image.open(os.path.join(GAME, "assets/wray.png")).convert("RGBA"),
        "banner": (lambda b: b.resize((b.size[0], 60)))(RR.sprite("assets/banner1.png")),
        "door": RR.sprite("anims/door.png", (0, 0, 48, 58)),
        "gem": RR.sprite("anims/pickups.png", (0, 0, 24, 14)),
        "heart": RR.sprite("anims/pickups.png", (48, 14, 72, 28)),
        "crate": RR.sprite("anims/box.png", (0, 0, 24, 18)),
        "bomb": RR.sprite("anims/bomb.png", (0, 0, 24, 22)),
        "pig": RR.sprite("anims/pig.png", (0, 0, 40, 30)),
        "cannon": RR.sprite("anims/cannon.png", (0, 0, 56, 30)),
        "king": RR.sprite("anims/king.png", (0, 0, 40, 34)),
    }
    H, W = len(cv), len(cv[0])
    bits = [[bits_of(cv[r][c]) for c in range(W)] for r in range(H)]
    # crop to used bounds
    ys = [r for r in range(H) if any(cv[r][c] != ' ' for c in range(W))]
    xs = [c for c in range(W) if any(cv[r][c] != ' ' for r in range(H))]
    if not ys: return Image.new("RGB", (32, 32), (30, 26, 40))
    y0, y1, x0, x1 = min(ys), max(ys), min(xs), max(xs)
    out = Image.new("RGBA", ((x1 - x0 + 1) * TS, (y1 - y0 + 1) * TS), (39, 33, 51, 255))
    for lname, bit in (("bgwall", 1), ("platthin", 2), ("platthick", 4), ("solidt", 8)):
        t = ts[lname]; tpr = t["sheet"].size[0] // TS
        for r in range(H):
            for c in range(W):
                if not (bits[r][c] & bit): continue
                mm = 0
                for dx, dy, bb in RR.NB:
                    rr, cc = r + dy, c + dx
                    same = t["edge"] if not (0 <= rr < H and 0 <= cc < W) else (bits[rr][cc] & bit) != 0
                    if same: mm |= bb
                cell = t["lut"][mm]; fx, fy = (cell % tpr) * TS, (cell // tpr) * TS
                out.alpha_composite(t["sheet"].crop((fx, fy, fx+TS, fy+TS)), ((c-x0)*TS, (r-y0)*TS))
    import numpy as np
    def feet(r, c):
        for rr in range(r+1, H):
            if cv[rr][c] in '#=-S': return rr
        return H-1
    for r in range(H):
        for c in range(W):
            ch = cv[r][c]; X, Y = (c-x0)*TS, (r-y0)*TS
            if ch == 'W':
                out.alpha_composite(spr["window"], (X-1, Y+4))
                ray = np.array(spr["wray"]); ox, oy = X-1+rdx, Y+4+rdy
                base = np.array(out); h2, w2 = ray.shape[0], ray.shape[1]
                oy2, ox2 = min(oy+h2, base.shape[0]), min(ox+w2, base.shape[1])
                if 0 <= oy < base.shape[0] and 0 <= ox < base.shape[1]:
                    reg = base[oy:oy2, ox:ox2, :3].astype(int)
                    add = ray[:oy2-oy, :ox2-ox, :3].astype(int)
                    base[oy:oy2, ox:ox2, :3] = np.clip(reg+add, 0, 255).astype('uint8')
                    out.paste(Image.fromarray(base))
            elif ch == 'F':
                out.alpha_composite(spr["banner"], (X+1, Y+1))
            elif ch in 'exdDhBbEC':
                fr = feet(r, c); fy = (fr-y0)*TS; cx = X + TS//2
                def g(im, cxx=cx, fyy=fy): out.alpha_composite(im, (cxx-im.size[0]//2, fyy-im.size[1]))
                if ch in 'ex': g(spr["door"])
                elif ch == 'd': g(spr["gem"])
                elif ch == 'D':
                    g(spr["gem"], cx-10); g(spr["gem"], cx+10)
                elif ch == 'h': g(spr["heart"])
                elif ch == 'B': g(spr["crate"])
                elif ch == 'b': g(spr["bomb"])
                elif ch == 'E': g(spr["pig"])
                elif ch == 'C': g(spr["cannon"])
            elif ch == 'e':
                pass
    # king at entrance
    return out.convert("RGB")

def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/kp_proto_floors.png"
    seed0 = int(sys.argv[2]) if len(sys.argv) > 2 else 1234
    imgs = []
    for i in range(4):
        cv, placed = gen(seed0 + i * 97, target=16)
        imgs.append(render(cv))
    pad = 12
    w = max(im.size[0] for im in imgs)
    h = sum(im.size[1] for im in imgs) + pad * (len(imgs) + 1)
    sheet = Image.new("RGB", (w + pad*2, h), (12, 12, 16))
    y = pad
    for im in imgs:
        sheet.paste(im, (pad, y)); y += im.size[1] + pad
    sheet.save(out_path)
    print("wrote", out_path, sheet.size)

if __name__ == "__main__":
    main()
