# ThumbPrince — a room-drafting roguelike for the Thumby Color (Mote engine)

The resource-management + tile-placement heart of *Blue Prince*, rebuilt as a
top-down pixel-art arcade roguelike. Draft rooms onto an estate grid, walk them
with a little character, manage steps/keys/gems/gold, and race the Antechamber
before you run out of steps — with arcade scoring all the way.

## The run ("one day")

- **Estate grid: 5 × 8 cells.** Entrance Hall pre-placed bottom-centre (2,7).
  The **Antechamber** is pre-marked top-centre (2,0) behind a **triple-locked
  gold door** (3 keys at once, or the Master Key). Entering it WINS the day.
- **Steps = the clock.** You start with 50. Every doorway you walk through
  costs 1 step. 0 steps → the day ends where you stand (results screen).
  Food and rest rooms restore steps.
- Run ends → score tally → new day, fresh empty grid, fresh resources.
  High score + stats persist (`save` slot).

## Drafting (the Blue Prince bit)

Walk into a doorway that leads to an **undrafted grid cell** and you draft:
**3 room cards** are dealt, LEFT/RIGHT to browse, A to place, B to redraw the
whole offer (costs 1 gem). The chosen room is stamped into that cell and you
step inside (1 step).

- **Shapes are entry-relative** (no rotation UI): every room's doors are
  defined relative to the door you entered by — DEAD END / STRAIGHT / CORNER-L /
  CORNER-R / T (entry+left+right) / CROSS. So an offered room always connects.
- Doors that would face off-grid become **sealed windows**; a door into an
  existing neighbour only opens if that neighbour also has a door on the shared
  edge (dead doors are drawn bricked-up).
- Cards show name, rarity colour, gem cost, door diagram, and effect line.
  **Gem-cost rooms are unpickable if you can't pay** (greyed). Every offer is
  guaranteed at least one free room.
- Unique rooms (shops, Observatory, Great Hall, Vault…) appear at most once
  per day; commons can repeat.
- **Locked doors:** each undrafted doorway may be locked (chance grows toward
  the top of the estate). A key opens it permanently; no key → it stays shut
  (find another route or come back). The Master Key (Locksmith, 30 gold —
  or rare Vault find) opens everything for the rest of the day.

## Resources

| Resource | Start | Get | Spend |
|---|---|---|---|
| Steps | 50 | Kitchen/Pantry/Bedroom, food from shop | 1 per doorway |
| Keys 🗝 | 1 | Closet/Security/loot, shops | locked doors, 3 for Antechamber |
| Gems 💎 | 2 | Terrace/Conservatory/loot | gem-cost rooms, 1 = redraw offer |
| Gold 🪙 | 0 | coins scattered in rooms, Vault | shops (keys, gems, food, Master Key) |

## Rooms walk like a Zelda screen

Each cell is a hand-authored **8×7 tile interior** (16 px tiles, HUD strip on
top). Pickups (coins/keys/gems/food/score stars) sit in the room from a
per-room loot table; walk over to collect. Shops (Commissary, Locksmith) have a
counter — stand at it, press A, buy from a menu. RB toggles the **estate map**
(room colours, door pips, locks, you-marker, Antechamber star).

## Arcade scoring

- Draft a room: **+10 × (rarity+1)**  (common/uncommon/rare = 10/20/30)
- Coin +5 · score star +25 (rare rooms hide big stars, +75/+100)
- **Room swept** (all loot collected): +20
- **Rank complete** (all 5 cells of a grid row drafted): +100, fanfare
- **Estate bonus effects**: green rooms (Terrace/Conservatory/Garden) score
  +25 per adjacent green room when placed
- **Win** (enter Antechamber): +500, then remaining steps ×10, keys ×25,
  gems ×15, gold ×2 all convert to points
- High score + best-day + wins/days persisted.

## Room set (v1)

| Room | Shape | Rarity | Gems | Effect |
|---|---|---|---|---|
| Entrance Hall | CROSS | — | — | start room |
| Hallway | STRAIGHT | c | 0 | plain corridor |
| West Passage | CORNER-L | c | 0 | corridor |
| East Passage | CORNER-R | c | 0 | corridor |
| Dining Room | STRAIGHT | c | 0 | +4 steps, a coin |
| Kitchen | CORNER-R | c | 0 | +10 steps |
| Bedroom | DEAD | c | 0 | +6 steps |
| Closet | DEAD | c | 0 | +1 key |
| Pantry | DEAD | c | 0 | +8 steps |
| Terrace | CORNER-L | c | 0 | green · +1 gem |
| Foyer | T | u | 1 | 3-door junction |
| Study | CORNER-L | u | 0 | +50 pts, +1 gem |
| Chapel | STRAIGHT | u | 0 | +75 pt star |
| Storeroom | DEAD | u | 1 | coins + star |
| Den of Gems | DEAD | u | 1 | +3 gems |
| Security | CORNER-R | u | 1 | +2 keys |
| Commissary | STRAIGHT | u | 0 | SHOP: key 8g · gem 5g · food 6g |
| Locksmith | DEAD | u | 0 | SHOP: key 5g · Master Key 30g |
| Conservatory | T | u | 1 | green · +2 gems · +25/adj green |
| Ballroom | T | r | 1 | +60 pts, coins |
| Master Suite | DEAD | r | 1 | +12 steps, +1 key |
| Vault | DEAD | r | 2 | 10 coins + star (door always locked) |
| Observatory | DEAD | r | 2 | +100 pt star |
| Great Hall | CROSS | r | 2 | +50 pts, 4-door hub |

## Architecture

- Pure 2D: `scene2d_begin(0,-16)` + sprites for tiles/props/items/player
  (`max_sprites 160`), HUD/draft/map/menus in `overlay()` with `ui_font`.
- Room interiors are ASCII templates in `src/rooms.h` (wolfmote-style text
  maps); art in editable PNGs under `assets/` (PIL generators, TerraMote
  style), SFX as `.sfx` recipes, all baked by `mote bake`.
- Estate state: 40 cells × (room id, entry dir, lock bits, loot-collected
  bits) — trivially small; no arena use beyond sprite pool.
- Dev hooks (host only): `DRAFT_SEED`, `DRAFT_SKIP` (skip title),
  `DRAFT_GIVE=keys:gems:gold:steps`, `DRAFT_ROOMS=id,id,id` (force offer).

## States

TITLE → PLAY ⇄ (DRAFT | MAP | SHOP | PAUSE) → RESULTS(win/exhausted) → TITLE
