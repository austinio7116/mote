# Grand Thumb Auto III Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fork `games/grandthumbauto` into `games/grandthumbauto3`, a third-person crime sandbox with a chase camera behind the player.

**Architecture:** The world geometry is already 3D — buildings are textured cubes, ground is textured quads. Only the camera is top-down. Three pure-C modules (`gta3_view`, `gta3_camera`, `gta3_veh`) hold the new maths and are unit-tested on the host with no engine or renderer around them; `game.c` stays the orchestrator and calls into them. Entity rendering moves from flat ground quads to meshes (vehicles) and billboards (characters, scenery, pickups).

**Tech Stack:** C99, the Mote engine ABI v47 (`sdk/mote_api.h`, `sdk/mote_build.h`), `tools/mote` CLI for build/run/push, CMake host test executables, Python 3 + Pillow for asset generation.

**Spec:** `docs/superpowers/specs/2026-09-21-grandthumbauto3-design.md`

## Global Constraints

- Target hardware: Thumby Color, RP2350, 128×128 RGB565. Frame budget target **30 fps on device**, measured with `mote push`, never from the SDL emulator.
- Engine ABI floor: **47**. `scene_add_billboard` is v33, `scene_add_shadow_ex` is v32, `scene_add_line` is v24 — all available. Guard nothing.
- `tools/mote` sweeps every `src/*.c` **except** `test_*.c` into the game module. New `.c` files in `src/` are compiled in automatically; `test_*.c` files are host-only and built by `CMakeLists.txt`.
- Host tests live under `if(NOT WIN32)` in `CMakeLists.txt`, compiled with `MOTE_HOST=1`, `-O2 -Wall -Wextra -Wno-unused-parameter`, linked against `m`. Follow the `cue_test_jump` pattern at `CMakeLists.txt:186-199`.
- Resource pools are declared in `k_vtbl.config`. Current values: `max_tex_tris = 1600`, `max_tris = 2200`, `depth = 1`, `max_bodies = NCAR+NSTAT`, `max_contacts = 220`.
- `MoteObject.color` does **not** suppress a mesh texture (`engine/render/mote_pipe.c:127-132`). A tinted draw requires a mesh with `.texture = 0` and `.face_uvs = 0`.
- World units are metres. `TILE = 4.0f` metres per map tile. Tile characters: `.` road, `,` pavement, `~` water, ` ` grass, `B` bridge, `#`/`O`/`H` buildings.
- Do not modify `games/grandthumbauto/`. It must keep working untouched.
- Commit after every task. Commit messages use the repo's existing style: a plain descriptive sentence, no `feat:`/`fix:` prefixes (see `git log`).

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `games/grandthumbauto3/src/gta3_view.h` / `.c` | Frustum-cone visibility test and band radii. Pure maths, no engine calls. |
| `games/grandthumbauto3/src/gta3_camera.h` / `.c` | Chase-camera framing, exponential smoothing, tile-grid DDA collision. Pure maths; takes a solidity callback. |
| `games/grandthumbauto3/src/gta3_veh.h` / `.c` | Vehicle silhouette geometry (7 car shapes + bus + tank) and the `CAR_CLS` → silhouette map. |
| `games/grandthumbauto3/src/carcolors.h` | Generated: `CAR_COL[54]`, one RGB565 paint colour per car type. |
| `games/grandthumbauto3/src/test_view.c` | Host test for `gta3_view`. |
| `games/grandthumbauto3/src/test_camera.c` | Host test for `gta3_camera`. |
| `games/grandthumbauto3/src/test_veh.c` | Host test for `gta3_veh`. |
| `games/grandthumbauto3/assets/make_carcolors.py` | Bakes `carcolors.h` from `cars2.png`. |
| `games/grandthumbauto3/assets/make_chars3d.py` | Generates side-view character and scenery sprites. |

**Modified:** `games/grandthumbauto3/src/game.c` (forked), `CMakeLists.txt` (three test targets).

Why three modules rather than one: each has a different reason to change (camera feel, draw budget, vehicle art), each is independently testable, and `game.c` is already 4434 lines — the repo's own `thumbycue` splits the same way (`cue_physics`, `cue_table`, `cue_rules`).

---

### Task 1: Fork the game, unchanged

Establishes a working baseline before anything is rewritten. At the end of this task `grandthumbauto3` is a byte-identical clone that builds, runs and is still top-down.

**Files:**
- Create: `games/grandthumbauto3/` (copy of `games/grandthumbauto/`)
- Modify: `games/grandthumbauto3/src/game.c` (metadata only)

**Interfaces:**
- Consumes: nothing.
- Produces: a buildable game directory `games/grandthumbauto3` whose module name is `GrandThumbAutoIII`.

- [ ] **Step 1: Copy the game directory**

```bash
cd /Users/chris/code/hardware/mote
cp -r games/grandthumbauto games/grandthumbauto3
```

- [ ] **Step 2: Rename the module**

In `games/grandthumbauto3/src/game.c`, at the bottom of the file (around line 4433):

```c
MOTE_GAME_META("Grand Thumb Auto III", "austinio7116");
MOTE_GAME_VERSION("0.1.0");
```

Also update the file's opening comment block (lines 1-13) to describe the third-person game rather than the top-down one:

```c
/*
 * Grand Thumb Auto III — a third-person crime sandbox for Mote.
 *
 * Forked from GrandThumbAuto. The world was always real 3D geometry; what
 * changed is the camera, the culling, and how entities are drawn:
 *   · CAMERA is a chase camera behind the player (gta3_camera.c), not overhead.
 *   · VISIBILITY is a world-space frustum cone with banded draw distance
 *     (gta3_view.c), not a fixed tile window.
 *   · VEHICLES are low-poly meshes tinted per car type (gta3_veh.c); CHARACTERS,
 *     trees and pickups are camera-facing billboards. Only genuinely flat things
 *     (road markings, decals) remain ground quads.
 */
```

- [ ] **Step 3: Verify it builds and runs**

Run: `./tools/mote run games/grandthumbauto3`
Expected: the SDL emulator opens and the game plays exactly as `grandthumbauto` does — top-down, title screen, A to start.

- [ ] **Step 4: Verify the original is untouched**

Run: `git status --short games/grandthumbauto`
Expected: no output.

- [ ] **Step 5: Commit**

```bash
git add games/grandthumbauto3
git commit -m "Fork GrandThumbAuto into GrandThumbAutoIII"
```

---

### Task 2: The visibility cone

Pure maths, no engine. Replaces the screen-space `tile_visible` with a world-space cone test.

**Files:**
- Create: `games/grandthumbauto3/src/gta3_view.h`, `games/grandthumbauto3/src/gta3_view.c`
- Create: `games/grandthumbauto3/src/test_view.c`
- Modify: `CMakeLists.txt` (add `gta3_test_view` under the existing `if(NOT WIN32)` block that holds the `cue_test_*` targets, around line 215)

**Interfaces:**
- Consumes: `Vec3`, `v3_*` from `engine/math/mote_vec.h`.
- Produces:
  - `typedef struct { Vec3 eye; Vec3 fwd; float cos_half; } Gta3View;`
  - `void gta3_view_set(Gta3View *v, Vec3 eye, Vec3 fwd, float fov_deg, float aspect_slack);`
  - `int gta3_view_tile(const Gta3View *v, float wx, float wy, float wz, float radius_m, float tile_m);`

- [ ] **Step 1: Write the header**

Create `games/grandthumbauto3/src/gta3_view.h`:

```c
/*
 * Grand Thumb Auto III — world-space visibility.
 *
 * Top-down, deciding what to draw was a fixed square of tiles around the player
 * and a screen-space test per tile. Behind a car that is both too small (the
 * world would end mid-street) and too wasteful (most of the square is behind
 * you). This is the replacement: a cone from the eye along the camera forward,
 * with a per-caller distance cap so ground and buildings can use different
 * draw distances.
 *
 * Pure maths. No engine, no renderer, no globals — so it is testable on the host.
 */
#ifndef GTA3_VIEW_H
#define GTA3_VIEW_H

#include "mote_vec.h"

typedef struct {
    Vec3  eye;       /* camera position, world metres */
    Vec3  fwd;       /* camera forward, unit length */
    float cos_half;  /* cosine of the half-angle the cone accepts */
} Gta3View;

/* Build the cone for this frame.
 *
 * `fov_deg` is the engine's vertical field of view — the same value handed to
 * scene_camera(). The screen is square (128x128), so horizontal and vertical
 * are equal, and the corner of the frustum sits further out than either: the
 * half-angle is scaled by `aspect_slack` (use 1.45f, ~sqrt(2) plus margin) so
 * tiles at the screen corners are not culled. */
void gta3_view_set(Gta3View *v, Vec3 eye, Vec3 fwd, float fov_deg, float aspect_slack);

/* Is the tile centred at (wx, wy, wz) worth submitting?
 *
 * Rejects beyond `radius_m`, then outside the cone. The angular threshold is
 * slackened by the tile's own angular size (a tile of `tile_m` at that
 * distance), so a tile whose CENTRE is outside the cone but whose near corner
 * is inside still draws — without that, tiles pop at the screen edge.
 *
 * Returns 1 to draw, 0 to skip. */
int gta3_view_tile(const Gta3View *v, float wx, float wy, float wz,
                   float radius_m, float tile_m);

#endif /* GTA3_VIEW_H */
```

- [ ] **Step 2: Write the failing test**

Create `games/grandthumbauto3/src/test_view.c`:

```c
/*
 * gta3_view — the claims that matter.
 *
 * Three things have to hold or the draw windows are wrong in a way that shows
 * on screen as either holes or a blown triangle budget:
 *   1. What is in front is drawn and what is behind is not. The whole point.
 *   2. The distance cap is honoured, because that IS the draw distance.
 *   3. A tile just outside the cone at close range still draws. Its centre can
 *      be outside while half the tile is on screen; culling on the centre alone
 *      is what makes tiles blink at the screen edge.
 */
#include "gta3_view.h"

#include <math.h>
#include <stdio.h>

static int s_fail;
static void ok(int cond, const char *what) {
    if (!cond) { s_fail++; printf("  FAIL %s\n", what); }
    else printf("  ok   %s\n", what);
}

int main(void) {
    Gta3View v;
    /* eye at the origin looking down +Z, 55 degree fov */
    gta3_view_set(&v, v3(0, 3, 0), v3(0, 0, 1), 55.0f, 1.45f);

    printf("gta3_view\n");

    /* 1. front vs behind */
    ok(gta3_view_tile(&v, 0, 0, 40, 112.0f, 4.0f) == 1, "straight ahead draws");
    ok(gta3_view_tile(&v, 0, 0, -40, 112.0f, 4.0f) == 0, "directly behind culls");
    ok(gta3_view_tile(&v, 40, 0, 0, 112.0f, 4.0f) == 0, "square to the side culls");

    /* 2. the distance cap */
    ok(gta3_view_tile(&v, 0, 0, 100, 112.0f, 4.0f) == 1, "inside the cap draws");
    ok(gta3_view_tile(&v, 0, 0, 130, 112.0f, 4.0f) == 0, "beyond the cap culls");
    ok(gta3_view_tile(&v, 0, 0, 100, 52.0f, 4.0f) == 0, "a shorter cap culls the same tile");

    /* 3. edge slack: a tile at 8 m whose centre is just outside the cone.
     * At 55 deg with 1.45 slack the half-angle is ~39.9 deg, so a tile centre
     * at (7, 0, 8) sits at ~41.2 deg — outside by a degree. A 4 m tile at 8 m
     * subtends ~14 deg, so half of it is comfortably inside and it must draw. */
    ok(gta3_view_tile(&v, 7, 0, 8, 112.0f, 4.0f) == 1,
       "a near tile straddling the cone edge draws");

    /* The same angular offset far away is genuinely off screen and must cull:
     * at 80 m a 4 m tile subtends under 3 deg, so nothing rescues it. */
    ok(gta3_view_tile(&v, 70, 0, 80, 112.0f, 4.0f) == 0,
       "a far tile outside the cone culls");

    /* A degenerate tile at the eye must not divide by zero or cull the ground
     * under the player's feet. */
    ok(gta3_view_tile(&v, 0, 3, 0, 112.0f, 4.0f) == 1, "the tile at the eye draws");

    printf(s_fail ? "FAILED (%d)\n" : "passed\n", s_fail);
    return s_fail ? 1 : 0;
}
```

- [ ] **Step 3: Add the test target to CMakeLists.txt**

In `CMakeLists.txt`, inside the existing `if(NOT WIN32)` block that contains the `cue_test_*` targets (after the last one, around line 215), add:

```cmake
    # Grand Thumb Auto III: the draw-distance cone, checked without a renderer.
    # If this is wrong the city either has holes in it or blows the triangle
    # budget, and neither is diagnosable by looking at a 128x128 screen.
    add_executable(gta3_test_view
        games/grandthumbauto3/src/test_view.c
        games/grandthumbauto3/src/gta3_view.c)
    target_include_directories(gta3_test_view PRIVATE
        ${MOTE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/games/grandthumbauto3/src)
    target_compile_definitions(gta3_test_view PRIVATE MOTE_HOST=1)
    target_compile_options(gta3_test_view PRIVATE -O2 -Wall -Wextra
        -Wno-unused-parameter)
    target_link_libraries(gta3_test_view PRIVATE m)
```

- [ ] **Step 4: Create an empty implementation so the target configures**

Create `games/grandthumbauto3/src/gta3_view.c` with only the include, so CMake has a file to compile:

```c
#include "gta3_view.h"
```

- [ ] **Step 5: Run the test to verify it fails**

```bash
cd /Users/chris/code/hardware/mote
cmake -S . -B build && cmake --build build --target gta3_test_view
```

Expected: FAIL at link time — `undefined reference to 'gta3_view_set'` and `'gta3_view_tile'`.

- [ ] **Step 6: Write the implementation**

Replace `games/grandthumbauto3/src/gta3_view.c` with:

```c
#include "gta3_view.h"

#include <math.h>

void gta3_view_set(Gta3View *v, Vec3 eye, Vec3 fwd, float fov_deg, float aspect_slack) {
    v->eye = eye;
    v->fwd = v3_norm(fwd);
    float half = fov_deg * 0.5f * (3.14159265f / 180.0f) * aspect_slack;
    if (half > 1.55f) half = 1.55f;          /* never open past ~89 deg */
    v->cos_half = cosf(half);
}

int gta3_view_tile(const Gta3View *v, float wx, float wy, float wz,
                   float radius_m, float tile_m) {
    float dx = wx - v->eye.x, dy = wy - v->eye.y, dz = wz - v->eye.z;
    float d2 = dx*dx + dy*dy + dz*dz;
    if (d2 > radius_m * radius_m) return 0;

    /* At or inside a tile's own half-width there is no meaningful direction to
     * test, and this is the ground under the player. Always draw it. */
    float half_tile = tile_m * 0.5f;
    if (d2 <= half_tile * half_tile) return 1;

    float d = sqrtf(d2);
    float cosang = (dx * v->fwd.x + dy * v->fwd.y + dz * v->fwd.z) / d;

    /* Slacken by the tile's angular half-size at this distance: a tile whose
     * centre is outside the cone can still have its near corner on screen.
     * sin(slack) = half_tile / d, and widening the cone by that angle means
     * comparing against cos(half + slack), expanded here rather than via a
     * second acosf/cosf pair. */
    float sin_slack = half_tile / d;
    if (sin_slack > 1.0f) sin_slack = 1.0f;
    float cos_slack = sqrtf(1.0f - sin_slack * sin_slack);
    float sin_half = sqrtf(1.0f - v->cos_half * v->cos_half);
    float cos_widened = v->cos_half * cos_slack - sin_half * sin_slack;

    return cosang >= cos_widened;
}
```

- [ ] **Step 7: Run the test to verify it passes**

```bash
cmake --build build --target gta3_test_view && ./build/gta3_test_view
```

Expected: every line `ok`, final line `passed`, exit code 0.

- [ ] **Step 8: Commit**

```bash
git add games/grandthumbauto3/src/gta3_view.h games/grandthumbauto3/src/gta3_view.c \
        games/grandthumbauto3/src/test_view.c CMakeLists.txt
git commit -m "A world-space visibility cone for the third-person draw windows"
```

---

### Task 3: The chase camera

Pure maths, no engine. The framing, the smoothing, and the collision that stops the eye entering a facade.

**Files:**
- Create: `games/grandthumbauto3/src/gta3_camera.h`, `games/grandthumbauto3/src/gta3_camera.c`
- Create: `games/grandthumbauto3/src/test_camera.c`
- Modify: `CMakeLists.txt` (add `gta3_test_camera`)

**Interfaces:**
- Consumes: `Vec3`, `v3_*` from `engine/math/mote_vec.h`.
- Produces:
  - `typedef struct { Vec3 eye; Vec3 target; float yaw; int started; } Gta3Cam;`
  - `typedef int (*Gta3SolidFn)(int tx, int tz, void *ud);`
  - `void gta3_cam_reset(Gta3Cam *c);`
  - `float gta3_wrap_angle(float a);`
  - `void gta3_cam_update(Gta3Cam *c, float anchor_x, float anchor_z, float facing_yaw, float dist, float height, float look, float dt, Gta3SolidFn solid, void *ud, float tile_m);`

- [ ] **Step 1: Write the header**

Create `games/grandthumbauto3/src/gta3_camera.h`:

```c
/*
 * Grand Thumb Auto III — the chase camera.
 *
 * Three jobs, and each exists for a reason that shows immediately if it is
 * skipped:
 *
 *  · FRAMING — eye behind and above the anchor, target ahead of it. The look
 *    ahead distance is what keeps the road, rather than the roof of your own
 *    car, in the middle of a 128x128 screen.
 *
 *  · SMOOTHING — position and yaw are smoothed at DIFFERENT rates, yaw slower.
 *    Equal rates make corners snap; a slower yaw is what makes the camera swing
 *    through a turn. Yaw follows the vehicle's HEADING, never its velocity, so
 *    reversing does not spin the camera around.
 *
 *  · COLLISION — a DDA along the tile grid from the anchor back to the wanted
 *    eye. In a grid city, every corner would otherwise put the camera inside a
 *    facade and show you the inside of a building. This is not optional.
 *
 * Pure maths: the tile map arrives as a callback, so this is testable on the
 * host with a hand-written map and no engine.
 */
#ifndef GTA3_CAMERA_H
#define GTA3_CAMERA_H

#include "mote_vec.h"

typedef struct {
    Vec3  eye;      /* smoothed camera position, world metres */
    Vec3  target;   /* smoothed look-at point, world metres */
    float yaw;      /* smoothed camera yaw, radians */
    int   started;  /* 0 until the first update snaps instead of smoothing */
} Gta3Cam;

/* Returns non-zero if the tile at (tx, tz) blocks the camera. */
typedef int (*Gta3SolidFn)(int tx, int tz, void *ud);

/* Forget the smoothed state. Call on respawn, on entering or leaving a car, and
 * at the start of a game — otherwise the camera sails across the map from
 * wherever it was. */
void gta3_cam_reset(Gta3Cam *c);

/* Wrap to (-pi, pi]. Exposed because yaw smoothing must go the short way round
 * and callers need the same wrap for their own heading maths. */
float gta3_wrap_angle(float a);

/* Advance the camera one frame.
 *
 * `anchor_x`/`anchor_z` are the player or vehicle position; `facing_yaw` is its
 * heading. `dist`, `height` and `look` are the framing for the current state
 * (on foot or in a car at this speed). `solid`/`ud` answer which tiles block,
 * and `tile_m` is the world size of one tile. */
void gta3_cam_update(Gta3Cam *c, float anchor_x, float anchor_z, float facing_yaw,
                     float dist, float height, float look, float dt,
                     Gta3SolidFn solid, void *ud, float tile_m);

/* Smoothing rates, per second. Position is faster than yaw on purpose. */
#define GTA3_CAM_POS_K   8.0f
#define GTA3_CAM_YAW_K   4.0f
/* The camera never comes closer to the anchor than this, whatever it hits. */
#define GTA3_CAM_MIN_D   1.8f
/* Eye height above the anchor's ground position, for the collision ray. */
#define GTA3_CAM_EYE_Y   1.2f

#endif /* GTA3_CAMERA_H */
```

- [ ] **Step 2: Write the failing test**

Create `games/grandthumbauto3/src/test_camera.c`:

```c
/*
 * gta3_camera — the claims that matter.
 *
 * The camera is the port. If it is wrong the game is unplayable in ways that
 * are hard to see frame by frame, so each claim is checked directly:
 *
 *  1. It sits BEHIND the anchor and looks PAST it. Fail this and you are
 *     staring at your own roof.
 *  2. Reversing does not flip it. A camera that whips round every time you back
 *     out of a parking space is the single most common way to get this wrong.
 *  3. Yaw takes the SHORT way round -pi/+pi. Fail this and the camera does a
 *     full lazy spin every time you drive north.
 *  4. It does not enter a building. Checked against a real wall.
 *  5. It converges. Smoothing that never settles reads as drift.
 */
#include "gta3_camera.h"

#include <math.h>
#include <stdio.h>

static int s_fail;
static void ok(int cond, const char *what) {
    if (!cond) { s_fail++; printf("  FAIL %s\n", what); }
    else printf("  ok   %s\n", what);
}

#define TILE 4.0f

/* An open world: nothing blocks. */
static int solid_none(int tx, int tz, void *ud) { (void)tx; (void)tz; (void)ud; return 0; }

/* A wall: every tile with tz <= -1 is solid. With the anchor at the origin and
 * the camera pushed toward -Z, the eye wants to be inside it. */
static int solid_wall(int tx, int tz, void *ud) { (void)tx; (void)ud; return tz <= -1; }

/* Settle the camera by running enough frames that smoothing has converged. */
static void settle(Gta3Cam *c, float ax, float az, float yaw,
                   Gta3SolidFn solid, int frames) {
    for (int i = 0; i < frames; i++)
        gta3_cam_update(c, ax, az, yaw, 7.0f, 3.0f, 5.0f, 1.0f / 60.0f, solid, 0, TILE);
}

int main(void) {
    printf("gta3_camera\n");

    /* 1. behind and looking past. Facing yaw 0 is +X in this game's convention
     * (fwd = cosf(yaw), sinf(yaw)), so the eye must end up at negative X. */
    {
        Gta3Cam c; gta3_cam_reset(&c);
        settle(&c, 0, 0, 0.0f, solid_none, 240);
        ok(c.eye.x < -6.0f && c.eye.x > -8.0f, "eye sits behind the anchor");
        ok(c.eye.y > 2.5f && c.eye.y < 3.5f, "eye sits above the anchor");
        ok(c.target.x > 4.0f, "target is ahead of the anchor");
        ok(fabsf(c.eye.z) < 0.01f && fabsf(c.target.z) < 0.01f, "no lateral drift");
    }

    /* 2. reversing does not flip. The heading is unchanged while the car backs
     * up, so the camera must stay on the same side. */
    {
        Gta3Cam c; gta3_cam_reset(&c);
        settle(&c, 0, 0, 0.0f, solid_none, 240);
        float was = c.eye.x;
        for (int i = 0; i < 120; i++)   /* anchor slides backward, heading held */
            gta3_cam_update(&c, -i * 0.05f, 0, 0.0f, 7.0f, 3.0f, 5.0f,
                            1.0f / 60.0f, solid_none, 0, TILE);
        ok(c.eye.x < was, "reversing keeps the camera behind the nose, not ahead");
    }

    /* 3. the short way round. Snap the heading from just under +pi to just over
     * -pi: that is a 0.2 rad turn, and the camera must not travel 6.1 rad. */
    {
        Gta3Cam c; gta3_cam_reset(&c);
        settle(&c, 0, 0, 3.04f, solid_none, 240);
        float start = c.yaw;
        gta3_cam_update(&c, 0, 0, -3.04f, 7.0f, 3.0f, 5.0f, 1.0f / 60.0f,
                        solid_none, 0, TILE);
        float step = fabsf(gta3_wrap_angle(c.yaw - start));
        ok(step < 0.1f, "yaw crosses the pi boundary the short way");
    }
    ok(fabsf(gta3_wrap_angle(3.0f * 3.14159265f)) < 3.15f, "wrap_angle folds 3pi");
    ok(fabsf(gta3_wrap_angle(0.5f) - 0.5f) < 1e-5f, "wrap_angle leaves 0.5 alone");

    /* 4. the wall. The camera wants to be 7 m toward -Z, which is inside solid
     * tiles from z = -4 m. It must stop short, and not closer than the floor. */
    {
        Gta3Cam c; gta3_cam_reset(&c);
        /* facing +Z (yaw = pi/2 puts fwd at +Z), so the eye is pushed to -Z */
        settle(&c, 0, 0, 1.5708f, solid_wall, 240);
        ok(c.eye.z > -4.0f, "the eye stops short of the wall");
        float d = sqrtf(c.eye.x * c.eye.x + c.eye.z * c.eye.z);
        ok(d >= GTA3_CAM_MIN_D - 0.01f, "the eye never comes closer than the floor");
    }

    /* 5. convergence: two settled runs from different starts must agree. */
    {
        Gta3Cam a, b; gta3_cam_reset(&a); gta3_cam_reset(&b);
        settle(&a, 20, 20, 0.7f, solid_none, 400);
        gta3_cam_reset(&b);
        settle(&b, 20, 20, 0.7f, solid_none, 400);
        ok(fabsf(a.eye.x - b.eye.x) < 1e-3f && fabsf(a.eye.z - b.eye.z) < 1e-3f,
           "smoothing converges to one answer");
    }

    printf(s_fail ? "FAILED (%d)\n" : "passed\n", s_fail);
    return s_fail ? 1 : 0;
}
```

- [ ] **Step 3: Add the test target to CMakeLists.txt**

Next to `gta3_test_view`:

```cmake
    # Grand Thumb Auto III: the chase camera. Behind-not-in-front, no flip on
    # reverse, the short way round pi, and never inside a building — none of
    # which are safe to eyeball on a 128x128 screen.
    add_executable(gta3_test_camera
        games/grandthumbauto3/src/test_camera.c
        games/grandthumbauto3/src/gta3_camera.c)
    target_include_directories(gta3_test_camera PRIVATE
        ${MOTE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/games/grandthumbauto3/src)
    target_compile_definitions(gta3_test_camera PRIVATE MOTE_HOST=1)
    target_compile_options(gta3_test_camera PRIVATE -O2 -Wall -Wextra
        -Wno-unused-parameter)
    target_link_libraries(gta3_test_camera PRIVATE m)
```

- [ ] **Step 4: Create an empty implementation and run the test to verify it fails**

Create `games/grandthumbauto3/src/gta3_camera.c` containing only `#include "gta3_camera.h"`, then:

```bash
cmake -S . -B build && cmake --build build --target gta3_test_camera
```

Expected: FAIL at link time — `undefined reference to 'gta3_cam_reset'`.

- [ ] **Step 5: Write the implementation**

Replace `games/grandthumbauto3/src/gta3_camera.c` with:

```c
#include "gta3_camera.h"

#include <math.h>

void gta3_cam_reset(Gta3Cam *c) {
    c->eye = v3(0, 0, 0);
    c->target = v3(0, 0, 0);
    c->yaw = 0.0f;
    c->started = 0;
}

float gta3_wrap_angle(float a) {
    const float TAU = 6.2831853f;
    while (a >  3.14159265f) a -= TAU;
    while (a <= -3.14159265f) a += TAU;
    return a;
}

/* March the tile grid from the anchor toward the wanted eye and report how far
 * we may travel before hitting something solid. Steps at a quarter tile: the
 * grid is 4 m and buildings are full tiles, so this cannot skip a wall, and at
 * a maximum of ~10 m of travel it is at most 40 samples. */
static float clear_run(float ax, float az, float dx, float dz, float want,
                       Gta3SolidFn solid, void *ud, float tile_m) {
    if (!solid) return want;
    float step = tile_m * 0.25f;
    for (float t = step; t <= want; t += step) {
        int tx = (int)floorf((ax + dx * t) / tile_m);
        int tz = (int)floorf((az + dz * t) / tile_m);
        if (solid(tx, tz, ud)) {
            float back = t - step * 0.5f;      /* stop just short of the face */
            return back < 0.0f ? 0.0f : back;
        }
    }
    return want;
}

void gta3_cam_update(Gta3Cam *c, float anchor_x, float anchor_z, float facing_yaw,
                     float dist, float height, float look, float dt,
                     Gta3SolidFn solid, void *ud, float tile_m) {
    /* Yaw is smoothed, not the eye vector, so the camera swings through corners
     * rather than sliding sideways through them. */
    if (!c->started) {
        c->yaw = facing_yaw;
    } else {
        float d = gta3_wrap_angle(facing_yaw - c->yaw);
        float ky = 1.0f - expf(-GTA3_CAM_YAW_K * dt);
        c->yaw = gta3_wrap_angle(c->yaw + d * ky);
    }

    float fx = cosf(c->yaw), fz = sinf(c->yaw);

    /* How far back may we actually sit? Cast from the anchor along -fwd. */
    float run = clear_run(anchor_x, anchor_z, -fx, -fz, dist, solid, ud, tile_m);
    if (run < GTA3_CAM_MIN_D) run = GTA3_CAM_MIN_D;

    Vec3 want_eye = v3(anchor_x - fx * run, height, anchor_z - fz * run);
    Vec3 want_tgt = v3(anchor_x + fx * look, GTA3_CAM_EYE_Y, anchor_z + fz * look);

    if (!c->started) {
        c->eye = want_eye;
        c->target = want_tgt;
        c->started = 1;
        return;
    }

    float kp = 1.0f - expf(-GTA3_CAM_POS_K * dt);
    c->eye = v3_lerp(c->eye, want_eye, kp);
    c->target = v3_lerp(c->target, want_tgt, kp);
}
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build build --target gta3_test_camera && ./build/gta3_test_camera
```

Expected: every line `ok`, final line `passed`, exit code 0.

- [ ] **Step 7: Commit**

```bash
git add games/grandthumbauto3/src/gta3_camera.h games/grandthumbauto3/src/gta3_camera.c \
        games/grandthumbauto3/src/test_camera.c CMakeLists.txt
git commit -m "A chase camera with collision, smoothed yaw and no flip on reverse"
```

---

### Task 4: Put the camera in the game

First playable third-person build. Everything still draws as it did — entities are still flat ground sprites and will look wrong from behind. That is expected and fixed in Tasks 7-9. What this task proves is that the camera, the projection and the existing culling still agree.

**Files:**
- Modify: `games/grandthumbauto3/src/game.c:422-440` (camera constants and `set_topdown_camera`), `:3605-3606`, `:3772-3773`, `:3963-3974` (the three camera call sites)

**Interfaces:**
- Consumes: `Gta3Cam`, `gta3_cam_reset`, `gta3_cam_update` from Task 3.
- Produces: file-scope `static Gta3Cam g_cam;` and `static void chase_camera(float tx, float tz, float yaw, float dt);`, which set the existing `cam_basis`, `cam_pos`, `view_x`, `view_z` that the rest of `game.c` and `world_to_screen` already read.

- [ ] **Step 1: Replace the camera constants**

In `games/grandthumbauto3/src/game.c`, replace lines 422-425:

```c
#define FOV       60.0f
#define CAM_FOOT  15.0f      /* tight zoom on foot */
#define CAM_CAR   21.0f      /* zoom when stopped in a car */
#define CAM_MAX   40.0f      /* pulled right out at top speed */
```

with:

```c
/* 55, not 60: a 60 degree horizontal field behind a car reads as fisheye on a
 * 128x128 panel, and every extra degree is more city inside the draw cone. */
#define FOV       55.0f
/* Chase framing, metres. The car values interpolate on speed over 0..CAM_SPD. */
#define CAM_FOOT_D   4.5f
#define CAM_FOOT_H   2.4f
#define CAM_FOOT_L   3.0f
#define CAM_CAR_D0   6.5f
#define CAM_CAR_H0   2.8f
#define CAM_CAR_L0   4.0f
#define CAM_CAR_D1  10.0f
#define CAM_CAR_H1   3.6f
#define CAM_CAR_L1   9.0f
#define CAM_SPD     18.0f    /* speed, m/s, at which the framing is fully out */
```

- [ ] **Step 2: Add the include**

After the existing includes near the top of the file (after `#include "citygen.h"`, around line 47):

```c
#include "gta3_camera.h"
#include "gta3_view.h"
```

- [ ] **Step 3: Replace set_topdown_camera**

Replace `set_topdown_camera` (game.c:433-440) with:

```c
static Gta3Cam g_cam;
static Gta3View g_view;

/* The camera's tile-solidity test. Buildings block; bridges, roads and water do
 * not — driving a bridge with the camera snapping to the deck would be worse
 * than letting it fly. */
static int cam_solid(int tx, int tz, void *ud) {
    (void)ud;
    char c = tile_at(tx, tz);
    return c == '#' || c == 'O' || c == 'H';
}

/* Place the chase camera and publish everything the rest of the file reads:
 * cam_basis + cam_pos for the engine and world_to_screen, view_x/view_z as the
 * centre the draw windows iterate around, and g_view as this frame's cone. */
static void chase_camera(float tx, float tz, float yaw, float dt) {
    float dist = CAM_FOOT_D, height = CAM_FOOT_H, look = CAM_FOOT_L;
    if (player.mode == MODE_CAR) {
        float s = fabsf(cars[player.car].spd) / CAM_SPD;
        if (s > 1.0f) s = 1.0f;
        dist   = CAM_CAR_D0 + (CAM_CAR_D1 - CAM_CAR_D0) * s;
        height = CAM_CAR_H0 + (CAM_CAR_H1 - CAM_CAR_H0) * s;
        look   = CAM_CAR_L0 + (CAM_CAR_L1 - CAM_CAR_L0) * s;
    }
    if (g_lookback) { dist *= 0.8f; yaw += 3.14159265f; }

    gta3_cam_update(&g_cam, tx, tz, yaw, dist, height, look, dt,
                    cam_solid, 0, TILE);

    cam_pos   = g_cam.eye;
    cam_basis = mote_camera_look(g_cam.eye, g_cam.target);
    /* The draw windows iterate tiles around a centre. Centre them on a point
     * ahead of the camera, not on the camera itself: almost everything drawn is
     * in front, so a window centred on the eye wastes half its span behind. */
    view_x = g_cam.eye.x + cam_basis.r[2].x * 40.0f;
    view_z = g_cam.eye.z + cam_basis.r[2].z * 40.0f;

    gta3_view_set(&g_view, g_cam.eye, cam_basis.r[2], FOV, 1.45f);
}
```

Delete the now-unused `static float g_camh` declaration at line 431.

- [ ] **Step 4: Add the look-behind flag**

Next to the other input-derived file-scope flags (near `g_showmap` at game.c:947):

```c
static int g_lookback;      /* LB held on foot: swing the camera round */
```

- [ ] **Step 5: Replace the three camera call sites**

At game.c:3605-3606 (title and death screens), replace:

```c
        set_topdown_camera(pl_x(), pl_z());
        mote->scene_camera(&cam_basis, cam_pos, FOV);
```

with:

```c
        chase_camera(pl_x(), pl_z(), pl_yaw(), dt);
        mote->scene_camera(&cam_basis, cam_pos, FOV);
```

At game.c:3772-3773 (deathmatch hold), make the identical replacement.

At game.c:3963-3974, delete the `ztarget` / `g_camh` zoom block but **keep the engine-pitch lines inside it**, then replace the camera call. The block becomes:

```c
    /* engine pitch rides speed: quiet on foot, loud with speed */
    if (player.mode==MODE_CAR){ float s=fabsf(cars[player.car].spd);
        g_eng_f = 30.0f + s*5.0f;
        float at = 0.11f + s*0.012f; if(at>0.30f)at=0.30f; g_eng_a += (at-g_eng_a)*0.3f;
    } else g_eng_a *= 0.85f;

    float tx = (player.mode==MODE_CAR)? cars[player.car].x : player.x;
    float tz = (player.mode==MODE_CAR)? cars[player.car].z : player.z;
    float tyaw = (player.mode==MODE_CAR)? cars[player.car].yaw : player.yaw;
    chase_camera(tx, tz, tyaw, dt);
    mote->scene_camera(&cam_basis, cam_pos, FOV);
```

- [ ] **Step 6: Add a helper for the player's heading**

Next to the existing `pl_x()` / `pl_z()` helpers, add:

```c
static float pl_yaw(void){ return player.mode==MODE_CAR ? cars[player.car].yaw : player.yaw; }
```

- [ ] **Step 7: Reset the camera on state changes**

Call `gta3_cam_reset(&g_cam);` in `reset_game()`, and at every point where the player enters or leaves a vehicle or respawns — otherwise the camera sails across the map from where it was. Search for `player.mode = MODE_CAR` and `player.mode = MODE_FOOT` and add the reset after each, plus in `dm_respawn_me`.

- [ ] **Step 8: Widen the draw windows so the world does not end mid-street**

This is a temporary measure — Task 5 replaces these windows entirely. For now, in `draw_ground_window` (game.c:1272) change the loop bounds from `12` to `16`, and in `draw_buildings_window` (game.c:1297) from `14` to `22`, so the first playable build is not looking at the edge of the world.

- [ ] **Step 9: Build and run**

```bash
cd /Users/chris/code/hardware/mote
./tools/mote run games/grandthumbauto3
```

Expected, verified by eye: the camera sits behind the player and swings through corners; buildings are seen from the side with facades visible, not from above; reversing does not flip the view; driving a corner does not put the camera inside a building. Cars and people look wrong — they are still flat top-down sprites lying on the road. That is expected at this task.

- [ ] **Step 10: Commit**

```bash
git add games/grandthumbauto3/src/game.c
git commit -m "Put the chase camera behind the player"
```

---

### Task 5: Cone culling and the haze band

Replaces the fixed tile windows with the Task 2 cone, and adds the untextured far band.

**Files:**
- Modify: `games/grandthumbauto3/src/game.c:1084-1090` (`tile_visible`), `:1272-1296` (`draw_ground_window`), `:1297-1321` (`draw_buildings_window`), `:307-327` (`build_buildings`)

**Interfaces:**
- Consumes: `g_view`, `gta3_view_tile` from Tasks 2 and 4.
- Produces: `static Mesh g_hmesh[NBLV];` — untextured haze meshes sharing `g_bv[L]` / `g_bf[L]`.

- [ ] **Step 1: Add the band constants**

Next to the camera constants added in Task 4:

```c
/* Draw distances, metres. Ground is shorter than buildings on purpose: past
 * ~50 m the road is near edge-on and mostly hidden by facades anyway. */
#define VIEW_GROUND_R   52.0f
#define VIEW_BLD_R     112.0f
#define VIEW_HAZE_R     70.0f    /* beyond this, buildings go untextured + tinted */
#define VIEW_BLD_CAP     110     /* max building submissions per frame */
```

- [ ] **Step 2: Build the haze meshes**

In `build_buildings` (game.c:307), after the existing `g_bmesh` loop, add:

```c
    /* The far band draws UNTEXTURED, so it can be tinted toward the background
     * and so it costs max_tris rather than max_tex_tris. A per-draw colour does
     * NOT suppress a texture — mote_pipe.c picks the textured path purely on
     * (mesh->texture && mesh->face_uvs), and obj->color only feeds the flat
     * path. Hence a parallel mesh with no texture, sharing the same geometry. */
    for (int L=0; L<NBLV; L++)
        g_hmesh[L] = (Mesh){ .verts=g_bv[L], .faces=g_bf[L], .nverts=8, .nfaces=12,
                             .scale=g_bmd[L], .bound_r=g_bmd[L]*1.75f,
                             .texture=0, .face_uvs=0, .color=MOTE_RGB565(60,64,78) };
```

and declare `static Mesh g_hmesh[NBLV];` next to `g_bmesh` (game.c:305).

- [ ] **Step 3: Add the haze tint helper**

Above `draw_buildings_window`:

```c
/* Blend a building's tone toward the background over the far band, so the draw
 * distance reads as haze instead of a wall of nothing. t = 0 at VIEW_HAZE_R,
 * 1 at VIEW_BLD_R. */
static uint16_t haze_tint(float d) {
    float t = (d - VIEW_HAZE_R) / (VIEW_BLD_R - VIEW_HAZE_R);
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    int r = (int)(60 + (24 - 60) * t);
    int g = (int)(64 + (26 - 64) * t);
    int b = (int)(78 + (32 - 78) * t);
    return MOTE_RGB565(r, g, b);
}
```

The background is `MOTE_RGB565(24, 26, 32)`, set in `g_init` (game.c:1069).

- [ ] **Step 4: Replace tile_visible**

Replace `tile_visible` (game.c:1084-1090) with:

```c
/* World-space cone test for a tile centre. Replaces the old screen-space test,
 * which was written for a camera pointing straight down. */
static int tile_visible_r(int x, int z, float y, float radius) {
    return gta3_view_tile(&g_view, x*TILE+TILE*0.5f, y, z*TILE+TILE*0.5f,
                          radius, TILE);
}
```

- [ ] **Step 5: Rewrite draw_ground_window**

Replace the loop bounds and the visibility call in `draw_ground_window` (game.c:1272-1275):

```c
static void draw_ground_window(void) {
    int cx = (int)(view_x / TILE), cz = (int)(view_z / TILE);
    int w = (int)(VIEW_GROUND_R / TILE) + 1;
    for (int z = cz - w; z <= cz + w; z++) {
        for (int x = cx - w; x <= cx + w; x++) {
            if (!tile_visible_r(x, z, 0, VIEW_GROUND_R)) continue;
```

The rest of the function body is unchanged.

- [ ] **Step 6: Rewrite draw_buildings_window**

Replace `draw_buildings_window` (game.c:1297-1321) with:

```c
static void draw_buildings_window(void) {
    int cx = (int)(view_x / TILE), cz = (int)(view_z / TILE);
    int w = (int)(VIEW_BLD_R / TILE) + 1;
    int submitted = 0;
    /* Two passes: the near, textured band first, then the far haze band until
     * the cap. Overflow then shows as distant blocks missing rather than as
     * whatever arrived last being dropped. */
    for (int pass = 0; pass < 2; pass++) {
        for (int z = cz - w; z <= cz + w; z++)
            for (int x = cx - w; x <= cx + w; x++) {
                char c = tile_at(x, z);
                if (c != '#' && c != 'O' && c != 'H') continue;
                int gdir = is_garage(x, z) ? garage_dir(x, z) : -1;
                float th, hy;
                if (gdir >= 0){ th = GARAGE_H; hy = th * 0.5f; }
                else { int L = bld_level(x, z); th = g_lvl_h[L]; hy = th * 0.5f; }
                float wx = x*TILE+TILE*0.5f, wz = z*TILE+TILE*0.5f;
                float ddx = wx - cam_pos.x, ddz = wz - cam_pos.z;
                float d = sqrtf(ddx*ddx + ddz*ddz);
                int far = d > VIEW_HAZE_R;
                if (far != pass) continue;
                if (submitted >= VIEW_BLD_CAP) return;
                /* Base OR top in the cone: culling on the elevated centre alone
                 * made tall blocks blink out while their footprint was still
                 * plainly in view. */
                if (!gta3_view_tile(&g_view, wx, 0.1f, wz, VIEW_BLD_R, TILE) &&
                    !gta3_view_tile(&g_view, wx, th,   wz, VIEW_BLD_R, TILE)) continue;
                if (gdir >= 0) { mote_draw(mote, &gr_mesh[gdir], v3(wx, hy, wz)); }
                else if (far) {
                    MoteObject o = { .pos = v3(wx, hy, wz), .basis = m3_identity(),
                                     .mesh = &g_hmesh[bld_level(x,z)],
                                     .color = haze_tint(d) };
                    mote->scene_add_object(&o);
                } else {
                    mote_draw(mote, &g_bmesh[bld_level(x,z)][bld_tex(x,z)], v3(wx, hy, wz));
                }
                submitted++;
            }
    }
}
```

- [ ] **Step 7: Build and run**

```bash
./tools/mote run games/grandthumbauto3
```

Expected: the city extends far down the street instead of ending abruptly; distant blocks are flat and tinted toward the background rather than textured; no tile pops at the screen edge as you turn; nothing is visibly missing beside or ahead of you.

- [ ] **Step 8: Commit**

```bash
git add games/grandthumbauto3/src/game.c
git commit -m "Cone culling with a banded draw distance and an untextured haze band"
```

---

### Task 6: Vehicle silhouette meshes

Pure geometry. The 19 `CAR_CLS` classes collapse to 7 shapes, built as a body box plus a cabin box.

**Files:**
- Create: `games/grandthumbauto3/src/gta3_veh.h`, `games/grandthumbauto3/src/gta3_veh.c`
- Create: `games/grandthumbauto3/src/test_veh.c`
- Modify: `CMakeLists.txt` (add `gta3_test_veh`)

**Interfaces:**
- Consumes: `Mesh`, `MeshVert`, `MeshFace` from `engine/render/mote_mesh.h`; `mote__face` from `sdk/mote_build.h`.
- Produces:
  - `enum { GTA3_SIL_SEDAN, GTA3_SIL_COMPACT, GTA3_SIL_WEDGE, GTA3_SIL_LONGHOOD, GTA3_SIL_WAGON, GTA3_SIL_VAN, GTA3_SIL_TRUCK, GTA3_SIL_N };`
  - `int gta3_sil_for_class(int car_cls);`
  - `typedef struct { MeshVert bv[8]; MeshFace bf[12]; MeshVert cv[8]; MeshFace cf[12]; Mesh body; Mesh cabin; } Gta3VehMesh;`
  - `void gta3_veh_build(Gta3VehMesh *m, int sil);`

- [ ] **Step 1: Write the header**

Create `games/grandthumbauto3/src/gta3_veh.h`:

```c
/*
 * Grand Thumb Auto III — vehicle silhouettes.
 *
 * The game has 54 car types. Authoring 54 meshes would be absurd and shipping
 * 54 sprite sheets of 8 yaw angles each would not fit in flash, so identity
 * comes from COLOUR and the geometry collapses to seven shapes: the 19
 * CAR_CLS handling classes map onto a sedan, a compact, a low wedge, a long
 * hood, a wagon, a van and a truck.
 *
 * Each vehicle is two boxes — a body and a cabin — drawn as two objects with
 * separate per-draw colours, because MoteObject.color is a single colour for
 * the whole object and a car with no windows reads as a brick.
 *
 * No wheels. Four more boxes is 48 triangles per car for something two pixels
 * tall on a 128x128 screen.
 *
 * Dimensions are normalised: the body spans +/-1 in each axis before the Mesh
 * scale is applied, so the caller sizes each car from its own measured art
 * (cars2_meta.h) and handling stays exactly as it was tuned.
 */
#ifndef GTA3_VEH_H
#define GTA3_VEH_H

#include "mote_mesh.h"

enum { GTA3_SIL_SEDAN, GTA3_SIL_COMPACT, GTA3_SIL_WEDGE, GTA3_SIL_LONGHOOD,
       GTA3_SIL_WAGON, GTA3_SIL_VAN, GTA3_SIL_TRUCK, GTA3_SIL_N };

/* Map one of the 19 CAR_CLS handling classes to a silhouette. Out-of-range
 * input returns GTA3_SIL_SEDAN rather than reading off the end. */
int gta3_sil_for_class(int car_cls);

typedef struct {
    MeshVert bv[8]; MeshFace bf[12];    /* body box */
    MeshVert cv[8]; MeshFace cf[12];    /* cabin box */
    Mesh body, cabin;
} Gta3VehMesh;

/* Build both boxes for a silhouette. Local axes: +Z along the heading (nose),
 * +X to the right, +Y up. The body sits on y=0 and both meshes carry
 * .scale = 1, so the caller draws with scene_add_object_scaled or sets .scale
 * to half the car's longest dimension. */
void gta3_veh_build(Gta3VehMesh *m, int sil);

#endif /* GTA3_VEH_H */
```

- [ ] **Step 2: Write the failing test**

Create `games/grandthumbauto3/src/test_veh.c`:

```c
/*
 * gta3_veh — the claims that matter.
 *
 * A mesh with inward normals is invisible (the engine backface-culls), and a
 * mesh whose cabin sits inside the body is a brick. Neither is obvious at 128x128
 * with a car six pixels long, so both are checked here.
 *
 *  1. Every CAR_CLS maps to a real silhouette, including out-of-range input.
 *  2. Every silhouette is a closed box pair: 12 faces each, normals outward.
 *  3. The cabin sits ON the body, not inside it, and is narrower than it.
 *  4. The silhouettes are actually different shapes.
 */
#include "gta3_veh.h"

#include <math.h>
#include <stdio.h>

static int s_fail;
static void ok(int cond, const char *what) {
    if (!cond) { s_fail++; printf("  FAIL %s\n", what); }
    else printf("  ok   %s\n", what);
}

/* A face's normal points outward if it points away from the box's centre. */
static int normals_outward(const MeshVert *v, const MeshFace *f, int nf) {
    float cx=0, cy=0, cz=0;
    for (int i = 0; i < 8; i++) { cx += v[i].x; cy += v[i].y; cz += v[i].z; }
    cx /= 8.0f; cy /= 8.0f; cz /= 8.0f;
    for (int i = 0; i < nf; i++) {
        float mx = (v[f[i].a].x + v[f[i].b].x + v[f[i].c].x) / 3.0f - cx;
        float my = (v[f[i].a].y + v[f[i].b].y + v[f[i].c].y) / 3.0f - cy;
        float mz = (v[f[i].a].z + v[f[i].b].z + v[f[i].c].z) / 3.0f - cz;
        if (mx*f[i].nx + my*f[i].ny + mz*f[i].nz <= 0.0f) return 0;
    }
    return 1;
}

static void span(const MeshVert *v, float *lo, float *hi, int axis) {
    *lo = 1e9f; *hi = -1e9f;
    for (int i = 0; i < 8; i++) {
        float c = axis == 0 ? v[i].x : axis == 1 ? v[i].y : v[i].z;
        if (c < *lo) *lo = c;
        if (c > *hi) *hi = c;
    }
}

int main(void) {
    printf("gta3_veh\n");

    /* 1. the class map */
    int seen[GTA3_SIL_N] = {0};
    for (int c = 0; c < 19; c++) {
        int s = gta3_sil_for_class(c);
        if (s < 0 || s >= GTA3_SIL_N) { s_fail++; printf("  FAIL class %d maps out of range\n", c); }
        else seen[s] = 1;
    }
    printf("  ok   all 19 classes map in range\n");
    ok(gta3_sil_for_class(-1) == GTA3_SIL_SEDAN, "negative class falls back to sedan");
    ok(gta3_sil_for_class(999) == GTA3_SIL_SEDAN, "huge class falls back to sedan");
    {
        int all = 1;
        for (int s = 0; s < GTA3_SIL_N; s++) if (!seen[s]) all = 0;
        ok(all, "every silhouette is reachable from some class");
    }

    /* 2, 3, 4 */
    float zlen[GTA3_SIL_N], yhgt[GTA3_SIL_N];
    for (int s = 0; s < GTA3_SIL_N; s++) {
        Gta3VehMesh m;
        gta3_veh_build(&m, s);

        ok(m.body.nfaces == 12 && m.cabin.nfaces == 12, "both boxes have 12 faces");
        ok(normals_outward(m.bv, m.bf, 12), "body normals point outward");
        ok(normals_outward(m.cv, m.cf, 12), "cabin normals point outward");

        float blo, bhi, clo, chi;
        span(m.bv, &blo, &bhi, 1);            /* body Y */
        span(m.cv, &clo, &chi, 1);            /* cabin Y */
        ok(clo >= bhi - 1.0f, "the cabin sits on top of the body, not inside it");
        ok(chi > bhi, "the cabin is the tallest part of the car");

        float bxl, bxh, cxl, cxh;
        span(m.bv, &bxl, &bxh, 0);
        span(m.cv, &cxl, &cxh, 0);
        ok((cxh - cxl) <= (bxh - bxl), "the cabin is no wider than the body");

        float zl, zh, yl, yh;
        span(m.bv, &zl, &zh, 2); zlen[s] = zh - zl;
        span(m.cv, &yl, &yh, 1); yhgt[s] = yh;
    }

    /* 4. a van and a wedge must not be the same box */
    ok(fabsf(yhgt[GTA3_SIL_VAN] - yhgt[GTA3_SIL_WEDGE]) > 8.0f,
       "a van is visibly taller than a low wedge");
    ok(zlen[GTA3_SIL_LONGHOOD] > 0.0f && zlen[GTA3_SIL_COMPACT] > 0.0f,
       "silhouettes have non-zero length");

    printf(s_fail ? "FAILED (%d)\n" : "passed\n", s_fail);
    return s_fail ? 1 : 0;
}
```

- [ ] **Step 3: Add the test target to CMakeLists.txt**

```cmake
    # Grand Thumb Auto III: vehicle silhouettes. An inward normal is an
    # invisible car and a cabin inside the body is a brick — neither is
    # diagnosable at 128x128 with a car six pixels long.
    add_executable(gta3_test_veh
        games/grandthumbauto3/src/test_veh.c
        games/grandthumbauto3/src/gta3_veh.c)
    target_include_directories(gta3_test_veh PRIVATE
        ${MOTE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/games/grandthumbauto3/src)
    target_compile_definitions(gta3_test_veh PRIVATE MOTE_HOST=1)
    target_compile_options(gta3_test_veh PRIVATE -O2 -Wall -Wextra
        -Wno-unused-parameter)
    target_link_libraries(gta3_test_veh PRIVATE m)
```

- [ ] **Step 4: Create an empty implementation and run the test to verify it fails**

Create `games/grandthumbauto3/src/gta3_veh.c` containing only `#include "gta3_veh.h"`, then:

```bash
cmake -S . -B build && cmake --build build --target gta3_test_veh
```

Expected: FAIL at link time — `undefined reference to 'gta3_sil_for_class'`.

- [ ] **Step 5: Write the implementation**

Replace `games/grandthumbauto3/src/gta3_veh.c` with:

```c
#include "gta3_veh.h"

#include "mote_build.h"    /* mote__face: normals computed from the geometry */

/* CAR_CLS order, from game.c:
 *  0 SEDAN  1 COMPACT  2 COUPE  3 SPORTS  4 RACER  5 MUSCLE  6 HOTHATCH
 *  7 CLASSIC  8 CLASSICSPT  9 LUXURY  10 WAGON  11 VAN  12 PICKUP  13 JEEP
 * 14 TAXI  15 POLICE  16 AMBULANCE  17 FIRETRUCK  18 TOWTRUCK */
static const unsigned char SIL[19] = {
    GTA3_SIL_SEDAN,    /*  0 SEDAN      */
    GTA3_SIL_COMPACT,  /*  1 COMPACT    */
    GTA3_SIL_WEDGE,    /*  2 COUPE      */
    GTA3_SIL_WEDGE,    /*  3 SPORTS     */
    GTA3_SIL_WEDGE,    /*  4 RACER      */
    GTA3_SIL_LONGHOOD, /*  5 MUSCLE     */
    GTA3_SIL_COMPACT,  /*  6 HOTHATCH   */
    GTA3_SIL_LONGHOOD, /*  7 CLASSIC    */
    GTA3_SIL_WEDGE,    /*  8 CLASSICSPT */
    GTA3_SIL_SEDAN,    /*  9 LUXURY     */
    GTA3_SIL_WAGON,    /* 10 WAGON      */
    GTA3_SIL_VAN,      /* 11 VAN        */
    GTA3_SIL_VAN,      /* 12 PICKUP     */
    GTA3_SIL_WAGON,    /* 13 JEEP       */
    GTA3_SIL_SEDAN,    /* 14 TAXI       */
    GTA3_SIL_SEDAN,    /* 15 POLICE     */
    GTA3_SIL_TRUCK,    /* 16 AMBULANCE  */
    GTA3_SIL_TRUCK,    /* 17 FIRETRUCK  */
    GTA3_SIL_TRUCK,    /* 18 TOWTRUCK   */
};

int gta3_sil_for_class(int car_cls) {
    if (car_cls < 0 || car_cls >= 19) return GTA3_SIL_SEDAN;
    return SIL[car_cls];
}

/* Per silhouette, in normalised int8 space (127 = the car's half-length):
 *   body_h  : how tall the body box is
 *   cab_top : the roofline
 *   cab_z0  : where the cabin starts, toward the tail
 *   cab_z1  : where the cabin ends, toward the nose
 *   cab_w   : the cabin's half-width as a fraction of the body's
 * A long hood pushes cab_z0/z1 toward the tail; a van runs the cabin nearly the
 * whole length and stands tall; a wedge is low with a shallow cabin. */
typedef struct { int body_h, cab_top, cab_z0, cab_z1, cab_w; } Sil;
static const Sil SILDEF[GTA3_SIL_N] = {
    /* SEDAN    */ { 34, 62, -55,  40, 88 },
    /* COMPACT  */ { 36, 64, -50,  50, 88 },
    /* WEDGE    */ { 28, 48, -50,  25, 84 },
    /* LONGHOOD */ { 32, 58, -72,  14, 86 },
    /* WAGON    */ { 40, 74, -55,  55, 90 },
    /* VAN      */ { 46, 96, -80,  70, 94 },
    /* TRUCK    */ { 44, 88, -30,  80, 92 },
};

static void box(MeshVert *v, MeshFace *f, int *nf,
                int x0, int x1, int y0, int y1, int z0, int z1) {
    const int C[8][3] = { {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
                          {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1} };
    for (int i = 0; i < 8; i++) {
        v[i].x = (signed char)C[i][0];
        v[i].y = (signed char)C[i][1];
        v[i].z = (signed char)C[i][2];
    }
    /* CCW from outside, so mote__face derives outward normals. */
    *nf = 0;
    mote__face(v, f, nf, 0, 2, 1, 0); mote__face(v, f, nf, 0, 3, 2, 0);  /* -Z */
    mote__face(v, f, nf, 4, 5, 6, 0); mote__face(v, f, nf, 4, 6, 7, 0);  /* +Z */
    mote__face(v, f, nf, 0, 4, 7, 0); mote__face(v, f, nf, 0, 7, 3, 0);  /* -X */
    mote__face(v, f, nf, 1, 2, 6, 0); mote__face(v, f, nf, 1, 6, 5, 0);  /* +X */
    mote__face(v, f, nf, 0, 1, 5, 0); mote__face(v, f, nf, 0, 5, 4, 0);  /* -Y */
    mote__face(v, f, nf, 3, 7, 6, 0); mote__face(v, f, nf, 3, 6, 2, 0);  /* +Y */
}

void gta3_veh_build(Gta3VehMesh *m, int sil) {
    if (sil < 0 || sil >= GTA3_SIL_N) sil = GTA3_SIL_SEDAN;
    const Sil *s = &SILDEF[sil];
    int nf;

    box(m->bv, m->bf, &nf, -127, 127, 0, s->body_h, -127, 127);
    m->body = (Mesh){ .verts=m->bv, .faces=m->bf, .nverts=8, .nfaces=nf,
                      .scale=1.0f, .bound_r=1.8f, .color=0xFFFF };

    int cw = (127 * s->cab_w) / 100;
    box(m->cv, m->cf, &nf, -cw, cw, s->body_h, s->cab_top, s->cab_z0, s->cab_z1);
    m->cabin = (Mesh){ .verts=m->cv, .faces=m->cf, .nverts=8, .nfaces=nf,
                       .scale=1.0f, .bound_r=1.8f, .color=MOTE_RGB565(40,46,60) };
}
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build build --target gta3_test_veh && ./build/gta3_test_veh
```

Expected: every line `ok`, final line `passed`, exit code 0.

- [ ] **Step 7: Commit**

```bash
git add games/grandthumbauto3/src/gta3_veh.h games/grandthumbauto3/src/gta3_veh.c \
        games/grandthumbauto3/src/test_veh.c CMakeLists.txt
git commit -m "Seven vehicle silhouettes for the 54 car types"
```

---

### Task 7: Draw vehicles as meshes

Bakes the per-type paint colours, swaps cars from flat ground sprites to tinted meshes, and replaces the hand-built octagon shadows.

**Files:**
- Create: `games/grandthumbauto3/assets/make_carcolors.py`, `games/grandthumbauto3/src/carcolors.h`
- Modify: `games/grandthumbauto3/src/game.c:3540-3560` (car drawing), `:3984-4000` (octagon shadows), `:307` region (mesh init)

**Interfaces:**
- Consumes: `gta3_veh_build`, `gta3_sil_for_class`, `Gta3VehMesh` from Task 6.
- Produces: `static const uint16_t CAR_COL[54]` in `carcolors.h`; `static Gta3VehMesh g_veh[GTA3_SIL_N];` and `static void draw_vehicle(const Car *c);` in `game.c`.

- [ ] **Step 1: Write the colour baker**

Create `games/grandthumbauto3/assets/make_carcolors.py`:

```python
#!/usr/bin/env python3
"""
Bake one paint colour per car type from cars2.png into src/carcolors.h.

The 54 cars in this game are distinguished by their PAINT, not their shape:
seven mesh silhouettes cover all of them, so identity has to come from colour.
Rather than pick 54 colours by hand and have them drift from the sprite sheet,
take each cell's dominant body colour straight from the art.

"Dominant body colour" deliberately ignores two things: transparent padding,
and the dark pixels that are windows, tyres and shadow. What is left is the
paint. Greys survive because the darkness cut is on value, not saturation --
a silver car should stay silver.
"""
import os
from collections import Counter
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SHEET = os.path.join(HERE, "cars2.png")
OUT = os.path.join(HERE, "..", "src", "carcolors.h")

CW, CH = 28, 60          # cell size, matching CAR_CW/CAR_CH in game.c
COLS = 8                 # the sheet is 8 cells wide
N = 54                   # 54 car types

def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def dominant(img, cx, cy):
    cell = img.crop((cx, cy, cx + CW, cy + CH)).convert("RGBA")
    counts = Counter()
    for r, g, b, a in cell.getdata():
        if a < 128:
            continue                      # transparent padding
        if r + g + b < 150:
            continue                      # windows, tyres, shadow
        counts[(r, g, b)] += 1
    if not counts:
        return (160, 160, 170)            # a car that is all glass: grey it
    return counts.most_common(1)[0][0]

def main():
    img = Image.open(SHEET)
    cols = []
    for i in range(N):
        r, g, b = dominant(img, (i % COLS) * CW, (i // COLS) * CH)
        cols.append(rgb565(r, g, b))
    with open(OUT, "w") as f:
        f.write("/* Generated by assets/make_carcolors.py -- do not edit.\n"
                " * One RGB565 paint colour per car type, sampled from cars2.png.\n"
                " * All 54 cars share seven mesh silhouettes; this is what tells\n"
                " * them apart. Re-run the script if the sheet changes. */\n")
        f.write("#ifndef CARCOLORS_H\n#define CARCOLORS_H\n\n")
        f.write("static const uint16_t CAR_COL[%d] = {\n" % N)
        for i in range(0, N, 8):
            f.write("    " + ", ".join("0x%04X" % c for c in cols[i:i+8]) + ",\n")
        f.write("};\n\n#endif /* CARCOLORS_H */\n")
    print("wrote %s (%d colours)" % (os.path.normpath(OUT), N))

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it and check the output**

```bash
cd /Users/chris/code/hardware/mote/games/grandthumbauto3/assets
python3 make_carcolors.py
head -12 ../src/carcolors.h
```

Expected: `wrote .../src/carcolors.h (54 colours)`, and the header shows 54 hex values. Spot-check that car 26 (the dark police cruiser) is dark and car 30 (the teal taxi) is not.

- [ ] **Step 3: Include the header and build the meshes**

In `game.c`, next to the other generated includes:

```c
#include "gta3_veh.h"
#include "carcolors.h"     /* CAR_COL[54] — generated by assets/make_carcolors.py */
```

Add `static Gta3VehMesh g_veh[GTA3_SIL_N];` near `g_bmesh`, and in `g_init` (game.c:1064) next to `build_vstats()`:

```c
    for (int s = 0; s < GTA3_SIL_N; s++) gta3_veh_build(&g_veh[s], s);
```

- [ ] **Step 4: Write the vehicle draw**

Add above the existing car-drawing code (near game.c:3540):

```c
/* Draw one vehicle as two tinted boxes plus a ground shadow.
 *
 * Two objects, not one: MoteObject.color is a single colour for the whole
 * object, so a one-box car has no windows and reads as a brick. The body takes
 * the type's paint from CAR_COL, the cabin a dark glass tone.
 *
 * A wreck is the same geometry in a charred tone — the husk stays visible and
 * pushable, exactly as the sprite version did. */
static void draw_vehicle(const Car *c) {
    int sil = (c->type < CARS2_N) ? gta3_sil_for_class(CAR_CLS[c->type])
                                  : (c->type == VEH_BUS ? GTA3_SIL_VAN : GTA3_SIL_TRUCK);
    const VStat *vs = &VSTAT[c->type];
    Gta3VehMesh *m = &g_veh[sil];

    Mat3 b = m3_identity();
    m3_rotate_local(&b, 1, -c->yaw);

    /* Normalised geometry spans +/-127 in each axis; scale each axis to this
     * car's measured size by scaling the object and letting the mesh's own
     * proportions carry the shape. The longest axis sets the scale. */
    float half_len = vs->len * 0.5f, half_wid = vs->wid * 0.5f;
    float sc = half_len > half_wid ? half_len : half_wid;

    uint16_t paint = c->wrecked ? MOTE_RGB565(38,34,34)
                   : (c->type < CARS2_N ? CAR_COL[c->type] : MOTE_RGB565(190,190,200));
    uint16_t glass = c->wrecked ? MOTE_RGB565(24,22,22) : MOTE_RGB565(40,46,60);

    MoteObject body = { .pos=v3(c->x, 0.0f, c->z), .basis=b, .mesh=&m->body, .color=paint };
    mote->scene_add_object_scaled(&body, sc);
    MoteObject cab  = { .pos=v3(c->x, 0.0f, c->z), .basis=b, .mesh=&m->cabin, .color=glass };
    mote->scene_add_object_scaled(&cab, sc);

    /* One oriented shadow, replacing the eight-triangle octagon this used to
     * assemble by hand from scene_add_tri. */
    float fx = cosf(c->yaw), fz = sinf(c->yaw);
    mote->scene_add_shadow_ex(v3(c->x, 0.02f, c->z),
                              v3(fx * vs->len * 0.46f, 0, fz * vs->len * 0.46f),
                              v3(-fz * vs->wid * 0.48f, 0, fx * vs->wid * 0.48f),
                              0.55f);
}
```

- [ ] **Step 5: Replace the call sites**

At game.c:3546, replace the `draw_ground_sprite(img, c->x, c->z, ...)` car draw with `draw_vehicle(c);`, removing the sprite-cell lookup (`fxw`, `fyw`, `cw`, `ch`, `dlen`, `dwid`) that fed it. Do the same for the remote deathmatch car at game.c:3364.

Delete the octagon shadow loop at game.c:3984-4000 entirely — `draw_vehicle` now issues the shadow.

- [ ] **Step 6: Declare the new pools**

In `k_vtbl.config` (game.c:4428), add `.max_shadows = 40,`.

- [ ] **Step 7: Build and run**

```bash
cd /Users/chris/code/hardware/mote
./tools/mote run games/grandthumbauto3
```

Expected: cars are solid 3D boxes with a darker cabin, each a different colour, correctly oriented, with a soft oval shadow under each. A police cruiser is dark, a taxi is teal. Wrecks are charred. No car is invisible (that would mean inward normals — go back to Task 6).

- [ ] **Step 8: Commit**

```bash
git add games/grandthumbauto3/assets/make_carcolors.py \
        games/grandthumbauto3/src/carcolors.h games/grandthumbauto3/src/game.c
git commit -m "Draw vehicles as tinted meshes with oriented ground shadows"
```

---

### Task 8: Side-view characters as billboards

**Files:**
- Create: `games/grandthumbauto3/assets/make_chars3d.py`
- Modify: `games/grandthumbauto3/src/game.c:4034` (peds), `:4040` (player), `:3367` (remote player), `:2031` region (foot cops)

**Interfaces:**
- Consumes: `g_cam` (for `cam_basis.r[2]`), `mote->scene_add_billboard`, `mote->scene_add_shadow_ex`.
- Produces: `static int facing_cell(float yaw);` and `static void draw_character(const MoteImage *img, float x, float z, float yaw, int variant, int frame);` in `game.c`; regenerated `player.h`, `ped.h`, `cop.h`.

- [ ] **Step 1: Write the character generator**

Create `games/grandthumbauto3/assets/make_chars3d.py`. It regenerates `player.png`, `ped.png` and `cop.png` as side-view sheets laid out as 4 facings across × N frames down, 16×16 cells:

```python
#!/usr/bin/env python3
"""
Generate side-view character sprites for the third-person camera.

The top-down game drew people from directly above: a head, two shoulders, and
nothing else. As a billboard that is a disc. These are the replacements --
back, front, left and right views with a walk cycle -- laid out as

    columns = facing  (0 back, 1 front, 2 left, 3 right)
    rows    = frame   (walk cycle)

at 16x16 per cell, which is what the existing sprite code already expects.

The camera trails the player, so the BACK view is what is on screen almost all
the time; the other three exist for pedestrians and for the moments the camera
swings round. They are drawn as simple blocked figures at this size because a
16x16 person at 128x128 is about twelve pixels tall on screen -- detail below
that is noise.
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
CELL = 16
KEY = (255, 0, 255, 255)        # the engine's colour key

# (name, skin, shirt, trousers, hair, frames)
CHARS = [
    ("player", (226, 190, 150), (60, 90, 170), (50, 50, 62), (60, 44, 32), 4),
    ("ped",    (226, 190, 150), (170, 70, 70), (70, 66, 60), (30, 26, 22), 4),
    ("cop",    (226, 190, 150), (36, 50, 96),  (36, 42, 60), (40, 34, 28), 2),
]
# ped.png holds 4 variants side by side in the original; keep that by shifting
# the shirt hue per variant.
PED_SHIRTS = [(170, 70, 70), (70, 150, 90), (180, 150, 60), (120, 80, 160)]


def figure(d, ox, oy, facing, phase, skin, shirt, trousers, hair):
    """One 16x16 figure. phase in [0,1) drives the leg swing."""
    # legs: swing opposite each other, 2 px of travel
    swing = 1 if phase < 0.5 else -1
    d.rectangle([ox + 6, oy + 11, ox + 7, oy + 15], fill=trousers)
    d.rectangle([ox + 8, oy + 11, ox + 9, oy + 15], fill=trousers)
    if facing in (2, 3):                       # side view: one leg forward
        d.rectangle([ox + 6 + swing, oy + 13, ox + 7 + swing, oy + 15], fill=trousers)
    else:                                      # front/back: legs apart
        d.rectangle([ox + 6, oy + 14, ox + 7, oy + 15 - (swing > 0)], fill=trousers)

    # torso
    d.rectangle([ox + 5, oy + 5, ox + 10, oy + 11], fill=shirt)
    # arms
    if facing in (2, 3):
        ax = ox + 4 if facing == 2 else ox + 11
        d.rectangle([ax, oy + 6, ax, oy + 10], fill=shirt)
    else:
        d.rectangle([ox + 4, oy + 6, ox + 4, oy + 10], fill=shirt)
        d.rectangle([ox + 11, oy + 6, ox + 11, oy + 10], fill=shirt)

    # head
    d.rectangle([ox + 6, oy + 1, ox + 9, oy + 4], fill=skin)
    # hair: covers the whole head from behind, a fringe from the front
    if facing == 0:
        d.rectangle([ox + 6, oy + 1, ox + 9, oy + 4], fill=hair)
    elif facing == 1:
        d.rectangle([ox + 6, oy + 1, ox + 9, oy + 1], fill=hair)
        d.point((ox + 7, oy + 3), fill=(40, 30, 25))     # eyes
        d.point((ox + 8, oy + 3), fill=(40, 30, 25))
    else:
        d.rectangle([ox + 6, oy + 1, ox + 9, oy + 2], fill=hair)
        side = ox + 6 if facing == 2 else ox + 9
        d.point((side, oy + 3), fill=(40, 30, 25))


def sheet(name, skin, shirt, trousers, hair, frames, variants=1, shirts=None):
    w, h = CELL * 4, CELL * frames * variants
    img = Image.new("RGBA", (w, h), KEY)
    d = ImageDraw.Draw(img)
    for v in range(variants):
        sh = shirts[v] if shirts else shirt
        for fr in range(frames):
            for fac in range(4):
                figure(d, fac * CELL, (v * frames + fr) * CELL,
                       fac, fr / float(frames), skin, sh, trousers, hair)
    out = os.path.join(HERE, name + ".png")
    img.save(out)
    print("wrote %s (%dx%d)" % (out, w, h))


def main():
    for name, skin, shirt, trousers, hair, frames in CHARS:
        if name == "ped":
            sheet(name, skin, shirt, trousers, hair, frames,
                  variants=len(PED_SHIRTS), shirts=PED_SHIRTS)
        else:
            sheet(name, skin, shirt, trousers, hair, frames)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it and rebake the headers**

```bash
cd /Users/chris/code/hardware/mote/games/grandthumbauto3/assets
python3 make_chars3d.py
```

Then rebake each PNG to its `src/*.h` the way the existing assets were baked. Check how `make_sprites.py` in this directory emits its header and follow the same call, so `player.h`, `ped.h` and `cop.h` are regenerated in place with the same image-name symbols (`player_img`, `ped_img`, `cop_img`).

Expected: `src/player.h`, `src/ped.h`, `src/cop.h` change; the symbol names do not.

- [ ] **Step 3: Write the facing selector and the character draw**

In `game.c`, next to `draw_ground_sprite`:

```c
/* Which of the four side-view columns to show for a character facing `yaw`.
 * The angle is measured against the CAMERA forward, not a world axis, so the
 * choice follows the view: a ped walking away from the camera shows its back
 * whichever compass direction that happens to be.
 * 0 back, 1 front, 2 left, 3 right — matching make_chars3d.py's column order. */
static int facing_cell(float yaw) {
    float cyaw = atan2f(cam_basis.r[2].z, cam_basis.r[2].x);
    float rel = gta3_wrap_angle(yaw - cyaw);
    if (rel > -0.785f && rel <= 0.785f) return 0;        /* same way as the camera: back */
    if (rel > 0.785f && rel <= 2.356f)  return 3;
    if (rel < -0.785f && rel >= -2.356f) return 2;
    return 1;                                            /* toward the camera: front */
}

/* A person, as an upright camera-facing quad with a shadow under it. 1.8 m is
 * roughly human height in this world's scale, and the shadow is what stops a
 * billboard reading as a sticker floating over the road. */
#define CHAR_H 1.8f
static void draw_character(const MoteImage *img, float x, float z, float yaw,
                           int variant, int frame, int nframes) {
    int col = facing_cell(yaw);
    int row = variant * nframes + frame;
    mote->scene_add_billboard(v3(x, CHAR_H * 0.5f, z), img,
                              col * 16, row * 16, 16, 16, CHAR_H, MOTE_BLEND_NONE);
    mote->scene_add_shadow_ex(v3(x, 0.02f, z), v3(0.42f, 0, 0), v3(0, 0, 0.42f), 0.5f);
}
```

- [ ] **Step 4: Replace the character call sites**

At game.c:4034 (pedestrians), replace the `draw_ground_sprite(img, p->x, p->z, p->yaw, fr*16, fy, 16,16, 1.9f, 1.9f, 1)` call with:

```c
        draw_character(img, p->x, p->z, p->yaw, p->variant, fr, 4);
```

matching the variant field the ped struct already uses to pick `fy`. Foot cops use `cop_img` with `nframes = 2` and variant 0.

At game.c:4040 (the player on foot) and game.c:3367 (the remote deathmatch player), make the equivalent replacement with `player_img`, variant 0, `nframes = 4`.

- [ ] **Step 5: Declare the billboard pool**

In `k_vtbl.config`, add `.max_billboards = 48,`.

- [ ] **Step 6: Build and run**

```bash
cd /Users/chris/code/hardware/mote
./tools/mote run games/grandthumbauto3
```

Expected: the player stands upright, seen from behind, with a shadow. Walking turns the legs over. Pedestrians face the way they walk — turn the camera around one and its facing changes. Nobody is lying flat on the road.

- [ ] **Step 7: Commit**

```bash
git add games/grandthumbauto3/assets/make_chars3d.py \
        games/grandthumbauto3/assets/*.png games/grandthumbauto3/src/*.h \
        games/grandthumbauto3/src/game.c
git commit -m "Side-view characters as billboards with ground shadows"
```

---

### Task 9: Scenery, pickups, props and tracers

The remaining flat sprites that are not genuinely flat.

**Files:**
- Modify: `games/grandthumbauto3/src/game.c:4058` (scenery), `:4015` (markers), `:4022` (a rotated prop), `:2126` region (pickups), `:3557` (tank turret), `:4301` (bullet tracers)
- Modify: `games/grandthumbauto3/assets/make_chars3d.py` (add side-view scenery)

**Interfaces:**
- Consumes: `draw_character`'s billboard pattern from Task 8.
- Produces: `static void draw_upright(const MoteImage *img, float x, float z, int fx, int fy, int fw, int fh, float h);` in `game.c`; a `gta3_turret` mesh.

- [ ] **Step 1: Add side-view scenery to the generator**

Append to `make_chars3d.py` a `scenery` sheet: oak, pine, autumn, bush, flowers and boulder as 20×20 side views (a trunk and a canopy, not a canopy seen from above), keeping the existing cell layout `scenery_img` is indexed with at game.c:4058. Re-run the script and rebake `src/scenery.h`.

- [ ] **Step 2: Add the upright sprite helper**

```c
/* An upright camera-facing sprite that is not a person: a tree, a pickup, a
 * phone box. Anchored at the ground, so `h` is its full world height. */
static void draw_upright(const MoteImage *img, float x, float z,
                         int fx, int fy, int fw, int fh, float h) {
    mote->scene_add_billboard(v3(x, h * 0.5f, z), img, fx, fy, fw, fh, h,
                              MOTE_BLEND_NONE);
}
```

- [ ] **Step 3: Convert scenery and pickups**

At game.c:4058, replace the scenery `draw_ground_sprite` with:

```c
          draw_upright(&scenery_img, tx, tz, ((h>>2)&1)*20, 0, 20, 20, 5.5f);
```

For pickups, replace their `draw_ground_sprite` with `draw_upright` at 1.0 m, plus a bob so they read as collectable:

```c
        float bob = 0.15f * sinf(g_time * 3.0f + i);
        mote->scene_add_billboard(v3(p->x, 0.6f + bob, p->z), &pickups_img,
                                  p->kind*16, 0, 16, 16, 1.0f, MOTE_BLEND_NONE);
```

- [ ] **Step 4: Keep the genuinely flat things flat**

Leave `draw_ground_sprite` in place and keep using it for the spray decal and the gun mat at game.c:4015 and 4022, and leave `road_markings` untouched. A decal painted on the road is correct from any camera angle — converting it to a billboard would stand it up like a signpost.

The phone box marker becomes upright: give it `draw_upright` at 2.2 m.

- [ ] **Step 5: Replace the tank turret sprite with a mesh**

At game.c:3557, the turret is drawn as a rotated top-down sprite. Replace it with a box plus a barrel box, built the same way `gta3_veh_build` builds a cabin, rotated to `tyaw` about Y and drawn at the hull's position. Add the two meshes as file-scope statics built in `g_init`.

- [ ] **Step 6: Depth-test the tracers**

At game.c:4301, bullet tracers are drawn in `overlay` via `world_to_screen` and two `draw_line` calls, so they paint over buildings. Replace with:

```c
        mote->scene_add_line(v3(b->x, 0.9f, b->z),
                             v3(b->x - b->vx*0.03f, 0.9f, b->z - b->vz*0.03f),
                             MOTE_RGB565(255, 230, 140));
```

moved out of `overlay` and into the 3D build in `g_update`. Add `.max_lines = 24,` to `k_vtbl.config`.

- [ ] **Step 7: Build and run**

```bash
./tools/mote run games/grandthumbauto3
```

Expected: trees stand up with trunks. Pickups hover and bob. Road decals stay painted flat on the road. The tank's turret is a box that rotates independently of the hull. Tracers disappear behind buildings instead of drawing over them.

- [ ] **Step 8: Commit**

```bash
git add games/grandthumbauto3/assets games/grandthumbauto3/src
git commit -m "Stand up the scenery, pickups and turret; depth-test the tracers"
```

---

### Task 10: Tank controls on foot

**Files:**
- Modify: `games/grandthumbauto3/src/game.c` — the on-foot input block (search for `MODE_FOOT` in `g_update`, near the `drive_car` call at game.c:3872)

**Interfaces:**
- Consumes: `g_lookback` declared in Task 4.
- Produces: no new symbols; changes how `player.yaw`, `player.x` and `player.z` are driven.

- [ ] **Step 1: Replace the on-foot movement**

The top-down game moves the player in absolute world directions from the d-pad. Replace that block with turn-and-walk:

```c
    /* Tank controls: left/right turn, up/down walk. The d-pad has eight
     * directions and the camera swings; camera-relative movement on a d-pad
     * flips direction under you every time the camera comes round a corner.
     * Turning in place does not. */
    if (player.mode == MODE_FOOT) {
        const float TURN = 3.0f;          /* rad/s */
        if (mote_pressed(in, MOTE_BTN_LEFT))  player.yaw -= TURN * dt;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) player.yaw += TURN * dt;
        player.yaw = gta3_wrap_angle(player.yaw);

        float drive = 0.0f;
        if (mote_pressed(in, MOTE_BTN_UP))   drive =  1.0f;
        if (mote_pressed(in, MOTE_BTN_DOWN)) drive = -0.5f;   /* backing up is slower */

        if (drive != 0.0f) {
            float sp = WALK_SPD * drive * dt;
            float nx = player.x + cosf(player.yaw) * sp;
            float nz = player.z + sinf(player.yaw) * sp;
            move_body(&player.x, &player.z, nx, nz, 0);
        }
        g_lookback = mote_pressed(in, MOTE_BTN_LB);
    } else {
        g_lookback = 0;
    }
```

Use the existing walk speed constant rather than inventing one — find what the current on-foot movement multiplies by and name it `WALK_SPD` if it is inline.

- [ ] **Step 2: Keep the walk animation in step**

The walk frame is currently advanced from whether the d-pad is pressed. Drive it from `drive != 0.0f` instead, so turning in place does not animate a walk cycle.

- [ ] **Step 3: Build and run**

```bash
./tools/mote run games/grandthumbauto3
```

Expected: left and right rotate the character on the spot with the camera swinging to follow; up walks the way they face; down backs up slowly without turning around; holding LB swings the camera to look back and releasing returns it. A and B still enter cars and attack; RB still cycles weapons.

- [ ] **Step 4: Commit**

```bash
git add games/grandthumbauto3/src/game.c
git commit -m "Tank controls on foot with LB to look behind"
```

---

### Task 11: Overlay fixes

**Files:**
- Modify: `games/grandthumbauto3/src/game.c:4069-4090` (marker arrows), `:4228-4236` (the mission arrow), `:3973` region (title camera)

**Interfaces:**
- Consumes: `cam_basis`, `world_to_screen`.
- Produces: `static int screen_or_edge(float wx, float wz, float *sx, float *sy, float *ang);`

- [ ] **Step 1: Write the behind-camera bearing helper**

`world_to_screen` returns 0 for anything behind the near plane. Top-down that meant "off the edge of the screen" and the surrounding code coped. With a horizontal camera, behind the camera is most of the world, and markers to your rear silently vanish. Add above the marker code:

```c
/* Where does a world point belong on screen — and if it is not on screen, which
 * edge does it point to?
 *
 * Returns 1 when the point projects normally (sx, sy are its screen position).
 * Returns 0 when it is behind the camera or off the edge; sx/sy are then
 * clamped to the screen border and `ang` is the direction to draw an arrow.
 *
 * The behind-camera case cannot use the projection at all — it failed — so the
 * bearing comes from the world-space angle between the camera forward and the
 * direction to the point. */
static int screen_or_edge(float wx, float wz, float *sx, float *sy, float *ang) {
    if (world_to_screen(v3(wx, 0.6f, wz), sx, sy, 0) &&
        *sx > 4 && *sx < 124 && *sy > 4 && *sy < 124) {
        *ang = 0.0f;
        return 1;
    }
    /* World-space bearing, relative to where the camera looks. */
    float dx = wx - cam_pos.x, dz = wz - cam_pos.z;
    float fwd_a = atan2f(cam_basis.r[2].z, cam_basis.r[2].x);
    float rel = gta3_wrap_angle(atan2f(dz, dx) - fwd_a);
    /* rel = 0 straight ahead, +/-pi behind. Screen up is ahead. */
    *ang = rel;
    float ex = sinf(rel), ey = -cosf(rel);
    float k = 54.0f / (fabsf(ex) > fabsf(ey) ? fabsf(ex) : fabsf(ey));
    *sx = 64.0f + ex * k;
    *sy = 64.0f + ey * k;
    return 0;
}
```

- [ ] **Step 2: Route the marker arrows through it**

At game.c:4069-4090 and 4228-4236, replace the direct `world_to_screen` calls with `screen_or_edge`, drawing the marker icon when it returns 1 and an edge arrow rotated to `ang` when it returns 0.

- [ ] **Step 3: Replace the title camera with a slow orbit**

At game.c:3973, the title and death screens call `chase_camera` on a stationary player, which frames the back of a parked car. Replace with an orbit around the spawn block:

```c
    /* Title and death screens: a slow orbit round the block, which is also the
     * shot the gallery screenshot wants. */
    g_titlet += dt * 0.15f;
    float ox = pl_x() + cosf(g_titlet) * 22.0f;
    float oz = pl_z() + sinf(g_titlet) * 22.0f;
    cam_pos = v3(ox, 12.0f, oz);
    cam_basis = mote_camera_look(cam_pos, v3(pl_x(), 3.0f, pl_z()));
    view_x = pl_x(); view_z = pl_z();
    gta3_view_set(&g_view, cam_pos, cam_basis.r[2], FOV, 1.45f);
    mote->scene_camera(&cam_basis, cam_pos, FOV);
```

with `static float g_titlet;` at file scope, reset in `reset_game()`.

- [ ] **Step 4: Build and run**

```bash
./tools/mote run games/grandthumbauto3
```

Expected: mission and pickup markers behind you show an arrow at the correct screen edge pointing backward, and the arrow rotates correctly as you turn. The title screen orbits the city slowly.

- [ ] **Step 5: Commit**

```bash
git add games/grandthumbauto3/src/game.c
git commit -m "Point marker arrows correctly when the target is behind the camera"
```

---

### Task 12: The radar

**Files:**
- Modify: `games/grandthumbauto3/src/game.c` — `g_overlay` (near the HUD code, before the `g_showmap` early return at game.c:4203)

**Interfaces:**
- Consumes: `tile_at`, `markers`, `nmark`, `cars`, `cam_basis`.
- Produces: `static void draw_radar(uint16_t *fb);`

- [ ] **Step 1: Write the radar**

The MENU full-map pause view (`g_showmap`, `draw_map`) stays exactly as it is. This is the live companion to it: pausing to read a map at every junction is not navigation.

```c
/* A live radar, rotated so up is where the camera looks.
 *
 * Top-down, the street grid ahead was simply visible in the play view. Behind a
 * car at 3 m it is not, and the missions route across the city. The MENU map
 * still exists for planning; this is for driving.
 *
 * 40 px across, bottom-left, sampling the tile map on a coarse step — fine
 * enough to show which way a street runs, cheap enough to run every frame. */
#define RADAR_X   4
#define RADAR_Y   84
#define RADAR_R   20
#define RADAR_M   2.6f        /* world metres per radar pixel */
static void draw_radar(uint16_t *fb) {
    int cx = RADAR_X + RADAR_R, cy = RADAR_Y + RADAR_R;
    float a = atan2f(cam_basis.r[2].z, cam_basis.r[2].x);
    /* Rotate world -> radar so the camera forward points up the screen. */
    float ca = cosf(-a - 1.5708f), sa = sinf(-a - 1.5708f);

    mote->draw_circle(fb, cx, cy, RADAR_R + 1, MOTE_RGB565(16,18,24), 1, 0, 128);
    for (int py = -RADAR_R; py <= RADAR_R; py++)
        for (int px = -RADAR_R; px <= RADAR_R; px++) {
            if (px*px + py*py > RADAR_R*RADAR_R) continue;
            /* radar pixel -> world, undoing the rotation */
            float wx = pl_x() + ( px*ca + py*sa) * RADAR_M;
            float wz = pl_z() + (-px*sa + py*ca) * RADAR_M;
            char c = tile_at((int)floorf(wx/TILE), (int)floorf(wz/TILE));
            uint16_t col;
            if (c=='.'||c=='B') col = MOTE_RGB565(120,124,136);
            else if (c=='~')    col = MOTE_RGB565(30,44,86);
            else if (c==','||c==' ') col = MOTE_RGB565(58,62,72);
            else                col = MOTE_RGB565(38,40,50);
            mote->draw_pixel(fb, cx + px, cy + py, col);
        }

    /* mission markers, then wanted cops on top of them */
    for (int m = 0; m < nmark; m++) {
        float dx = markers[m].x - pl_x(), dz = markers[m].z - pl_z();
        float rx = ( dx*ca + dz*sa) / RADAR_M, ry = (-dx*sa + dz*ca) / RADAR_M;
        if (rx*rx + ry*ry > RADAR_R*RADAR_R) continue;
        mote->draw_rect(fb, cx+(int)rx-1, cy+(int)ry-1, 3, 3,
                        MOTE_RGB565(240,200,80), 1, 0, 128);
    }
    if (wanted() > 0)
        for (int i = 0; i < NCAR; i++) {
            Car *c = &cars[i];
            if (!c->alive || c->driver != DRV_COP) continue;
            float dx = c->x - pl_x(), dz = c->z - pl_z();
            float rx = ( dx*ca + dz*sa) / RADAR_M, ry = (-dx*sa + dz*ca) / RADAR_M;
            if (rx*rx + ry*ry > RADAR_R*RADAR_R) continue;
            mote->draw_rect(fb, cx+(int)rx-1, cy+(int)ry-1, 3, 3,
                            MOTE_RGB565(90,150,255), 1, 0, 128);
        }

    /* the player: always dead centre, always pointing up */
    mote->draw_pixel(fb, cx, cy, MOTE_RGB565(255,255,255));
    mote->draw_pixel(fb, cx, cy-1, MOTE_RGB565(255,255,255));
    mote->draw_pixel(fb, cx, cy-2, MOTE_RGB565(200,255,200));
}
```

- [ ] **Step 2: Call it**

In `g_overlay`, after the existing HUD is drawn and only while `g_state == ST_PLAY` and `!g_showmap`, call `draw_radar(fb);`.

- [ ] **Step 3: Build and run**

```bash
./tools/mote run games/grandthumbauto3
```

Expected: a circular radar bottom-left showing the street grid, rotating as you turn so the road ahead points up. Mission markers show as yellow dots. Cops show blue when wanted. MENU still opens the full map and the radar hides while it is open.

- [ ] **Step 4: Commit**

```bash
git add games/grandthumbauto3/src/game.c
git commit -m "A live rotating radar for driving"
```

---

### Task 13: Profile on device and set the budget

The final task, and the only one whose numbers are not guesses. Everything before this used starting values from the spec.

**Files:**
- Modify: `games/grandthumbauto3/src/game.c` — debug HUD, `k_vtbl.config`, the `VIEW_*` constants

**Interfaces:**
- Consumes: `mote->metrics`, `mote->scene_tri_count`.
- Produces: final pool sizes and draw radii.

- [ ] **Step 1: Add the profiling HUD**

Behind the existing `MOTE_GTA_DEBUG` gate, draw per-frame `fps`, `update_us`, `raster_us` from `mote->metrics` and `mote->scene_tri_count()`:

```c
#ifdef MOTE_HOST
    if (getenv("MOTE_GTA_DEBUG")) {
        int mt[8] = {0};
        if (mote->metrics) mote->metrics(mt, 8);
        char line[48];
        snprintf(line, sizeof line, "%dfps u%d r%d t%d",
                 mt[0], mt[1], mt[2], mote->scene_tri_count());
        mote_ftext(mote, fb, g_fmed, 2, 2, MOTE_RGB565(200,255,140), line);
    }
#endif
```

Check `mote->metrics`'s exact signature in `sdk/mote_api.h` before writing this — README.md:1724 describes the array layout as `[0]=fps, [1]=update_us, [2]=raster_us`.

- [ ] **Step 2: Build for the device and push**

```bash
cd /Users/chris/code/hardware/mote
./tools/mote build games/grandthumbauto3 --device
./tools/mote push games/grandthumbauto3
```

- [ ] **Step 3: Measure the worst case**

The worst case is the densest downtown cell, at speed, facing down the longest avenue. Reach it directly with the teleport rather than driving there:

```bash
MOTE_GTA_VIEW=<x>,<z> ./tools/mote run games/grandthumbauto3
```

Record, on device: triangle count, textured-triangle count, and frame time, in downtown at speed, on foot in an alley, and on the bridge.

- [ ] **Step 4: Set the pools from what you measured**

Set `max_tris` and `max_tex_tris` in `k_vtbl.config` to the measured peaks plus roughly 15%. If the textured peak exceeds the pool, the engine latches `s_textri_starved` and falls back to flat shading rather than dropping the draw — so a starved frame shows as untextured buildings, which is the signal to either raise `max_tex_tris` or pull `VIEW_HAZE_R` inward.

- [ ] **Step 5: Tune the radii against the frame target**

If downtown misses 30 fps, reduce in this order, re-measuring each time: `VIEW_HAZE_R` (cheapest — moves buildings from textured to flat), then `VIEW_BLD_CAP`, then `VIEW_BLD_R`. Reduce `VIEW_GROUND_R` last; it is already the shortest and the ground is what sells speed.

If it still misses after all four, the spec's reserve plan applies: road-corridor occlusion, deferred from the design on purpose. Stop and raise it rather than shipping under target.

- [ ] **Step 6: Pin the frame rate**

Once downtown holds the target, pin it for steady timing. Once, on the first frame of `update()`:

```c
    static int fps_armed; if (!fps_armed) { fps_armed = 1; mote->set_fps_limit(30); }
```

- [ ] **Step 7: Run the full regression**

Every ported system, on device:

- Traffic flows and stops at junctions; cars do not drive through each other.
- Pedestrians path along pavements and react to being threatened.
- Cops escalate through heat levels; sirens play; the spray shop clears heat.
- Every weapon fires and hits; the tank drives and its shell explodes.
- A mission can be started from a phone and completed.
- Pickups collect; cash banks; death and arrest both save the best score.
- Link deathmatch connects between two devices and both avatars are visible.
- The camera never enters geometry: garages, bridges, alleys, tight corners.

- [ ] **Step 8: Run all three host tests one final time**

```bash
cmake --build build --target gta3_test_view gta3_test_camera gta3_test_veh
./build/gta3_test_view && ./build/gta3_test_camera && ./build/gta3_test_veh
```

Expected: three `passed` lines, exit code 0.

- [ ] **Step 9: Commit**

```bash
git add games/grandthumbauto3/src/game.c
git commit -m "Set the draw budget from the downtown profile"
```

---

## Self-Review

**Spec coverage.** Every section of the spec maps to a task: camera → 3, 4; controls → 10; visibility → 2, 5; entities (vehicles) → 6, 7; entities (characters) → 8; entities (scenery, turret, tracers) → 9; grounding shadows → 7, 8; radar → 12; resource budget → 13; fork mechanics → 1; overlay changes → 11; verification → 13. The spec's two deferred items (road-corridor occlusion, vehicle wheels) are correctly absent, with occlusion named as the escalation path in Task 13 Step 5.

**Type consistency.** `gta3_wrap_angle` is defined in Task 3 and used in Tasks 8, 10 and 11. `g_view` / `gta3_view_set` / `gta3_view_tile` are defined in Task 2 and used in Tasks 4, 5 and 11. `g_lookback` is declared in Task 4 and written in Task 10. `Gta3VehMesh` / `gta3_sil_for_class` are defined in Task 6 and used in Task 7. `CAR_COL` is generated in Task 7 Step 1 and read in Step 4. `draw_upright` (Task 9) and `draw_character` (Task 8) are distinct on purpose: characters need facing selection and a shadow, props need neither.

**Known soft spots**, flagged rather than hidden:
- Task 8 Step 2 says to follow `make_sprites.py`'s baking call rather than spelling it out, because that script's exact interface has not been read. The implementer must read it first.
- Task 9 Steps 1 and 5 (side-view scenery art, turret mesh) describe the work and its constraints without full code. Both are direct applications of patterns written out in full in Tasks 8 and 6 respectively.
- Task 10 Step 1 refers to `WALK_SPD`, which may be an inline constant in the current code. The step says to find it rather than assume it.
