# TerraMote — a Terraria demake for the Thumby Color (Mote engine)

Side-view sandbox: dig, build, craft, fight. One seeded persistent world per save.

## Architecture

- **Tiles are 8×8 px** → 16×16 tiles visible on the 128×128 screen.
- **World**: 448 cols × 248 rows, two byte-planes in the ARENA (~217 KB):
  - `fg[]` — foreground tile id (0 = air, 1..N drawn by the ENGINE autotiler
    via `scene2d_set_autotiles`, one baked `.tileset` ruleset per tile id).
  - `bg[]` — packed: low nibble = wall id, high nibble = liquid (bits 4-6 level
    0-7, bit 7 = lava).
- **Render pipeline** (no render_band — engine does the dual-core work):
  1. `set_background_cb`: sky gradient + sun/moon + clouds + parallax hills
     (surface) or cave backdrop; then visible wall tiles (autotiled manually via
     `mote_autotile_mask` + wall rulesets, art pre-darkened).
  2. Engine 2D scene: autotiled fg tiles + entity sprites (player layers,
     enemies, drops, projectiles).
  3. `overlay()`: liquid translucency blend, then the **lighting pass** — a
     per-tile light window (BFS from sunlight columns + torches + lava +
     glowing tiles) bilinearly interpolated per pixel and multiplied over the
     framebuffer — then HUD/menus (untouched by darkness).
- **Character builder**: the player body sheet is baked palette-indexed
  (≤16 colours). At runtime the palette is copied to RAM and the reserved
  skin/shirt/pants/hair slots are rewritten → recolouring with zero
  compositing. Hair styles are separate cells overlaid on the head with a
  per-frame offset table. Enemy colour variants (slimes) use the same trick.
- **Persistence** (`kv_*`): world planes RLE-compressed in row-band chunks
  (`w0..`), player blob (`plr`): appearance, inventory, HP, spawn, world seed,
  time, boss-killed flags.

## Controls

- D-pad move; A jump; B use held item (swing / mine / place / fire / eat).
- Aim: the reticle sits one tile ahead of facing; UP/DOWN while holding B tilts
  aim; DOWN+B targets below the feet (digging down).
- LB cycle hotbar (hold LB + LEFT/RIGHT to move fast); RB inventory/crafting;
  RB on a station in range shows its recipes; MENU pause (save & exit).

## Tech tree

wood → workbench → wooden tools/sword/bow → torches (gel) → furnace (stone)
→ copper/iron/gold bars → anvil (iron) → metal picks/swords/armor
→ 6 lenses @ demon altar → Suspicious Eye → **Eye of Cthulhu** (night)
→ demonite bars → Light's Bane + Nightmare Pickaxe → mine hellstone/obsidian
in the underworld → molten armor / Volcano sword / Molten Fury bow.

Pick tiers gate tiles: wood < stone(any) — copper needed for gold ore,
gold pick for demonite/ebonstone, nightmare for hellstone+obsidian.

## World layout (rows)

sky 0..~55 · surface w/ biomes (snow | forest | desert | corruption) ~55-75 ·
dirt 75-100 · stone 100-205 (caves, ores, underground mushrooms, chests) ·
underworld 208-248 (ash, lava pools, hellstone).

## Enemies

green/blue slime (day), zombie + demon eye (night), bat + skeleton (caverns),
lava slime (hell), Eye of Cthulhu (summoned, 2 phases). Contact damage,
knockback, i-frames, drops (gel, lenses, coins, hearts).
