#!/usr/bin/env python3
"""Regenerate the guide's icon plates from the sheets, with their names under them.

The plates were made by hand the first time and the recipe went with the session, so
when two species were renamed there was no way to redo them. The cells here are the
ones MB_SP actually indexes (src/mb_unit.c) — if a plate and the game disagree, this
file is the thing to fix.

    python3 authoring/plates.py            # -> docs/img/gallery/motebox-p-*.png
"""
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
DOCS = os.path.join(os.path.dirname(os.path.dirname(GAME)), "docs", "img", "gallery")
SHEETS = os.path.join(GAME, "assets", "sheets")

CELL, PAD, LABEL = 64, 6, 16          # 70 x 70 a cell, six across — matches the first set
BG = (32, 38, 54)
CELLBG = (44, 50, 70)
INK = (150, 158, 186)
SEA = (25, 107, 158)      # the game's deep water, for the one tile that sits in it

# name, sheet, column, row — the SAME cells as MB_SP in src/mb_unit.c
ANIMALS = [
    ("deer",     "animals",  4, 0), ("boar",     "animals",  5, 0),
    ("sheep",    "animals", 10, 0), ("hen",      "animals",  8, 0),
    ("goat",     "animals", 11, 0), ("wolf",     "animals",  3, 3),
    ("wild dog", "animals",  1, 1), ("snake",    "animals",  3, 1),
    # RENAMED, because the art was always this and the names were not: the red one has two
    # raised claws and the green one is a domed shell with a small head.
    ("crab",     "monsters", 2, 0), ("turtle",   "animals",  6, 4),
    ("bat",      "animals",  5, 1), ("rat",      "animals",  9, 1),
]


def load(name):
    im = Image.open(os.path.join(SHEETS, name + ".png")).convert("RGBA")
    px = im.load()
    for y in range(im.height):                       # magenta is the transparency key
        for x in range(im.width):
            r, g, b, _a = px[x, y]
            if r > 200 and g < 70 and b > 200:
                px[x, y] = (0, 0, 0, 0)
    return im


def plate(entries, out, cols=6, zoom=7):
    sheets = {}
    rows = (len(entries) + cols - 1) // cols
    cw, ch = CELL + PAD, CELL + PAD + LABEL
    im = Image.new("RGB", (cols * cw, rows * ch), BG)
    d = ImageDraw.Draw(im)
    for i, (name, sheet, cx, cy) in enumerate(entries):
        if sheet not in sheets:
            sheets[sheet] = load(sheet)
        src = sheets[sheet].crop((cx * 8, cy * 8, cx * 8 + 8, cy * 8 + 8))
        x, y = (i % cols) * cw + PAD // 2, (i // cols) * ch + PAD // 2
        d.rectangle([x, y, x + CELL - 1, y + CELL - 1], fill=CELLBG)
        big = src.resize((8 * zoom, 8 * zoom), Image.NEAREST)
        im.paste(big, (x + (CELL - 8 * zoom) // 2, y + (CELL - 8 * zoom) // 2), big)
        w = d.textlength(name)
        d.text((x + (CELL - w) / 2, y + CELL + 1), name, fill=INK)
    im.save(out)
    print("wrote", out, im.size)


# THE TOWN SHEET, whose cells are 8 wide and 14 TALL — a building stands above its tile — so
# the buildings plate needs its own crop. Names as the game names them (O_NAME), in the order
# the sheet is built, minus the blueprint ghost which is not a building.
BUILDINGS = [
    "fire pit", "hall", "great hall", "castle", "house", "cottage", "manor", "farm", "mine",
    "woodcutter", "barracks", "temple", "tower", "dock", "wall", "granary", "market", "library",
    "foundry", "college", "hospital", "factory", "station", "power station", "silo", "monument",
    "fountain",
]


def buildings_plate(out, cols=9, zoom=4, row=0):
    """One column of the town sheet per building, in the kingdom colour `row`."""
    sheet = load("town")
    cw = 8 * zoom + 18
    ch = 14 * zoom + LABEL + 6
    rows = (len(BUILDINGS) + cols - 1) // cols
    im = Image.new("RGB", (cols * cw, rows * ch), BG)
    d = ImageDraw.Draw(im)
    for i, name in enumerate(BUILDINGS):
        src = sheet.crop((i * 8, row * 14, i * 8 + 8, row * 14 + 14))
        x, y = (i % cols) * cw, (i // cols) * ch
        d.rectangle([x + 2, y + 2, x + cw - 4, y + ch - LABEL - 4], fill=CELLBG)
        if name == "dock":
            # The one building drawn from ABOVE, lying flat on the water. Against a plain
            # cell it reads as a dropped plank; against water it reads as a jetty. It occupies
            # the bottom eight rows of its cell, which is its tile.
            d.rectangle([x + 2, y + 4 + 6 * zoom, x + cw - 4, y + ch - LABEL - 4], fill=SEA)
        big = src.resize((8 * zoom, 14 * zoom), Image.NEAREST)
        im.paste(big, (x + (cw - 8 * zoom) // 2, y + 4), big)
        w = d.textlength(name)
        d.text((x + (cw - w) / 2, y + ch - LABEL), name, fill=INK)
    im.save(out)
    print("wrote", out, im.size)


# THE TRAITS THAT HAVE AN ICON, in the order mb_ui.c draws them. The rest of a soul's
# traits are shown as a count, and the guide lists them as text.
TRAITS = [
    ("blessed", "ui_status",  5, 1), ("cursed",  "ui_status",  6, 1),
    ("plagued", "ui_status",  0, 3), ("immune",  "ui_status", 12, 3),
    ("tough",   "ui_status",  4, 5), ("fast",    "ui_status",  3, 3),
    ("brave",   "treasure_ore", 0, 3), ("coward", "ui_status",  4, 3),
    ("mad",     "ui_status",  2, 3), ("risen",   "ui_status", 11, 3),
    ("fertile", "ui_status",  0, 5), ("marked",  "ui_status",  3, 0),
]


STORES = [("food", "stores", 0, 0), ("wood", "stores", 1, 0), ("stone", "stores", 2, 0),
          ("iron", "stores", 3, 0), ("gold", "stores", 4, 0)]


if __name__ == "__main__":
    plate(ANIMALS, os.path.join(DOCS, "motebox-p-animals.png"))
    buildings_plate(os.path.join(DOCS, "motebox-p-buildings.png"))
    plate(TRAITS, os.path.join(DOCS, "motebox-p-traits.png"), cols=6)
    plate(STORES, os.path.join(DOCS, "motebox-p-stores.png"), cols=5)
