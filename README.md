# Mote — a native game engine + console OS for the Thumby Color

Mote is a native-C game-development platform for the **Thumby Color** (RP2350:
dual Cortex-M33 @ 280 MHz w/ FPU, 520 KB SRAM, 128×128 RGB565 LCD). The engine
lives **resident in the OS**; games are tiny native modules that reach the engine
through a stable jump-table ABI. You **compile optimized native code on the host
and deploy it to the device with one command** — no per-game firmware reflash.

```
host:   mote new mygame  →  edit src/game.c  →  mote run mygame   (instant SDL emulator)
                                              →  mote push mygame  (USB → device, seconds)
device: boot → hero-menu launcher → pick a game → resident engine runs it
```

The repo ships **23 example games** spanning 3D raster, rigid-body physics,
Gaussian splats, voxels, and 2D — all built on the same ABI.

---

## 1. Quick start

Prereqs (host): `cmake`, a C toolchain, `libsdl2-dev`, `imagemagick` (for screenshots).

```bash
cmake -B build_host -S . && cmake --build build_host -j8   # build the engine + host OS once
mote new mygame                                            # scaffold a game (spinning cube)
mote run mygame                                            # build + launch the SDL emulator
```

`tools/mote` is the CLI (put it on your `PATH` or call `./tools/mote`).

| Command | What it does |
|---|---|
| `mote new <dir>` | Scaffold a game: `src/game.c` (uses the helpers + declares a config) |
| `mote build <dir>` | Compile → host `.so` (add `--device` for the RP2350 `.mote`) |
| `mote run <dir>` | Build for host + launch the SDL emulator |
| `mote bake <dir>` | Convert `assets/*.obj` and `assets/*.stl` → `src/<name>.h` |
| `mote push <dir>` | Build + upload the module over USB (`--launch` runs it now) |
| `mote ping` / `mote logs` | Check the device / stream its log + profiler |

Emulator keys: D-pad = arrows/WASD · A = `.`/K · B = `,`/J · LB = LShift · RB = Space · MENU = Enter · quit = Esc.

Headless (CI / screenshots): `SDL_VIDEODRIVER=dummy MOTE_PICK=0 MOTE_SHOT=/tmp/s.ppm MOTE_SHOT_FRAME=60 ./build_host/mote_host examples/mygame/build/mygame.so`.

---

## 1b. Mote Studio — the bespoke IDE (host)

`mote studio` opens **Mote Studio**, a hand-rolled native **C/SDL2** desktop IDE for
building Thumby Color games — no Electron/web, no Python. It builds for Linux and
**Windows** (a single self-contained `mote_studio.exe`).

```bash
mote studio              # or: ./build_host/mote_studio  (run from the repo root)
mote studio calibrate    # one-off: align the emulator screen to the device photo
```

**Layout** — Unity/Godot-style docks, resizable window with draggable separators:

- **Menu bar + toolbar** — Project/Assets/Build/Help + Run · Stop · Build · Push ·
  VS Code, with Lucide icons.
- **Project tree** (left) — the open game's files with tree connector lines + coloured
  type icons; auto-refreshes on file changes (+ a refresh button). Open a project from
  the **Open Project** modal (Project ▸ Open).
- **Emulator** (centre) — the *real engine* running the game inside a **photo-accurate
  Thumby Color shell** (calibrated, crisp integer-scaled screen). On-shell buttons are
  **clickable** and outline on hover/press; zoom control; keyboard **+ gamepad** input.
- **Inspector** (right) — context properties of the selected file; for `game.toml` it
  parses the `MoteConfig` pools from the C source and shows a **~280 KB arena budget**
  meter.
- **Bottom dock** — tabbed tools:
  - **Pixel Art** — HSV colour picker + hex, recent/preset swatches, pencil/eraser/
    fill/eyedropper/line/rect, undo, grid, sizes 8–128, zoom + pan, image import
    (PNG/BMP/JPG); **Save** writes `assets/sprite.png` and bakes a `MoteImage` header.
  - **Assets** — the project's baked assets.
  - **Mesh** — software-rendered 3D preview of `.stl`/`.obj` (drag to rotate).
  - **Audio** — load a WAV/MP3 (→ 22050 Hz mono), see the waveform, drag to crop,
    play, **Save Crop** into `assets/`.
  - **Device** — Ping / List / Push / Push & Launch / Stream Logs / Wipe over USB-CDC.
  - **Console** — live build + device output.

**Native + Python-free.** The Studio reimplements the `mote` CLI's build/scaffold/bake
in C (`studio/motecore.c`) and talks to the board over USB-CDC directly
(`studio/usb.c` — cross-platform: Linux `termios` / Windows Win32 COM, found by VID:PID
`CAFE:4D01`), with a **built-in file browser** (no zenity). It still needs a C compiler
(`gcc`, `arm-none-eabi-gcc`) and, for the Audio tab, `ffmpeg`. "Edit in VS Code" opens
the selected file in your existing VS Code window. Game modules load via a
cross-platform loader (`dlopen` / `LoadLibrary`) — a game is a `.so` on Linux, a `.dll`
on Windows.

**Windows build:** `scripts/build-windows.sh` fetches SDL2-mingw and cross-compiles
with MinGW-w64 → `dist-windows/mote_studio.exe`. SDL2 and the MinGW runtime are
statically linked, so it's a **single file with no DLL dependencies**; drop it in the
repo root and run from there.

---

## 2. Anatomy of a game module

A game is one file, `src/game.c`. It links **no engine code** — it's handed the
engine jump table (`mote`) and the OS drives its vtable.

```c
#include "mote_api.h"
#include "mote_build.h"          // helper layer (mesh primitives, camera, UI)

MOTE_GAME_MODULE();              // declares the `mote` jump-table pointer
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();            // device module entry/relocation glue
#endif

static const Mesh *s_cube;
static Mat3 s_rot;

static void g_init(void) {                       // once, after the engine is ready
    mote->scene_set_background(MOTE_RGB565(10, 12, 26));
    mote->scene_set_sun(v3(0.4f, 0.7f, -0.6f));
    s_cube = mote_mesh_box(mote, 1, 1, 1, MOTE_RGB565(120, 180, 230));
    s_rot = m3_identity();
}
static void g_update(float dt) {                 // per frame: logic + build the draw-list
    const MoteInput *in = mote->input();
    m3_rotate_local(&s_rot, 1, 0.9f * dt); m3_orthonormalize(&s_rot);
    Mat3 cam = mote_camera_look(v3(0,0,0), v3(0,0,1));
    mote->scene_begin(&cam, 60.0f);
    MoteObject o = { .pos = v3(0,0,4.5f), .basis = s_rot, .mesh = s_cube };
    mote->scene_add_object(&o);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update,
    .config = { .max_tris = 256, .depth = 1 },   // declare the pools you use (§4)
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
```

**Vtable callbacks** (all optional except you'll want `update`):

| Callback | When |
|---|---|
| `init()` | once, after the engine + pools are set up. Build meshes here. |
| `update(dt)` | every frame — read input, move state, build the scene draw-list. Runs concurrently with the previous frame's LCD flush. |
| `render_band(fb, y0, y1)` | optional custom raster per row band (both cores call it). Omit to use the built-in scene rasteriser. |
| `overlay(fb)` | 2D HUD drawn on top (core0), after the 3D pass. |

The whole engine is reached through `mote->…`. **Never call engine internals
directly** — only the ABI.

---

## 3. The engine API (`mote->`)

### 3D scene
- `scene_set_background(uint16_t col)` · `scene_set_sun(Vec3 dir)`
- `scene_begin(const Mat3 *cam_basis, float fov)` — start the frame's draw-list
- `scene_add_object(const MoteObject *o)` — `o = { pos, basis, mesh }` (pos is **camera-relative**, see §6)
- `scene_add_object_scaled(o, float scale)`
- `scene_add_sphere(Vec3 pos, float r, uint16_t col)` — a cheap per-pixel **sphere impostor** (no triangles; great for balls, particles, glows)
- `scene_tri_count()` → emitted triangles this frame

### Gaussian splats
- `scene_set_splats(splats, n, order, cam, cam_pos, fov, depth)` — registers a splat set, rendered as a second depth-composited dual-core pass
- `depth_buffer()` → the 3D pass's depth buffer (pass to splats for occlusion)

### 2D
- `scene2d_begin(...)` · `scene2d_set_tilemap(...)` · `scene2d_add(...)` · `blit(...)`

### Physics (impulse rigid bodies)
- `phys_world_defaults(MoteWorld *w)` then set `w->gravity`, `w->walls`/`bmin`/`bmax`, `w->substep`, `w->max_substeps`
- `phys_step(MoteWorld *w, MoteBody *bodies, int n, float dt)`
- `phys_raycast(...)` · `phys_overlap(...)`
- Bodies: `MOTE_SHAPE_SPHERE / BOX / PLANE / HULL`; per-body `inv_mass` (0 = static/kinematic), `restitution`, `friction`, `orient`, `vel`, `w`.

### Input
- `input()` → `const MoteInput *`; `mote_pressed(in, MOTE_BTN_X)` / `mote_just_pressed(in, …)`
- Buttons: `MOTE_BTN_A B UP DOWN LEFT RIGHT LB RB MENU`
- **MENU is yours** — the OS reserves only a **3-second solo MENU hold** for the system menu (§7).

### Audio
- `audio_note(float freq, float amp)` — strike a note on the polyphonic synth
  (piano-ish strike + decay; 8 voices, stolen when busy). Mixed to 22050 Hz — SDL
  on the host, 12-bit PWM (GP23) on the device. `audio_off()` silences everything.
  Master volume follows the engine menu's VOLUME. See `piano3d`.

### Text & telemetry & memory
- `text(fb, str, x, y, col)` · `text_2x(...)` — 8×8 bitmap font
- `micros()` → `uint64_t` · `log(const char *)` · `perf(uint32_t out[6])` (fps, update_us, raster_us, flush_us, c0%, c1%)
- `alloc(uint32_t bytes)` → arena memory (zeroed, NULL on overflow) · `arena_free()` (§4)
- `exit_to_launcher()`

---

## 4. Memory: the load-time arena + `MoteConfig`

There is **one shared 280 KB SRAM arena**. At load, the OS sizes the engine's
pools to *your game's* declared `MoteConfig`; whatever's left, your game claims
via `mote->alloc()`. A lean game keeps the slack.

```c
.config = {
    .max_tris    = 2000,   // 3D triangle draw-list (0 = no 3D raster)
    .max_spheres = 64,     // sphere impostors per frame
    .max_splats  = 0,      // Gaussian splats
    .max_sprites = 0,      // 2D sprites (0 = no 2D scene)
    .max_bodies  = 32,     // physics bodies (0 = no physics)
    .max_contacts= 200,    // physics contact manifolds
    .max_mesh_tris = 0,    // largest mesh collider's tri count
    .depth       = 1,      // 1 = allocate the 32 KB depth buffer (3D / splats)
}
```

- Declare only what you use — `max_tris`/`depth` cost the most.
- If pools + your `alloc()`s exceed the arena, the loader shows an **OUT OF MEMORY**
  screen (which pool to shrink) instead of crashing.
- **LB+RB → … →** the engine menu's perf overlay shows live **ARENA used/total** so
  you can right-size.

---

## 5. The helper layer — `mote_build.h`

Header-only, no ABI cost. Build meshes from **world-unit dimensions + a colour** —
it picks the int8 quantisation scale, winds faces CCW-from-outside, and computes
per-face normals, so you never hit the quantisation/winding footguns. Meshes are
arena-allocated; call these in `init()` and keep the `const Mesh *`.

```c
const Mesh *mote_mesh_box     (mote, hx, hy, hz, col);            // half-extents
const Mesh *mote_mesh_sphere  (mote, r, segs, col);
const Mesh *mote_mesh_cylinder(mote, r, halfh, segs, col);
const Mesh *mote_mesh_revolve (mote, profile, n, segs, col);     // lathe a {radius,height} profile
int         mote_mesh_grid    (mote, nx, nz, x0,z0, x1,z1, heightfn, colfn, user, out[], max, &center);
                                                                 // heightfield → auto-chunked render meshes

Mat3 mote_camera_look(Vec3 eye, Vec3 target);                    // view basis (subtract eye for object pos)

void mote_ui_panel(fb, x, y, w, h, bg, border);                  // tiny immediate-mode UI
void mote_ui_bar  (fb, x, y, w, h, frac, fg, bg);
void mote_ui_rect (fb, x, y, w, h, col);
int  mote_itoa(int n, char *out);                                // → length
```

---

## 6. Rendering conventions

- **Camera-relative world** — the camera is the origin; object positions are
  `world − cam`. View **+Z is forward**.
- **Depth**: `uint16`, **larger = nearer** (`d = K/z`).
- **Winding**: meshes are CCW-from-outside; the projection mirrors them to
  screen-CW front faces. The `mote_mesh_*` helpers handle this for you.
- **Mesh format** (`MeshVert`/`MeshFace`): vertices are `int8` (× a per-mesh
  `scale`); face indices are **`uint8` → a hard 255-vertex cap per mesh**. Large
  models must be split into chunks (the STL baker and `mote_mesh_grid` do this).
- All-float math + `-ffast-math` (RP2350 FPU).
- **Dual-core render**: `update()` builds the draw-list on core0; both cores then
  rasterise their own row band (0–63 / 64–127), lock-free. Don't toggle shared
  render globals mid-frame.

---

## 7. The system layer (free in every game)

- **Engine menu** — hold **MENU alone for 3 s**: a Steam-Deck-style overlay to
  cycle the **performance overlay** (off / FPS / mini-graph / full graphs),
  adjust **brightness** + **volume**, or **return to the lobby**. Short MENU taps,
  sub-3 s holds, and every MENU chord stay free for your game.
- **Hero-menu launcher** — a framed screenshot hero icon + a highlighted browse list.
- **Reusable menus** — `mote->menu(title, items, n)` pops up a modal list menu in
  the same system look (gold title, accent rules, selection bar) and returns the
  chosen index (or -1). Use it for pause / game-over / level-select menus — the
  whole system shares one menu style (`os/mote_ui.c`). See `arkanoid3d`'s game-over.

---

## 8. Assets — `mote bake`

Drop sources in `<game>/assets/` and run `mote bake <game>`; headers land in `src/`.

| Source | Tool | Output |
|---|---|---|
| `*.obj` | `obj2mesh` | `<name>_mesh` (small models that fit ≤255 verts) |
| `*.stl` (binary or ASCII) | `stl2mesh` | `<name>_chunks[]` + `<name>_NCHUNKS` |
| `*.png` / `*.bmp` | `img2tex` (ImageMagick) | `<name>_img` (`MoteImage`, RGB565 + magenta key) + `<name>_W/_H` |

Images bake to a `MoteImage` (RGB565 pixels + a transparent colour-key from the
alpha channel) — load it as a sprite/tilemap (`scene2d_add`) or `blit` it. A sheet
is just one image whose frames you select with the sprite's `fx,fy,fw,fh`. See
`examples/imgdemo` (a baked logo + an animated 2-frame sprite).

`stl2mesh` welds duplicate vertices, **decimates by vertex clustering** (binary-
searched to a triangle budget, default ~1500), and **chunks** into ≤255-vertex
sub-meshes. Render a model by drawing every chunk at one transform — see
`examples/modelview` (a real 6,742-tri fighter → 1,494 tris in 4 chunks).

---

## 9. Gotchas

- **255-vertex cap per mesh** (uint8 indices). Use chunks for anything bigger.
- **int8 vertices** — use the `mote_mesh_*` helpers (they pick the scale); raw
  `*127` quantisation overflows silently if a coordinate exceeds the scale.
- **Arena budget** — `max_tris` × 36 B + 32 KB depth + physics pools must fit
  280 KB. Watch the ARENA line in the perf overlay.
- **A is held on entry** — the launcher's A press carries into your first frame.
  Gate one-shot fire actions behind an "armed" flag set once A is released (see
  the `s_armed` pattern in `fling`/`pong3d`/`arkanoid3d`).
- **Return to lobby is the engine menu only** — games no longer exit on a MENU tap.

---

## 10. Examples

| Game | Shows |
|---|---|
| `hello-mesh`, `tumbler` | minimal mesh + camera (start here) |
| `tetris3d` | grid logic, engine-rendered cubes (~190 lines) |
| `pong3d`, `arkanoid3d` | polished arcade games: trails, particles, power-ups, levels |
| `physics`, `materials`, `playground`, `dominoes`, `hulls` | the rigid-body solver (boxes / spheres / hulls / materials / stacking) |
| `pickups`, `shooter` | `phys_overlap` / `phys_raycast` as game mechanics |
| `golf`, `chess`, `pool`, `fling`, `world` | full games (terrain, AI, splats) |
| `terrain` | `mote_mesh_grid` auto-chunked heightfield |
| `cluster`, `zelda`, `splats` | Gaussian-splat scenes |
| `modelview` | loading a real STL model |
| `piano3d` | the audio synth — a playable 3D keyboard |
| `tiledemo` | the 2D scene + sprites |
| `imgdemo` | loading baked PNG/BMP images (sprites + blit) |

---

## 11. Device build (only when engine C changes)

Games deploy with `mote push` — no reflash. The **firmware** itself (engine + OS +
launcher) is built separately:

```bash
cmake -B build_os -S os/device      # configure (needs the Pico SDK)
cmake --build build_os -j8          # → build_os/mote_os.uf2
```

Flash via BOOTSEL (off → hold a button → on, copy the `.uf2`). Reflash only when
you change engine/OS C code (a new ABI function, the launcher, a driver).

## Layout

```
engine/     the engine — math, render (raster/pipe/splat), physics, core, assets
platform/   host/ (SDL2)   studio/ (IDE backend)   device/ (RP2350: LCD, buttons, audio, USB)
os/         launcher, engine menu, the frame loop + ABI assembly, device store/USB
sdk/        mote_api.h (the ABI) + mote_build.h (the helper layer)
studio/     Mote Studio IDE — main.c (UI), motecore.c (native build/bake), usb.c (device link)
tools/      mote (CLI), obj2mesh, stl2mesh
scripts/    build-windows.sh + mingw-toolchain.cmake (Windows cross-build)
examples/   23 sample games
```
