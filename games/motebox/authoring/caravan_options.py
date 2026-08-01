#!/usr/bin/env python3
"""Candidate 8x8 sprites for a caravan on the road, for choosing from.

A town with a surplus sends one of its own to a neighbour with a load on their back, and the
hauler is drawn as an ordinary villager — so the trade that keeps a famine regional rather than
local is invisible. This draws the candidates at actual size and at eight times, on the two
grounds they will be seen on (a road and grass), so the choice is made by looking.

Everything is drawn in code, in the palette build_sprites.py already uses.

    python3 authoring/caravan_options.py     # -> /tmp/motebox_caravan_options.png
"""
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from build_sprites import (BROWN, DKGREY, LTGREY, WHITE, YELLOW, ORANGE, RED,  # noqa: E402
                           DKGREEN, PEACH, SLATE, NAVY)

WOOD_L = (168, 120, 76)
WOOD_D = (110, 74, 46)
CANVAS = (222, 214, 190)
OX     = (150, 108, 66)
MULE   = (128, 104, 84)
SACK   = (186, 156, 106)
IRON_D = (86, 90, 104)
SKIN   = PEACH
CLOTH  = (96, 108, 150)


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


OUTL = (36, 28, 22)          # the keyline: at eight pixels a shape needs an edge


def a_handcart():
    """A — HANDCART: one big wheel, a load over it, a figure in the shafts."""
    im, p = blank()
    rect(p, 0, 1, 3, 3, SACK)                  # the load, tall and bright
    rect(p, 0, 1, 3, 1, (232, 208, 156))
    rect(p, 0, 4, 4, 4, OUTL)                  # the bed
    rect(p, 5, 3, 5, 4, WOOD_L)                # the shaft
    rect(p, 1, 5, 2, 6, OUTL)                  # the wheel
    px(p, 1, 5, LTGREY)
    rect(p, 6, 1, 7, 3, SKIN)                  # the hauler, leaning forward
    rect(p, 6, 4, 7, 6, CLOTH)
    px(p, 6, 7, OUTL); px(p, 7, 7, OUTL)
    return im


def b_oxcart():
    """B — OX CART: the animal in front with horns, two wheels behind."""
    im, p = blank()
    rect(p, 0, 2, 2, 4, SACK)                  # the load
    rect(p, 0, 2, 2, 2, (232, 208, 156))
    rect(p, 0, 5, 3, 5, OUTL)                  # the bed
    px(p, 0, 6, DKGREY); px(p, 3, 6, DKGREY)   # two wheels
    px(p, 4, 4, WOOD_L)                        # the yoke
    rect(p, 5, 3, 7, 5, OX)                    # the ox
    rect(p, 5, 6, 5, 6, OUTL); px(p, 7, 6, OUTL)
    px(p, 6, 2, WHITE); px(p, 7, 2, WHITE)     # horns
    px(p, 7, 3, OUTL)                          # the eye
    return im


def c_packmule():
    """C — PACK MULE: panniers either side, ears up, a drover behind."""
    im, p = blank()
    rect(p, 1, 3, 4, 5, MULE)                  # the body
    px(p, 1, 6, OUTL); px(p, 4, 6, OUTL)       # legs
    rect(p, 1, 2, 4, 2, SACK)                  # the panniers, bright over the back
    px(p, 0, 2, MULE); px(p, 0, 3, MULE)       # head
    px(p, 0, 1, MULE)                          # an ear
    rect(p, 6, 1, 7, 3, SKIN)                  # the drover
    rect(p, 6, 4, 7, 6, CLOTH)
    px(p, 6, 7, OUTL)
    return im


def d_porter():
    """D — A PORTER: no cart, a bundle taller than they are."""
    im, p = blank()
    rect(p, 1, 2, 2, 3, SKIN)                  # head
    rect(p, 1, 4, 3, 6, CLOTH)                 # body
    px(p, 1, 7, OUTL); px(p, 3, 7, OUTL)
    rect(p, 4, 1, 7, 5, SACK)                  # the bundle
    rect(p, 4, 1, 7, 1, (232, 208, 156))
    rect(p, 4, 5, 7, 5, OUTL)
    return im


def e_wagon():
    """E — A COVERED WAGON: white canvas, four wheels, the biggest silhouette."""
    im, p = blank()
    rect(p, 1, 1, 6, 4, CANVAS)                # the hood
    rect(p, 2, 1, 5, 1, (250, 246, 232))
    px(p, 1, 1, OUTL); px(p, 6, 1, OUTL)
    rect(p, 0, 5, 7, 5, OUTL)                  # the bed
    px(p, 1, 6, DKGREY); px(p, 2, 6, LTGREY)
    px(p, 5, 6, LTGREY); px(p, 6, 6, DKGREY)
    px(p, 1, 7, OUTL); px(p, 6, 7, OUTL)
    return im


def f_donkey():
    """F — A LADEN DONKEY on its own: no driver, just the animal and the load."""
    im, p = blank()
    rect(p, 1, 4, 5, 6, MULE)                  # body
    px(p, 0, 3, MULE); px(p, 0, 4, MULE)       # head
    px(p, 0, 2, MULE)                          # ear
    rect(p, 1, 2, 5, 3, SACK)                  # the load
    rect(p, 1, 2, 5, 2, (232, 208, 156))
    px(p, 1, 7, OUTL); px(p, 3, 7, OUTL); px(p, 5, 7, OUTL)
    return im


def g_trader():
    """G — A YOKE: a pole across the shoulders with a sack hanging at each end."""
    im, p = blank()
    rect(p, 3, 1, 4, 2, SKIN)                  # head
    rect(p, 3, 3, 4, 5, CLOTH)                 # body
    rect(p, 0, 3, 7, 3, OUTL)                  # the pole
    rect(p, 0, 4, 1, 6, SACK)                  # left sack
    rect(p, 6, 4, 7, 6, SACK)                  # right sack
    px(p, 0, 4, (232, 208, 156)); px(p, 6, 4, (232, 208, 156))
    px(p, 3, 6, CLOTH); px(p, 4, 6, CLOTH)
    px(p, 3, 7, OUTL); px(p, 4, 7, OUTL)
    return im


CANDS = [("A handcart", a_handcart), ("B ox cart", b_oxcart), ("C pack mule", c_packmule),
         ("D porter", d_porter), ("E wagon", e_wagon), ("F donkey", f_donkey),
         ("G yoke", g_trader)]

ROAD = (128, 128, 132)
GRASS = (86, 148, 70)


def main():
    Z = 10
    cw = 8 * Z + 26
    out = Image.new("RGB", (len(CANDS) * cw + 20, 8 * Z + 116), (28, 30, 40))
    d = ImageDraw.Draw(out)
    d.text((10, 6), "a caravan on the road — pick one (they are 8x8, drawn at 10x and at size)",
           fill=(240, 208, 96))
    for i, (label, fn) in enumerate(CANDS):
        im = fn()
        x = 10 + i * cw
        big = im.resize((8 * Z, 8 * Z), Image.NEAREST)
        for j, bg in enumerate((ROAD, GRASS)):
            tile = Image.new("RGB", (8 * Z, 8 * Z), bg)
            tile.paste(big, (0, 0), big)
            if j == 0:
                out.paste(tile, (x, 24))
            else:
                small = Image.new("RGB", (8, 8), bg)
                small.paste(im, (0, 0), im)
                out.paste(small.resize((8 * 4, 8 * 4), Image.NEAREST), (x, 8 * Z + 34))
                # and on grass, at size, three of them walking in a line
                line = Image.new("RGB", (26, 8), bg)
                for k in range(3):
                    line.paste(im, (k * 9, 0), im)
                out.paste(line.resize((26 * 3, 8 * 3), Image.NEAREST), (x, 8 * Z + 74))
        d.text((x, 8 * Z + 26), label, fill=(226, 230, 246))
    out.save("/tmp/motebox_caravan_options.png")
    print("wrote /tmp/motebox_caravan_options.png", out.size)


if __name__ == "__main__":
    main()
