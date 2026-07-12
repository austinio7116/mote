#!/usr/bin/env python3
"""TerraMote sheet EXTRACTOR — turns the developer-supplied AI sheets into the
game's editable sprite PNGs (overwriting the procedural placeholders).

Sources (checked in, editable):
  sources_sheet1.png         characters/enemies/boss/items on a fake-checker bg
  sources_sheet2_weapons.png Terraria-style weapon icons in a dark grid
  sources_sheet3_hair.png    100 greyscale hairstyles in a light grid

Run AFTER make_sprites.py (which lays down the procedural base + items grid):
  python3 assets/make_sprites.py && python3 assets/extract_sheets.py
then `mote bake` the game dir.

Outputs: player.png (+anims/player.anims), hair.png, slime*.png, skeleton.png,
eye.png, bat.png, eoc.png, and curated cells patched into items.png (16px grid).

The player + hair are QUANTIZED to the reserved builder palette (see
make_sprites.py / player.c) so runtime recolouring keeps working.
"""
import os, json
import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
HEAD_CX, HEAD_CY = 8.0, 7.0    # measured from player.png cell 0 (make_player)

# ---- reserved palette (must match make_sprites.py / player.c) --------------
OUTLINE  = (26, 20, 26)
SKIN     = (232, 190, 150); SKIN_SH  = (188, 140, 104)
HAIR     = (140, 88, 40);   HAIR_SH  = (94, 58, 28)
SHIRT    = (196, 64, 60);   SHIRT_SH = (140, 40, 40)
PANTS    = (64, 84, 180);   PANTS_SH = (40, 56, 128)
BOOTS    = (72, 48, 32)
EYE_W    = (240, 240, 248); EYE_P    = (40, 60, 148)

def snap(c):
    return ((c[0] >> 3) << 3, (c[1] >> 2) << 2, (c[2] >> 3) << 3)

# ------------------------------------------------------------- sheet1 comps --
def sheet1_components():
    im = Image.open(os.path.join(HERE, "sources_sheet1.png")).convert("RGB")
    a = np.array(im).astype(int)
    lum = a.mean(axis=2)
    spread = a.max(axis=2) - a.min(axis=2)
    bg = (spread < 16) & (lum > 170)
    lab, _ = ndimage.label(~bg, structure=np.ones((3, 3)))
    sl = ndimage.find_objects(lab)
    boxes = []
    for i, s in enumerate(sl):
        if s is None: continue
        h = s[0].stop - s[0].start; w = s[1].stop - s[1].start
        if w * h < 900 or w < 16 or h < 12: continue
        sub = lab[s] == (i + 1)
        cols = a[s][sub]
        dark = ((cols.max(axis=1) - cols.min(axis=1) < 40) & (cols.mean(axis=1) < 70)).mean()
        if dark > 0.85: continue
        boxes.append((i + 1, s[1].start, s[0].start, w, h))
    boxes.sort(key=lambda b: (b[2] // 110, b[1]))
    return im, lab, boxes

def comp_rgba(im, lab, boxes, idx, pad=0):
    """Extract component #idx (curated index) as RGBA with true transparency,
    holes filled so enclosed whites survive."""
    i, x, y, w, h = boxes[idx]
    mask = (lab[y:y + h, x:x + w] == i)
    mask = ndimage.binary_fill_holes(mask)
    crop = np.array(im.crop((x, y, x + w, y + h)).convert("RGB"))
    out = np.zeros((h, w, 4), np.uint8)
    out[..., :3] = crop
    out[..., 3] = np.where(mask, 255, 0)
    return Image.fromarray(out)

def scale_to(img, tw, th, anchor="bottom"):
    """NEAREST-downscale into a (tw,th) RGBA cell, preserving aspect."""
    w, h = img.size
    sc = min(tw / w, th / h)
    nw, nh = max(1, round(w * sc)), max(1, round(h * sc))
    img = img.resize((nw, nh), Image.NEAREST)
    cell = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
    ox = (tw - nw) // 2
    oy = th - nh if anchor == "bottom" else (th - nh) // 2
    cell.paste(img, (ox, oy), img)
    return cell

def quant_rgba(img, targets):
    """Snap every opaque pixel to the nearest of `targets` (RGB tuples)."""
    a = np.array(img)
    px = a[..., :3].astype(int)
    al = a[..., 3]
    t = np.array(targets)
    d = ((px[..., None, :] - t[None, None, :, :]) ** 2).sum(axis=-1)
    best = d.argmin(axis=-1)
    q = t[best].astype(np.uint8)
    out = np.zeros_like(a)
    out[..., :3] = q
    out[..., 3] = al
    # crush near-black to outline
    dark = (px.sum(axis=-1) < 150) & (al > 0)
    out[dark, 0], out[dark, 1], out[dark, 2] = OUTLINE
    return Image.fromarray(out)

def rgb565ify(img):
    a = np.array(img)
    a[..., 0] &= 0xF8; a[..., 1] &= 0xFC; a[..., 2] &= 0xF8
    return Image.fromarray(a)

def tint(img, col):
    """Recolour by luminance ramp toward `col`; outlines stay dark."""
    a = np.array(img).astype(int)
    al = a[..., 3]
    lum = a[..., :3].mean(axis=-1) / 255.0
    out = a.copy()
    for k in range(3):
        out[..., k] = np.clip(col[k] * (lum * 1.35), 0, 255)
    keep_dark = lum < 0.22
    out[keep_dark, :3] = a[keep_dark, :3]
    out[..., 3] = al
    return Image.fromarray(out.astype(np.uint8))

def sheet_of(cells, cw, ch):
    img = Image.new("RGBA", (cw * len(cells), ch), (0, 0, 0, 0))
    for i, c in enumerate(cells):
        img.paste(c, (i * cw, 0), c)
    return img

# ---------------------------------------------------------------- player -----
PLAYER_TARGETS = [SKIN, SKIN_SH, SHIRT, SHIRT_SH, PANTS, PANTS_SH, BOOTS, OUTLINE, EYE_W, EYE_P]

def make_player(im, lab, boxes):
    # curated component ids (see s1_contact.png): walk 0,2,3,5,6 · swing 13,8 · hurt 18
    frames = [0, 2, 3, 5, 6, 3, 13, 8, 18]   # idle,w1..w4,jump,windup,strike,hurt
    BOUNCE = (2, 4)                          # mid-stride cells get a 1px hop
    cells = []
    for idx, k in enumerate(frames):
        c = comp_rgba(im, lab, boxes, k)
        c = scale_to(c, 12, 16, anchor="bottom")
        c = quant_rgba(c, PLAYER_TARGETS)
        if idx in BOUNCE:                    # synthetic walk bounce (source is rigid)
            up = Image.new("RGBA", (12, 16), (0, 0, 0, 0))
            up.paste(c.crop((0, 1, 12, 16)), (0, 0), c.crop((0, 1, 12, 16)))
            c = up
        cells.append(c)
    sheet_of(cells, 12, 16).save(os.path.join(HERE, "player.png"))
    # per-frame head centers -> src/player_meta.h so the hair overlay tracks the
    # walk/swing/hurt animation (frame 0 is the reference the hair art is drawn on)
    centers = []
    for c in cells:
        ca = np.array(c)
        sk = ((np.abs(ca[..., 0].astype(int) - SKIN[0]) < 40) &
              (np.abs(ca[..., 1].astype(int) - SKIN[1]) < 40) & (ca[..., 3] > 0))
        sk[7:, :] = False                    # head only — arms/hands sit lower
        ys, xs = np.where(sk)
        if len(xs):
            centers.append(((xs.min() + xs.max()) / 2.0, (ys.min() + ys.max()) / 2.0))
        else:
            centers.append(centers[0] if centers else (5.5, 3.0))
    ref = centers[0]
    gd0 = os.path.dirname(HERE)
    with open(os.path.join(gd0, "src", "player_meta.h"), "w") as f:
        f.write("/* GENERATED by extract_sheets.py — per-frame head offsets for the hair overlay. */\n")
        f.write("#ifndef PLAYER_META_H\n#define PLAYER_META_H\n")
        f.write("#define PLAYER_FRAMES %d\n" % len(cells))
        f.write("static const int8_t player_head_dx[PLAYER_FRAMES] = { %s };\n"
                % ", ".join(str(int(round(cx - ref[0]))) for cx, cy in centers))
        f.write("static const int8_t player_head_dy[PLAYER_FRAMES] = { %s };\n"
                % ", ".join(str(int(round(cy - ref[1]))) for cx, cy in centers))
        f.write("#endif\n")
    print("[extract] player_meta.h head offsets:",
          [(int(round(cx - ref[0])), int(round(cy - ref[1]))) for cx, cy in centers])
    # measure the head (skin pixels in the top half of cell 0) for hair anchoring
    ca = np.array(cells[0])
    skin = ((np.abs(ca[..., 0].astype(int) - SKIN[0]) < 30) &
            (np.abs(ca[..., 1].astype(int) - SKIN[1]) < 30) &
            (ca[..., 3] > 0))
    skin[9:, :] = False
    ys, xs = np.where(skin)
    global HEAD_CX, HEAD_CY
    if len(xs):
        HEAD_CX = (xs.min() + xs.max()) / 2.0
        HEAD_CY = (ys.min() + ys.max()) / 2.0
    print("[extract] head center", HEAD_CX, HEAD_CY)
    gd = os.path.dirname(HERE)
    os.makedirs(os.path.join(gd, "anims"), exist_ok=True)
    with open(os.path.join(gd, "anims", "player.anims"), "w") as f:
        f.write("sheet assets/player.png\ntile 12 16\nclips 5\n")
        f.write("clip idle 1 2 0 0 1\nf 0 500 -\n")
        f.write("clip walk 1 10 0 0 4\n")
        for i in range(1, 5): f.write("f %d 95 -\n" % i)
        f.write("clip jump 1 8 0 0 1\nf 5 200 -\n")
        f.write("clip swing 0 12 0 0 2\nf 6 60 -\nf 7 110 -\n")
        f.write("clip hurt 0 8 0 0 1\nf 8 220 -\n")
    print("[extract] player.png (9 cells) + player.anims")

# ---------------------------------------------------------------- enemies ----
def make_enemies(im, lab, boxes):
    def grab(ids, tw, th, targets=None, hflip=False):
        cells = []
        for k in ids:
            c = comp_rgba(im, lab, boxes, k)
            if hflip: c = c.transpose(Image.FLIP_LEFT_RIGHT)
            c = scale_to(c, tw, th, anchor="bottom")
            if targets: c = quant_rgba(c, targets)
            cells.append(rgb565ify(c))
        return cells

    for name, ids in (("slime", (55, 56, 52)), ("slime_blue", (58, 59, 53)),
                      ("slime_lava", (60, 61, 71))):
        sheet_of(grab(ids, 16, 12), 16, 12).save(os.path.join(HERE, name + ".png"))
        open(os.path.join(HERE, name + ".sheet"), "w").write("cell 16 12\n")
    # skeleton walk (art faces right)
    sheet_of(grab((40, 41, 42, 43), 12, 16), 12, 16).save(os.path.join(HERE, "skeleton.png"))
    open(os.path.join(HERE, "skeleton.sheet"), "w").write("cell 12 16\n")
    # (demon eye + bat stay procedural — the sheet versions don't survive 16px)
    # Eye of Cthulhu: phase1 30,31 · phase2 33,34 (art faces left)
    sheet_of(grab((30, 31, 33, 34), 40, 40), 40, 40).save(os.path.join(HERE, "eoc.png"))
    open(os.path.join(HERE, "eoc.sheet"), "w").write("cell 40 40\n")
    print("[extract] slimes / skeleton / eye / bat / eoc")

# ------------------------------------------------------------------ items ----
CS = 16   # items.png cell size (make_sprites.py must agree)

# sheet2 weapon grid cells: (x0,y0,x1,y1) in source px (label strip excluded)
S2 = {
    "SWORD_COPPER":  (40, 195, 160, 318),
    "SWORD_IRON":    (165, 195, 285, 318),
    "SWORD_BANE":    (290, 195, 408, 318),    # muramasa
    "SWORD_VOLCANO": (412, 195, 530, 318),    # fiery greatsword
    "SWORD_GOLD":    (535, 195, 652, 318),    # excalibur
    "BOW_WOOD":      (778, 195, 895, 318),
    "BOW_MOLTEN":    (1022, 195, 1140, 318),  # hellwing bow
    "SWORD_WOOD":    (40, 448, 160, 570),
    "AXE_IRON":      (2562, 192, 2678, 314),  # the axe
}

def grid_sprite(im2, box):
    """Whole weapon from a dark-panel grid cell: everything that differs from
    the panel colour (union of components, labels/frame trimmed), holes filled."""
    a0 = np.array(im2.crop(box)).astype(int)
    a = a0[13:-13, 13:-13]                        # inside the cell frame
    h, w = a.shape[:2]
    corners = np.concatenate([a[2:12, 2:12].reshape(-1, 3), a[2:12, -12:-2].reshape(-1, 3)])
    bg = np.median(corners, axis=0)               # the panel colour
    dist = np.abs(a - bg[None, None, :]).sum(axis=2)
    fg = dist > 55
    lab, n = ndimage.label(fg, structure=np.ones((3, 3)))
    keep = np.zeros_like(fg)
    for i in range(1, n + 1):
        m = lab == i
        if m.sum() < 30: continue
        ys, xs = np.where(m)
        if ys.min() > h - 14: continue            # label text at the cell bottom
        touches = (xs.min() <= 1) + (xs.max() >= w - 2) + (ys.min() <= 1) + (ys.max() >= h - 2)
        if touches >= 2: continue                 # frame ring remnants hug the edges
        keep |= m
    keep = ndimage.binary_fill_holes(keep)
    ys, xs = np.where(keep)
    if len(xs) == 0: raise SystemExit("empty weapon cell %s" % (box,))
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    return a[y0:y1, x0:x1], keep[y0:y1, x0:x1]

def est_pitch(a, mask):
    """The AI sheets are fake-pixel art: recover the art-pixel pitch+phase from
    colour-boundary alignment (smallest pitch within 95% of the best score, so
    a 2x harmonic never wins)."""
    d = np.abs(np.diff(a, axis=1)).sum(axis=2).astype(float)
    prof = d.sum(axis=0)
    cands = []
    w = a.shape[1]
    for p10 in range(30, 90):
        p = p10 / 10.0
        bestph = (0.0, 0.0)
        for ph10 in range(0, p10, 2):
            ph = ph10 / 10.0
            sc = 0.0; cnt = 0
            x = ph
            while x < w - 1:
                sc += prof[int(x)]; cnt += 1; x += p
            if cnt >= 3:
                sc /= cnt
                if sc > bestph[0]: bestph = (sc, ph)
        cands.append((bestph[0] / (prof.mean() + 1e-6), p, bestph[1]))
    top = max(c[0] for c in cands)
    for sc, p, ph in cands:                        # smallest acceptable pitch
        if sc >= top * 0.95:
            return p, ph
    return 5.0, 0.0

def best_phase(a, p):
    """Phase for a KNOWN pitch (both axes share it on these sheets)."""
    d = np.abs(np.diff(a, axis=1)).sum(axis=2).astype(float)
    prof = d.sum(axis=0)
    w = a.shape[1]
    best = (0.0, 0.0)
    for ph10 in range(0, int(p * 10), 2):
        ph = ph10 / 10.0
        sc = 0.0; cnt = 0
        x = ph
        while x < w - 1:
            sc += prof[int(x)]; cnt += 1; x += p
        if cnt >= 2 and sc / cnt > best[0]: best = (sc / cnt, ph)
    return best[1]

def depixelate(a, mask, pitch=None):
    """(h,w,3) int + mask -> native-resolution RGBA pixel art. Pass the sheet's
    global pitch when known (per-sprite estimation drifts on small sprites)."""
    if pitch is None:
        p, ph = est_pitch(a, mask)
    else:
        p, ph = pitch, best_phase(a, p if False else pitch)
    h, w = a.shape[:2]
    aw = max(1, int(round((w - ph) / p))); ah = max(1, int(round((h - ph) / p)))
    out = np.zeros((ah, aw, 4), np.uint8)
    for j in range(ah):
        for i in range(aw):
            x0 = int(ph + i * p); x1 = int(ph + (i + 1) * p)
            y0 = int(ph + j * p); y1 = int(ph + (j + 1) * p)
            x1 = min(x1, w); y1 = min(y1, h)
            if x1 <= x0: x1 = min(x0 + 1, w)
            if y1 <= y0: y1 = min(y0 + 1, h)
            blk = a[y0:y1, x0:x1].reshape(-1, 3)
            bm = mask[y0:y1, x0:x1].reshape(-1)
            if bm.mean() < 0.4: continue
            cols = blk[bm]
            med = np.median(cols, axis=0)
            out[j, i, :3] = med.astype(np.uint8)
            out[j, i, 3] = 255
    return Image.fromarray(out)

def fit_cell(art, size):
    """Native art -> size x size cell. Kept 1:1 when it fits, else NEAREST from
    the TRUE art pixels (clean, unlike sampling the fake-pixel original)."""
    w, h = art.size
    if w > size or h > size:
        sc = size / max(w, h)
        art = art.resize((max(1, round(w * sc)), max(1, round(h * sc))), Image.NEAREST)
    cell = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    cell.paste(art, ((size - art.width) // 2, (size - art.height) // 2), art)
    return cell

def make_items(im, lab, boxes):
    items_path = os.path.join(HERE, "items.png")
    sheet = Image.open(items_path).convert("RGBA")
    ids = ITEM_IDS
    big = {}                       # name -> native art (for weapons_big.png)
    def put(name, cell):
        i = ids.index(name)
        cell = rgb565ify(cell)
        ox, oy = (i % 8) * CS, (i // 8) * CS
        sheet.paste(Image.new("RGBA", (CS, CS), (0, 0, 0, 0)), (ox, oy))
        sheet.paste(cell, (ox, oy), cell)

    im2 = Image.open(os.path.join(HERE, "sources_sheet2_weapons.png")).convert("RGB")
    sprites = {name: grid_sprite(im2, box) for name, box in S2.items()}
    pits = []
    for name, (a, m) in sprites.items():
        if m.sum() > 2500:
            pits.append(est_pitch(a, m)[0])
    p2 = float(np.median(pits)) if pits else 4.3
    if p2 > 6: p2 /= 2               # oversample: keeps the AI art's sub-grid detail
    print("[extract] sheet2 pitch", p2)
    arts = {}
    for name, (a, m) in sprites.items():
        arts[name] = depixelate(a, m, pitch=p2)
    arts["SWORD_GOLD"] = tint(arts["SWORD_GOLD"], (242, 200, 60))
    arts["BOW_GOLD"]   = tint(arts["BOW_WOOD"], (242, 200, 60))
    arts["AXE_WOOD"]   = tint(arts["AXE_IRON"], (168, 120, 62))

    # sheet1 picks: depixelate the gold pick, tint per metal
    def s1_raw(k):
        i, x, y, w, h = boxes[k]
        m = ndimage.binary_fill_holes(lab[y:y + h, x:x + w] == i)
        a = np.array(im.crop((x, y, x + w, y + h)).convert("RGB")).astype(int)
        return a, m
    a1, m1 = s1_raw(124)                        # the chest: big + regular
    p1 = est_pitch(a1, m1)[0]
    if p1 > 6: p1 /= 2
    print("[extract] sheet1 pitch", p1)
    def s1_art(k):
        a, m = s1_raw(k)
        return depixelate(a, m, pitch=p1)
    pick = s1_art(128)
    arts["PICK_GOLD"]      = pick
    arts["PICK_WOOD"]      = tint(pick, (166, 120, 66))
    arts["PICK_COPPER"]    = tint(pick, (208, 118, 50))
    arts["PICK_IRON"]      = tint(pick, (182, 182, 192))
    arts["PICK_NIGHTMARE"] = tint(pick, (140, 80, 215))

    for name, art in arts.items():
        put(name, fit_cell(art, CS))
        big[name] = art

    # sheet1 furniture / misc (contiguous sprites): depixelated 16px icons
    for name, k in (("CHEST", 124), ("TORCH", 126), ("PLATFORM", 127), ("DOOR", 137),
                    ("ANVIL", 139), ("WORKBENCH", 140), ("COIN", 141),
                    ("POTION_HEAL", 150), ("SUSPICIOUS_EYE", 151)):
        put(name, fit_cell(s1_art(k), CS))

    mail = s1_art(148)
    for name, col in (("MAIL_COPPER", (208, 118, 50)), ("MAIL_IRON", None),
                      ("MAIL_GOLD", (240, 200, 60)), ("MAIL_MOLTEN", (240, 100, 34))):
        put(name, fit_cell(mail if col is None else tint(mail, col), CS))
    greaves = s1_art(149)
    for name, col in (("LEGS_COPPER", (208, 118, 50)), ("LEGS_IRON", (182, 182, 192)),
                      ("LEGS_GOLD", None), ("LEGS_MOLTEN", (240, 100, 34))):
        put(name, fit_cell(greaves if col is None else tint(greaves, col), CS))
    lc = s1_art(123)
    put("LIFE_CRYSTAL", fit_cell(tint(lc, (235, 70, 100)), CS))
    put("DEMONITE_ORE", fit_cell(lc, CS))

    sheet.save(items_path)

    # in-hand sheet at native art resolution (order must match player.c's table)
    order = ["SWORD_WOOD", "SWORD_COPPER", "SWORD_IRON", "SWORD_GOLD", "SWORD_BANE",
             "SWORD_VOLCANO", "BOW_WOOD", "BOW_GOLD", "BOW_MOLTEN",
             "AXE_WOOD", "AXE_IRON",
             "PICK_WOOD", "PICK_COPPER", "PICK_IRON", "PICK_GOLD", "PICK_NIGHTMARE"]
    BC = 32
    wb = Image.new("RGBA", (BC * len(order), BC * 2), (0, 0, 0, 0))
    for i, name in enumerate(order):
        cell = fit_cell(big[name], BC)
        wb.paste(cell, (i * BC, 0), cell)
        flip = cell.transpose(Image.FLIP_LEFT_RIGHT)     # row 1: left-facing
        wb.paste(flip, (i * BC, BC), flip)
    rgb565ify(wb).save(os.path.join(HERE, "weapons_big.png"))
    open(os.path.join(HERE, "weapons_big.sheet"), "w").write("cell %d %d\n" % (BC, BC))
    print("[extract] items.png icons + weapons_big.png (%d in-hand sprites)" % len(order))

ITEM_IDS = """NONE DIRT STONE WOOD SAND SNOW EBON CLAY ASH HELLSTONE OBSIDIAN TORCH
PLATFORM WORKBENCH FURNACE ANVIL CHEST DOOR ACORN GEL LENS MUSHROOM COIN
COPPER_ORE IRON_ORE GOLD_ORE DEMONITE_ORE COPPER_BAR IRON_BAR GOLD_BAR
DEMONITE_BAR HELL_BAR PICK_WOOD PICK_COPPER PICK_IRON PICK_GOLD PICK_NIGHTMARE
AXE_WOOD AXE_IRON SWORD_WOOD SWORD_COPPER SWORD_IRON SWORD_GOLD SWORD_BANE
SWORD_VOLCANO BOW_WOOD BOW_GOLD BOW_MOLTEN ARROW ARROW_FLAME HELM_COPPER
MAIL_COPPER LEGS_COPPER HELM_IRON MAIL_IRON LEGS_IRON HELM_GOLD MAIL_GOLD
LEGS_GOLD HELM_MOLTEN MAIL_MOLTEN LEGS_MOLTEN POTION_HEAL SUSPICIOUS_EYE LIFE_CRYSTAL""".split()

# ------------------------------------------------------------------- hair ----
def make_hair():
    """Hand-drawn hairstyles for the 12x16 player (head x3..7, y0..5, faces
    right; keep the face x5..8, y2..5 clear). Cells 12x10, palette slots
    HAIR / HAIR_SH / OUTLINE so the builder recolours them."""
    H, S, O = HAIR, HAIR_SH, OUTLINE
    W_, H_ = 12, 10

    def blank(): return [[None] * W_ for _ in range(H_)]
    def put(g, x, y, c):
        if 0 <= x < W_ and 0 <= y < H_: g[y][x] = c
    def row(g, x0, x1, y, c):
        for x in range(x0, x1 + 1): put(g, x, y, c)
    def col(g, x, y0, y1, c):
        for y in range(y0, y1 + 1): put(g, x, y, c)

    def cap(g):
        row(g, 2, 8, 0, H)
        row(g, 2, 4, 1, H); put(g, 5, 1, S)      # hairline over the brow, face clear
        put(g, 8, 1, S)
        col(g, 2, 1, 2, H)

    styles = []
    g = blank(); cap(g); put(g, 2, 3, S)
    styles.append(g)                                        # 0 short crop
    g = blank(); cap(g)
    for x in (3, 5, 7): put(g, x, 0, H)
    put(g, 4, 0, S); put(g, 6, 0, S)
    styles.append(g)                                        # 1 spiky (ragged top)
    g = blank(); cap(g)
    col(g, 1, 1, 7, H); col(g, 2, 2, 6, S); put(g, 1, 8, S)
    styles.append(g)                                        # 2 long
    g = blank(); cap(g)
    col(g, 1, 2, 4, H); put(g, 0, 4, H); put(g, 0, 5, S); put(g, 0, 6, S)
    styles.append(g)                                        # 3 ponytail
    g = blank(); cap(g)
    col(g, 1, 1, 4, H); put(g, 2, 3, S)
    col(g, 9, 1, 3, H); put(g, 9, 4, S)
    styles.append(g)                                        # 4 bob
    g = blank(); row(g, 4, 6, 0, H); row(g, 4, 6, 1, S)
    styles.append(g)                                        # 5 mohawk
    g = blank(); cap(g)
    for x in (2, 4, 6, 8): put(g, x, 0, S)
    put(g, 1, 1, H); put(g, 9, 1, H)
    styles.append(g)                                        # 6 curly
    g = blank(); row(g, 3, 7, 0, S)
    styles.append(g)                                        # 7 buzz
    g = blank(); cap(g)
    row(g, 5, 8, 1, H); put(g, 8, 2, S)
    styles.append(g)                                        # 8 side-swept fringe
    g = blank(); cap(g)
    col(g, 0, 1, 5, H); put(g, 1, 2, S)
    col(g, 10, 1, 5, H); put(g, 9, 2, S)
    put(g, 0, 6, S); put(g, 10, 6, S)
    styles.append(g)                                        # 9 twin tails
    g = blank(); cap(g)
    put(g, 1, 0, H); put(g, 3, 0, H); put(g, 6, 0, S); put(g, 8, 0, H)
    put(g, 9, 1, H); put(g, 2, 2, H)
    styles.append(g)                                        # 10 messy
    g = blank(); row(g, 2, 8, 0, H); row(g, 2, 7, 1, S)
    col(g, 1, 1, 4, H); put(g, 2, 2, S)
    styles.append(g)                                        # 11 slick back
    styles.append(blank())                                  # 12 bald
    n = len(styles)
    img = Image.new("RGBA", (12 * n, H_), (0, 0, 0, 0))
    for i, g in enumerate(styles):
        for y in range(H_):
            for x in range(W_):
                if g[y][x] is not None:
                    img.putpixel((i * 12 + x, y), g[y][x] + (255,))
        for y in range(H_):
            for x in range(W_):
                if g[y][x] is not None: continue
                near = False
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < W_ and 0 <= yy < H_ and g[yy][xx] in (H, S):
                        near = True; break
                if near and y < 8:
                    img.putpixel((i * 12 + x, y), O + (255,))
    img.save(os.path.join(HERE, "hair.png"))
    open(os.path.join(HERE, "hair.sheet"), "w").write("cell 12 10\n")
    print("[extract] hair.png (%d drawn styles)" % n)
    return n

if __name__ == "__main__":
    im, lab, boxes = sheet1_components()
    make_player(im, lab, boxes)
    make_enemies(im, lab, boxes)
    make_items(im, lab, boxes)
    n = make_hair()
    print("done — hair styles:", n)
