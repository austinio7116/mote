# Changelog

## 0.9-alpha

**Smoother sound, and you can now texture a 3D model right in the Studio.** The on-the-fly
sound-effect synth now does its maths in single precision (with a small lookup table for
sine and vibrato) instead of double precision — which is what the device's chip is fast at —
so the framerate no longer drops when a lot of sounds play at once. In the IDE you can now
pick a PNG and apply it to a model from the Mesh and Rig views and see it textured live.
**Reflash the firmware** to get the sound change on the device; games already on the device
keep working.

### Audio

- **Sound-effect synth is now single precision.** The synth that generates sound effects
  as they play was doing double-precision maths on every sample, which is slow on the
  device's single-precision chip. It now uses single precision throughout, with a lookup
  table for sine and vibrato. This fixes the framerate dropping when many sounds play at
  once.
- **Indemnity Run uses its own original sound engine again.** It went back to the cheap
  fixed-point synth it shipped with, instead of the streamed recipe synth — its original
  sounds, very low CPU cost, and tiny in flash. No size regression.

### Engine

- **A textured model can no longer silently vanish.** If a game declares no textured-triangle
  budget (`MoteConfig.max_tex_tris = 0`) but a model is textured, the engine now draws it flat
  (in the texture's average colour, baked in for it) and logs a one-time hint — instead of
  dropping every textured triangle, which left the model invisible with no feedback.

### Studio (the IDE)

- **Assign a texture to a 3D model, in the GUI.** The Mesh and Rig views now have an
  **Assign…** button (and **Clear**): pick a PNG, and it's applied to the model and shown
  textured live in the preview. The bake embeds it for you automatically. Works for OBJ and
  STL models. Under the hood, the image you pick is saved as a `<model>.png` file next to the
  model; the baker embeds that and it takes priority over an OBJ's own `.mtl` texture
  reference. OBJ models use the texture coordinates from the model; STL and decimated models
  get a triplanar projection so they texture cleanly without any.
- **The texture preview is live.** The Mesh/Rig preview re-reads a model's texture the instant
  the file changes — assign, edit-and-save, repaint, clear — so there's no stale "Texture:
  none" and no need to reselect the model.
- **It warns before a texture goes missing in-game.** If you texture a model whose game has no
  textured-triangle budget, the texture row says so and suggests a value, so you know to raise
  `max_tex_tris` (and the Inspector now shows a game's pool/arena summary for `game.c`, not just
  a manifest).
- **The Console catches everything.** A running game's `log()` output, engine warnings, and
  game-load failures now stream into the Console instead of the terminal.
- **Push shows live progress.** Uploading a game over USB now streams a percentage in the
  Console as it goes, instead of looking frozen on a big upload.
- **Mesh bakers rebuild when edited.** A baker is recompiled when its source (or the shared
  texture embedder) changes, so editing one doesn't silently re-bake with a stale tool.
- **Windows build fixed.** A build error that broke the Windows Studio build is resolved.

### Projects

- **`MOTE_GAME_META` replaces `game.toml`.** A game's name and author now live in `game.c`
  — `MOTE_GAME_META("My Game", "me");` — the single source of truth, right next to the code.
  New projects no longer create a `game.toml`; existing projects keep working (the tools fall
  back to a `game.toml` name, then to the folder name).

### Examples

- **ThumbyCue: breaking no longer tanks the framerate.** The opening break used to drag the
  framerate down; the physics step is now capped per frame and the hot physics code runs from
  fast SRAM, so the break stays smooth.
- **ThumbyCraft: world saves no longer cause periodic stutters on the device.** It now writes
  the world only when you save (or once enough edits have built up), instead of on a repeating
  timer — so there's no regular hitch while you play.

## 0.8-alpha

**Sound effects got much smaller, and the whole Mote console now runs as a single tile
on ThumbyOne.** The headline change: a game's hand-tuned sound effects no longer have to
be baked into big blocks of raw audio — the engine can now *play the recipe directly*,
synthesising each sound on the fly. Indemnity Run's sound dropped from ~620 KB of baked
audio to a few KB of recipes, shrinking the whole game from **958 KB to 328 KB** with no
audible change. ThumbyCraft now **saves your world** on the device too. **Reflash the
firmware** — the engine interface version goes 36 → **38**; games built against it need the
new firmware, and games already on the device keep working.

### Audio

- **Streamed SFX recipes (`audio_play_sfx`).** `mote->audio_play_sfx(&recipe, gain)` plays
  an editable ~88-byte SFX recipe by generating the sound as it plays — tiny in flash and
  almost no memory, any length, up to **8 sounds at once**. This is now the recommended way
  to ship a whole game's sound effects (a weapon rack, UI blips), instead of baking each one
  to a raw clip. Baking to a raw clip is still available for the rare game that fires so many
  sounds at once it needs zero synthesis cost.
- The Studio Audio tab and `mote bake` now point you at the recipe path, and the Studio's
  preview plays a sound exactly as the device will.

### Mote as a ThumbyOne slot

- The entire Mote console — engine, launcher and game library — now runs as **one slot** on
  ThumbyOne, in place of three separate game slots. Games live as `.mote` files in a visible
  **`/mote/`** folder on the shared drive; drag them on over USB and they appear in the Mote
  launcher.
- It runs as **two images** so a game gets the most memory: a small launcher picks a game,
  and a separate engine image runs it with the launcher and USB out of the way.
- A game runs **straight from the drive** with no copying, which needs its file laid out on a
  4 KB boundary — so the shared drive now uses 4 KB clusters.
- **Live logs to the IDE during a game** — turn on *USB LOGS* in the engine menu and a game's
  `mote->log()` lines stream to the IDE over USB. It's off by default so normal play has zero
  overhead. The launcher also accepts `mote push` over USB to drop a game straight into `/mote/`.

### Saves

- **Per-game saves on a real filesystem.** A game's `mote->save`/`load` slots are now files
  under `/mote/saves/<game>/` — each game gets its own folder, so two games can't clash, and
  saves survive reboots and firmware updates. (The old scheme wrote raw flash at a fixed
  address that, in a ThumbyOne build, overlapped the shared drive.)
- **Named-blob storage (`kv_save`/`kv_load`/`kv_list`, ABI v38).** For games that persist
  *many* pieces rather than one record — arbitrary keyed blobs of any size, file-backed.
- **ThumbyCraft now saves your world.** Built on the above: the player record rides a save
  slot, and every edited voxel chunk is a blob — so your builds, chests and torches come back
  exactly as you left them.

## 0.7-alpha

**ThumbyCraft — a full Minecraft-style voxel sandbox — now runs on Mote**, and getting
it there pushed the engine in ways that help every game. ThumbyCraft is a big, demanding
game (a 256 KB voxel world, its own raycaster, procedural worlds with eight biomes, mobs,
redstone, day/night, crafting, its own music) — so it became the stress test that proved
how far a Mote game can go, and drove three reusable engine additions. There's also a new
paper.io-style game (papermote) and the command-line tool now bakes every asset type the
Studio does. **Reflash `firmware_mote_os.uf2`** — the engine interface version goes
35 → **36**; games built against it need the new firmware, and games already on the device
keep working.

### New games

- **ThumbyCraft** — the full voxel sandbox: infinite procedural terrain, eight biomes,
  mobs, redstone, torches and lighting, day/night, crafting and combat. It fills both of a
  Mote game's memory budgets and runs at the same framerate as its original standalone
  build. See the new memory section in the README for how it fits.
- **papermote** — a paper.io-style territory game: claim ground by closing loops, steal
  (and destroy) rivals by enclosing or cutting them, with ten pickable creature sprites,
  AI opponents, three game modes, and a procedural music bed that builds as you dominate.

### Engine / ABI

- **A game can feed its own audio stream** (`audio_set_stream`, ABI v36): register a
  callback that fills mono 16-bit samples at 22050 Hz; the engine mixes it on top of its
  own synth voices. For games with their own software synth (full music + effects) instead
  of the built-in note/sample API — like ThumbyCraft. Cleared automatically on exit.
- **Run hot code from RAM** (`.ramtext`, ABI v36): a game can mark its hottest functions
  (e.g. a raycaster inner loop) to run from SRAM instead of flash, so they don't lose the
  cache to large texture reads. The loader copies them in at launch. This took
  ThumbyCraft's raycaster from 8–12 fps to 12–20 fps; backward-compatible with older games.
- **More room for big games:** a game's static RAM grew 128 → 134 KB (reclaimed from unused
  OS heap), and audio now stays glitch-free even on heavy frames — the engine keeps the
  sound buffer topped up *during* the frame's render, not just once per frame.

### Tools & Studio

- **`mote bake` now bakes every asset type the Studio does** — images, `.sfx` recipes,
  `.wav`, tilesets, levels and animations — so the command line and the IDE produce
  identical headers.
- **Per-game `cflags`** — a game folder may include a `cflags` file (one flag per line,
  `#` comments allowed) added to both the host and device compiles, for ports that need
  build-time defines.
- **Studio:** console text is now selectable for copy/paste, `.sfx`/`.sfx.h` files load
  into the Audio panel, and the arena budget meter covers all resource pools.
- **fling:** zoom the camera in/out with LB/RB. **Host:** a frame-sequence recorder
  (`MOTE_REC`) for capturing gameplay.

## 0.6-alpha

**The big games now draw through the built-in engine.** ThumbyCue and Indemnity Run
used to carry their own custom rendering code; now they draw the same way any small
game does — by calling the engine. To make that work, the engine gained the drawing
features those games needed (textured/numbered balls, lit planets, shadows, particle
and beam effects, circles, sky backgrounds), so every game can use them too.
**Reflash `firmware_mote_os.uf2`** — the engine interface version goes 23 → **35**, so
games built against it need the new firmware; games already on the device keep working.

### Engine / ABI

New, reusable rendering primitives (each opt-in and metered via `MoteConfig` pools, so
they cost nothing unless declared — static OS-BSS growth stayed ≈ 0):

- **3D scene FX** (depth-tested, dual-core): `scene_add_point`, `scene_add_line`,
  `scene_add_disc` (v24); `scene_add_ring` — a camera-facing circle outline for ghost
  balls/reticles (v31).
- **`scene_add_tri`** (v27) — a double-sided immediate-mode world triangle for dynamic /
  procedural geometry that isn't a baked `Mesh`.
- **`scene_add_sphere_tex`** (v25) — a textured *or* per-pixel-callback-shaded, oriented
  sphere impostor (numbered/striped balls, lit textured planets), with FLAT/LIT/SMOOTH/
  TOON/GLOSS/CUSTOM shade modes (`mote_sphere.h`).
- **`scene_add_shadow`** / **`scene_add_shadow_ex`** (v28/v32) — soft ground-shadow
  decals, round or oriented-oval (object-shaped), depth-tested.
- **`set_background_cb`** (v26) — a per-band background pass (starfield/nebula/gradient)
  run before the scene; **`scene_set_near`** (v28) — runtime near plane for small scenes.
- **`scene_add_object_ex`** + `MOTE_DRAW_NO_DEPTH_WRITE` (v24) — coplanar overlays.
- **2D framebuffer drawing**: `draw_pixel`, `draw_line`, `draw_rect` (v30), `draw_circle`
  (v31) — for HUDs/overlays/backgrounds, alongside `text`/`blit`.
- **Engine master volume**: `audio_set_master` / `audio_get_master` (v29) — one knob the
  engine menu and every game share, so in-game volume and the system menu stay in sync.
- **3D sprites (billboards)**: `scene_add_billboard` (v33) — a camera-facing textured
  quad sized in world units, depth-tested, for trees, pickups, enemies, smoke and
  explosions.
- **Textured meshes** (v35) — a `Mesh` can now carry a `texture` + per-corner `face_uvs`
  and is drawn UV-mapped (still sun-lit) through the normal `scene_add_object`.
- **Rotated/scaled 2D sprites**: `blit_ex` (v34) — draw a sprite at any angle and zoom
  (spinning pickups, HUD dials), not just axis-aligned like `blit`.
- **Blend modes** (v33): `MOTE_BLEND_ALPHA` (~50%) and `MOTE_BLEND_ADD` (additive) for
  billboards, `blit_ex`, and whole meshes via `MOTE_DRAW_BLEND()` — glows, lasers, water,
  glass, force-fields.

### Games

- **Indemnity Run** and **ThumbyCue** render through the engine directly (no
  `render_band`): ships/stations/table → `scene_add_object`/`scene_add_tri`, planets/balls
  → `scene_add_sphere_tex`, FX → point/line/disc, sky → `set_background_cb`. They share
  the engine's `mote_vec.h`/`mote_mesh.h`; the private `r3d_*` renderers were deleted.
- **pool** — textured/numbered balls + soft shadows, and a fixed table (real cushions,
  no z-fight flicker; the table now draws double-sided so the cushions show).
- **Soft shadows** added to `tanks` (oriented oval), `chess`, `dominoes`, `golf`.
- New **`fxdemo`** example showcasing the FX, impostor, shadow, ring and background
  primitives, plus the new 3D-sprite billboards, a textured mesh, a rotating additive
  `blit_ex` HUD sparkle, and additive/alpha blending.
- New **`wolfmote`** example — a Wolfenstein-3D-style FPS built entirely on the engine:
  textured wall + door cubes (textured meshes), billboard enemies (two types, with
  aim/fire/hit/dead frames), billboard pickups + scenery (barrels, lamps, pillars,
  plants), a 2D `blit_ex` gun viewmodel with an additive muzzle flash, two weapons
  (pistol + chaingun), doors you open with B, three hand-authored text-map levels with
  an exit + level progression, and `MoteSfx` sound effects.
- Both new examples (**`fxdemo`**, **`wolfmote`**) now ship launcher **icons**.

### Fixed

- **Audio after the engine menu** — the menu's blocking loop left the device audio dead;
  it now re-arms on close, and in-game volume + the menu drive the one engine master.
- **Perspective-correct textured triangles** — textured meshes (and the wolfmote walls)
  no longer skew/swim up close; the raster now interpolates `u/z`, `v/z`, `1/z`.
- **Config-pool detection** — a game declaring *only* the newer pools (`max_tex_tris`,
  `max_billboards`, etc.) was misread as an undeclared legacy game and piled the static
  worst-case (3328 tris + physics) on top, overflowing the arena. The loader now treats
  any declared pool as an explicit config.
- The load arena is **272 KB** (was 276) to fit the new OS state.

## 0.5-alpha

A big engine-API release: persistent saves and rumble, plus a third large game port.
**Reflash `firmware_mote_os.uf2`** — the ABI bumps to 23 and games built against it are
refused by older firmware.

### New game

- **Indemnity Run** — a full port of the bare-metal Elite-style space sim (~20k lines):
  flight, combat, trading, galaxy, the lot, on the dual-core `render_band` rasteriser.
  Its big render buffers live in the arena; every weapon and sound is an editable, baked
  SFX clip; uses the new save + rumble APIs.

### Engine / ABI (v23)

- **Persistent save.** `save(slot, data, len)` / `load(slot, data, max)` / `save_slots()`
  — per-slot blobs that survive power-off (device: flash sectors that even outlast an OS
  reflash; host/Studio: files).
- **Rumble.** `rumble(intensity, ms)` pulses the motor (device: GP5 PWM, eased-out;
  host/Studio: no-op).
- The load arena is **276 KB** (trimmed from 280) to make room for the new OS state —
  immaterial to games (the heaviest example uses ~254 KB).

### Audio: prefer baked clips (and a footgun fixed)

- **Guidance + guard rails** so a game's SFX don't silently eat arena RAM: `mote_sfx_bake`
  is documented as costing RAM per sound; the right path for a sound *set* is the baked
  `<name>_snd` clip (flash, 0 RAM) played with `audio_play`. The Studio save hint, the
  README audio section, and the OUT-OF-MEMORY screen all now steer you there.
- **Studio Audio tab**: applying a seed preset or **Randomize** no longer renames the
  sound you loaded — Save writes back to the same `.sfx`/`.sfx.h`.

### Fixed

- Clean build console — suppressed the noisy `format-truncation` / `unused-result`
  warnings in game builds and in Studio itself (both build paths).

## 0.4-alpha

Two new example games and a batch of Studio/tooling improvements. The device firmware is
updated (reflash `firmware_mote_os.uf2`), but the ABI is unchanged (22) — existing games
keep running without a rebuild.

### New games

- **ThumbyCue** — a full, faithful port of the 3D snooker & pool game, running on the
  engine's dual-core full-screen rasteriser (`render_band`): table, spinning textured
  ball impostors, physics, rules, AI and HUD all intact, with sampled hit/pot SFX.
- **Nightmote** — a real-time horde survivor (auto-attacking weapons, XP gems, a
  level-up build with weapons + passives, scaling difficulty and a boss). A compact
  showcase of the 2D sprite scene, WANG16 autotiled ground, baked SFX recipes and the
  overlay HUD — all native Mote.

### New and improved

- **Use the C standard library in games.** `snprintf` / the `printf` family (for
  formatting HUD strings), plus the usual string/math, now link on device — the build
  supplies the libc syscall stubs. (Use `mote->alloc` for memory, not `malloc`.) See the
  new note in §3 / the API reference.
- **Toggle Chassis** (Studio ▸ View) — switch the emulator between the solid and a new
  clear/translucent Thumby Color shell.
- **Import assets** (Studio ▸ Assets ▸ Import) — pick a file and it's copied straight
  into the project's `assets/` (and shown in the tree), instead of an info message.
- **Panel separator highlight** — dragging the IDE's panel splitters now lights up on
  hover, so the affordance is visible even where the OS resize cursor isn't (e.g. WSLg).
- **Higher-resolution Studio chassis** photo (4×).

### Fixed

- **pong3d** — paddles flash brighter on contact instead of morphing into a sphere.
- **piano3d** — the press-glow and cursor-marker spheres sit at the front of the keys
  (toward the camera) instead of being hidden behind the long key tops.
- **Device builds** now link the libc syscall stubs in *both* build paths (the `mote`
  CLI and the Studio's own builder), so a game using `snprintf` builds either way.
- `mote bake` no longer emits a redundant plain-image header for a tileset's sheet PNG.

## 0.3-alpha

This release updates the device firmware — reflash `firmware_mote_os.uf2`, and rebuild
your games so they pick up the smaller launcher icons.

### New and improved

- **3D model animation.** Models can now be rigged into moving parts and animated: build
  a rig, pose it on a keyframe timeline, bake a clip, and trigger it from game events
  (e.g. a gun firing) — all authored in the Studio Rig tab. See the new "Creating rigs
  and animations" guide.
- **Rig editor.** A 3-axis on-model manipulator to move/rotate parts and place pivots, a
  scrubbable keyframe timeline with draggable keys, and ROTATE/MOVE inputs. "View as mesh"
  opens a rigged model in the Mesh tab.
- **Tanks example.** Rebuilt with detailed 3D tanks (tracks + road wheels, sloped hull,
  rounded turret, muzzle), team colours with dark tracks/gun, and a barrel that recoils
  via a baked animation clip on every shot.
- **Smaller launcher icons.** Icons are compressed in flash — roughly half the space
  (much less for simple ones), with no visible change.
- **Better Open Project screen.** Each game shows its icon, an estimated memory bar, and
  a proper scrollbar.
- **Edit the game icon in the IDE.** Draw or import the launcher icon right in the Pixel
  Art editor (Assets ▸ Edit Icon) — it bakes automatically.
- **Pixel/Texture sizes.** More canvas sizes (including 60 for icons) plus a −/+ for any
  size, keeping your art when you resize.
- **Optional frame-rate cap** a game can set, honoured by both the device and the emulator.
- **File manager in the tree.** Right-click to New File / New Folder / Rename / Delete
  (with a confirm), and double-click a folder to collapse/expand it. The tree shows
  subfolders; clicking a `.sfx` opens the Audio tab and a `.rig` the Rig tab.
- **Mousewheel zoom** in the Mesh and Rig 3D previews.
- **Consistent naming dialog** for New/Rename/Save As (click-to-select-all), and the file
  picker opens in the current project's folder.
- **Windows fast-update** dev workflow (`scripts/sync-windows.sh`) — update an unzipped
  bundle in place instead of re-unzipping.

### Fixed

- The Mesh/Rig 3D preview is sharp again, no longer squashes on wide windows, and the
  left/right orbit drag is no longer inverted; the rig view no longer auto-spins.
- The rig editor is editable the moment a model loads (a rest keyframe is seeded), so
  pose values and the manipulator work right away.
- "VS Code", and "Reveal in Files" / "Open Folder", now work on Windows.
- Launcher icons are handled by the build — a game no longer needs to include anything,
  so they can't go missing (the tanks icon was hitting this).
- Assets placed in subfolders are now built too.

## 0.2-alpha

This release updates the device firmware — reflash `firmware_mote_os.uf2`, and
rebuild your games so they pick up the new per-game icons.

### New and improved

- Each game now carries its own launcher icon, so adding a game no longer means updating the firmware.
- The device holds and shows many more games — up to 56, was 24.
- Creating a game gives you a wizard: pick a starter template (3D, physics, or 2D) with sensible memory settings already filled in.
- The **fling** example is now a full game — procedurally generated levels, large rolling terrain, and taller, more varied forts.
- Animation files are much smaller: only the frames a clip actually uses are saved.
- More games have proper icons, taken from a screenshot of the game.
- Mote Studio shows its version under Help ▸ About.

### Fixed

- Switching between projects now refreshes the Tiles, Anim, and Mesh tabs.
- Listing games on the device no longer cuts off when you have a lot of them.
- **fling** forts no longer fall over before you take a shot, and the aim dots are now easy to see against the sky.

## 0.1-alpha

First public alpha: the native C engine, console OS, and Studio IDE for the Thumby Color.
