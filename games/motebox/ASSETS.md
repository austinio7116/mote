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

**Generated from a designed vocabulary** (`authoring/biomes.py`), not stamped from the
master. The first version of this pipeline picked "texture cells" out of the master by
**ink coverage** — any hand-drawn cell covering 18–55% of a tile was a candidate — and
composited the winner over a flat colour. That is selection without looking, and it
produced exactly what it deserved:

- six biomes (mountain, ash, scorched, tundra, peak, ice) wore the **same
  diagonal-chunk motif** in six different colours, so none of them had an identity
- four more (grass, savanna, desert, swamp) wore the same arrow-blob
- snow was scattered with the master's **hearts**, tundra with its **plus signs**
- mountains were covered in neat **masonry brick**

Rows 47–49 of the master are decorative **line art** — dashes, brackets, arrows,
hearts, scales, arches — drawn to edge a dungeon room. One motif tiled across a
continent is wallpaper however good the motif is, because real terrain has no
repeating unit.

### The vocabulary

Seven generators, each saying what a material *is*. Every colour is one of the
master's sixteen; every pattern **wraps** at the tile edge, so it is seamless; each
biome gets 3–4 variants and the engine picks one per cell by position hash, so a large
area never shows a repeat.

| Generator | Says | Used by |
|---|---|---|
| `grains` | a surface of particles, two tones for depth | sand, ash, dust |
| `ripple` | short offset horizontal dashes — windswept | dunes, beach |
| `blades` | upright 2 px marks: it stands up, so it reads as a plant | grass, savanna, tundra |
| `clumps` | 2×2 with a shadow — the smallest shape that reads as an object | pebbles, muck, rubble |
| `facets` | diagonal light/shadow runs — stone is planes meeting at edges | mountain, rock |
| `cracks` | a one-pixel line **with momentum** | ice, scorched |
| `furrows` | widely spaced rows with crops standing between them | farmland |
| `snowcap` | light on the upper edge — a peak has to have an *up* | peaks |

**Variant 0 of every biome is the plain base**, weighted heaviest, so a biome reads as
its colour first and its texture second. That ordering is most of what stops this
becoming wallpaper again.

Two generators were wrong on the first pass and the fix is recorded in each
docstring: `cracks` turned on a coin flip every pixel and drew **L-shaped glyphs**
instead of cracks (it needed momentum), and `furrows` at period 3 with a dot grid read
as **chain-link fence** instead of a field (it needed wide spacing and upright crops).

### What the master still draws better than we can

Its **chevron wave band** (cell 48,35) is genuinely good water — drawn as flowing
liquid, and it tiles. So ocean, sea, lava and acid are that cell over a palette flat
and nothing is generated; the shallows keep its hand-drawn **sandbar blob** (56,34).
And roguemote's six **blob47** bands beat anything here, so they are synced (§3). The
coverage guard survives for the two cells we do take: the wave band continues into
fully opaque body cells that look identical at thumbnail size.

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
