# Roguemote asset library

Carved from the **Simple Roguelike Tileset** v0.16 by Ink_Slime (DC Slime), **CC0 /
public domain**. Source is a 512×512 sheet of **8×8** tiles (64×64 grid).

## Pipeline (editable source → baked header)

The authoring pipeline lives in `authoring/` and is fully reproducible:

```bash
python3 authoring/extract.py       # source_tileset.png -> assets/sheets, assets/font, tilesets/
../../tools/mote bake .            # -> src/*.h  (or Studio Save)
python3 authoring/gen_catalogue.py # -> /tmp/roguemote_sprites.html (review page)
```

- `authoring/source_tileset.png` — the CC0 master (not baked; lives outside `assets/`).
- `authoring/CATALOGUE.md` — full zone map of the source grid.
- `authoring/catalogue/*.png` — labelled zoom crops used while cataloguing.

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
- Author `icon.png` (60×60) in the game root.
- Fold the user's catalogue corrections back into `authoring/extract.py`.
</content>
