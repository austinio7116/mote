#!/usr/bin/env python3
"""Render a synthetic coastline through the engine's own mask->cell logic.

The point is to see REAL TRANSITIONS. A preview that renders one tile repeated tells
you nothing about a tileset whose whole job is edges, and that is exactly how two
versions of the biome art got shipped looking like squares butted together.
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import terrain

GAME = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = "/tmp/motebox_terrain.png"

NAMES = ["bio_ocean", "bio_sea", "bio_shallow", "bio_beach", "bio_grass", "bio_savanna",
         "bio_hill", "bio_mountain", "bio_peak", "bio_farm", "bio_ash", "bio_lava",
         "bio_snow", "bio_swamp", "bio_tundra", "bio_scorched", "bio_ice", "bio_desert",
         "bio_rubble", "bio_acid"]

cols, rows = 40, 28
ids = [[None] * cols for _ in range(rows)]
for y in range(rows):
    for x in range(cols):
        h = x * 0.55 + y * 0.25 + 3 * math.sin(x * 0.4) + 2 * math.cos(y * 0.5)
        ids[y][x] = ("bio_ocean" if h < 4 else "bio_sea" if h < 7 else "bio_shallow" if h < 9
                     else "bio_beach" if h < 11 else "bio_grass" if h < 15
                     else "bio_savanna" if h < 18 else "bio_hill" if h < 22
                     else "bio_mountain" if h < 26 else "bio_peak")
# patches that exercise the man-made, burnt and frozen edges
for y in range(6, 11):
    for x in range(16, 24): ids[y][x] = "bio_farm"
for y in range(14, 19):
    for x in range(6, 13): ids[y][x] = "bio_ash"
for y in range(20, 24):
    for x in range(28, 33): ids[y][x] = "bio_lava"
for y in range(2, 6):
    for x in range(30, 37): ids[y][x] = "bio_snow"
for y in range(22, 27):
    for x in range(4, 11): ids[y][x] = "bio_swamp"

terrain.render_map(os.path.join(GAME, "tilesets"), ids, NAMES, cols, rows, scale=4).save(OUT)
print("wrote", OUT)
