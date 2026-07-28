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

So terrain is generated here instead, from a small vocabulary that says what a
material IS: grains, ripples, blades, facets, cracks, furrows, dust. Every colour
is still one of the master's sixteen, every pattern WRAPS at the tile edge so it is
seamless, and each biome gets several variants (the engine picks one per cell by
position hash) so a large area never shows a repeat.

Where the master DOES draw a terrain properly, we use its art and generate nothing:
the chevron wave band is genuinely good water, and roguemote's six hand-drawn
blob47 bands are better than anything here. See SYNC_TERRAIN in extract_box.py.
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

def grains(base, light, dark, n_light, n_dark, seed):
    """Loose single pixels of two tones. SAND, ASH, DUST — a surface of particles.
    The two tones matter: one tone on a flat base reads as dirt on a wall, two
    read as a material with depth."""
    im, px = _new(base)
    r = _rng(seed)
    for _ in range(n_light):
        _put(px, r() % TS, r() % TS, light)
    for _ in range(n_dark):
        _put(px, r() % TS, r() % TS, dark)
    return im


def ripple(base, ink, seed, rows=(1, 5), width=6):
    """Short horizontal dashes on a couple of rows, offset per row. DUNES and
    WIND-BLOWN ground: the eye reads horizontal breaks as a flat windswept plane."""
    im, px = _new(base)
    r = _rng(seed)
    for row in rows:
        x0 = r() % TS
        for i in range(width):
            _put(px, x0 + i, row, ink)
        # a second, shorter dash further along, so the row is not one solid bar
        x1 = x0 + width + 1 + (r() % 3)
        for i in range(2):
            _put(px, x1 + i, row + 1, ink)
    return im


def blades(base, ink, dark, n, seed):
    """Upright two-pixel marks. GRASS, SAVANNA, TUNDRA — the mark reads as a plant
    because it stands up, which a scattered dot never does."""
    im, px = _new(base)
    r = _rng(seed)
    for _ in range(n):
        x, y = r() % TS, r() % TS
        _put(px, x, y, ink)
        _put(px, x, y + 1, ink)
        if r() & 1:
            _put(px, x + 1, y + 1, dark)      # a shadow at the base of the blade
    return im


def clumps(base, mid, dark, n, seed):
    """Two-by-two blocks with a shadow pixel. PEBBLES, MUCK, RUBBLE — a 2x2 is the
    smallest shape that reads as an object rather than as noise."""
    im, px = _new(base)
    r = _rng(seed)
    for _ in range(n):
        x, y = r() % TS, r() % TS
        _put(px, x,     y,     mid)
        _put(px, x + 1, y,     mid)
        _put(px, x,     y + 1, mid)
        _put(px, x + 1, y + 1, dark)
    return im


def facets(base, light, dark, seed):
    """Angular light-and-shadow wedges. ROCK — stone reads as flat planes meeting at
    edges, so the texture is diagonal runs rather than scattered anything."""
    im, px = _new(base)
    r = _rng(seed)
    for _ in range(3):
        x, y = r() % TS, r() % TS
        ln = 2 + (r() % 3)
        dirn = 1 if (r() & 1) else -1
        for i in range(ln):
            _put(px, x + i, y + i * dirn, light)
        # the shadow side of the same edge
        for i in range(ln):
            _put(px, x + i, y + i * dirn + 1, dark)
    return im


def cracks(base, ink, seed, n=2):
    """A thin wandering line with MOMENTUM. ICE and BAKED GROUND.

    The first version turned on a coin flip every pixel, which produced tight
    staircase corners that read as little L-shaped glyphs stamped on the ground
    rather than as cracks. A crack keeps going: it only changes direction
    occasionally, and that one property is the whole difference."""
    im, px = _new(base)
    r = _rng(seed)
    for _ in range(n):
        x, y = r() % TS, r() % TS
        dx, dy = (1, 0) if (r() & 1) else (0, 1)
        for _ in range(6 + (r() % 5)):
            _put(px, x, y, ink)
            x += dx; y += dy
            if (r() % 5) == 0:                 # a kink, not a zigzag
                if dx:
                    dx, dy = 0, (1 if (r() & 1) else -1)
                else:
                    dx, dy = (1 if (r() & 1) else -1), 0
    return im


def furrows(base, ink, crop, seed, period=4):
    """Two ploughed rows with crops standing in them. PLOUGHED FIELD — the one
    biome that should look man-made, so it gets the only regular pattern here.

    At period 3 with a dot every 3 rows this drew a lattice and read as CHAIN-LINK
    FENCE, not as a field. A field is a few widely spaced furrows with things
    growing between them, so: one dark furrow every four pixels, and the crop is an
    upright two-pixel mark (the same shape the grass uses) rather than a dot in a
    grid."""
    im, px = _new(base)
    r = _rng(seed)
    off = r() % period
    for x in range(TS):
        if (x + off) % period == 0:
            for y in range(TS):
                _put(px, x, y, ink)
    for _ in range(4):                          # crops between the furrows
        x = (r() % TS)
        if (x + off) % period == 0:
            x = (x + 1) % TS
        y = r() % TS
        _put(px, x, y, crop)
        _put(px, x, y + 1, crop)
    return im


def dimples(base, light, n, seed):
    """Very sparse soft marks. SNOW — snow is nearly featureless and the texture's
    job is only to stop a big white field looking like a hole in the screen."""
    im, px = _new(base)
    r = _rng(seed)
    for _ in range(n):
        x, y = r() % TS, r() % TS
        _put(px, x,     y, light)
        _put(px, x + 1, y, light)
    return im


def snowcap(base, snow, rock, seed):
    """Light on the upper edge, rock below. PEAKS — a mountain top reads as a peak
    because the snow sits on top of it, so the texture has to have an up."""
    im, px = _new(base)
    r = _rng(seed)
    for x in range(TS):
        h = 1 + ((r() % 3) if x % 2 else (r() % 2))
        for y in range(h):
            _put(px, x, y, snow)
    for _ in range(2):
        _put(px, r() % TS, 4 + (r() % 4), rock)
    return im


# ----------------------------------------------------------------- recipes ----
# (name, base, [variant builders]) — variant 0 is the plainest, and the engine's
# vweight makes it the common one, so a biome reads as its COLOUR first and its
# texture second. That ordering is what stops any of this becoming wallpaper.
#
# `None` means the master's own art is used instead and nothing is generated here;
# those live in extract_box.py's WAVE_FILLS and SYNC_TERRAIN.

def recipes():
    return [
        # --- ice and snow -------------------------------------------------
        ("bio_ice",   WHITE,  [lambda: cracks(WHITE, BLUE, 11, n=1),
                               lambda: cracks(WHITE, BLUE, 12, n=2),
                               lambda: grains(WHITE, BLUE, LTGREY, 2, 3, 13)],
                              [3, 2, 2]),
        ("bio_snow",  WHITE,  [lambda: dimples(WHITE, LTGREY, 2, 21),
                               lambda: dimples(WHITE, LTGREY, 3, 22),
                               lambda: grains(WHITE, LTGREY, PEACH, 3, 1, 23)],
                              [4, 2, 1]),
        ("bio_tundra", SLATE, [lambda: blades(SLATE, DKGREEN, NAVY, 3, 31),
                               lambda: blades(SLATE, DKGREEN, NAVY, 5, 32),
                               lambda: grains(SLATE, LTGREY, NAVY, 4, 3, 33)],
                              [3, 2, 2]),
        # --- sand ----------------------------------------------------------
        ("bio_beach", PEACH,  [lambda: grains(PEACH, WHITE, ORANGE, 5, 3, 41),
                               lambda: grains(PEACH, WHITE, ORANGE, 8, 4, 42),
                               lambda: ripple(PEACH, WHITE, 43)],
                              [3, 2, 2]),
        ("bio_desert", YELLOW,[lambda: ripple(YELLOW, ORANGE, 51),
                               lambda: ripple(YELLOW, ORANGE, 52, rows=(2, 6)),
                               lambda: grains(YELLOW, WHITE, ORANGE, 3, 5, 53)],
                              [3, 3, 2]),
        # --- green ---------------------------------------------------------
        ("bio_grass", DKGREEN,[lambda: blades(DKGREEN, GREEN, NAVY, 4, 61),
                               lambda: blades(DKGREEN, GREEN, NAVY, 7, 62),
                               lambda: grains(DKGREEN, GREEN, NAVY, 3, 2, 63)],
                              [3, 2, 2]),
        ("bio_savanna", ORANGE,[lambda: blades(ORANGE, DKGREEN, BROWN, 4, 71),
                               lambda: blades(ORANGE, DKGREEN, BROWN, 6, 72),
                               lambda: grains(ORANGE, YELLOW, BROWN, 4, 3, 73)],
                              [3, 2, 2]),
        ("bio_swamp", DKGREEN,[lambda: clumps(DKGREEN, BROWN, NAVY, 2, 81),
                               lambda: clumps(DKGREEN, BROWN, NAVY, 3, 82),
                               lambda: blades(DKGREEN, DKGREY, NAVY, 4, 83)],
                              [3, 2, 2]),
        # --- rock ----------------------------------------------------------
        ("bio_hill",  BROWN,  [lambda: clumps(BROWN, DKGREY, NAVY, 2, 91),
                               lambda: clumps(BROWN, DKGREY, NAVY, 3, 92),
                               lambda: grains(BROWN, ORANGE, DKGREY, 3, 3, 93)],
                              [3, 2, 2]),
        ("bio_mountain", DKGREY,[lambda: facets(DKGREY, LTGREY, NAVY, 101),
                                 lambda: facets(DKGREY, LTGREY, NAVY, 102),
                                 lambda: clumps(DKGREY, LTGREY, NAVY, 2, 103)],
                                [3, 2, 2]),
        ("bio_peak",  LTGREY, [lambda: snowcap(LTGREY, WHITE, DKGREY, 111),
                               lambda: snowcap(LTGREY, WHITE, DKGREY, 112),
                               lambda: facets(LTGREY, WHITE, DKGREY, 113)],
                              [3, 3, 2]),
        ("bio_rubble", LTGREY,[lambda: clumps(LTGREY, DKGREY, NAVY, 3, 121),
                               lambda: clumps(LTGREY, DKGREY, NAVY, 4, 122),
                               lambda: facets(LTGREY, WHITE, DKGREY, 123)],
                              [2, 2, 2]),
        # --- burnt ---------------------------------------------------------
        ("bio_ash",   DKGREY, [lambda: grains(DKGREY, LTGREY, BLACK, 4, 4, 131),
                               lambda: grains(DKGREY, LTGREY, BLACK, 7, 6, 132),
                               lambda: dimples(DKGREY, LTGREY, 3, 133)],
                              [3, 2, 2]),
        # one crack per tile, not two or three: at three the kinks overlapped into
        # thick junctions and the ground read as charcoal rubble rather than as
        # scorched earth with a split in it
        ("bio_scorched", MAROON,[lambda: cracks(MAROON, BLACK, 141, n=1),
                                 lambda: cracks(MAROON, BLACK, 142, n=1),
                                 lambda: grains(MAROON, DKGREY, BLACK, 3, 4, 143)],
                                [3, 2, 2]),
        # --- worked land ---------------------------------------------------
        ("bio_farm",  BROWN,  [lambda: furrows(BROWN, DKGREY, YELLOW, 151),
                               lambda: furrows(BROWN, DKGREY, GREEN, 152),
                               lambda: furrows(BROWN, DKGREY, YELLOW, 153, period=4)],
                              [3, 2, 2]),
    ]
