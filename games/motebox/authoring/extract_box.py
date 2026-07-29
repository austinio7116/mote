#!/usr/bin/env python3
"""
Motebox asset extraction — CC0 source tileset -> editable assets/ + tilesets/.

    python3 authoring/extract_box.py            # write everything
    python3 authoring/extract_box.py --preview  # + /tmp/motebox_assets.html review page

Source: Simple Roguelike Tileset v0.16 by Ink_Slime (DC Slime), CC0 / public
domain. 512x512, 8x8 tiles, 64x64 grid, palette index 0 (black) = transparent.
The same master roguemote uses; see games/roguemote/authoring/CATALOGUE.md for
the zone map.

Everything this writes is an EDITABLE SOURCE FILE the developer can open in Mote
Studio (a PNG, a .tileset). `mote bake .` turns them into src/*.h. Nothing here
writes a baked header, and the game generates no art at runtime.

Three kinds of output:

1. SPRITE SUBSHEETS (assets/sheets/*.png) — thematic rectangles cut from the
   master, layout preserved so colour-variant families stay column/row aligned.
   Same rectangles as roguemote where the contents are the same, but named for
   what they actually draw (see BUILDINGS below).

2. BIOME FILL TILESETS (tilesets/*.png + .tileset) — the master has exactly
   THREE pure single-colour cells in 4096, so there is no ready-made flat
   terrain art. What it does have, in rows 35 and 47-49, is ~40 hand-drawn
   MONOCHROME TEXTURES that tile seamlessly: chevron wave bands, brick courses,
   pebble grids, hatching, dashes, furrows. Each biome fill is one of those
   textures composited over a flat of a source PALETTE colour, with the texture
   ink recoloured to another palette colour. Base, ink and shape are all from
   the CC0 master; only the combination is ours, and it is declared in one table
   (BIOMES) rather than painted by hand.

3. FX RECOLOURS (assets/sheets/fx_*.png) — `blit` is colour-keyed with no tint,
   so a white FX frame can only ever draw white. The 73-frame monochrome FX band
   is therefore palette-swapped into six elemental sheets (fire/frost/acid/ash/
   holy/void), which is what gives 22 disasters their visual range: 73 x 6 = 438
   frames, every one of them hand-drawn.

Terrain that roguemote already generates as a full 47-cell blob autotile is
SYNCED, not re-derived: gen_terrain.py over there is the single source of truth
for those six bands, and forking its 300-line derivation would let the two
copies drift. See SYNC_TERRAIN.
"""
import os, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from palette import mix          # interpolated tones inside the artist's hue families
from PIL import Image

HERE   = os.path.dirname(os.path.abspath(__file__))
GAME   = os.path.dirname(HERE)
ROGUE  = os.path.join(os.path.dirname(GAME), "roguemote")
ASSETS = os.path.join(GAME, "assets")
SHEETS = os.path.join(ASSETS, "sheets")
TSETS  = os.path.join(GAME, "tilesets")
TS     = 8                      # source tile size

SRC = Image.open(os.path.join(HERE, "source_tileset.png"))
assert SRC.mode == "P", "expected the palettised CC0 master"
IDX = SRC.load()
PAL = SRC.getpalette()
TRANSPARENT_IDX = 0             # palette index 0 = pure black = the sheet's transparency

# ---------------------------------------------------------------- palette ----
# The master is PICO-8's 16 colours. Every base and ink below is one of these,
# so nothing in the biome art is a colour the artist did not use.
NAVY   = (29, 43, 83);    MAROON = (126, 37, 83);  DKGREEN = (0, 135, 81)
BROWN  = (171, 82, 54);   DKGREY = (95, 87, 79);   LTGREY  = (194, 195, 199)
WHITE  = (255, 241, 232); RED    = (255, 0, 77);   ORANGE  = (255, 163, 0)
YELLOW = (255, 236, 39);  GREEN  = (0, 228, 54);   BLUE    = (41, 173, 255)
SLATE  = (131, 118, 156); PINK   = (255, 119, 168); PEACH  = (255, 204, 170)


def _rgb(ix):
    return (PAL[ix * 3], PAL[ix * 3 + 1], PAL[ix * 3 + 2])


def cell(c, r):
    """One 8x8 source cell as RGBA, index 0 -> transparent."""
    im = Image.new("RGBA", (TS, TS)); px = im.load()
    for y in range(TS):
        for x in range(TS):
            ix = IDX[c * TS + x, r * TS + y]
            px[x, y] = (0, 0, 0, 0) if ix == TRANSPARENT_IDX else _rgb(ix) + (255,)
    return im


def region(c0, r0, c1, r1):
    """An inclusive cell rectangle as one RGBA subsheet."""
    im = Image.new("RGBA", ((c1 - c0 + 1) * TS, (r1 - r0 + 1) * TS))
    for r in range(r0, r1 + 1):
        for c in range(c0, c1 + 1):
            im.paste(cell(c, r), ((c - c0) * TS, (r - r0) * TS))
    return im


def save_sheet(name, img):
    os.makedirs(SHEETS, exist_ok=True)
    p = os.path.join(SHEETS, name + ".png")
    img.save(p)
    print(f"[sheet]   {name:20s} {img.width // TS}x{img.height // TS} cells -> {os.path.relpath(p, GAME)}")


# ------------------------------------------------------------ subsheets ------
# (c0, r0, c1, r1) inclusive, in source cell coordinates.
SPRITE_SHEETS = {
    # --- actors ---
    "characters":  (32,  0, 47,  7),   # @, portraits, adventurers, villagers, kings
    "animals":     (32,  8, 47, 15),   # the ecology: prey, predators, birds, vermin
    "monsters":    (32, 16, 47, 23),   # goblins, demons, ghosts, slimes
    "bosses":      (32, 32, 47, 39),   # 17 kaiju, every one 2x2
    "crowns_fx":   (32, 24, 40, 31),   # crowns (5 colours), explosion/fire/splash/smoke
    # --- buildings & world props ---
    # Named for what it DRAWS. roguemote calls this rectangle
    # "doors_gems_banners" and its ASSETS.md already records the correction: cols
    # 11-14 are door-closed / door-open / house / house-extend, five colour rows
    # deep. That is the whole per-kingdom building set, so it gets the real name
    # here and the row index IS the kingdom colour.
    "buildings":   (11,  1, 14,  5),   # door / open door / house / house-extend x 5 colours
    "props":       ( 0,  6,  9,  9),   # campfire, torches, fences, bridges, house, igloo, logs
    "nature":      ( 0,  8, 15, 13),   # trees, bushes, rocks, mushrooms, clouds, splats
    "terrain_edges": (48, 33, 63, 39), # coast foam, cloud, drift, debris
    "furniture":   ( 0, 53,  5, 55),   # top-down beds/tables/dressers for interiors
    "blueprint":   ( 0, 50, 11, 52),   # floor-plan line art: the lords' build ghosts
    "chests":      ( 0,  1,  1,  5),   # stockpiles / tribute, 5 colours
    # --- economy & tech iconography ---
    "treasure_ore": (16, 16, 27, 19),  # ore/gold/gem deposits + 9 heraldic shields
    "food":        (16,  8, 27, 13),   # crops, market goods
    "weapons":     (16,  0, 23,  6),   # tech-tier weapon icons
    "tools":       (16, 28, 31, 31),   # pickaxes, axes, hammers, staffs
    "devices":     (24,  5, 31,  6),   # lamps, crystal ball, HOURGLASS, instruments
    # --- UI ---
    "fx_mono":     (48, 36, 63, 43),   # bolts, rings, sparkles, wind — the FX master
    "ui_status":   (48, 24, 63, 31),   # mood faces, element icons, hearts, meter segments
    "ui_icons":    ( 0, 14, 15, 15),   # tiny HUD icons
    "ui_gauges":   (48, 16, 59, 19),   # 8-dir arrows, bars, signal gauges
    "ui_buttons":  (48, 20, 63, 23),   # A/B/X/Y prompts
    "panels":      ( 0, 20, 15, 23),   # rounded panels in 4 colours
    "ui_symbols":  (48, 32, 51, 32),   # lineage marks
    "font_cp437":  (48,  0, 63, 15),   # monospace atlas (char c -> cell c%16, c/16)
}

# ------------------------------------------------------------ biome fills ----
# The master's chevron wave band IS good water: it is drawn as flowing liquid and it
# tiles, so the four flowing biomes use the artist's art and generate nothing.
#
# EVERYTHING ELSE IS GENERATED, from the vocabulary in authoring/biomes.py. The first
# version of this file picked "texture cells" out of the master by ink coverage — any
# hand-drawn cell covering 18-55% of a tile was a candidate — and stamped the winner
# over a flat colour. Selection without looking, and it showed: six biomes wore the
# same diagonal-chunk motif in six colours, four more wore the same arrow-blob, snow
# was scattered with the master's HEARTS and tundra with its PLUS SIGNS, and mountains
# were covered in neat masonry brick. Rows 47-49 are decorative line art for edging a
# dungeon room, and one motif tiled across a continent is wallpaper however good the
# motif is. See biomes.py for what replaced it.
COV_MIN, COV_MAX = 18, 55         # still guards the two cells we DO take from the master

WAVE = (48, 35)                   # the one 37%-coverage chevron edge in the wave band
WAVE_FILLS = [
    ("bio_ocean",   NAVY,   SLATE),    # deep, cold, barely moving
    ("bio_sea",     BLUE,   SLATE),    # lighter water, the same swell
    ("bio_lava",    RED,    ORANGE),   # the same flow, read as heat
    ("bio_acid",    GREEN,  YELLOW),
]
FOAM = (56, 34)                   # the master's sandbar blob, for the shallows


# Terrain roguemote already derives as a hand-drawn 47-cell blob set (or a
# checked fill). Synced verbatim: gen_terrain.py in games/roguemote/authoring is
# the single source of truth, and these are the biomes that want real borders.
SYNC_TERRAIN = [
    ("hedge",        "forest — the garden-maze band reads as a canopy with edges"),
    ("floor_jungle", "meadow — grass with dirt sides and gold steps"),
    ("floor_cobble", "road / paved plaza"),
    ("floor_grass",  "the master's one pure-green fill, kept as a flat option"),
    ("wall_brick",   "human city wall"),
    ("wall_marble",  "elf city wall"),
    ("wall_bone",    "undead / ruin wall"),
    ("wall_aztec",   "orc city wall"),
]

# FX elemental recolours: every non-transparent pixel of the mono FX band is
# mapped by luminance onto a two-stop ramp of source palette colours, so a bolt
# keeps its hand-drawn highlight and core.
FX_RAMPS = {
    "fx_fire":  (YELLOW, RED),
    "fx_frost": (WHITE,  BLUE),
    "fx_acid":  (YELLOW, DKGREEN),
    "fx_ash":   (LTGREY, DKGREY),
    "fx_holy":  (WHITE,  YELLOW),
    "fx_void":  (SLATE,  MAROON),
}


def coverage(tex_cell):
    """Percent of the cell the ink would paint."""
    c, r = tex_cell
    n = sum(1 for y in range(TS) for x in range(TS)
            if IDX[c * TS + x, r * TS + y] != TRANSPARENT_IDX)
    return n * 100 // (TS * TS)


def flat(colour, variant=None, ink=None):
    """One opaque 8x8 biome tile: a source texture over a flat source colour.

    `variant` is None (plain) or ((c,r), roll_x, roll_y) — the roll wraps the
    cell, giving a second phase of a seamless pattern out of the same art.
    """
    im = Image.new("RGBA", (TS, TS), colour + (255,))
    if variant is None:
        return im
    tex_cell, rx, ry = variant
    covr = coverage(tex_cell)
    if not (COV_MIN <= covr <= COV_MAX):
        raise SystemExit(f"refusing texture cell {tex_cell}: {covr}% ink coverage, "
                         f"outside {COV_MIN}-{COV_MAX}% — it would paint over the base "
                         f"(the master's wave band continues into fully opaque body cells)")
    t = cell(*tex_cell); tp = t.load(); ip = im.load()
    for y in range(TS):
        for x in range(TS):
            if tp[(x - rx) % TS, (y - ry) % TS][3]:
                ip[x, y] = (ink or WHITE) + (255,)
    return im


def write_tileset(name, tiles, weights, edge=1):
    """A fill ruleset: every neighbour mask uses cell 0; variety is nvar rows.

    Sheet layout must be ONE tile wide and nvar tall — the engine reads
    tpr = sheet_w/tile_w and steps a whole base block per variant, so a 1xN
    column makes cell 0 + variant v land on row v (see mote_2d.c draw_autotile).
    """
    os.makedirs(TSETS, exist_ok=True)
    sheet = Image.new("RGBA", (TS, TS * len(tiles)))
    for i, t in enumerate(tiles):
        sheet.paste(t, (0, i * TS))
    sheet.save(os.path.join(TSETS, name + ".png"))
    vw = (list(weights) + [1] * 8)[:8]
    with open(os.path.join(TSETS, name + ".tileset"), "w") as f:
        f.write(f"sheet tilesets/{name}.png\n")
        f.write("tile 8\n")
        f.write(f"edge {edge}\n")
        f.write(f"nvar {len(tiles)}\n")
        f.write("lut " + " ".join(["0"] * 256) + "\n")
        f.write("xform " + " ".join(["0"] * 256) + "\n")
        f.write("vweight " + " ".join(str(v) for v in vw) + "\n")
    print(f"[biome]   {name:20s} nvar={len(tiles)} weights={vw[:len(tiles)]}")


def artist_tile(cx, cy, under=DKGREY):
    """One 8x8 cell straight from the master, opaque, for a terrain whose art the artist
    already drew better than any generator will."""
    im = region(cx, cy, cx, cy).convert("RGBA")
    # ONTO ROCK, NOT BLACK. The master's palette index 0 is its transparency, so filling
    # behind it with black puts real black pixels in a terrain tile — which is the exact
    # bug that made forest look outlined, walked straight back in through a new door.
    flat = Image.new("RGBA", im.size, under + (255,))
    flat.alpha_composite(im)
    return flat.convert("RGBA")


def build_biomes():
    """Real blob47 sets, so every biome boundary is DRAWN.

    Two earlier versions of this were flat fills. Both looked like squares butted
    together, because a fill has no idea what is next to it and terrain is mostly
    edges. roguemote's hand-drawn bands look right precisely because they are 47-cell
    blobs, and that is the standard this has to meet — so the interiors come from the
    texture vocabulary and terrain.py wraps each one in a real blob47 set with rims
    and inner corners. See terrain.py and biomes.py for the reasoning.
    """
    import biomes, terrain

    # 1. WATER, LAVA, ACID: a flat base with a regular ripple course, and a BRIGHT
    #    2 px rim on the sides facing anything else. These four are the only biomes
    #    that keep an interior pattern beyond a variant or two, because a liquid
    #    genuinely is a surface in motion — everything else is flat, like hedge.
    # WATER IS TWO BLUES. The master's chevron band was used for this once and it came
    # out as SLATE (a mauve) zigzagging on cyan — pink triangles on blue, which is what
    # a sea must never look like. A dash course in a second tone of the SAME hue is
    # what reads as water at eight pixels.
    # THREE TONES EACH, not two: a body, a ripple and a crest. Two colours make a flat
    # field with stripes on it; the third makes a surface with troughs.
    for name, base, ink, ink2, step in (
            ("bio_ocean", NAVY,   BLUE,   SLATE,  3),   # deep water, paler ripples
            ("bio_sea",   BLUE,   LTGREY, WHITE,  4),   # bright water, white crests
            ("bio_lava",  MAROON, RED,    ORANGE, 3),   # the same course, read as heat
            ("bio_acid",  DKGREEN, GREEN, YELLOW, 3)):
        sheet = terrain.flat(
            lambda v, _b=base, _i=ink, _i2=ink2, _s=step:
                biomes.dashes(_b, _i, 7 + v, step=_s, run=3, ink2=_i2),
            nvar=2)
        terrain.write(TSETS, name, sheet, 2, vweight=[1, 1], lut=[0] * 256)
        print(f"[flat]    {name:20s} nvar=2")

    # 2. the shallows: brighter water, denser foam. It keeps a white rim now that the
    #    generator thins the band to one pixel where opposite edges are both open —
    #    which is exactly the one-tile river case. With a fixed 2 px rim a river was
    #    entirely rim, and an earlier sand-coloured version rendered every river in the
    #    world as a peach footpath.
    sheet = terrain.flat(lambda v: biomes.dashes(BLUE, LTGREY, 17 + v, step=3, run=4,
                                                 ink2=WHITE), nvar=2)
    terrain.write(TSETS, "bio_shallow", sheet, 2, vweight=[1, 1], lut=[0] * 256)
    print(f"[flat]    {'bio_shallow':20s} nvar=2")

    # 3. everything else, from the vocabulary — flat bases, bright rims
    for rec in biomes.recipes():
        name, base, builders, weights, rim = rec[:5]
        # What to fill the master's transparency with, per artist-sourced biome: its own
        # body colour, so a hole in his art reads as more of the same rock.
        ARTIST_UNDER = {"bio_mountain": NAVY, "bio_peak": LTGREY}
        if base is biomes.ARTIST:
            # tiles straight from the master, one per variant
            sheet = Image.new("RGBA", (TS, TS * len(builders)))
            for v, (cx, cy) in enumerate(builders):
                sheet.paste(artist_tile(cx, cy, ARTIST_UNDER.get(name, DKGREY)),
                            (0, v * TS))
            terrain.write(TSETS, name, sheet, len(builders), vweight=weights,
                          lut=[0] * 256)
            print(f"[artist]  {name:20s} nvar={len(builders)} cells={builders}")
            continue
        sides = rec[5] if len(rec) > 5 else "NSEW"
        nvar = len(builders)
        sheet = terrain.flat(lambda v: builders[v](v), nvar=nvar)
        terrain.write(TSETS, name, sheet, nvar, vweight=weights, lut=[0] * 256)
        print(f"[flat]    {name:20s} nvar={nvar} weights={weights}")


# What each synced set stands ON. roguemote's sets are OVERLAYS — hedge is 15% transparent
# because it is designed to sit on a dungeon floor, and it insets itself by a pixel on
# every open side. Motebox draws one terrain per cell with nothing beneath, so those
# transparent pixels showed the dark clear colour and every patch of forest in the world
# came out with a BLACK OUTLINE around it. Compositing them onto the ground they would
# naturally stand on makes them opaque and the outline disappears.
SYNC_GROUND = {
    "hedge":        (0, 135, 81),      # forest, standing on grass
    "floor_jungle": (0, 135, 81),      # meadow
    "wall_brick":   (95, 87, 79),      # a city wall, standing on trodden ground
    "wall_marble":  (95, 87, 79),
    "wall_bone":    (95, 87, 79),
    "wall_aztec":   (95, 87, 79),
}


def flatten_synced(name):
    """Composite a synced overlay set onto its ground so it has no transparency left."""
    ground = SYNC_GROUND.get(name)
    if not ground:
        return
    p = os.path.join(TSETS, name + ".png")
    if not os.path.isfile(p):
        return
    im = Image.open(p).convert("RGBA")
    if not any(q[3] == 0 for q in im.getdata()):
        return
    flat = Image.new("RGBA", im.size, ground + (255,))
    flat.alpha_composite(im)
    flat.save(p)


def sync_terrain():
    """Copy roguemote's derived blob47 sets in as editable source."""
    missing = []
    for name, why in SYNC_TERRAIN:
        for ext in (".png", ".tileset"):
            s = os.path.join(ROGUE, "tilesets", name + ext)
            if not os.path.isfile(s):
                missing.append(s); continue
            shutil.copy2(s, os.path.join(TSETS, name + ext))
        flatten_synced(name)
        print(f"[sync]    {name:20s} {why}")
    if missing:
        print("  !! missing in roguemote (run its extract.py first):")
        for m in missing: print("     ", m)


def build_sprites():
    for name, (c0, r0, c1, r1) in SPRITE_SHEETS.items():
        save_sheet(name, region(c0, r0, c1, r1))


def build_fx_recolours():
    c0, r0, c1, r1 = SPRITE_SHEETS["fx_mono"]
    base = region(c0, r0, c1, r1)
    for name, (hi, lo) in FX_RAMPS.items():
        im = base.copy(); px = im.load()
        for y in range(im.height):
            for x in range(im.width):
                r, g, b, a = px[x, y]
                if not a: continue
                lum = (r * 77 + g * 151 + b * 28) >> 8
                px[x, y] = (hi if lum >= 176 else lo) + (255,)
        save_sheet(name, im)


def build_ore():
    """Ore deposits, generated — because the master has none.

    The four deposits used to point at `treasure_ore` cells (1,0) (1,1) (1,2) (7,0),
    and the label set is unambiguous about what those are: "gold small pile (3)",
    "silver small pile (3)", "copper small pile (3)", "silver ingot/bar". So the
    wilderness was strewn with LOOSE COINS AND BARS lying in the grass — which is
    what "why are there coins in some places on the map?" was looking at. That whole
    rectangle is treasure: a reward you pick up, not a seam you mine. Searching every
    label in the master for ore, vein, crystal or outcrop turns up nothing that reads
    as a deposit in the ground.

    So one is built, from the artist's own "brown rock/boulder" with the metal set
    INTO it — the same palette-swap trick the FX sheets use, and it keeps his
    silhouette and shading. A rock with a glint is legible at eight pixels and, more
    to the point, it is a thing you would send a miner to rather than pocket.
    """
    rock = region(*SPRITE_SHEETS["nature"])
    cell = rock.crop((5 * TS, 4 * TS, 6 * TS, 5 * TS))       # "brown rock/boulder"

    # The rock is a fourteen-pixel blob: DKGREY body, ONE light highlight, a navy
    # underside. So the glint is two pixels, placed on body pixels chosen by hand —
    # a first attempt scattered four on a lattice and two landed in the shadow, which
    # read as a rock with confetti on it rather than a seam of metal.
    BODY = (95, 87, 79)
    SPOTS = ((4, 3), (2, 5))
    ORES = (("iron",   BROWN,  ORANGE),      # rust through brown stone
            ("silver", BODY,   WHITE),       # plain stone, a bright glint
            ("gold",   BROWN,  YELLOW),
            ("gem",    BODY,   BLUE))

    out = Image.new("RGBA", (TS * len(ORES), TS))
    for i, (_name, body, glint) in enumerate(ORES):
        im = cell.copy(); px = im.load()
        for y in range(TS):
            for x in range(TS):
                if px[x, y][3] and px[x, y][:3] == BODY:
                    px[x, y] = body + (255,)
        for (sx, sy) in SPOTS:
            px[sx, sy] = glint + (255,)
        out.paste(im, (i * TS, 0))
    save_sheet("ore", out)


# WHICH TERRAIN BLEEDS OVER WHICH. A height in a stack: only the higher one reaches
# into the lower one's cell, so a boundary is drawn exactly once and two neighbours can
# never both claim the same pixels. Water is the bottom of the world and rock the top,
# which is also the order they lie in reality.
#
# The (body, shore) pair per biome is the colour the bleed is painted in: the body is
# the terrain's own flat colour and the shore is a lighter tone for the innermost pixel,
# so the thing that creeps over its neighbour has a lit lip and looks like it has a
# thickness rather than like a stain.
# Every biome's RAMP, deep to shallow: the body it is made of, then the tones its shore
# fades through as it creeps over its neighbour. PICO-8's sixteen are not a smooth set,
# but they do contain real families — navy/blue/white for water, brown/orange/peach for
# earth, dkgrey/ltgrey/white for rock, plum/red/orange for heat — and spending them is
# the difference between a boundary that is a flat band with a highlight and one that
# reads as a shore.
TRANSITIONS = [
    #  name             prec  ramp: THE BODY IT IS MADE OF, then its shore tones
    #
    # ramp[0] MUST BE THE BIOME'S OWN BODY COLOUR — the same colour its flat base tile is
    # filled with. This got written as a SHADING triple instead, (shadow, body, highlight),
    # and since the overlay paints ramp[0] where it joins its own field, fourteen of the
    # twenty terrains bled a colour they are not made of: hill crept over grass in DKGREY,
    # grass crept over sand in NAVY. Every boundary in the world therefore had a hard seam
    # a few pixels in, where the bled colour met the real interior — and an isolated grass
    # cell came out as a dark-bordered green box. check_transition_bodies() below now
    # asserts this against the generated art, so it cannot come back.
    ("bio_ocean",         0,  [NAVY,    BLUE,    LTGREY]),
    ("bio_sea",           1,  [BLUE,    LTGREY,  WHITE]),
    ("bio_shallow",       2,  [BLUE,    LTGREY,  WHITE]),
    ("bio_ice",           3,  [WHITE,   LTGREY,  BLUE]),
    ("bio_lava",          4,  [MAROON,  RED,     YELLOW]),
    ("bio_acid",          4,  [DKGREEN, GREEN,   YELLOW]),
    ("bio_swamp",         5,  [DKGREEN, DKGREY,  DKGREY]),
    ("bio_beach",         6,  [PEACH,   YELLOW,  ORANGE]),
    ("bio_desert",        7,  [YELLOW,  PEACH,   ORANGE]),
    ("bio_savanna",       8,  [ORANGE,  YELLOW,  YELLOW]),
    ("bio_farm",          9,  [BROWN,   ORANGE,  ORANGE]),
    ("bio_grass",        10,  [DKGREEN, GREEN,   GREEN]),
    ("bio_snow",         11,  [WHITE,   LTGREY,  LTGREY]),
    ("bio_tundra",       12,  [BROWN,   PEACH,   PEACH]),
    ("bio_ash",          13,  [DKGREY,  LTGREY,  WHITE]),
    ("bio_scorched",     14,  [MAROON,  RED,     ORANGE]),
    ("bio_rubble",       15,  [LTGREY,  WHITE,   WHITE]),
    ("bio_hill",         16,  [BROWN,   ORANGE,  YELLOW]),
    ("bio_mountain",     17,  [NAVY,    LTGREY,  WHITE]),
    ("bio_peak",         18,  [LTGREY,  WHITE,   WHITE]),
]


def check_transition_bodies():
    """Assert every overlay bleeds the colour its own terrain is actually made of.

    A transition paints ramp[0] where it meets its own field, so if that is not the base
    tile's fill colour there is a hard seam a few pixels inside every boundary in the
    world. Fourteen of twenty were wrong at once when the ramps were written by hand as
    shading triples, and the symptom is easy to misread as a tileset problem rather than
    a table problem — so it is checked against the generated art instead of by eye.
    """
    from collections import Counter
    bad = []
    for name, _prec, ramp in TRANSITIONS:
        p = os.path.join(TSETS, name + ".png")
        if not os.path.isfile(p):
            continue
        im = Image.open(p).convert("RGBA")
        dom = Counter([q[:3] for q in im.getdata() if q[3]]).most_common(1)[0][0]
        if tuple(ramp[0]) != dom:
            bad.append("%s: base is %s but the overlay bleeds %s"
                       % (name, dom, tuple(ramp[0])))
    if bad:
        raise SystemExit("TRANSITION BODY MISMATCH\n  " + "\n  ".join(bad))
    print("[check]   all %d overlays bleed their own body colour" % len(TRANSITIONS))


# THE BAND STACK. Deep ocean is the BACKGROUND and costs no layer; mountain, lava and
# scorched are sprites. That leaves eight layers for eight bands, which is one more than
# the eight colour families need — so nothing has to merge lossily at all, and desert and
# ash keep tones of their own.
#
# Order is bottom-up and the map is CUMULATIVE: a cell in band B sets bits 0..B. Layer L
# therefore covers every cell at band L or above, the engine autotiles it against its own
# bit, and its fringe reveals the band below — which turns out to be the NEIGHBOUR's own
# colour, because a neighbour in a lower band is exactly a cell that lacks this bit.
BAND_LIST = [
    ("bd_water",   BLUE,    WHITE,   ["sea", "shallow"]),
    ("bd_frost",   WHITE,   BLUE,    ["ice", "snow"]),
    ("bd_sand",    PEACH,   WHITE,   ["beach"]),
    ("bd_desert",  YELLOW,  WHITE,   ["desert"]),
    # A LIGHTER GRASS. The band body was DKGREEN and so is the artist's tree foliage, so
    # a wood was invisible against the field it stood in. Grass is a third of the way to
    # GREEN now, which leaves his trees reading dark against it.
    ("bd_green",   mix(DKGREEN, GREEN, 0.30), GREEN, ["grass", "swamp", "acid", "meadow", "forest"]),
    ("bd_savanna", ORANGE,  YELLOW,  ["savanna"]),
    # Ash and scorched sit on this band: their sprite fringes reveal BARE EARTH,
    # which is what is under a burn. On the rock band a burnt forest showed light
    # grey stone through its own edge.
    ("bd_dry",     BROWN,   ORANGE,  ["farm", "hill", "tundra", "ash", "scorched"]),
    ("bd_rock",    LTGREY,  WHITE,   ["peak", "rubble", "road", "mountain"]),
]


# Burnt ground. Not from the artist's sixteen — see the note in build_hot().
CHAR  = ( 44,  34,  36)
EMBER = (112,  46,  36)
SOOT  = ( 24,  20,  24)


def build_hot():
    """Ash, scorched earth and lava — see authoring/burnt.py.

    These three are the band that would not fit: eight layers hold eight bands and there
    is no ninth, so the materials a fire leaves behind are drawn as sprites over whatever
    band they sit on. They are the right ones to move because they are the RAREST — 0.0 to
    0.6% of a world, measured across six — and because they always carry flux and FX
    particles anyway, so a sprite among sprites is where they belong.

    They are BLOB 47 sets, not single cells. One opaque cell per world cell pinned every
    burn scar to the tile grid, and "it looks like black squares" was exactly that: a
    rectilinear blob of one flat dark tone. Now the cell comes from the eight-neighbour
    mask and every open side is fringed, so a scar has a ragged edge with the ground it
    burnt showing through — the same edge treatment as a coastline.
    """
    import burnt
    save_sheet("hot", burnt.build(nvar=3))
    burnt.write_lut(os.path.join(GAME, "src", "blob47_same.h"))


def build_bands():
    """Eight autotiled band tilesets — the whole terrain, done by the ruleset system."""
    import bands, palette
    for i, (name, body, toward, members) in enumerate(BAND_LIST):
        tones = palette.ramp(body, toward)
        # GRAIN, not flat. Two variants with different marks, so the same band is not the
        # same speckle repeated across a continent.
        # Rock gets STONE, not grain: see bands.stone(). The grass-style speckle left a
        # mountain interior as flat grey with three dots on it, which is exactly what the
        # screenshots showed for six rounds.
        interior = (bands.stone(tones, 101 + i * 7) if name == "bd_rock"
                    else bands.grain(tones, 101 + i * 7))
        # FOUR variants, not two. With two, the hash alternates one motif against one
        # other across a whole continent and the eye reads the result as a lattice —
        # a screenshot of a mountain interior came out as diagonal polka dots. Four
        # different fringe profiles and four different interiors cost 3 KB a band.
        sheet = bands.build(tones, interior_fn=interior, nvar=4)
        bands.write_tileset(TSETS, name, sheet, nvar=4, vweight=[1, 1, 1, 1])
        print("[band]    %-12s %-8s -> %s" % (name, str(body), ", ".join(members)))


def build_transitions():
    """One transparent overlay sheet per biome — see authoring/transitions.py.

    This is the answer to the thing no tileset could fix: in a single opaque layer a
    cell may only paint its own eight by eight pixels, so the colour boundary between
    two terrains lies exactly on the tile grid and every coastline is a staircase of
    straight segments meeting at right angles. The base pass now paints flat fields and
    THIS pass paints the boundary, in the higher terrain's colours, inside the LOWER
    terrain's cell — so the visible edge is a ragged rounded shape with nothing to do
    with the grid.
    """
    import transitions
    for name, _prec, ramp in TRANSITIONS:
        nm = "tr_" + name[4:]
        # A .tileset SIDECAR, not a bare sheet: these are genuine Blob 47 rule tiles, so
        # they belong in tilesets/ where Studio can open and edit them. `mote bake`
        # emits both the MoteAutotile and the MoteImage from one .tileset, and the draw
        # pass wants the image — so shipping it properly costs the game nothing.
        transitions.write_tileset(TSETS, nm, transitions.build(ramp), nvar=2,
                                  vweight=[1, 1])
        stale = os.path.join(SHEETS, nm + ".png")
        if os.path.isfile(stale):
            os.remove(stale)        # or bake emits two headers for one name
    transitions.write_lut_header(os.path.join(GAME, "src", "blob47_lut.h"))
    print("[blob47]  transition overlays: 47 cells x 2 variants x %d biomes"
          % len(TRANSITIONS))


def build_rocks():
    """Loose stone — see authoring/rocks.py. The master's boulders sheet holds fragments
    of a composed mountain picture, not objects, so scattering it drew a field of pale
    right-triangles; these are drawn to be put down anywhere."""
    import rocks
    save_sheet("rocks", rocks.build())


def build_town():
    """The settlement's own sprites — see authoring/build_sprites.py.

    Sixteen buildings across, five banner colours down, each cell 8 wide by 14 TALL:
    a building stands six pixels above its own tile so it has a roof with a pitch
    instead of being a face-on box the size of a bush. The column index is
    `obj - O_BUILD0`, so the C side needs no lookup table.
    """
    import build_sprites
    save_sheet("town", build_sprites.build())


def build_roads():
    """The road network overlay — see authoring/roads.py for the reasoning.

    Sixteen cells indexed by the four-neighbour mask, drawn as a four-pixel
    carriageway inside an eight-pixel tile with a one-pixel kerb, so the ground shows
    along both sides of every road in the world and a corner can round properly.

    PEACH surface, ORANGE kerb, LIGHT GREY stones — chosen by rendering the same
    network over grass, sand and beach and looking at all six. A grey cobble with a
    dark grey kerb reads as an outlined ribbon, and a pale road with a pale kerb
    disappears entirely the moment it crosses a beach.
    """
    import roads
    save_sheet("road", roads.build(PEACH, ORANGE, LTGREY))


def build_icon():
    """60x60 launcher icon: the world as a disc, with a meteor coming in.

    Reads at launcher size because it is one bold silhouette — a circle — rather
    than a screenshot. Every pixel comes from the game's own biome tiles and the
    master's FX cells, so the icon can never drift from what the game draws:
    the terrain textures are the same BIOMES recipes the map uses, and the
    meteor is the crowns_fx explosion cell.
    """
    W, R, CX, CY = 60, 27, 30, 31
    icon = Image.new("RGBA", (W, W), NAVY + (255,))
    tiles = {
        "ocean": flat(NAVY,    ((48, 35), 0, 0), SLATE),
        "sea":   flat(BLUE,    ((48, 35), 0, 0), SLATE),
        "grass": flat(DKGREEN, (( 0, 47), 0, 0), GREEN),
        "sand":  flat(PEACH,   (( 6, 48), 0, 0), WHITE),
        "peak":  flat(LTGREY,  (( 1, 47), 0, 0), WHITE),
        "lava":  flat(RED,     ((48, 35), 0, 0), ORANGE),
    }
    tp = {k: v.load() for k, v in tiles.items()}

    def land(x, y):
        """A blobby continent: two overlapping ellipses plus a wobble, so the
        coastline has bays instead of reading as a drawn shape."""
        a = ((x - 24) ** 2) / 190.0 + ((y - 26) ** 2) / 130.0
        b = ((x - 38) ** 2) / 90.0 + ((y - 38) ** 2) / 70.0
        w = 0.16 * (((x * 7 + y * 13) % 11) / 11.0)
        return min(a, b) + w

    px = icon.load()
    for y in range(W):
        for x in range(W):
            dx, dy = x - CX, y - CY
            d2 = dx * dx + dy * dy
            if d2 > R * R:
                continue                                  # space
            if d2 > (R - 1) * (R - 1):
                px[x, y] = SLATE + (255,)                 # rim light
                continue
            L = land(x, y)
            if   L < 0.62: t = "grass"
            elif L < 0.80: t = "sand"
            else:          t = "sea"   # all water bright, so the disc reads against NAVY space
            if L < 0.30 and abs(dx + dy // 2) < 3: t = "peak"    # a spine of mountains
            px[x, y] = tp[t][x % TS, y % TS]
    # the meteor: a fire trail from the top-right corner into the disc, then the
    # master's explosion cell at the impact point
    for i in range(13):
        mx, my = 56 - i * 2, 3 + i * 2
        px[mx, my] = (YELLOW if i % 2 else ORANGE) + (255,)
        if mx + 1 < W: px[mx + 1, my] = ORANGE + (255,)
    boom = cell(36, 31)                                   # crowns_fx explosion (37,31 is a sparkle)
    boom = boom.resize((16, 16), Image.NEAREST)   # 2x keeps the pixels crisp
    icon.paste(boom, (24, 19), boom)               # centred where the trail ends
    p = os.path.join(GAME, "icon.png")
    icon.convert("RGB").save(p)
    print(f"[icon]    icon.png 60x60 -> {os.path.relpath(p, GAME)}")


def preview():
    """A review page: every biome fill tiled 6x6, every sheet, every FX recolour."""
    out = "/tmp/motebox_assets"
    os.makedirs(out, exist_ok=True)
    rows = []
    import glob
    # Preview whatever build_biomes() actually wrote, rather than a second description
    # of it that can drift from the first — the old preview re-derived the tiles and so
    # could show something the game does not draw.
    for path in sorted(glob.glob(os.path.join(TSETS, "bio_*.png"))):
        name = os.path.basename(path)[:-4]
        sheet = Image.open(path).convert("RGBA")
        nvar = max(1, sheet.height // TS)
        big = Image.new("RGBA", (10 * TS, 10 * TS))
        for r in range(10):
            for c in range(10):
                h = ((c * 73856093) ^ (r * 19349663)) & 0xFFFFFFFF
                h ^= h >> 13; h = (h * 1274126177) & 0xFFFFFFFF
                v = h % nvar
                big.paste(sheet.crop((0, v * TS, TS, (v + 1) * TS)), (c * TS, r * TS))
        big.resize((10 * TS * 3, 10 * TS * 3), Image.NEAREST).save(os.path.join(out, name + ".png"))
        rows.append(f'<figure><img src="{name}.png"><figcaption>{name}</figcaption></figure>')
    for f in sorted(os.listdir(SHEETS)):
        if not f.endswith(".png"): continue
        im = Image.open(os.path.join(SHEETS, f))
        im.resize((im.width * 3, im.height * 3), Image.NEAREST).save(os.path.join(out, "s_" + f))
        rows.append(f'<figure class="s"><img src="s_{f}"><figcaption>{f[:-4]}</figcaption></figure>')
    html = ("<!doctype html><meta charset=utf-8><title>Motebox assets</title>"
            "<style>body{background:#181820;color:#ddd;font:13px system-ui;padding:16px}"
            "figure{display:inline-block;margin:0 12px 16px 0;vertical-align:top}"
            "img{image-rendering:pixelated;background:#2a2a34;display:block}"
            "figcaption{font-size:11px;color:#9ab;padding-top:3px}"
            "figure.s img{max-width:none}</style><h1>Motebox assets</h1>" + "".join(rows))
    with open("/tmp/motebox_assets.html", "w") as f:
        f.write(html)
    print("[preview] /tmp/motebox_assets.html")


def main():
    build_sprites()
    build_fx_recolours()
    build_ore()
    build_roads()
    build_rocks()
    build_town()
    build_bands()
    build_hot()
    build_biomes()
    check_transition_bodies()
    sync_terrain()
    build_icon()
    print("\nbiome tilesets written (the B_* enum in src/mb.h must match this order):")
    import glob
    for p in sorted(glob.glob(os.path.join(TSETS, "*.tileset"))):
        print("  ", os.path.basename(p)[:-8])
    if "--preview" in sys.argv:
        preview()


if __name__ == "__main__":
    main()
