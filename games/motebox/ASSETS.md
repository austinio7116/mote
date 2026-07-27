# Motebox asset library

Carved from the **Simple Roguelike Tileset** v0.16 by Ink_Slime (DC Slime), **CC0 /
public domain** — the same 512×512 sheet of **8×8** tiles (64×64 grid) roguemote uses.
`authoring/source_tileset.png` is the master; palette index 0 (black) is its transparency,
and its 16 colours are PICO-8's.

## Pipeline (editable source → baked header)

```bash
python3 authoring/extract_box.py            # master -> assets/sheets, tilesets/, icon.png
python3 authoring/extract_box.py --preview   # + /tmp/motebox_assets.html review page
../../tools/mote bake .                      # -> src/*.h   (or Studio Save)
```

Everything the script writes is an **editable source file** — a PNG, a `.tileset` — that
opens in Mote Studio. Nothing hand-writes a baked `src/*.h`, and the game generates no art
at runtime. For the zone map of the master, see
`../roguemote/authoring/CATALOGUE.md`.

## 1. Sprite subsheets — `assets/sheets/*.png` → `src/<name>.h` (`<name>_img`)

28 thematic rectangles, layout preserved so colour-variant families stay column/row
aligned. Same rectangles as roguemote where the contents are the same — but **named for
what they draw**, which matters in one case:

| Motebox | roguemote | Why the rename |
|---|---|---|
| `buildings` | `doors_gems_banners` | The rectangle is **door-closed / door-open / house / house-extend × 5 colour rows**, which roguemote's own ASSETS.md records as a correction to the agent labels. It is the entire per-kingdom building set, and **the row index is the kingdom colour**. |
| `nature` | `trinkets` | Trees, bushes, cactus, rocks, mushrooms, flowers, clouds, ooze splats — the vegetation layer, not jewellery. |
| `props` | `props_light` | Campfire, torches, fences, bridges, signposts, mushroom house, statue, throne. |

The rest keep their roguemote names: `characters`, `animals`, `monsters`, `bosses`,
`crowns_fx`, `boulders`, `terrain_edges`, `furniture`, `blueprint`, `chests`,
`treasure_ore`, `food`, `weapons`, `tools`, `devices`, `fx_mono`, `ui_status`, `ui_icons`,
`ui_gauges`, `ui_buttons`, `panels`, `ui_symbols`, `font_cp437`.

### Cells worth knowing (verified by eye, at 13× zoom)

| Sprite | Cell |
|---|---|
| explosion / fireball / splash / smoke swirl | `crowns_fx` (4,7) / (6,7) / (7,7) / (3,7) |
| the five dynasty crowns | `crowns_fx` (0–4, 5) |
| tree / dead tree / bush / rock / cactus | `nature` (6,4) (7,4) · (8,4) · (4,4) · (5,4) · (9,3) |
| bonfire · torch · bridge · fence · statue | `props` (3,3) · (7,2) · (2,2) · (0,2) · (2,3) |
| house / house-extend per kingdom colour | `buildings` (2,row) / (3,row) |
| iron / silver / gold / gem deposit | `treasure_ore` (1,2) / (1,1) / (1,0) / (7,0) |

`crowns_fx` (5,7) is a **sparkle**, not an explosion — an easy and costly mix-up, since it
reads as a plus sign at icon size.

## 2. Biome fill tilesets — `tilesets/bio_*.{png,tileset}` → `src/bio_*.tiles.h`

The master contains exactly **three pure single-colour cells in 4096**, so there is no
ready-made flat terrain art, and it has **no water at all** (roguemote's `extract.py` says
so and ships nothing for it).

What it does have, in rows **35** and **47–49**, is ~40 hand-drawn **monochrome textures
that tile seamlessly**: chevron wave bands, brick courses, pebble grids, hatching, dashes,
plough furrows, scales. Each of the 20 biome fills is one of those textures **composited
over a flat of a source palette colour**, with the ink recoloured to a second palette
colour. Declared in one table (`BIOMES`), not painted:

```python
("bio_ocean", NAVY, SLATE, [((48, 35), 0, 0)],                        [1]),
("bio_grass", DKGREEN, GREEN, [None, ((0,47),0,0), ((9,47),0,0)], [5, 1, 1]),
```

Two rules the script enforces, both learned by shipping the failure first:

- **`COV_MIN..COV_MAX` = 18–55% ink coverage.** The wave band continues into *fully opaque*
  body cells (row 35 cols 50–55, row 34 cols 58–63) which look like more chevrons at
  thumbnail size and paint the ink over the whole tile. `flat()` raises rather than emitting
  the blotches; it caught two bad recipes.
- **Flowing biomes are `nvar=1`.** Mixing a plain variant with a full-width chevron gave
  hard-edged 8 px blocks — a chevron must meet another chevron to read as a surface. The
  wave cell is a top-*edge* band, horizontally periodic but not vertically, so a y-roll
  lifts the line off the seam. Ocean, sea, lava, acid and farm are one chevron line every
  8 px. Still ground (grass, rock, snow, sand) gets a plain variant plus two sparse
  textures, weighted so plain dominates.

`vweight` carries the weighting into the engine, which picks a variant per cell by a
position hash (`mote__at_variant`), so large areas do not repeat.

## 3. Synced blob47 rulesets — `tilesets/{hedge,floor_*,wall_*}`

Terrain roguemote already derives as a hand-drawn **47-cell blob autotile** is **copied,
not re-derived**: its `authoring/gen_terrain.py` is the single source of truth, and forking
a 300-line derivation would let the two copies drift. `sync_terrain()` copies these in as
editable source every run (and says so if roguemote has not generated them yet):

| Ruleset | Motebox job |
|---|---|
| `hedge` | **forest** — the garden-maze band reads as a canopy with real edges |
| `floor_jungle` | **meadow** — grass with dirt sides and gold steps |
| `floor_cobble` | **road / paved plaza** |
| `floor_grass` | the master's one pure-green fill, kept as a flat option |
| `wall_brick` / `wall_marble` / `wall_bone` / `wall_aztec` | human / elf / undead / orc city walls |

## 4. FX elemental recolours — `assets/sheets/fx_{fire,frost,acid,ash,holy,void}.png`

`blit` is colour-keyed with **no tint**, so a white FX frame can only ever draw white. The
73-frame monochrome FX band is therefore palette-swapped six ways, mapping each pixel by
luminance onto a two-stop ramp of source palette colours — so a bolt keeps its hand-drawn
highlight and core:

| Sheet | Ramp (highlight → core) |
|---|---|
| `fx_fire` | yellow → red |
| `fx_frost` | white → blue |
| `fx_acid` | yellow → dark green |
| `fx_ash` | light grey → dark grey |
| `fx_holy` | white → yellow |
| `fx_void` | slate → maroon |

**73 × 6 = 438 FX frames**, every one hand-drawn. This is where the 22 disasters get their
visual range without inventing art.

## 5. Font — synced from roguemote

`assets/font/rogue8_glyphs.png` + `.gsheet` → `src/rogue8.font.h` (`rogue8`), the complete
CP437 8×8 font as a proportional `MoteFont`. Used for dense in-world text (chronicle, soul
cards) where ~24 characters per line is the difference between a sentence and a fragment;
`ui_font(MOTE_FONT_MED)` handles titles, toasts and menu chrome. See DESIGN.md §9.

## 6. Icon — `icon.png` (60×60, game root) → `src/icon.h`

The world as a **disc** with a meteor coming in: one bold silhouette that reads at launcher
size. Composited from the game's own biome recipes plus the master's explosion cell, so the
icon cannot drift from what the game draws.

## Known gaps

- **Tundra is PICO-8 slate**, which reads faintly purple over large areas. The palette has
  no olive; the alternatives (dark grey, brown) are already ash and hill. Revisit if it
  still looks alien on the device panel.
- **No igloo** was found in the `props` rectangle the master's catalogue implies one in;
  (2,3) is a statue or fountain. Snow-biome housing needs another answer.
- **Sprite labels are roguemote's**, 50/1494 human-verified. The cell picks above were
  checked by eye at 13× and are trustworthy; anything added later should be too.
