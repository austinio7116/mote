# Source tileset catalogue — `source_tileset.png`

**Simple Roguelike Tileset** v0.16 by Ink_Slime (DC Slime) — **CC0 / Public Domain**
(https://ink-slime.itch.io/simple-roguelike-tileset)

- Sheet: **512×512**, **8×8 px** native tiles → a **64×64** tile grid.
- Coordinates below are **(col,row)** in tile units (0-indexed). Pixel = col·8, row·8.
- Recurring convention: many object families come in a **5-colour column-block**
  arranged as rows red / blue / yellow / green / grey-white.

## Vertical zones (rough)
| Cols | Pixel X | Theme |
|------|---------|-------|
| 0–15  | 0–127   | Dungeon objects + terrain/wall autotile blocks |
| 16–31 | 128–255 | Items: weapons, potions, treasure, food, tools, armour |
| 32–47 | 256–383 | Characters, NPCs, monsters, animals, bosses |
| 48–63 | 384–511 | Full CP437 font + UI/HUD icons |

---

## Rows 0–7
- **(0–1, 1–5)** Chests — closed (col 0) / open (col 1), 5 colour rows.
- **(2–4, 1–5)** Grey furniture: tables / altars / beds.
- **(5, 1–5)** Colour creature faces. **(6,1–5)** Skulls. **(7,1–5)** Keys.
- **(8–10, 1–4)** Colour rune/letter glyphs (A/M/Z style).
- **(11–14, 0–5)** Doors, big colour gems/slimes, banners; row 0 = ladders & curtain tops.
- **(16–23, 0–5)** Weapons (swords, spears, bows) + potions/flasks (colour).
- **(24–25, 0–1)** Scroll, arrow, small bomb, large bomb — **four separate items**, not the
  "2×2 NPC portrait" this was first catalogued as (the scroll's parchment reads as a face at 8×8).
- **(24–31, 5–7)** Torches, banners, barrels, goblet/trophy (gold), armour, books, food.
- **(32, 0)** `@` player glyph. **(32–47, 0–7)** Character heads/portraits + full-body
  adventurers (warrior/mage/rogue/ranger), villager heads, ghost/skull.
- **(48–63, 0–7)** **CP437 font block A**: symbols, digits 0–9, `@`, A–Z, `[\]^_`, a–z.
- **(0–9, 6–7)** Computer terminals, wooden logs / planks / fences / bridges.

## Rows 8–15
- **(0–15, 8–13)** Books, house/tent, igloo, helmet, lit torches; colour slime puddles,
  gems, red mushrooms, green snakes, rocks, clouds, bushes.
- **(0–15, 14–15)** **Magenta-bg UI icon strip**: hearts, eyes, stars, snowflakes,
  droplets, fire, letters (P/S), music notes.
- **(16–27, 8–13)** **Food**: meat, cherries, apple, fruit, watermelon, carrot, cheese,
  bread, burger, pie, egg (+ chef portrait at 16,12).
- **(32–47, 8–15)** **Animals/vermin**: dogs, deer, chickens, rooster, birds, bats, pigs,
  rats, snakes, spiders, frogs, lizards, mushroom-creatures.
- **(48–63, 8–15)** **CP437 font block B**: accented letters, box-drawing ─│┌┐└┘├┤┬┴┼,
  double lines, block elements ░▒▓█▄▀, math.
- **(25–29, 15)** Colour orbs / bubbles.

## Rows 16–23
- **(0–7, 16–18)** Purple/pink **brick wall** autotile set.
- **(0–15, 20–23)** Colour **solid blocks** (green / pink-red / blue-slate / yellow),
  4-col blocks each → floor/wall autotile candidates.
- **(16–31, 16–19)** Treasure: gold/silver/copper ore piles, gems, crown; colour armour/helmets.
- **(16–24, 20–23)** Table, barrels/crates, chalice, bones, shield.
- **(32–47, 16–23)** Monsters: birds, bats, cows/goats, goblins, demons, ghosts.
- **(48–53, 16–19)** **UI arrows** (8 directions). **(54–59,16–19)** gauges/signal bars, green screens.
- **(48–59, 20–21)** **Gamepad button prompts**: colour circles + `A/B/X/Y`.
- **(60–63, 20–23)** `O`/`X` face-button icons.

## Rows 24–31
- **(0–4, 25–28)** Grey **stone wall** autotile. **(5–10, 25–28)** Mountain / snow-capped peaks.
- **(0–15, 29–31)** Green **hedge/bush maze** autotile.
- **(16–31, 24–26)** Colour demon/imp creatures.
- **(16–31, 28–31)** **Tools/weapons**: pickaxes, axes, hammers (gold/iron), staffs, wands.
- **(32–40, 24–31)** Crowns (colour), armour, slimes, fire/explosion FX.
- **(48–63, 24–31)** **Status icons/emotes**: faces (happy/sad), check/✗, skulls; element
  icons (leaf, note, snowflake, fire, lightning, cross); **hearts full/half/empty** (health
  meter); speech bubbles (+ − … z !).

## Rows 32–39
- **(0–5, 32–34)** White marble/temple décor (windows, fountains).
- **(0–15, 35–39)** Dungeon **stone-brick wall** autotile (navy/grey + light mortar, arch/doorway).
- **(16–30, 32–34)** Elemental weapons (ice/fire swords, colour).
- **(16–22, 35)** **Guns** (pistols/rifles, sci-fi).
- **(32–47, 32–39)** **Boss monsters** — 17 sprites, every one **2×2**, on rows 33–38 only.
  Cols 32–39 fill all three bands; cols 40–47 only the top band, plus a giant green insect at
  (40–41, 35–36). Note (40–41, 33–36) is *two* bosses (medusa above, insect below), not one tall one.
- **(48–51, 32)** Gender/alchemy symbols.
- **(48–63, 33–39)** Terrain edges: **water/waves**, **clouds/snow**, **rocks/boulders**.

## Rows 40–47
- **(0–15, 41–43)** Green **grass/jungle** autotile (with orange flowers).
- **(0–15, 44–46)** **Temple/aztec wall** autotile (navy + gold bands, columns, steps).
- **(0–15, 40 & 47)** Snow / rubble ground. Cols 16–63 empty.

## Rows 48–55
- **(0–12, 48–49)** White abstract/dither pattern tiles.
- **(0–11, 50–52)** White **blueprint / floor-plan** line-art tiles.
- **(0–5, 53–55)** White top-down **furniture** (beds, tables, dressers).
- **(60–63, 48–50)** White cross / puzzle-piece shapes. Rest empty.

## Rows 56–63
- **(0–~30, 60–63)** CC0 **credits banner** text + two author avatars — *not game content*.
- Everything else empty.
</content>
