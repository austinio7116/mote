#!/usr/bin/env python3
"""
What the game currently draws each source tile as.

Every content table in src/ points at a (sheet, cell) pair, and every sheet was
cut from a rectangle of authoring/source_tileset.png. This module walks the
tables, resolves each reference back to a (col, row) in the SOURCE, and returns
a map from that coordinate to the list of things drawn with it.

That map is what lets the annotator say "this tile is currently the Warrior" or
"nothing uses this", which is the difference between labelling a tileset and
correcting a game.

A coordinate carrying two unrelated uses is almost always a mapping bug -- it is
how five mimic animation frames ended up as a log worm, an aurochs, two cows and
a bull. dupes() lists them.

Usage:  python3 authoring/inuse.py          # prints a report
        from inuse import build             # {(col,row): ["monster: jackal", ...]}
"""
import os
import re

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
SRC = os.path.join(GAME, "src")
SHEETS = os.path.join(GAME, "assets", "sheets")
TS = 8


def read(name):
    return open(os.path.join(SRC, name), encoding="utf-8").read()


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def table(text, marker):
    i = text.index(marker)
    j = text.index("=", i)
    k = text.index("};", j)
    return strip_comments(text[j + 1:k])


# --- sheet geometry -------------------------------------------------------
# The origin comes from extract.py's SPRITE_SHEETS (the authoring source of
# truth); the column count comes from the baked PNG, exactly as rl_blit_cell
# resolves it at draw time from img->w.
def sheet_geometry():
    txt = open(os.path.join(HERE, "extract.py"), encoding="utf-8").read()
    body = txt[txt.index("SPRITE_SHEETS = {"):]
    body = body[:body.index("\n}")]
    geo = {}
    for m in re.finditer(r'"([a-z_0-9]+)"\s*:\s*\((\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)', body):
        name, c0, r0 = m.group(1), int(m.group(2)), int(m.group(3))
        p = os.path.join(SHEETS, name + ".png")
        if not os.path.isfile(p):
            continue
        geo[name] = (c0, r0, Image.open(p).width // TS)
    return geo


GEO = sheet_geometry()

# SH_* enum order in rl.h -> subsheet name
SH_NAMES = [
    "animals", "monsters", "bosses", "characters", "weapons_potions",
    "tools_wands", "weapons_elemental", "treasure_ore", "crowns_fx", "food",
    "runes", "loot_furniture", "trinkets", "props_light",
    "doors_gems_banners", "chests", "boulders_mountains", "fx_mono",
    "dungeon_mono", "stairs", "jewellery", "helms_hoods",
]
SH_BY_NAME = {n: i for i, n in enumerate(SH_NAMES)}


def src_coord(sheet_name, cell):
    """(sheet, cell) -> (col, row) in source_tileset.png"""
    c0, r0, cols = GEO[sheet_name]
    return (c0 + cell % cols, r0 + cell // cols)


def build():
    """{(col,row): ["monster: jackal", "class: Warrior", ...]}"""
    use = {}

    def add(sheet, cell, what, w=1, h=1):
        if sheet not in GEO:
            return
        cx, cy = src_coord(sheet, cell)
        for dy in range(h):
            for dx in range(w):
                use.setdefault((cx + dx, cy + dy), []).append(what)

    data_c, item_c, world_c, draw_c = (read("rl_data.c"), read("rl_item.c"),
                                       read("rl_world.c"), read("rl_draw.c"))

    # --- monsters: A()/M()/G() macros ---
    MON_SHEET = {"A": "animals", "M": "monsters", "G": "crowns_fx"}
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*([AMG])\((\d+)\)',
                         table(data_c, "g_mon_kind[]")):
        add(MON_SHEET[m.group(2)], int(m.group(3)), "monster: " + m.group(1))

    # --- bosses: 2x2 blocks on the bosses sheet ---
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,',
                         table(data_c, "g_boss_kind[]")):
        add("bosses", int(m.group(2)), "BOSS: " + m.group(1), 2, 2)

    # --- player classes: the characters sheet ---
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,',
                         table(data_c, "g_class[]")):
        add("characters", int(m.group(2)), "CLASS: " + m.group(1))

    # --- items ---
    ITEM_SHEET = {"W": "weapons_potions", "T": "tools_wands",
                  "E": "weapons_elemental", "O": "treasure_ore",
                  "G": "crowns_fx", "F": "food", "R": "runes",
                  "L": "loot_furniture", "K": "trinkets"}
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*([WTEOGFRLK])\((\d+)\)\s*,\s*(TV_\w+)',
                         table(item_c, "g_item_kind[]")):
        add(ITEM_SHEET[m.group(2)], int(m.group(3)),
            "item(%s): %s" % (m.group(4)[3:].lower(), m.group(1)))

    # --- shopfronts ---
    names = re.findall(r'"([^"]+)"', table(world_c, "g_shop_name[SHOP_N]"))
    sheets = re.findall(r"SH_(\w+)", table(world_c, "g_shop_sheet[SHOP_N]"))
    cells = [int(x) for x in re.findall(r"\d+", table(world_c, "g_shop_cell[SHOP_N]"))]
    SH_ALIAS = {"DOORS": "doors_gems_banners", "PROPS": "props_light"}
    for i, n in enumerate(names):
        add(SH_ALIAS.get(sheets[i], sheets[i].lower()), cells[i], "shop: " + n)

    # --- named feature sprites (the SPR_* defines) ---
    # Each is "#define SPR_X  N  /* sheet (c,r): note */" or plain; the sheet is
    # taken from the switch in rl_draw that uses it, so read the pairing there.
    spr = {m.group(1): int(m.group(2))
           for m in re.finditer(r"#define\s+(SPR_\w+)\s+(\d+)", draw_c)}
    for m in re.finditer(r"sheet = (SH_\w+);\s*cell = (SPR_\w+)", draw_c):
        sh, name = m.group(1)[3:].lower(), m.group(2)
        if name in spr:
            add(SH_NAMES[SH_BY_NAME[{"doors": "doors_gems_banners",
                                     "props": "props_light",
                                     "trinkets": "trinkets",
                                     "stairs": "stairs",
                                     "chests": "chests"}.get(sh, sh)]],
                spr[name], "terrain: " + name[4:].lower().replace("_", " "))

    return use


def dupes(use):
    """Coordinates whose uses are not all the same entity -- likely bugs."""
    out = {}
    for k, v in use.items():
        kinds = {u.split(":")[0] for u in v}
        if len(v) > 1 and (len(kinds) > 1 or len({u for u in v}) > 1):
            out[k] = v
    return out


if __name__ == "__main__":
    u = build()
    print("%d source tiles are referenced by the game" % len(u))
    d = dupes(u)
    if not d:
        print("no tile carries two different uses")
    else:
        print("\n%d tiles carry more than one use:" % len(d))
        for k in sorted(d):
            print("  (%2d,%2d)  %s" % (k[0], k[1], "  |  ".join(d[k])))
