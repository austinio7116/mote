"""Preview the cost of the 8-layer ruleset stack: the SAME world, both palettes.

The engine's layered autotiler takes one byte per cell where each bit is a layer, so
there are eight layers and each has exactly one tileset. A cumulative stack (bit L set
for every cell at band L or above) is what makes a transition automatic — but it needs
each layer opaque in its interior, so a cell's colour becomes its LAYER's colour and
twenty biomes have to be bucketed into eight.

This renders the same real world (dumped with MOTEBOX_DUMPBIOME) with the twenty
distinct body colours it has now, and with the eight it would have merged, so the cost
is something you can look at rather than a number in a sentence.
"""
import os, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

MW, MH = 128, 112

NAVY = (29, 43, 83);      BLUE = (41, 173, 255);   WHITE = (255, 241, 232)
LTGREY = (194, 195, 199); DKGREY = (95, 87, 79);   BROWN = (171, 82, 54)
PEACH = (255, 204, 170);  YELLOW = (255, 236, 39); ORANGE = (255, 163, 0)
DKGREEN = (0, 135, 81);   GREEN = (0, 228, 54);    MAROON = (126, 37, 83)
RED = (255, 0, 77)

# B_* order from mb.h, index 1..23
B_NAMES = ["ocean", "sea", "shallow", "ice", "beach", "desert", "savanna", "grass",
           "swamp", "hill", "mountain", "peak", "tundra", "snow", "ash", "scorched",
           "lava", "acid", "farm", "rubble", "meadow", "forest", "road"]

# What each biome looks like TODAY — its own base tile's body colour.
NOW = {
    "ocean": NAVY, "sea": BLUE, "shallow": BLUE, "ice": WHITE, "beach": PEACH,
    "desert": YELLOW, "savanna": ORANGE, "grass": DKGREEN, "swamp": DKGREEN,
    "hill": BROWN, "mountain": NAVY, "peak": LTGREY, "tundra": BROWN, "snow": WHITE,
    "ash": DKGREY, "scorched": MAROON, "lava": MAROON, "acid": DKGREEN,
    "farm": BROWN, "rubble": LTGREY, "meadow": DKGREEN, "forest": DKGREEN,
    "road": DKGREY,
}

# The eight bands, bottom to top. Precedence is REORDERED so colour families are
# contiguous — it only has to be a consistent order, not physical elevation — but eight
# slots still force five merges, and two of them are visible: savanna loses its orange
# and mountain loses the navy the artist gave it.
BANDS = [
    ("deep water", NAVY,    ["ocean"]),
    ("water",      BLUE,    ["sea", "shallow"]),
    ("frost",      WHITE,   ["ice", "snow"]),
    ("sand",       PEACH,   ["beach", "desert"]),          # desert loses YELLOW
    ("green",      DKGREEN, ["grass", "swamp", "acid", "meadow", "forest"]),  # acid loses green
    ("dry",        BROWN,   ["savanna", "farm", "hill", "tundra"]),  # savanna loses ORANGE
    ("rock",       LTGREY,  ["mountain", "peak", "ash", "rubble", "road"]),   # mountain loses NAVY
    ("hot",        MAROON,  ["lava", "scorched"]),
]
MERGED = {}
for _label, col, members in BANDS:
    for m in members:
        MERGED[m] = col


def render(biome, palette, cols=MW, rows=MH):
    """One pixel a tile, which is enough to judge a palette: what is being compared is
    the COLOUR each terrain gets, not the shape of its edges — those are identical in
    both, because the same blob47 fringe draws them either way."""
    im = Image.new("RGB", (cols, rows), (0, 0, 0))
    px = im.load()
    for y in range(rows):
        for x in range(cols):
            t = biome[y * cols + x]
            if 1 <= t <= len(B_NAMES):
                px[x, y] = palette.get(B_NAMES[t - 1], (0, 0, 0))
    return im


def main(path):
    data = open(path, "rb").read()
    assert len(data) >= MW * MH, "expected a %d-byte dump" % (MW * MH)
    biome = data[:MW * MH]

    Z = 4
    a = render(biome, NOW).resize((MW * Z, MH * Z), Image.NEAREST)
    b = render(biome, MERGED).resize((MW * Z, MH * Z), Image.NEAREST)

    # and a difference map, so the cost is not a matter of opinion
    diff = Image.new("RGB", (MW, MH), (20, 20, 26))
    dpx = diff.load()
    changed = 0
    for y in range(MH):
        for x in range(MW):
            t = biome[y * MW + x]
            if not (1 <= t <= len(B_NAMES)):
                continue
            n = B_NAMES[t - 1]
            if NOW.get(n) != MERGED.get(n):
                dpx[x, y] = RED; changed += 1
            else:
                dpx[x, y] = DKGREY
    d3 = diff.resize((MW * Z, MH * Z), Image.NEAREST)

    W = MW * Z
    out = Image.new("RGB", (3 * W + 32, MH * Z + 30), (20, 20, 26))
    dr = ImageDraw.Draw(out)
    for i, (img, label) in enumerate(((a, "now: 20 biomes, 11 colours"),
                                      (b, "merged: 8 layers, 8 colours"),
                                      (d3, "red = cells that changed"))):
        out.paste(img, (8 + i * (W + 8), 6))
        dr.text((10 + i * (W + 8), MH * Z + 12), label, fill=(255, 220, 120))
    out.save("/tmp/motebox/130-merge-preview.png")
    print("changed %d of %d land+sea cells (%.1f%%)"
          % (changed, MW * MH, changed * 100.0 / (MW * MH)))
    print("wrote /tmp/motebox/130-merge-preview.png")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "biome.bin")
