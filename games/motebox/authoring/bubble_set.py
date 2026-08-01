#!/usr/bin/env python3
"""Render the five status bubbles the game actually draws, as a set, to confirm them.

The mapping lives in one place in mb_draw.c and is read back from there rather than restated
here, so this picture cannot show one thing while the game draws another.

    python3 authoring/bubble_set.py       # -> /tmp/motebox_bubble_set.png
"""
import os
import re
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
SHEET = os.path.join(HERE, "source_tileset.png")

sys.path.insert(0, HERE)
from extract_box import SPRITE_SHEETS                            # noqa: E402

# what each one means, in the order mb_draw.c tests them
MEANING = [
    ("madness",  "attacks anyone, family included"),
    ("plague",   "sick, and spreading it"),
    ("marked",   "every hunting thing is coming"),
    ("starving", "nothing left to eat"),
    ("wounded",  "bleeding, close to dying"),
]


def picks():
    """The cells out of mb_draw.c, so the sheet and the game cannot disagree."""
    src = open(os.path.join(GAME, "src", "mb_draw.c")).read()
    blk = src[src.index("int bcx = -1, bcy = 2;"):]
    blk = blk[:blk.index("if (bcx >= 0)")]
    out = []
    for m in re.finditer(r"bcx = (\d+); /\* (\d+),(\d+)", blk.replace("  ", " ")):
        out.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    return out


def main():
    im = Image.open(SHEET).convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, _a = px[x, y]
            if (r, g, b) == (0, 0, 0):
                px[x, y] = (0, 0, 0, 0)
    c0, r0, _c1, _r1 = SPRITE_SHEETS["ui_status"]
    # a villager to stand under them, from the characters band
    ch0, chr0, _a, _b = SPRITE_SHEETS["characters"]
    person = im.crop(((ch0 + 4) * 8, (chr0 + 4) * 8, (ch0 + 5) * 8, (chr0 + 5) * 8))

    got = picks()
    assert len(got) == len(MEANING), (got, len(MEANING))

    Z, GAP = 12, 26
    GRASS = (86, 148, 70)
    cw = 8 * Z + GAP
    out = Image.new("RGB", (len(got) * cw + 20, 8 * Z + 150), (28, 30, 40))
    d = ImageDraw.Draw(out)
    d.text((10, 6), "the five status bubbles, as the game draws them "
                    "(cell coordinates are the master sheet's)", fill=(240, 208, 96))
    for i, ((cx, mc, mr), (name, note)) in enumerate(zip(got, MEANING)):
        cell = im.crop((mc * 8, mr * 8, mc * 8 + 8, mr * 8 + 8))
        x = 10 + i * cw
        tile = Image.new("RGB", (8 * Z, 8 * Z), (38, 42, 58))
        big = cell.resize((8 * Z, 8 * Z), Image.NEAREST)
        tile.paste(big, (0, 0), big)
        out.paste(tile, (x, 24))
        d.text((x, 8 * Z + 28), "%s  (%d,%d)" % (name, mc, mr), fill=(226, 230, 246))
        d.text((x, 8 * Z + 42), note, fill=(150, 158, 180))
        # and the thing itself: bubble over a villager, at actual size, on grass
        scene = Image.new("RGB", (8, 17), GRASS)
        scene.paste(cell, (0, 0), cell)
        scene.paste(person, (0, 9), person)
        out.paste(scene.resize((8 * 5, 17 * 5), Image.NEAREST), (x, 8 * Z + 58))
    out.save("/tmp/motebox_bubble_set.png")
    print("wrote /tmp/motebox_bubble_set.png", out.size)


if __name__ == "__main__":
    main()
