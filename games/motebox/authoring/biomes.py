"""
Motebox — the biome texture vocabulary.

WHY THIS FILE EXISTS. The first version of the biome fills picked "texture cells"
out of the master by INK COVERAGE — any hand-drawn cell whose ink covered 18-55%
of the tile was a candidate — and stamped the winner over a flat colour. That is
selection without looking, and it produced exactly what it deserved:

  * six biomes (mountain, ash, scorched, tundra, peak, ice) wore the SAME
    diagonal-chunk motif in six different colours, so none of them had an identity
  * four more (grass, savanna, desert, swamp) wore the same arrow-blob
  * snow was scattered with the master's HEARTS
  * tundra was scattered with its PLUS SIGNS
  * mountains were covered in neat masonry brick

Rows 47-49 of the master are decorative LINE ART — dashes, brackets, arrows,
hearts, scales, arches — drawn to edge a dungeon room, not to be ground. Tiling
one motif identically across a continent reads as wallpaper however good the motif
is, because real terrain has no repeating unit.

SECOND ATTEMPT, ALSO WRONG: random scatter. Grains, blades and clumps sprinkled at
random positions read as litter dropped on a flat colour, not as a material. Put
side by side with the artist's own sets the reason is obvious:

  * wall_brick's interior is a REGULAR STAGGERED PATTERN — courses of navy blocks
    with brown mortar. It has structure, and structure is what reads as a surface.
  * hedge's interior is simply FLAT. Dark green, nothing else, with a bright green
    rim. Also completely fine, because a clean flat field with a good edge reads
    better than any amount of noise.

So the vocabulary here is now REGULAR: offset dash courses, staggered brickwork,
a lattice of specks on a fixed step. Nothing is placed at random. Every colour is
one of the master's sixteen, every pattern WRAPS at the tile edge so it is seamless,
and the rim (terrain.py) carries the material's identity at its boundary.

And the colour PAIRS matter as much as the pattern. The second attempt drew the sea
as SLATE (a mauve) on BLUE, which came out as pink zigzags on cyan. Water is two
blues, or a blue with white foam. Nothing else.
"""
from PIL import Image

# ---------------------------------------------------------------- palette ----
NAVY   = (29, 43, 83);    MAROON = (126, 37, 83);  DKGREEN = (0, 135, 81)
BROWN  = (171, 82, 54);   DKGREY = (95, 87, 79);   LTGREY  = (194, 195, 199)
WHITE  = (255, 241, 232); RED    = (255, 0, 77);   ORANGE  = (255, 163, 0)
YELLOW = (255, 236, 39);  GREEN  = (0, 228, 54);   BLUE    = (41, 173, 255)
SLATE  = (131, 118, 156); PINK   = (255, 119, 168); PEACH  = (255, 204, 170)
BLACK  = (0, 0, 0)

TS = 8


def _rng(seed):
    """A tiny deterministic generator, so a texture is reproducible from a seed."""
    s = (seed * 2654435761) & 0xFFFFFFFF
    def nxt():
        nonlocal s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        return s & 0xFFFFFFFF
    return nxt


def _new(base):
    im = Image.new("RGBA", (TS, TS), base + (255,))
    return im, im.load()


def _put(px, x, y, c):
    """Every write wraps, which is what makes the whole vocabulary seamless."""
    px[x % TS, y % TS] = c + (255,)


# ------------------------------------------------------------- vocabulary ----
# Everything below is REGULAR. Nothing is placed at random, because random reads as
# litter. Compare wall_brick (staggered courses) and hedge (flat) — those are the two
# things that work at eight pixels.

def mottle(body, light, dark, seed, n=2):
    """A flat field with GRAIN: a handful of lighter and darker pixels on a fixed
    lattice, so a continent has surface without having a pattern.

    The line to walk here was learned the hard way twice. High-contrast scatter reads as
    litter dropped on a colour; a regular lattice of anything reads as wallpaper once it
    covers a continent. What works is FEW marks (three of each in sixty-four pixels),
    placed regularly enough not to look strewn, in tones from the SAME family so the
    eye reads them as one surface catching light rather than as three colours.
    """
    im, px = _new(body)
    r = _rng(seed)
    for i in range(n):
        _put(px, r() % TS, r() % TS, light)
        _put(px, r() % TS, r() % TS, dark)
    return im


def plain(base):
    """Flat. hedge is flat and it looks better than anything textured here; for a
    biome whose identity is its colour, this is the right answer."""
    im, _ = _new(base)
    return im


def dashes(base, ink, seed, step=3, run=3, slip=3, ink2=None):
    """Courses of short horizontal dashes, each row slipped along from the last.
    WATER, DUNES, LAVA — a regular course reads as a surface in motion, and the slip
    stops the columns lining up into a grid."""
    im, px = _new(base)
    r = _rng(seed)
    off = r() % step
    for y in range(TS):
        if (y + off) % step:
            continue
        x0 = (y // step) * slip + (r() % 2)
        # ALTERNATE COURSES TAKE THE SECOND TONE. A liquid drawn in two colours is a
        # flat field with stripes on it; a third tone reads as a surface with troughs
        # and crests, which is what water and lava actually have.
        c = ink2 if (ink2 and (y // step) % 2) else ink
        for i in range(run):
            _put(px, x0 + i, y, c)
        for i in range(run):
            _put(px, x0 + i + TS // 2 + 1, y, c)
    return im


def brickwork(base, ink, seed, bw=4, bh=2):
    """Staggered courses, mortar between them. ROCK and STRATA — this is exactly
    what wall_brick does, and it is the single most legible 8 px texture there is."""
    im, px = _new(base)
    r = _rng(seed)
    off = r() % bw
    for y in range(TS):
        course = y // bh
        if y % bh == bh - 1:                      # the mortar line under a course
            for x in range(TS):
                _put(px, x, y, ink)
        else:
            shift = off + (bw // 2 if course % 2 else 0)
            for x in range(TS):
                if (x + shift) % bw == 0:         # the vertical joint
                    _put(px, x, y, ink)
    return im


def pebbles(base, ink, seed, step=4):
    """Chunky 2x2 marks on a lattice, rows half-offset. SCREE, RUBBLE, BOULDER FIELD.
    A stone seen from above is a BLOB, not a line: at eight pixels a two-by-two mark
    is the smallest thing that still reads as an object with a size, where a single
    pixel reads as grain and a line reads as a joint."""
    im, px = _new(base)
    r = _rng(seed)
    ox, oy = r() % step, r() % step
    for y in range(0, TS, step):
        half = (step // 2) if (y // step) % 2 else 0
        for x in range(0, TS, step):
            bx, by = x + ox + half, y + oy
            for dy in range(2):
                for dx in range(2):
                    _put(px, bx + dx, by + dy, ink)
    return im


def facets(base, ink, seed, n=4):
    """Short joint segments that NEVER TOUCH THE TILE EDGE. Fractured bedrock from
    above.

    This exists because brickwork does not work on ground. Staggered courses with
    mortar are what wall_brick is, and the artist uses it for a WALL — so when this
    game laid it over hills and mountains, the map came out as a red brick wall you
    could walk on, which is exactly what it looked like. The failure is not the
    pattern's quality, it is that a mortar course is a man-made thing and ground has
    none.

    Keeping every segment off the border is the whole trick: joints then cannot line
    up across tile boundaries, so no amount of repetition can grow a course."""
    im, px = _new(base)
    r = _rng(seed)
    for _ in range(n):
        horiz = r() & 1
        ln = 2 + (r() % 3)
        x = 1 + (r() % (TS - 2 - (ln if horiz else 0)))
        y = 1 + (r() % (TS - 2 - (0 if horiz else ln)))
        for i in range(ln):
            px[x + (i if horiz else 0), y + (0 if horiz else i)] = ink + (255,)
    return im


def specks(base, ink, seed, step=4):
    """One pixel on a fixed lattice, the row offset by half a step. GRASS, SNOW,
    ASH — sparse and even, so it reads as a fine grain rather than as debris."""
    im, px = _new(base)
    r = _rng(seed)
    ox, oy = r() % step, r() % step
    for y in range(TS):
        if (y + oy) % step:
            continue
        half = (step // 2) if ((y + oy) // step) % 2 else 0
        for x in range(TS):
            if (x + ox + half) % step == 0:
                _put(px, x, y, ink)
    return im


def tufts(base, ink, seed, step=4):
    """The speck lattice, but each mark stands up two pixels. GRASS and REEDS — a
    vertical pair reads as a plant, and on a lattice it reads as a field of them."""
    im, px = _new(base)
    r = _rng(seed)
    ox, oy = r() % step, r() % step
    for y in range(TS):
        if (y + oy) % step:
            continue
        half = (step // 2) if ((y + oy) // step) % 2 else 0
        for x in range(TS):
            if (x + ox + half) % step == 0:
                _put(px, x, y, ink)
                _put(px, x, y - 1, ink)
    return im


def stripes(base, ink, crop, seed, period=4):
    """Vertical furrows with a crop standing between them. PLOUGHED FIELD — the one
    biome that should look man-made."""
    im, px = _new(base)
    r = _rng(seed)
    off = r() % period
    for x in range(TS):
        if (x + off) % period == 0:
            for y in range(TS):
                _put(px, x, y, ink)
    if crop:
        for y in range(1, TS, 3):
            for x in range(TS):
                if (x + off) % period == 2:
                    _put(px, x, y, crop)
    return im


def cracks(base, ink, seed, n=1):
    """One thin line with momentum. ICE and BAKED GROUND — the only irregular thing
    left in the vocabulary, because a crack that is regular is a joint."""
    im, px = _new(base)
    r = _rng(seed)
    for _ in range(n):
        x, y = r() % TS, r() % TS
        dx, dy = (1, 0) if (r() & 1) else (0, 1)
        for _ in range(6 + (r() % 4)):
            _put(px, x, y, ink)
            x += dx; y += dy
            if (r() % 5) == 0:
                dx, dy = (0, 1 if (r() & 1) else -1) if dx else (1 if (r() & 1) else -1, 0)
    return im


def capped(base, snow, seed):
    """Light across the upper rows. PEAKS — a mountain top reads as a peak because
    the snow sits on top of it, so the texture has to have an up."""
    im, px = _new(base)
    r = _rng(seed)
    for x in range(TS):
        h = 1 + ((r() % 2) if x % 2 else 0)
        for y in range(h):
            _put(px, x, y, snow)
    return im


# ----------------------------------------------------------------- recipes ----
# (name, base, [interior builders], [weights], rim)
#
# FLAT BASE, BRIGHT 2px RIM. That is the artist's own structure — see the docstring in
# terrain.py, which dumps hedge's pixels — and it is what every earlier version of this
# file got backwards. The identity of a biome is its COLOUR plus its EDGE, in that
# order; interior pattern is a distant third and mostly a liability, because two
# variants of any lattice tiled over a continent read as wallpaper.
#
# So `plain` is the default and almost the only answer. A pattern earns its place only
# where the real surface has one:
#   - water has ripples, so ocean/sea/shallow keep `dashes`
#   - a ploughed field has furrows, so farmland keeps `stripes`
#   - lava is molten, so it keeps `dashes` too
# Everything else — grass, sand, snow, rock, ash, swamp — is flat, exactly like hedge.
#
# NOTHING RIMS ANY MORE. Every boundary in the world is drawn by the TRANSITION
# OVERLAY (authoring/transitions.py) in the higher terrain's colours, inside the lower
# terrain's cell — so the base layer is pure flat colour fields and the visible edge is
# an organic shape that owes nothing to the tile grid. A rim here would draw a straight
# bright line along the straight tile edge underneath the overlay, which is precisely
# the "too many hard straight lines" this replaced.
#
# (Kept for the record: the rim was brighter than the base and from the same family —
# DKGREEN rimmed GREEN
# (which is literally what hedge does), BROWN rimmed ORANGE, DKGREY rimmed LTGREY. A
# rim in a contrasting hue is what made the map read as outlined ribbons, and a dark
# rim — NAVY, BLACK — is what "why does the grey texture have black outlines?" was
# looking at. Nothing here rims dark.

def recipes():
    return [
        #  name           base      interiors                                  weights  rim      sides
        # --- ice and snow ---------------------------------------------------
        ("bio_ice",    WHITE,   [lambda v: plain(WHITE),
                                 lambda v: cracks(WHITE, BLUE, 11 + v)],       [4, 2], None),
        ("bio_snow", WHITE, [lambda v: mottle(WHITE, LTGREY, LTGREY, 200 + v),
                                 lambda v: mottle(WHITE, LTGREY, LTGREY, 205 + v)],
                                                                       [1, 1], None),
        # TUNDRA IS A COLD BROWN STEPPE, not lavender. It was PICO-8's SLATE, which is
        # a mauve, and on a world where tundra is the biggest biome the map came out
        # PINK — an alien planet rather than a cold one. The palette has no olive or
        # khaki, so the honest choice is brown with a pale rim: frozen ground with
        # frost on its edges. Hill is also brown, but hill rims ORANGE and tundra rims
        # LTGREY, so a warm slope and a cold one still read apart.
        ("bio_tundra", BROWN, [lambda v: mottle(BROWN, PEACH, DKGREY, 210 + v),
                                 lambda v: mottle(BROWN, PEACH, DKGREY, 215 + v)],
                                                                       [1, 1], None),
        # --- sand -----------------------------------------------------------
        ("bio_beach", PEACH, [lambda v: mottle(PEACH, YELLOW, ORANGE, 220 + v),
                                 lambda v: mottle(PEACH, YELLOW, ORANGE, 225 + v)],
                                                                       [1, 1], None),
        ("bio_desert", YELLOW, [lambda v: mottle(YELLOW, PEACH, ORANGE, 230 + v),
                                 lambda v: mottle(YELLOW, PEACH, ORANGE, 235 + v)],
                                                                       [1, 1], None),
        # --- green: hedge's exact pairing, dark green field with a bright edge
        ("bio_grass", DKGREEN, [lambda v: mottle(DKGREEN, GREEN, DKGREY, 240 + v),
                                 lambda v: mottle(DKGREEN, GREEN, DKGREY, 245 + v)],
                                                                       [1, 1], None),
        ("bio_savanna", ORANGE, [lambda v: mottle(ORANGE, YELLOW, BROWN, 250 + v),
                                 lambda v: mottle(ORANGE, YELLOW, BROWN, 255 + v)],
                                                                       [1, 1], None),
        ("bio_swamp",  DKGREEN, [lambda v: plain(DKGREEN),
                                 lambda v: specks(DKGREEN, DKGREY, 81 + v, step=4)],
                                                                               [3, 2], None),
        # --- rock: flat, and it is the RIM that makes it a cliff -------------
        ("bio_hill", BROWN, [lambda v: mottle(BROWN, ORANGE, DKGREY, 260 + v),
                                 lambda v: mottle(BROWN, ORANGE, DKGREY, 265 + v)],
                                                                       [1, 1], None),
        ("bio_mountain", DKGREY, [lambda v: mottle(DKGREY, LTGREY, BROWN, 270 + v),
                                 lambda v: mottle(DKGREY, LTGREY, BROWN, 275 + v)],
                                                                       [1, 1], None),
        ("bio_peak", LTGREY, [lambda v: mottle(LTGREY, WHITE, DKGREY, 280 + v),
                                 lambda v: mottle(LTGREY, WHITE, DKGREY, 285 + v)],
                                                                       [1, 1], None),
        ("bio_rubble", LTGREY,  [lambda v: plain(LTGREY),
                                 lambda v: pebbles(LTGREY, DKGREY, 121 + v, step=4)],
                                                                               [2, 3], None),
        # --- burnt ----------------------------------------------------------
        # ASH DOES NOT RIM. It is formless burnt ground, and a light-grey band on dark
        # grey outlined every scar: a mid-game world had a quarter of one screen in ash
        # and each isolated tile read as a hard grey square dropped on the grass. With
        # no band of its own, a burn is a soft dark patch and the grass's own lit edge
        # is what bounds it — which is also what a burn actually looks like from above.
        ("bio_ash",    DKGREY,  [lambda v: mottle(DKGREY, LTGREY, BROWN, 300 + v),
                                 lambda v: mottle(DKGREY, LTGREY, BROWN, 305 + v)],
                                                                               [1, 1], None),
        ("bio_scorched", MAROON,[lambda v: plain(MAROON),
                                 lambda v: cracks(MAROON, RED, 141 + v)],      [3, 2], None),
        # --- worked land: the one biome that SHOULD look man-made ------------
        ("bio_farm",   BROWN,   [lambda v: stripes(BROWN, DKGREY, YELLOW, 151 + v),
                                 lambda v: stripes(BROWN, DKGREY, GREEN, 153 + v)],
                                                                               [3, 2], None),
    ]
