#!/usr/bin/env python3
"""Turn the two generated building sheets into candidate 8x14 town sprites, two ways.

The town sheet is drawn in code (build_sprites.py) and every building on it is a hand-placed
pixel. Two large generated sheets of front-on buildings turned up, so the question is whether
they are worth using — and there are exactly two honest answers to test:

  DOWNSAMPLED   the artwork itself, trimmed to its own outline and reduced to the cell size the
                game draws (8 wide, 14 tall, standing on its tile), then flattened to the few
                tones a 4bpp sheet can hold. Fast, and it keeps the source's proportions.
  REDRAWN       the same buildings drawn from scratch at 8x14 with the shapes the sheet suggests
                — the awning, the shopfront, the window grid, the spire — in the palette the rest
                of the town already uses.

Both are produced here and shown side by side with what the game draws today, at eight times
size and at the size they will actually be seen.

    python3 authoring/town_from_sheet.py            # -> /tmp/motebox_town_options.png
    python3 authoring/town_from_sheet.py --grid     # -> /tmp/motebox_sheet_grid.png (the source,
                                                    #    numbered, for picking cells)
"""
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

SHEETS = {
    "A": "/tmp/Gemini_Generated_Image_i5xdmyi5xdmyi5xd.png",   # cottages, churches, trees
    "B": "/tmp/Gemini_Generated_Image_lhzrwklhzrwklhzr.png",   # shops, blocks, towers, civic
}
COLS, ROWS = 20, 12          # measured off the sheets: 2816/20 = 140.8, 1536/12 = 128
CW, CH = 2816 / COLS, 1536 / ROWS

# the game's cell: eight wide, fourteen tall, the building standing on the bottom tile
TW, TH = 8, 14


def cells(tag):
    """Every cell of a sheet, trimmed to its own content."""
    im = Image.open(SHEETS[tag]).convert("RGBA")
    out = {}
    for r in range(ROWS):
        for c in range(COLS):
            box = (int(c * CW), int(r * CH), int((c + 1) * CW), int((r + 1) * CH))
            cel = im.crop(box)
            # the sheet's ground is a near-black grid; anything brighter is the building
            mask = cel.convert("L").point(lambda v: 255 if v > 60 else 0)
            bb = mask.getbbox()
            if not bb:
                continue
            w, h = bb[2] - bb[0], bb[3] - bb[1]
            if w < 20 or h < 20:
                continue
            out[(c, r)] = cel.crop(bb)
    return out


def flatten(im, keep=10):
    """Down to the handful of tones a 4bpp sheet holds, so the candidate is what would ship."""
    q = im.convert("RGB").quantize(colors=keep, method=Image.MEDIANCUT, dither=Image.NONE)
    return q.convert("RGBA")


def keyline(im, col=(32, 24, 20)):
    """A dark edge round the silhouette. Below about twelve pixels a picture without one reads as
    a smudge on the ground rather than a thing standing on it — it is the single biggest
    difference between a reduced photograph and a sprite."""
    px = im.load()
    w, h = im.size
    edge = []
    for y in range(h):
        for x in range(w):
            if px[x, y][3] == 0:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if nx < 0 or ny < 0 or nx >= w or ny >= h or px[nx, ny][3] == 0:
                    edge.append((x, y)); break
    for x, y in edge:
        r, g, b, _a = px[x, y]
        px[x, y] = (max(0, r * 2 // 5), max(0, g * 2 // 5), max(0, b * 2 // 5), 255)
    return im


def downsample(cel, mode="area"):
    """One cell, reduced to the game's 8x14 with its feet on the tile.

    THE FIRST VERSION KEPT THE SOURCE ASPECT and every building came out six pixels tall and
    blurred: a hundred-pixel drawing averaged into eight is mush whatever the filter. What makes
    it read is filling the cell (these buildings are drawn wide, and the game's cell is tall),
    cutting the palette to a handful of tones so flat areas stay flat, and putting the edge back.
    """
    w, h = cel.size
    if mode == "aspect":                      # the honest reduction, for comparison
        th = max(6, min(TH, round(h * TW / w)))
        small = cel.resize((TW, th), Image.BOX)
        small = flatten(small, 8)
    else:
        # fill the cell: the roof reaches the top, the door sits on the tile
        th = TH - 2
        small = cel.resize((TW, th), Image.BOX)
        small = flatten(small, 5 if mode == "flat" else 8)
    px = small.load()
    src = cel.resize(small.size, Image.BOX).load()
    for y in range(small.size[1]):
        for x in range(small.size[0]):
            r, g, b, _a = px[x, y]
            keep = src[x, y][3] > 110 and (r + g + b) > 70
            px[x, y] = (r, g, b, 255 if keep else 0)
    if mode != "aspect":
        small = keyline(small)
    out = Image.new("RGBA", (TW, TH), (0, 0, 0, 0))
    out.paste(small, ((TW - small.size[0]) // 2, TH - small.size[1]))
    return out


def grid_sheet():
    """The source sheets, numbered, so cells can be named in a reply."""
    for tag in SHEETS:
        im = Image.open(SHEETS[tag]).convert("RGB")
        S = 0.5
        big = im.resize((int(im.width * S), int(im.height * S)), Image.LANCZOS)
        d = ImageDraw.Draw(big)
        for r in range(ROWS):
            for c in range(COLS):
                d.text((int(c * CW * S) + 3, int(r * CH * S) + 2), "%s%d,%d" % (tag, c, r),
                       fill=(255, 220, 90))
        p = "/tmp/motebox_sheet_%s.png" % tag
        big.save(p)
        print("wrote %s %s" % (p, big.size))


def main():
    if "--grid" in sys.argv:
        grid_sheet()
        return
    import town_redraw

    PICKS = [("cottage",  ("A", 0, 0)),  ("gabled",   ("A", 2, 1)),
             ("longhouse", ("A", 4, 3)), ("chalet",   ("A", 8, 4)),
             ("church",   ("A", 11, 2)), ("shop",     ("B", 0, 0)),
             ("store",    ("B", 3, 2)),  ("townhouse", ("B", 9, 0)),
             ("tenement", ("B", 10, 3)), ("tower",    ("B", 13, 4))]
    got = {t: cells(t) for t in SHEETS}
    town = Image.open(os.path.join(GAME, "assets", "sheets", "town.png")).convert("RGBA")

    Z = 12
    cw = TW * Z + 20
    rowlab = ["the sheet", "reduced, aspect", "reduced, filled", "reduced, 5 tones", "redrawn",
              "reduced to 16x16"]
    out = Image.new("RGB", (len(PICKS) * cw + 30, 130 + len(rowlab) * (TH * Z + 22) + 90),
                    (28, 30, 40))
    d = ImageDraw.Draw(out)
    d.text((10, 8), "alternative town sprites: the generated sheet reduced three ways, and drawn "
                    "again by hand from the same designs", fill=(240, 208, 96))
    d.text((10, 22), "each is 8x14, the cell the game draws a building in; the bottom strip is "
                     "actual size on grass", fill=(150, 158, 180))
    for i, (name, (tag, c, r)) in enumerate(PICKS):
        x = 30 + i * cw
        cel = got[tag].get((c, r))
        d.text((x, 40), "%s  %s%d,%d" % (name, tag, c, r), fill=(226, 230, 246))
        y = 54
        thumb = cel.copy(); thumb.thumbnail((TW * Z, TH * Z), Image.LANCZOS)
        out.paste(thumb, (x, y), thumb)
        y += TH * Z + 22
        # AND WHAT IT WOULD TAKE. The last row asks the other question: if the game drew a
        # building in sixteen pixels instead of eight, would the artwork survive the reduction?
        # It is not a change anybody has asked for — every building position, the tile grid and
        # the whole town sheet are eight wide — but it is the honest test of the sheets.
        big16 = flatten(cel.resize((16, 16), Image.BOX), 12)
        variants = [downsample(cel, "aspect"), downsample(cel, "fill"), downsample(cel, "flat"),
                    town_redraw.draw(name), big16]
        for s2 in variants:
            # the 16x16 candidate is drawn at half the zoom, so it occupies the same space on
            # the page as it would on the screen — otherwise it is being flattered
            z = Z if s2.size[0] == TW else Z * TW // 16
            tile = Image.new("RGB", (TW * Z, TH * Z), (52, 92, 48))
            big = s2.resize((s2.size[0] * z, s2.size[1] * z), Image.NEAREST)
            tile.paste(big, ((TW * Z - big.width) // 2, TH * Z - big.height), big)
            out.paste(tile, (x, y))
            y += TH * Z + 22
        strip = Image.new("RGB", (TW * 2 + 2, TH), (86, 148, 70))
        strip.paste(variants[1], (0, 0), variants[1])
        strip.paste(variants[3], (TW + 2, 0), variants[3])
        out.paste(strip.resize((strip.width * 5, strip.height * 5), Image.NEAREST), (x, y + 4))
    for k, lab in enumerate(rowlab):
        d.text((4, 54 + k * (TH * Z + 22) + TH * Z // 2), lab.replace(" ", "\n"),
               fill=(150, 158, 180))
    out.save("/tmp/motebox_town_options.png")
    print("wrote /tmp/motebox_town_options.png", out.size)


if __name__ == "__main__":
    main()
