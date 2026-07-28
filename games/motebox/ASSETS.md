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

### Cells are picked from the LABEL SET, not by eye

`roguemote/authoring/labels_{ai,human}.json` labels 1306 of the master's cells with a
name, a category and a confidence, and `labels_human` wins. **Use it.** Two passes at
the sprite tables guessed instead: the first drew sheep as the master's *chicken*,
wolves as its *snake*, and bears and boars both as the same *cow face*; the second drew
every civ race as a **portrait bust** from rows 5–6 (they are busts, not bodies) and
the orc as *"witch/mage casting (purple)"*.

**The five races are five humanoid peoples**, all full-body figures:
`human` characters[2,3] "adult" · `elf` characters[7,3] "green-hooded ranger (pointed
hat)" · `dwarf` characters[5,3] "dwarf" (human-labelled) · `orc` monsters[7,4] "goblin
(green)" · `troll` monsters[2,1] "brown troll/ogre".

**The wildlife is wild first.** The animals sheet is genuinely farm-heavy — the labels
count dogs, pigs, chicks, hens, lambs, sheep, ducks and geese — so the wilderness is
stocked from what *is* wild in it (deer, the navy wolf, snake, spider, bat, rat, frog)
plus the goat off the monsters sheet, and **sheep and hens are livestock that only
spawn beside a village**. A wilderness full of poultry is a farmyard, not a world.

### Other cells worth knowing

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

## 2. Biome terrain — real blob47 sets

`authoring/terrain.py` + `authoring/biomes.py` → `tilesets/bio_*.{png,tileset}`

Every biome is a 47-cell blob47 autotile, generated. The mask→cell contract is
**imported from `roguemote/authoring/blob47.py`**, never reimplemented, so there is
one definition of "cell 23" in the repo. Layout is 8 × 6 = 48 cells (47 used) per
variant block, stacked for `nvar`, matching how the engine steps variants.

Three things had to be true before it stopped looking like squares, and each was
learned by shipping the failure:

### 1. It has to actually be blob47 — with corners CUT

A rim around a square tile is not a blob47 set. Every convex corner stays a hard
right angle, so a coastline is a staircase and a diagonal river is a flight of steps.
Each cell now **chamfers its outside corners**: where two adjacent edges are both
open, the corner pixel and one step along each edge are cut back to a darker tone, so
the silhouette reads as a 45° bevel. That single change is what gives shapely coasts,
round islands and diagonal rivers.

(The engine draws one terrain per cell with nothing beneath, so a cut cannot be
transparent — it has to be a colour. A darker tone reads as cut, which is the trick
hand-drawn sets use.)

### 2. Interiors must have STRUCTURE, or be flat

Compare the artist's own sets: `wall_brick`'s interior is a **regular staggered
course** of blocks and mortar; `hedge`'s is simply **flat** with a bright rim. Both
read perfectly. What does not read is random scatter — grains and blades sprinkled at
random look like litter dropped on a flat colour.

So the vocabulary is regular: `dashes` (offset courses — water, dunes, lava),
`brickwork` (staggered courses — rock, strata), `specks` and `tufts` (a fixed
lattice — grass, snow, ash), `stripes` (furrows — ploughed field), `capped` (light on
the upper rows — peaks), `plain` (flat, and often the best answer), and `cracks` (the
only irregular one, because a regular crack is a joint). Nothing is placed at random.

Colour **pairs** matter as much as pattern: the sea was once drawn as SLATE — a mauve —
on BLUE, which came out as pink zigzags on cyan. Water is two blues, or blue with
white foam. Nothing else.

### 3. Only some biomes rim

When every biome rimmed itself, every boundary carried **two** rims — one from each
side — and the map read as outlined ribbons. Soft ground (grass, snow, sand, ash,
hill, swamp, savanna, tundra, rubble) now has `rim=None` and simply stops; water,
rock, peak, ice, lava and ploughed land keep theirs, because those edges are
physically real. Everything still cuts its corners.

And a rim must survive a **one-tile-wide band**, which is entirely rim. Shallow water
had a sand-coloured rim, so every river in the world rendered as a peach footpath.

| Biome | Rim | Reads as |
|---|---|---|
| sea | white | foam |
| ocean | pale blue | deep water at the shore |
| shallow | *none* | a one-tile river is still water |
| lava | yellow | a hot edge glowing against what it eats |
| rock, peak | light top + dark underside | a cliff |
| farmland | dark grey | a ploughed boundary |
| grass, sand, snow, ash… | *none* | the neighbour's edge is the edge |

Check it with `python3 authoring/preview_terrain_map.py` — it renders a synthetic
coastline, a diagonal river and a round island through the engine's own mask→cell
logic. A preview that repeats one tile tells you nothing about a tileset whose entire
job is edges, which is exactly how two versions of this shipped.

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

## 6. Sound — `assets/*.sfx` → `src/*.sfx.h`

Fifteen SFXR recipes, each ~88 bytes of flash and no RAM, streamed through
`audio_play_sfx`. Deliberately few and deliberately quiet: a god sim is a thing you
leave running, and at ×8 a year goes past in under a second, so a world that pinged
every time a villager picked a berry would be unlistenable.

`fire · boom · quake · thunder · splash · freeze · build · found · war · fall ·
titan · bless · curse · deny · age`

Two rules make it work: every sound is **rate-limited** (a firestorm lights hundreds
of cells a second and should sound like one fire), and the headline sounds hang off
the **chronicle** rather than off the code that causes them — a thing worth a
headline is exactly a thing worth a noise, so the two can never drift apart.

## 7. Icon — `icon.png` (60×60, game root) → `src/icon.h`

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
