---
name: mote-game-dev
description: Build games for the Thumby Color handheld with the Mote engine + Studio IDE. Use when creating a new Mote game, writing or editing a game's src/game.c, or working with Mote's engine API (3D/2D scene, physics, audio, input), its asset pipeline (sprites/tiles/anim/mesh/sfx baking), or the device build/push workflow.
---

# Building Mote games

**Mote** is a native C game engine + console OS + IDE for the **Thumby Color**
(RP2350, 128×128 RGB565, dual-core, **272 KB game arena**). Games are plain C modules.
Current engine ABI: **v40** (textured meshes v35, streamed SFX recipes v37, key-value
blob storage v38, proportional `text_font` v39, native low-fi `MoteSound` v40 — see below).

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

### Golden rule: author EDITABLE SOURCE assets + bake — never write headers directly

Every asset must exist as an **editable source file under `assets/`** (or
`tilesets/`, `levels/`) that the developer can open and tweak in the Studio, and be
turned into its `src/<name>.h` by **`mote bake <dir>`** (or Studio Save). **NEVER
hand-write the baked `src/*.h`** (`*_img`, `*.sfx.h`, `*.tiles.h`, `*.level.h`,
mesh/rig headers) and **never procedurally generate assets in C at init** — a baked
header / runtime-built pixels are invisible and uneditable in the IDE. The ONLY
exception is when *procedurally generated content is itself the game's requirement*
(e.g. a noise-driven world); even then, prefer real source assets for the fixed art.

Editable source → baked header (`mote bake` handles all of these):

| Source (`assets/` etc.) | Baked header | Edit in Studio |
|---|---|---|
| `*.png` / `*.bmp` | `<name>.h` (`MoteImage`) | Pixel Art / Texture tab |
| `*.sfx` (SFXR recipe, text) | `<name>.sfx.h` (`MoteSfx`) | Audio tab |
| `*.wav` | `<name>_snd.h` (`MoteSound` PCM) | Audio tab |
| `tilesets/*.tileset` (+ sheet png) | `<name>.tiles.h` (`MoteAutotile`) | Tiles tab |
| `levels/*.level` | `<name>.level.h` | Tiles tab |
| `*.obj` / `*.stl` (+ `*.rig`) | `<name>.h` / `.rig.h` (`Mesh`/`MoteModel`/rig) | Mesh / Rig tab |
| game-root `icon.png` | `src/icon.h` | Pixel Art tab (icon) |

If you must generate art programmatically (PIL etc.) as the *authoring* step, write the
result to the editable source file (the `.png`/`.sfx`), commit that, and bake it — the
deliverable is the editable file, not a C array.

**Texturing a 3D model (v35 textured faces) — from the Studio:** open the OBJ/STL in the
**Mesh** (or **Rig**) view and click **Assign…** → pick a PNG. It's saved as a `<model>.png`
**sidecar** that `mote bake`/`obj2mesh`/`stl2mesh` embeds as the mesh's `texture`, filling
`face_uvs` from the OBJ's `vt` coords (else a triplanar projection); **Clear** removes it.
The sidecar **wins over** an OBJ `.mtl map_Kd`, and the preview shows it live. (No hand-editing
`.mtl` files or typing magic filenames — Assign does the import + the assignment in one step.)

When in doubt, **read `README.md`** (the full engine API + asset pipeline reference)
and **copy the closest `examples/<game>`** — every example is a self-contained,
readable reference (`tetris3d`/`hello-mesh` minimal; `pong3d`/`arkanoid3d` polished
arcade; `chess`/`golf`/`pool` full games; `fling` procedural levels + big terrain +
physics; `tiledemo` layered tiles; `herodemo` sprite animation; `modelview` STL models;
`fxdemo` the FX/billboard/blend toolkit; **`wolfmote` a full Wolf3D-style FPS** (textured
wall/door cubes, billboard enemies + scenery, two weapons, doors, text-map levels, SFX);
`thumbycue`/`indemnity` big games rendered entirely through the built-in engine).

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

MOTE_GAME_META("My Game", "me");   // name (shown in the launcher / .mote filename) + author
```

`MOTE_GAME_META` is the source of truth for a game's name — there is **no `game.toml`**
anymore (the tools fall back to a legacy `game.toml`, then the folder name, only if the macro
is absent). If a model is textured, size `.config.max_tex_tris` (≥ the model's triangle count)
or the engine draws it flat — the Studio warns you when you assign a texture without a budget.

### ALWAYS ship an icon — every game needs `icon.png`

**A game is not done until it has an icon.** Author a **60×60 `icon.png` in the game
ROOT** (next to `src/`, NOT in `assets/`) as part of building any game — treat it as a
required deliverable, same as `game.c`. `mote bake` (and Studio Save) turns it into
`src/icon.h` (a weak `mote_game_icon_data` blob that `mote_build.h` auto-includes via
`__has_include`, so it travels inside the `.mote` with zero boilerplate — do NOT `#include`
it yourself). Without it the launcher just draws a plain name-coloured tile with the game's
initial. Design it to read at launcher size: a bold, high-contrast emblem of the game (a key
sprite/hero/vehicle on a simple background), not a busy screenshot. Author it like any other
sprite — a PIL script writing the editable `icon.png`, then bake — and verify it renders by
capturing the launcher (`./tools/mote run <dir>` with `MOTE_SHOT`, no `MOTE_AUTORUN`). Note:
an `assets/icon.png` only bakes a plain `icon_img` texture — the launcher icon MUST be in the
game root so the icon baker emits the `mote_game_icon_data` blob.

`config` declares the resource pools the OS allocates from the 272 KB arena. Classic
pools: `max_tris`/`max_spheres`/`max_splats`/`max_sprites`/`max_bodies`/`max_contacts`/
`max_mesh_tris`, plus `depth=1` (the 32 KB depth buffer for 3D). The newer per-frame
draw pools (v24–v35): `max_points`/`max_lines`/`max_discs` (FX), `max_tex_spheres`
(sphere impostors), `max_shadows`, `max_rings`, `max_billboards` (3D sprites),
`max_tex_tris` (textured-mesh faces). Size each to what you draw — too small silently
clips, too big wastes arena. **Declare every pool you use** (any non-zero field counts as
"declared"; a fully-zero config is treated as a legacy game and gets a big static
worst-case).

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

**Sprites, textures, FX & blend in 3D** (v24–v35 — all depth-tested, dual-core, metered
by their `config` pool):
```c
// 3D sprite billboard — camera-facing textured quad, sized in world units (enemies,
// pickups, scenery, particles). fx/fy/fw/fh pick a sprite-sheet cell (0 = whole image).
mote->scene_add_billboard(world_pos, &img, fx,fy,fw,fh, world_h, MOTE_BLEND_NONE);
// textured mesh: give a Mesh a `texture` (MoteImage*) + `face_uvs` (nfaces*6 bytes,
// per-corner u,v 0..255) and draw it through scene_add_object — UV-mapped + sun-lit,
// perspective-correct. (Winding is irrelevant to the textured raster; only the face
// NORMAL drives backface cull + lighting — handy for hand-built textured cubes.)
mote->scene_add_sphere_tex(world_pos, r, &orient, &tex);   // textured/shaded sphere (mote_sphere.h)
mote->scene_add_point(p,col,size); mote->scene_add_line(a,b,col); mote->scene_add_disc(p,r,col);
mote->scene_add_ring(p, r, col);                           // camera-facing circle outline
mote->scene_add_tri(a,b,c, col, 0);                        // double-sided immediate world tri
mote->scene_add_shadow(ground_pos, r, strength);          // soft ground shadow (_ex = oval)
mote->set_background_cb(bg_fn);                            // per-band sky/floor/gradient drawn before the scene
mote->scene_set_near(0.08f);                               // INDOOR/close scenes: shrink near plane or walls clip
mote->scene_add_object_ex(&obj, MOTE_DRAW_BLEND(MOTE_BLEND_ALPHA)); // translucent mesh (water/glass); or MOTE_DRAW_NO_DEPTH_WRITE
```
Blend modes (`MOTE_BLEND_NONE/ALPHA/ADD`) apply to billboards, `blit_ex`, and meshes
(`MOTE_DRAW_BLEND(mode)`). `MOTE_BLEND_ADD` = glows/lasers/muzzle flashes.

**2D framebuffer drawing + rotated blit** (HUDs, overlays, sprites at any angle):
```c
mote->draw_pixel/draw_line/draw_rect/draw_circle(fb, ...);            // pass band 0,128 in overlay()
mote->blit(fb,&img,x,y, fx,fy,fw,fh, MOTE_SPR_HFLIP, 0,128);          // axis-aligned
mote->blit_ex(fb,&img, cx,cy, fx,fy,fw,fh, angle, scale, MOTE_BLEND_ADD, 0,128); // rotate+scale
```

**Save + rumble** (v23): `mote->save(slot,data,len)` / `load(slot,buf,max)` / `save_slots()`
(survives power-off); `mote->rumble(intensity, ms)`. **Master volume** (v29):
`audio_set_master(v)` / `audio_get_master()` — the one knob the engine menu + every game
share (route a game's volume option here). **Key-value blobs** (v38):
`mote->kv_save(key,data,len)` / `kv_load(key,buf,max)` / `kv_list(prefix,cb,arg)` — named
persistent blobs beyond the fixed save slots, for variable/large data keyed by name (e.g.
per-chunk world edits, ThumbyCraft's chunk store). On ThumbyOne these land on the shared FAT.

**Input** (read via helpers, never poke the struct):
```c
const MoteInput *in = mote->input();
if (mote_pressed(in, MOTE_BTN_RIGHT)) ...        // held
if (mote_just_pressed(in, MOTE_BTN_A)) ...       // edge this frame
// buttons: A B UP DOWN LEFT RIGHT LB RB MENU  (MENU long-hold is reserved by the OS)
```

**Audio** — paths, all end at the mixer:
```c
mote->audio_play_sfx(&coin_sfx, 1.0f);           // v37 RECOMMENDED: STREAM a MoteSfx recipe — synthesised
                                                 //   on the fly, ~88 B flash, ~0 RAM, up to 8 at once.
                                                 //   Ship a game's whole SFX set this way.
mote->audio_note(440.0f, 0.85f);                 // synth tone, one per event
MoteSound coin = mote_sfx_bake(mote, &coin_sfx); // bake a recipe to PCM at load (zero per-sample synth
mote->audio_play(&coin, 1.0f);                   //   cost — only for games firing SO many SFX it matters)
mote->audio_set_stream(fill_fn);                 // v36: register your own PCM source the mixer pulls each block
#include "hit.h"                                  // v40: a baked WAV (mote bake / Audio tab) -> a MoteSound kept
mote->audio_play(&hit_snd, 1.0f);                //   at the WAV's NATIVE quality (8/16-bit, source rate); the
                                                 //   mixer resamples+expands on playback. 8-bit/11025 = ~1/4 flash.
```
The synth is single-precision (fixed in 0.8); the recipe path is cheap, but a game with
*dense* simultaneous polyphony (a bullet-hell, a fast break) can still saturate it — bake
to PCM (or use a cheap fixed-point synth via `audio_set_stream`) in that case.

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
- **Memory is the ceiling, not framerate**: ~5,700 textured tris hold 60 fps on device;
  the arena (272 KB) runs out around ~7,000 tris first. Size the pool to what you draw.
- **Bake before build**: `mote build`/`run` compile whatever headers exist — bake the
  asset first (Studio Save auto-bakes; `mote bake <dir>` for the CLI). The `.h` is committed.
- **Transparency is the PNG ALPHA channel.** The baker keys `alpha < 128` to the magenta
  colour-key (`0xF81F`) and marks the image keyed; a fully-opaque image bakes `opaque=1`
  (no key). **Author sprites with a transparent background — do NOT fill it with magenta
  RGB** (that bakes as a solid colour + `opaque=1`, so nothing is transparent). Wall/floor
  textures stay fully opaque on purpose.
- **Examples ship REAL editable asset FILES** (PNGs, `*.sfx.h`, STL) for every
  sprite/texture/sound — **never procedurally generate art in C at init**. The whole point
  is the dev opens the example in the Studio and tweaks the assets visually; runtime-built
  pixels are invisible/uneditable there. (Authoring a PNG with a script is fine — the
  deliverable is the editable file under `assets/`.)
- **Indoor / close-up scenes need a smaller near plane**: the 0.5 m default clips geometry
  you stand next to (walls, a snooker table). Call `mote->scene_set_near(0.05..0.1)` in init.
- **256 verts per render mesh**: `MeshFace` indices are `uint8`, so one `Mesh` caps at 256
  verts. For bigger geometry (e.g. terrain) split it into chunks that share one center +
  scale — shared-edge verts then quantise identically, so they stitch with no cracks. A
  **physics collider** has no such cap: a single `MOTE_SHAPE_MESH` `MoteMesh` uses `uint16`
  indices, so one mesh can both *be* the ground collider and feed the render chunks (`fling`/`golf`).
- **Generate geometry into game-owned static buffers, not the arena**: the arena is
  bump-only (no per-alloc free), so calling `mote_mesh_*` every frame/level leaks it. For
  geometry you rebuild (per-level terrain, etc.) keep static arrays and refill them in place.
- **Don't fight the engine**: prefer the `mote_build.h` helpers (mesh builders, particles,
  RNG, clamps) over hand-rolling. Match the surrounding example's style.

## Where to look

`README.md` is the authoritative reference — §3 anatomy, §4 asset pipeline, §5 engine
API (5.1 3D, 5.2 2D/tiles/anim, 5.3 physics, 5.5 input, 5.6 audio, 5.8 helpers),
§7 memory, §11 gotchas. Headers: `sdk/mote_api.h` (the ABI — every `mote->` call +
`MoteConfig`), `sdk/mote_build.h` (inline helpers), `engine/render/mote_sphere.h`
(`MoteSphereTex` shade modes), `engine/render/mote_object.h` (`MOTE_BLEND_*` / draw
flags), `engine/assets/mote_mesh.h` (`Mesh.texture`/`face_uvs`), `sdk/mote_tile.h`,
`sdk/mote_anim.h`. The rendered API ref is [`docs/index.html`](../../docs/index.html)
(austinio7116.github.io/mote). When stuck, read the nearest `examples/` game.

> **Maintenance:** when the engine ABI/API changes, update this skill alongside
> `docs/index.html` + `README.md` §5 + `CHANGELOG.md` (the version line at the top here too).
