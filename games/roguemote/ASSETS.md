# Roguemote asset library

Carved from the **Simple Roguelike Tileset** v0.16 by Ink_Slime (DC Slime), **CC0 /
public domain**. Source is a 512×512 sheet of **8×8** tiles (64×64 grid).

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

Full rule files, verified against the engine raster (`authoring/preview_autotile.py`):

| Ruleset | Template | Notes |
|---|---|---|
| `wall_brick` | nine-slice | Dungeon stone-brick walls; `edge=1` (seamless map border). **Verified.** |
| `hedge` | nine-slice | Garden hedge maze; `edge=0`. **Verified.** |
| `floor_cobble` | fill (nvar 2) | Grey cobblestone floor, 2 variants. |
| `floor_grass` | fill (nvar 2) | Dark jungle-grass floor. |
| `water` | fill (nvar 2) | Blue ripple bands (source has no flat water tile). |

Decorative terrain that isn't a clean autotile is shipped as **raw subsheets** instead
(`panels_colour`, `wall_purple`, `wall_stonebrick`, `wall_temple`, `grass_garden`,
`cobble_floors`, `terrain_edges`) — author a rule for these in the Studio Tiles tab if a
game needs one.

## Font — `assets/font/rogue8_glyphs.png` + `.gsheet` → `src/rogue8.font.h`

Complete **CP437** 8×8 font, baked two ways:
- `rogue8` **MoteFont** (proportional) — draw with `mote->text_font(fb, &rogue8, …)`.
- `font_cp437` tile atlas (monospace blit; char `c` → cell `c%16, c//16`).

## Verification harness — `src/game.c`

Not a game yet: renders a slice of every asset group (autotiled dungeon room, sprites,
both fonts, heart meter) to prove the whole pipeline bakes, compiles and draws. Confirmed
rendering headless on ABI v47.

## TODO before this becomes a real game
- ~~Author `icon.png` (60×60) in the game root.~~ Done — `assets/gen_icon.py`
  composites the source tileset's `@` glyph + open chest tile.
- Finish the correction pass. 38 of 1494 tiles are human-decided so far
  (`apply_labels.py --report`); the rest are still agent guesses, 393 of them
  low-confidence. Review in the annotator, export, `apply_labels.py <export>`.
- Once labels settle, re-cut any region the corrections proved wrong: row 8
  cols 0–5 are train track (the agent read them as fence/rope) and cols 11–14
  rows 1–5 are door-closed / door-open / house / house-extend (read as
  gems and banners), so `SPRITE_SHEETS`' `props_light` and `doors_gems_banners`
  rectangles are named for the wrong contents.
</content>
