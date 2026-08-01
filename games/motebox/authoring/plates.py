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


if __name__ == "__main__":
    plate(ANIMALS, os.path.join(DOCS, "motebox-p-animals.png"))
