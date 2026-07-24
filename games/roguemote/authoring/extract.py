#!/usr/bin/env python3
"""
Extract clean thematic sub-sprite-sheets + terrain autotile sheets from the CC0
Simple Roguelike Tileset (source_tileset.png, 64x64 grid of 8x8 tiles).

This script IS the editable authoring step. It writes:
  - assets/sheets/<name>.png        thematic sprite subsheets (RGBA, idx0 -> transparent)
  - tilesets/<name>.tileset + sheet  Mote autotile rulesets (nine-slice / floor)
  - assets/font/rogue8.png           CP437 8x8 font atlas (for MoteFont bake)
Run `mote bake games/roguemote` afterwards to produce the src/*.h headers.

Coordinates are (col,row) in 8px tile units, per assets/CATALOGUE.md.
"""
import os
from PIL import Image

HERE   = os.path.dirname(os.path.abspath(__file__))           # games/roguemote/authoring
GAME   = os.path.dirname(HERE)                                # games/roguemote
ASSETS = os.path.join(GAME, "assets")
SRC  = Image.open(os.path.join(HERE, "source_tileset.png"))   # palette 'P' mode
IDX  = SRC.load()
PAL  = SRC.getpalette()
W, H = SRC.size
TS   = 8
TRANSPARENT_IDX = 0   # palette index 0 = pure black = tileset's designated transparency

def _rgba_px(ix):
    if ix == TRANSPARENT_IDX:
        return (0, 0, 0, 0)
    return (PAL[ix*3], PAL[ix*3+1], PAL[ix*3+2], 255)

def tile_img(c, r):
    """One 8x8 tile as RGBA, idx0 -> transparent."""
    im = Image.new("RGBA", (TS, TS))
    px = im.load()
    for y in range(TS):
        for x in range(TS):
            px[x, y] = _rgba_px(IDX[c*TS + x, r*TS + y])
    return im

def tile_opaque(c, r):
    """One 8x8 tile as RGBA, keeping idx0 as opaque black (for terrain fills)."""
    im = Image.new("RGBA", (TS, TS))
    px = im.load()
    for y in range(TS):
        for x in range(TS):
            ix = IDX[c*TS + x, r*TS + y]
            px[x, y] = (PAL[ix*3], PAL[ix*3+1], PAL[ix*3+2], 255)
    return im

def region(c0, r0, c1, r1, keep_black=False):
    """Crop a rectangle of tiles (inclusive), preserving layout. RGBA."""
    cols, rows = c1-c0+1, r1-r0+1
    out = Image.new("RGBA", (cols*TS, rows*TS))
    f = tile_opaque if keep_black else tile_img
    for r in range(rows):
        for c in range(cols):
            out.paste(f(c0+c, r0+r), (c*TS, r*TS))
    return out

def save_sheet(name, img):
    d = os.path.join(ASSETS, "sheets"); os.makedirs(d, exist_ok=True)
    p = os.path.join(d, name + ".png"); img.save(p)
    print(f"[sheet] {name:22s} {img.width//TS}x{img.height//TS} tiles -> {p}")

# ---------------------------------------------------------------------------
# Sprite subsheets — rectangles that preserve the source's meaningful layout.
# Many families are 5-colour column blocks (rows = red/blue/yellow/green/grey).
# ---------------------------------------------------------------------------
SPRITE_SHEETS = {
    # --- dungeon objects (cols 0-15) ---
    "chests":            (0, 1, 1, 5),     # closed / open x 5 colours
    "furniture_stone":   (2, 1, 4, 5),     # tables / altars / beds x 5 colours
    "faces_skulls_keys": (5, 1, 7, 5),     # creature face / skull / key x 5 colours
    "runes":             (8, 1, 10, 4),    # colour rune glyphs
    "doors_gems_banners":(11, 0, 14, 5),   # doors, big gems/slimes, banners, ladders
    "props_light":       (0, 6, 9, 9),     # terminals, logs, planks, fences, bridges, house, igloo, torches
    "trinkets":          (0, 8, 15, 13),   # slimes, gems, mushrooms, snakes, rocks, clouds, bushes, books
    # --- items (cols 16-31) ---
    "weapons_potions":   (16, 0, 23, 5),   # swords, spears, bows + potions/flasks
    "food":              (16, 8, 27, 13),  # meat, fruit, bread, pie, egg + chef
    "treasure_ore":      (16, 16, 27, 19), # gold/silver/copper ore, gems, crown, armour/helmets
    "loot_furniture":    (16, 20, 24, 23), # table, barrels, chalice, bones, shield
    "tools_wands":       (16, 28, 31, 31), # pickaxes, axes, hammers, staffs, wands
    "weapons_elemental": (16, 32, 30, 34), # ice/fire/colour swords & staves
    "guns":              (16, 35, 22, 35), # pistols/rifles
    # --- characters & monsters (cols 32-47) ---
    "items_scroll_bomb": (24, 0, 25, 1),   # scroll, arrow, small bomb, large bomb
    "characters":        (32, 0, 47, 7),   # @, portraits, adventurers, villagers
    "animals":           (32, 8, 47, 15),  # beasts, vermin, birds, frogs
    "monsters":          (32, 16, 47, 23), # cows, goblins, demons, ghosts
    "crowns_fx":         (32, 24, 40, 31), # crowns, armour, slimes, fire/explosions
    "bosses":            (32, 32, 47, 39), # 2x2 large enemies
    # --- terrain feature props ---
    "boulders_mountains":(5, 25, 10, 28),  # boulders, snow peaks
    # --- UI / HUD (cols 48-63, lower) ---
    "ui_icons_tiny":     (0, 14, 15, 15),  # magenta-bg strip: hearts, eyes, stars, drops, fire, notes
    "ui_arrows_gauges":  (48, 16, 59, 19), # 8-dir arrows + bars/gauges
    "ui_buttons":        (48, 20, 63, 23), # A/B/X/Y + O/X face buttons
    "ui_status_emotes":  (48, 24, 63, 31), # faces, check/x, element icons, hearts, speech bubbles
    "ui_symbols":        (48, 32, 51, 32), # gender/alchemy symbols
    # --- white / blueprint set ---
    "furniture_white":   (0, 53, 5, 55),   # top-down beds/tables/dressers
    "blueprint":         (0, 50, 11, 52),  # floor-plan line art
    # --- font as a cream tile atlas (monospace blit; char c -> cell c%16,c//16) ---
    "font_cp437":        (48, 0, 63, 15),  # full 256-glyph CP437 atlas
}

def build_font():
    """CP437 block (cols 48-63) -> a MoteFont via the glyphs2font pipeline.
    Glyph i is codepoint (first+i) at cell (i%16, i/16); the source block starts
    at codepoint 0, so first=0 aligns ASCII directly. White-on-transparent =
    crisp 1-bit coverage; text_font() applies the draw colour."""
    fd = os.path.join(ASSETS, "font"); os.makedirs(fd, exist_ok=True)
    COUNT = 128                       # codepoints 0..127 (control pics + ASCII)
    rows = (COUNT + 15) // 16         # 8 rows
    glyphs = Image.new("RGBA", (16*TS, rows*TS))
    gp = glyphs.load()
    for i in range(COUNT):
        cc, rr = 48 + (i % 16), (i // 16)
        cx, cy = (i % 16)*TS, (i // 16)*TS
        for y in range(TS):
            for x in range(TS):
                ink = IDX[cc*TS + x, rr*TS + y] != TRANSPARENT_IDX
                gp[cx+x, cy+y] = (255, 255, 255, 255) if ink else (0, 0, 0, 0)
    glyphs.save(os.path.join(fd, "rogue8_glyphs.png"))
    with open(os.path.join(fd, "rogue8_glyphs.gsheet"), "w") as f:
        f.write("16 8 9 0 %d\n" % COUNT)   # cols cell line_h first count
    print(f"[font] rogue8_glyphs.png {16}x{rows} cells + .gsheet (first=0 count={COUNT})")

def build_sprites():
    for name, (c0, r0, c1, r1) in SPRITE_SHEETS.items():
        save_sheet(name, region(c0, r0, c1, r1))

# ---------------------------------------------------------------------------
# Terrain autotile rulesets. `.tileset` is the editable source; `mote bake`
# turns it (+ its sheet) into src/<name>.tiles.h. The engine reads lut[mask]
# as a linear atlas cell index; variants stack vertically (nvar blocks).
# ---------------------------------------------------------------------------
NB_N, NB_E, NB_S, NB_W = 1 << 0, 1 << 2, 1 << 4, 1 << 6

def nineslice_lut():
    """Replicates mote_autotile_template(MOTE_AT_NINESLICE): mask -> cell 0..8."""
    lut = [0]*256
    for m in range(256):
        W, E = bool(m & NB_W), bool(m & NB_E)
        N, S = bool(m & NB_N), bool(m & NB_S)
        col = 0 if (not W and E) else (2 if (W and not E) else 1)
        row = 0 if (not N and S) else (2 if (N and not S) else 1)
        lut[m] = row*3 + col
    return lut

def floor_lut():
    return [0]*256   # a fill: every config uses cell 0; variety comes from nvar rows

def write_tileset(name, lut, edge, nvar, vweight=None):
    d = os.path.join(GAME, "tilesets"); os.makedirs(d, exist_ok=True)
    vweight = vweight or [1]*8
    with open(os.path.join(d, name + ".tileset"), "w") as f:
        f.write(f"sheet tilesets/{name}.png\n")
        f.write("tile 8\n")
        f.write(f"edge {edge}\n")
        f.write(f"nvar {nvar}\n")
        f.write("lut " + " ".join(str(v) for v in lut) + "\n")
        f.write("xform " + " ".join("0" for _ in range(256)) + "\n")
        f.write("vweight " + " ".join(str(v) for v in vweight[:8]) + "\n")
    print(f"[tileset] {name}.tileset (edge={edge} nvar={nvar})")

def write_tileset_sheet(name, img):
    d = os.path.join(GAME, "tilesets"); os.makedirs(d, exist_ok=True)
    p = os.path.join(d, name + ".png"); img.save(p)
    print(f"[tileset] {name:14s} sheet {img.width//TS}x{img.height//TS} -> {p}")

def nineslice_sheet(cells, keep_black=True):
    """cells: 9 (col,row) tuples in reading order TL,T,TR,L,C,R,BL,B,BR."""
    out = Image.new("RGBA", (3*TS, 3*TS))
    f = tile_opaque if keep_black else tile_img
    for i, (c, r) in enumerate(cells):
        out.paste(f(c, r), ((i % 3)*TS, (i // 3)*TS))
    return out

def stacked_variants(cells_per_var, cols, keep_black=True):
    """cells_per_var: list of variant blocks, each a flat list of (col,row).
       Produces a sheet `cols` wide and len*rows_per_block tall."""
    rows_per = len(cells_per_var[0]) // cols
    out = Image.new("RGBA", (cols*TS, len(cells_per_var)*rows_per*TS))
    f = tile_opaque if keep_black else tile_img
    for v, block in enumerate(cells_per_var):
        for i, (c, r) in enumerate(block):
            x, y = (i % cols)*TS, (v*rows_per + i // cols)*TS
            out.paste(f(c, r), (x, y))
    return out

# nine-slice piece maps (TL,T,TR, L,C,R, BL,B,BR)
WALL_BRICK = [(3,35),(4,35),(5,35), (3,36),(4,36),(5,36), (3,37),(4,37),(5,37)]
HEDGE      = [(0,29),(1,29),(2,29), (0,30),(1,30),(2,30), (0,31),(1,31),(2,31)]

# floor fills: each entry is one variant tile (col,row); stacked as 1-wide sheet
FLOOR_COBBLE = [[(1,26)],[(1,27)]]          # grey cobble: patterned + plain
FLOOR_GRASS  = [[(4,42)],[(5,42)]]          # clean dark jungle-grass centres
WATER        = [[(51,35)],[(52,35)]]        # blue ripple bands (no flat water in source)

def build_terrain():
    # nine-slice walls / hedges
    write_tileset_sheet("wall_brick", nineslice_sheet(WALL_BRICK))
    write_tileset("wall_brick", nineslice_lut(), edge=1, nvar=1)
    write_tileset_sheet("hedge", nineslice_sheet(HEDGE, keep_black=False))
    write_tileset("hedge", nineslice_lut(), edge=0, nvar=1)
    # floor fills (nvar variants, single logical cell)
    for name, blocks, edge in [("floor_cobble", FLOOR_COBBLE, 1),
                               ("floor_grass",  FLOOR_GRASS, 1),
                               ("water",        WATER, 1)]:
        write_tileset_sheet(name, stacked_variants(blocks, cols=1))
        write_tileset(name, floor_lut(), edge=edge, nvar=len(blocks))

# ---------------------------------------------------------------------------
# Terrain raw subsheets — decorative sets not forced into a ruleset (author a
# rule in Studio's Tiles tab if a game needs one).
# ---------------------------------------------------------------------------
TERRAIN_RAW = {
    "panels_colour":   (0, 20, 15, 23),   # green/pink/blue/yellow rounded panels
    "wall_purple":     (0, 16, 7, 18),    # purple brick
    "wall_stonebrick": (0, 35, 15, 39),   # full stone-brick set (feature tiles incl.)
    "wall_temple":     (0, 44, 15, 46),   # aztec/temple wall + tiki idol
    "grass_garden":    (0, 40, 15, 47),   # jungle grass borders + gold steps + flower
    "cobble_floors":   (0, 25, 4, 28),    # 5-shade cobblestone floors
    "terrain_edges":   (48, 33, 63, 39),  # water/cloud/rock edge tiles
}

def build_terrain_raw():
    for name, (c0, r0, c1, r1) in TERRAIN_RAW.items():
        save_sheet(name, region(c0, r0, c1, r1, keep_black=True))

if __name__ == "__main__":
    build_sprites()
    build_terrain_raw()
    build_terrain()
    build_font()
    print("done")
