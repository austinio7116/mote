#!/usr/bin/env python3
"""DraftMote tile-library builder (step 2 of the art pipeline).

Slices each extracted reference room (assets/ref/rooms/roomNN.png, a 7x7
grid of ~32px tiles) into cells, downscales each to 16x16, and clusters
near-identical cells into a deduped, reusable tile library:

  assets/ref/tilelib.png     - the library, 16 tiles per row, numbered preview
  assets/ref/tilelib_big.png - 3x preview with indices for curation
  assets/ref/room_maps.json  - per room: 7x7 grid of library tile ids

The library + maps drive gen_rooms.py (step 3), which composes the game's
tiles.png and room layouts from curated tile ids.
"""
import os, json
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(os.path.dirname(HERE), "art_sources", "ref")
ROOMS = os.path.join(REF, "rooms")

GRID = 7
CELL = 16
THRESH = 22.0          # mean-abs-diff threshold on 8x8-smoothed tiles (0-255)

def snap565(arr):
    a = arr.astype(np.uint16)
    return np.stack(((a[..., 0] >> 3) << 3, (a[..., 1] >> 2) << 2, (a[..., 2] >> 3) << 3),
                    axis=-1).astype(np.uint8)

# skip the odd-sized crops (half-height hall segments + the double-tall bed)
SKIP = {20, 30, 31, 32, 33, 34, 35, 36, 37}

reps = []              # cluster representative arrays (float, 16x16)
keys = []              # 8x8 smoothed comparison keys
counts = []

def key_of(a16):
    im = Image.fromarray(a16.astype(np.uint8)).resize((8, 8), Image.LANCZOS)
    return np.asarray(im).astype(np.float32)
room_maps = {}

names = sorted(f for f in os.listdir(ROOMS) if f.endswith(".png"))
for fn in names:
    idx = int(fn[4:6])
    if idx in SKIP:
        continue
    im = Image.open(os.path.join(ROOMS, fn)).convert("RGB")
    w, h = im.size
    if not (200 < w < 250 and 200 < h < 250):
        continue
    grid = []
    for ty in range(GRID):
        row = []
        for tx in range(GRID):
            x0 = round(tx * w / GRID); x1 = round((tx + 1) * w / GRID)
            y0 = round(ty * h / GRID); y1 = round((ty + 1) * h / GRID)
            cell = im.crop((x0, y0, x1, y1)).resize((CELL, CELL), Image.LANCZOS)
            a = np.asarray(cell).astype(np.float32)
            k = key_of(a)
            best, bestd = -1, 1e9
            for ci, kk in enumerate(keys):
                d = np.abs(k - kk).mean()
                if d < bestd:
                    best, bestd = ci, d
            if bestd < THRESH:
                # fold into cluster (running mean keeps the representative stable)
                w0 = counts[best] / (counts[best] + 1.0)
                reps[best] = reps[best] * w0 + a / (counts[best] + 1.0)
                keys[best] = keys[best] * w0 + k / (counts[best] + 1.0)
                counts[best] += 1
                row.append(best)
            else:
                reps.append(a); keys.append(k); counts.append(1)
                row.append(len(reps) - 1)
        grid.append(row)
    room_maps[fn[:-4]] = grid

print("rooms mapped:", len(room_maps), " unique tiles:", len(reps))

with open(os.path.join(REF, "room_maps.json"), "w") as f:
    json.dump(room_maps, f)

# library sheets
PER = 16
rows = (len(reps) + PER - 1) // PER
lib = Image.new("RGB", (PER * CELL, rows * CELL), (0, 0, 0))
for i, r in enumerate(reps):
    t = Image.fromarray(snap565(np.clip(r, 0, 255)))
    lib.paste(t, ((i % PER) * CELL, (i // PER) * CELL))
lib.save(os.path.join(REF, "tilelib.png"))

S = 3
big = lib.resize((lib.width * S, lib.height * S), Image.NEAREST).convert("RGB")
d = ImageDraw.Draw(big)
for i in range(len(reps)):
    d.text(((i % PER) * CELL * S + 1, (i // PER) * CELL * S), str(i), fill=(255, 255, 0))
big.save(os.path.join(REF, "tilelib_big.png"))
print("wrote tilelib.png / tilelib_big.png")
