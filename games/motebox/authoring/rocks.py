"""Motebox — rock OBJECTS, drawn here, because the master has none that work loose.

The master's `boulders` sheet is not a sheet of boulders. Its cells are FRAGMENTS of a
composed mountain picture — the label set calls (4,1) "mountain slope" — so scattering
them across peak cells drew a field of identical pale right-triangles, 59 of them in one
16x14 view. And (0,1), the "grey boulder", is a flat khaki square. Both were measured on
screen before this file existed; neither is a rock you can put down anywhere.

So these are drawn. Three things make them read as stone rather than as blobs:

  LIGHT FROM THE NORTH-WEST, the same direction as the hillshade in mb_draw_relief().
  A lit top-left facet, a mid body, a dark bottom-right — nothing else is needed at this
  size, and getting it consistent with the ground shading is what stops an outcrop
  looking pasted on.

  ANGULAR SILHOUETTES. Stone breaks along planes, so every outline here is made of
  straight runs meeting at corners, never a circle. The bushes and trees in the master
  are round; rock has to be the other thing or it reads as another shrub.

  NO TWO ALIKE. Four cells, each a different mass and a different break, so a scree
  slope is a scatter and not a stamp.
"""
from PIL import Image

TS = 8

# Rock greys, from the master so an outcrop belongs to the same world as the tilesets.
LIT   = (194, 195, 199)
BODY  = (95, 87, 79)
DARK  = (29, 43, 83)
SNOW  = (255, 241, 232)


def _im():
    im = Image.new("RGBA", (TS, TS), (0, 0, 0, 0))
    return im, im.load()


def _mass(rows, snow=False):
    """`rows` gives (x0, x1) inclusive spans per row, top down — the silhouette.

    Shading is derived from the silhouette rather than drawn: the top row and the leftmost
    pixel of each row catch the light, the bottom row and the rightmost pixel fall into
    shadow, everything else is body. That keeps the light direction correct for any shape
    without hand-placing a single highlight.
    """
    im, px = _im()
    y0 = TS - len(rows)
    for i, span in enumerate(rows):
        if span is None:
            continue
        x0, x1 = span
        y = y0 + i
        for x in range(x0, x1 + 1):
            px[x, y] = BODY + (255,)
        px[x1, y] = DARK + (255,)
        px[x0, y] = LIT + (255,)
    # the lit crown: the whole top run, plus the row under it where the mass widens
    top = rows[0]
    if top:
        for x in range(top[0], top[1] + 1):
            px[x, y0] = (SNOW if snow else LIT) + (255,)
    # the shadow it casts on itself along the bottom
    bot = rows[-1]
    if bot:
        for x in range(bot[0], bot[1] + 1):
            px[x, TS - 1] = DARK + (255,)
    return im


def boulder():
    """One big stone, wider at the base — the thing a person walks around."""
    return _mass([(2, 5), (1, 6), (1, 6), (2, 6)])


def outcrop():
    """Rock breaking the surface: a low ridge with a step in it, so a line of them along
    a scarp reads as broken ground rather than as a row of lumps."""
    return _mass([None, (4, 6), (2, 6), (1, 5), (1, 4)])


def crag(snow=True):
    """A high crag, snow on the crown. This is the one that goes on peaks, and the snow
    is what ties it to the snowline the relief pass draws on the ground around it."""
    return _mass([(3, 4), (2, 5), (1, 6), (0, 6)], snow=snow)


def scree():
    """Loose stone: three small angular chips, not one mass. Scattered thinly this is
    what makes a slope look like it is shedding rock."""
    im, px = _im()
    for (x, y) in ((1, 5), (2, 5), (2, 4), (5, 6), (6, 6), (4, 2), (5, 2), (5, 3)):
        px[x, y] = BODY + (255,)
    for (x, y) in ((1, 4), (5, 5), (4, 1)):
        px[x, y] = LIT + (255,)
    for (x, y) in ((3, 6), (7, 7), (6, 4)):
        px[x, y] = DARK + (255,)
    return im


CELLS = (boulder, outcrop, crag, scree)


def build():
    sheet = Image.new("RGBA", (len(CELLS) * TS, TS), (0, 0, 0, 0))
    for i, fn in enumerate(CELLS):
        sheet.paste(fn(), (i * TS, 0))
    return sheet
