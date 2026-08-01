#!/usr/bin/env python3
"""Draw a contact sheet of candidate house variants so a human can choose.

Nothing here ships. It imports the real helpers and the real palette out of
build_sprites so a candidate that gets picked can be moved across verbatim.

Every candidate is 8x14 with the anchor six pixels above its tile, and every one keeps
the game's single light direction: left slope lit with a ridge highlight, right slope
shaded. That is not style, it is what makes a street read as one lit scene.
"""
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_sprites as B
from build_sprites import (H, W, WALL, WALL_SHADE, TIMBER, BROWN, DKGREY, LTGREY,
                           WHITE, YELLOW, _blank, _rect, _door, _window, _wall,
                           _pitched, BANNERS)

OUT = os.environ.get("OUT", "/tmp/motebox_houses.png")


def _slope(px, x0, x1, top, roof, shade, ridge):
    """A pitched roof over x0..x1 only, apex centred, 45 degrees. Returns the first wall row."""
    span = x1 - x0 + 1
    rows = (span + 1) // 2
    cx = (x0 + x1) / 2.0
    for i in range(rows):
        a, b = int(cx - 0.5 - i), int(cx + 0.5 + i)
        a, b = max(a, x0), min(b, x1)
        y = top + i
        _rect(px, a, y, b, y, roof)
        mid = (a + b) // 2
        _rect(px, mid + 1, y, b, y, shade)
        px[a, y] = ridge + (255,)
    return top + rows


def _chimney(px, x, top, rows=2):
    _rect(px, x, top, x, top + rows - 1, DKGREY)
    px[x, top] = LTGREY + (255,)


# --------------------------------------------------------------- candidates ----

def a_gable(bn):
    """A — the existing tall house, for reference. Full-width gable, chimney, four windows."""
    roof, shade, ridge = bn
    im, px = _blank()
    body = _pitched(px, 2, roof, shade, ridge)
    _wall(px, body, WALL, WALL_SHADE, TIMBER)
    _door(px, 3, body + 4, BROWN, TIMBER)
    _window(px, 1, body + 1); _window(px, 6, body + 1)
    _window(px, 1, body + 4); _window(px, 6, body + 4)
    _chimney(px, 6, 2)
    return im


def b_lplan(bn):
    """B — L-PLAN. A pitched range on the right, a lower wing on the left whose roof falls
    away one row at a time. The outline steps down."""
    roof, shade, ridge = bn
    im, px = _blank()
    body = _slope(px, 3, 7, 2, roof, shade, ridge)
    _rect(px, 3, body, 7, H - 1, WALL)
    px[7, body] = TIMBER + (255,)
    _window(px, 5, body + 1); _window(px, 5, body + 4)
    px[2, 6] = ridge + (255,)
    _rect(px, 1, 7, 2, 7, roof); px[1, 7] = ridge + (255,)
    _rect(px, 0, 8, 2, 8, shade)
    _rect(px, 0, 9, 2, H - 1, WALL)
    px[0, 9] = TIMBER + (255,)
    _door(px, 1, 11, BROWN, TIMBER)
    _rect(px, 0, H - 1, 7, H - 1, WALL_SHADE)
    return im


def c_dormer(bn):
    """C — DORMER. Cottage footprint with a small gable breaking out of the front slope,
    plus a chimney at the ridge, so the roofline is broken rather than a clean triangle."""
    roof, shade, ridge = bn
    im, px = _blank()
    _chimney(px, 6, 1, 3)
    body = _pitched(px, 3, roof, shade, ridge)
    _rect(px, 1, 4, 2, 4, ridge)
    _rect(px, 1, 5, 2, 6, WALL)
    _window(px, 1, 5)
    _wall(px, body, WALL, WALL_SHADE, TIMBER)
    _door(px, 3, body + 2, BROWN, TIMBER)
    _window(px, 6, body + 1)
    return im


def d_leanto(bn):
    """D — NARROW WITH A LEAN-TO. Six pixels of pitched house and a single-slope outhouse
    on the shaded side: the shape a town gets when it runs out of frontage."""
    roof, shade, ridge = bn
    im, px = _blank()
    body = _slope(px, 0, 5, 3, roof, shade, ridge)
    _rect(px, 0, body, 5, H - 1, WALL)
    _rect(px, 5, body, 5, H - 1, WALL_SHADE)
    px[0, body] = TIMBER + (255,)
    _window(px, 1, body + 1); _window(px, 3, body + 1)
    _door(px, 2, body + 4, BROWN, TIMBER)
    px[6, 8] = roof + (255,); px[7, 8] = shade + (255,)
    _rect(px, 6, 9, 7, H - 1, WALL_SHADE)
    px[6, 10] = TIMBER + (255,)
    _rect(px, 0, H - 1, 7, H - 1, TIMBER)
    return im


def e_hip(bn):
    """E — HIPPED ROOF. Slopes on all four sides, so the top is a short ridge instead of a
    point and the roof has no gable end. Reads as a squarer, more solid house."""
    roof, shade, ridge = bn
    im, px = _blank()
    _rect(px, 2, 2, 5, 2, roof); _rect(px, 4, 2, 5, 2, shade); px[2, 2] = ridge + (255,)
    _rect(px, 1, 3, 6, 3, roof); _rect(px, 4, 3, 6, 3, shade); px[1, 3] = ridge + (255,)
    _rect(px, 0, 4, 7, 4, roof); _rect(px, 4, 4, 7, 4, shade); px[0, 4] = ridge + (255,)
    _rect(px, 0, 5, 7, 5, shade)
    _wall(px, 6, WALL, WALL_SHADE, TIMBER)
    _window(px, 1, 7); _window(px, 6, 7)
    _window(px, 1, 10); _window(px, 6, 10)
    _door(px, 3, 10, BROWN, TIMBER)
    _chimney(px, 5, 1, 2)
    return im


def f_crossgable(bn):
    """F — CROSS GABLE. A small gable facing the street set into a wide main roof — a T in
    plan. Two roof directions at once, which no other variant has."""
    roof, shade, ridge = bn
    im, px = _blank()
    _rect(px, 0, 4, 7, 4, roof); _rect(px, 4, 4, 7, 4, shade); px[0, 4] = ridge + (255,)
    _rect(px, 0, 5, 7, 5, shade)
    px[3, 2] = ridge + (255,); px[4, 2] = shade + (255,)
    _rect(px, 2, 3, 5, 3, roof); px[2, 3] = ridge + (255,); _rect(px, 4, 3, 5, 3, shade)
    _rect(px, 2, 4, 5, 4, roof); px[2, 4] = ridge + (255,); _rect(px, 4, 4, 5, 4, shade)
    _rect(px, 2, 5, 5, 5, roof); px[2, 5] = ridge + (255,); _rect(px, 4, 5, 5, 5, shade)
    _rect(px, 2, 6, 5, 6, shade)
    _wall(px, 6, WALL, WALL_SHADE, TIMBER)
    _window(px, 1, 8); _window(px, 6, 8)
    _door(px, 3, 10, BROWN, TIMBER)
    return im


def g_tower(bn):
    """G — TOWER HOUSE. Three storeys on five pixels with a steep roof, and a one-storey
    annexe beside it. The tallest silhouette available at this size."""
    roof, shade, ridge = bn
    im, px = _blank()
    px[2, 0] = ridge + (255,); px[3, 0] = shade + (255,)
    _rect(px, 1, 1, 4, 1, roof); px[1, 1] = ridge + (255,); _rect(px, 3, 1, 4, 1, shade)
    _rect(px, 0, 2, 5, 2, roof); px[0, 2] = ridge + (255,); _rect(px, 3, 2, 5, 2, shade)
    _rect(px, 0, 3, 5, 3, shade)
    _rect(px, 1, 4, 4, H - 1, WALL)
    px[1, 4] = TIMBER + (255,); px[4, 4] = TIMBER + (255,)
    _window(px, 2, 5); _window(px, 2, 8)
    _door(px, 2, 11, BROWN, TIMBER)
    _rect(px, 5, 9, 7, 9, roof); px[5, 9] = ridge + (255,); px[7, 9] = shade + (255,)
    _rect(px, 5, 10, 7, H - 1, WALL_SHADE)
    _window(px, 6, 11, YELLOW)
    _rect(px, 0, H - 1, 7, H - 1, WALL_SHADE)
    return im


def h_longhouse(bn):
    """H — LONGHOUSE. A wide roof carried nearly to the ground with the door in the gable
    end: a farm building, low and heavy, no upper storey at all."""
    roof, shade, ridge = bn
    im, px = _blank()
    body = _pitched(px, 6, roof, shade, ridge)
    _rect(px, 0, 5, 7, 5, shade)
    _rect(px, 0, body, 7, H - 1, WALL)
    _rect(px, 0, body, 0, H - 1, TIMBER)
    _rect(px, 7, body, 7, H - 1, TIMBER)
    _door(px, 3, body, BROWN, TIMBER)
    _window(px, 1, 11); _window(px, 6, 11)
    _rect(px, 0, H - 1, 7, H - 1, WALL_SHADE)
    return im


def i_shop(bn):
    """I — SHOPFRONT. A house over an open ground floor with a canopy and a counter: the
    building a market street is made of. The dark opening under the canopy is the giveaway."""
    roof, shade, ridge = bn
    im, px = _blank()
    body = _pitched(px, 2, roof, shade, ridge)
    _rect(px, 0, body, 7, body + 3, WALL)
    px[0, body] = TIMBER + (255,); px[7, body] = TIMBER + (255,)
    _window(px, 2, body + 1); _window(px, 5, body + 1)
    _rect(px, 0, body + 4, 7, body + 4, TIMBER)          # the canopy
    _rect(px, 1, body + 4, 6, body + 4, ridge)
    _rect(px, 1, body + 5, 6, H - 2, DKGREY)             # the open front, in shadow
    _rect(px, 0, body + 5, 0, H - 1, WALL)
    _rect(px, 7, body + 5, 7, H - 1, WALL_SHADE)
    _rect(px, 1, H - 1, 6, H - 1, BROWN)                 # a counter across it
    return im


def j_round(bn):
    """J — ROUNDHOUSE. A conical thatch with its corners knocked off over a round wall: no
    straight line anywhere, which is the opposite of every other variant."""
    roof, shade, ridge = bn
    im, px = _blank()
    px[3, 1] = ridge + (255,); px[4, 1] = shade + (255,)
    _rect(px, 2, 2, 5, 2, roof); px[2, 2] = ridge + (255,); _rect(px, 4, 2, 5, 2, shade)
    _rect(px, 1, 3, 6, 3, roof); px[1, 3] = ridge + (255,); _rect(px, 4, 3, 6, 3, shade)
    _rect(px, 1, 4, 6, 4, roof); px[1, 4] = ridge + (255,); _rect(px, 4, 4, 6, 4, shade)
    _rect(px, 0, 5, 7, 5, roof); px[0, 5] = ridge + (255,); _rect(px, 4, 5, 7, 5, shade)
    _rect(px, 0, 6, 7, 6, shade)
    _rect(px, 1, 7, 6, H - 1, WALL)                      # a round wall: corners cut
    _rect(px, 0, 8, 0, H - 2, WALL)
    _rect(px, 7, 8, 7, H - 2, WALL_SHADE)
    _rect(px, 5, 7, 6, H - 1, WALL_SHADE)
    _door(px, 3, 10, BROWN, TIMBER)
    _window(px, 1, 8)
    _rect(px, 1, H - 1, 6, H - 1, WALL_SHADE)
    return im


def k_gambrel(bn):
    """K — GAMBREL. The roof breaks halfway and gets steeper, so it swallows most of the
    facade and the house reads as roof-with-a-door. A barn shape."""
    roof, shade, ridge = bn
    im, px = _blank()
    _rect(px, 2, 1, 5, 1, roof); px[2, 1] = ridge + (255,); _rect(px, 4, 1, 5, 1, shade)
    _rect(px, 1, 2, 6, 2, roof); px[1, 2] = ridge + (255,); _rect(px, 4, 2, 6, 2, shade)
    _rect(px, 1, 3, 6, 3, roof); px[1, 3] = ridge + (255,); _rect(px, 4, 3, 6, 3, shade)
    _rect(px, 0, 4, 7, 4, roof); px[0, 4] = ridge + (255,); _rect(px, 4, 4, 7, 4, shade)
    _rect(px, 0, 5, 7, 7, roof); _rect(px, 4, 5, 7, 7, shade)
    _rect(px, 0, 5, 0, 7, ridge)
    _rect(px, 3, 5, 4, 6, WALL)                          # a hay door high in the gable
    _window(px, 3, 6)
    _rect(px, 0, 8, 7, 8, TIMBER)
    _wall(px, 9, WALL, WALL_SHADE, TIMBER)
    _door(px, 3, 10, BROWN, TIMBER)
    _window(px, 1, 10); _window(px, 6, 10)
    return im


def l_walled(bn):
    """L — WALLED YARD. A house set back behind its own low wall with a gate in it, so the
    footprint includes ground. Two depths in one sprite."""
    roof, shade, ridge = bn
    im, px = _blank()
    body = _slope(px, 1, 7, 2, roof, shade, ridge)        # set right and back
    _rect(px, 1, body, 7, H - 3, WALL)
    px[1, body] = TIMBER + (255,); px[7, body] = TIMBER + (255,)
    _window(px, 3, body + 1); _window(px, 6, body + 1)
    _door(px, 4, H - 4, BROWN, TIMBER)
    _rect(px, 0, H - 2, 7, H - 1, LTGREY)                # the yard wall, in front
    _rect(px, 0, H - 1, 7, H - 1, DKGREY)
    _rect(px, 2, H - 2, 3, H - 1, BROWN)                 # a gate in it
    return im


def m_timber(bn):
    """M — HALF-TIMBERED. Same gable, but the wall is framed in dark timber with braces and
    a jettied upper floor overhanging the ground floor. The most decorated variant."""
    roof, shade, ridge = bn
    im, px = _blank()
    body = _pitched(px, 2, roof, shade, ridge)
    _rect(px, 0, body, 7, body + 3, WALL)                # the jettied storey, full width
    _rect(px, 0, body, 0, body + 3, TIMBER)
    _rect(px, 7, body, 7, body + 3, TIMBER)
    px[2, body + 1] = TIMBER + (255,); px[5, body + 1] = TIMBER + (255,)
    _window(px, 3, body + 1); _window(px, 4, body + 1)
    _rect(px, 0, body + 4, 7, body + 4, TIMBER)          # the bressumer it sits on
    _rect(px, 1, body + 5, 6, H - 1, WALL)               # ground floor, set back
    _rect(px, 1, body + 5, 1, H - 1, TIMBER)
    _rect(px, 6, body + 5, 6, H - 1, TIMBER)
    _door(px, 3, body + 5, BROWN, TIMBER)
    _window(px, 2, H - 3)
    _rect(px, 1, H - 1, 6, H - 1, WALL_SHADE)
    return im


def n_stair(bn):
    """N — OUTSIDE STAIR. Two dwellings in one building, the upper reached by a stair up the
    lit side. The diagonal is unique on the sheet."""
    roof, shade, ridge = bn
    im, px = _blank()
    body = _slope(px, 2, 7, 2, roof, shade, ridge)
    _rect(px, 2, body, 7, H - 1, WALL)
    px[7, body] = TIMBER + (255,)
    _window(px, 6, body + 1)
    _door(px, 4, body + 1, BROWN, TIMBER)                # the upper door
    _door(px, 4, H - 3, BROWN, TIMBER)                   # and the lower one
    _rect(px, 2, body, 2, H - 1, TIMBER)
    for i in range(4):                                   # the stair, climbing left to right
        px[i, H - 2 - i] = LTGREY + (255,)
        px[i, H - 1 - i] = DKGREY + (255,)
    _rect(px, 2, H - 1, 7, H - 1, WALL_SHADE)
    return im


CANDIDATES = [
    ("A gable*", a_gable), ("B L-plan", b_lplan), ("C dormer", c_dormer),
    ("D lean-to", d_leanto), ("E hip", e_hip), ("F cross", f_crossgable),
    ("G tower", g_tower), ("H long", h_longhouse), ("I shop", i_shop),
    ("J round", j_round), ("K gambrel", k_gambrel), ("L yard", l_walled),
    ("M timber", m_timber), ("N stair", n_stair),
]

# ------------------------------------------------------------------ sheet ----

GRASS = (58, 106, 62)
BG = (38, 42, 58)
S = 9
COLS = 7


def main():
    rows = (len(CANDIDATES) + COLS - 1) // COLS
    cw, chh = W * S + 14, H * S + 22
    out = Image.new("RGB", (COLS * cw, rows * chh + 34 + 20 * 3), BG)
    d = ImageDraw.Draw(out)
    for n, (label, fn) in enumerate(CANDIDATES):
        cx, cy = (n % COLS) * cw, (n // COLS) * chh
        im = fn(BANNERS[0])
        cell = Image.new("RGB", (W * S, H * S), GRASS)
        cell.paste(im.resize((W * S, H * S), Image.NEAREST), (0, 0),
                   im.resize((W * S, H * S), Image.NEAREST))
        out.paste(cell, (cx + 7, cy + 18))
        d.text((cx + 7, cy + 4), label, fill=(226, 230, 246))

    # AND AT ACTUAL SIZE, which is the only size that decides it: a street of every
    # candidate in each of the five kingdom colours, 8 pixels wide, as the game draws them.
    y0 = rows * chh + 8
    d.text((8, y0), "actual size, all five kingdom colours:", fill=(180, 186, 210))
    strip = Image.new("RGB", (len(CANDIDATES) * (W + 2), (H + 2) * 5), GRASS)
    for r, bn in enumerate(BANNERS):
        for n, (_, fn) in enumerate(CANDIDATES):
            im = fn(bn)
            strip.paste(im, (n * (W + 2) + 1, r * (H + 2) + 1), im)
    z = 4
    out.paste(strip.resize((strip.width * z, strip.height * z), Image.NEAREST), (8, y0 + 16))
    grown = Image.new("RGB", (out.width, y0 + 16 + strip.height * z + 12), BG)
    grown.paste(out.crop((0, 0, out.width, y0 + 16)), (0, 0))
    grown.paste(out.crop((0, y0 + 16, out.width, min(out.height, y0 + 16 + strip.height * z))),
                (0, y0 + 16))
    grown.paste(strip.resize((strip.width * z, strip.height * z), Image.NEAREST), (8, y0 + 16))
    grown.save(OUT)
    print("wrote", OUT, grown.size)


if __name__ == "__main__":
    main()
