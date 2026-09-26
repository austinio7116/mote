# Grand Thumb Auto III — known issues

Open defects with a reproduction and the evidence already gathered, so the next
person does not re-derive it. Each entry says what was RULED OUT, which is the
expensive part.

---

## Squad cars plateau ~40 m from the player on some layouts

**Status:** open. Partially mitigated in `7b37bf62`, not fixed.

### Symptom

At three stars the dispatcher keeps three squad cars alive, they close to
roughly 40 m, and then stop closing. They orbit at 40–52 m indefinitely while
the wanted level decays to zero. To the player it looks like the police gave up
one block away.

### Reproduce

```
SDL_VIDEODRIVER=dummy MOTE_DT_MS=33 MOTE_GTA_SEED=42 MOTE_GTA_HEAT=3 \
MOTE_KEYS="a:30-33 a:150-153 down:160-1400" \
MOTE_SHOT=/tmp/n.ppm MOTE_SHOT_FRAME=1400 \
./build_host/mote_host games/grandthumbauto3/build/GrandThumbAutoIII.so
```

Seed 42 is the reliable case. Seeds 1, 7, 99, 123 and 555 do not show it — on
those the cops reach contact and make the arrest.

### What was measured

Instrumenting `update_heat` to print the nearest cop's straight-line distance
and `road_dist()` between that cop and the player, once a second:

```
ROUTE cop=82.3m roaddist=96.0 playercell_hasroad=1 playertile=,
ROUTE cop=48.4m roaddist=64.0 playercell_hasroad=1 playertile=,
ROUTE cop=40.6m roaddist=48.0 playercell_hasroad=1 playertile=,
ROUTE cop=47.6m roaddist=80.0 playercell_hasroad=1 playertile=,
ROUTE cop=41.3m roaddist=64.0 playercell_hasroad=1 playertile=,
   ... 40-48 m for the next forty-five seconds ...
```

A road route exists the entire time and the player is standing on a roaded
cell. Route distance cycles 48 → 64 → 80 → 48: the car is driving back and
forth, gaining and losing ground.

The map around the player at the time of the stall (`P` = player, tile 124,75;
the nearest cop is at 121,86, south and outside this window):

```
|,,,,,,,..,,,,,,,P,..,OOO,,,#####,|
| HOH,,,..,,###HHO,..,OOOHHHHHH,##|
|H H#O,,..,H,##,,O,..,OOOOHHHHO,HH|
```

The player is on a pavement strip. Immediately south is a building block
(`###` at x=119–121). The nearest drivable corridors are the vertical `..`
runs at x=115–116 and x=126–127, three tiles either side. Reaching the player
requires a dogleg the cops never complete.

### What was RULED OUT

- **No route exists.** False — `road_dist()` returns a finite 48–96 m throughout.
- **Player is somewhere unroutable** (park, pier, across water). False —
  `playercell_hasroad=1`, and the tile is `,` (pavement), which `is_road()`
  counts and `drivable_world()` permits.
- **Turn scoring.** The seek used to score cardinals by BEARING to the player,
  which has local minima. It now descends the BFS routed-cost field instead
  (`7b37bf62`). That helped elsewhere — across six seeds, time spent within
  15 m of the player roughly doubled, 4.7% → 8.6% of sampled seconds — but
  **seed 42 is bit-identical at 40 m under both.** Whatever holds cops off here
  is not how the turn is chosen.
- **Per-frame re-decision fighting the car's momentum.** A variant committing
  the chosen cardinal until the cop left the coarse cell it was chosen in
  measured *worse* on every metric (mean nearest 44.0 m vs 41.4 m; 6.8% vs 8.6%
  in range). A cell is 16 m and the tile grid is 4 m, so committed cars sail
  past the junctions they need. Do not retry this without finer granularity.
- **Cops physically wedged.** `copstuck[]` was 0.0 and throttle 1.0 through most
  of the stall; they are driving freely, just not toward the player.

### Suggested next step

Route chasing cops through `update_traffic`'s CRUISE/TURN state machine the way
the rival racer does (`is_racer`, `game.c`), instead of bypassing it with raw
`ai_drive()`. That machine already does junction detection, lane holding and
turn commitment at **tile** granularity, which is the resolution this needs and
the resolution the rejected cell-commit variant lacked. The racer is the
existing proof that a car can be steered to an arbitrary moving target through
that machine.

### Measuring a fix

Minimum cop distance is the WRONG metric — a bust respawns the player and
produces a spurious 0 m that makes every variant look identical. Truncate each
run at the first arrest (wanted goes >0 then back to 0) and report mean nearest
distance plus the fraction of sampled seconds within 15 m. The first attempt at
measuring this scored three genuinely different variants as all-equal before
the contamination was spotted.
