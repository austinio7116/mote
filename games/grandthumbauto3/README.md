# Grand Thumb Auto III

A third-person open-city crime sandbox for the Thumby Color, built on the Mote
engine. The third-person fork of `grandthumbauto`: the camera sits behind the
player rather than overhead, so the city is drawn as 3D massing instead of a
top-down map.

![Grand Thumb Auto III — on foot at a junction: a kerbside traffic light on green, clouds over the water, the rotating minimap and the wanted stars in the HUD](../../docs/img/grandthumbauto3.png)

## Play

```bash
./tools/mote run games/grandthumbauto3            # SDL emulator
./tools/mote push games/grandthumbauto3 --launch  # USB -> device
```

**Controls** (as the in-game CONTROLS page lists them, under SETTINGS):

| Action | Button |
|---|---|
| **On foot** | |
| Move / turn | `DPAD` |
| Run (hold) | `A` |
| Attack | `B` |
| Enter car / switch weapon | `RB` |
| Look back | `LB` |
| **In the helicopter** | |
| Pitch / yaw | `DPAD` |
| Climb | `A` |
| Descend | `B` |
| Get out (landed only) | `RB` |
| **In a car** | |
| Steer | `DPAD` |
| Gas | `A` |
| Brake / reverse | `B` |
| Fire | `LB` |
| Get out | `RB` |
| **Anywhere** | |
| Map / settings | `MENU` |

`B` is the brake first and reverse second: held from speed it only ever brakes,
and it drops into reverse on its own once the car has actually come to rest, no
release needed. Held **while steering** it is a handbrake — the pedal keeps
under half its stopping power and lateral grip falls, so the tail steps out and
you can slide a corner. Keep the throttle on through it and the car gathers
itself up again when you let go of `B`.

## Features

- **Procedural city** — a 254x256 tile map generated at boot: road grid, avenues, pavements, parks, rivers, lakes, bridges and a plaza, different every seed.
- **Third-person chase camera** — smoothed yaw, wall collision, no flip when reversing, and an orbit for the title and death screens.
- **On-foot play** — walk, sprint on A with stamina, punch, eight weapons plus fists, and LB to look behind you.
- **Vehicle play** — RB to get in or out, A for the throttle, B to brake and drop into reverse once stopped, LB to fire from the car.
- **Handbrake drifts** — braking while steering cuts lateral grip instead of stopping you, so you can slide a corner and pick the car back up on the exit.
- **54 vehicles on 16 silhouettes** — sedan, compact, coupe, sports, racer, long-hood, wagon, van, truck, pickup, jeep, classic sports and taxi, plus a bus, a tank and a helicopter that are vehicle types rather than handling classes. No silhouette carries more than 17% of the 54 types. Each is a tinted mesh with a cabin, a wheel line and an oriented ground shadow; taxis are yellow with a roof sign.
- **Car damage** — vehicles take damage, catch fire and wreck, ejecting the driver; a damage bar sits under the health bar while you drive.
- **Traffic AI** — cars hold a right-hand lane with pure-pursuit steering, change lanes, take turns, yield at junctions, queue behind each other and wait at red lights.
- **Traffic lights** — one signal head per intersection on a kerbside post, red/amber/green off a single global clock with no per-junction state.
- **Pedestrians** — 34 live peds who walk the pavements, flee a fight, and occasionally fight back.
- **Street crews** — one city in three has a knot of four matching pedestrians who come for you on sight rather than when provoked. Put all four down and the last one drops the crew's takings.
- **Police and a wanted level** — up to six stars, ambient patrols that act as witnesses, squad cars and foot officers that pursue you, sirens, and an automatic felony for ramming a cop car.
- **14 mission types** — courier, rampage, getaway, hit, deliver, pickup, repo, escort, smuggle, demolition, vigilante, wanted-survival, circuit time-trial and a rubber-band rival race.
- **Day/night cycle** — a gradient sky on a full clock, a sun and moon on their own arc, stars that fade up through dusk, and daylight clouds.
- **Weather** — rain and thunderstorms that come and go, with wet road tinting.
- **Rotating minimap** — a live radar under the title bar, hideable from the settings page.
- **Save and load** — a settings tab on the pause screen, plus a persisted best-cash record.
- **A flyable helicopter** — parked on an open pad somewhere in the city, with a 45 m ceiling, rooftop landings, flight over water and buildings, an altitude readout in place of the speedo, and police who can only shoot at you below 18 m. It hovers rather than ditches over water, because there is only one of them.
- **Hidden content** — a treasure island in the bottom-left water reached by a footbridge, weapon and cash caches in the forests, five more on the roofs of tall buildings that only the helicopter can reach, a rare one-shot rocket launcher, and a drivable tank with finite shells.
- **Street detail** — park benches on plazas and pavements, railings on the long bridges, palm trees on the waterline, zebra crossings and phone boxes.
- **Beaches** — a quarter of the waterfront is sand two tiles deep, wet on the waterline and dry behind it, with the seawall dropped where it meets the water.
- **Draw budget** — cone culling, a banded draw distance with a haze band and ground skirt, and every pool (triangles, billboards, discs, shadows) sized from a measured host profile.

![The helicopter at its 45 m ceiling, rotor spinning, the city and a rooftop below](../../docs/img/grandthumbauto3-heli.png)

![A beach: palm trees on dry sand, wet sand on the waterline, and water with no seawall where it meets the sand](../../docs/img/grandthumbauto3-beach.png)

## Temporary debug affordances

**BRING HELI** and **BRING TANK** — two rows on the SETTINGS page (MENU, then
RB to the settings tab) that put the one-off vehicles 9 m in front of you, so
both can be tested on the device without first finding where they were hidden.
Each steps closer until the tile is somewhere the vehicle can sit, and always
delivers directly ahead rather than at a random bearing. A delivered tank comes
with a full load of shells.

These are meant to come out. To remove them, delete the `SET_HELI` and
`SET_TANK` rows from the settings enum, their entries in `NAME[]`, and their
two `case`s in the settings switch — nothing else refers to them.

The host-only equivalents are environment variables, which are no use with the
handheld in your hands: `MOTE_GTA_TP_HELI=1` stands beside the aircraft, `2`
puts you in it already airborne, `3` also parks it over the nearest tall roof,
`4` over the nearest water, and `5` over the nearest rooftop cache. `MOTE_GTA_TP_TANK=1` stands beside the tank,
and `MOTE_GTA_CARTYPE=<n>` forces the type of the first eight traffic cars so a
given silhouette can be got on camera.

## Build and test

```bash
./tools/mote build games/grandthumbauto3            # host .so
./tools/mote build games/grandthumbauto3 --device   # .mote for the device
cmake --build build_host --target gta3_test_veh gta3_test_view gta3_test_camera
```

Three host test binaries cover the parts that are testable without a frame:
`gta3_test_veh` (vehicle mesh construction, silhouettes, lamp styles),
`gta3_test_view` (the world-space visibility cone) and `gta3_test_camera`
(chase-camera framing, angle wrapping, wall collision).

The game is close to two budget ceilings, and both are checked on every change:

- **`GAME_RAM`** — 134 KB at `0x2005E800`, with about 2.2 KB free. Check with
  `arm-none-eabi-nm` on the `.elf`: `__mote_bss_end` against the top of the region.
- **Triangles** — `max_tris` is 850 and the worst measured scene peaks near 740.
  See [`PROFILING.md`](PROFILING.md) for the measured scenes behind every pool size.

After any device build, disassemble `mote_game_register` and confirm the literal
it returns is the address of `k_vtbl`. A NULL there is the "map failed" bug —
see [`docs/DEVICE_DEBUGGING.md`](../../docs/DEVICE_DEBUGGING.md).

## Assets

`assets/*.png` are the editable sources; `tools/mote bake` turns them into the
`src/*.h` headers the game includes. The `make_*.py` scripts author them.

`make_citytiles.py` has an absolute `ROOT` pointing at another machine and at
`games/grandthumbauto`, not this game — running it here overwrites the wrong
game's committed art. `make_beach.py` derives its `ROOT` from its own location.
`mote bake` also rewrites *every* header in `src/`, so bake a staging directory
holding only the files you changed and copy the results across.
