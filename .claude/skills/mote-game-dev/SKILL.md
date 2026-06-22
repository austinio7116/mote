---
name: mote-game-dev
description: Build games for the Thumby Color handheld with the Mote engine + Studio IDE. Use when creating a new Mote game, writing or editing a game's src/game.c, or working with Mote's engine API (3D/2D scene, physics, audio, input), its asset pipeline (sprites/tiles/anim/mesh/sfx baking), or the device build/push workflow.
---

# Building Mote games

**Mote** is a native C game engine + console OS + IDE for the **Thumby Color**
(RP2350, 128×128 RGB565, dual-core, ~280 KB game arena). Games are plain C modules.

## The fast workflow: IDE for assets, Claude (you) for code

The intended loop — and the one to recommend — splits cleanly:

- **Mote Studio (the human) authors assets** and bakes them to headers: sprites
  (Pixel Art tab), procedural textures, **tiles/levels** (Tiles tab — rule-tile
  autotiling + layered levels), **sprite animation** (Anim tab), **3D models**
  (Mesh tab — STL → decimated/chunked `MoteModel`), and **audio** (Audio tab — SFXR
  synth + WAV import). Each bake writes a `src/<name>.h` the game `#include`s.
- **You (Claude) write/edit `src/game.c`** — the game logic, drawing, and wiring the
  baked assets. Then the human hits **Save** in Studio and it hot-reloads instantly.

So: don't hand-roll pixel art or tile LUTs in C — tell the human to author it in the
Studio tab and `#include` the baked header. Spend your effort on `game.c`.

When in doubt, **read `README.md`** (the full engine API + asset pipeline reference)
and **copy the closest `examples/<game>`** — every example is a self-contained,
readable reference (`tetris3d`/`hello-mesh` minimal; `pong3d`/`arkanoid3d` polished
arcade; `chess`/`golf`/`pool` full games; `tiledemo` layered tiles; `herodemo` sprite
animation; `modelview` STL models).

## Game skeleton

```c
#include "mote_api.h"
#include "mote_build.h"        // header-only helpers (no ABI cost)
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static void g_init(void)         { /* set background/sun, build/load meshes & sounds once */ }
static void g_update(float dt)   { /* read input, step state, submit the scene every frame */ }
static void g_overlay(uint16_t *fb) { /* draw HUD text/rects directly into the framebuffer */ }

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 400, .max_spheres = 32, .depth = 1 },   // size the pools you use
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
```

`config` declares the resource pools the OS allocates from the ~280 KB arena
(`max_tris`/`max_spheres`/`max_splats`/`max_sprites`/`max_bodies`/`max_contacts`/
`max_mesh_tris`, `depth=1` adds the 32 KB depth buffer for 3D). Size them to what you
draw — too small clips, too big wastes arena.

## The engine is immediate-mode

There is **no retained scene graph**. Each frame in `update()` you start the scene
and re-submit everything you want drawn; the draw-list is cleared for you next frame.
Keep your own state (positions, etc.) and draw from it — moving an object is just
changing your variable; hiding it is not calling draw. It holds 60 fps at thousands
of triangles, so re-submitting the whole scene every frame is the intended pattern.

## API cheat-sheet (`mote->...`; helpers in `mote_build.h`)

**3D scene** — world-space camera, then draw:
```c
mote->scene_set_background(MOTE_RGB565(10,12,26));
mote->scene_set_sun(v3_norm(v3(0.4f,0.7f,-0.6f)));            // once, in init
Mat3 basis = mote_camera_look(cam_pos, target);
mote->scene_camera(&basis, cam_pos, 60.0f);                   // each frame
mote_draw(mote, mesh, world_pos);                             // or _ex(pos,basis,scale) / _tint(...,color)
mote_model_draw(mote, &model, pos);                           // a baked STL MoteModel (all chunks)
mote->scene_add_sphere(v3_sub(p,cam_pos), r, col);            // cheap lit sphere impostor
const Mesh *box = mote_mesh_box(mote, hx,hy,hz, col);         // runtime mesh builders (call in init)
```
Mesh colour lives on the `Mesh` (`color` / optional `face_colors[]`); a per-draw
`MoteObject.color` (via `*_tint`) overrides it.

**2D scene** — sprites + tilemap/autotile (great for top-down/platformers):
```c
mote->scene2d_begin(cam_x, cam_y);
world_draw(mote);                                             // baked layered autotile level
MoteSprite s = { .img=&img, .x=px,.y=py, .fx=frame*16,.fy=0,.fw=16,.fh=16, .layer=10,
                 .flags = facing_left ? MOTE_SPR_HFLIP : 0 };
mote->scene2d_add(&s);
```

**Input** (read via helpers, never poke the struct):
```c
const MoteInput *in = mote->input();
if (mote_pressed(in, MOTE_BTN_RIGHT)) ...        // held
if (mote_just_pressed(in, MOTE_BTN_A)) ...       // edge this frame
// buttons: A B UP DOWN LEFT RIGHT LB RB MENU  (MENU long-hold is reserved by the OS)
```

**Audio** — three paths, all end at the mixer:
```c
mote->audio_note(440.0f, 0.85f);                 // synth tone, one per event
MoteSound coin = mote_sfx_bake(mote, &coin_sfx); // bake a MoteSfx recipe at load (tiny flash)
mote->audio_play(&coin, 1.0f);                   // or a baked WAV MoteSound
```

**Text / control** (overlay + engine):
```c
mote->text(fb, "SCORE", x, y, MOTE_RGB565(250,230,90));   // also text_2x; mote_textf for printf-style
mote->micros();  mote->log("...");  int i = mote->menu("Pause", items, n);
```

**Animation** (`sdk/mote_anim.h`, header-only): play a baked `MoteAnimClip` with a
`MoteAnimPlayer` — `mote_anim_play`, then per frame `mote_anim_tick(&p, dt)` and read
`mote_anim_fx/fy` into a `MoteSprite`.

## Build / run / deploy

```bash
./tools/mote build examples/mygame      # host .so
./tools/mote run   examples/mygame       # build + run in the SDL emulator
./tools/mote push  examples/mygame --launch   # cross-build .mote + upload over USB
./tools/mote studio                      # the IDE (recommended)
```
Host emulator keys: W/A/S/D = d-pad, `.` = A, `,` = B, Shift = LB, Space = RB,
Enter = MENU. Headless capture: `MOTE_SHOT=1 MOTE_SHOT_FRAME=20` dumps a frame.

## Gotchas that will bite

- **ABI version**: changing the engine's shared structs (`mote_api.h` `MOTE_ABI_VERSION`)
  means the device needs a **reflashed OS firmware**; header-only/SDK changes do not.
  Game code rarely touches this — just match the SDK you build against.
- **`max_tris` is the limit, not framerate**: a `depth=1` game has ~248 KB of arena for
  the draw-list → ~7,000 triangles max; framerate gives out before that. Size the pool.
- **Bake before build**: `mote build`/`run` compile whatever headers exist — bake the
  asset first (Studio Save auto-bakes; `mote bake` for the CLI). The `.h` is committed.
- **Transparency** is the magenta key `0xF81F` (`MOTE_KEY_MAGENTA`); alpha<128 in source
  art bakes to it.
- **Don't fight the engine**: prefer the `mote_build.h` helpers (mesh builders, particles,
  RNG, clamps) over hand-rolling. Match the surrounding example's style.

## Where to look

`README.md` is the authoritative reference — §3 anatomy, §4 asset pipeline, §5 engine
API (5.1 3D, 5.2 2D/tiles/anim, 5.3 physics, 5.5 input, 5.6 audio, 5.8 helpers),
§7 memory, §11 gotchas. Headers: `sdk/mote_api.h` (the ABI), `sdk/mote_build.h`,
`sdk/mote_tile.h`, `sdk/mote_anim.h`. When stuck, read the nearest `examples/` game.
