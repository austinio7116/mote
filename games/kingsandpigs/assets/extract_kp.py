#!/usr/bin/env python3
"""Kings and Pigs asset extractor.

Turns the Pixel Frog "Kings and Pigs" pack (source/, original 1x sheets) into
Mote-ready editable assets:

  anims/<char>.png + anims/<char>.anims   packed animation grids (2x downscaled,
                                          everyone normalized to FACE RIGHT,
                                          frames aligned on a feet/center anchor)
  assets/*.png                            UI art kept at native res (dialogue
                                          bubbles, live bar, numbers, logo) +
                                          decorations (windows, banners, shelves)
  assets/{solidt,bgwall,beam,pillar}.png  16px tile sheets cut from the terrain
  tilesets/*.tileset                      autotile rulesets (nine-slice + strips)
  src/kp_meta.h                           generated sizes/anchors/colliders

Everything this script writes is an editable Studio source (or a generated meta
header) — run `mote bake games/kingsandpigs` afterwards to produce src/*.h.

Downscale is 2x decimation (a[::2,::2]) — tested against darkest-pick and box
filter; decimation keeps the art brightest and most readable at 128x128.
"""
import os
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
SRC  = os.path.join(GAME, "source")
ANIM = os.path.join(GAME, "anims")
TSET = os.path.join(GAME, "tilesets")
META = []

LOOP, ONCE, PP = 1, 0, 2
FPS = 10          # the whole pack is authored at 10 fps / 100 ms
NATIVE = True     # 1x original pixels (32px tiles); False = the old 2x downscale
CS = 32 if NATIVE else 16      # tile cell size

def load(path):
    return Image.open(os.path.join(SRC, path)).convert("RGBA")

def frames_of(img, fw):
    return [img.crop((i * fw, 0, (i + 1) * fw, img.size[1])) for i in range(img.size[0] // fw)]

def bbox(img):
    a = np.array(img.getchannel("A")) >= 128
    ys, xs = np.where(a)
    if len(xs) == 0:
        return (0, 0, 1, 1)
    return (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)

def decimate(img):
    """Plain 2x decimation — tested against blended/palette-snap variants;
    keeps the art brightest and most readable at 128x128. Used for sprites."""
    if NATIVE:
        return img.copy()
    a = np.array(img)
    return Image.fromarray(a[::2, ::2])


def decimate_tile(img):
    """Seam-preserving 2x decimation for terrain/deco tiles: plain decimation
    drops half of the 1px dark mortar joints, turning the brick pattern into
    wide featureless planks. Here a 2x2 block that contains a 'seam' pixel
    (clearly darker than the tile's typical brightness) keeps its darkest
    pixel, so every mortar line survives and the walls still read as bricks."""
    if NATIVE:
        return img.copy()
    a = np.array(img).astype(np.int32)
    lum = a[..., 0] * 3 + a[..., 1] * 6 + a[..., 2]
    op = a[..., 3] >= 128
    ref = np.median(lum[op]) if op.any() else 0
    H, W = a.shape[0] // 2, a.shape[1] // 2
    out = np.zeros((H, W, 4), dtype=np.uint8)
    for y in range(H):
        for x in range(W):
            blk = a[y * 2:y * 2 + 2, x * 2:x * 2 + 2].reshape(-1, 4)
            bl = lum[y * 2:y * 2 + 2, x * 2:x * 2 + 2].reshape(-1)
            ok = blk[:, 3] >= 128
            if not ok.any():
                continue
            dark = np.where(ok, bl, 1 << 30).argmin()
            if bl[dark] < ref * 0.80:            # a mortar/outline pixel: keep it
                out[y, x] = blk[dark]
            else:                                 # plain body: top-left sample
                out[y, x] = blk[np.argmax(ok)]
            out[y, x, 3] = 255
    return Image.fromarray(out)

# --------------------------------------------------------------- characters
# clips: (clipname, file, loop). First clip's frame 0 defines the anchor.
# flip=True mirrors every frame so the character faces RIGHT natively (the
# pig family + cannon face left in the pack; the cannon stays left, see OBJS).
CHARS = [
    ("king", "01-King Human", 78, True if 0 else False, "feet", [
        ("idle",    "Idle (78x58).png",     LOOP),
        ("run",     "Run (78x58).png",      LOOP),
        ("dead",    "Dead (78x58).png",     ONCE),
        ("doorin",  "Door In (78x58).png",  ONCE),
        ("doorout", "Door Out (78x58).png", ONCE),
        ("fall",    "Fall (78x58).png",     LOOP),
        ("ground",  "Ground (78x58).png",   ONCE),
        ("hit",     "Hit (78x58).png",      ONCE),
        ("jump",    "Jump (78x58).png",     LOOP),
    ]),
    ("kingatk", "01-King Human", 78, False, "feet", [
        ("attack",  "Attack (78x58).png",   ONCE),
    ]),
    ("kingpig", "02-King Pig", 38, True, "feet", [
        ("idle",   "Idle (38x28).png",   LOOP),
        ("run",    "Run (38x28).png",    LOOP),
        ("attack", "Attack (38x28).png", ONCE),
        ("dead",   "Dead (38x28).png",   ONCE),
        ("fall",   "Fall (38x28).png",   LOOP),
        ("ground", "Ground (38x28).png", ONCE),
        ("hit",    "Hit (38x28).png",    ONCE),
        ("jump",   "Jump (38x28).png",   LOOP),
    ]),
    ("pig", "03-Pig", 34, True, "feet", [
        ("idle",   "Idle (34x28).png",   LOOP),
        ("run",    "Run (34x28).png",    LOOP),
        ("attack", "Attack (34x28).png", ONCE),
        ("dead",   "Dead (34x28).png",   ONCE),
        ("fall",   "Fall (34x28).png",   LOOP),
        ("ground", "Ground (34x28).png", ONCE),
        ("hit",    "Hit (34x28).png",    ONCE),
        ("jump",   "Jump (34x28).png",   LOOP),
    ]),
    ("pigbox", "04-Pig Throwing a Box", 26, True, "feet", [
        ("idle",  "Idle (26x30).png",         LOOP),
        ("pick",  "Picking Box (26x30).png",  ONCE),
        ("run",   "Run (26x30).png",          LOOP),
        ("throw", "Throwing Box (26x30).png", ONCE),
    ]),
    ("pigbomb", "05-Pig Thowing a Bomb", 26, True, "feet", [
        ("idle",  "Idle (26x26).png",          LOOP),
        ("pick",  "Picking Bomb (26x26).png",  ONCE),
        ("run",   "Run (26x26).png",           LOOP),
        ("throw", "Throwing Boom (26x26).png", ONCE),
    ]),
    ("pighide", "06-Pig Hide in the Box", 26, True, "feet", [
        ("peek",   "Looking Out (26x20).png",        LOOP),
        ("prep",   "Jump Anticipation (26x20).png",  ONCE),
        ("jump",   "Jump (26x20).png",               ONCE),
        ("fall",   "Fall (26x20).png",               LOOP),
        ("ground", "Ground (26x20).png",             ONCE),
    ]),
    ("pigmatch", "07-Pig With a Match", 26, True, "feet", [
        ("matchon",  "Match On (26x18).png",            LOOP),
        ("light",    "Lighting the Match (26x18).png",  ONCE),
        ("fire",     "Lighting the Cannon (26x18).png", ONCE),
    ]),
    # ------- objects (packed the same way; anchor per table) -------
    ("box", "08-Box", 22, False, "feet", [
        ("idle", "Idle.png", LOOP),
        ("hit",  "Hit.png",  ONCE),
    ]),
    ("bomb", "09-Bomb", 52, False, "center", [
        ("off", "Bomb Off.png",        LOOP),
        ("on",  "Bomb On (52x56).png", LOOP),
    ]),
    ("boom", "09-Bomb", 52, False, "center", [
        ("go", "Boooooom (52x56).png", ONCE),
    ]),
    ("cannon", "10-Cannon", 44, False, "feet", [   # faces LEFT natively — flip at draw
        ("idle",  "Idle.png",           LOOP),
        ("shoot", "Shoot (44x28).png",  ONCE),
    ]),
    ("door", "11-Door", 46, False, "feet", [
        ("idle",  "Idle.png",               LOOP),
        ("open",  "Opening (46x56).png",    ONCE),
        ("close", "Closiong (46x56).png",   ONCE),
    ]),
    ("pickups", "12-Live and Coins", 18, False, "center1x", [
        ("dbig",     "Big Diamond Idle (18x14).png",  LOOP),
        ("dbighit",  "Big Diamond Hit (18x14).png",   ONCE),
        ("hbig",     "Big Heart Idle (18x14).png",    LOOP),
        ("hbighit",  "Big Heart Hit (18x14).png",     ONCE),
        ("dsmall",   "Small Diamond (18x14).png",     LOOP),
        ("hsmall",   "Small Heart Idle (18x14).png",  LOOP),
        ("hsmallhit","Small Heart Hit (18x14).png",   ONCE),
    ]),
]

def pack_char(name, subdir, fw, flip, anchor_mode, clips):
    sc = 1 if (NATIVE or anchor_mode.endswith("1x")) else 2      # native-res sheets keep UI art readable
    anchor_mode = anchor_mode.replace("1x", "")
    # load all frames
    allfr = []          # (clip_i, PIL frame)
    for ci, (cname, fname, loop) in enumerate(clips):
        img = load(os.path.join(subdir, fname))
        for f in frames_of(img, fw):
            if flip:
                f = f.transpose(Image.FLIP_LEFT_RIGHT)
            allfr.append((ci, f))

    # anchor from the FIRST frame of the FIRST clip
    x0, y0, x1, y1 = bbox(allfr[0][1])
    ax = (x0 + x1) // 2
    ay_feet = y1
    ay_mid = (y0 + y1) // 2

    # union extents relative to the anchor
    L = R = T = B = 1
    for _, f in allfr:
        bx0, by0, bx1, by1 = bbox(f)
        L = max(L, ax - bx0); R = max(R, bx1 - ax)
        if anchor_mode == "feet":
            T = max(T, ay_feet - by0); B = max(B, by1 - ay_feet)
        else:
            T = max(T, ay_mid - by0); B = max(B, by1 - ay_mid)
    half = max(L, R)
    half += half & 1                       # even
    T += T & 1; B += B & 1
    cw_nat, ch_nat = 2 * half, T + B
    ay = ay_feet if anchor_mode == "feet" else ay_mid

    cols = min(8, len(allfr))
    rows = (len(allfr) + cols - 1) // cols
    grid = Image.new("RGBA", (cols * cw_nat, rows * ch_nat), (0, 0, 0, 0))
    for i, (_, f) in enumerate(allfr):
        cx, cy = (i % cols) * cw_nat, (i // cols) * ch_nat
        grid.paste(f, (cx + half - ax, cy + T - ay), f)
    small = decimate(grid) if sc == 2 else grid
    small.save(os.path.join(ANIM, name + ".png"))

    # .anims
    cw, ch = cw_nat // sc, ch_nat // sc
    lines = [f"sheet anims/{name}.png", f"tile {cw} {ch}", f"clips {len(clips)}"]
    fi = 0
    for ci, (cname, fname, loop) in enumerate(clips):
        n = sum(1 for c, _ in allfr if c == ci)
        lines.append(f"clip {cname} {loop} {FPS} 0 0 {n}")
        for k in range(n):
            lines.append(f"f {fi} 0 -")
            fi += 1
    open(os.path.join(ANIM, name + ".anims"), "w").write("\n".join(lines) + "\n")

    # meta: cell size, anchor (cell-space), body box of the ref frame (downscaled)
    bw, bh = (x1 - x0) // sc, (y1 - y0) // sc
    U = name.upper()
    META.append(f"/* {name}: cell {cw}x{ch}, anchor=({cw//2},{T//sc}), ref body {bw}x{bh} */")
    META.append(f"#define KP_{U}_CW   {cw}")
    META.append(f"#define KP_{U}_CH   {ch}")
    META.append(f"#define KP_{U}_AX   {cw // 2}")
    META.append(f"#define KP_{U}_AY   {T // sc}")
    META.append(f"#define KP_{U}_BW   {bw}")
    META.append(f"#define KP_{U}_BH   {bh}")
    print(f"[char] {name}: {len(allfr)} frames, cell {cw}x{ch} (native {cw_nat}x{ch_nat}), body {bw}x{bh}")

# ------------------------------------------------------------------ UI / misc
def save_img(im, name):
    im.save(os.path.join(HERE, name + ".png"))

def build_ui():
    # dialogue: 10 types x [in0 in1 in2 out0 out1], native 34x16 frames
    types = ["Hello", "Hi", "!!!", "Interrogation", "Attack", "Boom", "Dead", "No", "WTF", "Loser"]
    FW, FH = 34, 16
    sheet = Image.new("RGBA", (5 * FW, len(types) * FH), (0, 0, 0, 0))
    for r, t in enumerate(types):
        i_img = load(os.path.join("13-Dialogue Boxes", f"{t} In (24x8).png"))
        o_img = load(os.path.join("13-Dialogue Boxes", f"{t} Out (24x8).png"))
        for c, f in enumerate(frames_of(i_img, FW)):
            sheet.paste(f, (c * FW, r * FH), f)
        for c, f in enumerate(frames_of(o_img, FW)):
            sheet.paste(f, ((3 + c) * FW, r * FH), f)
    save_img(sheet, "dialogue")
    META.append("/* dialogue.png: rows = HELLO,HI,EXCLAIM,QUESTION,ATTACK,BOOM,DEAD,NO,WTF,LOSER;")
    META.append(" * cols = in0,in1,in2,out0,out1 (34x16 native) */")
    META.append("#define KP_DLG_FW 34")
    META.append("#define KP_DLG_FH 16")

    # live bar: erase the three painted hearts (the HUD draws animated small
    # hearts over the slots) and emit the slot positions to meta
    lb = load("12-Live and Coins/Live Bar.png")
    a = np.array(lb)
    a[13:22, 17:50] = (209, 134, 121, 255)   # flat banner body over the painted hearts
    lb = Image.fromarray(a)
    save_img(lb, "livebar")                  # native (title screens etc.)
    save_img(decimate(lb), "livebars")       # half-size HUD version (33x17)
    slots = [(22, 17), (33, 17), (44, 17)]   # heart slot centres in the banner
    META.append("/* livebars.png: half-size live bar; slots below are half-res too */")
    for i, (sx, sy) in enumerate(slots):
        META.append(f"#define KP_LBS_HX{i} {sx // 2}")
        META.append(f"#define KP_LBS_HY{i} {sy // 2}")
    META.append("/* livebar heart slot centres (native px) */")
    for i, (sx, sy) in enumerate(slots):
        META.append(f"#define KP_LB_HX{i} {sx}")
        META.append(f"#define KP_LB_HY{i} {sy}")
    save_img(load("12-Live and Coins/Numbers (6x8).png"), "numbers")   # digits 1..9,0
    save_img(load("Kings and Pigs.png"), "logo")
    META.append("/* numbers.png: 6x8 digits in order 1234567890 (digit d -> col d? d-1 : 9) */")

    # box debris pieces: 4 cells of 5x5 (downscaled 10x10)
    ps = Image.new("RGBA", (4 * 5, 5), (0, 0, 0, 0))
    for i in range(4):
        p = decimate(load(f"08-Box/Box Pieces {i + 1}.png"))
        ps.paste(p, (i * 5, 0), p)
    save_img(ps, "pieces")

    # cannonball: tight crop, downscaled
    b = load("10-Cannon/Cannon Ball.png")
    x0, y0, x1, y1 = bbox(b)
    x0 -= x0 & 1; y0 -= y0 & 1
    ball = decimate(b.crop((x0, y0, x1 + (x1 & 1), y1 + (y1 & 1))))
    save_img(ball, "ball")
    META.append(f"#define KP_BALL_W {ball.size[0]}")
    META.append(f"#define KP_BALL_H {ball.size[1]}")

def build_deco():
    deco = load("14-TileSets/Decorations (32x32).png")
    def cut(px0, py0, px1, py1):
        px0 -= px0 & 1; py0 -= py0 & 1
        return decimate_tile(deco.crop((px0, py0, px1 + (px1 & 1), py1 + (py1 & 1))))
    save_img(cut(35, 32, 61, 121), "banner1")     # tall banner  -> 13x45
    save_img(cut(35, 128, 61, 153), "banner2")    # short banner -> 13x13
    save_img(cut(73, 103, 107, 146), "window")    # arched window + light shaft
    # shelves: thin plank (7px tall) and metal-capped beam (15px tall), L/M/R
    # sections; each native 32px section -> one 16px cell
    thin = decimate_tile(deco.crop((64, 32, 192, 40)))
    save_img(thin.crop((0, 0, 48, 4)), "shelfa")            # L,M,R of the thin plank
    beam = decimate_tile(deco.crop((64, 64, 192, 80)))
    save_img(_beam_lmr(beam), "shelfb")

def _beam_lmr(beam8):   # sections 0,1,3 of the 64px-wide downscaled beam strip
    out = Image.new("RGBA", (48, 8), (0, 0, 0, 0))
    out.paste(beam8.crop((0, 0, 16, 8)), (0, 0))
    out.paste(beam8.crop((16, 0, 32, 8)), (16, 0))
    out.paste(beam8.crop((32, 0, 48, 8)), (32, 0))
    return out

# ------------------------------------------------------------------- tiles
NB_N, NB_NE, NB_E, NB_SE, NB_S, NB_SW, NB_W, NB_NW = (1, 2, 4, 8, 16, 32, 64, 128)

def write_tileset(name, sheet_rel, edge, lut, xform=None):
    lines = [f"sheet {sheet_rel}", f"tile {CS}", f"edge {edge}", "nvar 1",
             "lut " + " ".join(str(v) for v in lut),
             "xform " + " ".join(str(v) for v in (xform or [0] * 256)),
             "vweight 1 1 1 1 1 1 1 1"]
    open(os.path.join(TSET, name + ".tileset"), "w").write("\n".join(lines) + "\n")

def reduce_mask(m):
    """Drop a corner bit unless both adjacent cardinals are set (blob47 reduction,
    mirrors mote__at_reduce)."""
    N, E, S, W = bool(m & NB_N), bool(m & NB_E), bool(m & NB_S), bool(m & NB_W)
    r = m & (NB_N | NB_E | NB_S | NB_W)
    if (m & NB_NE) and N and E: r |= NB_NE
    if (m & NB_SE) and S and E: r |= NB_SE
    if (m & NB_SW) and S and W: r |= NB_SW
    if (m & NB_NW) and N and W: r |= NB_NW
    return r

# The packed wall sheet keeps the SOURCE terrain layout (cols 1-17, rows 1-5 of
# the 32px sheet -> a 17x5 cell grid), so every cell is recognisable in the
# Studio Tiles tab. Cell index for source tile (sx, sy):
def _cell(sx, sy):
    return (sy - 1) * 17 + (sx - 1)

def blob47_lut():
    """The pack IS a complete 47-piece blob set — every neighbour configuration
    has real art (verified by trim-nub measurement of every tile):
      nine-slice (1,1)-(3,3) · column (5,1)-(5,3) · beam (1,5)-(3,5) · single (5,5)
      inner corners: singles (7,1)-(8,2) · adjacent pairs (10,1)-(11,2) ·
      triples (13,1)-(14,2) · opposites (16,2),(17,2) · quad (16,1)
      edge+diag (7,4)-(11,5) · edge+both diags (13,4)-(14,5) · corner+diag (16,4)-(17,5)
    """
    lut = [0] * 256
    for m in range(256):
        rm = reduce_mask(m)
        N, E, S, W = bool(rm & NB_N), bool(rm & NB_E), bool(rm & NB_S), bool(rm & NB_W)
        # open diagonals, only where both adjacent cardinals are solid
        NWo = N and W and not (rm & NB_NW)
        NEo = N and E and not (rm & NB_NE)
        SEo = S and E and not (rm & NB_SE)
        SWo = S and W and not (rm & NB_SW)
        nC = N + E + S + W
        if nC == 0:
            c = _cell(5, 5)                                    # isolated block
        elif nC == 1:
            if E:   c = _cell(1, 5)                            # beam left end
            elif W: c = _cell(3, 5)                            # beam right end
            elif S: c = _cell(5, 1)                            # column top
            else:   c = _cell(5, 3)                            # column bottom
        elif nC == 2 and N and S:
            c = _cell(5, 2)                                    # column middle
        elif nC == 2 and E and W:
            c = _cell(2, 5)                                    # beam middle
        elif nC == 2:                                          # corner (+diag)
            if S and E:   c = _cell(16, 4) if SEo else _cell(1, 1)   # TL corner
            elif S and W: c = _cell(17, 4) if SWo else _cell(3, 1)   # TR corner
            elif N and E: c = _cell(16, 5) if NEo else _cell(1, 3)   # BL corner
            else:         c = _cell(17, 5) if NWo else _cell(3, 3)   # BR corner
        elif nC == 3:                                          # edge (+diags)
            if not N:      # top edge: SE/SW relevant
                c = _cell(13, 4) if (SEo and SWo) else                     _cell(7, 4) if SEo else _cell(8, 4) if SWo else _cell(2, 1)
            elif not S:    # bottom edge: NE/NW relevant
                c = _cell(14, 5) if (NEo and NWo) else                     _cell(7, 5) if NEo else _cell(8, 5) if NWo else _cell(2, 3)
            elif not W:    # left edge: NE/SE relevant
                c = _cell(13, 5) if (NEo and SEo) else                     _cell(10, 5) if NEo else _cell(10, 4) if SEo else _cell(1, 2)
            else:          # right edge: NW/SW relevant
                c = _cell(14, 4) if (NWo and SWo) else                     _cell(11, 5) if NWo else _cell(11, 4) if SWo else _cell(3, 2)
        else:                                                  # enclosed
            D = (NWo, NEo, SEo, SWo)
            n = sum(D)
            if n == 0:   c = _cell(2, 2)                       # plain fill
            elif n == 1:
                c = _cell(8, 2) if NWo else _cell(7, 2) if NEo else                     _cell(7, 1) if SEo else _cell(8, 1)
            elif n == 2:
                if NWo and NEo:   c = _cell(11, 1)             # top pair
                elif SWo and SEo: c = _cell(10, 2)             # bottom pair
                elif NWo and SWo: c = _cell(10, 1)             # left pair
                elif NEo and SEo: c = _cell(11, 2)             # right pair
                elif NEo and SWo: c = _cell(16, 2)
                else:             c = _cell(17, 2)             # NW+SE
            elif n == 3:
                c = _cell(13, 1) if not NWo else _cell(14, 1) if not NEo else                     _cell(13, 2) if not SWo else _cell(14, 2)
            else:
                c = _cell(16, 1)                               # all four
        lut[m] = c
    return lut

def strip_lut_h():      # [L, M, R, single] by W/E
    lut = [0] * 256
    for m in range(256):
        W, E = bool(m & NB_W), bool(m & NB_E)
        lut[m] = 1 if (W and E) else (0 if E else (2 if W else 3))
    return lut

def build_tiles():
    terr = load("14-TileSets/Terrain (32x32).png")
    def tile16(tx, ty):
        return decimate_tile(terr.crop((tx * 32, ty * 32, tx * 32 + 32, ty * 32 + 32)))
    (void_ref,) = (tile16,)   # keep name; cells are CS px in NATIVE mode

    # Pack the COMPLETE solid + bg wall sets, keeping the source sheet layout
    # (a 17x5 grid of 16px cells) so every tile is where you expect it in the
    # Studio Tiles tab. blob47_lut() assigns all 47 pieces.
    def wall_sheet(row0):
        out = Image.new("RGBA", (17 * CS, 5 * CS), (0, 0, 0, 0))
        for sy in range(5):
            for sx in range(17):
                t = tile16(1 + sx, row0 + sy)
                out.paste(t, (sx * CS, sy * CS))
        return out
    save_img(wall_sheet(1), "solidt")    # bright-trim castle wall (source rows 1-5)
    save_img(wall_sheet(7), "bgwall")    # pink brick background (source rows 7-11)
    lut = blob47_lut()
    write_tileset("solidt", "assets/solidt.png", 1, lut)
    write_tileset("bgwall", "assets/bgwall.png", 1, lut)

    # the two wooden platforms from the Decorations sheet: 4 sections of 32px
    # each (L, M, R, single) -> 4 cells of 16px
    deco = load("14-TileSets/Decorations (32x32).png")
    def plat_strip(y0, y1, name):
        strip = decimate_tile(deco.crop((64, y0, 192, y1)))
        h = strip.size[1]
        out = Image.new("RGBA", (4 * CS, CS), (0, 0, 0, 0))
        for i in range(4):
            out.paste(strip.crop((i * CS, 0, i * CS + CS, h)), (i * CS, 0))
        save_img(out, name)
    plat_strip(32, 40, "platthin")       # thin plank (~4px)
    plat_strip(64, 80, "platthick")      # thick beam with metal ends (~8px)
    write_tileset("platthin",  "assets/platthin.png",  0, strip_lut_h())
    write_tileset("platthick", "assets/platthick.png", 0, strip_lut_h())

def main():
    os.makedirs(ANIM, exist_ok=True)
    os.makedirs(TSET, exist_ok=True)
    META.append("/* GENERATED by assets/extract_kp.py — sizes/anchors for the packed sheets.")
    META.append(" * Regenerate with: python3 assets/extract_kp.py (then mote bake). */")
    META.append("#ifndef KP_META_H")
    META.append("#define KP_META_H")
    for spec in CHARS:
        pack_char(*spec)
    build_ui()
    build_deco()
    build_tiles()
    META.append("#endif")
    open(os.path.join(GAME, "src", "kp_meta.h"), "w").write("\n".join(META) + "\n")
    print("[meta] src/kp_meta.h written")

if __name__ == "__main__":
    main()
