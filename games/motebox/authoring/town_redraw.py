#!/usr/bin/env python3
"""Ten town buildings drawn at 8x14, from the designs on the generated sheets.

The sheets are drawn at about a hundred pixels a building and the game draws one in eight by
fourteen. Reducing the artwork keeps the proportions and loses the design — the door, the window,
the awning, the spire all average into a brown block. So this is the other route: the same
buildings, drawn as pixels, taking from the sheets what survives at this size, which is the
silhouette and one strong feature each.

The palette is build_sprites.py's, so these sit with the rest of the town rather than beside it.
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from build_sprites import (BROWN, DKGREY, LTGREY, WHITE, YELLOW, ORANGE, RED,   # noqa: E402
                           DKGREEN, GREEN, PEACH, SLATE, NAVY, BLUE, PLUM)

W, H = 8, 14
OUTL  = (36, 28, 22)       # the keyline every one of these needs at this size
WALL  = (226, 206, 172)    # plaster
WALL2 = (196, 172, 140)
WOOD  = (140, 96, 60)
WOOD2 = (104, 70, 44)
TILE  = (176, 74, 62)      # a clay roof
TILE2 = (132, 52, 44)
SLATEY = (96, 104, 124)
SLATE2 = (70, 76, 96)
GLASS = (120, 178, 214)
GLASS2 = (74, 122, 160)
STONE = (176, 176, 172)
STONE2 = (132, 132, 130)
AWN   = (198, 76, 66)
DOOR  = (96, 62, 40)


def blank():
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    return im, im.load()


def px(p, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        p[x, y] = c + (255,)


def rect(p, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px(p, x, y, c)


def gable(p, y0, rows, c, c2):
    """A pitched roof seen front on: a triangle that widens by one either side per row, with the
    lit face and the shaded face separated down the ridge. This is the shape every cottage on the
    sheet has, and it is the one thing that has to survive the reduction."""
    for i in range(rows):
        x0, x1 = 3 - i, 4 + i
        for x in range(max(0, x0), min(W - 1, x1) + 1):
            px(p, x, y0 + i, c if x <= 3 else c2)
        px(p, max(0, x0), y0 + i, OUTL)
        px(p, min(W - 1, x1), y0 + i, OUTL)


def cottage():
    """A — the sheet's plainest house: steep roof, one door, one window."""
    im, p = blank()
    gable(p, 3, 4, TILE, TILE2)
    rect(p, 1, 7, 6, 12, WALL)
    rect(p, 1, 7, 1, 12, WALL2)
    rect(p, 0, 7, 0, 12, OUTL); rect(p, 7, 7, 7, 12, OUTL)
    rect(p, 3, 9, 4, 12, DOOR)                     # the door, on the ground
    px(p, 4, 10, YELLOW)                           # a handle catching the light
    rect(p, 5, 8, 6, 9, GLASS); px(p, 5, 8, WHITE)
    rect(p, 0, 13, 7, 13, OUTL)
    return im


def gabled():
    """B — two storeys under one gable, the window in the apex."""
    im, p = blank()
    gable(p, 1, 4, TILE, TILE2)
    rect(p, 2, 3, 5, 4, TILE2)
    px(p, 3, 3, GLASS); px(p, 4, 3, GLASS)         # the apex window
    rect(p, 1, 5, 6, 12, WALL)
    rect(p, 1, 5, 1, 12, WALL2)
    rect(p, 0, 5, 0, 12, OUTL); rect(p, 7, 5, 7, 12, OUTL)
    rect(p, 2, 7, 3, 8, GLASS); rect(p, 5, 7, 6, 8, GLASS)
    px(p, 2, 7, WHITE); px(p, 5, 7, WHITE)
    rect(p, 3, 10, 4, 12, DOOR)
    rect(p, 0, 13, 7, 13, OUTL)
    return im


def longhouse():
    """C — the wide, low one: a shallow roof and a run of windows."""
    im, p = blank()
    rect(p, 0, 5, 7, 5, OUTL)
    rect(p, 1, 6, 6, 7, TILE); rect(p, 4, 6, 6, 7, TILE2)
    rect(p, 0, 8, 7, 12, WALL)
    rect(p, 0, 8, 0, 12, OUTL); rect(p, 7, 8, 7, 12, OUTL)
    for x in (1, 4, 6):
        rect(p, x, 9, x, 10, GLASS)
    rect(p, 2, 10, 3, 12, DOOR)
    rect(p, 0, 13, 7, 13, OUTL)
    return im


def chalet():
    """D — the one with the chimney: a broad roof, smoke going up, a lit window."""
    im, p = blank()
    px(p, 6, 1, DKGREY); px(p, 6, 2, DKGREY)       # the chimney
    px(p, 6, 0, LTGREY)
    gable(p, 3, 4, SLATEY, SLATE2)
    rect(p, 1, 7, 6, 12, WALL)
    rect(p, 1, 7, 1, 12, WALL2)
    rect(p, 0, 7, 0, 12, OUTL); rect(p, 7, 7, 7, 12, OUTL)
    rect(p, 2, 8, 3, 9, YELLOW)                    # somebody is in
    rect(p, 4, 10, 5, 12, DOOR)
    rect(p, 0, 13, 7, 13, OUTL)
    return im


def church():
    """E — the spire is the whole silhouette, so it gets half the cell."""
    im, p = blank()
    px(p, 3, 0, WHITE); px(p, 4, 0, WHITE)         # the cross
    px(p, 3, 1, SLATEY); px(p, 4, 1, SLATEY)
    for i in range(3):                             # the spire
        rect(p, 3 - i, 2 + i, 4 + i, 2 + i, SLATEY if i < 2 else SLATE2)
    rect(p, 2, 5, 5, 8, STONE)
    rect(p, 2, 5, 2, 8, STONE2)
    px(p, 3, 6, GLASS); px(p, 4, 6, GLASS)
    rect(p, 1, 9, 6, 12, STONE)
    rect(p, 1, 9, 1, 12, STONE2)
    rect(p, 0, 9, 0, 12, OUTL); rect(p, 7, 9, 7, 12, OUTL)
    rect(p, 3, 10, 4, 12, DOOR)
    rect(p, 0, 13, 7, 13, OUTL)
    return im


def shop():
    """F — the sheet's storefront: a sign board over a striped awning."""
    im, p = blank()
    rect(p, 0, 2, 7, 3, OUTL)                      # the fascia
    rect(p, 1, 2, 6, 2, RED); px(p, 2, 2, WHITE); px(p, 4, 2, WHITE)   # lettering
    rect(p, 0, 4, 7, 9, WALL)
    rect(p, 0, 4, 0, 9, OUTL); rect(p, 7, 4, 7, 9, OUTL)
    for x in range(1, 7):                          # the awning, striped
        px(p, x, 5, AWN if x % 2 else WHITE)
    rect(p, 1, 7, 3, 9, GLASS); px(p, 1, 7, WHITE)
    rect(p, 5, 7, 6, 12, DOOR)
    rect(p, 1, 10, 4, 12, WALL2)
    rect(p, 0, 10, 0, 12, OUTL); rect(p, 7, 10, 7, 12, OUTL)
    rect(p, 0, 13, 7, 13, OUTL)
    return im


def store():
    """G — the corner shop: glass at the front, brick above."""
    im, p = blank()
    rect(p, 0, 3, 7, 3, OUTL)
    rect(p, 1, 4, 6, 7, WOOD)                      # brick upper storey
    rect(p, 1, 4, 6, 4, WOOD2)
    for x in (2, 5):
        rect(p, x, 5, x, 6, GLASS)
    rect(p, 0, 4, 0, 7, OUTL); rect(p, 7, 4, 7, 7, OUTL)
    rect(p, 0, 8, 7, 8, OUTL)                      # the shopfront band
    rect(p, 1, 9, 6, 12, GLASS2)
    rect(p, 1, 9, 6, 9, GLASS)
    rect(p, 3, 10, 4, 12, DOOR)
    rect(p, 0, 9, 0, 12, OUTL); rect(p, 7, 9, 7, 12, OUTL)
    rect(p, 0, 13, 7, 13, OUTL)
    return im


def townhouse():
    """H — three storeys, a window grid, a flat parapet."""
    im, p = blank()
    rect(p, 0, 2, 7, 2, OUTL)
    rect(p, 1, 3, 6, 12, WALL)
    rect(p, 1, 3, 1, 12, WALL2)
    rect(p, 0, 3, 0, 12, OUTL); rect(p, 7, 3, 7, 12, OUTL)
    for y in (4, 7):
        for x in (2, 5):
            rect(p, x, y, x + 1, y + 1, GLASS)
            px(p, x, y, WHITE)
    rect(p, 3, 10, 4, 12, DOOR)
    px(p, 2, 11, GLASS); px(p, 5, 11, GLASS)
    rect(p, 0, 13, 7, 13, OUTL)
    return im


def tenement():
    """I — the brick block: no roof to speak of, all windows."""
    im, p = blank()
    rect(p, 0, 1, 7, 1, OUTL)
    rect(p, 1, 2, 6, 12, TILE2)
    rect(p, 1, 2, 1, 12, (110, 44, 38))
    rect(p, 0, 2, 0, 12, OUTL); rect(p, 7, 2, 7, 12, OUTL)
    for y in (3, 6, 9):
        for x in (2, 4, 6):
            px(p, x, y, GLASS)
            px(p, x, y + 1, GLASS2)
    rect(p, 3, 11, 4, 12, DOOR)
    rect(p, 0, 13, 7, 13, OUTL)
    return im


def tower():
    """J — the glass tower: a tall thin silhouette with a lit grid."""
    im, p = blank()
    rect(p, 2, 0, 5, 0, OUTL)
    rect(p, 2, 1, 5, 12, GLASS2)
    rect(p, 2, 1, 2, 12, (52, 92, 126))
    rect(p, 1, 1, 1, 12, OUTL); rect(p, 6, 1, 6, 12, OUTL)
    for y in range(2, 12, 2):
        px(p, 3, y, GLASS); px(p, 5, y, GLASS if y % 4 else YELLOW)
    rect(p, 3, 11, 4, 12, DOOR)
    rect(p, 1, 13, 6, 13, OUTL)
    return im


DESIGNS = {
    "cottage": cottage, "gabled": gabled, "longhouse": longhouse, "chalet": chalet,
    "church": church, "shop": shop, "store": store, "townhouse": townhouse,
    "tenement": tenement, "tower": tower,
}


def draw(name):
    return DESIGNS[name]()


if __name__ == "__main__":
    from PIL import ImageDraw
    Z = 14
    out = Image.new("RGB", (len(DESIGNS) * (W * Z + 12) + 20, H * Z + 60), (28, 30, 40))
    d = ImageDraw.Draw(out)
    d.text((10, 8), "redrawn from the sheets, 8x14, in the town palette", fill=(240, 208, 96))
    for i, (name, fn) in enumerate(DESIGNS.items()):
        im = fn()
        x = 10 + i * (W * Z + 12)
        tile = Image.new("RGB", (W * Z, H * Z), (52, 92, 48))
        big = im.resize((W * Z, H * Z), Image.NEAREST)
        tile.paste(big, (0, 0), big)
        out.paste(tile, (x, 26))
        d.text((x, H * Z + 30), name, fill=(226, 230, 246))
    out.save("/tmp/motebox_town_redraw.png")
    print("wrote /tmp/motebox_town_redraw.png", out.size)
