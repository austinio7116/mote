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
    cells = []
    for k in frames:
        c = comp_rgba(im, lab, boxes, k)
        c = scale_to(c, 12, 16, anchor="bottom")
        c = quant_rgba(c, PLAYER_TARGETS)
        cells.append(c)
    sheet_of(cells, 12, 16).save(os.path.join(HERE, "player.png"))
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

# sheet2 weapon grid cells: (x0,y0,x1,y1) in source px
S2 = {
    "SWORD_COPPER":  (40, 200, 160, 330),
    "SWORD_IRON":    (165, 200, 285, 330),
    "SWORD_BANE":    (290, 200, 408, 330),    # muramasa
    "SWORD_VOLCANO": (412, 200, 530, 330),    # fiery greatsword
    "SWORD_GOLD":    (535, 200, 652, 330),    # excalibur
    "BOW_WOOD":      (778, 200, 895, 330),
    "BOW_MOLTEN":    (1022, 200, 1140, 330),  # hellwing bow
    "SWORD_WOOD":    (40, 452, 160, 585),
    "AXE_IRON":      (2578, 195, 2695, 320),  # the axe
}

def cell_from_dark_grid(im2, box):
    a = np.array(im2.crop(box)).astype(int)
    lum = a.mean(axis=2)
    spread = a.max(axis=2) - a.min(axis=2)
    fg = (lum > 72) & ~((spread < 25) & (lum < 135))
    # largest component = the weapon (labels are small)
    lab, n = ndimage.label(fg, structure=np.ones((3, 3)))
    if n == 0: raise SystemExit("empty weapon cell %s" % (box,))
    sizes = ndimage.sum(fg, lab, range(1, n + 1))
    big = 1 + int(np.argmax(sizes))
    mask = ndimage.binary_fill_holes(lab == big)
    ys, xs = np.where(mask)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    out = np.zeros((y1 - y0, x1 - x0, 4), np.uint8)
    out[..., :3] = a[y0:y1, x0:x1].astype(np.uint8)
    out[..., 3] = np.where(mask[y0:y1, x0:x1], 255, 0)
    return Image.fromarray(out)

def make_items(im, lab, boxes):
    items_path = os.path.join(HERE, "items.png")
    sheet = Image.open(items_path).convert("RGBA")
    ids = ITEM_IDS
    def put(name, cell):
        i = ids.index(name)
        cell = rgb565ify(cell)
        ox, oy = (i % 8) * CS, (i // 8) * CS
        sheet.paste(Image.new("RGBA", (CS, CS), (0, 0, 0, 0)), (ox, oy))
        sheet.paste(cell, (ox, oy), cell)

    im2 = Image.open(os.path.join(HERE, "sources_sheet2_weapons.png")).convert("RGB")
    for name, box in S2.items():
        c = scale_to(cell_from_dark_grid(im2, box), CS, CS, anchor="center")
        if name == "SWORD_GOLD": c = tint(c, (242, 200, 60))       # excalibur -> gold
        put(name, c)
    # gold bow: the wooden bow, gilded
    put("BOW_GOLD", tint(scale_to(cell_from_dark_grid(im2, S2["BOW_WOOD"]), CS, CS, "center"), (242, 200, 60)))
    # wood axe = the axe, browned
    put("AXE_WOOD", tint(scale_to(cell_from_dark_grid(im2, S2["AXE_IRON"]), CS, CS, "center"), (168, 120, 62)))

    # sheet1 picks: comp 128 (gold pick) tinted per metal
    pick = comp_rgba(im, lab, boxes, 128)
    for name, col in (("PICK_WOOD", (166, 120, 66)), ("PICK_COPPER", (208, 118, 50)),
                      ("PICK_IRON", (182, 182, 192)), ("PICK_GOLD", None),
                      ("PICK_NIGHTMARE", (140, 80, 215))):
        c = scale_to(pick, CS, CS, "center")
        put(name, c if col is None else tint(c, col))

    # sheet1 furniture / misc, straight extracts
    for name, k in (("CHEST", 124), ("TORCH", 126), ("PLATFORM", 127), ("DOOR", 137),
                    ("ANVIL", 139), ("WORKBENCH", 140), ("COIN", 141),
                    ("POTION_HEAL", 150), ("SUSPICIOUS_EYE", 151)):
        put(name, scale_to(comp_rgba(im, lab, boxes, k), CS, CS, "center"))

    # armor: mail 148 (iron) tinted per metal; greaves 149 (gold) tinted
    mail = comp_rgba(im, lab, boxes, 148)
    for name, col in (("MAIL_COPPER", (208, 118, 50)), ("MAIL_IRON", None),
                      ("MAIL_GOLD", (240, 200, 60)), ("MAIL_MOLTEN", (240, 100, 34))):
        c = scale_to(mail, CS, CS, "center")
        put(name, c if col is None else tint(c, col))
    greaves = comp_rgba(im, lab, boxes, 149)
    for name, col in (("LEGS_COPPER", (208, 118, 50)), ("LEGS_IRON", (182, 182, 192)),
                      ("LEGS_GOLD", None), ("LEGS_MOLTEN", (240, 100, 34))):
        c = scale_to(greaves, CS, CS, "center")
        put(name, c if col is None else tint(c, col))
    # life crystal: purple crystal 123 shifted red
    lc = comp_rgba(im, lab, boxes, 123)
    put("LIFE_CRYSTAL", tint(scale_to(lc, CS, CS, "center"), (235, 70, 100)))
    # demonite ore icon: the purple crystal as-is
    put("DEMONITE_ORE", scale_to(lc, CS, CS, "center"))

    sheet.save(items_path)
    print("[extract] items.png patched (%d curated cells)" % (len(S2) + 24))

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
