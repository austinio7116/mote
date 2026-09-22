# Grand Thumb Auto III — resource-budget profile (Task 13)

This is the handover for finishing the draw-budget work on the real device. It
was written on a machine with no Thumby Color attached, so everything here is
either (a) measured on the host, which is a faithful proxy for **counts**
(triangles, textured triangles, billboards) but not for **timing**, or (b) a
checklist for someone who has the device.

Read the three measurement facts below before changing anything — getting them
wrong invalidates the whole profile.

## Three facts that shaped this profile

1. **`mote->perf()` does not cover `overlay()`.** `os/mote_os.c:378-382` runs
   the raster, calls `vt->overlay(fb)`, THEN records the frame's timings (the
   ones captured before `overlay()` ran). The HUD, radar and marker arrows are
   invisible to every field `perf()` reports. If you want overlay's own cost,
   time it separately with `mote->micros()`.
2. **`MOTE_DT_MS` pins `mote_plat_micros()`** to a constant within a frame
   (`platform/host/mote_plat_host.c:188`). Any micros-based timing inside a
   fixed-timestep run reads zero. All counting in this file used
   `MOTE_DT_MS=16.6` for determinism (reproducible scenes); no timing numbers
   were taken this way, because there aren't any host timing numbers in this
   file at all — see "What host timing does NOT tell you" below.
3. **The engine degrades silently under pool pressure, in two different ways:**
   - **Textured triangles (`max_tex_tris`):** overflow latches
     `s_textri_starved` and the pipeline falls back to FLAT shading instead of
     dropping the draw. **A starved frame LOOKS LIKE untextured, tinted
     buildings** — not missing buildings. `os/mote_os.c` prints one host-log
     warning the first time this trips
     (`"a textured model has no textured-triangle budget..."`).
   - **Billboards (`max_billboards`):** `engine/render/mote_scene3d.c:417`-ish
     (`mote_scene_add_billboard`) returns 0 and draws **nothing at all** past
     the cap. The game already counts this: `g_bb_used` / `g_bb_budget_drop`,
     logged as `[BB]` lines behind `MOTE_GTA_DEBUG=1`. **A starved frame LOOKS
     LIKE missing trees, missing pedestrians, or a missing phonebox marker** —
     whichever billboard call happened to run out of pool that frame (trees
     are drawn last on purpose, so they drop first).

## What host timing does NOT tell you

A host machine is enormously faster than the 280 MHz Cortex-M33 on the device.
Host frame times say nothing about device fps. **No fps/update_us/raster_us
numbers appear anywhere in this document** — they would be meaningless. What
DOES transfer from host to device is **counts**: triangles, textured
triangles, billboards, shadows, lines. Every number below is a count.

## How the counts were captured

`scene_tri_count()` (exposed via `mote->scene_tri_count`) only reports the
**flat/gouraud** triangle pool (`max_tris`) — it does not include textured
triangles. There is no ABI-exposed getter for the textured-triangle pool
(`max_tex_tris`), so the peak textured-triangle counts below were captured
with a **temporary, uncommitted** one-line instrumentation directly in
`engine/render/mote_scene3d.c`'s `scene_reset_lists()` (prints
`s_ntris`/`s_ntextris`/`s_nbbs`/`s_nshadows`/`s_nlines` against their pool
caps, gated behind a `MOTE_GTA_ENGDBG` env var, guarded `#ifdef MOTE_HOST`),
plus a one-line `s_arena.overflow`/`s_arena.used` print added the same way in
`os/mote_os.c` right after `mote_perf_set_mem(...)`. **Both of these were
reverted before committing** — `git diff` on this branch touches only
`games/grandthumbauto3/src/game.c` and this file. If you need to re-derive
these numbers, the same two one-line hooks reproduce them in about a minute;
they are not required for normal play or for the on-device checklist below.

The billboard counts (`g_bb_used`, `g_bb_budget_drop`) and the on-screen
profiling HUD (`fps`, `update_us`, `raster_us`, `scene_tri_count()`) ARE part
of the committed game code, behind `MOTE_GTA_DEBUG`, permanently — see
"Debug HUD" below.

All scenes were reached by teleport (`MOTE_GTA_VIEW="x,z"`, map tile
coordinates) on a **fixed city seed** (`MOTE_GTA_SEED=<n>` — the game reads
this env var in `reset_game()` and skips the normal micros-based seed, so the
same seed always regenerates the same city and the same teleport coordinates
land in the same place). Runs used `MOTE_DT_MS=16.6` for a deterministic
frame cadence and `MOTE_KEYS="a:2-2 left:5-300"` to accept the title screen
then hold the on-foot turn button for the rest of the run, sweeping the
camera through a full rotation so the reported peak is the worst FACING at
that position, not just whatever the player happened to be looking at.
`MOTE_AUTORUN=1` skips the game-select launcher (`mote_host <game.so>` would
otherwise show a list). All measurements were on foot (`MODE_FOOT`); see
"What was not measured" for why.

## Peak counts, by scenario

Pool caps below reflect this profile's own settings (`max_tris=700`,
`max_tex_tris=950`, `max_billboards=112`) — see "How to reproduce" for the
exact command per row. Every column is a genuine measured peak, not a value
truncated by hitting a smaller cap (verified: `dropped=0` in every run below
except the two facade-jam rows, which are described).

| Scenario | flat tris (`max_tris`) peak | textured tris (`max_tex_tris`) peak | billboards peak | shadows peak |
|---|---|---|---|---|
| Downtown avenue, rotating | 375 | **671** | **92** (drop 0) | 10 |
| Alley | **597**\* | **798** | 11 | 12 |
| Park | 597 | 626 | 6 | 15 |
| Bridge | 355 | 516 | 22 | 11 |
| Spawn facade-jam (static) | 393 | 598 | 48\*\* pinned, drop 4 (pool capped at 48 for this specific check) | 23 |
| Spawn facade-jam (rotating) | 508 | 656 | 65 | 21 |

\* Alley's flat-tri peak (595) is essentially tied with park's (597); either
is the flat-pool worst case depending on rounding — treated as the same
number.

\*\* The facade-jam row is the one place `max_billboards` was deliberately
capped back to 48 for the check, to reproduce the exact regression an
earlier review found (see below) before re-measuring uncapped.

**Worst case per pool:**
- **`max_tris` (flat triangles): park/alley, ~597.**
- **`max_tex_tris` (textured triangles): alley, 798.**
- **`max_billboards`: downtown avenue, 92** (measured with the pool
  temporarily raised to 96, `dropped=0` — this is a genuine, un-truncated
  peak, not a value the 96-cap cut off).

## The facade-jam billboard regression, reproduced

An earlier review found that standing still at spawn with the chase camera
jammed against a facade pins billboard usage at 48/48 for 150+ consecutive
frames, and that this is **not** the open park — it is nearby tiles (behind
or beside the wall the camera is pressed against) passing `bb_add`'s
view-cone test even though the wall physically occludes them, filling the
pool with trees the player can't see.

This is seed-dependent (the city regenerates per seed, and whether spawn ends
up backed against a building is chance). It reproduces reliably at
`MOTE_GTA_SEED=13`, natural spawn (no `MOTE_GTA_VIEW` override — the
teleport env var is for the OTHER four scenarios; this one needs the game's
own spawn placement):

```
MOTE_AUTORUN=1 MOTE_GTA_DEBUG=1 MOTE_GTA_SEED=13 MOTE_DT_MS=16.6 \
  MOTE_KEYS="a:2-2" MOTE_REC=/tmp/rec MOTE_REC_N=160 \
  ./build_host/mote_host games/grandthumbauto3/build/GrandThumbAutoIII.so
```

At `max_billboards=48` this pins 48/48 for 158 of 160 frames, `dropped=4` —
matching the earlier finding almost exactly. At `max_billboards=96` the true
uncapped demand there is 54 (static) / 65 (rotating) — lower than downtown
avenue's 92. **Downtown avenue is the actual worst case for the billboard
pool**, not the facade-jam spot; the facade-jam spot is the worst case for
*wasted* billboard slots (budget spent on occluded trees instead of visible
peds/pickups), which is a real problem worth knowing about even though its
raw peak is lower.

## Pool values set, and the measurement each came from

| Setting | Old (spec guess) | New | From |
|---|---|---|---|
| `max_tris` | 2200 | **700** | park/alley peak 597, +15% ≈ 686, rounded up |
| `max_tex_tris` | 1600 | **950** | alley peak 798, +15% ≈ 918, rounded up |
| `max_billboards` | 48 | **112** | downtown peak 92 (uncapped), +15% ≈ 106, rounded up to a clean number |
| `max_shadows` | 40 | 40 (unchanged) | peak observed 23, well under cap — not touched |
| `max_lines` | 24 | 24 (unchanged) | no bullets were fired in the count-focused runs above; see "regression" section for a separate run that did fire a weapon without crashing, but it did not produce a peak-line-count measurement. Left at spec value. |
| `max_contacts`, `max_bodies`, `depth` | unchanged | unchanged | not in scope for this task |

**`set_fps_limit(30)` was deliberately NOT added.** The brief's Step 6 says
to pin the frame rate once downtown holds 30 fps on-device, and that can't be
established without the device. `k_vtbl.config` and the `VIEW_*` radii
(`VIEW_GROUND_R`, `VIEW_BLD_R`, `VIEW_HAZE_R`, `VIEW_BLD_CAP`) are also
**unchanged from their spec starting values** — tuning those against a frame
target (brief Step 5) likewise needs a real fps reading, which this machine
cannot produce. Both are the first two items on the device checklist below.

## No conflict with the arena ceiling — but only because the pools shrank

The brief's known ceiling: `max_billboards=96` boots cleanly; **128 overflows
the host arena** at the OLD tri-pool sizes (`max_tex_tris=1600`,
`max_tris=2200`). That ceiling does NOT hold at this profile's new,
measured-and-shrunk tri pools (`max_tex_tris=950`, `max_tris=700`): with
those pools, `max_billboards=128` boots cleanly and the arena sits at
**65.0%** used (184,336 / 283,648 bytes on host, which is 277 KB — 5 KB more
than the device's 272 KB, so this is a close proxy, not a generous one). At
`max_billboards=112` (the value actually set) the arena is at **64.7%**.

So: **no conflict to report.** The old ceiling was an artifact of the old,
oversized triangle pools, not a hard limit — shrinking `max_tris` and
`max_tex_tris` to their measured peaks freed enough arena to comfortably fit
a billboard pool sized off ITS OWN measured peak. If a future change grows
`max_tris`/`max_tex_tris` back up, re-check the arena headroom before
assuming 112 billboards still fits (the one-line `s_arena.used` hook
described above reproduces this check in under a minute).

## What was not measured (and why)

- **Driving speed.** All counts above are `MODE_FOOT` at the teleport point,
  camera swept through a full rotation. `VIEW_GROUND_R`, `VIEW_BLD_R`,
  `VIEW_HAZE_R` and `VIEW_BLD_CAP` are constants — not speed-dependent — so
  the set of buildings/ground tiles a given camera position+facing can draw
  is the same on foot as in a car. What DOES differ in `MODE_CAR` is the
  chase-camera framing itself (`CAM_CAR_D0..D1`, `CAM_CAR_H0..H1`,
  `CAM_CAR_L0..L1` in `game.c`, interpolated by speed up to `CAM_SPD`): at
  speed the eye sits up to 10 m back and 3.6 m up (vs. 4.5 m / 2.4 m on
  foot), and the look-ahead target is 9 m ahead (vs. 3 m). A camera further
  back and higher generally sees MORE geometry, not less, so these
  foot-mode numbers are a plausible but not certain proxy for "at speed."
  **Confirm on device**: drive down the downtown avenue at full speed and
  read the debug HUD's `t` (triangle) value against the foot-mode numbers
  above.
- **Textured-triangle count on device.** The debug HUD (see below) can only
  show `scene_tri_count()` (the flat pool) via the shipped API — there is no
  `mote->` entry for the textured pool. On device, the SIGNAL for textured
  overflow is the starved-flag behavior (untextured, tinted buildings) and
  the one-time host-log warning is device-log-only equivalent — watch for it
  if `mote logs` is available, otherwise watch for the visual symptom.
- **Full regression, visually.** See the dedicated section below — several
  items need eyes on a screen (siren audio, camera-in-geometry checks,
  two-device link play) that a headless host run cannot provide.

## Debug HUD (permanent, behind `MOTE_GTA_DEBUG`)

Added in `g_overlay()`, `games/grandthumbauto3/src/game.c` (search
`Profiling HUD (Task 13)`). Two lines, top-left, drawn only when
`getenv("MOTE_GTA_DEBUG")` is set and only in `#ifdef MOTE_HOST` builds (it
does nothing on device without also being ported past the `#ifdef` — see
note below):

```
<fps>fps u<update_us> r<raster_us> t<scene_tri_count>
bb<used>/<cap> drop<budget_drop>
```

`mote->perf()` (real signature: `void (*perf)(uint32_t out[6])`, filling
`[fps, update_us, raster_us, flush_us, core0_pct, core1_pct]` — NOT
`mote->metrics`, which does not exist in `sdk/mote_api.h`) is called each
overlay frame; per fact #1 above, the values it returns are for the frame
BEFORE this one (one frame of lag), and never include `overlay()`'s own
cost. `scene_tri_count()` is current-frame accurate.

**This HUD is currently gated to `#ifdef MOTE_HOST`.** For it to show on the
real device, drop that `#ifdef` (keep the `getenv` check — device builds
don't have `getenv`, so replace it with whatever this game's convention is
for a device-side debug toggle, or leave it host-only and rely on `mote
logs`/screenshots for the on-device numbers instead). This was left
host-only deliberately, matching every other `MOTE_GTA_DEBUG` block already
in this file, all of which are `#ifdef MOTE_HOST`.

## On-device checklist (do this with the device in hand)

1. **Build and push:**
   ```
   cd /Users/chris/code/hardware/mote
   ./tools/mote build games/grandthumbauto3 --device
   ./tools/mote push games/grandthumbauto3
   ```
2. **Downtown avenue, at speed.** Get into a car and drive down the longest
   avenue this profile found (map tile coordinates `(101,144)`, seed 12345 —
   if the shipped build doesn't support `MOTE_GTA_SEED`/`MOTE_GTA_VIEW` on
   device, drive to the densest downtown block manually instead). Read the
   debug HUD's `t` value at full speed, facing down the avenue. **Pass
   criterion: steady 30 fps** (once you also read the `fps` field — this
   profile could not produce that number). If it's below 30, follow the
   reduction order in "If downtown misses 30 fps" below.
3. **On foot in an alley.** Any narrow pavement gap between two building
   faces (this profile used tile `(126,125)`, seed 12345). Confirm the HUD's
   `t` value roughly matches this document's alley row (798 max textured —
   though the HUD only shows the flat count, so compare its `t` field against
   595-597) and that buildings render TEXTURED, not flat-and-tinted (the
   starvation symptom).
4. **On the bridge.** Tile `(137,186)`, seed 12345, or any bridge crossing.
   Confirm 30 fps and no visible stutter crossing onto/off the bridge deck.
5. **Spawn, camera against a wall.** Start a fresh game (or use seed 13 if
   seeding is supported on device) and stand still. Watch for trees or peds
   flickering in/out near the edge of view — that is the billboard-pool
   symptom (missing entities, not wrong-looking ones). If it doesn't
   reproduce with the new pool sizes, that's a good sign; the old 48-slot
   pool pinned here for 150+ frames in an earlier review, and this profile
   found true demand there is only 54-65 against the new cap of 112.
6. **What a starved frame looks like** — read this BEFORE looking at the
   debug HUD's numbers, so you recognize the visual symptom immediately:
   - **Textured-triangle starvation:** buildings that should be textured
     (concrete/window facades) instead render as FLAT, TINTED blocks. This
     is a rendering fallback, not a crash — nothing else breaks. If you see
     this, either raise `max_tex_tris` in `k_vtbl.config`
     (`games/grandthumbauto3/src/game.c`) or pull `VIEW_HAZE_R` inward (moves
     more of the building band from textured to flat, cheaper on the
     textured pool at the cost of flat-shaded buildings closer to the
     camera).
   - **Billboard starvation:** trees, pedestrians, cops, or pickups that
     should be visible simply AREN'T DRAWN AT ALL — not flickering, not
     wrong-colored, just absent. If you see empty patches where trees or
     people should be, especially near the edge of the view radius, raise
     `max_billboards` (already raised to 112 in this profile; the arena has
     headroom to go higher — see "No conflict with the arena ceiling"
     above) or reduce `VIEW_GROUND_R` (last resort per the brief's own
     order — it's already the shortest radius and driving speed reads off
     it).
7. **If downtown misses 30 fps**, reduce in this order, re-measuring each
   time (re-run this checklist's step 2 after each change):
   1. `VIEW_HAZE_R` (cheapest — moves buildings from textured to flat
      sooner, shrinking the textured-tri pool's load without shrinking the
      city visually as much as a full radius cut)
   2. `VIEW_BLD_CAP` (fewer building submissions per frame)
   3. `VIEW_BLD_R` (shorter building draw distance)
   4. `VIEW_GROUND_R` LAST — it's already the shortest radius, and ground
      detail is what sells speed to the player. If the city still misses 30
      fps after all four, the spec's reserve plan applies: road-corridor
      occlusion (deferred from the design on purpose). Stop and raise this
      rather than shipping under target.
8. **Once downtown holds 30 fps**, pin it. Add, once, on the first frame of
   `update()`:
   ```c
   static int fps_armed; if (!fps_armed) { fps_armed = 1; mote->set_fps_limit(30); }
   ```
   This profile deliberately left this OUT — see "Pool values set" above.

## Regression — what was exercised on host, and what needs the device

All of the following were run headless on host (`MOTE_AUTORUN=1`,
`MOTE_GTA_DEBUG=1`, `MOTE_GTA_SEED=42`, `MOTE_GTA_BRIEF=7 MOTE_GTA_MTYPE=1` —
the latter two are host-only test hooks in `g_update()` that auto-start a
mission without needing to walk to a phone box — `MOTE_DT_MS=16.6`,
`MOTE_KEYS="a:2-2 up:10-400 b:15-400 a:420-420 left:450-500"`, 520 frames).
The process exited cleanly (exit code 0), produced no crash/error/NaN/Inf
indicators in ~1000 lines of debug log, and:

- **Traffic flows:** exercised and confirmed via `[AI]`/`[DENS]` debug
  lines — traffic AI reported 0% off-road, single-digit percent "stuck," a
  sane average speed (~7.6-8.1 m/s), and low steering "wiggle" (0.04-0.10)
  throughout. The density streamer maintained its target car count (`nn`
  tracked `target` at steady-state, `R=11 rt=224 target=10 nn=10`),
  confirming cars spawn/despawn to hold a steady population rather than
  drifting to zero or exploding — evidence traffic flows and self-regulates.
  **Could not visually confirm** cars stop at junctions or don't drive
  through each other — that needs eyes on a screen.
- **Pedestrians:** the same run had peds present (billboard counts >0
  throughout) and the process didn't crash or hang while moving/firing near
  them. **Could not visually confirm** pathing along pavements or the
  "react to being threatened" behavior specifically — needs a screen.
- **Missions:** `MOTE_GTA_BRIEF=7 MOTE_GTA_MTYPE=1` auto-started a
  MI_COURIER mission (bypassing the phone-box walk-up), and the scripted
  run pressed A again at frame 420 to accept the briefing. No crash, no
  hang. **Could not confirm mission completion** — that requires actually
  reaching the delivery target, which this scripted run did not attempt.
  Starting a mission from a phone box (rather than the `MOTE_GTA_BRIEF` test
  hook) was also not exercised.
- **Weapons:** the `b:15-400` key range held the fire button through most of
  the run while moving; no crash. **Could not confirm every weapon fires and
  hits**, or that the tank's shell explodes — the player started unarmed
  (fist) and this run didn't pick up/buy other weapons or the tank.
- **Cops / heat:** not exercised — this run committed no crimes, so heat
  never left 0. Escalation, sirens, and the spray shop clearing heat all
  need a dedicated run (or device play) that actually commits a crime near
  a cop.
- **Pickups / cash / best-score save:** not specifically exercised —
  `add_pickup` calls happen at world init (weapons/health scattered near
  spawn) but this run's movement script wasn't aimed at any of them, and no
  death/arrest was triggered to test the best-score save path.
- **Link deathmatch (two devices):** **not exercised at all** — this needs
  two physical devices (or two host processes over `MOTE_LINK_SOCK`, which
  this session did not attempt) and is explicitly a two-device test in the
  brief.
- **Camera-in-geometry (garages, bridges, alleys, tight corners):** the
  camera's own collision guard (`gta3_camera.c`'s `clear_run`, covered by
  `gta3_test_camera`'s "the eye stops short of the wall" /
  "the eye never comes closer than the floor" cases, both passing) was
  exercised structurally via the alley and facade-jam scenarios above
  without crashing, but **visually confirming the eye never clips into a
  wall** needs a screen, not a host log.

## How to reproduce every number in this document

```
cd /Users/chris/code/hardware/mote/.worktrees/grandthumbauto3
./tools/mote build games/grandthumbauto3
cmake --build build_host --target mote_host -j8

# Downtown avenue (flat 375, tex 671, bb 92)
MOTE_AUTORUN=1 MOTE_GTA_DEBUG=1 MOTE_GTA_SEED=12345 MOTE_GTA_VIEW="101,144" \
  MOTE_DT_MS=16.6 MOTE_KEYS="a:2-2 left:5-300" MOTE_REC=/tmp/rec MOTE_REC_N=300 \
  ./build_host/mote_host games/grandthumbauto3/build/GrandThumbAutoIII.so

# Alley (flat 597, tex 798)
MOTE_AUTORUN=1 MOTE_GTA_DEBUG=1 MOTE_GTA_SEED=12345 MOTE_GTA_VIEW="126,125" \
  MOTE_DT_MS=16.6 MOTE_KEYS="a:2-2 left:5-300" MOTE_REC=/tmp/rec MOTE_REC_N=300 \
  ./build_host/mote_host games/grandthumbauto3/build/GrandThumbAutoIII.so

# Park (flat 597, tex 626)
MOTE_AUTORUN=1 MOTE_GTA_DEBUG=1 MOTE_GTA_SEED=12345 MOTE_GTA_VIEW="124,160" \
  MOTE_DT_MS=16.6 MOTE_KEYS="a:2-2 left:5-300" MOTE_REC=/tmp/rec MOTE_REC_N=300 \
  ./build_host/mote_host games/grandthumbauto3/build/GrandThumbAutoIII.so

# Bridge (flat 355, tex 516)
MOTE_AUTORUN=1 MOTE_GTA_DEBUG=1 MOTE_GTA_SEED=12345 MOTE_GTA_VIEW="137,186" \
  MOTE_DT_MS=16.6 MOTE_KEYS="a:2-2 left:5-300" MOTE_REC=/tmp/rec MOTE_REC_N=300 \
  ./build_host/mote_host games/grandthumbauto3/build/GrandThumbAutoIII.so

# Spawn facade-jam (seed 13, natural spawn — no MOTE_GTA_VIEW)
MOTE_AUTORUN=1 MOTE_GTA_DEBUG=1 MOTE_GTA_SEED=13 \
  MOTE_DT_MS=16.6 MOTE_KEYS="a:2-2" MOTE_REC=/tmp/rec MOTE_REC_N=160 \
  ./build_host/mote_host games/grandthumbauto3/build/GrandThumbAutoIII.so
```

`MOTE_REC` must point at a directory that exists (it dumps `.ppm` frames
there — harmless, delete afterward) — it exists only to bound the headless
run to `MOTE_REC_N` frames; without it, a headless run with no display never
quits on its own. `[BB]` lines print to stderr each frame under
`MOTE_GTA_DEBUG=1`; grep them for `used=`/`peak=`/`dropped=`. The
textured-triangle and arena numbers in this document required the temporary
engine-side instrumentation described in "How the counts were captured" —
they are not reproducible from the committed `game.c` alone (by design; the
`mote->` ABI intentionally does not expose the textured-tri pool or the raw
arena byte count to games).

The teleport coordinates above (map tile x,z passed to `MOTE_GTA_VIEW`) are
only meaningful together with `MOTE_GTA_SEED=12345` — they were found by
scanning that seed's generated city for the densest building window, the
longest straight road run inside it, the largest grass window, a bridge tile
nearest the map center, and a pavement tile pinched between two building
faces. A different seed will put different tiles at those coordinates.
