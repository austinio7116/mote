#!/usr/bin/env python3
"""SCRAPWING art generator — the EDITABLE SOURCE for every sprite/tile sheet.

Writes tiles+props+gate (ships/weapons/bosses/icon come from extract_sheets.py).
All editable in Mote Studio's Pixel Art tab, re-bake with `mote bake`):
  assets/ships.png     8x6 grid of 16x12 tiny pixelships (cell 0 = player)
  assets/bigships.png  3x2 grid of 32x24 heavy hunters
  assets/props.png     row of 8x8 cells: 6 weapon chips + turret x2 + core x2
  assets/gate.png      2 frames 16x24 exit gate
  assets/rock.png      BLOB47 autotile sheet, 47 cells 8x8, 2 variant rows
  assets/hull.png      BLOB47 autotile sheet, 47 cells 8x8, 2 variant rows
  ../icon.png          60x60 launcher icon
  ../src/ships_meta.h  per-cell opaque bbox (collision sizing) + shape names

Ships are procedural in the pixelships.com spirit: seeded hull profile facing
RIGHT, top canopy, tail engine, swept fins, outline + top-light/bottom-shade.
Tweak a seed/palette below and re-run; the PNGs are the deliverable.
"""
import os, random
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

# ------------------------------------------------------------------ helpers
def clamp(v, a, b): return a if v < a else b if v > b else v

def shade(c, f):
    return (clamp(int(c[0]*f), 0, 255), clamp(int(c[1]*f), 0, 255),
            clamp(int(c[2]*f), 0, 255), 255)

def put(img, x, y, c):
    if 0 <= x < img.width and 0 <= y < img.height:
        img.putpixel((x, y), c)

# ------------------------------------------------------------------ ships
CW, CH = 16, 12           # ship cell size
COLS, ROWS = 8, 6         # sheet grid

PALETTES = [  # (base, accent, canopy)
    ((172, 46, 58),  (240, 196, 80),  (140, 220, 255)),   # crimson raider
    ((64, 96, 176),  (230, 235, 245), (255, 210, 120)),   # cobalt navy
    ((150, 96, 52),  (90, 210, 190),  (170, 240, 255)),   # rust pirate
    ((62, 148, 96),  (220, 90, 200),  (255, 250, 180)),   # viridian swarm
]

SHAPE_NAMES = ["WASP", "DART", "MANTA", "HORNET", "SKIFF", "VIPER",
               "LANCE", "GNAT", "RAY", "TALON", "MOTH", "DRONE"]

PLAYER_ART = [
    "................",
    "......55........",
    "..8..15551......",
    ".88114444411....",
    "E881444444441...",
    "E88144CC4444441.",
    "E881444444444441",
    ".881444444411...",
    "..8114444411....",
    "..8..15551......",
    "......55........",
    "................",
]
PLAYER_KEY = {
    '1': (30, 34, 52, 255),        # dark hull outline-ish
    '4': (120, 150, 190, 255),     # base steel-blue
    '5': (200, 220, 245, 255),     # light fin
    'C': (120, 230, 255, 255),     # canopy
    '8': (70, 78, 100, 255),       # engine casing
    'E': (255, 170, 60, 255),      # engine glow
}

def draw_player(img, ox, oy):
    for y, row in enumerate(PLAYER_ART):
        for x, ch in enumerate(row):
            if ch in PLAYER_KEY:
                put(img, ox + x, oy + y, PLAYER_KEY[ch])

def smooth_profile(rng, n, lo, hi, style):
    """Half-thickness per column (tail at x=0, nose at x=n-1).
    style 0 = rounded, 1 = wedge (fat tail, thin nose), 2 = needle."""
    if style == 2:
        hi = min(hi, lo + 1.2)
    k = [rng.uniform(lo, hi) for _ in range(4)]
    if style == 1:
        k = sorted(k, reverse=True)
    prof = []
    for i in range(n):
        t = i / max(1, n - 1)
        seg = t * 3
        j = min(2, int(seg)); f = seg - j
        v = k[j] * (1 - f) + k[j + 1] * f
        v *= 1.0 - 0.8 * max(0.0, t - 0.55) / 0.45        # taper to the nose
        v *= 0.55 + 0.45 * min(1.0, i / 2.5)              # taper at the tail too
        prof.append(v)
    return prof

def gen_ship(img, ox, oy, w, h, seed, pal, styles=(0, 0, 1, 1, 2)):
    """One pixelship, nose pointing RIGHT, inside a w*h cell at (ox,oy)."""
    rng = random.Random(seed)
    base, accent, canopy = pal
    cy = h // 2
    style = rng.choice(styles)
    L = rng.randint(w - 5, w - 2)          # hull length
    if style == 2:
        L = w - 2                           # needles run long
    x0 = 1                                  # tail x
    maxt = (h - 4) / 2
    top = smooth_profile(rng, L, 0.8, maxt, style)
    bot = smooth_profile(rng, L, 0.8, maxt, style)
    solid = [[False] * w for _ in range(h)]

    for i in range(L):
        x = x0 + i
        t0, t1 = int(round(top[i])), int(round(bot[i]))
        for y in range(cy - t0, cy + t1 + 1):
            if 0 <= y < h:
                solid[y][x] = True

    # nose spike on some ships
    if rng.random() < 0.45 and x0 + L < w:
        for s in range(rng.randint(1, w - (x0 + L))):
            solid[cy][x0 + L + s] = True

    # swept fins: connected right-triangles rising off the hull spine
    nfin = rng.choice([1, 1, 2])
    for f in range(nfin):
        fx = x0 + rng.randint(1, max(2, L // 3))
        flen = rng.randint(2, 3)
        both = rng.random() < 0.7
        for j in range(flen):
            xx = fx + j
            if xx - x0 >= L: break
            ht = flen - j                       # column height shrinks toward nose
            ytop = cy - int(round(top[xx - x0]))
            for q in range(1, ht + 1):
                if 0 <= ytop - q < h: solid[ytop - q][xx] = True
            if both:
                ybot = cy + int(round(bot[xx - x0]))
                for q in range(1, ht + 1):
                    if 0 <= ybot + q < h: solid[ybot + q][xx] = True

    # paint: base + top-light + bottom-shade + outline
    dark = shade(base, 0.45); lite = shade(base, 1.45); mid = (*base, 255)
    for y in range(h):
        for x in range(w):
            if not solid[y][x]:
                continue
            up = y > 0 and solid[y - 1][x]
            dn = y < h - 1 and solid[y + 1][x]
            c = mid
            if not up: c = lite
            elif not dn: c = dark
            put(img, ox + x, oy + y, c)

    # accent stripe along the mid row
    sy = cy + rng.choice([-1, 0, 0, 1])
    for i in range(L):
        x = x0 + i
        if solid[sy][x] and rng.random() < 0.85 and 2 < i < L - 2:
            put(img, ox + x, oy + sy, (*accent, 255))

    # canopy near the nose, upper half
    cx = x0 + int(L * rng.uniform(0.55, 0.72))
    clen = rng.randint(2, 3)
    for j in range(clen):
        yy = cy - max(0, int(round(top[clamp(cx - x0, 0, L - 1)])) - 1)
        if solid[yy][cx + j]:
            put(img, ox + cx + j, oy + yy, (*canopy, 255))

    # engine: casing column + a small glow cluster at the tail centre
    ecas = shade(base, 0.6)
    for y in range(h):
        if solid[y][x0]:
            put(img, ox + x0, oy + y, ecas)
            if abs(y - cy) <= 1:
                put(img, ox + x0 - 1, oy + y, (255, 170, 60, 255))

def measure(img, ox, oy, w, h):
    """Opaque bbox of a cell."""
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            if img.getpixel((ox + x, oy + y))[3] >= 128:
                x0 = min(x0, x); y0 = min(y0, y); x1 = max(x1, x); y1 = max(y1, y)
    if x1 < 0: return (0, 0, 0, 0)
    return (x0, y0, x1 - x0 + 1, y1 - y0 + 1)

def make_ships():
    img = Image.new("RGBA", (COLS * CW, ROWS * CH), (0, 0, 0, 0))
    meta = []
    for cell in range(COLS * ROWS):
        ox, oy = (cell % COLS) * CW, (cell // COLS) * CH
        if cell == 0:
            draw_player(img, ox, oy)
        else:
            shape = (cell - 1) % len(SHAPE_NAMES)
            pal = PALETTES[((cell - 1) // len(SHAPE_NAMES)) % len(PALETTES)]
            gen_ship(img, ox, oy, CW, CH, 7700 + shape * 31, pal)
        meta.append(measure(img, ox, oy, CW, CH))
    img.save(os.path.join(HERE, "ships.png"))
    return meta

def make_bigships():
    BW, BH = 32, 24
    img = Image.new("RGBA", (3 * BW, 2 * BH), (0, 0, 0, 0))
    meta = []
    for cell in range(6):
        ox, oy = (cell % 3) * BW, (cell // 3) * BH
        pal = PALETTES[cell % len(PALETTES)]
        gen_ship(img, ox, oy, BW, BH, 5100 + cell * 97, pal, styles=(0, 0, 1))
        meta.append(measure(img, ox, oy, BW, BH))
    img.save(os.path.join(HERE, "bigships.png"))
    return meta

# ------------------------------------------------------------------ props
ELEMENT_COLORS = [   # matches EL_* order in game.c
    (255, 214, 80),   # PULSE  gold
    (90, 225, 255),   # PLASMA cyan
    (255, 130, 50),   # FIRE   orange
    (200, 220, 255),  # VOLT   white-blue
    (120, 255, 100),  # VENOM  green
    (220, 100, 255),  # VOID   violet
]

def make_props():
    """Row of 8x8 cells: chips[6], turret closed, turret open, core pulse x2."""
    n = 10
    img = Image.new("RGBA", (n * 8, 8), (0, 0, 0, 0))
    for e, col in enumerate(ELEMENT_COLORS):
        ox = e * 8
        # weapon chip: bright diamond w/ dark frame + inner spark
        pts = [(3, 1), (4, 1), (1, 3), (1, 4), (6, 3), (6, 4), (3, 6), (4, 6),
               (2, 2), (5, 2), (2, 5), (5, 5)]
        for (x, y) in pts: put(img, ox + x, y, shade(col, 0.5))
        for (x, y) in [(3, 2), (4, 2), (2, 3), (5, 3), (2, 4), (5, 4), (3, 5), (4, 5)]:
            put(img, ox + x, y, (*col, 255))
        put(img, ox + 3, 3, (255, 255, 255, 255))
        put(img, ox + 4, 4, shade(col, 1.3))
    # turret: dome on plate, frame 2 = barrel out
    for f in range(2):
        ox = (6 + f) * 8
        for x in range(1, 7): put(img, ox + x, 7, (70, 78, 100, 255))
        for x in range(2, 6):
            for y in range(4, 7): put(img, ox + x, y, (120, 130, 160, 255))
        for x in range(3, 5): put(img, ox + x, 3, (170, 180, 210, 255))
        put(img, ox + 3, 4, (255, 90, 90, 255)); put(img, ox + 4, 4, (255, 90, 90, 255))
        if f: put(img, ox + 5, 2, (200, 210, 235, 255)); put(img, ox + 6, 1, (200, 210, 235, 255))
    # sector core (gate key item): pulsing orb, 2 frames
    for f in range(2):
        ox = (8 + f) * 8
        r = 2 + f
        for y in range(8):
            for x in range(8):
                d2 = (x - 3.5) ** 2 + (y - 3.5) ** 2
                if d2 <= r * r:
                    put(img, ox + x, y, (255, 255, 255, 255) if d2 <= 1 else (140, 220, 255, 255))
    img.save(os.path.join(HERE, "props.png"))

def make_gate():
    img = Image.new("RGBA", (32, 24), (0, 0, 0, 0))
    for f in range(2):
        ox = f * 16
        for y in range(24):
            for x in range(16):
                onx = x in (2, 13); ony = y in (1, 22)
                inx = 2 <= x <= 13; iny = 1 <= y <= 22
                if (onx and iny) or (ony and inx):
                    put(img, ox + x, y, (90, 200, 220, 255))
                elif 4 <= x <= 11 and 3 <= y <= 20:
                    v = (x + y * 2 + f * 3) % 6
                    if v < 2:
                        put(img, ox + x, y, (40 + 30 * f, 120 + 40 * f, 200, 255))
        put(img, ox + 7, 0, (200, 255, 255, 255)); put(img, ox + 8, 23, (200, 255, 255, 255))
    img.save(os.path.join(HERE, "gate.png"))

# ------------------------------------------------------------------ tiles
NB_N, NB_NE, NB_E, NB_SE, NB_S, NB_SW, NB_W, NB_NW = (1, 2, 4, 8, 16, 32, 64, 128)

def reduce_mask(m):
    r = m & (NB_N | NB_E | NB_S | NB_W)
    if (m & NB_NE) and (r & NB_N) and (r & NB_E): r |= NB_NE
    if (m & NB_SE) and (r & NB_S) and (r & NB_E): r |= NB_SE
    if (m & NB_SW) and (r & NB_S) and (r & NB_W): r |= NB_SW
    if (m & NB_NW) and (r & NB_N) and (r & NB_W): r |= NB_NW
    return r

def blob47_cells():
    """Canonical first-seen order — MUST match mote_autotile_template(BLOB47)."""
    seen, cells = {}, []
    for m in range(256):
        r = reduce_mask(m)
        if r not in seen:
            seen[r] = len(cells); cells.append(r)
    return cells

def hash2(x, y):
    h = (x * 73856093) ^ (y * 19349663)
    h ^= (h >> 13) & 0x7FFFFFFF
    return (h * 1274126177) & 0xFFFFFFFF

TS = 8  # tile size

def draw_tile(img, ox, oy, mask, style, var):
    """One 8x8 autotile cell. mask bits = same-terrain neighbour PRESENT."""
    if style == "rock":
        b0 = (96, 74, 78); b1 = (78, 58, 66); b2 = (112, 90, 92)
        edge_lit = (168, 140, 130); edge_dk = (46, 32, 42)
        fleck = (130, 108, 100) if var == 0 else (110, 150, 140)
    else:  # hull
        b0 = (74, 88, 100); b1 = (62, 74, 86); b2 = (86, 102, 116)
        edge_lit = (140, 170, 190); edge_dk = (34, 42, 52)
        fleck = (100, 118, 132) if var == 0 else (96, 130, 126)
    for y in range(TS):
        for x in range(TS):
            # coarse 2x2 patches so the fill reads as rock strata, not static
            h = hash2((ox * 31 + x + var * 7) >> 1, (oy * 17 + y) >> 1)
            c = b0 if h % 7 < 5 else (b1 if h % 7 == 5 else b2)
            if style == "hull":
                # panel lines
                if (x + ox // TS) % 4 == 3 or (y + oy // TS) % 4 == 3:
                    c = b1
                if (h >> 8) % 41 == 0:
                    c = fleck  # rivet
            else:
                if (h >> 8) % 29 == 0:
                    c = fleck  # mineral fleck
            put(img, ox + x, oy + y, (*c, 255))
    # exposed edges (neighbour ABSENT): lit on top, shadow below, sides mid
    if not (mask & NB_N):
        for x in range(TS):
            put(img, ox + x, oy, (*edge_lit, 255))
            put(img, ox + x, oy + 1, shade(edge_lit, 0.72))
    if not (mask & NB_S):
        for x in range(TS):
            put(img, ox + x, oy + TS - 1, (*edge_dk, 255))
    if not (mask & NB_W):
        for y in range(TS):
            put(img, ox, oy + y, shade(edge_lit, 0.62))
    if not (mask & NB_E):
        for y in range(TS):
            put(img, ox + TS - 1, oy + y, shade(edge_dk, 1.4))
    # inner corners: both cardinals present but the diagonal missing
    if (mask & NB_N) and (mask & NB_E) and not (mask & NB_NE):
        put(img, ox + TS - 1, oy, (*edge_lit, 255))
    if (mask & NB_N) and (mask & NB_W) and not (mask & NB_NW):
        put(img, ox, oy, (*edge_lit, 255))
    if (mask & NB_S) and (mask & NB_E) and not (mask & NB_SE):
        put(img, ox + TS - 1, oy + TS - 1, (*edge_dk, 255))
    if (mask & NB_S) and (mask & NB_W) and not (mask & NB_SW):
        put(img, ox, oy + TS - 1, (*edge_dk, 255))

def make_tiles(style):
    cells = blob47_cells()
    nvar = 2
    img = Image.new("RGBA", (len(cells) * TS, nvar * TS), (0, 0, 0, 0))
    for v in range(nvar):
        for i, m in enumerate(cells):
            draw_tile(img, i * TS, v * TS, m, style, v)
    img.save(os.path.join(HERE, style + ".png"))
    return len(cells)

# ------------------------------------------------------------------ icon
def make_icon():
    img = Image.new("RGBA", (60, 60), (10, 10, 24, 255))
    rng = random.Random(4)
    for _ in range(40):
        x, y = rng.randrange(60), rng.randrange(60)
        v = rng.choice([90, 140, 200])
        put(img, x, y, (v, v, min(255, v + 30), 255))
    # nebula wisp
    for _ in range(160):
        x, y = rng.randrange(60), rng.randrange(60)
        if (x - 44) ** 2 + (y - 14) ** 2 < 180:
            put(img, x, y, (60, 30, 80, 255))
    # player ship at 3x, centred-ish
    ship = Image.new("RGBA", (16, 12), (0, 0, 0, 0))
    draw_player(ship, 0, 0)
    big = ship.resize((48, 36), Image.NEAREST)
    img.alpha_composite(big, (5, 14))
    # weapon chips trailing
    for i, e in enumerate([1, 5, 2]):
        col = ELEMENT_COLORS[e]
        cx, cy = 10 + i * 5, 52 - i * 3
        for dx, dy in [(0, -1), (-1, 0), (0, 0), (1, 0), (0, 1)]:
            put(img, cx + dx, cy + dy, (*col, 255))
    img.convert("RGB").save(os.path.join(GAME, "icon.png"))

# ------------------------------------------------------------------ meta
def write_meta(meta, bigmeta):
    p = os.path.join(GAME, "src", "ships_meta.h")
    with open(p, "w") as f:
        f.write("/* GENERATED by assets/make_art.py — per-cell opaque bboxes for the ship\n"
                " * sheets (collision sizing + pixel-shatter bounds). Edit the script. */\n"
                "#ifndef SHIPS_META_H\n#define SHIPS_META_H\n\n")
        for name, m, note in (("ship", meta, "16x12 cells, cell 0 = player"),
                              ("bigship", bigmeta, "32x24 cells")):
            f.write("/* %s */\n" % note)
            for fi, field in enumerate(("bx", "by", "bw", "bh")):
                f.write("static const uint8_t %s_%s[%d] = {%s};\n" %
                        (name, field, len(m), ",".join(str(mm[fi]) for mm in m)))
        f.write("\nstatic const char *const ship_shape_name[%d] = {%s};\n" %
                (len(SHAPE_NAMES), ",".join('"%s"' % s for s in SHAPE_NAMES)))
        f.write("\n#endif\n")

if __name__ == "__main__":
    make_props()
    make_gate()
    n = make_tiles("rock"); make_tiles("hull")
    print("blob47 cells:", n)
    print("done")
