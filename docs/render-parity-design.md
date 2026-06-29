# Engine render parity — design

Goal: fold the visual features that flagship games currently ship themselves
(via the full-frame `render_band` escape hatch) into the built-in engine, in a
form **generic enough for devs to do more** — not a one-off recreation of any
single game's renderer.

Flagship games this must serve, and *why they break the naive plan*:

- **Indemnity Run** — procedural ship/station meshes, lit/flat shading, planet
  spheres, particle + beam FX. (Triangle raster, but richer than flat.)
- **ThumbyCue** — flat-shaded table + **textured** spinning ball impostors,
  shadow decals. (Triangle raster + textured impostors.)
- **ThumbyCraft** — a **voxel raycaster**. *Not triangles at all.* This is the
  proof that "add more triangle features" is the wrong target: the engine has to
  host fundamentally different renderers, not just a fatter triangle path.
  > **Update (2026-06-29):** ThumbyCraft's raycaster is *not* being folded into the
  > engine — it stays a game-side `render_band` renderer (see "Renderer scope" in
  > [`PLAN.md`](PLAN.md)). The Layer-2 design below stands as the cheap path *if*
  > that's ever revisited, but the engine's supported renderers are 2D + 3D raster.

## Architecture: three layers, by how custom the dev must get

Today there are two options and a cliff between them — the flat-shaded triangle
scene (easy, flat-only) or `render_band` (own the whole frame, lose all engine
help). Every flagship lives at the `render_band` extreme because the middle is
empty. The design fills the middle:

| Layer | Dev supplies | Engine owns | Serves |
|---|---|---|---|
| **1. Rich built-in passes** | data: meshes + *materials*, textured impostors, particle emitters | transform, clip, light, raster, depth, dual-core dispatch | ThumbyCue, ~80% of Indemnity |
| **2. Composable custom passes** | a banded `f(fb,y0,y1)` pass *registered alongside* engine passes | depth buffer, band dispatch, compositing, present, timing | **ThumbyCraft raycaster**, future renderers |
| **3. Material shader callbacks** | a C function pointer per material (per-span, never per-pixel) | the whole pipeline; calls you per span | exotic surfaces (planet terrain) |

Layer 2 is the generality lever: it turns `render_band` from "replace the frame"
into "add a depth-sharing pass," so a ThumbyCraft world can have engine-rendered
mobs/particles composited into it with correct occlusion — the engine becomes
useful to renderers it has never heard of.

## The three budgets (this is what constrains everything)

| Budget | Headroom | Who pays | Scarcity |
|---|---|---|---|
| **Static OS BSS** | ~19 KB heap gap, blocked by the post-arena alignment jump | the OS, every game, forever | **brutal** — ~4 KB added likely forces another arena trim |
| **Arena** | 276 KB, per-game, sized by `MoteConfig` | each game, only for pools it declares | medium — dev controls it; OOM is real |
| **Flash** | ~1 MB of 2 MB+ used | code + rodata + game textures | abundant |

**Design rule:** every new feature lands in **arena (opt-in pool)** or **flash
(`const`)** — and adds **essentially nothing to static BSS**. The engine is
already built this way (all render pools are `mote_arena_alloc`). The one place
the rule was broken — `s_spr[MOTE_SCENE2D_MAX_SPRITES]`, a fixed 128-entry static
array — is the anti-pattern to never repeat.

### Measured entry sizes (real, from the structs)

- `ScreenTri` = **36 B** (3×{float x, float y, u16 z} + u16 color). At
  `max_tris=3328` the flat draw-list is already **~117 KB of arena**.
- `ScreenSphere` = **24 B**. At `max_spheres=200` ≈ **4.7 KB**.

## Extended MoteConfig — every feature is an opt-in, metered knob

Flat games set the new knobs to 0 and pay **nothing**. The Studio arena meter
sums knob × entry size, so the cost is visible *before* building.

```c
typedef struct MoteConfig {
    /* --- existing --- */
    uint16_t max_tris;        /* flat/lit tri draw-list   — 36 B/entry */
    uint16_t max_spheres;     /* sphere impostors         — 24 B/entry */
    uint16_t max_splats;
    uint16_t max_sprites;     /* 2D                       (fixed 128 static — legacy) */
    uint16_t max_bodies;
    uint16_t max_contacts;
    uint16_t max_mesh_tris;
    uint8_t  depth;           /* the 32 KB depth buffer */

    /* --- ABI v24: render parity (all opt-in; 0 = pay nothing) --- */
    uint16_t max_tex_tris;    /* textured-tri pool        — ~44 B/entry */
    uint16_t max_particles;   /* particle/beam pool       — ~32 B/entry */
    uint8_t  tex_impostors;   /* 1 = widen ScreenSphere 24->~40 B for textured/oriented balls */
} MoteConfig;
```

### Per-feature cost ledger

| Feature | Layer | Arena knob | Entry | Typical | Arena cost | Static BSS | Flash |
|---|---|---|---|---|---|---|---|
| **Material: LIT / TOON / GLOSS** | 1 | *none* | — | — | **0** | 0 | shading branches |
| **Textured triangles** | 1 | `max_tex_tris` | ~44 B (ScreenTri + 3×u8 uv + u8 tex idx) | 512 | **~22 KB** | **~1 KB** (pipe `s_uv[MOTE_MAX_VERTS]` transform scratch) | raster path |
| **Textured / oriented impostors** | 1 | `tex_impostors` (widens `max_spheres`) | 24→~40 B | 200 | **+~3.2 KB** | 0 (angle→UV LUT lives in flash `const`) | LUT + path |
| **Particle / beam FX** | 1 | `max_particles` | ~32 B | 512 | **~16 KB** | 0 | integ + raster |
| **Composable custom pass** | 2 | *none* (shares depth) | — | — | **0** | ~32 B (pass-ptr registry) | dispatch glue |
| **Material shader callback** | 3 | *none* | — | — | **0** | 0 | call site |
| **Procedural mesh builders** | SDK | game's own `alloc()` | — | dev-sized | dev's budget | 0 | builder code |

**Single largest *static* cost of the whole effort: ~1 KB** — the per-vertex UV
scratch the pipe needs to carry texture coords through transform. Everything else
expensive is opt-in arena the dev sizes and the meter shows.

### How each flagship maps to knobs

- **ThumbyCue**: `max_tris` (table) + `max_spheres` + `tex_impostors=1`. No new
  large pool; impostor widening costs ~3 KB.
- **Indemnity Run**: `max_tris` (ships/stations, `material=LIT` — free) +
  `max_spheres` + `tex_impostors` (planets) + `max_particles` (FX).
- **ThumbyCraft**: `max_tris=0`, registers a **Layer-2 custom raycast pass** +
  `depth=1`; its voxel buffers are the game's own `alloc()`. Engine pool cost
  ≈ 0. (The meter can't see the game's `alloc()` — the existing "custom renderer"
  inspector note applies; a game may declare an estimate.)

## IDE authoring (per the IDE-first philosophy — every runtime feature gets one)

- **Material editor** — mode + texture + lit/toon/gloss params, live on the Mesh tab.
- **Particle/FX editor** — emitter shape, rate, lifetime, colour ramp, gravity.
- **Mesh-gen tool** — expose parametric builders (lathe/extrude/loft + box/sphere/
  cylinder) so an Indemnity-class ship is authored, not hand-coded.
- **Impostor/planet editor** — wrap a texture on the sphere, terrain/tint params.
- "Shaders" need no shading language: Mote games are native C, so a material
  shader is just a callback in `game.c`; the IDE authors the *data*.

## Perf reality (RP2350 M33 @280 MHz, software raster, no GPU)

- Textured tris: affine is cheap (2 adds + texel fetch/pixel); perspective-correct
  is a per-span divide. Fine for small tris; the textured pool gets its own budget
  line because each entry is fatter and the per-frame fill cost is higher.
- Textured impostors are the per-pixel-heavy case — sphere normal → orientation →
  equirect UV needs an **angle→UV LUT (flash `const`)**, not naive `asin/atan2`.
- Layer-3 callbacks are **per-span, never per-pixel** (a per-pixel C call tanks).
  Per-pixel exotica stays a built-in fast path in engine C.

## Refined scope (after reading both renderers)

Element-by-element inventory of ThumbyCue (`cue_render.c`, ~2k lines) and Indemnity
(`r3d_*.c`, ~3k lines) against the engine changed the picture:

- **Flat + sun mesh shading is ALREADY in the engine.** Indemnity's per-face
  `shade565` is identical to `mote_pipe`'s (`0.25 + 0.75·ndotl`); ThumbyCue's table
  is the same flat-diffuse idea. → Ships, stations, table, cushions, rails, pockets,
  cue stick all port to `scene_add_object` with **no new shading feature**.
- **Neither game needs textured TRIANGLES.** All texturing is on *impostors*
  (ThumbyCue balls, Indemnity planets/suns). → **Drop `max_tex_tris`** from this
  effort; defer textured tris to a later, future-game-driven phase.
- **Particles/beams are point/line/disc draws.** Indemnity's FX (1024 particles +
  12 fireballs + 24 beams) and ThumbyCue's aim dots / ghost ring / cue lines all map
  to depth-tested points/lines/discs. The raster *primitives already exist*
  (`mote_point/line/disc`) — what's missing is **scene-level draw-lists** so both
  cores walk them in the banded pass.
- **The one substantial new feature is the textured + oriented sphere impostor**
  (balls, HUD ball icons, planets, suns) — with a small shade-mode set and an
  optional per-pixel sample callback for procedural surfaces.
- **Backgrounds want a callback, not full `render_band`.** Starfield, nebula,
  galaxies, supercruise dust, the gradient: a game-supplied `bg(fb,y0,y1)` run
  before the scene (depth cleared, composited) covers them generically.
- **Stays custom (fine):** ThumbyCue ball-shadow ground decals, the 6-wedge snooker
  icon, and all HUD/overlay (already on the `overlay` hook, untouched).

### Revised MoteConfig knobs for THIS effort (ABI v24)

```c
/* drop max_tex_tris/max_particles from the v24-for-these-games set */
uint16_t max_points;      /* particle/aim/dust point list — 12 B/entry */
uint16_t max_lines;       /* beam/ghost-ring/cue line list — 20 B/entry */
uint16_t max_discs;       /* fireball/expanding-disc list   — 16 B/entry */
uint8_t  tex_impostors;   /* widen ScreenSphere 24->~40 B for textured/oriented balls */
```

Cost check (Indemnity, the heavier): points 1300×12 ≈ 15 KB, lines 48×20 ≈ 1 KB,
discs 28×16 ≈ 0.5 KB, impostor widening 200×16 ≈ 3 KB. ~20 KB arena, all opt-in,
all metered. Static BSS: still ~nothing (point/line/disc are stateless raster prims;
impostor UV LUT → flash). **No textured-tri UV scratch needed → static cost ≈ 0.**

### ABI v24 API additions (append-only)

- `scene_add_point(Vec3 cam_rel, uint16 color, int size)` — depth-tested point.
- `scene_add_line(Vec3 a, Vec3 b, uint16 color)` — depth-tested line (beams).
- `scene_add_disc(Vec3 cam_rel, float r_world, uint16 color)` — depth-tested disc.
- `scene_add_sphere_tex(Vec3 pos, float r, const Mat3 *orient, const MoteSphereTex *tex, int shade_mode)`
  — the textured/oriented impostor. `MoteSphereTex` = either a baked equirect RGB565
  texture (planets, pre-baked balls) **or** a per-pixel sample callback
  `uint16 (*sample)(Vec3 local_n, void *ud)` (ThumbyCue's procedural stripes/number).
- `scene_add_object_ex(const MoteObject*, uint32 flags)` — `flags` carries
  `MOTE_TRI_NO_DEPTH_WRITE` (ThumbyCue pocket lips) and future per-object material bits.
- `set_background_cb(void (*bg)(uint16 *fb, int y0, int y1))` — banded background pass
  run before geometry.

### Phased plan (port both games while generalising each capability)

**Phase 1 — Scene primitives + plumbing (low risk, high coverage).**
Add the point/line/disc draw-lists + `scene_add_*`, the `MOTE_TRI_NO_DEPTH_WRITE`
flag, and the v24 config/ABI. Port Indemnity's FX and ThumbyCue's aim/cue/ghost/lips
onto them. Balls & planets stay on their existing custom impostor for now (hybrid).
Proves the engine path end-to-end on real games.

**Phase 2 — Textured/oriented impostor (the marquee feature).**
Add `scene_add_sphere_tex` + shade modes + the angle→UV LUT (flash). Move ThumbyCue
balls + HUD ball icons and Indemnity planets/suns onto it. This is the one
perf-sensitive, per-pixel-heavy piece — budget the LUT and test fill cost on device.

**Phase 3 — Background callback.**
Add `set_background_cb`; move starfield/nebula/galaxies/dust/gradient onto it. After
this, both games' `render_band` bodies are gone — they render via `scene_add_object`
+ `scene_add_point/line/disc` + `scene_add_sphere_tex` + a background callback. **Goal
met: both flagships on the engine.**

**Phase 4 — Generalise & author (stretch / future games).**
Promote `ship_gen`/`station_gen`'s operations to an SDK procedural-mesh builder
(loft/extrude/revolve); a generic ground-decal primitive; the IDE editors
(material/impostor-texture/particle). Plus textured triangles when a future game needs
them. This is the "useful for other/similar games" payoff.

After Phase 3, the only things still custom are genuinely game-specific (ball-shadow
decals, the snooker wedge icon) and the HUD overlays — which were never the engine's job.

## Engine primitive set after the render-parity work (ABI v24–v31)

The render-parity arc added these reusable engine capabilities (all opt-in, metered):

- **3D scene FX** (depth-tested, dual-core): `scene_add_point` / `scene_add_line` /
  `scene_add_disc` (v24), `scene_add_tri` (v27, double-sided immediate-mode world
  triangle), `scene_add_shadow` (v28, soft ground decal), `scene_add_ring` (v31,
  camera-facing circle outline — ghost balls, reticles).
- **Impostors**: `scene_add_sphere_tex` (v25, textured *or* per-pixel-callback,
  oriented, shade modes).
- **Materials/look**: flat+sun was already built in; `MOTE_DRAW_NO_DEPTH_WRITE`
  (v24) for coplanar overlays.
- **Background**: `set_background_cb` (v26) banded sky pass; `scene_set_near` (v28)
  runtime near plane.
- **2D framebuffer drawing** (HUD/overlay/bg): `draw_pixel` / `draw_line` /
  `draw_rect` (v30), `draw_circle` (v31).
- **Audio**: engine-owned master volume `audio_set_master`/`audio_get_master` (v29).

Design rule held throughout: each lands in **opt-in arena pools** (sized by
MoteConfig) or **flash**, never new fixed static arrays — static OS-BSS growth ≈ 0.

## Result: engine vs custom, after the ports (ABI v31)

Both flagships now render through the built-in engine. "Custom" no longer means a
private rasteriser — it means game data/logic, or screen-space painting done
*through* engine primitives (`set_background_cb`, `draw_line/pixel/rect`, the
impostor shade callback). Neither game uses `render_band`.

### Indemnity Run

| Element | How it renders | Engine or custom |
|---|---|---|
| Ships, stations | `scene_add_object` (flat + sun-lit tri mesh) | **Engine** |
| Asteroids, loot canisters | `scene_add_object[_scaled]` | **Engine** |
| Planets | `scene_add_sphere_tex` (palette equirect texture, `MOTE_SHADE_LIT`) | **Engine** |
| Sun | `scene_add_disc` (emissive) | **Engine** |
| Lasers / beams | `scene_add_line` | **Engine** |
| Bolts, sparks, debris, engine trails, space dust | `scene_add_point` | **Engine** |
| Explosions / fireballs | `scene_add_disc` (world-radius) | **Engine** |
| Depth / occlusion, dual-core raster, present | engine scene pipeline | **Engine** |
| Audio (weapon/explosion SFX), volume | `audio_play` + engine master `audio_set_master` | **Engine** |
| Starfield, nebula / galactic band, distant galaxies | `set_background_cb` → game paints the sky (direct fb writes) | Custom *(via engine bg pass)* |
| Supercruise dust streaks | game computes screen tunnel → `draw_line` | Custom *(via engine 2D prim)* |
| Cockpit, HUD, reticle, scanner, text | `overlay()` + `text`/`draw_line/rect` | Custom *(via engine 2D prims)* |
| HUD world→screen marker projection | `r3d_pipe_project` (tiny camera helper) | Custom *(engine camera isn't ABI-exposed)* |
| Procedural ship/station/rock geometry | `ship_gen`/`station_gen` emit engine-format meshes | Custom (game data) |
| Flight model, AI, trading, galaxy sim | game logic | Custom (not rendering) |

### ThumbyCue

| Element | How it renders | Engine or custom |
|---|---|---|
| Table bed, cushions, rails, pocket voids | `scene_add_tri` (world triangles, pre-lit) | **Engine** |
| Pocket drop-lips | `scene_add_tri` + `MOTE_DRAW_NO_DEPTH_WRITE` | **Engine** |
| Balls + HUD ball icons | `scene_add_sphere_tex` (oriented impostor) + a **custom per-pixel shade callback** for the number/stripe/measles pattern & 4-lamp specular | **Engine** geometry, custom shader |
| Ball shadows | `scene_add_shadow` (soft depth-tested ground decal) | **Engine** |
| Aim dots | `scene_add_point` | **Engine** |
| Object-ball line, ghost-ball ring | `scene_add_line` | **Engine** |
| Cue stick | `scene_add_tri` (tapered quad) | **Engine** |
| Depth / occlusion, dual-core raster, present | engine scene pipeline | **Engine** |
| Audio (hit/pot SFX), volume | `audio_play` + engine master `audio_set_master` | **Engine** |
| Table backdrop gradient | `set_background_cb` | Custom *(via engine bg pass)* |
| HUD: 6-wedge snooker icon, score/labels, menus | `overlay()` + `text` | Custom *(via engine 2D prims)* |
| Physics, rules, AI | game logic | Custom (not rendering) |

The only rendering still done by the game itself is **screen-space 2D** (sky
background, HUD) — which it does *through* the engine's `set_background_cb` and
`draw_*`/`text` primitives, not a private rasteriser — plus one per-pixel impostor
**shade callback** (ThumbyCue balls), which the engine's impostor primitive invokes.
Everything 3D (meshes, planets, particles, beams, shadows, depth, dual-core
rasterisation) is the engine.

## Open decisions (before phasing)

1. **First-pass scope** — narrow ("ThumbyCue on the built-in engine": materials +
   textured impostors) vs. broad (Layer-2 composable passes first, so ThumbyCraft's
   paradigm is proven early).
   **Resolved (2026-06-29): narrow.** The render-parity work shipped for the raster +
   2D paths (ABI v24–v31). **Layer 2 is NOT pursued for the voxel raycaster** — see the
   "Renderer scope" decision in [`PLAN.md`](PLAN.md): the engine's baselines are the
   high-performance 2D and 3D-rasteriser paths, and the slow per-pixel voxel raycaster
   stays a game-side `render_band` renderer (ThumbyCraft/ThumbyRogue own it). The Layer-2
   "share the engine z-buffer with a custom pass" idea below remains the cheap *first*
   step **if** voxel-in-engine is ever revisited, but is not planned now.
2. **Material model** — material-on-mesh (baked, simplest) vs. material-on-object
   (per-draw override). Lean: object override on top of a mesh default, mirroring
   the existing `color` override.
3. **UV packing** — `ScreenTri` already has 3 pad bytes after the u16 depths;
   uv/tex-idx may pack into those, reclaiming part of the ~8 B/entry growth.
