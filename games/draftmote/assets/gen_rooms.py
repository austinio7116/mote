#!/usr/bin/env python3
"""DraftMote room-art generator (step 3 of the art pipeline).

Turns the extracted reference room cards (assets/ref/rooms/roomNN.png — the
game's custom art sheet) into the shipped room interiors:

  assets/rooms/<name>.png   112x112, <=255 colours (bakes 8bpp indexed)
  assets/doors.png          door overlays: h/v closed+open, gold closed+open
  src/room_masks.h          14x14 8px collision cells per room, from the art

Rooms the sheet doesn't cover (gardens, vault, antechamber) are COMPOSED from
harvested pieces (donor wall frames, floor patches, the hall plant) in the same
style. Edit the source cards or this mapping, then re-run + `mote bake`.

Run after extract_rooms.py:  python3 assets/gen_rooms.py
"""
import os
import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(os.path.dirname(HERE), "art_sources", "ref")
ROOMS = os.path.join(REF, "rooms")
OUT = os.path.join(HERE, "rooms")
os.makedirs(OUT, exist_ok=True)

SZ = 112                     # game room viewport (7x7 tiles of 16px)
CG = 8                       # collision cell size in px
CN = SZ // CG                # 14x14 cells

def load(idx):
    return Image.open(os.path.join(ROOMS, "room%02d.png" % idx)).convert("RGB")

def shrink(im):
    return im.resize((SZ, SZ), Image.LANCZOS)

def quant(im):
    q = im.quantize(colors=255, method=Image.MEDIANCUT).convert("RGB")
    return q

def clean_label(im):
    """the sheet bakes a name label into each card's bottom-right; the floor
    there is near-symmetric, so clone the clean bottom-left over it"""
    a = np.asarray(im).copy()
    f = np.asarray(im.transpose(Image.FLIP_LEFT_RIGHT))
    a[84:106, 58:108] = f[84:106, 58:108]
    return Image.fromarray(a)

# ---------------------------------------------------------------- mapping ----
# game room -> sheet card (None = composed below). Mirrors use ("mirror", idx).
SHEET = {
    "entrance":   22,   # hall, blue rug
    "hallway":    23,
    "wpass":      24,   # hall with plant
    "epass":      ("mirror", 24),
    "foyer":      25,   # hall with floor voids
    "greathall":  26,
    "lounge":     0,    # red sofas on green rug
    "drawing":    10,   # green sofa lounge
    "dining":     15,   # green chairs, blue rug
    "kitchen":    3,    # checker + hob
    "pantry":     41,   # dark kitchen
    "bedroom":    5,    # blue bed
    "suite":      7,    # purple bed
    "washroom":   17,   # big tub
    "library":    28,   # bookshelf walls
    "study":      42,   # green desk office
    "drafting":   45,   # office with the estate map on the desk
    "closet":     40,   # washer + white tile
    "storeroom":  47,   # red room, racks of gear
    "garage":     38,   # the car
    "locksmith":  39,   # utility workshop
    "commissary": 11,   # kitchen counters as the shop counter
    "stairwell":  46,   # red stairs up
    "darkroom":   44,   # dark blue room
}
COMPOSED = ["terrace", "garden", "conservatory", "vault", "antechamber"]
ORDER = ["entrance", "antechamber", "hallway", "wpass", "epass", "foyer",
         "greathall", "lounge", "drawing", "dining", "kitchen", "pantry",
         "bedroom", "suite", "washroom", "library", "study", "drafting",
         "closet", "storeroom", "garage", "locksmith", "commissary",
         "stairwell", "darkroom", "terrace", "garden", "conservatory", "vault"]

# ------------------------------------------------------------- composition ---
def key_crop(im, box, samples):
    """crop `box` from a room; alpha out everything close to the sampled
    background colours (for lifting props like the plant off the floor)"""
    a = np.asarray(im.crop(box)).astype(np.int32)
    px = np.concatenate([np.asarray(im.crop(r)).reshape(-1, 3) for r in samples]).astype(np.int32)
    pal = np.unique(px // 20, axis=0) * 20 + 10
    d = np.full(a.shape[:2], 1e9)
    for c in pal:
        d = np.minimum(d, np.abs(a - c).sum(axis=2))
    m = d > 46
    m = ndimage.binary_opening(m, np.ones((2, 2)))
    m = ndimage.binary_fill_holes(m)
    return Image.fromarray(np.dstack([a.astype(np.uint8), (m * 255).astype(np.uint8)]), "RGBA")

def grass_patch(w, h, seed, dark=False):
    rng = np.random.RandomState(seed)
    base = np.array([72, 128, 58] if not dark else [58, 106, 50])
    g = np.tile(base, (h, w, 1)).astype(np.int32)
    n = rng.randint(-7, 8, (h, w, 1))
    g = g + n
    # speckle blades + soil flecks in the sheet's chunky style
    for _ in range(w * h // 22):
        x, y = rng.randint(0, w), rng.randint(0, h)
        c = [92, 158, 72] if rng.rand() < 0.7 else [50, 88, 42]
        g[y, x] = c
        if x + 1 < w: g[y, x + 1] = c
    for _ in range(w * h // 160):
        x, y = rng.randint(0, w), rng.randint(0, h)
        g[y, x] = [190, 170, 90] if rng.rand() < 0.4 else [120, 96, 60]
    return Image.fromarray(np.clip(g, 0, 255).astype(np.uint8))

def donor_frame(idx=23):
    """a hall card whose interior we replace: returns (image, interior box)"""
    im = clean_label(shrink(load(idx)))
    return im.copy(), (9, 9, SZ - 9, SZ - 9)   # inside the wall frame

# the hall plant, lifted off the wood floor (source-scale coords in room24)
_r24 = load(24)
PLANT = key_crop(_r24, (160, 10, 218, 72),
                 [(60, 150, 130, 190), (30, 100, 60, 140)])

def compose_green(name, seed, plants, dark=False):
    im, (x0, y0, x1, y1) = donor_frame()
    im.paste(grass_patch(x1 - x0, y1 - y0, seed, dark), (x0, y0))
    for (px, py, s, fl) in plants:
        p = PLANT.resize((int(PLANT.width * s / 2), int(PLANT.height * s / 2)), Image.LANCZOS)
        if fl:
            p = p.transpose(Image.FLIP_LEFT_RIGHT)
        im.paste(p, (px, py), p)
    return im

def compose_vault():
    im, (x0, y0, x1, y1) = donor_frame(39)      # utility: grey floor + racks
    a = np.asarray(im).astype(np.int32)
    # cold navy cast over everything inside the frame
    inner = a[y0:y1, x0:x1]
    inner[:] = (inner * np.array([0.72, 0.75, 0.95]) + np.array([0, 2, 18])).astype(np.int32)
    im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
    rng = np.random.RandomState(9)
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    for _ in range(26):                          # scattered gold
        x, y = rng.randint(x0 + 6, x1 - 8), rng.randint(y0 + 26, y1 - 8)
        d.ellipse((x, y, x + 3, y + 2), fill=(214, 176, 60), outline=(120, 92, 26))
        d.point((x + 1, y), fill=(255, 232, 140))
    return im

def compose_ante():
    im = clean_label(shrink(load(26)))           # the great hall
    a = np.asarray(im).astype(np.int32)
    # gild it: warm golden wash, brightest at the top
    for y in range(SZ):
        w = 0.30 + 0.12 * (1.0 - y / SZ)
        a[y] = a[y] * (1 - w) + np.array([236, 196, 96]) * w
    im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    cx, cy = SZ // 2, SZ // 2 + 6                # floor medallion
    for r, c in ((16, (168, 130, 44)), (13, (214, 176, 76)), (8, (240, 210, 120))):
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=c, width=2)
    d.ellipse((cx - 3, cy - 3, cx + 3, cy + 3), fill=(255, 236, 160))
    return im

# ------------------------------------------------------------- collisions ----
# per-room extra floor sample rects (game-scale), for rugs/odd floors
EXTRA_SAMPLES = {
    "entrance":  [(25, 78, 39, 92)],       # blue rug (clear of the table)
    "dining":    [(20, 78, 34, 88)],
    "lounge":    [(46, 50, 62, 60)],       # green rug (under the coffee table edge)
    "drawing":   [(40, 60, 70, 72)],
    "bedroom":   [(30, 62, 46, 76)],       # bedroom rug beside the bed
    "suite":     [(30, 62, 46, 76)],
    "stairwell": [(30, 30, 80, 60)],       # the stairs are walkable
}
DEFAULT_SAMPLES = [(46, 86, 66, 98), (12, 84, 26, 96), (86, 84, 100, 96)]

# stubborn furniture the colour key can't separate from the floor (cell coords)
EXTRA_SOLID = {
    # the entrance coffee table is wood-on-rug — same browns as the floor
    "entrance": [(cx, cy) for cx in range(4, 10) for cy in range(6, 10)],
}

def collision_mask(im, name):
    a = np.asarray(im).astype(np.int32)
    rects = DEFAULT_SAMPLES + EXTRA_SAMPLES.get(name, [])
    px = np.concatenate([a[y0:y1, x0:x1].reshape(-1, 3) for (x0, y0, x1, y1) in rects])
    # drop the darkest 20% (plank crevices) — they'd absorb dark furniture;
    # the thin real crevices lose the per-cell vote anyway
    luma = px.sum(axis=1)
    px = px[luma > np.percentile(luma, 20)]
    pal = np.unique(px // 20, axis=0) * 20 + 10
    d1 = np.full((SZ, SZ), 1e9)     # L1 to nearest floor colour
    dm = np.full((SZ, SZ), 1e9)     # channel-max to nearest floor colour
    for c in pal:
        diff = np.abs(a - c)
        d1 = np.minimum(d1, diff.sum(axis=2))
        dm = np.minimum(dm, diff.max(axis=2))
    solid_px = (d1 > 44) & (dm > 24)
    cells = np.zeros((CN, CN), dtype=bool)
    for cy in range(CN):
        for cx in range(CN):
            frac = solid_px[cy * CG:(cy + 1) * CG, cx * CG:(cx + 1) * CG].mean()
            cells[cy, cx] = frac > 0.40
    for (cx, cy) in EXTRA_SOLID.get(name, []):
        cells[cy, cx] = True
    cells[0, :] = cells[-1, :] = True            # wall ring is always solid
    cells[:, 0] = cells[:, -1] = True
    # door approaches stay clear: 2 cells inside each mid-edge
    for (cx, cy) in ((6, 1), (7, 1), (6, 12), (7, 12), (1, 6), (1, 7), (12, 6), (12, 7)):
        cells[cy, cx] = False
    return cells

# ------------------------------------------------------------------- main ----
masks = {}
for name in ORDER:
    src = SHEET.get(name)
    if src is None:
        if name == "terrace":
            im = compose_green(name, 11, [(14, 12, 2, 0), (84, 12, 2, 1)])
        elif name == "garden":
            im = compose_green(name, 22, [(12, 66, 2, 0), (82, 64, 2, 1), (48, 10, 2, 0)])
        elif name == "conservatory":
            im = compose_green(name, 33, [(12, 12, 2, 0), (80, 14, 2, 1), (16, 68, 1, 0)], dark=True)
        elif name == "vault":
            im = compose_vault()
        else:
            im = compose_ante()
    elif isinstance(src, tuple):
        im = clean_label(shrink(load(src[1]))).transpose(Image.FLIP_LEFT_RIGHT)
    else:
        im = clean_label(shrink(load(src)))
    if name == "storeroom":
        # the sheet's decorative sparkle bled into this card's top-left; the
        # room is near-symmetric, so clone the clean right side over it
        a = np.asarray(im).copy()
        a[0:34, 0:32] = np.asarray(im.transpose(Image.FLIP_LEFT_RIGHT))[0:34, 0:32]
        im = Image.fromarray(a)
    im = quant(im)
    im.save(os.path.join(OUT, name + ".png"))
    masks[name] = collision_mask(im, name)
print("rooms written:", len(ORDER))

# ---- src/room_masks.h ----
lines = ["/* GENERATED by assets/gen_rooms.py — collision cells (14x14 of 8px) derived",
         " * from the room art; bit x of row y = cell solid. Edit the generator, not this. */",
         "#ifndef DRAFT_ROOM_MASKS_H", "#define DRAFT_ROOM_MASKS_H", "#include <stdint.h>", ""]
lines.append("static const uint16_t k_room_masks[][14] = {")
for name in ORDER:
    m = masks[name]
    rows = ", ".join("0x%04X" % int("".join("1" if m[y, x] else "0" for x in range(CN))[::-1], 2)
                     for y in range(CN))
    lines.append("    /* %-12s */ { %s }," % (name, rows))
lines.append("};")
lines.append("")
lines.append("#endif")
with open(os.path.join(os.path.dirname(HERE), "src", "room_masks.h"), "w") as f:
    f.write("\n".join(lines) + "\n")
print("wrote src/room_masks.h")

# ---- mask debug sheet: art with solid cells tinted red ----
cols_n = 8
rows_n = (len(ORDER) + cols_n - 1) // cols_n
dbg = Image.new("RGB", (cols_n * (SZ + 4), rows_n * (SZ + 14)), (18, 22, 40))
from PIL import ImageDraw
dd = ImageDraw.Draw(dbg)
for i, name in enumerate(ORDER):
    im = Image.open(os.path.join(OUT, name + ".png")).convert("RGB")
    a = np.asarray(im).astype(np.int32)
    m = masks[name]
    for cy in range(CN):
        for cx in range(CN):
            if m[cy, cx]:
                a[cy * CG:(cy + 1) * CG, cx * CG:(cx + 1) * CG, 0] = np.minimum(
                    a[cy * CG:(cy + 1) * CG, cx * CG:(cx + 1) * CG, 0] + 90, 255)
    ox, oy = (i % cols_n) * (SZ + 4) + 2, (i // cols_n) * (SZ + 14) + 12
    dbg.paste(Image.fromarray(a.astype(np.uint8)), (ox, oy))
    dd.text((ox, oy - 11), name, fill=(255, 255, 0))
dbg.save(os.path.join(REF, "masks_debug.png"))
print("wrote", os.path.join(REF, "masks_debug.png"))

# ------------------------------------------------------------ door overlays --
# 16x16 cells: h_closed h_open v_closed v_open gold_closed gold_open
D = Image.new("RGBA", (6 * 16, 16), (0, 0, 0, 0))
door_leaf = Image.open(os.path.join(REF, "door0.png")).convert("RGB")
leaf = door_leaf.resize((12, 16), Image.LANCZOS)

def frame_cell(fill=None):
    c = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    from PIL import ImageDraw
    d = ImageDraw.Draw(c)
    d.rectangle((0, 0, 15, 15), fill=(58, 48, 40, 255))
    d.rectangle((1, 0, 14, 15), fill=fill or (24, 18, 14, 255))
    return c

h_closed = frame_cell()
h_closed.paste(leaf.resize((12, 15), Image.LANCZOS), (2, 1))
h_open = frame_cell((26, 20, 16, 255))
hd = np.asarray(h_open).copy(); hd[12:16, 2:14] = (44, 34, 26, 255)
h_open = Image.fromarray(hd)

def gold_tint(im):
    a = np.asarray(im.convert("RGBA")).astype(np.int32)
    a[..., :3] = np.clip(a[..., :3] * np.array([1.25, 1.05, 0.45]) + np.array([40, 24, 0]), 0, 255)
    return Image.fromarray(a.astype(np.uint8))

g_closed = gold_tint(h_closed)
g_open = frame_cell((255, 244, 205, 255))

for i, c in enumerate([h_closed, h_open,
                       h_closed.transpose(Image.ROTATE_90), h_open.transpose(Image.ROTATE_90),
                       g_closed, g_open]):
    D.paste(c.resize((16, 16), Image.LANCZOS) if c.size != (16, 16) else c, (i * 16, 0))
D.save(os.path.join(HERE, "doors.png"))
print("wrote doors.png")
