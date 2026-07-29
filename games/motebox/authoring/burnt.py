"""Motebox — what a fire LEAVES: ash, scorched earth, and cooling lava.

Three of these used to be one 8x8 cell stamped per world cell, and the result was
"it looks like black squares". Both halves of that are true and both are fixed here.

BLACK. Scorched earth was the artist's MAROON with RED dashes, which drew as bright pink
brickwork, so it was replaced with charcoal — and charcoal at one flat tone is a black
square. Burnt ground is not one colour: it is pale mineral ash where the fine fuel went,
soot where the heat sat, bare baked earth showing through, and embers that have not gone
out yet. Four tones and a scatter, and it reads as a surface.

SQUARES. A per-cell opaque sprite pins the patch boundary to the tile grid, so a burn scar
is a rectilinear blob whatever its texture. These are BLOB 47 sets: the cell is chosen from
which of its eight neighbours are the same material, and every side that is NOT gets an
organic fringe eaten out of it — the same machinery the terrain bands use, so a burn scar
has the same kind of edge as a coastline and the ground it burnt shows through at the rim.

ASH and SCORCHED are deliberately different materials, because they mean different things:
vegetation burnt to mineral ash (pale, soft, and it will heal back to grass) against ground
baked by lava or a swamp fire (dark, hard, slow). Telling them apart on sight is worth more
than either one looking good alone.
"""
import os, sys
from PIL import Image

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
sys.path.insert(0, os.path.abspath(os.path.join(_HERE, "..", "..", "roguemote", "authoring")))
import blob47                                                  # noqa: E402
import bands                                                   # noqa: E402

TS = bands.TS
COLS, ROWS = bands.COLS, bands.ROWS


# Each entry: body, then the marks laid over it as (colour, count, size).
#
# Body first is the rule the transition sets were fixed by — the lip tones are what
# bands.cell() lays inward from the fringe, so tones[0] has to be the material itself.
#
# AND THE LIP GOES DARK, which is the opposite of what the terrain bands do. A band
# brightens toward its own edge because that is what a shore does with light; a burn does
# the reverse — the rim is where the fire stopped, so it is the most charred part of it.
# The first version used the band convention and put a bright white halo around every scar
# and around every unburnt island inside it. Lava is the same story for a different reason:
# a flow's edge is cooled crust, so its rim is nearly black and only the interior glows.
MATERIALS = {
    # Vegetation gone to mineral ash: pale, powdery, with soot where it burnt hottest
    # and the odd ember still alive in it.
    "ash": {
        "tones": [(108, 100, 96), (58, 52, 50), (80, 74, 70)],
        "marks": [((72, 66, 64), 5, 2),        # soot
                  ((182, 176, 168), 4, 1),     # fine ash catching the light
                  ((150, 74, 44), 1, 1)],      # one ember, so it reads as recent
    },
    # Ground baked hard — a swamp fire's floor, or where lava sat. Dark, but never flat:
    # cracks open on dull ember and dry earth shows through the crust.
    "scorch": {
        "tones": [(70, 52, 46), (34, 26, 24), (50, 38, 34)],
        "marks": [((38, 28, 28), 6, 2),        # soot in the hollows
                  ((132, 62, 38), 2, 1),       # a crack still glowing
                  ((104, 76, 58), 4, 1)],      # baked earth
    },
    # Molten rock. The only one of the three that is a LIGHT SOURCE, so its own tones run
    # the other way: the body is the hot interior and the fringe cools toward crust.
    "lava": {
        "tones": [(255, 106, 0), (76, 26, 22), (150, 44, 20)],
        "marks": [((196, 46, 12), 5, 2),       # crust islands floating in it
                  ((255, 236, 120), 3, 1),     # the brightest cracks
                  ((120, 30, 20), 2, 1)],      # a cooled raft
    },
}

ORDER = ("lava", "scorch", "ash")      # row blocks in the sheet, so C can index them


def _interior(spec, seed):
    """A textured body. Marks are placed on a per-variant hash and WRAPPED, so a patch of
    any size is continuous and no mark is clipped at a cell edge (a clipped mark reads as
    a line on the grid, which is the thing being fixed)."""
    tones = spec["tones"]

    def fn(profile):
        im = Image.new("RGBA", (TS, TS), tones[0] + (255,))
        px = im.load()
        h = (seed * 2654435761 + profile * 2246822519) & 0xFFFFFFFF
        for col, count, size in spec["marks"]:
            for _ in range(count):
                h = (h * 1103515245 + 12345) & 0xFFFFFFFF
                x, y = (h >> 9) % TS, (h >> 17) % TS
                w = size + ((h >> 25) & 1 if size > 1 else 0)
                for dy in range(size):
                    for dx in range(w):
                        px[(x + dx) % TS, (y + dy) % TS] = col + (255,)
        return im
    return fn


def build(nvar=3):
    """One sheet, three 47-cell blob sets stacked in ORDER, `nvar` variants each.

    Layout is (block * nvar + variant) row-blocks of ROWS rows, so the C side finds
    material M variant V at row (M * nvar + V) * ROWS. Three variants because a burn scar
    is large and flat: with two, the position hash alternates one texture against one
    other and the eye picks the lattice out immediately.
    """
    sheet = Image.new("RGBA", (COLS * TS, ROWS * TS * nvar * len(ORDER)), (0, 0, 0, 0))
    for bi, name in enumerate(ORDER):
        spec = MATERIALS[name]
        for v in range(nvar):
            interior = _interior(spec, 7919 + bi * 131 + v * 17)
            for ci, mask in enumerate(blob47.CANON):
                cell = bands.cell(mask, spec["tones"], interior, profile=v)
                y0 = ((bi * nvar + v) * ROWS + ci // COLS) * TS
                sheet.paste(cell, ((ci % COLS) * TS, y0))
    return sheet


def write_lut(path):
    """The blob47 mask -> cell table, in the SAME sense the sets are built in: a set bit
    means the material continues at that neighbour.

    Not to be confused with MB_BLOB47 in blob47_lut.h, which the transition overlays
    generated with the bit sense INVERTED (there a set bit meant the higher terrain was
    that way). Two tables that look alike and mean opposite things is a trap, so this one
    is named for what it means.
    """
    with open(path, "w") as f:
        f.write("/* GENERATED by authoring/burnt.py from roguemote's blob47.py "
                "— do not edit.\n *\n"
                " * Mask bits: N=1 NE=2 E=4 SE=8 S=16 SW=32 W=64 NW=128, where a set bit\n"
                " * means THE SAME MATERIAL CONTINUES at that neighbour. Value is a cell\n"
                " * index 0..46 within one %d x %d block.\n */\n" % (COLS, ROWS))
        f.write("#ifndef MB_BLOB47_SAME_H\n#define MB_BLOB47_SAME_H\n\n")
        f.write("#define MB_B47_COLS %d\n#define MB_B47_ROWS %d\n\n" % (COLS, ROWS))
        f.write("static const uint8_t MB_B47_SAME[256] = {\n")
        for i in range(0, 256, 16):
            f.write("    " + ",".join("%3d" % v for v in blob47.LUT[i:i + 16]) + ",\n")
        f.write("};\n\n#endif\n")
