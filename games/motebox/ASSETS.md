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

### The geometry was READ OFF the artist's pixels, not guessed

Three versions of this got it wrong by reasoning about what a good tileset ought to
be. The fourth dumped `hedge`'s cells as a colour map. That settles it:

```
   body cell     N open      NW convex    island       inner NW
   00000000      ........    ........     ........     .RR.....
   00000000      RRRRRRRR    ..RRRRRR     .bRRRRb.     R.R.....    . = transparent
   00000000      RRRRRRRR    .RbRRRRR     .bRbbRb.     RR......    R = BRIGHT rim
   00000000      bbbbbbbb    .RRbbbbb     .bRbbRb.     ........    b = base
```

1. **The interior is FLAT.** One solid colour, every pixel — `hedge` and
   `floor_jungle` both. All the character is in the edge cells. Earlier versions
   spent everything on interior patterns and gave the edge a single-pixel line,
   which is exactly backwards, and it is why large areas read as wallpaper.
2. **The rim is 2 px thick and BRIGHTER than the base.** A 1 px line reads as an
   outline; a 2 px band of a lighter tone reads as a *lit edge*, which is what makes
   a patch look like a raised, shaped thing.
3. **Corners round with a single pixel**, and the rim thins to 1 px where opposite
   edges are both open. That is the whole secret of round islands, shapely coasts
   and one-tile rivers — no chamfer, no three-pixel bevel.

The one thing not copied is the transparency. `hedge` insets itself a pixel on every
open side because it is an *overlay* on a floor; Motebox draws one terrain per cell
with nothing underneath, so an inset would put a dark gap around every patch — two,
where neighbours both contribute. So the rim runs to the tile edge and the corner
rounding is done in base colour.

### Only the south and east are lit on soft ground

Rimming all four sides of everything is a trap this fell into twice. When both
neighbours rim the edge they share, that edge carries **two** bright bands, and a
continent of grass, savanna and sand reads as a jigsaw of outlined pieces.

So `sides="NSEW"` is for materials with a physically real edge all round — water has
foam, rock has a cliff, lava has a hot line, a ploughed field has a boundary.
Everything soft gets `sides="SE"`: one light direction, so exactly one of any two
neighbours draws a band on their shared edge. Every patch still has a lit side and a
shaped corner, and nothing is outlined.

### Patterns must earn their place, and no pattern is masonry

`plain` is the default and nearly the only answer. A pattern is allowed only where
the real surface has one: water and lava keep `dashes` (a liquid is a surface in
motion), farmland keeps `stripes` (furrows, the one biome that *should* look
man-made), and rubble/swamp/scorched carry a second variant.

**`brickwork` is gone from the ground entirely.** Three versions used staggered
courses for hill, mountain, peak and rubble on the argument that `wall_brick` is the
most legible 8 px texture in the master. True, and beside the point — `wall_brick` is
a **wall**. Laid over terrain it made a map with visible mortar: a red brick wall
with villagers standing on it. Ground has no joints.

Colour **pairs** matter as much as geometry: base and rim come from the same family
and the rim is the brighter one — DKGREEN rimmed GREEN (literally `hedge`'s pair),
BROWN rimmed ORANGE, DKGREY rimmed LTGREY. A contrasting hue makes ribbons; a *dark*
rim (NAVY, BLACK) is what "why does the grey texture have black outlines?" was
looking at. Nothing rims dark. The sea was once drawn as SLATE — a mauve — on BLUE,
which came out as pink zigzags on cyan; water is two blues, or blue with white foam.

| Biome | Rim | Sides | Reads as |
|---|---|---|---|
| ocean / sea | pale blue / white | all | deep water, then foam |
| shallow | white | all | a river — the rim thins to 1 px, so it stays water |
| lava / acid | yellow | all | a hot edge glowing against what it eats |
| mountain / peak / rubble | light grey / white | all | a cliff |
| farmland | orange | all | a ploughed boundary |
| grass, sand, snow, hill, ash… | brighter own hue | **SE** | a lit side, one sun |

Check it with `python3 authoring/preview_terrain_map.py` — it renders a synthetic
coastline, a diagonal river and a round island through the engine's own mask→cell
logic, and it will also render the artist's 47×1 sheets for comparison. A preview
that repeats one tile tells you nothing about a tileset whose entire job is edges,
which is exactly how two versions of this shipped.

**And `mote bake` is not optional.** `extract_box.py` writes `tilesets/*.png`; the
game compiles `src/*.tiles.h`. Regenerating art without baking means the build keeps
drawing the old tileset, which cost a full round of "this is still a brick wall".

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
