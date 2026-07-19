#!/usr/bin/env python3
"""Floor-generation survey: run the REAL generator (the game binary, headless,
KP_DUMP) across many seeds x depths, then

  1. check whole-floor accessibility (entrance door -> exit door under the
     king's movement model: 2-tile jump, 2-tile gaps, fall any depth, drop
     through the punched holes),
  2. census every enemy/object type by depth,
  3. render a contact sheet of floors (real tilesets + sprite thumbnails)
     for eyeballing variety.

Usage: python3 assets/gen_survey.py [runs-per-depth] [out.png]
"""
import os
import re
import subprocess
import sys
import random
from collections import Counter, defaultdict
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(GAME))
COLS, ROWS = 64, 24

# ---------------------------------------------------------------- run + parse
def run_floor(seed, depth):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               KP_SEED=str(seed), KP_DEPTH=str(depth), KP_DUMP="1",
               MOTE_AUTORUN="1", MOTE_DT_MS="33",
               MOTE_SHOT="/tmp/kp_survey_shot.ppm", MOTE_SHOT_FRAME="2")
    p = subprocess.run([os.path.join(ROOT, "tools", "mote"), "run", GAME],
                       env=env, capture_output=True, text=True, timeout=60)
    lines = p.stderr.splitlines()
    grid, ents = [], {"ENEMY": [], "CANNON": [], "PICKUP": [], "SBOMB": [], "CRATE": []}
    doors = None
    for ln in lines:
        m = re.match(r"^\s*(\d+) ([#=. ]{%d})$" % COLS, ln)
        if m and len(grid) < ROWS:
            grid.append(m.group(2))
        m = re.match(r"ENEMY (\w+) at tile (\d+),(\d+)", ln)
        if m: ents["ENEMY"].append((m.group(1), int(m.group(2)), int(m.group(3))))
        m = re.match(r"CANNON facing (-?\d+) at tile (\d+),(\d+)", ln)
        if m: ents["CANNON"].append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
        m = re.match(r"PICKUP (\d+) at tile (\d+),(\d+)", ln)
        if m: ents["PICKUP"].append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
        m = re.match(r"(SBOMB|CRATE) at tile (\d+),(\d+)", ln)
        if m: ents[m.group(1)].append((int(m.group(2)), int(m.group(3))))
        m = re.match(r"DOORS in (\d+),(\d+) out (\d+),(\d+) boss=(\d)", ln)
        if m and doors is None:
            doors = (int(m.group(1)), int(m.group(2)), int(m.group(3)),
                     int(m.group(4)), int(m.group(5)))
    if len(grid) != ROWS or doors is None:
        return None
    return {"grid": grid, "ents": ents, "doors": doors, "seed": seed, "depth": depth}

# ------------------------------------------------------- floor accessibility
def solid(g, r, c):
    return r < 0 or r >= ROWS or c < 0 or c >= COLS or g[r][c] == "#"

def plank(g, r, c):
    return 0 <= r < ROWS and 0 <= c < COLS and g[r][c] == "="

def standable(g, r, c):
    if not (solid(g, r, c) or plank(g, r, c)):
        return False
    return r - 1 >= 0 and not solid(g, r - 1, c)

def flood(g, seeds):
    stands = {(r, c) for r in range(ROWS) for c in range(COLS) if standable(g, r, c)}
    seen = set(s for s in seeds if s in stands)
    todo = list(seen)
    while todo:
        r, c = todo.pop()
        cand = set()
        def headroom(n):
            for rr in range(r - 1 - n, r - 1):
                if rr >= 0 and solid(g, rr, c):
                    return False
            return True
        def arc_clear(cc2):
            step = 1 if cc2 > c else -1
            for ci in range(c + step, cc2, step):
                for rr2 in (r - 1, r - 2):
                    if rr2 >= 0 and solid(g, rr2, ci):
                        return False
            return True
        for dc in range(-2, 3):
            cc = c + dc
            if not (0 <= cc < COLS):
                continue
            for rr in range(max(1, r - 2), r + 1):     # same level / step / jump
                if (rr, cc) in stands and headroom(r - rr) and arc_clear(cc):
                    cand.add((rr, cc))
            if dc == 0 or not solid(g, r, cc):         # can actually step off there
                for rr in range(r + 1, ROWS):          # step off and fall
                    if (rr, cc) in stands:
                        cand.add((rr, cc))
                        break
                    if solid(g, rr, cc):               # never fall through solid
                        break
        for dc in (-3, 3):     # long flat jump (clears a 2-wide gap) / air-steered fall
            cc = c + dc
            if not (0 <= cc < COLS) or not headroom(1):
                continue
            if not arc_clear(cc):
                continue
            if solid(g, r, cc):                        # land on it, or it blocks
                if (r, cc) in stands:
                    cand.add((r, cc))
                continue
            for rr in range(r, ROWS):
                if (rr, cc) in stands:
                    cand.add((rr, cc))
                    break
                if solid(g, rr, cc):
                    break
        for s in cand:
            if s not in seen:
                seen.add(s)
                todo.append(s)
    return seen

def feet_of(g, c, r):
    """Feet cell for an entity whose tile row is r (scan down to a surface)."""
    for rr in range(r + 1, ROWS):
        if solid(g, rr, c) or plank(g, rr, c):
            return (rr, c)
    return (ROWS - 1, c)

def check_floor(f):
    g = f["grid"]
    ic, ir, oc, orow, boss = f["doors"]
    seen = flood(g, [feet_of(g, ic, ir - 1)])
    errs = []
    if feet_of(g, oc, orow - 1) not in seen:
        errs.append("EXIT UNREACHABLE")
    for kind, x, y in f["ents"]["PICKUP"]:
        if feet_of(g, x, y - 1) not in seen:
            errs.append(f"pickup k{kind} at {x},{y} unreachable")
    return errs, seen

# ----------------------------------------------------------------- rendering
NB = [(0, -1, 1), (1, -1, 2), (1, 0, 4), (1, 1, 8),
      (0, 1, 16), (-1, 1, 32), (-1, 0, 64), (-1, -1, 128)]

def load_tileset(name):
    conf = {"lut": [0] * 256, "edge": 0, "sheet": None, "tile": 16}
    for line in open(os.path.join(GAME, "tilesets", name + ".tileset")):
        t = line.split()
        if not t: continue
        if t[0] == "sheet": conf["sheet"] = Image.open(os.path.join(GAME, t[1])).convert("RGBA")
        elif t[0] == "edge": conf["edge"] = int(t[1])
        elif t[0] == "tile": conf["tile"] = int(t[1])
        elif t[0] == "lut": conf["lut"] = [int(x) for x in t[1:]]
    return conf

def thumb(path, box):
    im = Image.open(path).convert("RGBA")
    im = im.crop(im.getchannel("A").getbbox())
    im.thumbnail((box, box), Image.NEAREST)
    return im

def render_floor(f, tilesets, thumbs):
    g = f["grid"]
    TS = tilesets["solidt"]["tile"]
    bits = [[0] * COLS for _ in range(ROWS)]
    for r in range(ROWS):
        for c in range(COLS):
            ch = g[r][c]
            bits[r][c] = 8 if ch == "#" else (1 | 2 if ch == "=" else (1 if ch == "." else 0))
    out = Image.new("RGBA", (COLS * TS, ROWS * TS), (57, 49, 75, 255))
    for lname, bit in (("bgwall", 1), ("platthick", 2), ("solidt", 8)):
        ts = tilesets[lname]
        tpr = ts["sheet"].size[0] // TS
        for r in range(ROWS):
            for c in range(COLS):
                if not (bits[r][c] & bit): continue
                m = 0
                for dx, dy, b in NB:
                    rr, cc = r + dy, c + dx
                    same = ts["edge"] if not (0 <= rr < ROWS and 0 <= cc < COLS) \
                        else (bits[rr][cc] & bit) != 0
                    if same: m |= b
                cell = ts["lut"][m]
                fx, fy = (cell % tpr) * TS, (cell // tpr) * TS
                out.alpha_composite(ts["sheet"].crop((fx, fy, fx + TS, fy + TS)), (c * TS, r * TS))
    # entities
    ic, ir, oc, orow, boss = f["doors"]
    for (name, c, r) in [("door", ic, ir), ("door", oc, orow)]:
        t = thumbs[name]
        out.alpha_composite(t, (c * TS + TS // 2 - t.size[0] // 2, (r + 1) * TS - t.size[1]))
    for kind, x, y in f["ents"]["PICKUP"]:
        t = thumbs["heart" if kind == 2 else "gem"]
        out.alpha_composite(t, (x * TS + TS // 2 - t.size[0] // 2, y * TS - t.size[1] // 2))
    for (c, r) in f["ents"]["CRATE"]:
        t = thumbs["crate"]
        out.alpha_composite(t, (c * TS + TS // 2 - t.size[0] // 2, (r + 1) * TS - t.size[1]))
    for (c, r) in f["ents"]["SBOMB"]:
        t = thumbs["bomb"]
        out.alpha_composite(t, (c * TS + TS // 2 - t.size[0] // 2, (r + 1) * TS - t.size[1]))
    for (fc, c, r) in f["ents"]["CANNON"]:
        t = thumbs["cannon"]
        out.alpha_composite(t, (c * TS + TS // 2 - t.size[0] // 2, (r + 1) * TS - t.size[1]))
    for (tname, c, r) in f["ents"]["ENEMY"]:
        t = thumbs.get(tname, thumbs["pig"])
        out.alpha_composite(t, (c * TS + TS // 2 - t.size[0] // 2, (r + 1) * TS - t.size[1]))
    return out

def main():
    per_depth = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    out_path = sys.argv[2] if len(sys.argv) > 2 else "/tmp/kp_survey.png"
    depths = [1, 2, 3, 4, 5, 6]
    random.seed(99)
    floors, fails = [], []
    census = Counter(); census_d = defaultdict(Counter)
    for depth in depths:
        for i in range(per_depth):
            seed = random.randrange(1, 1 << 30)
            f = run_floor(seed, depth)
            if not f:
                fails.append((seed, depth, ["PARSE FAIL"]))
                continue
            errs, seen = check_floor(f)
            f["errs"] = errs
            floors.append(f)
            if errs:
                fails.append((seed, depth, errs))
            for (tname, _, _) in f["ents"]["ENEMY"]:
                census[tname] += 1; census_d[depth][tname] += 1
            census["cannon"] += len(f["ents"]["CANNON"])
            census_d[depth]["cannon"] += len(f["ents"]["CANNON"])
            census["crate"] += len(f["ents"]["CRATE"])
            census["sbomb"] += len(f["ents"]["SBOMB"])
            census["gem"] += sum(1 for k, _, _ in f["ents"]["PICKUP"] if k != 2)
            census["heart"] += sum(1 for k, _, _ in f["ents"]["PICKUP"] if k == 2)

    n = len(floors)
    bad = [f for f in floors if f["errs"]]
    print(f"\n==== {n} floors generated ({per_depth} per depth x {depths}) ====")
    print(f"accessible entrance->exit + all pickups: {n - len(bad)}/{n}")
    for s, d, e in fails[:20]:
        print(f"  FAIL seed={s} depth={d}: {e[:3]}")
    print("\nenemy/object census (total across floors):")
    for k, v in census.most_common():
        print(f"  {k:10s} {v}")
    print("\nenemies by depth:")
    for d in depths:
        print(f"  depth {d}: " + "  ".join(f"{k}={v}" for k, v in sorted(census_d[d].items())))

    # ---- contact sheet: 2 floors per depth (prefer a failing one if any) ----
    tilesets = {x: load_tileset(x) for x in ("bgwall", "solidt", "platthick")}
    thumbs = {
        "pig": thumb(os.path.join(GAME, "anims/pig.png"), 28),
        "boxpig": thumb(os.path.join(GAME, "anims/pigbox.png"), 28),
        "bombpig": thumb(os.path.join(GAME, "anims/pigbomb.png"), 28),
        "hidepig": thumb(os.path.join(GAME, "anims/pighide.png"), 28),
        "matchpig": thumb(os.path.join(GAME, "anims/pigmatch.png"), 28),
        "KINGPIG": thumb(os.path.join(GAME, "anims/kingpig.png"), 34),
        "cannon": thumb(os.path.join(GAME, "anims/cannon.png"), 32),
        "door": thumb(os.path.join(GAME, "anims/door.png"), 52),
        "crate": thumb(os.path.join(GAME, "anims/box.png"), 24),
        "bomb": thumb(os.path.join(GAME, "anims/bomb.png"), 20),
        "gem": thumb(os.path.join(GAME, "assets/logo.png"), 14),   # replaced below
        "heart": thumb(os.path.join(GAME, "assets/logo.png"), 14),
    }
    pick = Image.open(os.path.join(GAME, "anims/pickups.png")).convert("RGBA")
    cw, chh = pick.size[0] // 4, pick.size[1] // 10
    pk = Image.open(os.path.join(GAME, "anims/pickups.png")).convert("RGBA")
    # big diamond = clip dbig frame 0 -> cell 0; big heart idle first frame is a few cells in;
    # crop generous cells and trim alpha
    thumbs["gem"] = pk.crop((0, 0, 40, 30)).crop(pk.crop((0, 0, 40, 30)).getchannel("A").getbbox())
    hb = pk.crop((40, 30, 120, 60))
    bbx = hb.getchannel("A").getbbox()
    thumbs["heart"] = hb.crop(bbx) if bbx else thumbs["gem"]

    sel = []
    for d in depths:
        fd = [f for f in floors if f["depth"] == d]
        fd.sort(key=lambda f: (not f["errs"], random.random()))
        sel.extend(fd[:2])
    SC = 0.28
    tiles_img = []
    for f in sel:
        im = render_floor(f, tilesets, thumbs)
        im = im.resize((int(im.size[0] * SC), int(im.size[1] * SC)), Image.NEAREST)
        tiles_img.append((f, im))
    w = tiles_img[0][1].size[0]
    h = tiles_img[0][1].size[1] + 14
    out = Image.new("RGB", (w * 2 + 12, h * len(depths) + 8), (12, 12, 16))
    from PIL import ImageDraw
    dctx = ImageDraw.Draw(out)
    for i, (f, im) in enumerate(tiles_img):
        x = (i % 2) * (w + 8) + 4
        y = (i // 2) * h + 4
        tag = f"depth {f['depth']} seed {f['seed']}" + ("  boss" if f["doors"][4] else "") + \
              ("  !! " + f["errs"][0] if f["errs"] else "")
        dctx.text((x, y), tag, fill=(255, 120, 120) if f["errs"] else (200, 200, 140))
        out.paste(im.convert("RGB"), (x, y + 12))
    out.save(out_path)
    print(f"\ncontact sheet ({len(sel)} floors) -> {out_path}")

if __name__ == "__main__":
    main()
