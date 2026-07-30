"""Motebox — the settlement's own sprites, drawn here rather than carved from the master.

WHY NOT THE MASTER SHEET. Its buildings rectangle is four columns by five colour rows:
a walled face with a window, the same with the door open, a house with a pitched roof,
and a WIDE CONTINUATION meant to sit beside the third. Two usable standalone fronts,
in other words, for a ladder of six dwelling and hall tiers plus a temple, a barracks,
a tower, a barn, a mine and a dock — so most of the settlement had to share art, and
one round of this shipped cottages drawn with the continuation piece, which made towns
look like rows of fences. There is no fixing that by picking different cells; the cells
do not exist.

SO THEY ARE DRAWN, and drawn TALLER THAN THEIR TILE. A building occupies one cell of
the simulation but stands 8x14, anchored six pixels above its own tile so the roof
rises into the cell behind it. That is the single biggest change: at 8x8 every
structure is a face-on box the same size as a bush, and at 8x14 it has a roof with a
pitch, a wall with a door in it, and a silhouette you can read at a glance. Southern
buildings draw over northern ones, so a street has depth.

The palette is still PICO-8's sixteen — the same ones the tileset is drawn in — and the
roof takes the kingdom's banner colour, so a village is visibly one nation's village.
"""
from PIL import Image

W, H = 8, 14                 # 8 wide, and six pixels taller than its own tile
ANCHOR_Y = -6                # how far above the tile the sprite is drawn

BLACK  = (0, 0, 0)
NAVY   = (29, 43, 83)
PLUM   = (126, 37, 83)
DKGREEN= (0, 135, 81)
BROWN  = (171, 82, 54)
DKGREY = (95, 87, 79)
LTGREY = (194, 195, 199)
WHITE  = (255, 241, 232)
RED    = (255, 0, 77)
ORANGE = (255, 163, 0)
YELLOW = (255, 236, 39)
GREEN  = (0, 228, 54)
BLUE   = (41, 173, 255)
SLATE  = (131, 118, 156)
PINK   = (255, 119, 168)
PEACH  = (255, 204, 170)

# The five banner colours, as (roof, roof shadow, ridge highlight). A kingdom is read
# off its roofs, so the three tones have to stay in one family or the roof stops
# looking like one surface.
BANNERS = [
    (RED,    PLUM,    PINK),
    (BLUE,   NAVY,    WHITE),
    (ORANGE, BROWN,   YELLOW),
    (GREEN,  DKGREEN, YELLOW),
    (LTGREY, DKGREY,  WHITE),
]

WALL, WALL_SHADE, TIMBER = PEACH, BROWN, DKGREY
STONE, STONE_SHADE, STONE_LIT = LTGREY, DKGREY, WHITE


def _blank():
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    return im, im.load()


def _rect(px, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if 0 <= x < W and 0 <= y < H:
                px[x, y] = c + (255,)


def _pitched(px, top, roof, shade, ridge, rows=4):
    """A pitched roof starting at row `top`, widening by two a row to the full eight.

    A 45 degree pitch, which at this size is the only one that reads: the first attempt
    narrowed the apex to one pixel and widened slowly, and every building came out a
    witch's hat. Widths go 2, 4, 6, 8 and stop, so the roof is BROAD and LOW and leaves
    the wall enough rows to have a door in it.

    The left slope takes the light and the right the shade — one light direction, held
    to across every building in the game, which is what makes a street look lit rather
    than like a set of unrelated stickers.
    """
    for i in range(rows):
        y = top + i
        half = min(W // 2, i + 1)
        x0, x1 = W // 2 - half, W // 2 + half - 1
        _rect(px, x0, y, x1, y, roof)
        _rect(px, W // 2, y, x1, y, shade)      # the shaded slope
        px[x0, y] = ridge + (255,)              # a lit edge down the windward side
    return top + rows


def _wall(px, top, wall, shade, timber=None):
    """Wall from `top` to the ground, with a shadow line where it meets it."""
    _rect(px, 0, top, W - 1, H - 1, wall)
    _rect(px, 0, H - 1, W - 1, H - 1, shade)          # it sits ON something
    if timber:                                        # a timber frame, Tudor fashion
        for y in range(top, H - 1):
            px[0, y] = timber + (255,)
            px[W - 1, y] = timber + (255,)


def _door(px, cx, top, c=BROWN, lintel=None):
    _rect(px, cx, top, cx + 1, H - 2, c)
    if lintel:
        _rect(px, cx, top, cx + 1, top, lintel)


def _window(px, x, y, lit=YELLOW, frame=None):
    px[x, y] = lit + (255,)
    if frame:
        px[x, y - 1] = frame + (255,)


# ------------------------------------------------------------------ buildings ----

def cottage(banner):
    """The commonest building in the world, so it has to be the most legible one."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 4, roof, shade, ridge)          # rows 4-7
    _wall(px, body, WALL, WALL_SHADE, TIMBER)           # rows 8-13
    _door(px, 3, body + 2, BROWN, TIMBER)
    _window(px, 1, body + 1)
    _window(px, 6, body + 1)
    return im


def house(banner):
    """A storey taller than the cottage: the roof starts higher and the wall shows two
    windows, so a grown village reads as grown."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 2, roof, shade, ridge)          # a storey taller
    _wall(px, body, WALL, WALL_SHADE, TIMBER)
    _door(px, 3, body + 4, BROWN, TIMBER)
    _window(px, 1, body + 1); _window(px, 6, body + 1)
    _window(px, 1, body + 4); _window(px, 6, body + 4)  # an upper floor
    _rect(px, 6, 0, 6, 1, DKGREY)                       # a chimney
    px[6, 0] = LTGREY + (255,)
    return im


def manor(banner):
    """Two gables and a stone ground floor — the richest private house."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 1, roof, shade, ridge)          # a deep roof, rows 1-4
    _rect(px, 0, 5, W - 1, 5, shade)                    # and a bold eaves line
    _rect(px, 0, 5, 3, 5, ridge)
    _wall(px, 6, STONE, STONE_SHADE)                    # a stone ground floor
    _door(px, 3, 10, BROWN, STONE_LIT)
    _window(px, 1, 7); _window(px, 6, 7)
    _window(px, 1, 10); _window(px, 6, 10)
    _rect(px, 1, 0, 1, 1, DKGREY); px[1, 0] = LTGREY + (255,)
    return im


def hall(banner):
    """A long low mead hall: a wide roof carried down almost to the ground."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 5, roof, shade, ridge)          # broad and low: a mead hall
    _rect(px, 0, body, W - 1, body, shade)              # deep overhanging eaves
    _rect(px, 0, body, 3, body, ridge)
    _wall(px, body + 1, WALL, WALL_SHADE, TIMBER)
    _door(px, 3, body + 1, BROWN, TIMBER)
    return im


def great_hall(banner):
    """The hall grown into a stone-walled thing with a banner on it."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 3, roof, shade, ridge)
    _rect(px, 0, body, W - 1, body, shade)
    _rect(px, 0, body, 3, body, ridge)
    _wall(px, body + 1, STONE, STONE_SHADE)
    _door(px, 3, body + 2, BROWN, STONE_LIT)
    _window(px, 1, body + 1); _window(px, 6, body + 1)
    _rect(px, 1, 0, 1, 2, DKGREY)                     # the flagstaff
    _rect(px, 2, 0, 3, 1, roof)                       # and its banner
    return im


def castle(banner):
    """A crenellated keep. No pitched roof at all, which is what makes a castle read
    as a castle at eight pixels: the silhouette is battlements, not a triangle."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 3, W - 1, H - 1, STONE)
    _rect(px, 5, 3, W - 1, H - 1, STONE_SHADE)        # the shaded face
    for x in range(0, W, 2):                          # battlements
        _rect(px, x, 2, x, 2, STONE)
        _rect(px, x, 1, x, 1, STONE_LIT)
    _rect(px, 0, 4, W - 1, 4, STONE_LIT)              # a string course
    _door(px, 3, 10, NAVY, STONE_LIT)                 # the gate
    _window(px, 1, 7); _window(px, 6, 7)
    _rect(px, 4, 0, 4, 1, DKGREY)
    _rect(px, 5, 0, 6, 0, roof)                       # the standard
    return im


def barn(banner):
    """A farm building: a big roof, a big door, and hay showing inside."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 4, BROWN, DKGREY, ORANGE)
    _rect(px, 0, body, W - 1, body, DKGREY)           # eaves
    _rect(px, 0, body, 3, body, BROWN)
    _wall(px, body + 1, BROWN, DKGREY)
    _rect(px, 2, body + 1, 5, H - 2, YELLOW)          # the open bay, full of hay
    _rect(px, 2, body + 1, 5, body + 1, ORANGE)
    return im


def temple(banner):
    """Columns and a pediment. The one building that is pure stone and pure geometry."""
    im, px = _blank()
    for i in range(4):                                # pediment
        _rect(px, 3 - i, 4 - i, 4 + i, 4 - i, STONE)
        px[3 - i, 4 - i] = STONE_LIT + (255,)
    _rect(px, 0, 5, W - 1, 6, STONE)
    _rect(px, 0, 5, W - 1, 5, STONE_LIT)              # architrave
    for x in (1, 3, 5, 6):                            # columns
        _rect(px, x, 7, x, H - 2, STONE)
        px[x, 7] = STONE_LIT + (255,)
    _rect(px, 0, H - 1, W - 1, H - 1, STONE_SHADE)    # stylobate
    return im


def tower(banner):
    """A watchtower: tall, narrow, with a lit beacon so it reads at night and at
    distance. Narrow is the point — it must not be confused with a keep."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 1, 4, 6, H - 1, STONE)
    _rect(px, 4, 4, 6, H - 1, STONE_SHADE)
    _rect(px, 1, 4, 6, 4, STONE_LIT)                  # a corbel course
    for x in range(1, 7, 2):
        _rect(px, x, 3, x, 3, STONE)                  # battlements
    _rect(px, 3, 1, 4, 2, ORANGE)                     # the beacon burning on top
    _rect(px, 3, 1, 4, 1, YELLOW)
    _window(px, 2, 8); _window(px, 5, 8)
    _door(px, 3, 11, BROWN, STONE_LIT)
    return im


def barracks(banner):
    """Low, long, shuttered, with a shield hung on the front."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 5, DKGREY, NAVY, LTGREY)
    _wall(px, body, BROWN, DKGREY, TIMBER)
    _rect(px, 3, body + 2, 4, body + 3, roof)         # a shield hung on the front
    px[3, body + 2] = ridge + (255,)
    _door(px, 1, body + 2, DKGREY)
    return im


def mine(banner):
    """A timbered adit into a spoil heap. Reads as a hole in the ground, which is what
    distinguishes it from every building that stands up."""
    im, px = _blank()
    _rect(px, 0, 8, W - 1, H - 1, DKGREY)             # the spoil
    _rect(px, 0, 8, W - 1, 8, LTGREY)
    _rect(px, 2, 6, 5, 12, BLACK)                     # the adit mouth
    _rect(px, 2, 6, 5, 6, BROWN)                      # its lintel
    _rect(px, 2, 7, 2, 12, BROWN)                     # and its props
    _rect(px, 5, 7, 5, 12, BROWN)
    px[3, 9] = YELLOW + (255,)                        # a lamp inside
    return im


def woodcutter(banner):
    """A lean-to, a stack of cut logs and an axe in a block."""
    im, px = _blank()
    _rect(px, 0, 6, W - 1, 7, BROWN)                  # the lean-to roof
    _rect(px, 0, 6, W - 1, 6, ORANGE)
    _rect(px, 0, 8, 1, H - 1, BROWN)                  # its post
    for y in range(9, H - 1, 2):                      # the log stack
        _rect(px, 3, y, 6, y, BROWN)
        px[3, y] = PEACH + (255,)
        px[5, y] = PEACH + (255,)
    _rect(px, 2, 8, 2, 9, LTGREY)                     # the axe
    return im


def dock(banner):
    """A jetty with a moored boat: the only building that belongs on the water's edge,
    so its silhouette has to say so."""
    im, px = _blank()
    _rect(px, 0, 9, W - 1, 10, BROWN)                 # the decking
    _rect(px, 0, 9, W - 1, 9, PEACH)
    for x in (1, 4, 6):
        _rect(px, x, 11, x, H - 1, DKGREY)            # piles
    _rect(px, 2, 6, 2, 8, BROWN)                      # the mast
    _rect(px, 3, 6, 5, 8, WHITE)                      # and its sail
    px[3, 6] = LTGREY + (255,)
    return im


def wall_seg(banner):
    """A stretch of town wall with a walkway on top."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 6, W - 1, H - 1, STONE)
    _rect(px, 0, 9, W - 1, H - 1, STONE_SHADE)
    _rect(px, 0, 6, W - 1, 6, STONE_LIT)
    for x in range(0, W, 2):
        _rect(px, x, 5, x, 5, STONE)
    return im


def campfire(banner):
    """The founding marker: small, and shaped like a flame.

    Two goes at this were wrong in different ways. First it was a village WELL — a
    windlass over a bucket, which is what "a T over a water bowl" was. Then it was a
    campfire that filled the whole fourteen-row building cell, so a village's first fire
    stood as tall as a house; everything on this sheet is fourteen rows so that HOUSES
    can carry a roof above their footprint, and a fire has no use for them.

    And the flame itself was two stacked RECTANGLES, which is why it read as a strange
    block rather than as fire. A flame is a taper: wide and dark at the fuel, narrow and
    bright at the tip, and asymmetric so it looks like it is moving.
    """
    im, px = _blank()
    # the taper, bottom-heavy: five rows, narrowing and brightening upward
    # FOUR ROWS, and never a solid rectangle: the previous two goes drew 4x2 blocks and
    # read as a strange yellow brick. A flame is a taper with a hollow — bright at the
    # tip, hot in the middle, dark where it meets the fuel.
    px[3, 9] = YELLOW + (255,)                       # the tip
    px[2, 10] = ORANGE + (255,); px[3, 10] = YELLOW + (255,); px[4, 10] = ORANGE + (255,)
    px[2, 11] = RED + (255,); px[3, 11] = ORANGE + (255,); px[4, 11] = RED + (255,)
    px[5, 10] = RED + (255,)                         # one lick off to the side
    # the fuel: two logs, lit where the flame sits on them
    _rect(px, 1, 12, 6, 12, BROWN)
    px[1, 12] = PEACH + (255,); px[6, 12] = PEACH + (255,)
    px[3, 12] = RED + (255,)
    _rect(px, 2, 13, 5, 13, DKGREY)                  # its shadow, not a full-width bar
    return im


def cottage(banner):
    """The commonest building in the world, so it has to be the most legible one."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 4, roof, shade, ridge)          # rows 4-7
    _wall(px, body, WALL, WALL_SHADE, TIMBER)           # rows 8-13
    _door(px, 3, body + 2, BROWN, TIMBER)
    _window(px, 1, body + 1)
    _window(px, 6, body + 1)
    return im


def house(banner):
    """A storey taller than the cottage: the roof starts higher and the wall shows two
    windows, so a grown village reads as grown."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 2, roof, shade, ridge)          # a storey taller
    _wall(px, body, WALL, WALL_SHADE, TIMBER)
    _door(px, 3, body + 4, BROWN, TIMBER)
    _window(px, 1, body + 1); _window(px, 6, body + 1)
    _window(px, 1, body + 4); _window(px, 6, body + 4)  # an upper floor
    _rect(px, 6, 0, 6, 1, DKGREY)                       # a chimney
    px[6, 0] = LTGREY + (255,)
    return im


def manor(banner):
    """Two gables and a stone ground floor — the richest private house."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 1, roof, shade, ridge)          # a deep roof, rows 1-4
    _rect(px, 0, 5, W - 1, 5, shade)                    # and a bold eaves line
    _rect(px, 0, 5, 3, 5, ridge)
    _wall(px, 6, STONE, STONE_SHADE)                    # a stone ground floor
    _door(px, 3, 10, BROWN, STONE_LIT)
    _window(px, 1, 7); _window(px, 6, 7)
    _window(px, 1, 10); _window(px, 6, 10)
    _rect(px, 1, 0, 1, 1, DKGREY); px[1, 0] = LTGREY + (255,)
    return im


def hall(banner):
    """A long low mead hall: a wide roof carried down almost to the ground."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 5, roof, shade, ridge)          # broad and low: a mead hall
    _rect(px, 0, body, W - 1, body, shade)              # deep overhanging eaves
    _rect(px, 0, body, 3, body, ridge)
    _wall(px, body + 1, WALL, WALL_SHADE, TIMBER)
    _door(px, 3, body + 1, BROWN, TIMBER)
    return im


def great_hall(banner):
    """The hall grown into a stone-walled thing with a banner on it."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 3, roof, shade, ridge)
    _rect(px, 0, body, W - 1, body, shade)
    _rect(px, 0, body, 3, body, ridge)
    _wall(px, body + 1, STONE, STONE_SHADE)
    _door(px, 3, body + 2, BROWN, STONE_LIT)
    _window(px, 1, body + 1); _window(px, 6, body + 1)
    _rect(px, 1, 0, 1, 2, DKGREY)                     # the flagstaff
    _rect(px, 2, 0, 3, 1, roof)                       # and its banner
    return im


def castle(banner):
    """A crenellated keep. No pitched roof at all, which is what makes a castle read
    as a castle at eight pixels: the silhouette is battlements, not a triangle."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 3, W - 1, H - 1, STONE)
    _rect(px, 5, 3, W - 1, H - 1, STONE_SHADE)        # the shaded face
    for x in range(0, W, 2):                          # battlements
        _rect(px, x, 2, x, 2, STONE)
        _rect(px, x, 1, x, 1, STONE_LIT)
    _rect(px, 0, 4, W - 1, 4, STONE_LIT)              # a string course
    _door(px, 3, 10, NAVY, STONE_LIT)                 # the gate
    _window(px, 1, 7); _window(px, 6, 7)
    _rect(px, 4, 0, 4, 1, DKGREY)
    _rect(px, 5, 0, 6, 0, roof)                       # the standard
    return im


def barn(banner):
    """A farm building: a big roof, a big door, and hay showing inside."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 4, BROWN, DKGREY, ORANGE)
    _rect(px, 0, body, W - 1, body, DKGREY)           # eaves
    _rect(px, 0, body, 3, body, BROWN)
    _wall(px, body + 1, BROWN, DKGREY)
    _rect(px, 2, body + 1, 5, H - 2, YELLOW)          # the open bay, full of hay
    _rect(px, 2, body + 1, 5, body + 1, ORANGE)
    return im


def temple(banner):
    """Columns and a pediment. The one building that is pure stone and pure geometry."""
    im, px = _blank()
    for i in range(4):                                # pediment
        _rect(px, 3 - i, 4 - i, 4 + i, 4 - i, STONE)
        px[3 - i, 4 - i] = STONE_LIT + (255,)
    _rect(px, 0, 5, W - 1, 6, STONE)
    _rect(px, 0, 5, W - 1, 5, STONE_LIT)              # architrave
    for x in (1, 3, 5, 6):                            # columns
        _rect(px, x, 7, x, H - 2, STONE)
        px[x, 7] = STONE_LIT + (255,)
    _rect(px, 0, H - 1, W - 1, H - 1, STONE_SHADE)    # stylobate
    return im


def tower(banner):
    """A watchtower: tall, narrow, with a lit beacon so it reads at night and at
    distance. Narrow is the point — it must not be confused with a keep."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 1, 4, 6, H - 1, STONE)
    _rect(px, 4, 4, 6, H - 1, STONE_SHADE)
    _rect(px, 1, 4, 6, 4, STONE_LIT)                  # a corbel course
    for x in range(1, 7, 2):
        _rect(px, x, 3, x, 3, STONE)                  # battlements
    _rect(px, 3, 1, 4, 2, ORANGE)                     # the beacon burning on top
    _rect(px, 3, 1, 4, 1, YELLOW)
    _window(px, 2, 8); _window(px, 5, 8)
    _door(px, 3, 11, BROWN, STONE_LIT)
    return im


def barracks(banner):
    """Low, long, shuttered, with a shield hung on the front."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 5, DKGREY, NAVY, LTGREY)
    _wall(px, body, BROWN, DKGREY, TIMBER)
    _rect(px, 3, body + 2, 4, body + 3, roof)         # a shield hung on the front
    px[3, body + 2] = ridge + (255,)
    _door(px, 1, body + 2, DKGREY)
    return im


def mine(banner):
    """A timbered adit into a spoil heap. Reads as a hole in the ground, which is what
    distinguishes it from every building that stands up."""
    im, px = _blank()
    _rect(px, 0, 8, W - 1, H - 1, DKGREY)             # the spoil
    _rect(px, 0, 8, W - 1, 8, LTGREY)
    _rect(px, 2, 6, 5, 12, BLACK)                     # the adit mouth
    _rect(px, 2, 6, 5, 6, BROWN)                      # its lintel
    _rect(px, 2, 7, 2, 12, BROWN)                     # and its props
    _rect(px, 5, 7, 5, 12, BROWN)
    px[3, 9] = YELLOW + (255,)                        # a lamp inside
    return im


def woodcutter(banner):
    """A lean-to, a stack of cut logs and an axe in a block."""
    im, px = _blank()
    _rect(px, 0, 6, W - 1, 7, BROWN)                  # the lean-to roof
    _rect(px, 0, 6, W - 1, 6, ORANGE)
    _rect(px, 0, 8, 1, H - 1, BROWN)                  # its post
    for y in range(9, H - 1, 2):                      # the log stack
        _rect(px, 3, y, 6, y, BROWN)
        px[3, y] = PEACH + (255,)
        px[5, y] = PEACH + (255,)
    _rect(px, 2, 8, 2, 9, LTGREY)                     # the axe
    return im


def dock(banner):
    """A jetty with a moored boat: the only building that belongs on the water's edge,
    so its silhouette has to say so."""
    im, px = _blank()
    _rect(px, 0, 9, W - 1, 10, BROWN)                 # the decking
    _rect(px, 0, 9, W - 1, 9, PEACH)
    for x in (1, 4, 6):
        _rect(px, x, 11, x, H - 1, DKGREY)            # piles
    _rect(px, 2, 6, 2, 8, BROWN)                      # the mast
    _rect(px, 3, 6, 5, 8, WHITE)                      # and its sail
    px[3, 6] = LTGREY + (255,)
    return im


def wall_seg(banner):
    """A stretch of town wall with a walkway on top."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 6, W - 1, H - 1, STONE)
    _rect(px, 0, 9, W - 1, H - 1, STONE_SHADE)
    _rect(px, 0, 6, W - 1, 6, STONE_LIT)
    for x in range(0, W, 2):
        _rect(px, x, 5, x, 5, STONE)
    return im


def campfire(banner):
    """The founding marker, and it is SMALL. A fire is knee-high.

    Drawn first as a village well (a windlass over a bucket, which is what "a T over a
    water bowl" was), then as a campfire that filled the full 8x14 building cell — so the
    village's first fire stood as tall as a house. Everything on this sheet is 14 rows so
    that HOUSES can have a roof above their footprint; a fire has no reason to use them.
    It sits in the bottom six rows and leaves the rest empty.
    """
    im, px = _blank()
    _rect(px, 2, 8, 5, 9, YELLOW)                      # the flame, four rows in total
    _rect(px, 2, 10, 5, 11, ORANGE)
    px[2, 9] = ORANGE + (255,); px[5, 9] = ORANGE + (255,)
    _rect(px, 1, 12, 6, 12, BROWN)                     # logs across the base
    px[1, 12] = PEACH + (255,); px[6, 12] = PEACH + (255,)
    px[2, 12] = RED + (255,); px[5, 12] = RED + (255,)  # burning where the flame meets them
    _rect(px, 1, 13, 6, 13, DKGREY)                    # and its shadow on the ground
    return im


def cottage(banner):
    """The commonest building in the world, so it has to be the most legible one."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 4, roof, shade, ridge)          # rows 4-7
    _wall(px, body, WALL, WALL_SHADE, TIMBER)           # rows 8-13
    _door(px, 3, body + 2, BROWN, TIMBER)
    _window(px, 1, body + 1)
    _window(px, 6, body + 1)
    return im


def house(banner):
    """A storey taller than the cottage: the roof starts higher and the wall shows two
    windows, so a grown village reads as grown."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 2, roof, shade, ridge)          # a storey taller
    _wall(px, body, WALL, WALL_SHADE, TIMBER)
    _door(px, 3, body + 4, BROWN, TIMBER)
    _window(px, 1, body + 1); _window(px, 6, body + 1)
    _window(px, 1, body + 4); _window(px, 6, body + 4)  # an upper floor
    _rect(px, 6, 0, 6, 1, DKGREY)                       # a chimney
    px[6, 0] = LTGREY + (255,)
    return im


def manor(banner):
    """Two gables and a stone ground floor — the richest private house."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 1, roof, shade, ridge)          # a deep roof, rows 1-4
    _rect(px, 0, 5, W - 1, 5, shade)                    # and a bold eaves line
    _rect(px, 0, 5, 3, 5, ridge)
    _wall(px, 6, STONE, STONE_SHADE)                    # a stone ground floor
    _door(px, 3, 10, BROWN, STONE_LIT)
    _window(px, 1, 7); _window(px, 6, 7)
    _window(px, 1, 10); _window(px, 6, 10)
    _rect(px, 1, 0, 1, 1, DKGREY); px[1, 0] = LTGREY + (255,)
    return im


def hall(banner):
    """A long low mead hall: a wide roof carried down almost to the ground."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 5, roof, shade, ridge)          # broad and low: a mead hall
    _rect(px, 0, body, W - 1, body, shade)              # deep overhanging eaves
    _rect(px, 0, body, 3, body, ridge)
    _wall(px, body + 1, WALL, WALL_SHADE, TIMBER)
    _door(px, 3, body + 1, BROWN, TIMBER)
    return im


def great_hall(banner):
    """The hall grown into a stone-walled thing with a banner on it."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 3, roof, shade, ridge)
    _rect(px, 0, body, W - 1, body, shade)
    _rect(px, 0, body, 3, body, ridge)
    _wall(px, body + 1, STONE, STONE_SHADE)
    _door(px, 3, body + 2, BROWN, STONE_LIT)
    _window(px, 1, body + 1); _window(px, 6, body + 1)
    _rect(px, 1, 0, 1, 2, DKGREY)                     # the flagstaff
    _rect(px, 2, 0, 3, 1, roof)                       # and its banner
    return im


def castle(banner):
    """A crenellated keep. No pitched roof at all, which is what makes a castle read
    as a castle at eight pixels: the silhouette is battlements, not a triangle."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 3, W - 1, H - 1, STONE)
    _rect(px, 5, 3, W - 1, H - 1, STONE_SHADE)        # the shaded face
    for x in range(0, W, 2):                          # battlements
        _rect(px, x, 2, x, 2, STONE)
        _rect(px, x, 1, x, 1, STONE_LIT)
    _rect(px, 0, 4, W - 1, 4, STONE_LIT)              # a string course
    _door(px, 3, 10, NAVY, STONE_LIT)                 # the gate
    _window(px, 1, 7); _window(px, 6, 7)
    _rect(px, 4, 0, 4, 1, DKGREY)
    _rect(px, 5, 0, 6, 0, roof)                       # the standard
    return im


def barn(banner):
    """A farm building: a big roof, a big door, and hay showing inside."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 4, BROWN, DKGREY, ORANGE)
    _rect(px, 0, body, W - 1, body, DKGREY)           # eaves
    _rect(px, 0, body, 3, body, BROWN)
    _wall(px, body + 1, BROWN, DKGREY)
    _rect(px, 2, body + 1, 5, H - 2, YELLOW)          # the open bay, full of hay
    _rect(px, 2, body + 1, 5, body + 1, ORANGE)
    return im


def temple(banner):
    """Columns and a pediment. The one building that is pure stone and pure geometry."""
    im, px = _blank()
    for i in range(4):                                # pediment
        _rect(px, 3 - i, 4 - i, 4 + i, 4 - i, STONE)
        px[3 - i, 4 - i] = STONE_LIT + (255,)
    _rect(px, 0, 5, W - 1, 6, STONE)
    _rect(px, 0, 5, W - 1, 5, STONE_LIT)              # architrave
    for x in (1, 3, 5, 6):                            # columns
        _rect(px, x, 7, x, H - 2, STONE)
        px[x, 7] = STONE_LIT + (255,)
    _rect(px, 0, H - 1, W - 1, H - 1, STONE_SHADE)    # stylobate
    return im


def tower(banner):
    """A watchtower: tall, narrow, with a lit beacon so it reads at night and at
    distance. Narrow is the point — it must not be confused with a keep."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 1, 4, 6, H - 1, STONE)
    _rect(px, 4, 4, 6, H - 1, STONE_SHADE)
    _rect(px, 1, 4, 6, 4, STONE_LIT)                  # a corbel course
    for x in range(1, 7, 2):
        _rect(px, x, 3, x, 3, STONE)                  # battlements
    _rect(px, 3, 1, 4, 2, ORANGE)                     # the beacon burning on top
    _rect(px, 3, 1, 4, 1, YELLOW)
    _window(px, 2, 8); _window(px, 5, 8)
    _door(px, 3, 11, BROWN, STONE_LIT)
    return im


def barracks(banner):
    """Low, long, shuttered, with a shield hung on the front."""
    roof, shade, ridge = banner
    im, px = _blank()
    body = _pitched(px, 5, DKGREY, NAVY, LTGREY)
    _wall(px, body, BROWN, DKGREY, TIMBER)
    _rect(px, 3, body + 2, 4, body + 3, roof)         # a shield hung on the front
    px[3, body + 2] = ridge + (255,)
    _door(px, 1, body + 2, DKGREY)
    return im


def mine(banner):
    """A timbered adit into a spoil heap. Reads as a hole in the ground, which is what
    distinguishes it from every building that stands up."""
    im, px = _blank()
    _rect(px, 0, 8, W - 1, H - 1, DKGREY)             # the spoil
    _rect(px, 0, 8, W - 1, 8, LTGREY)
    _rect(px, 2, 6, 5, 12, BLACK)                     # the adit mouth
    _rect(px, 2, 6, 5, 6, BROWN)                      # its lintel
    _rect(px, 2, 7, 2, 12, BROWN)                     # and its props
    _rect(px, 5, 7, 5, 12, BROWN)
    px[3, 9] = YELLOW + (255,)                        # a lamp inside
    return im


def woodcutter(banner):
    """A lean-to, a stack of cut logs and an axe in a block."""
    im, px = _blank()
    _rect(px, 0, 6, W - 1, 7, BROWN)                  # the lean-to roof
    _rect(px, 0, 6, W - 1, 6, ORANGE)
    _rect(px, 0, 8, 1, H - 1, BROWN)                  # its post
    for y in range(9, H - 1, 2):                      # the log stack
        _rect(px, 3, y, 6, y, BROWN)
        px[3, y] = PEACH + (255,)
        px[5, y] = PEACH + (255,)
    _rect(px, 2, 8, 2, 9, LTGREY)                     # the axe
    return im


def dock(banner):
    """A jetty with a moored boat: the only building that belongs on the water's edge,
    so its silhouette has to say so."""
    im, px = _blank()
    _rect(px, 0, 9, W - 1, 10, BROWN)                 # the decking
    _rect(px, 0, 9, W - 1, 9, PEACH)
    for x in (1, 4, 6):
        _rect(px, x, 11, x, H - 1, DKGREY)            # piles
    _rect(px, 2, 6, 2, 8, BROWN)                      # the mast
    _rect(px, 3, 6, 5, 8, WHITE)                      # and its sail
    px[3, 6] = LTGREY + (255,)
    return im


def wall_seg(banner):
    """A stretch of town wall with a walkway on top."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 6, W - 1, H - 1, STONE)
    _rect(px, 0, 9, W - 1, H - 1, STONE_SHADE)
    _rect(px, 0, 6, W - 1, 6, STONE_LIT)
    for x in range(0, W, 2):
        _rect(px, x, 5, x, 5, STONE)
    return im


def campfire(banner):
    """The founding marker, and it is a CAMPFIRE — the first thing settlers light.

    This was drawn as a village well: a stone rim with blue water and a windlass frame
    over it. The object it stands for is O_FIRE_PIT, which the game calls a campfire, so
    the art and the name disagreed and it read as "a T over a water bowl" — which is
    exactly what a windlass over a full bucket looks like at eight pixels.

    The master's own (3,9) is labelled campfire at LOW confidence and is a brown mound
    with orange flecks; it reads as a loaf. Its flame cells are good but have no logs. So
    this is logs plus a flame: crossed timber with lit ends, a tapering flame above in
    yellow through red, and two embers. A fire is the one thing whose silhouette has to
    say "fire" instantly, because it is the marker that says a village starts HERE.
    """
    im, px = _blank()
    # the flame, widest at its base and tapering, three tones out from the core
    _rect(px, 2, 9, 5, 10, RED)
    _rect(px, 2, 7, 5, 8, ORANGE)
    _rect(px, 3, 5, 4, 6, YELLOW)
    px[3, 4] = YELLOW + (255,)
    px[2, 6] = RED + (255,); px[5, 6] = RED + (255,)   # licks off the sides
    # crossed logs under it, lit where the fire touches them
    _rect(px, 0, 11, 7, 12, BROWN)
    px[0, 11] = PEACH + (255,); px[7, 11] = PEACH + (255,)
    px[1, 12] = ORANGE + (255,); px[6, 12] = ORANGE + (255,)
    _rect(px, 0, 13, 7, 13, DKGREY)                    # its shadow on the ground
    px[1, 10] = ORANGE + (255,); px[6, 10] = ORANGE + (255,)   # embers
    return im


def plan(banner):
    """A surveyor's peg and a line of string: decided, not yet paid for. Deliberately
    the faintest thing the settlement can contain."""
    im, px = _blank()
    px[2, 11] = LTGREY + (255,)
    px[2, 12] = DKGREY + (255,)
    px[5, 11] = LTGREY + (255,)
    px[5, 12] = DKGREY + (255,)
    _rect(px, 3, 11, 4, 11, WHITE)
    return im


# The order here IS the order of O_FIRE_PIT..O_PLAN in mb.h, so the sheet's column
# index is `obj - O_BUILD0` and the C side needs no lookup table at all.

# ---------------------------------------------------------------- the civic ladder ------
# One building per era, so a skyline dates itself. The vocabulary is deliberately shared and
# deliberately drifts: timber and thatch in the early eras, dressed stone and pillars in the
# middle, then BRICK WITH A CHIMNEY for the industrial rungs and concrete for the modern ones.
# A player should be able to tell a Renaissance city from an Industrial one at a glance,
# without knowing what any single building is.
# Timber, for the granary. The sheet's own palette calls these WALL/WALL_SHADE/TIMBER.
WOOD, WOOD_LIT, WOOD_DARK = BROWN, PEACH, DKGREY

BRICK      = (150,  66,  52)
BRICK_LIT  = (186,  92,  70)
BRICK_DARK = (104,  44,  36)
GLASS      = (110, 170, 210)
GLASS_LIT  = (170, 214, 236)
CONC       = (150, 150, 158)
CONC_LIT   = (196, 196, 202)
CONC_DARK  = ( 96,  96, 106)
STEEL      = ( 70,  74,  86)
COPPER     = (140, 190, 170)
EMBER      = (255, 150,  40)
GOLD       = YELLOW          # the sheet has no separate gold; the artist's yellow is it


def _chimney(px, x, top, w=1, cap=None):
    """A stack. The one silhouette that says industry at eight pixels wide."""
    _rect(px, x, top, x + w - 1, H - 1, BRICK)
    _rect(px, x, top, x, H - 1, BRICK_LIT)
    _rect(px, x, top, x + w - 1, top, cap or BRICK_DARK)


def granary(banner):
    """POTTERY. A round timber drum with a conical thatch cap — the oldest civic building
    there is, and the only one here still made of the same stuff as the houses."""
    roof, shade, ridge = banner
    im, px = _blank()
    _pitched(px, 2, roof, shade, ridge, rows=3)          # the conical cap, the banner's colour
    _rect(px, 1, 5, 6, H - 1, WOOD)
    _rect(px, 1, 5, 1, H - 1, WOOD_LIT)
    _rect(px, 6, 5, 6, H - 1, WOOD_DARK)
    for y in range(6, H - 1, 3):                          # the hoops that hold it together
        _rect(px, 1, y, 6, y, WOOD_DARK)
    _door(px, 3, H - 4)
    return im


def market(banner):
    """CURRENCY. A stall under a striped awning: the awning is the whole silhouette, and the
    stripes are the one pattern in the town that is not masonry or thatch."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 4, 7, 6, roof)                           # the canopy
    for x in range(0, 8, 2):
        _rect(px, x, 4, x, 6, WHITE)                      # its stripes
    _rect(px, 0, 7, 7, 7, shade)                          # its shadow
    _rect(px, 1, 8, 1, H - 1, WOOD_DARK)                  # the posts
    _rect(px, 6, 8, 6, H - 1, WOOD_DARK)
    _rect(px, 2, 10, 5, H - 2, WOOD)                      # the trestle, and goods on it
    _rect(px, 2, 10, 5, 10, WOOD_LIT)
    _rect(px, 3, 9, 3, 9, GOLD)
    _rect(px, 5, 9, 5, 9, EMBER)
    return im


def library(banner):
    """WRITING. Dressed stone with a pillared front and one tall window — the first building
    in the town that is about knowing something rather than storing or defending it."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 5, 7, H - 1, STONE)
    _rect(px, 0, 5, 7, 5, STONE_LIT)
    _rect(px, 0, 6, 0, H - 1, STONE_LIT)
    _rect(px, 7, 6, 7, H - 1, STONE_SHADE)
    for i in range(3):                                    # the pediment
        _rect(px, 3 - i, 4 - i, 4 + i, 4 - i, STONE_LIT)
    for x in (1, 3, 5):                                   # pillars
        _rect(px, x, 8, x, H - 2, STONE_LIT)
    _rect(px, 6, 8, 6, 10, GLASS)                         # and the tall window
    _rect(px, 6, 8, 6, 8, GLASS_LIT)
    return im


def foundry(banner):
    """METALLURGY. Brick, a stack, and a door with the furnace showing through it. This and
    the market are the only two buildings that produce anything the treasury can count."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 6, 5, H - 1, BRICK)
    _rect(px, 0, 6, 5, 6, BRICK_LIT)
    _rect(px, 5, 7, 5, H - 1, BRICK_DARK)
    _chimney(px, 6, 1, 2, STEEL)
    _rect(px, 1, 9, 3, H - 2, EMBER)                      # the furnace mouth
    _rect(px, 2, 10, 2, H - 3, (255, 220, 120))
    _rect(px, 6, 0, 7, 0, (120, 120, 130))                # a wisp of smoke
    return im


def college(banner):
    """THE UNIVERSITY. Stone, pillars and a dome — the tallest thing in the town until the
    factory arrives, and the reason a kingdom that builds them out-researches one that does
    not by a factor of several."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 6, 7, H - 1, STONE)
    _rect(px, 0, 6, 7, 6, STONE_LIT)
    _rect(px, 7, 7, 7, H - 1, STONE_SHADE)
    _rect(px, 2, 3, 5, 5, COPPER)                         # the dome
    _rect(px, 3, 2, 4, 2, COPPER)
    _rect(px, 2, 3, 3, 3, (180, 230, 210))
    _rect(px, 3, 1, 4, 1, GOLD)                           # and its finial
    for x in (1, 3, 5):
        _rect(px, x, 8, x, H - 2, STONE_LIT)
    _door(px, 6, 10)
    return im


def hospital(banner):
    """SANITATION. Whitewashed, with a cross over the door. Plainest building in the town on
    purpose: it is the only one whose job is that fewer people die."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 5, 7, H - 1, (222, 222, 214))
    _rect(px, 0, 5, 7, 5, WHITE)
    _rect(px, 7, 6, 7, H - 1, (176, 176, 170))
    _rect(px, 3, 2, 4, 6, (200, 40, 50))                  # the cross
    _rect(px, 2, 3, 5, 4, (200, 40, 50))
    for y in (8, 11):                                     # windows in rows, like a ward
        for x in (1, 3, 5):
            _rect(px, x, y, x, y, GLASS)
    return im


def factory(banner):
    """STEAM. Two stacks and a saw-tooth roof — the silhouette that says a town has stopped
    being medieval. It is the widest building on the sheet for the same reason."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 7, 7, H - 1, BRICK)
    _rect(px, 0, 7, 7, 7, BRICK_LIT)
    for x in range(0, 8, 2):                              # the saw-tooth roof
        _rect(px, x, 6, x, 6, BRICK_DARK)
        _rect(px, x + 1, 6, x + 1, 6, GLASS)
    _chimney(px, 1, 1, 1, STEEL)
    _chimney(px, 5, 3, 1, STEEL)
    _rect(px, 1, 0, 1, 0, (120, 120, 130))
    for x in (3, 6):                                      # lit windows: it runs at night
        _rect(px, x, 10, x, 11, GLASS_LIT)
    return im


def station(banner):
    """THE RAILWAY. A canopy, a platform and a signal. What it does is make caravans quick,
    and what it looks like is the first building here that is mostly steel."""
    roof, shade, ridge = banner
    im, px = _blank()
    _rect(px, 0, 5, 7, 6, STEEL)                          # the canopy
    _rect(px, 0, 5, 7, 5, (110, 116, 130))
    _rect(px, 0, 7, 0, H - 1, STEEL)                      # its posts
    _rect(px, 7, 7, 7, H - 1, STEEL)
    _rect(px, 1, 9, 6, H - 2, BRICK)                      # the booking hall
    _rect(px, 1, 9, 6, 9, BRICK_LIT)
    _rect(px, 2, 11, 3, 11, GLASS_LIT)
    _rect(px, 6, 1, 6, 4, STEEL)                          # the signal post
    _rect(px, 5, 1, 5, 1, (60, 220, 90))
    _rect(px, 0, H - 1, 7, H - 1, (90, 86, 80))           # the rails
    return im


def power(banner):
    """ELECTRICITY. Two cooling towers and a switchyard. The tallest and coldest thing in the
    town, and the last civic rung before the silo — a city with one of these is a city that is
    four techs from the bomb."""
    roof, shade, ridge = banner
    im, px = _blank()
    for x0 in (0, 4):                                     # the towers, waisted like real ones
        _rect(px, x0, 3, x0 + 2, 3, CONC_LIT)
        _rect(px, x0 + 1, 4, x0 + 1, 6, CONC)
        _rect(px, x0, 4, x0, 6, CONC_LIT)
        _rect(px, x0 + 2, 4, x0 + 2, 6, CONC_DARK)
        _rect(px, x0, 7, x0 + 2, H - 2, CONC)
        _rect(px, x0, 7, x0, H - 2, CONC_LIT)
        _rect(px, x0 + 2, 7, x0 + 2, H - 2, CONC_DARK)
        _rect(px, x0, 2, x0 + 2, 2, (190, 190, 196))      # steam off the top
    _rect(px, 3, 8, 3, H - 2, STEEL)                      # the switchyard between them
    _rect(px, 0, H - 1, 7, H - 1, CONC_DARK)
    _rect(px, 3, 5, 3, 5, (255, 236, 120))                # and a light on
    return im


def silo(banner):
    """A MISSILE SILO, the end of the tech tree made concrete.

    It has to read as the odd one out in a town of thatch and stone, because that is exactly
    what it is: the only building here not made of the same materials as the rest. So it is a
    concrete drum with a steel blast door and a hazard stripe — cold greys against a village
    of browns — and the banner colour appears only as a small flag, because a silo belongs to
    a kingdom rather than to a lord.
    """
    roof, shade, ridge = banner
    im, px = _blank()
    CONC      = (150, 150, 158)
    CONC_LIT  = (196, 196, 202)
    CONC_DARK = ( 96,  96, 106)
    STEEL     = ( 70,  74,  86)
    HAZARD    = (255, 214,  60)

    _rect(px, 1, 4, 6, H - 1, CONC)                 # the drum
    _rect(px, 1, 4, 1, H - 1, CONC_LIT)             # lit from the left, as everything is
    _rect(px, 6, 4, 6, H - 1, CONC_DARK)
    _rect(px, 0, H - 2, 7, H - 1, CONC_DARK)        # the apron it stands on

    _rect(px, 1, 3, 6, 4, STEEL)                    # the blast door, split down the middle
    _rect(px, 3, 3, 4, 4, CONC_DARK)
    _rect(px, 2, 2, 5, 2, STEEL)

    for x in range(1, 7, 2):                        # the one thing that says what is inside
        _rect(px, x, 8, x, 8, HAZARD)

    _rect(px, 7, 1, 7, 5, STEEL)                    # a mast, and the crown's flag on it
    _rect(px, 5, 1, 7, 1, roof)
    return im


BUILDINGS = [
    ("campfire",   campfire),
    ("hall",       hall),
    ("great_hall", great_hall),
    ("castle",     castle),
    ("house",      house),
    ("cottage",    cottage),
    ("manor",      manor),
    ("barn",       barn),
    ("mine",       mine),
    ("woodcutter", woodcutter),
    ("barracks",   barracks),
    ("temple",     temple),
    ("tower",      tower),
    ("dock",       dock),
    ("wall",       wall_seg),
    ("granary",    granary),
    ("market",     market),
    ("library",    library),
    ("foundry",    foundry),
    ("college",    college),
    ("hospital",   hospital),
    ("factory",    factory),
    ("station",    station),
    ("power",      power),
    ("silo",       silo),
    ("plan",       plan),
]


def build():
    """One column per building, one row per banner colour: 16 x 5 cells of 8 x 14."""
    sheet = Image.new("RGBA", (W * len(BUILDINGS), H * len(BANNERS)), (0, 0, 0, 0))
    for row, banner in enumerate(BANNERS):
        for col, (_name, fn) in enumerate(BUILDINGS):
            sheet.paste(fn(banner), (col * W, row * H))
    return sheet


def names():
    return [n for n, _ in BUILDINGS]
