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

def plain(base):
    """Flat. hedge is flat and it looks better than anything textured here; for a
    biome whose identity is its colour, this is the right answer."""
    im, _ = _new(base)
    return im


def dashes(base, ink, seed, step=3, run=3, slip=3):
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
        for i in range(run):
            _put(px, x0 + i, y, ink)
        for i in range(run):
            _put(px, x0 + i + TS // 2 + 1, y, ink)
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
# (name, base, [interior builders], [weights], rim, rim_south, cut)
#
# `cut` is the colour of a chamfered outside corner — see terrain.py. A biome cuts
# toward whatever it normally sits IN, so an island's corners read as shoreline and a
# rock outcrop's as shadow, and a diagonal run of tiles reads as a diagonal.
#
# THE RIM IS HALF OF IT and the artist's own sets prove it: hedge is a flat green
# field with a bright green rim, and it reads perfectly. So most biomes here are flat
# or nearly flat, and their identity comes from colour plus edge. Only the materials
# that genuinely have structure — rock, water, ploughed ground — carry a pattern, and
# those patterns are regular.
#
# BUT ONLY SOME BIOMES RIM. When every one of them rimmed itself, every boundary
# carried two rims — one from each side — and the map read as a set of outlined
# ribbons. So the soft ground (grass, snow, sand, ash, hill, swamp) has rim=None and
# simply stops; water, rock, lava, ice and ploughed land keep theirs, because those
# edges are physically real. Every biome still CUTS its outside corners, so shapes
# stay rounded either way.

def recipes():
    return [
        #  name          base     interiors                                      weights   rim      rim_south
        # --- water: TWO BLUES, or blue with white foam. Never a mauve. ------
        ("bio_ice",   WHITE,  [lambda v: plain(WHITE),
                               lambda v: cracks(WHITE, BLUE, 11 + v)],
                              [3, 2], BLUE, None, BLUE),
        ("bio_snow",  WHITE,  [lambda v: plain(WHITE),
                               lambda v: specks(WHITE, LTGREY, 21 + v, step=4)],
                              [3, 2], None, DKGREY, LTGREY),
        ("bio_tundra", SLATE, [lambda v: plain(SLATE),
                               lambda v: specks(SLATE, LTGREY, 31 + v, step=4)],
                              [3, 2], None, None, DKGREY),
        # --- sand ----------------------------------------------------------
        ("bio_beach", PEACH,  [lambda v: plain(PEACH),
                               lambda v: specks(PEACH, ORANGE, 41 + v, step=4)],
                              [3, 2], None, BROWN, BROWN),
        ("bio_desert", YELLOW,[lambda v: dashes(YELLOW, ORANGE, 51 + v, step=4, run=3),
                               lambda v: specks(YELLOW, ORANGE, 53 + v, step=4)],
                              [3, 2], None, BROWN, BROWN),
        # --- green ---------------------------------------------------------
        # PLAIN DOMINATES. hedge is a flat green field and it reads better than any
        # amount of texture; the tufts are the occasional variant, not the rule.
        ("bio_grass", DKGREEN,[lambda v: plain(DKGREEN),
                               lambda v: tufts(DKGREEN, GREEN, 61 + v, step=4)],
                              [4, 2], None, None, DKGREY),
        ("bio_savanna", ORANGE,[lambda v: plain(ORANGE),
                               lambda v: tufts(ORANGE, YELLOW, 71 + v, step=4)],
                              [4, 2], None, None, BROWN),
        ("bio_swamp", DKGREEN,[lambda v: specks(DKGREEN, DKGREY, 81 + v, step=3),
                               lambda v: tufts(DKGREEN, DKGREY, 83 + v, step=4)],
                              [3, 2], None, None, NAVY),
        # --- rock: staggered courses, exactly like wall_brick ---------------
        ("bio_hill",  BROWN,  [lambda v: brickwork(BROWN, DKGREY, 91 + v, bw=5, bh=4),
                               lambda v: specks(BROWN, DKGREY, 93 + v, step=3)],
                              [3, 2], None, NAVY, NAVY),
        # bh=4 keeps the mortar to one row in four. At bh=2 the two greys were half
        # the tile each and the rock read as noise instead of as courses.
        ("bio_mountain", DKGREY,[lambda v: brickwork(DKGREY, LTGREY, 101 + v, bw=4, bh=4),
                                 lambda v: brickwork(DKGREY, LTGREY, 103 + v, bw=5, bh=4)],
                                [3, 2], LTGREY, NAVY, NAVY),
        ("bio_peak",  LTGREY, [lambda v: capped(LTGREY, WHITE, 111 + v),
                               lambda v: brickwork(LTGREY, WHITE, 113 + v, bw=5, bh=4)],
                              [3, 2], WHITE, DKGREY, DKGREY),
        ("bio_rubble", LTGREY,[lambda v: brickwork(LTGREY, DKGREY, 121 + v, bw=3, bh=3),
                               lambda v: specks(LTGREY, DKGREY, 123 + v, step=3)],
                              [2, 2], None, NAVY, NAVY),
        # --- burnt ---------------------------------------------------------
        ("bio_ash",   DKGREY, [lambda v: specks(DKGREY, LTGREY, 131 + v, step=4),
                               lambda v: plain(DKGREY)],
                              [3, 2], None, None, BLACK),
        ("bio_scorched", MAROON,[lambda v: cracks(MAROON, BLACK, 141 + v),
                                 lambda v: specks(MAROON, DKGREY, 143 + v, step=4)],
                                [3, 2], None, None, BLACK),
        # --- worked land ---------------------------------------------------
        ("bio_farm",  BROWN,  [lambda v: stripes(BROWN, DKGREY, YELLOW, 151 + v),
                               lambda v: stripes(BROWN, DKGREY, GREEN, 153 + v)],
                              [3, 2], DKGREY, NAVY, NAVY),
    ]
