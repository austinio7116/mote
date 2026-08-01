#!/usr/bin/env python3
"""Candidate 8x8 icons for the five town stores, for choosing from.

The screen borrows these from the master tileset's STATUS atlas, which has no resource
icons in it at all — so food is a clover, timber is a tree that reads as an up arrow,
and iron and gold are two piles of nuggets you have to tell apart by hue. Drawing them
is the fix: five things, eight by eight, and they only have to be told apart from each
other.

    python3 authoring/res_icons.py      # -> /tmp/motebox_res_icons.png
"""
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_sprites import (BROWN, DKGREY, LTGREY, WHITE, YELLOW, ORANGE, RED, GREEN,
                           DKGREEN, PEACH, SLATE, NAVY)

PANEL = (22, 26, 46)          # the info panel these sit on
GOLD_D = (168, 132, 24)
IRON_L = (150, 152, 162)
IRON_D = (86, 90, 104)
WOOD_L = (168, 120, 76)
WOOD_D = (110, 74, 46)
WHEAT = (232, 190, 78)
WHEAT_D = (170, 128, 40)


def blank():
    im = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
    return im, im.load()


def px(p, x, y, c):
    if 0 <= x < 8 and 0 <= y < 8:
        p[x, y] = c + (255,)


def rect(p, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px(p, x, y, c)


# ------------------------------------------------------------------- food ----

def food_wheat():
    """A — SHEAF. Three ears on stalks, tied. The oldest picture of a full store there is."""
    im, p = blank()
    for x in (2, 4, 6):
        rect(p, x, 3, x, 7, WHEAT_D)
    for x, top in ((2, 1), (4, 0), (6, 1)):
        rect(p, x - 1, top, x + 1, top + 2, WHEAT)
        px(p, x, top, WHITE)
        px(p, x - 1, top + 2, WHEAT_D)
        px(p, x + 1, top + 2, WHEAT_D)
    rect(p, 1, 5, 7, 5, BROWN)                 # the band
    return im


def food_bread():
    """B — LOAF. A round-topped loaf with a slashed crust."""
    im, p = blank()
    rect(p, 1, 3, 6, 6, (196, 140, 78))
    rect(p, 2, 2, 5, 2, (214, 162, 96))
    rect(p, 1, 7, 6, 7, (140, 92, 46))
    for x in (2, 4):
        px(p, x, 3, (240, 206, 150))
        px(p, x + 1, 4, (240, 206, 150))
    return im


def food_meat():
    """C — A JOINT. Meat on the bone: unmistakable, but it says hunting more than harvest."""
    im, p = blank()
    rect(p, 2, 2, 6, 5, (192, 82, 72))
    rect(p, 3, 3, 5, 4, (222, 120, 104))
    rect(p, 1, 5, 2, 6, WHITE)
    px(p, 1, 7, LTGREY)
    return im


# ------------------------------------------------------------------- wood ----

def wood_logs():
    """A — CUT LOGS, end on: three rings with grain. Timber, not a tree."""
    im, p = blank()
    for (x, y) in ((1, 4), (5, 4), (3, 1)):
        rect(p, x, y, x + 2, y + 2, WOOD_L)
        px(p, x + 1, y + 1, WOOD_D)
        px(p, x, y + 2, WOOD_D)
        px(p, x + 2, y, (200, 156, 110))
    rect(p, 0, 7, 7, 7, WOOD_D)
    return im


def wood_plank():
    """B — A STACK OF PLANKS, seen from the side."""
    im, p = blank()
    for i, y in enumerate((2, 4, 6)):
        rect(p, 1 - (i & 1), y, 6 + (i & 1) - 1, y + 1, WOOD_L if i % 2 == 0 else WOOD_D)
        px(p, 1 - (i & 1), y, (200, 156, 110))
    return im


def wood_axe():
    """C — AN AXE IN A STUMP. A tool, so it reads as the trade rather than the material."""
    im, p = blank()
    rect(p, 1, 5, 6, 7, WOOD_D)
    rect(p, 2, 6, 5, 6, WOOD_L)
    rect(p, 4, 1, 4, 5, (140, 96, 60))
    rect(p, 3, 0, 6, 2, LTGREY)
    px(p, 6, 1, WHITE)
    return im


# ------------------------------------------------------------------ stone ----

def stone_rocks():
    """A — TWO BOULDERS, lit from the left. What the game already uses, redrawn cleaner."""
    im, p = blank()
    rect(p, 0, 4, 3, 7, (128, 128, 138))
    rect(p, 0, 4, 1, 5, (170, 172, 182))
    rect(p, 3, 2, 7, 7, (108, 110, 122))
    rect(p, 3, 2, 5, 3, (150, 152, 164))
    px(p, 7, 7, (72, 74, 86))
    return im


def stone_blocks():
    """B — DRESSED BLOCKS, coursed. Says masonry rather than rubble."""
    im, p = blank()
    rect(p, 0, 2, 7, 7, (134, 136, 148))
    for y in (2, 5):
        rect(p, 0, y, 7, y, (168, 170, 182))
    rect(p, 0, 4, 7, 4, (96, 98, 110))
    rect(p, 0, 7, 7, 7, (96, 98, 110))
    for x in (2, 5):
        rect(p, x, 5, x, 6, (96, 98, 110))
    px(p, 3, 2, (96, 98, 110))
    return im


# ------------------------------------------------------------------- iron ----

def iron_ingots():
    """A — TWO BARS, stacked. Grey and cold, and shaped nothing like a coin."""
    im, p = blank()
    rect(p, 1, 4, 6, 5, IRON_L)
    px(p, 1, 4, (188, 190, 200)); px(p, 6, 5, IRON_D)
    rect(p, 0, 6, 7, 7, IRON_D)
    rect(p, 1, 6, 6, 6, IRON_L)
    px(p, 0, 6, (110, 114, 128))
    return im


def iron_anvil():
    """B — AN ANVIL. Iron as the thing it becomes; the most distinct outline of the five."""
    im, p = blank()
    rect(p, 1, 2, 6, 3, IRON_L)
    px(p, 1, 2, (190, 192, 202)); px(p, 6, 3, IRON_D)
    rect(p, 3, 4, 4, 5, IRON_D)
    rect(p, 2, 6, 5, 7, (110, 114, 128))
    return im


def iron_ore():
    """C — ORE IN THE ROCK: grey stone with dark metal flecks."""
    im, p = blank()
    rect(p, 1, 2, 6, 7, (118, 120, 132))
    rect(p, 1, 2, 3, 3, (156, 158, 170))
    for (x, y) in ((2, 4), (5, 3), (4, 6), (3, 6)):
        px(p, x, y, (62, 66, 80))
        px(p, x + 1, y, (86, 90, 104))
    return im


# ------------------------------------------------------------------- gold ----

def gold_coins():
    """A — A STACK OF COINS with one face on, which no other icon looks like."""
    im, p = blank()
    for i, y in enumerate((5, 6, 7)):
        rect(p, 0, y, 4, y, YELLOW if i == 0 else GOLD_D)
        px(p, 0, y, (255, 246, 170))
    rect(p, 4, 1, 7, 4, YELLOW)
    rect(p, 5, 2, 6, 3, GOLD_D)
    px(p, 4, 1, (255, 246, 170))
    return im


def gold_bar():
    """B — A BAR. Reads as treasury rather than pocket money."""
    im, p = blank()
    rect(p, 1, 3, 6, 4, YELLOW)
    rect(p, 0, 5, 7, 6, YELLOW)
    rect(p, 0, 6, 7, 6, GOLD_D)
    rect(p, 1, 3, 5, 3, (255, 246, 170))
    px(p, 0, 5, (255, 246, 170))
    return im


def gold_pouch():
    """C — A PURSE, tied at the neck, with coins showing."""
    im, p = blank()
    rect(p, 2, 1, 5, 2, (150, 108, 60))
    rect(p, 1, 3, 6, 7, (186, 140, 82))
    rect(p, 2, 4, 5, 6, YELLOW)
    px(p, 2, 4, (255, 246, 170))
    px(p, 5, 6, GOLD_D)
    return im


GROUPS = [
    ("food",  [("A sheaf", food_wheat), ("B loaf", food_bread), ("C joint", food_meat)]),
    ("wood",  [("A logs", wood_logs), ("B planks", wood_plank), ("C axe", wood_axe)]),
    ("stone", [("A boulders", stone_rocks), ("B blocks", stone_blocks)]),
    ("iron",  [("A bars", iron_ingots), ("B anvil", iron_anvil), ("C ore", iron_ore)]),
    ("gold",  [("A coins", gold_coins), ("B bar", gold_bar), ("C purse", gold_pouch)]),
]


def main():
    S = 16
    cw, ch = 8 * S + 14, 8 * S + 34
    cols = max(len(v) for _k, v in GROUPS)
    out = Image.new("RGB", (cols * cw + 92, len(GROUPS) * ch), (38, 42, 58))
    d = ImageDraw.Draw(out)
    for r, (name, cands) in enumerate(GROUPS):
        d.text((8, r * ch + ch // 2 - 6), name, fill=(240, 210, 100))
        for c, (label, fn) in enumerate(cands):
            im = fn()
            bg = Image.new("RGB", (8 * S, 8 * S), PANEL)
            big = im.resize((8 * S, 8 * S), Image.NEAREST)
            bg.paste(big, (0, 0), big)
            x, y = 92 + c * cw, r * ch + 18
            out.paste(bg, (x, y))
            d.text((x, y - 14), label, fill=(226, 230, 246))
            # and at actual size, on the panel, which is where it has to work
            small = Image.new("RGB", (10, 10), PANEL)
            small.paste(im, (1, 1), im)
            out.paste(small.resize((40, 40), Image.NEAREST), (x + 8 * S - 40, y + 8 * S + 2))
    out.save("/tmp/motebox_res_icons.png")
    print("wrote /tmp/motebox_res_icons.png", out.size)


if __name__ == "__main__":
    main()
