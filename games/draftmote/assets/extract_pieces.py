#!/usr/bin/env python3
"""DraftMote tile + wall extractor (art pipeline step 2).

Harvests REUSABLE pieces from the reference room cards
(art_sources/ref/rooms/roomNN.png, ~224px cards, 32px art tiles):

  assets/floors.png          - 32x32 floor macro-tiles (= 2x2 game tiles), one
                               per theme, sampled from large clean floor areas
  assets/walls.png           - 16-cell edge16 rule sheet (16px) for the wall
                               ring: grey stone frame + cream north baseboard
  tilesets/wall.tileset      - the rule map for the Studio Tiles tab

Floor themes (column order = FLOOR_* ids in rooms.h):
  wood checker_beige checker_white bath_blue concrete dark_tile white_tile
  red_carpet grass grass_dark
"""
import os
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(os.path.dirname(HERE), "art_sources", "ref")
ROOMS = os.path.join(REF, "rooms")
GAME = os.path.dirname(HERE)

def load(idx):
    return Image.open(os.path.join(ROOMS, "room%02d.png" % idx)).convert("RGB")

def snap565(im):
    a = np.asarray(im).astype(np.uint16)
    a = np.stack(((a[..., 0] >> 3) << 3, (a[..., 1] >> 2) << 2, (a[..., 2] >> 3) << 3), axis=-1)
    return Image.fromarray(a.astype(np.uint8))

# ------------------------------------------------------------------ floors ---
# (room, x0, y0) of a clean 64x64 source patch -> 32x32 macro-tile.
# Patches picked from large open floor, aligned to the art's own pattern.
FLOORS = [
    ("wood",          23, 80, 120),
    ("checker_beige", 3,  76, 120),
    ("checker_white", 12, 80, 120),
    ("bath_blue",     17, 130, 130),
    ("concrete",      38, 130, 150),
    ("dark_tile",     41, 80, 110),
    ("white_tile",    40, 60, 130),
    ("red_carpet",    46, 80, 150),
]

def grass_macro(seed, dark=False):
    rng = np.random.RandomState(seed)
    base = np.array([72, 128, 58] if not dark else [58, 106, 50])
    g = np.tile(base, (32, 32, 1)).astype(np.int32) + rng.randint(-6, 7, (32, 32, 1))
    for _ in range(46):
        x, y = rng.randint(0, 32), rng.randint(0, 32)
        c = [92, 158, 72] if rng.rand() < 0.7 else [50, 88, 42]
        g[y, x] = c
        g[y, (x + 1) % 32] = c
    for _ in range(6):
        x, y = rng.randint(0, 32), rng.randint(0, 32)
        g[y, x] = [150, 140, 84]
    return Image.fromarray(np.clip(g, 0, 255).astype(np.uint8))

def blend_seams(im):
    """soften the wrap seam of a macro-tile by cross-fading 3px borders"""
    a = np.asarray(im).astype(np.float32)
    n = a.shape[0]
    for i in range(3):
        w = (i + 1) / 4.0
        a[i, :] = a[i, :] * w + a[n - 1 - i, :] * (1 - w) * 0.5 + a[i, :] * (1 - w) * 0.5
        a[:, i] = a[:, i] * w + a[:, n - 1 - i] * (1 - w) * 0.5 + a[:, i] * (1 - w) * 0.5
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))

sheet = Image.new("RGB", ((len(FLOORS) + 2) * 32, 32), (0, 0, 0))
for i, (name, room, x, y) in enumerate(FLOORS):
    src = load(room).crop((x, y, x + 64, y + 64)).resize((32, 32), Image.LANCZOS)
    sheet.paste(snap565(blend_seams(src)), (i * 32, 0))
sheet.paste(snap565(grass_macro(7)), (len(FLOORS) * 32, 0))
sheet.paste(snap565(grass_macro(8, dark=True)), ((len(FLOORS) + 1) * 32, 0))
sheet.save(os.path.join(HERE, "floors.png"))
print("wrote floors.png (%d themes)" % (len(FLOORS) + 2))

# ------------------------------------------------------------------- walls ---
# Harvest the ring pieces from a clean hall card. The art's wall band is
# ~30px grey + ~14px cream (north only); we take 32px strips positioned so a
# 16px game tile reads as frame + baseboard.
W_ROOM = 23
wim = load(W_ROOM)
w, h = wim.size

def strip(x0, y0):
    return snap565(wim.crop((x0, y0, x0 + 32, y0 + 32)).resize((16, 16), Image.LANCZOS))

# ring pieces: corners sit at the card corners; edges from mid-runs.
NW = strip(0, 12);  NN = strip(96, 12);  NE = strip(w - 32, 12)
WW = strip(0, 96);  EE = strip(w - 32, 96)
SW = strip(0, h - 34); SS = strip(96, h - 34); SE = strip(w - 32, h - 34)
FILL = strip(96, 4)   # solid grey top band as the isolated/filler cell

# edge16 sheet: cell index = mask of SAME-TERRAIN neighbours (N=1 E=2 S=4 W=8).
# For a rectangular ring: top edge has E+W -> 10, left edge N+S -> 5, corners
# two bits, crossings for safety mapped to nearest look.
cells = {
    0: FILL,
    1: WW, 4: WW, 5: WW,           # N, S, N+S -> vertical run
    2: NN, 8: NN, 10: NN,          # E, W, E+W -> horizontal run
    6: NW, 12: NE,                 # E+S corner (top-left), S+W (top-right)
    3: SW, 9: SE,                  # N+E (bottom-left), N+W (bottom-right)
    7: WW, 13: WW,                 # T-pieces fall back to runs
    11: NN, 14: NN,
    15: FILL,
}
ws = Image.new("RGB", (4 * 16, 4 * 16), (0, 0, 0))
for idx in range(16):
    ws.paste(cells[idx], ((idx % 4) * 16, (idx // 4) * 16))
ws.save(os.path.join(HERE, "walls.png"))
print("wrote walls.png")

os.makedirs(os.path.join(GAME, "tilesets"), exist_ok=True)
lut = []
for m in range(256):
    c = 0
    if m & 1: c |= 1      # N
    if m & 4: c |= 2      # E
    if m & 16: c |= 4     # S
    if m & 64: c |= 8     # W
    lut.append(c)
with open(os.path.join(GAME, "tilesets", "wall.tileset"), "w") as f:
    f.write("sheet assets/walls.png\n")
    f.write("tile 16\ntype 1\nedge 1\nnvar 1\ncols 4\nrows 4\n")
    f.write("lut " + " ".join(str(v) for v in lut) + "\n")
    f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
    f.write("vweight 1 1 1 1 1 1 1 1\n")
print("wrote tilesets/wall.tileset")
