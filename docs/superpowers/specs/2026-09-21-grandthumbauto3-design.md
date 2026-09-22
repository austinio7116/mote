# Grand Thumb Auto III — third-person port

Date: 2026-09-21
Status: approved design, not yet implemented

## Goal

Fork `games/grandthumbauto` into `games/grandthumbauto3`, a third-person
crime sandbox: the camera sits behind and slightly above the player instead of
directly overhead. The full simulation ports over. Rendering, camera, culling,
entity art, and on-foot controls are rewritten.

## Decisions taken

| Decision | Choice |
|---|---|
| Relationship to the existing game | Fork to `games/grandthumbauto3`; `grandthumbauto` is untouched |
| Entity representation | Vehicles are 3D meshes; characters, trees and pickups are billboards |
| Simulation scope in v1 | Full port — traffic, peds, cops, heat, weapons, missions, pickups, tank, link deathmatch |
| On-foot controls | Tank controls, camera trails the facing |
| Visibility strategy | World-space frustum cone with banded draw distance |
| Radar | Included in v1 |

Rejected: road-corridor occlusion flooding (approach B) — kept in reserve for
the downtown profile, not written up front.

## Starting point

`games/grandthumbauto/src/game.c`, 4434 lines. What is already 3D:

- Buildings are textured cubes with facades and roofs, quantised to 14 height
  levels across 8 facade atlases (`build_bgeom:286`, `build_buildings:307`,
  `g_bmesh[NBLV][NBTEX]`). Garages are 5-sided boxes (`build_garages:329`).
- Ground is textured quads on y=0 (`build_ground:192`, `draw_ground_window:1272`).
- Road markings are thin quads at y=0.03 (`paint_quad:1091`, `road_markings:1111`).

What is top-down only:

- `set_topdown_camera:433` — eye directly above the player at 15 m on foot,
  ramping to 40 m at top speed; forward = −Y.
- Every entity is a flat ground quad with top-down sprite art, drawn
  `MOTE_DRAW_NO_DEPTH_WRITE` (`draw_ground_sprite:226`): player, cars, peds,
  cops, props, pickups, scenery, tank turret.
- Culling is a fixed tile window (±12 ground, ±14 buildings) gated by
  `world_to_screen:442`.

`world_to_screen` is generic — it derives from `cam_basis`/`cam_pos` — so it
stays correct under any camera. Its 26 call sites do not need rewriting for
correctness, only the three noted under *Overlay changes*.

Resource budget today (`k_vtbl.config`, game.c:4428): `max_tex_tris = 1600`,
`max_tris = 2200`, `depth = 1`, `max_bodies = NCAR+NSTAT`, `max_contacts = 220`.

In-repo precedent: `games/motokart` already runs a chase camera via
`mote_camera_look(eye, target)` + `mote->scene_camera()` (game.c:1223) with
billboarded drivers and trees and dot-product frustum culling.

## Architecture

### 1. Camera

`set_topdown_camera(tx, tz)` is replaced by `chase_camera(dt)`. The three call
sites (3606, 3773, 3974) keep calling
`mote->scene_camera(&cam_basis, cam_pos, FOV)` unchanged.

Framing. Anchor is the player position at eye height. Facing yaw is the
vehicle's `yaw` in a car, the character's `yaw` on foot.

```
eye    = anchor - fwd*dist + up*height
target = anchor + fwd*look
```

| State | dist | height | look |
|---|---|---|---|
| On foot | 4.5 m | 2.4 m | 3.0 m |
| In car, stopped | 6.5 m | 2.8 m | 4.0 m |
| In car, 18 m/s | 10.0 m | 3.6 m | 9.0 m |

In-car values interpolate on speed over 0–18 m/s, reusing the existing speed
ramp at game.c:3963 that currently drives `g_camh`.

`FOV` changes from 60° to 55°. At 128×128 a 60° horizontal field behind a car
reads as fisheye.

Smoothing is exponential (`1 - expf(-k*dt)`), with position at k≈8/s and yaw at
k≈4/s, yaw interpolated through the shortest angular difference. The slower yaw
rate is what makes corners swing rather than snap.

Yaw follows the vehicle's heading, never its velocity, so reversing does not
spin the camera. The camera stays behind the nose and never flips.

Camera collision is mandatory, not optional: in a grid city every corner would
otherwise put the eye inside a facade. DDA the tile grid from anchor toward the
desired eye using `tile_at`; on hitting `#`, `O` or `H`, place the eye just
short of the hit, with a floor of 1.8 m from the anchor.

### 2. Controls

In-car bindings are unchanged: RB throttle, LB brake, d-pad steer (game.c:3872).

On foot, tank controls:

| Input | Action |
|---|---|
| d-pad left/right | Turn in place, ~3 rad/s |
| d-pad up | Walk forward |
| d-pad down | Back up at half speed |
| A | Enter vehicle / context action (unchanged) |
| B | Attack (unchanged) |
| RB | Cycle weapon (unchanged, game.c:3815) |
| LB | Hold to look behind — new; LB is currently unused on foot |

Existing auto-aim is retained. A d-pad plus tank controls cannot aim precisely,
so auto-aim is load-bearing here rather than a convenience.

### 3. Visibility

`tile_visible:1084` and the two draw windows (1272, 1297) are rewritten around a
world-space cone.

Once per frame, derive `cam_fwd` and `cos_half` from the basis and FOV.
`tile_in_view(x, z, y, radius)` rejects on distance first, then on
`dot(dir, cam_fwd) < cos_half`. The angular threshold is slackened by one tile's
angular size at that distance so tiles do not pop at the screen edge.

Two radii, each iterating a square window sized to itself rather than the
current fixed ±12/±14:

- Ground: 52 m (13 tiles). Beyond that the road is near edge-on and occluded by
  facades.
- Buildings: 112 m (28 tiles). The existing dual base/top visibility test
  (game.c:1312-1313) is kept so tall blocks do not blink out.

Buildings beyond 70 m draw with `MoteObject.color` set to a blend toward the
`scene_set_background` colour. This reads as haze, hides the cut-off, and moves
those draws off `max_tex_tris` onto `max_tris`, which is where the headroom is.

This depends on the per-draw colour override superseding a mesh texture. Verify
that in the rasterizer before building on it. If the override does not apply to
textured meshes, the fallback is a hard distance cut with a larger building
radius and no haze.

Submission order is near band first, then far band until a submission cap is
hit. Overflow then degrades as distant haze blocks disappearing, rather than the
engine silently dropping whatever arrives last.

Starting radii, the 70 m haze threshold and the submission cap are starting
values. Final numbers come from profiling `scene_tri_count()` in the dense
downtown, not from this document.

### 4. Entities

The rule: anything genuinely flat stays a ground quad, anything with volume
becomes a mesh, anything small becomes a billboard.

**Unchanged.** `draw_ground_sprite:226` is correct in 3D for things lying on the
ground, and is kept for road markings, the spray decal, the gun mat, and skid
and blood decals. `road_markings:1111` needs no change.

**Vehicles → meshes.** The 19 `CAR_CLS` classes collapse to 7 silhouettes:

| Silhouette | `CAR_CLS` classes |
|---|---|
| sedan | SEDAN, LUXURY, TAXI, POLICE |
| compact | COMPACT, HOTHATCH |
| low wedge | COUPE, SPORTS, RACER, CLASSICSPT |
| long hood | MUSCLE, CLASSIC |
| wagon | WAGON, JEEP |
| van | VAN, PICKUP |
| truck | AMBULANCE, FIRETRUCK, TOWTRUCK |

Bus and tank keep their own meshes. Each is built like `build_bgeom` —
int8 vertices, hand-authored faces — as a body box plus a cabin box, about 22
triangles. Twelve cars on screen is roughly 264 triangles.

Per-car identity comes from colour, not geometry. Each vehicle draws as two
objects: body with `MoteObject.color` set to that car type's paint, cabin with a
dark glass tint. A new `assets/make_carcolors.py` samples the dominant opaque
pixel of each of the 54 `cars2.png` cells into
`static const uint16_t CAR_COL[54]` in `src/carcolors.h`. All 54 car types keep
their distinct look with no per-type 3D art.

Mesh dimensions come from the existing per-type art sizes (`cars2_meta.h`,
`CARS2_MPP`), so handling and collision stay exactly as tuned.

No wheels in v1: four boxes is 48 triangles per car for something two pixels
tall on screen.

The tank turret becomes a mesh — box plus barrel, parented to hull yaw —
replacing the rotated `tankturret` sprite at game.c:3557.

**Characters → billboards.** `scene_add_billboard` at 1.8 m world height.
Positions are absolute, which is valid because the game uses `scene_camera`.

The existing `player_img`, `ped_img` and `cop_img` are top-down views of heads
and shoulders and are unusable as billboards. A new `assets/make_chars3d.py`
generates side-view art: 4 facings (back, front, left, right) × 4 walk frames ×
the existing character variants, still 16×16 cells. Facing is selected from the
angle between the character's yaw and `cam_fwd`. The camera trails, so the back
view dominates for the player and the other three are mostly seen on peds.

**Scenery and pickups → billboards.** `scenery_img` currently holds top-down
canopies; `make_chars3d.py` also generates side-view oak, pine and bush.
Pickups (`pickups_img`, 16×16) become billboards with a small vertical bob. The
phone box becomes a low box mesh.

**Grounding.** Every vehicle and character gets one `scene_add_shadow_ex` using
its footprint semi-axes and heading. Without it, billboards and meshes float.
This is the cheapest thing that sells the third-person view.

**Bullets and tracers** (game.c:4188, 4301) currently project by hand in
`overlay`, which works but draws over buildings. They move to depth-tested
`scene_add_line` in the 3D pass.

### 5. Radar

New in this fork, not a port. Without it the missions, which route you across
the city by marker arrow alone, stop being navigable — top-down the street grid
is self-evident, and behind a car at 3 m it is not.

A small rotating radar drawn in `overlay`, sampling `tile_at` around the player:
roads light, water dark, buildings mid, with mission markers and wanted cops as
dots. Rotates with the camera yaw so up is forward. No new assets.

### 6. Resource budget

Changes to `k_vtbl.config` (game.c:4428):

- add `max_billboards ≈ 48`
- add `max_shadows ≈ 40`
- add `max_lines ≈ 24`
- raise `max_tris` (vehicle meshes and far-band haze buildings now compete for it)
- re-tune `max_tex_tris` once the near/far band split is measured

Exact values are set from the downtown profile.

## What ports verbatim

The entire simulation is XZ world-space and camera-independent: `citygen.h`,
`update_traffic`, `update_peds`, `update_cops`, `update_heat`, `fire_weapon`,
`do_runovers`, `sight_clear`, `place_markers`, the 2D physics bodies, missions,
pickups, and the link deathmatch. That is the bulk of the 4434 lines.

## Overlay changes

Three places in `overlay` beyond rendering:

1. **Off-screen marker arrows** (game.c:4069-4090, 4228-4236). `world_to_screen`
   returns 0 behind the near plane, and behind the camera is now most of the
   world. Today the maths happens to work; with a horizontal camera it silently
   drops markers to the rear. Add an explicit behind-camera branch that computes
   the screen-edge direction from the world-space bearing instead of from the
   failed projection.
2. **Title / attract camera** (game.c:3973). Becomes a slow orbit around a
   downtown block, which doubles as the gallery screenshot shot.
3. **Physics debug overlay** (game.c:4188-4195). Projects body hulls at y=0.1;
   correct, but now drawn through buildings. Leave it, gated behind the existing
   `MOTE_GTA_DEBUG`.

`MOTE_GTA_VIEW` teleport-by-tile (game.c:2983) is kept — it is the fastest way
to reach downtown for profiling.

## Fork mechanics

`tools/mote` discovers games by directory, so:

1. `cp -r games/grandthumbauto games/grandthumbauto3`
2. `MOTE_GAME_META("Grand Thumb Auto III", "austinio7116")`
3. `MOTE_GAME_VERSION("0.1.0")`
4. New `icon.png` via the existing `assets/make_icon.py`

No `CMakeLists.txt` change — that file only holds `thumbycue`'s host test
binaries. `docs/games.json` is generated by `tools/gen_gallery.py` at release
time.

## Verification

`mote run grandthumbauto3` on the SDL emulator for iteration; `mote push` for
the numbers that matter. Wire `mote->metrics` (per-frame `fps`, `update_us`,
`raster_us`) and `scene_tri_count()` to a debug HUD behind `MOTE_GTA_DEBUG`.

Checkpoints, measured on device and not in the emulator:

1. Triangle and textured-triangle counts stay under budget at the densest
   downtown cell, at speed, facing down the longest avenue. That worst case is
   reachable directly via `MOTE_GTA_VIEW`.
2. Frame time holds 30 fps there. If it does, pin it with `set_fps_limit(30)`
   for steady timing.
3. The camera is never inside geometry. Drive every map edge case: garages,
   bridges, narrow alleys, tight corners.
4. Every ported system still works: traffic flows and stops at junctions, peds
   path and react, cops escalate through heat levels, weapons fire and hit,
   missions can be started and completed, pickups collect, the tank drives and
   fires, link deathmatch connects.

## Deferred

- Road-corridor occlusion flooding (approach B), unless the downtown profile
  demands it.
- Vehicle wheels.
- Look-behind in a car. LB and RB are both taken by brake and throttle, and the
  no-flip reverse camera covers the case it would serve.
