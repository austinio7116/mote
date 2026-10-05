# Grand Thumb Auto III

**Version 1.0.2.** A third-person open-city crime sandbox for the Thumby Color,
built on the Mote engine. The third-person fork of `grandthumbauto`: the camera sits behind the
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
| Get out (landed, and not on a roof) | `RB` |
| **In the boat** | |
| Steer | `DPAD` |
| Ahead | `A` |
| Astern | `B` |
| Get out (alongside only) | `RB` |
| **In a car** | |
| Steer | `DPAD` |
| Gas | `A` |
| Brake / reverse | `B` |
| Fire | `LB` |
| Get out | `RB` |
| **Anywhere** | |
| Map / settings | `MENU` |
| **Title screen** | |
| Pick an option / take it | `UP`,`DOWN` / `A` |
| Settings: pick a row / change its value | `UP`,`DOWN` / `LEFT`,`RIGHT` |

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
- **Vehicle play** — RB to get in or out, A for the throttle, B to brake and drop into reverse once stopped, LB to fire from the car. The bottom-right corner names what you are driving over the speed readout — the handling class, so a cab says TAXI and a cruiser says POLICE — and the helicopter shows altitude in place of speed.
- **Handbrake drifts** — braking while steering cuts lateral grip instead of stopping you, so you can slide a corner and pick the car back up on the exit. The rear tyres smoke while they are actually scrubbing, one puff per wheel.
- **54 vehicles on 16 silhouettes** — sedan, compact, coupe, sports, racer, long-hood, wagon, van, truck, pickup, jeep, classic sports, plus a bus, a tank, a boat and a helicopter that are vehicle types rather than handling classes. No silhouette carries more than 17% of the 54 types. Each is a tinted mesh with a cabin, a wheel line and an oriented ground shadow; taxis are yellow with a roof sign.
- **Car damage** — vehicles take damage, catch fire and wreck, ejecting the driver; a damage bar sits under the health bar while you drive.
- **Armour** — found in the weapon caches. It soaks damage before health does and never regenerates, so it is a consumable advantage rather than a second health bar. A blue strip appears over the health bar only while you are wearing some.
- **Traffic AI** — cars hold a right-hand lane with pure-pursuit steering, change lanes, take turns, yield at junctions, queue behind each other and wait at red lights.
- **Traffic lights** — one signal head per intersection on a kerbside post, red/amber/green off a single global clock with no per-junction state.
- **Pedestrians** — 34 live peds who walk the pavements, flee a fight, and occasionally fight back.
- **The city keeps hours** — traffic and pedestrian counts follow the clock. The working day runs at full density and only the small hours thin out: measured at one seed, 10 cars and 22 people about at midday against 6 and 12 at 1 a.m. Costs nothing — it scales targets the streamer already had — and the thinner night traffic also lowers the triangle peak.
- **Street crews** — one city in three has a knot of four matching pedestrians who come for you on sight rather than when provoked. Put all four down and the last one drops the crew's takings.
- **Police and a wanted level** — six stars, and all six are reachable. A star is a band — `wanted()` is `(int)heat` — so the top one needs headroom to 7, not to 6; at a 6.0 ceiling it was a single exact value the first frame of decay left, which is why `wanted()` clamped to 5 and the HUD drew five dots. Six was unreachable in the logic as well as undrawn. Ambient patrols are the witnesses: a crime nobody sees costs you nothing, and gunfire and explosions are heard through walls where a theft has to be watched. Ramming a squad car is an automatic star; stealing one is two on the spot; **killing an officer adds two stars outright**, and unlike every other killing it needs no witness — a man with a radio stops answering it. **Stay in sight of the police and the level climbs on its own** — every twelve seconds of unbroken pursuit adds a star, up to four, so a chase you cannot shake gets worse rather than stalling. Escaping means both 30 m of separation and no line of sight, held for long enough; ducking behind one building only slows the count down. Squad cars chase by descending a routed distance field over the street grid rather than steering at you in a straight line, so they corner instead of grinding along the wall between you. At three stars they arrive in pairs, at four they throw roadblocks across the road ahead, and at five the army sends a tank.
- **Taxi fares** — drive a cab and people start flagging you down. Pull up to whoever waved, they get in, the beacon moves to where they want to go, and stopping there pays you. Fares chain: each one delivered raises the next one's pay by 15% up to 1.9x, and losing one — a timeout, getting out mid-ride, or two stars putting sirens behind you — resets the streak. Pay is deliberately a fraction of a phone job's per metre, because fares are repeatable for as long as you keep the cab: 110-250 a fare before the streak, so a shift buys a shotgun rather than a minigun in a minute. Nobody hails a cab you are being chased in. The job runs only while no phone job is active, so the two never fight over the beacon or the HUD row.
- **Traffic that sees you** — step into the road and drivers brake and stop, brake lights on, rather than driving through you. The lane test traffic already used to queue behind other cars now also looks for the player on foot, in a narrower corridor and with a longer look, since you lift off earlier for a person than for a bumper. **One driver in five never brakes** — derived from the car slot and its paint, so a given car behaves consistently and rerolls when the slot is recycled. Measured by standing in a live lane for 50 s across six seeds: times hit fell from 8 to 4 and total health lost from 108 to 92. Two of the six have no traffic on the chosen tile and score 0 either way; one (seed 99) came out worse with yielding on, which is what a traffic sim that diverges the moment one car stops will do. The mechanism is the thing that is certain — cars are measurably stopped and waiting — not the size of the effect on any one seed.
- **14 mission types** — courier, rampage, getaway, hit, deliver, pickup, repo, escort, smuggle, demolition, vigilante, wanted-survival, circuit time-trial and a rubber-band rival race.
- **Day/night cycle** — a gradient sky on a full clock, a sun and moon on their own arc, stars that fade up through dusk, and a daylight cloud deck. After dark the city lights up: building facades swap to a night palette that drops the walls and lights a scattered share of the windows, costing no RAM and no extra triangles because the atlases are palette-indexed and the night image reuses the same pixels. The clouds sit at one altitude the way real cumulus do, all their flat bottoms on a single plane, so the only thing that varies is how far away a cloud is — perspective then makes the distant ones smaller and lower in the sky at the same time, and hazier, without any of it being faked per cloud.
- **Weather** — rain and thunderstorms that come and go, with wet road tinting.
- **A map that agrees with the world** — the 3D view puts +x on the player's left, so the full map is mirrored on the way out to match it. Minimap, map page and what you can see out of the windscreen all turn the same way.
- **Rotating minimap** — a live radar under the title bar, hideable from the settings page. It turns with your heading, so it matches what is out of the windscreen; an arrow travels round the rim pointing north, because heading south the dial is the map turned 180 and on a grid city that reads as a mirror.
- **A clock** — the time of day, right-aligned in the title bar beside the wanted stars, as `7 AM` / `12 PM`. `g_tod` is `[0,1)` with 0 = midnight, so the hour is `tod*24`. The hour only: a full day is 240 real seconds, which makes one game minute a sixth of a second, so a minutes field changes six times a second and reads as a broken digit rather than as a clock. The hour changes every ten seconds, and it is the part that tells you whether the lights are about to come on.
- **Save and load** — three save slots on the settings tab of the pause screen, picked with LEFT/RIGHT and marked when occupied, plus a persisted best-cash record and a persisted sound on/off setting. **NEW GAME** is on the same page, so you can start a fresh city without quitting; it throws away the one you are standing in, so it asks twice — the first A arms it and the row reads `A AGAIN`, and moving off the row, closing the menu or three seconds of silence all disarm it. A save records the **city seed**, so loading rebuilds the map you saved in rather than dropping you at those coordinates in an unrelated one. The title screen's menu offers `CONTINUE <slot>` when the selected slot holds a game, so you need not start a new city just to reach the menu. Dynamic state — traffic, pedestrians, uncollected caches, a mission in progress — is not saved and repopulates.
- **A flyable helicopter** — parked on an open pad somewhere in the city, with a 45 m ceiling, flight over water and buildings, an altitude readout in place of the speedo, and police who can only shoot at you below 18 m. It hovers rather than ditches over water, because there is only one of them. **You can set it down on any roof**, and the skids settle on the building properly — but you cannot step out up there, because nothing on foot in this game has a height. Rooftop caches are meant to be taken from the cockpit anyway: they have a 4 m collection radius against 1.5 m on the ground, so hovering level with the roof picks them up.
- **A boat** — moored at a shoreline in one city in two, rarer than the helicopter. It runs on water and nowhere else: drive it aground and the engine gives you nothing, but astern still works at reduced power to back you off the beach. RB will not put you out into open water. Low grip, so a hull carries its momentum through a turn, with a wake that widens with speed.
- **Hideaways** — two unmarked bolt-holes per city, cut into a building the way the pay-n-sprays are, but open only to someone **on foot**: the bay stays off the drivable map and keeps its collider, so a car bounces off the mouth and you have to get out and walk in. Each holds a cache — armour plus a weapon, cash or a medkit. Nothing marks one until you have stood in it; after that it is a violet dot on the map page, because a safehouse is worth being able to run back to. Standing in one **bleeds the wanted level off at 0.3 stars a second** for free, against the pay-n-spray's instant clear for $100: one star takes 3 seconds, three stars 10. It holds against the street and not against a man in the doorway — any officer within 11 m of the mouth blows it and the cooling stops until you shake them off. At six stars that buys about 6 seconds and a star and a half before they arrive. The placement rule is stricter than a spray bay's: back wall and *both* sides must still be solid, so it is a slot cut into the mass rather than an open corner, and it is kept well clear of downtown and of every shop and phone box.
- **Hidden content** — a treasure island in the bottom-left water reached by a footbridge, weapon and cash caches in the forests, five more on the roofs of tall buildings that only the helicopter can reach, a rare one-shot rocket launcher, and a drivable tank with finite shells.
- **Shop signs after dark** — the gun shop, spray garage, phone boxes and dock light up at night, each in the colour the map page uses for it, so what you saw on the map is what is lit in front of you. Glow discs, no RAM; the budget has room precisely at night because the clouds stop drawing.
- **Street detail** — street lamps along the kerbs whose heads light up at dusk, bus stops with a sign facing the road, park benches on plazas and pavements, shrubs in the parks, railings on the long bridges, palm trees on the waterline, puddles on the road while it rains, zebra crossings and phone boxes.
- **Parking lots** — open pavement pockets away from the kerb are surfaced as asphalt with painted bays, about fifty-five to a city and averaging ten bays each. They cost nothing to draw: the ground pass emits the same triangles either way, just with a different cell of a sheet it has already bound.
- **City ambience** — a siren somewhere else in the city every twenty seconds or so, quiet enough to read as distance, and only when nothing is chasing you.
- **Birds** — flocks circle over the parks and the water by day and vanish when you get close, which reads as taking flight. Stateless: the flock comes from the tile hash and the circling from the clock, drawn as scene points (two pixels, depth-tested) with twelve of the 56-point pool reserved for them.
- **Beaches** — a quarter of the waterfront is sand two tiles deep, wet on the waterline and dry behind it, with the seawall dropped where it meets the water.
- **Draw budget** — cone culling, a banded draw distance with a haze band and ground skirt, and every pool (triangles, billboards, discs, shadows) sized from a measured host profile. That profile never covered combat, and a three-star pursuit now fills the triangle list — see [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md).

![The helicopter at its 45 m ceiling, rotor spinning, the city and a rooftop below](../../docs/img/grandthumbauto3-heli.png)

![A beach: palm trees on dry sand, wet sand on the waterline, and water with no seawall where it meets the sand](../../docs/img/grandthumbauto3-beach.png)

## Temporary debug affordances

**BRING / GO TO** — a hidden row on the SETTINGS page. Open the menu, `RB` to
the settings tab, then **tap `B` nine times**: one more row appears at the
bottom. `LEFT`/`RIGHT` walks five entries and `A` does the one showing. Nine
more taps puts the row away again.

Three bring a vehicle to you: HELI and TANK arrive 9 m in front; BOAT has to
arrive on water, so it goes to the nearest mooring and the message says so.

Two take you somewhere, and the row's label changes to `GO TO` for them. **DEN**
puts you on foot at the mouth of the nearest hideaway, facing in; **ISLE** puts
you on the treasure island. These two exist because they are the only places in
the game you cannot reach on purpose — a den is unmarked until you have stood
in one, and the island is a footbridge walk across the map — so checking either
by hand meant a scripted host run with a teleport hook that the device build
does not have. Both step you out of whatever you were driving, because neither
is somewhere a vehicle can follow you, and both close the pause screen so you
can see where you landed. If the city has no den or no island, the row says so
and nothing moves.

One row rather than two because an eighth settings row does not fit: rows are
11 px apart from y=19, so the eighth lands at y=96 and its highlight runs to
y=106, over the result line at y=99.

`B` is the button because the settings tab is the one screen where it does
nothing — `UP`/`DOWN` pick a row, `LEFT`/`RIGHT` set its value, `A` activates
it, `LB`/`RB` flip tabs, `MENU` closes — so the code cannot disturb anything on
its way in.

This replaced an in-play code (four `LB`s then a direction, entered on foot).
That one passed its scripted capture but was no good in the hand: it wanted
four taps inside a 1.5 s window while `LB` was also swinging the look-back
camera, and nothing on screen told you whether a tap had registered. Here the
row either appears or it does not.

This is meant to come out. To remove it, delete `bring_vehicle`, the
`goto_place`, the `BRING_*`/`BRING_N` enum and `BRING_VEH_N`, `g_bring`, `g_cheats`,
`set_rows()` and its two call sites, the `SET_BRING` row in the settings enum,
the B-tap block in the settings handler, its `LEFT`/`RIGHT` and `A` arms, its
`NAME[]` entry, its `GO TO` label special-case and the `BN[]` value arm in
`draw_settings`.

The host-only equivalents are environment variables, which are no use with the
handheld in your hands: `MOTE_GTA_TP_HELI=1` stands beside the aircraft, `2`
puts you in it already airborne, `3` also parks it over the nearest tall roof,
`4` over the nearest water, and `5` over the nearest rooftop cache. `MOTE_GTA_TP_TANK=1` stands beside the tank.
`MOTE_GTA_BOAT=1` forces the boat to exist (one city in two), `MOTE_GTA_TP_BOAT=1`
stands you beside it and `=2` puts you aboard. `MOTE_GTA_CARTYPE=<n>` forces the
type of the first eight traffic cars so a given silhouette can be got on camera.
`MOTE_GTA_NOWIN=1` holds the day building palettes after dark, so the lit windows
can be A/B'd within one scene. `MOTE_GTA_NOTOD=1` pins the traffic density to full regardless of the hour, which
is how the clock's effect on it was measured. `MOTE_GTA_ARMOUR=1` starts play in a full vest and
`=10` in ten points of one, since the caches that carry armour are scattered over
the whole map and reaching one in a scripted capture is not practical.
`MOTE_GTA_INCAR=1 MOTE_GTA_CARTYPE=30` starts you at the wheel of a cab, which
is the only way to see the fare loop in a scripted run: fares are offered only
while you are driving a taxi, so no amount of walking around reaches them.

## Build and test

```bash
./tools/mote build games/grandthumbauto3            # host .so
./tools/mote build games/grandthumbauto3 --device   # .mote for the device
cmake --build build_host --target gta3_test_veh gta3_test_view gta3_test_camera
```

`gta3_shots.sh` lives in this folder and takes headless gameplay captures to
`games/grandthumbauto3/gta3_shots/` (git-ignored). Run it from anywhere; it
finds the repo root from its own location.

Three host test binaries cover the parts that are testable without a frame:
`gta3_test_veh` (vehicle mesh construction, silhouettes, lamp styles),
`gta3_test_view` (the world-space visibility cone) and `gta3_test_camera`
(chase-camera framing, angle wrapping, wall collision).

The game is close to two budget ceilings, and both are checked on every change:

- **`GAME_RAM`** — 134 KB at `0x2005E800`, with **968 bytes free**. Check with
  `arm-none-eabi-nm` on the `.elf`: `__mote_bss_end` against the top of the region.
- **Triangles** — `max_tris` is 850, and a three-star pursuit now **reaches it**.
  (The game runs on a Thumby Color; what is unmeasured is the engine arena's
  spare capacity, which the device build cannot report — see KNOWN_ISSUES.)
  `MOTE_GTA_DEBUG=1` prints `[TRI] peak=` to stderr and shows `t<n>` in the HUD;
  seed 7 at `MOTE_GTA_HEAT=3` peaks at exactly 850, which means the list filled
  and further triangles were dropped. See [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md).
  See [`PROFILING.md`](PROFILING.md) for the measured scenes behind every pool size —
  note that combat was never among them.

  Open defects, with reproductions and what has already been ruled out, are in
  [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md).

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
