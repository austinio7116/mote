#!/usr/bin/env python3
"""Lay out the status atlas's bubbles and markers, labelled with their cells, for choosing from.

A bubble over a villager's head is a good way to show a critical state — plague, madness, hunger —
and the master sheet has a whole band of them. Which bubble means what is not something to guess
at: this renders every cell of the STATUS atlas at eight times size with its own master-sheet
coordinate under it, so the choice can be made by looking and then named.

    python3 authoring/bubble_options.py      # -> /tmp/motebox_bubbles.png
"""
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SHEET = os.path.join(HERE, "source_tileset.png")

sys.path.insert(0, HERE)
from extract_box import SPRITE_SHEETS                            # noqa: E402

BANDS = [("ui_status", "the STATUS atlas: bubbles, faces, elements, hearts"),
         ("crowns_fx", "crowns and effects"),
         ("fx_mono",   "the FX master: bolts, rings, sparkles")]
Z = 8
PAD = 6
LBL = 12


def main():
    im = Image.open(SHEET).convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, _a = px[x, y]
            if (r, g, b) == (0, 0, 0):
                px[x, y] = (0, 0, 0, 0)

    rows = []
    for key, title in BANDS:
        c0, r0, c1, r1 = SPRITE_SHEETS[key]
        rows.append((key, title, c0, r0, c1, r1))

    cellw = 8 * Z + PAD
    cellh = 8 * Z + PAD + LBL
    width = max((c1 - c0 + 1) for _k, _t, c0, _r0, c1, _r1 in rows) * cellw + 20
    height = sum((r1 - r0 + 1) * cellh + 26 for _k, _t, _c0, r0, _c1, r1 in rows) + 20
    out = Image.new("RGB", (width, height), (24, 26, 36))
    d = ImageDraw.Draw(out)

    y = 10
    for key, title, c0, r0, c1, r1 in rows:
        d.text((10, y), "%s  —  %s" % (key, title), fill=(240, 208, 96))
        y += 18
        for r in range(r0, r1 + 1):
            x = 10
            for c in range(c0, c1 + 1):
                cell = im.crop((c * 8, r * 8, c * 8 + 8, r * 8 + 8))
                # every cell on the same dark ground the game draws it on
                tile = Image.new("RGB", (8 * Z, 8 * Z), (38, 42, 58))
                tile.paste(cell.resize((8 * Z, 8 * Z), Image.NEAREST), (0, 0),
                           cell.resize((8 * Z, 8 * Z), Image.NEAREST))
                out.paste(tile, (x, y))
                d.text((x + 2, y + 8 * Z + 1), "%d,%d" % (c, r), fill=(150, 158, 180))
                x += cellw
            y += cellh
        y += 8
    out.save("/tmp/motebox_bubbles.png")
    print("wrote /tmp/motebox_bubbles.png", out.size)


if __name__ == "__main__":
    main()
