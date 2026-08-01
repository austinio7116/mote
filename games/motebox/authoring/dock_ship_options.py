#!/usr/bin/env python3
"""Candidate dock and ship sprites, for choosing from.

Nothing here ships. It imports the real helpers and palette out of build_sprites, so a
candidate that gets picked moves across verbatim.

    python3 authoring/dock_ship_options.py          # -> /tmp/motebox_docks.png
                                                    #    /tmp/motebox_ships.png

DOCKS are 8x14 town-sheet cells drawn six pixels above their tile, like every other
building, and they sit on a shore: the sheet renders them with water on one side.

SHIPS are 8x8 and drawn on open water where a unit would be, so they are shown on water
and at actual size in all five kingdom colours — a sail is where the banner shows at sea.
"""
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_sprites import (H, W, WALL, WALL_SHADE, TIMBER, BROWN, DKGREY, LTGREY, WHITE,
                           YELLOW, PEACH, NAVY, BLUE, SLATE, RED, BLACK, _blank, _rect,
                           _door, _window, BANNERS)

DECK = (140, 96, 66)          # a mid plank tone: the current dock is one flat brown
DECK_LIT = (186, 138, 96)
PILE = (86, 66, 52)
ROPE = (198, 176, 140)


def _blank8():
    im = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
    return im, im.load()


def _px(px, x, y, c):
    if 0 <= x < 8 and 0 <= y < 8:
        px[x, y] = c + (255,)


# ------------------------------------------------------------------ docks ----

def dock_a(bn):
    """A — the one in the game now: decking, three piles and a moored sail."""
    im, px = _blank()
    _rect(px, 0, 9, W - 1, 10, BROWN)
    _rect(px, 0, 9, W - 1, 9, PEACH)
    for x in (1, 4, 6):
        _rect(px, x, 11, x, H - 1, DKGREY)
    _rect(px, 2, 6, 2, 8, BROWN)
    _rect(px, 3, 6, 5, 8, WHITE)
    px[3, 6] = LTGREY + (255,)
    return im


def dock_b(bn):
    """B — JETTY AND BOLLARDS. Planked decking with visible boards, two mooring posts with a
    rope slung between them, and piles going down into the water. No boat: the boat is a
    thing that comes and goes now."""
    roof, shade, ridge = bn
    im, px = _blank()
    _rect(px, 0, 8, W - 1, 8, DECK_LIT)               # the deck edge catches the light
    _rect(px, 0, 9, W - 1, 10, DECK)
    for x in range(0, W, 2):                          # the boards
        _rect(px, x, 9, x, 10, PILE)
    for x in (1, 4, 7):
        _rect(px, x, 11, x, H - 1, PILE)              # piles into the water
    _rect(px, 1, 5, 1, 7, TIMBER)                     # two bollards
    _rect(px, 6, 5, 6, 7, TIMBER)
    _rect(px, 2, 5, 5, 5, ROPE)                       # and a rope between them
    return im


def dock_c(bn):
    """C — A CRANE. The same jetty with a derrick over the water: a boom, a rope and a crate
    hanging off it. The one silhouette that says GOODS rather than boats."""
    roof, shade, ridge = bn
    im, px = _blank()
    _rect(px, 0, 8, W - 1, 8, DECK_LIT)
    _rect(px, 0, 9, W - 1, 10, DECK)
    for x in (1, 4, 7):
        _rect(px, x, 11, x, H - 1, PILE)
    _rect(px, 1, 2, 1, 7, TIMBER)                     # the mast of the derrick
    _rect(px, 2, 2, 5, 2, TIMBER)                     # the boom
    _rect(px, 5, 3, 5, 4, ROPE)                       # the fall
    _rect(px, 4, 5, 6, 7, BROWN)                      # a crate on the hook
    px[4, 5] = DECK_LIT + (255,)
    return im


def dock_d(bn):
    """D — HARBOUR HUT. A small building on the quay with a lamp on a post, in the kingdom's
    colours — the only dock that shows whose harbour it is."""
    roof, shade, ridge = bn
    im, px = _blank()
    _rect(px, 0, 8, W - 1, 8, DECK_LIT)
    _rect(px, 0, 9, W - 1, 10, DECK)
    for x in (1, 4, 7):
        _rect(px, x, 11, x, H - 1, PILE)
    _rect(px, 2, 3, 5, 3, ridge)                      # the hut, roofed in the banner colour
    _rect(px, 1, 4, 6, 5, roof)
    _rect(px, 4, 4, 6, 5, shade)
    _rect(px, 2, 6, 5, 7, WALL)
    _door(px, 3, 6, BROWN, TIMBER)
    _rect(px, 7, 4, 7, 7, TIMBER)                     # a lamp post at the head of the quay
    px[7, 3] = YELLOW + (255,)
    return im


def dock_e(bn):
    """E — STONE QUAY. Not timber at all: a masonry wall with steps down to the water, iron
    rings and a bollard. Reads as a harbour a city built rather than a village jetty."""
    im, px = _blank()
    _rect(px, 0, 7, W - 1, 7, LTGREY)
    _rect(px, 0, 8, W - 1, 11, SLATE)
    for y in (9, 11):                                 # courses
        _rect(px, 0, y, W - 1, y, (104, 94, 126))
    _rect(px, 5, 8, 7, 8, LTGREY)                     # steps down at one end
    _rect(px, 6, 9, 7, 9, LTGREY)
    _rect(px, 7, 10, 7, 10, LTGREY)
    _rect(px, 1, 5, 1, 6, DKGREY)                     # a bollard
    px[1, 5] = LTGREY + (255,)
    _rect(px, 3, 6, 3, 6, ROPE)                       # rings
    _rect(px, 0, 12, W - 1, H - 1, (36, 62, 104))     # the water it stands in
    return im


def dock_f(bn):
    """F — WORKING QUAY. Decking, barrels, a stack of crates and a net on a frame: a place
    where a catch is landed. Busiest of the six."""
    im, px = _blank()
    _rect(px, 0, 8, W - 1, 8, DECK_LIT)
    _rect(px, 0, 9, W - 1, 10, DECK)
    for x in (1, 4, 7):
        _rect(px, x, 11, x, H - 1, PILE)
    _rect(px, 0, 5, 1, 7, BROWN)                      # two barrels
    _rect(px, 0, 5, 1, 5, DECK_LIT)
    _rect(px, 2, 6, 3, 7, (150, 120, 80))             # a crate
    _rect(px, 5, 4, 5, 7, TIMBER)                     # a net on a drying frame
    _rect(px, 7, 4, 7, 7, TIMBER)
    for y in range(5, 8):
        _rect(px, 6, y, 6, y, ROPE if y % 2 else (120, 130, 120))
    _rect(px, 5, 4, 7, 4, ROPE)
    return im


# TOP DOWN, not in elevation. Every other building on the town sheet is drawn as a front
# with a roof, because a house is a thing you see the side of. A dock is not: it is a flat
# structure lying on the water, and drawn side-on it reads as a table. These occupy the
# BOTTOM EIGHT ROWS of the 8x14 cell — the tile itself — and leave the six rows above it
# empty, so a dock lies on the ground the way a road does instead of standing above it.
#
# Water is drawn to the SOUTH by convention here. In the game the sheet can carry four
# rotations and the draw picks by which neighbour is water.

PLANK  = (150, 106, 72)
PLANK2 = (128, 88, 58)
PLANK_LIT = (188, 142, 98)
GAP_C  = (86, 62, 44)
POST   = (74, 56, 42)
POST_LIT = (120, 96, 72)

T = H - 8          # first row of the tile itself


def _planks_v(px, x0, x1, y0, y1):
    """Boards running north-south, alternating tone, with a dark gap between each."""
    for x in range(x0, x1 + 1):
        c = PLANK if (x - x0) % 2 == 0 else PLANK2
        _rect(px, x, y0, x, y1, c)
    for x in range(x0 + 2, x1, 3):
        _rect(px, x, y0, x, y1, GAP_C)


def _planks_h(px, x0, x1, y0, y1):
    for y in range(y0, y1 + 1):
        c = PLANK if (y - y0) % 2 == 0 else PLANK2
        _rect(px, x0, y, x1, y, c)


def dock_a(bn):
    """A — PLANKS OUT. Boards running from the shore into the water, two mooring posts at the
    landward end and the deck ending square over open water."""
    im, px = _blank()
    _planks_v(px, 2, 5, T, H - 1)
    _rect(px, 2, T, 5, T, PLANK_LIT)                 # the landward edge catches the light
    _px = px
    px[1, T + 1] = POST + (255,); px[1, T] = POST_LIT + (255,)
    px[6, T + 1] = POST + (255,); px[6, T] = POST_LIT + (255,)
    return im


def dock_b(bn):
    """B — T-PIER. The same walk out, with a crossbar at the far end to tie up along — twice
    the mooring for one more row of pixels."""
    im, px = _blank()
    _planks_v(px, 3, 4, T, H - 3)
    _planks_h(px, 0, 7, H - 2, H - 1)
    _rect(px, 0, H - 2, 7, H - 2, PLANK_LIT)
    px[0, H - 1] = POST + (255,); px[7, H - 1] = POST + (255,)
    px[3, T] = PLANK_LIT + (255,); px[4, T] = PLANK_LIT + (255,)
    return im


def dock_c(bn):
    """C — L-JETTY. Out and then along, so the sheltered water is inside the elbow. The most
    harbour-like shape at this size."""
    im, px = _blank()
    _planks_v(px, 1, 2, T, H - 1)
    _planks_h(px, 1, 7, H - 2, H - 1)
    _rect(px, 1, T, 2, T, PLANK_LIT)
    px[7, H - 3] = POST + (255,)
    px[5, H - 3] = POST + (255,)
    return im


def dock_d(bn):
    """D — PIER AND A MOORED BOAT. The jetty with a hull tied alongside, seen from above:
    a pointed oval with a thwart across it."""
    roof, shade, ridge = bn
    im, px = _blank()
    _planks_v(px, 1, 3, T, H - 1)
    _rect(px, 1, T, 3, T, PLANK_LIT)
    px[0, T + 1] = POST + (255,)
    _rect(px, 5, T + 2, 6, H - 2, BROWN)             # the hull
    px[5, T + 1] = PLANK2 + (255,); px[6, H - 1] = PLANK2 + (255,)
    px[5, T + 3] = (186, 138, 96) + (255,)           # a thwart
    px[6, T + 3] = (186, 138, 96) + (255,)
    px[4, T + 4] = ROPE + (255,)                     # the painter, to the pier
    return im


def dock_e(bn):
    """E — STONE QUAY. A flagged platform along the water rather than a finger into it, with
    a dark edge where it drops to the water and two rings."""
    im, px = _blank()
    _rect(px, 0, T + 2, 7, H - 2, SLATE)
    for x in range(0, 8, 3):                         # flag joints
        _rect(px, x, T + 2, x, H - 2, (104, 94, 126))
    _rect(px, 0, T + 2, 7, T + 2, LTGREY)            # the landward edge, lit
    _rect(px, 0, H - 1, 7, H - 1, (30, 46, 78))      # the drop to the water
    px[2, H - 2] = DKGREY + (255,); px[5, H - 2] = DKGREY + (255,)
    return im


def dock_f(bn):
    """F — WORKING DECK. A square of decking with barrels, a crate and a coil of rope on it:
    a place where a catch is landed rather than a place to walk to."""
    im, px = _blank()
    _planks_v(px, 0, 7, T + 1, H - 1)
    _rect(px, 0, T + 1, 7, T + 1, PLANK_LIT)
    _rect(px, 0, T + 2, 1, T + 3, BROWN)             # barrels, from above: two dark rings
    px[0, T + 2] = (186, 138, 96) + (255,)
    px[1, T + 3] = (186, 138, 96) + (255,)
    _rect(px, 6, T + 2, 7, T + 3, (150, 120, 80))    # a crate
    px[3, H - 2] = ROPE + (255,); px[4, H - 2] = ROPE + (255,)   # a coil of rope
    px[3, H - 3] = ROPE + (255,)
    return im


def dock_g(bn):
    """G — SLIPWAY. A ramp of cross-beams running down into the water with a hull drawn up on
    it: a place ships are BUILT, which no other tile says."""
    im, px = _blank()
    for y in range(T + 1, H):                        # the ramp: beams across, getting darker
        c = PLANK if (y - T) % 2 else PLANK2
        _rect(px, 1, y, 6, y, c)
    _rect(px, 1, T + 1, 6, T + 1, PLANK_LIT)
    _rect(px, 3, T + 2, 4, H - 2, BROWN)             # a hull under construction
    px[3, T + 2] = (186, 138, 96) + (255,)
    px[4, T + 2] = (186, 138, 96) + (255,)
    px[3, H - 3] = GAP_C + (255,); px[4, H - 3] = GAP_C + (255,)  # open ribs
    return im


def dock_h(bn):
    """H — HARBOUR ARM. A stone breakwater curving out with a lamp on the end, in the
    kingdom's colour. The only one that shows whose harbour it is."""
    roof, shade, ridge = bn
    im, px = _blank()
    _rect(px, 0, T + 3, 4, T + 4, SLATE)
    _rect(px, 4, T + 2, 5, T + 4, SLATE)
    _rect(px, 5, T + 1, 6, T + 3, SLATE)
    _rect(px, 0, T + 3, 4, T + 3, LTGREY)
    px[5, T + 1] = LTGREY + (255,)
    px[6, T] = roof + (255,)                         # the lamp
    px[6, T - 1] = YELLOW + (255,)
    _rect(px, 0, T + 5, 6, T + 5, (30, 46, 78))      # its shadow on the water
    return im


DOCKS = [("A planks", dock_a), ("B T-pier", dock_b), ("C L-jetty", dock_c),
         ("D + boat", dock_d), ("E stone", dock_e), ("F deck", dock_f),
         ("G slipway", dock_g), ("H arm", dock_h)]


# ------------------------------------------------------------------ ships ----
# 8x8, drawn on open water where a person would be. The SAIL carries the banner colour so a
# ship at sea is readable as somebody's ship, which is the whole point of drawing one.

def ship_a(bn):
    """A — ROWING BOAT. A hull, an oar and a figure in it. The smallest thing that reads as
    a boat with somebody aboard."""
    roof, shade, ridge = bn
    im, px = _blank8()
    _rect(px, 1, 5, 6, 6, BROWN)                      # hull
    _rect(px, 1, 5, 6, 5, (186, 138, 96))
    _px(px, 0, 5, PILE); _px(px, 7, 5, PILE)          # bow and stern
    _px(px, 3, 3, PEACH); _px(px, 3, 4, roof)         # a rower: head and body
    _rect(px, 4, 4, 5, 4, TIMBER)                     # the oar
    _px(px, 6, 5, ROPE)
    return im


def ship_b(bn):
    """B — SINGLE-MAST COG. A square sail in the kingdom's colour on one mast. The plainest
    ship, and the one that reads at the smallest size."""
    roof, shade, ridge = bn
    im, px = _blank8()
    _rect(px, 3, 0, 3, 5, TIMBER)                     # mast
    _rect(px, 1, 1, 6, 4, roof)                       # the sail, banner-coloured
    _rect(px, 4, 1, 6, 4, shade)
    _rect(px, 1, 1, 1, 4, ridge)
    _rect(px, 0, 5, 7, 6, BROWN)                      # hull
    _rect(px, 1, 5, 6, 5, (186, 138, 96))
    _rect(px, 1, 7, 6, 7, (36, 62, 104))              # its own shadow on the water
    return im


def ship_c(bn):
    """C — CARAVEL. Two masts, a triangular lateen sail aft and a square sail forward: the
    ship a town builds when it means to cross something."""
    roof, shade, ridge = bn
    im, px = _blank8()
    _rect(px, 2, 0, 2, 5, TIMBER)
    _rect(px, 5, 1, 5, 5, TIMBER)
    _rect(px, 0, 1, 1, 4, roof)                       # square sail forward
    _px(px, 0, 1, ridge)
    _px(px, 6, 2, roof); _rect(px, 6, 3, 7, 4, roof)  # lateen aft
    _px(px, 7, 4, shade)
    _rect(px, 0, 5, 7, 6, BROWN)
    _rect(px, 1, 5, 6, 5, (186, 138, 96))
    _rect(px, 2, 7, 5, 7, (36, 62, 104))
    return im


def ship_d(bn):
    """D — LONGSHIP. A striped sail, a low hull and a carved prow. The most aggressive
    outline; would read as a raider as much as an explorer."""
    roof, shade, ridge = bn
    im, px = _blank8()
    _rect(px, 3, 0, 3, 4, TIMBER)
    for y in range(1, 5):                             # a striped sail
        _rect(px, 1, y, 6, y, roof if y % 2 else WHITE)
    _px(px, 0, 3, TIMBER); _px(px, 0, 2, TIMBER)      # the prow, rising
    _rect(px, 0, 5, 7, 5, BROWN)
    _rect(px, 1, 6, 6, 6, PILE)
    _px(px, 7, 4, TIMBER)
    _rect(px, 2, 7, 5, 7, (36, 62, 104))
    return im


def ship_e(bn):
    """E — FISHING SMACK. A small hull with a net over the side and a low sail: what puts out
    from a quay to fish rather than to travel."""
    roof, shade, ridge = bn
    im, px = _blank8()
    _rect(px, 4, 1, 4, 4, TIMBER)
    _rect(px, 2, 2, 3, 4, WHITE)                      # a small, dirty sail
    _px(px, 2, 2, LTGREY)
    _rect(px, 1, 5, 6, 6, BROWN)
    _rect(px, 1, 5, 6, 5, (186, 138, 96))
    _px(px, 0, 6, (120, 130, 120)); _px(px, 1, 7, (120, 130, 120))   # the net, trailing
    _px(px, 6, 4, roof)                               # a fisher aboard
    _rect(px, 2, 7, 5, 7, (36, 62, 104))
    return im


def ship_f(bn):
    """F — STEAMER. A funnel with smoke and no sail at all — the later-age form, for a
    kingdom that has reached steam. Could replace whichever sail ship is chosen once the
    tech is known, the way a campfire becomes a street light."""
    roof, shade, ridge = bn
    im, px = _blank8()
    _rect(px, 3, 2, 4, 4, DKGREY)                     # funnel
    _rect(px, 3, 2, 4, 2, roof)                       # a banded top in the banner colour
    _px(px, 2, 1, LTGREY); _px(px, 1, 0, (170, 170, 178))            # smoke
    _rect(px, 0, 5, 7, 6, SLATE)                      # an iron hull
    _rect(px, 1, 5, 6, 5, LTGREY)
    _rect(px, 5, 3, 6, 4, WALL)                       # a deckhouse
    _rect(px, 1, 7, 6, 7, (36, 62, 104))
    return im


SHIPS = [("A rowboat", ship_a), ("B cog", ship_b), ("C caravel", ship_c),
         ("D longship", ship_d), ("E smack", ship_e), ("F steamer", ship_f)]


# ------------------------------------------------------------------ sheet ----

SEA = (41, 92, 156)
SAND = (214, 190, 140)
GRASS = (58, 106, 62)


def sheet_docks(path):
    S, GAP = 9, 10
    cw = W * S + GAP
    out = Image.new("RGB", (len(DOCKS) * cw, H * S + 22), (38, 42, 58))
    d = ImageDraw.Draw(out)
    for i, (label, fn) in enumerate(DOCKS):
        bg = Image.new("RGB", (W * S, H * S), SEA)          # water below, shore above
        bd = ImageDraw.Draw(bg)
        bd.rectangle([0, 0, W * S, (H - 8) * S - 1], fill=GRASS)
        bd.rectangle([0, (H - 8) * S, W * S, (H - 7) * S - 1], fill=SAND)
        im = fn(BANNERS[0]).resize((W * S, H * S), Image.NEAREST)
        bg.paste(im, (0, 0), im)
        out.paste(bg, (i * cw + GAP // 2, 18))
        d.text((i * cw + GAP // 2, 4), label, fill=(226, 230, 246))
    out.save(path)
    print("wrote", path)


def sheet_ships(path):
    S, GAP = 12, 10
    cw = 8 * S + GAP
    rowh = 8 * S + 22
    out = Image.new("RGB", (len(SHIPS) * cw, rowh + 8 * 5 + 40), (38, 42, 58))
    d = ImageDraw.Draw(out)
    for i, (label, fn) in enumerate(SHIPS):
        bg = Image.new("RGB", (8 * S, 8 * S), SEA)
        im = fn(BANNERS[0]).resize((8 * S, 8 * S), Image.NEAREST)
        bg.paste(im, (0, 0), im)
        out.paste(bg, (i * cw + GAP // 2, 18))
        d.text((i * cw + GAP // 2, 4), label, fill=(226, 230, 246))
    # AT ACTUAL SIZE, on water, in all five kingdom colours — the size it is judged at
    d.text((6, rowh + 4), "actual size, five kingdoms:", fill=(180, 186, 210))
    strip = Image.new("RGB", (len(SHIPS) * 10, 5 * 10), SEA)
    for r, bn in enumerate(BANNERS):
        for i, (_l, fn) in enumerate(SHIPS):
            im = fn(bn)
            strip.paste(im, (i * 10 + 1, r * 10 + 1), im)
    z = 4
    out = out.crop((0, 0, out.width, rowh + 20 + strip.height * z))
    out.paste(strip.resize((strip.width * z, strip.height * z), Image.NEAREST), (6, rowh + 18))
    out.save(path)
    print("wrote", path)


if __name__ == "__main__":
    sheet_docks("/tmp/motebox_docks.png")
    sheet_ships("/tmp/motebox_ships.png")
