# Roguemote asset library

Carved from the **[Simple Roguelike Tileset](https://ink-slime.itch.io/simple-roguelike-tileset)**
v0.16 by **Ink_Slime** (DC Slime), **CC0 / public domain**. Source is a 512×512
sheet of **8×8** tiles (64×64 grid).

Every tile, character, item and effect Roguemote draws is from that sheet —
nothing is drawn on top of it. CC0 waives the requirement to attribute, so the
credit is in the game's title screen, its gallery page and its guide because it
is deserved, not because it is required.

## Pipeline (editable source → baked header)

The authoring pipeline lives in `authoring/` and is fully reproducible:

```bash
python3 authoring/extract.py       # source_tileset.png -> assets/sheets, assets/font, tilesets/
../../tools/mote bake .            # -> src/*.h  (or Studio Save)
python3 authoring/gen_catalogue.py # -> /tmp/roguemote_sprites.html (review page)
python3 authoring/gen_annotator.py # -> /tmp/roguemote_annotator.html (correction tool)
python3 authoring/apply_labels.py <export.json>   # fold a correction pass back in
```

- `authoring/source_tileset.png` — the CC0 master (not baked; lives outside `assets/`).
- `authoring/CATALOGUE.md` — full zone map of the source grid.
- `authoring/catalogue/*.png` — labelled zoom crops used while cataloguing.

### Tile labels — who decided what

Labels live in two layers, so regenerating the agent pass never destroys a human call:

| File | Source | Precedence |
|---|---|---|
| `labels_ai.json` | `merge_labels.py` over `batches/out_*.json` (vision-agent pass) | base |
| `labels_human.json` | `apply_labels.py` over an annotator export | **wins** |

`apply_labels.py` also propagates each correction two ways:

- **Twins** — 61 tiles fall inside two overlapping subsheet regions, so the annotator
  draws them on two cards. Correcting one has to fix the other (same 8×8 tile), and
  the annotator now syncs twin cards live so a stale twin can't win on export.
- **Colour blocks** — most families are one column repeated down five colour rows
  (red / blue / gold / green / grey-white), so a fix on one row carries to the rest with
  the colour word swapped. `COLOUR_BLOCKS` in `apply_labels.py` is a hand-verified
  table, *not* inference — look at the block in the source sheet before adding one.

## Sprite subsheets — `assets/sheets/*.png` → `src/<name>.h` (`<name>_img`)

Thematic RGBA subsheets (palette index 0 = transparent). Layout preserved from source,
so colour-variant families stay column/row aligned. ~28 sheets covering: chests,
furniture, keys/skulls/faces, runes, doors/gems/banners, props, trinkets, weapons,
potions, food, treasure/ore, tools, guns, characters, animals, monsters, bosses,
crowns/FX, UI icons, arrows, button prompts, status/emotes, symbols, portraits.

## Autotile rulesets — `tilesets/*.tileset` (+ sheet) → `src/<name>.tiles.h` (`<name>_at`)

| Ruleset | Template | Notes |
|---|---|---|
| `hedge` | **blob47** | Garden hedge maze; `edge=0`. 47/47. |
| `wall_bone` | **blob47** | Cream/bone outline wall; `edge=1`. 46/47, hollow interior. |
| `wall_brick` | **blob47** | Dungeon stone-brick; `edge=1` (seamless map border). 47/47. |
| `wall_marble` | **blob47** | White marble with navy trim; `edge=1`. 46/47, hollow interior. |
| `floor_jungle` | **blob47** | Jungle grass, dirt sides, gold steps; `edge=0`. 47/47. |
| `wall_aztec` | **blob47** | Aztec temple facade; `edge=1`. 46/47, hollow interior. |
| `wall_blueprint` | **EDGE16** | Thin white floor-plan lines; `edge=0`. 16/16. |
| `wall_plaster` | **EDGE16** | Thick white wall with drop shadow; `edge=0`. 15/16, no isolated-tile cell. |
| `floor_cobble` | fill (nvar 2) | Grey cobblestone floor, 2 variants. |
| `floor_grass` | fill (nvar 2) | Dark jungle-grass floor. |
| `water` | fill (nvar 2) | Blue ripple bands (source has no flat water tile). |

### blob47

Six terrains are full **47-cell blob autotiles**, covering all 256 neighbour masks. They
began as nine-slices, which can only express **16 of the 47** configurations — a nine-slice
has no inner corners and no cell with borders on opposite sides, so inner corners and
1-tile-wide walls drew wrong.

The source sheet **already draws a complete 47-tile blob set** for each — laid out as a row
of blocks of mixed width (some 3×3, some 2×3) across 16 columns × 3 rows: 48 cells, one
blank, 47 tiles, every configuration drawn once by hand. Nothing is synthesised.

The sheet stacks **six bands three rows apart** — rows 29, 32, 35, 38, 41 and 44 — alternating
a solid-interior terrain with a hollow-interior one (an outline style whose middle is meant to
show through). The hollow ones are 46/47: their interior cell is blank *by design*, declared as
a hole rather than shipped silently.

**Every band uses the same 16×3 layout**: relative position (col,row) always holds the
same configuration, with (15,2) blank. `map_blob47.py` derives that layout once, by classifying
the brick and hedge bands from their own pixels — those two agree at all 48 positions — and
`gen_terrain.py` then reads any band through it. A terrain is just *where its band starts*.

That matters because a colour-based classifier cannot read every band: `floor_jungle` uses a
different border colour per edge (grassy top, dirt sides, gold bottom), so no single-border-colour
test works on it. The shared layout sidesteps the problem entirely.

`build()` refuses to emit a sheet if the band has any blank cell not declared in `holes`, so a
wrong band start is a hard error rather than an atlas full of gaps.

### The monochrome bands are EDGE16, not blob47

Rows 50–52 and 53–55 are **line art** — bars, corners, T-junctions, crosses, ends — which is
the engine's `MOTE_AT_EDGE16` template, not a filled terrain. A tile connects to its N/E/S/W
neighbours and the cell index is those four bits (`c = N | E<<1 | S<<2 | W<<3`) on a 4×4 sheet.
`map_blob47.connections()` reads which edges a line touches; both bands map cleanly.

Rows 47–49 are a **dither/noise texture**, not an autotile: it scores 12/16 as EDGE16 and
renders as speckle. It stays a raw subsheet, as do the top-down furniture sprites.

```bash
python3 authoring/map_blob47.py       # which config each source tile draws
python3 authoring/gen_terrain.py      # -> tilesets/{wall_brick,hedge}.{png,tileset}
python3 authoring/verify_terrain.py   # exhaustive checks, incl. a classifier round-trip
python3 authoring/mutate_terrain.py   # prove the checks can actually fail
cc -I../../sdk -I../../engine/render -o /tmp/ct authoring/ctest_blob47.c && /tmp/ct
python3 authoring/preview_terrain.py  # -> /tmp/roguemote_terrain/*.png evidence
python3 authoring/gen_terrain_report.py  # -> a single review page of all of it
```

`gen_terrain.py` is invoked by `extract.py`, so the one-command pipeline still holds. It must
not be duplicated as a nine-slice there — both write the same `tilesets/<name>.png`.

### What can't be autotiled

Scanning every 3-row band at every column offset, only the six above hold the blob47 layout
and only rows 50–52 / 53–55 are EDGE16. Several regions CATALOGUE.md calls autotile sets are
not: `wall_purple` is pink decorative pieces, the "grey stone wall" is a 5-shade cobblestone
*floor*, and the colour-block region is four colours × four columns (20 tiles each), which is
neither template. These ship as **raw subsheets** (`panels_colour`, `wall_purple`,
`wall_stonebrick`, `wall_temple`, `grass_garden`, `cobble_floors`, `terrain_edges`) — author
a rule in the Studio Tiles tab if a game needs one.

## Font — `assets/font/rogue8_glyphs.png` + `.gsheet` → `src/rogue8.font.h`

Complete **CP437** 8×8 font, baked two ways:
- `rogue8` **MoteFont** (proportional) — draw with `mote->text_font(fb, &rogue8, …)`.
- `font_cp437` tile atlas (monospace blit; char `c` → cell `c%16, c//16`).

## Verification harness — `src/game.c`

Not a game yet: renders a slice of every asset group (autotiled dungeon, sprites, both
fonts, heart meter) to prove the whole pipeline bakes, compiles and draws. Confirmed
rendering headless on ABI v47.

Its wall layer is laid out to exercise the blob47 cases a nine-slice gets wrong — L and T
junctions (inner corners), a 1-tile-wide spur (borders on both sides at once), a
free-standing tile, a hole inside a block, and two blocks touching only at a diagonal. It
hits **19 of the 47 cells, 8 of them inner-corner cells**, so one look at the device
confirms the ruleset.

## TODO before this becomes a real game
- ~~Author `icon.png` (60×60) in the game root.~~ Done — `assets/gen_icon.py`
  composites the source tileset's `@` glyph + open chest tile.
- Finish the correction pass. 50 of 1494 tiles are human-decided so far
  (`apply_labels.py --report`); the rest are still agent guesses, 393 of them
  low-confidence. Review in the annotator, export, `apply_labels.py <export>`.
- Once labels settle, re-cut any region the corrections proved wrong: row 8
  cols 0–5 are train track (the agent read them as fence/rope) and cols 11–14
  rows 1–5 are door-closed / door-open / house / house-extend (read as
  gems and banners), so `SPRITE_SHEETS`' `props_light` and `doors_gems_banners`
  rectangles are named for the wrong contents.
</content>
