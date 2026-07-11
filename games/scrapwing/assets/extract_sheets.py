#!/usr/bin/env python3
"""SCRAPWING sprite extractor — sheets in ../sources/ are the EDITABLE assets.

Pulls sprites out of the four AI-generated sheets (dark navy bg, irregular grid):
  sources/ships_src.png    tiny ships   -> ships.png    (16x16 cells, oriented to face RIGHT)
  sources/weapons_src.png  weapon icons -> weapons.png  (16x16 cells, kept upright)
  sources/boss1_src.png    boss ships   -> bosses.png   (variable-size shelf pack — bosses
  sources/boss2_src.png    boss ships   ->   "           run up to ~100 art px, NOT 32x32)
plus src/ships_meta.h (counts, per-cell opaque bboxes, per-boss frame rects) and
../icon.png built from the player ship. The small ships on the weapon/boss sheets
duplicate ships_src, so those sheets contribute ONLY curated weapon icons / large
bosses. Numbered contact sheets land in CONTACT_DIR for re-curation.

Pipeline per sheet: seed mask on bright/saturated pixels -> binary_propagation
into mid-tones -> fill holes (keeps dark cockpits) -> dilate 3px (keeps the
near-black pixel-art outline) -> label with 4px gap bridging (reattaches
thruster flames) -> size/band filters -> grid sort -> NEAREST downsample by the
global art-pixel scale -> orientation fix (up-facing art rotates 90 CW).
"""
import os
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
SRC = os.path.join(GAME, "sources")   # NOT under assets/ — mote bake must not see them
CONTACT_DIR = os.environ.get("CONTACT_DIR", "/tmp")

# ---- curation (indices refer to the numbered contact sheets) ----------------
PLAYER_IDX = 250            # ships contact index -> the player ship (side-view fighter)

# weapons_src curation (developer-reviewed against contact_weapons.png):
# excluded ranges are ships/creatures (dupes of ships_src) — except the two
# mine ranges, which go to their own mines.png sheet instead.
WEAPON_EXCLUDE = [(17, 29), (46, 58), (72, 78), (106, 115), (132, 145), (162, 175),
                  (183, 205), (213, 235), (240, 325), (329, 356), (360, 382),
                  (387, 10**9)]
WEAPON_FORCE_KEEP = {267, 268}
MINE_RANGES = [(17, 29), (46, 58)]

def _in_ranges(i, ranges):
    return any(a <= i <= b for a, b in ranges)
# bosses (contact_bosses indices): merged fragments / orb clusters
BOSS_DROP = {10, 40, 52, 73, 80, 81, 83, 85, 86}
BOSS_MIN_DIM = 130          # smaller sprites on the boss sheets are duplicate ships

# hand-confirmed orientation fixes, reviewed against the numbered review grid
# (/tmp/scrapwing_ships_review.png; number = index + 1). Applied AFTER the
# automatic orient_right pass, BEFORE the drop (so review numbers stay valid).
SHIP_ROT_CW90 = {3, 32, 60, 61, 77, 91, 121, 122, 124, 179, 180, 181, 182, 239,
                 270, 271, 272, 299, 300, 302, 320, 324, 331, 334, 348, 360,
                 388, 390, 397}
SHIP_ROT_180 = {21, 132, 290, 291}
SHIP_DROP = {386}           # not actually a ship
# boss fixes (reviewed against /tmp/scrapwing_bosses_review.png; number = index + 1)
BOSS_ROT_180 = {6, 10, 16, 35, 36, 45, 50, 52, 57, 76, 77}

GRID_PITCH = 91.9           # full-res cell pitch (2816px / ~30.6 cells)


def extract(path, y_band=(95, 1445), min_size=110):
    im = np.asarray(Image.open(path).convert("RGB")).astype(int)
    lum = im.sum(axis=2)
    sat = im.max(axis=2) - im.min(axis=2)
    seed = (lum > 500) | (sat > 70)
    grow = ndimage.binary_propagation(seed, mask=(lum > 150) | (sat > 40))
    mask = ndimage.binary_fill_holes(grow)
    mask = ndimage.binary_dilation(mask, iterations=3)      # keep dark outline ring
    mask = ndimage.binary_fill_holes(mask)
    bridged = ndimage.binary_dilation(mask, iterations=4)   # bridge flame gaps <=8px
    labels, _ = ndimage.label(bridged, structure=np.ones((3, 3)))
    out = []
    for sl in ndimage.find_objects(labels):
        cy, cx = (sl[0].start + sl[0].stop) / 2, (sl[1].start + sl[1].stop) / 2
        if not (y_band[0] < cy < y_band[1]):
            continue
        m = mask[sl] & (labels[sl] > 0)
        if m.sum() < min_size:
            continue
        ys, xs = np.where(m)
        yy0, yy1, xx0, xx1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
        rgb = im[sl][yy0:yy1, xx0:xx1]
        a = (m[yy0:yy1, xx0:xx1] * 255).astype(np.uint8)
        out.append((cy, cx, Image.fromarray(np.dstack([rgb.astype(np.uint8), a]), "RGBA")))
    out.sort(key=lambda t: (round(t[0] / GRID_PITCH), t[1]))
    return [t[2] for t in out]


def orient_right(spr):
    """Rotate up-facing art 90 CW so it faces RIGHT; leave side-view art alone."""
    w, h = spr.size
    if h >= w * 1.15:
        return spr.transpose(Image.ROTATE_270)
    if w >= h * 1.15:
        return spr
    a = np.asarray(spr).astype(int)          # near-square: use mirror symmetry
    sv = np.abs(a - a[:, ::-1]).mean()       # symmetric about vertical axis = up-facing
    sh = np.abs(a - a[::-1, :]).mean()
    return spr.transpose(Image.ROTATE_270) if sv < sh * 0.95 else spr


def shrink_by(spr, s, cap=None):
    w, h = spr.size
    nw, nh = max(1, round(w / s)), max(1, round(h / s))
    if cap:
        f = min(1.0, cap / max(nw, nh))
        nw, nh = max(1, round(nw * f)), max(1, round(nh * f))
    return spr.resize((nw, nh), Image.NEAREST)


def grid_sheet(sprites, cell, cols):
    rows = (len(sprites) + cols - 1) // cols
    img = Image.new("RGBA", (cols * cell, rows * cell), (0, 0, 0, 0))
    meta = []
    for i, t in enumerate(sprites):
        ox = (i % cols) * cell + (cell - t.width) // 2
        oy = (i // cols) * cell + (cell - t.height) // 2
        img.alpha_composite(t, (ox, oy))
        meta.append((ox % cell, oy % cell, t.width, t.height))
    return img, meta


def shelf_pack(sprites, width=256, pad=1):
    """Variable-size packer: returns (img, [(fx,fy,fw,fh)])."""
    x, y, shelf_h = pad, pad, 0
    pos = []
    for t in sprites:
        if x + t.width + pad > width:
            x, y = pad, y + shelf_h + pad
            shelf_h = 0
        pos.append((x, y))
        x += t.width + pad
        shelf_h = max(shelf_h, t.height)
    H = y + shelf_h + pad
    img = Image.new("RGBA", (width, H), (0, 0, 0, 0))
    meta = []
    for t, (px, py) in zip(sprites, pos):
        img.alpha_composite(t, (px, py))
        meta.append((px, py, t.width, t.height))
    return img, meta


def quantize_keep_alpha(img, colors=255):
    """Palette-quantize RGB so the baked header is 8bpp indexed (half flash)."""
    alpha = img.getchannel("A")
    rgb = img.convert("RGB").quantize(colors=colors, method=Image.MEDIANCUT,
                                      dither=Image.NONE).convert("RGB")
    out = rgb.convert("RGBA")
    out.putalpha(alpha)
    return out


def contact(sprites, path, cell=48, scale_s=None):
    cols = 20
    rows = (len(sprites) + cols - 1) // cols
    img = Image.new("RGB", (cols * cell, rows * cell), (28, 28, 40))
    d = ImageDraw.Draw(img)
    for i, s in enumerate(sprites):
        f = min(1.0, (cell - 14) / max(s.size))
        t = s.resize((max(1, round(s.width * f)), max(1, round(s.height * f))), Image.NEAREST)
        ox, oy = (i % cols) * cell, (i // cols) * cell
        img.paste(t.convert("RGB"), (ox + (cell - t.width) // 2, oy + 10), t)
        d.text((ox + 2, oy), str(i), fill=(255, 220, 90))
        d.rectangle([ox, oy, ox + cell - 1, oy + cell - 1], outline=(50, 50, 70))
    img.save(path)


# biome ids must match game.c: 0 CAVERN, 1 HIVE, 2 GLACIER, 3 SPOREPIT, 4 EMBERFORGE
def classify_biome(spr):
    """Dominant SATURATED hue of a sprite -> the biome it haunts. Grey hull
    plating is ignored — the accent colours carry the faction identity; ships
    with almost no colour stay in the cavern (the catch-all biome)."""
    import colorsys
    a = np.asarray(spr)
    counts = [0, 0, 0, 0, 0]
    px = a[a[..., 3] >= 128]
    n_opaque = max(1, len(px))
    for r, g, b, _ in px:
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        if s < 0.32 or v < 0.30:
            continue                             # grey/dark plating: no vote
        if h < 0.03 or h > 0.87:
            counts[1] += 1                       # red/pink/magenta -> hive
        elif h < 0.15:
            counts[4] += 1                       # orange/amber -> emberforge
        elif h < 0.45:
            counts[3] += 1                       # green -> sporepit
        elif h < 0.70:
            counts[2] += 1                       # blue/cyan -> glacier
        else:
            counts[0] += 1                       # violet -> cavern
    if sum(counts) < n_opaque * 0.06:
        return 0                                 # barely any colour -> cavern
    return counts.index(max(counts))


def make_icon(player):
    img = Image.new("RGBA", (60, 60), (8, 9, 22, 255))
    rng = np.random.RandomState(11)
    for _ in range(46):
        x, y = rng.randint(60), rng.randint(60)
        v = int(rng.choice([80, 130, 200]))
        img.putpixel((x, y), (v, v, min(255, v + 35), 255))
    for _ in range(220):                       # violet nebula wisp
        x, y = rng.randint(60), rng.randint(60)
        if (x - 47) ** 2 + (y - 12) ** 2 < 210:
            img.putpixel((x, y), (56, 28, 78, 255))
    s = 52 / max(player.size)
    big = player.resize((round(player.width * s), round(player.height * s)), Image.NEAREST)
    img.alpha_composite(big, ((60 - big.width) // 2, (60 - big.height) // 2 + 2))
    img.convert("RGB").save(os.path.join(GAME, "icon.png"))


def main():
    ships_raw = extract(os.path.join(SRC, "ships_src.png"))
    weapons_raw = extract(os.path.join(SRC, "weapons_src.png"))
    boss_raw = [s for src in ("boss1_src.png", "boss2_src.png")
                for s in extract(os.path.join(SRC, src))]

    contact(ships_raw, os.path.join(CONTACT_DIR, "contact_ships.png"))
    contact(weapons_raw, os.path.join(CONTACT_DIR, "contact_weapons.png"))
    big_raw = [s for s in boss_raw if max(s.size) >= BOSS_MIN_DIM]
    contact(big_raw, os.path.join(CONTACT_DIR, "contact_bosses.png"), cell=64)

    # global art-pixel scale from the ships sheet (nominal 16px designs)
    s = float(np.median([max(t.size) for t in ships_raw])) / 16.0
    print("art-pixel scale: %.2f" % s)

    ships = [orient_right(shrink_by(t, s, cap=16)) for t in ships_raw]
    for i in range(len(ships)):                      # reviewer-confirmed corrections
        if i in SHIP_ROT_CW90:
            ships[i] = ships[i].transpose(Image.ROTATE_270)
        elif i in SHIP_ROT_180:
            ships[i] = ships[i].transpose(Image.ROTATE_180)
    player_idx = PLAYER_IDX - sum(1 for i in SHIP_DROP if i < PLAYER_IDX)
    ships = [t for i, t in enumerate(ships) if i not in SHIP_DROP]
    weapons = [shrink_by(t, s, cap=16) for i, t in enumerate(weapons_raw)
               if i in WEAPON_FORCE_KEEP or not _in_ranges(i, WEAPON_EXCLUDE)]
    mines = [shrink_by(t, s, cap=16) for i, t in enumerate(weapons_raw)
             if _in_ranges(i, MINE_RANGES)]
    bosses = [orient_right(shrink_by(t, s)) for i, t in enumerate(big_raw)
              if i not in BOSS_DROP]
    for i in range(len(bosses)):                     # reviewer-confirmed corrections
        if i in BOSS_ROT_180:
            bosses[i] = bosses[i].transpose(Image.ROTATE_180)

    ships_img, ships_meta = grid_sheet(ships, 16, 16)
    weap_img, _ = grid_sheet(weapons, 16, 16)
    mine_img, _ = grid_sheet(mines, 16, 16)
    boss_img, boss_meta = shelf_pack(bosses)
    print("ships %d  weapons %d  bosses %d (sheet %dx%d, largest %dpx)" %
          (len(ships), len(weapons), len(bosses), boss_img.width, boss_img.height,
           max(max(m[2], m[3]) for m in boss_meta)))

    quantize_keep_alpha(ships_img).save(os.path.join(HERE, "ships.png"))
    quantize_keep_alpha(weap_img).save(os.path.join(HERE, "weapons.png"))
    quantize_keep_alpha(mine_img).save(os.path.join(HERE, "mines.png"))
    quantize_keep_alpha(boss_img).save(os.path.join(HERE, "bosses.png"))
    make_icon(orient_right(ships_raw[PLAYER_IDX]))   # full-res source: crisp at 52px

    with open(os.path.join(GAME, "src", "ships_meta.h"), "w") as f:
        f.write("/* GENERATED by assets/extract_sheets.py — sprite counts, opaque bboxes and\n"
                " * boss frame rects (collision + pixel-shatter bounds). Edit the script. */\n"
                "#ifndef SHIPS_META_H\n#define SHIPS_META_H\n\n")
        f.write("#define SHIP_COUNT %d\n#define SHIP_COLS 16\n#define SHIP_CELL 16\n"
                % len(ships_meta))
        f.write("#define PLAYER_SHIP %d\n" % player_idx)
        f.write("#define BOSS_COUNT %d\n" % len(boss_meta))
        f.write("#define WEAPON_ICON_COUNT %d\n#define WEAPON_ICON_COLS 16\n" % len(weapons))
        f.write("#define MINE_COUNT %d\n#define MINE_COLS 16\n\n" % len(mines))
        for fi, field in enumerate(("bx", "by", "bw", "bh")):
            f.write("static const uint8_t ship_%s[%d] = {%s};\n" %
                    (field, len(ships_meta), ",".join(str(m[fi]) for m in ships_meta)))
        f.write("\n")
        for fi, field in enumerate(("fx", "fy", "fw", "fh")):
            f.write("static const uint16_t boss_%s[%d] = {%s};\n" %
                    (field, len(boss_meta), ",".join(str(m[fi]) for m in boss_meta)))
        f.write("\n/* dominant-hue biome per sprite: 0 cavern 1 hive 2 glacier 3 sporepit 4 ember */\n")
        f.write("static const uint8_t ship_biome[%d] = {%s};\n" %
                (len(ships), ",".join(str(classify_biome(s)) for s in ships)))
        f.write("static const uint8_t boss_biome[%d] = {%s};\n" %
                (len(bosses), ",".join(str(classify_biome(s)) for s in bosses)))
        f.write("\n#endif\n")


if __name__ == "__main__":
    main()
