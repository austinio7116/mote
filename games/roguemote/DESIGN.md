# Roguemote — design

A turn-based roguelike for the Thumby Color: a **Zelda/FF-shaped overworld** over
**Moria-shaped dungeons**, with Moria/Zangband combat, shops, item depth and spell effects.

The organising constraint is deliberate: **every sprite in the library earns a place**.
1670 non-empty tiles across 36 subsheets, plus 8 autotile rulesets. Where a design choice
below looks odd, it is usually because it is the honest way to give a sheet a job.

---

## 1. Hard constraints

These are measured, not assumed — they shape everything after.

| | |
|---|---|
| Screen | **128×128 px** (`engine/core/mote_config.h`) — **16×16 tiles** at 8px |
| Input | 9 buttons: D-pad, **A**, **B**, **LB**, **RB**, **MENU** (`engine/input/mote_input.h`) |
| ABI | v47 — `kv_set/get` blob storage, `sfx`, `text_font`, `blit_ex` (rotate+scale), `draw_line/rect/pixel`, `draw_circle`, autotile layers |
| Game RAM | ~134 KB arena (the Moria port fits in it, with the full Umoria core) |
| Tiles | 8×8, palette index 0 = transparent |

**The 16×16 viewport is the dominant design pressure.** You can see a 16-tile square and
nothing more. Every decision below — camera, HUD, dungeon room scale, how bosses read — is
downstream of that.

### Screen layout

```
┌────────────────────────────┐  0
│                            │
│      MAP VIEWPORT          │  16 × 13 tiles = 128 × 104 px
│      (camera follows,      │
│       scrolls smoothly)    │
│                            │
├────────────────────────────┤  104
│ @Kaeda  HP▓▓▓▓░ SP▓▓░  L12 │  24 px HUD, 3 text rows in rogue8
│ ⚔+2 ✦Fire  $340   DL:17    │
└────────────────────────────┘  128
```

13 rows of map is enough to read a room; the HUD is always live so you never open a screen
to learn you are dying. `rogue8` (proportional CP437) for the HUD, `font_cp437` tile atlas
where monospace columns matter (inventory, stores).

---

## 2. Shape of the game

```
        TITLE ──► CHARACTER CREATION ──► OVERWORLD
                                            │
                    ┌───────────────────────┼───────────────────────┐
                    ▼                       ▼                       ▼
                  TOWN                 WILD ENCOUNTER          DUNGEON MOUTH
              (6 shops, temple)      (animals, ambushes)             │
                    │                                                ▼
                    └────────────────────────────────────►  DUNGEON  DL:1 … DL:50+
                                                            (Moria: stairs, depth,
                                                             out-of-depth loot, uniques)
```

**Overworld** is the Zelda/FF layer: a generated continent you walk across, with towns,
shrines, and dungeon mouths. It is *not* where the depth lives — it is the connective
tissue and the thing that makes the world feel like a place.

**Dungeons** are the Moria layer: descend, get greedy, die. Depth is the difficulty knob.

---

## 3. Terrain → the eight rulesets

The rulesets are the reason the world can look like two different games. Each depth band
gets its own wall identity, so descending *looks* like descending.

| Layer | Ruleset | Where |
|---|---|---|
| Overworld ground | `floor_jungle` (blob47) | grass/dirt/gold-step continent |
| Overworld hedge | `hedge` (blob47) | garden mazes, shrine grounds, town walls |
| Dungeon **DL 1–10** | `wall_brick` (blob47) | the Mines — dungeon stone-brick |
| Dungeon **DL 11–20** | `wall_bone` (blob47) | the Catacombs — hollow-interior bone |
| Dungeon **DL 21–30** | `wall_marble` (blob47) | the Sunken Temple — marble + navy |
| Dungeon **DL 31–40** | `wall_aztec` (blob47) | the Lost City — gold/purple facade |
| Dungeon **DL 41+** | `wall_plaster` (EDGE16) | the Vault — thick white, unreal |
| Special | `wall_blueprint` (EDGE16) | the Architect's floors — line-art rooms, a surreal set-piece band |

The three **hollow-interior** sets (bone/marble/aztec) are a feature, not a defect: their
transparent middles let a floor layer read *through* the wall, which is exactly the look
you want for ruins. `scene2d_set_autotile_layers` already composites layers bottom-up.

Raw subsheets fill the rest of the world: `boulders_mountains` (impassable ranges),
`terrain_edges` (coast/cliff/water borders), `grass_garden` (decorative overworld fringe),
`cobble_floors` + `panels_colour` (town and shop interiors), `wall_stonebrick` /
`wall_temple` (architectural set-dressing: arches, doorways, windows).

---

## 4. Sprite budget — every sheet has a job

This is the contract. Numbers are non-empty tiles as counted from the baked sheets.

### Actors

| Sheet | n | Job |
|---|---|---|
| `characters` | 85 | **12 player classes** (each with its own sprite) + townsfolk, shopkeepers, quest NPCs |
| `animals` | 83 | Overworld wildlife + early dungeon: rats, bats, snakes, spiders, dogs, boars, birds |
| `monsters` | 72 | Dungeon proper: goblins, orcs, demons, ghosts, oozes, elementals |
| `bosses` | 68 | **17 bosses** (2×2 each) — one per depth band, plus uniques |
| `crowns_fx` | 23 | Spell impacts, explosions, summon flashes, crowns as boss regalia |

### Items

| Sheet | n | Job |
|---|---|---|
| `weapons_potions` | 32 | Base melee weapons + potion flavours |
| `weapons_elemental` | 29 | Ego/branded weapons (fire/ice/lightning swords) |
| `tools_wands` | 35 | Wands, staves, rods, digging tools |
| `treasure_ore` | 35 | Gold, gems, ore — the loot economy |
| `trinkets` | 55 | Rings, amulets, charms — the "+X and perks" slot |
| `food` | 36 | Rations, mushrooms (some cursed), cooking |
| `chests` | 10 | Trapped/locked containers, 5 rarity colours |
| `loot_furniture` | 16 | Barrels, crates, chalices, bones — smashables |
| `items_scroll_bomb` | 4 | **Scroll, arrow, small bomb, large bomb** (your correction) |
| `guns` | 7 | The Vault's anachronistic tier — DL 41+ only |
| `runes` | 12 | Rune-carving / enchanting system |
| `faces_skulls_keys` | 15 | Keys (5 colours = 5 door tiers), skulls, masks |
| `doors_gems_banners` | 24 | Doors (5 colours), gems, house/shop fronts |

### World & UI

| Sheet | n | Job |
|---|---|---|
| `props_light` | 25 | Torches, signs, barrels, **train track** (Vault rails), terminals |
| `furniture_stone` | 15 | Levers (5 colours × 3 states) — puzzle mechanism |
| `furniture_white` | 16 | Town interiors: beds, tables, dressers |
| `boulders_mountains` | 22 | Impassable overworld ranges, snow peaks |
| `blueprint` | 34 | The Architect's floors |
| `ui_status_emotes` | 83 | Status icons, hearts, element icons, speech bubbles |
| `ui_icons_tiny` | 32 | HUD micro-icons |
| `ui_arrows_gauges` | 34 | Menu arrows, bars, gauges |
| `ui_buttons` | 35 | Button prompts (A/B/LB/RB) in tutorials + menus |
| `ui_symbols` | 4 | Gender/alchemy symbols in character creation |
| `font_cp437` | 253 | Monospace UI columns |
| `panels_colour` | 64 | Menu/dialog backing panels |

**Coverage discipline:** a script (`authoring/coverage.py`, to build) walks the game's
content tables and reports any sheet cell not referenced by any table. Target: **0 unused**.
That check is how "use every sprite" stops being an aspiration.

---

## 5. Turn engine

Angband/Moria energy model — the thing that makes speed a real stat.

```c
/* every actor accumulates energy; at >= 100 it acts and pays the cost */
actor->energy += speed_table[actor->speed];   /* speed 110 = "normal" = +10/tick */
while (actor->energy >= 100) { take_turn(actor); actor->energy -= 100; }
```

- Normal speed = +10/tick. `+10 speed` = double actions. Haste/slow are dramatic and
  legible, exactly as in Moria.
- The player acts, then all monsters with energy act, then the world ticks
  (regeneration, torch burn, hunger, status timers).
- **Diagonal movement** is free and normal — 8-way, D-pad + a modifier for the diagonals
  (hold **LB** to convert the D-pad to diagonals), because there is no numpad.

### Controls

| | Overworld / dungeon | Menus |
|---|---|---|
| D-pad | move / attack by bumping | navigate |
| **A** | confirm, pick up, descend, talk | select |
| **B** | cancel / rest one turn | back |
| **LB** (hold) | D-pad becomes diagonals | page up |
| **RB** | quick-cast last spell | page down |
| **MENU** | the command wheel | close |

The **command wheel** (MENU) is a radial of 8 icons drawn from `ui_icons_tiny` —
inventory, cast, quaff, read, wield, drop, look, character. Radial beats a text menu at
this resolution and it reads instantly.

---

## 6. Combat and items — Moria depth

### To-hit and damage

Straight Moria, because it is proven and it makes the +X's matter:

```
to_hit  = base_class_skill + 3*(weapon.to_hit + ring.to_hit) + level_bonus
damage  = roll(weapon.dice) + weapon.to_dam + ring.to_dam + slay_multiplier
```

### Item generation

Every item rolls through a Moria-style pipeline:

1. **Base item** from a depth-weighted table (a Long Sword 2d5 is common at DL 5, a
   Blade of Chaos 6d5 is not).
2. **Enchantment** — `+to_hit` / `+to_dam` / `+AC`, magnitude scaling with depth, with
   a "great" roll chance that scales too.
3. **Ego type** — ~40 of them, drawn from `weapons_elemental` and `trinkets` art:
   *of Slay Dragon*, *of Extra Attacks*, *of Westernesse*, *Holy Avenger*, *of Venom*,
   *of the Magi*, *of Speed*, and cursed counterparts (*of Morgul*, *of Sickliness*).
4. **Artifacts** — ~20 named uniques, each a fixed sprite + fixed powers + a flavour line.
   One per boss, a few in vaults.

### Unidentified items

Moria's flavour system, and it is why the trinket/potion sheets are large: potions are
"a bubbling red potion", rings are "a granite ring", until identified by use, scroll,
or a sage in town. Flavour→item assignment is **shuffled per save seed**, so knowledge
is per-character.

### Curses and perks

Perks live on `trinkets` (55 sprites is a lot of rings and amulets):
`+speed`, `+telepathy`, `see invisible`, `free action`, `resist fire/cold/acid/elec`,
`sustain STR`, `regeneration`, `aggravate monster` (bad), `teleport` (chaotic).

---

## 7. Spells and effects — the "beautiful" requirement

Six realms, mapped to classes. Effects are built from primitives the ABI already has,
so they are cheap and they animate:

| Effect shape | Rendered with |
|---|---|
| **Bolt** (magic missile, frost bolt) | a sprite from `crowns_fx` walked along a Bresenham line over ~6 frames, with `draw_line` trailing in a fading colour |
| **Beam** (lightning, disintegration) | `draw_line` at 2–3 px jitter per frame, colour-cycled, plus endpoint flash |
| **Ball** (fireball, stinking cloud) | expanding `draw_circle` rings + a burst of `crowns_fx` sprites on a Poisson disc |
| **Breath** (dragon fire) | a cone of particles, density falling with distance |
| **Buff / summon** | a ring that contracts onto the target + `ui_status_emotes` icon pinned to the actor |
| **Screen shake** | camera offset on heavy impacts, plus `rumble()` |

Spell *readability* at 128×128 matters more than spectacle: every effect must show
**origin → path → impact** in under 400 ms, because you are about to take a turn based
on what you just saw.

Casting costs SP, can fail on a spell-skill roll (Moria's fail%), and a failed cast
still costs the turn — which is what makes INT/WIS a real decision.

---

## 8. Classes

12 classes, one `characters` sprite each, each with a distinct opening problem:

| Class | Hook |
|---|---|
| Warrior | No spells. Best HP/melee. Sees item plusses without ID. |
| Mage | Full arcane realm. Paper-thin. |
| Priest | Divine realm, heals, undead-turning. Blunt weapons only. |
| Rogue | Backstab, stealth, trap mastery, steals from shops (risky). |
| Ranger | Bows + a `animals`-sprite companion that levels with you. |
| Paladin | Melee + late divine. Immune to fear. |
| Alchemist | Brews potions and **bombs** from gathered reagents. |
| Necromancer | Raises the corpses of what you kill. Divine realm hates you. |
| Bard | Songs = auras that persist while sustained (costs SP/turn). |
| Monk | Unarmed scaling, no-armour bonus, fastest speed growth. |
| Druid | Shapeshift into `animals` sprites; different form = different stat block. |
| Warlock | Pact: power now, an escalating debt that manifests as a hunting unique. |

---

## 9. Bosses

17 boss sprites (2×2), one anchoring each depth band, each with a scripted mechanic
rather than just a big stat block — a 2×2 sprite on a 16×16 screen is already
imposing, so the fight has to use the room:

- **Medusa** (DL 12) — line-of-sight gaze petrifies; fight her using pillars for cover.
- **Giant Green Insect** (DL 15) — splits into two half-HP copies when damaged past 50%.
- **Fire Elemental** (DL 22) — ignites floor tiles; the arena becomes the hazard.
- **The Reaper** (DL 30) — cannot be killed, only outrun, until you find its phylactery.
- **Giant Skull** (DL 40) — summons the entire catacomb tier's undead.

Multi-tile boss rendering: 2×2 sprites blit as four 8×8 draws with a shared origin; the
`bosses` sheet is already grouped and cell-mapped by the annotator work.

---

## 10. Shops (Moria style)

Six shops + temple in the starting town, each an interior built from `furniture_white`,
`furniture_stone`, `panels_colour`, `cobble_floors`:

General Store · Armoury · Weaponsmith · Alchemist · Magic Shop · Black Market · Temple

Each has a stock table, a restock timer measured in game turns, a haggle-free fixed
price with a CHA modifier, and a **buy-back** list. The Black Market sells out-of-depth
items at 5× — Moria's classic money sink and the reason gold matters late.

---

## 11. Procedural generation

**Overworld** — one continent per save seed:
1. Value-noise heightmap → sea / beach / plain / forest / hill / mountain.
2. Poisson-disc placement of 1 town, 3–5 villages, 6–10 dungeon mouths, shrines.
3. A* road network between settlements, carved as `cobble_floors`.
4. Rivers descend the height gradient to the sea; `terrain_edges` handles the coast.

**Dungeon** — Moria's generator, which is the right one for this:
1. Rooms placed on a grid with rejection sampling; corridors L-tunnel between them.
2. Room types: plain, pillared, moated, inner-vault, checkerboard, lit/unlit.
3. **Vaults** at depth — hand-authored templates in the `.level` format, stuffed with
   out-of-depth monsters and loot.
4. Stairs up/down; `MENU→descend` on a down-stair. Levels are **not persistent** (Moria
   rule) — going back up regenerates, which is what makes the descent feel one-way.

---

## 12. Save

`kv_set/kv_get` (ABI v38) with a versioned blob: player, inventory, world seed, current
depth, flavour shuffle, town stock, kill counts, artifact-found flags. Autosave on level
change. **Permadeath** with a tombstone screen and a persistent high-score list, because
the whole loop is built on it.

---

## 13. Build order

Each phase ends with something runnable on the device.

| Phase | Deliverable |
|---|---|
| **1. Skeleton** | Turn engine, energy, player on a static dungeon level, HUD, camera. |
| **2. Dungeon** | Moria room/corridor generator, stairs, depth, FOV + light radius. |
| **3. Combat** | Monsters (animals + monsters sheets), AI, melee, death, XP/levels. |
| **4. Items** | Base tables, enchantment, ego types, inventory, flavours, ID. |
| **5. Magic** | Realms, SP, fail%, the six effect shapes. |
| **6. Town** | Shops, buy/sell/restock, temple. |
| **7. Overworld** | Continent gen, biomes, travel, dungeon mouths, encounters. |
| **8. Bosses** | 2×2 rendering, 17 scripted fights, artifacts. |
| **9. Coverage** | `coverage.py` to 0 unreferenced sprites; fill gaps with content. |
| **10. Polish** | Audio, screen shake, tombstone, high scores, balance. |

---

## 14. Open dependencies

- **Sprite labels are 50/1494 human-verified.** The content tables in §4 are provisional
  and assume the agent labels are broadly right. The annotator pass
  (`authoring/gen_annotator.py`) should land before Phase 3, when monster identity starts
  to matter. 393 tiles are flagged low-confidence and should be reviewed first.
- **RAM budget is unproven for this design.** Moria fits its whole core in the arena, but
  it does not also hold an overworld. The overworld should be **regenerated from seed on
  entry**, not held resident alongside a dungeon level.
- **`items_scroll_bomb`, `props_light` train track and `furniture_stone` levers** are
  confirmed by your corrections and are load-bearing in this design (bombs for the
  Alchemist, rails for the Vault, levers for puzzles).
