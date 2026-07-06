# Changelog

## 0.16-alpha — the Gallery, and a real UI font

**Install and update games straight from the gallery — on the PC *and* on the handheld —
and the whole UI reads properly now.** This is an integrated update: new **ThumbyOne
firmware 1.33.0**, a new **Mote Studio (0.16-alpha)**, and engine **ABI v46**. Reflash the
firmware; single-player games built against v45 still run (v46 is backward-compatible), but
per-game *versions* only show for games rebuilt with the new SDK.

### The Gallery
- **Browse, install and update games from an online gallery** — no manual file copying.
  Studio fetches the catalog and each game directly from GitHub Pages over HTTPS and keeps
  one on-disk cache that both the PC UI and the docked device read, so every game is
  downloaded at most once per version.
- **On the handheld too:** press **RB** in the Mote slot (docked to Studio over USB) to open
  a gallery browser on the device itself — a hero view per game with its screenshot, size,
  and an install/update state chip. Installs are written **in place** with a journal + marker,
  so an interrupted install can never leave a half-written game that launches.
- **Per-game versions** (ABI v46): each game embeds a version string in its module header, so
  the gallery can tell *New* from *Installed* from *Update available* by comparing versions.
- **On-device gallery UX:** the selected game's screenshots auto-advance as a slideshow with
  the next shot preloaded (seamless, no spinner flicker); long titles rock back and forth as a
  ticker instead of wrapping; **LB** opens a scrollable description; **B** cancels a slow load
  instead of waiting for a timeout.

### A new UI font (device + Studio)
- The device UI (launcher, gallery, engine menu) now renders in **Audiowide** — an
  anti-aliased proportional font baked at three sizes (medium/reading/large). The old 3×5
  bitmap was hard to read on the picker and gallery; text is now crisp and properly sized.
  The glyph tables are `const` (flash only) — **zero extra RAM**.
- The **engine overlay menu** (PERF / BRIGHT / VOLUME / USB LOGS / lobby / resume) was
  widened and moved onto the new font — legible at a glance.
- **Mote Studio** matches: Audiowide is the default UI face, with **View → "Hybrid Body
  Font"** to switch the dense body text back to DejaVu while keeping the Audiowide chrome
  (menu bar, tabs, section headers). The choice persists; `MOTE_STUDIO_UIFONT=<ttf>` overrides
  the face entirely. The code editor stays monospace.

### Rig / 3D-animation editor
- **Keyframes now persist.** Authored clips were previously kept only in memory — switching
  from RIG to another tab (which rebuilds the rig) or reopening the project **wiped every
  keyframe**; the only durable output was the generated `.anim3d.h`, which was never read back.
  Studio now writes an editable `<model>.anim` sidecar (clip length/loop + per-key Euler+pos,
  and each key's snap/linear flag) on Save, on Bake, and when you leave the RIG tab, and
  restores it on open — so a clip survives tab switches and reopens and stays re-editable.
- **Snap vs. linear keyframes.** Each keyframe now has an interpolation mode — a **key: snap /
  key: linear** toggle in the RIG inspector (snap keys draw as squares on the timeline, linear
  as diamonds). *Snap* holds the pose until the next key then jumps; *linear* eases between
  them. No more faking a hold with a pair of near-identical keys. The mode bakes into the clip
  (new trailing `step` field on `MoteModelKey`) and is honoured live by `mote_anim3d.h`; older
  baked clips are unaffected (missing field ⇒ linear).
- **Manipulator no longer moves/rotates the wrong way.** The gizmo was drawn along *world* axes
  while a part's translation and rotation live in its **parent** frame, so parented or rotated
  parts translated and spun opposite to the handle you grabbed. The gizmo axes and rotate rings
  now align to the part's parent basis, so dragging always follows the handle. The rotate-ring
  drag also picks its sign from whether the axis faces the camera, fixing the rotation that
  "went the complete opposite way" depending on the viewing angle.
- Dragging a translate/rotate handle now snaps the playhead to the edited key, so the change is
  visible immediately (matches the +/- steppers).

### Firmware / SDK
- **ThumbyOne firmware 1.33.0** — adds the on-device gallery slot and the AA UI fonts
  (lobby +~9 KB, runner +~7 KB of flash; no RAM change). Reflash `firmware_thumbyone.uf2`.
- **`MOTE_GAME_VERSION("x.y.z")`** in the SDK stamps a game's version into its module header
  (ABI v46); `tools/gen_gallery.py` merges those into `docs/games.json`. `game.toml` is gone —
  version + metadata live in the game and the gallery manifest.

## 0.15.1-alpha — multiplayer, fixed for real play

**Two-player now works reliably — over a USB cable or over the internet.** The four newest
2-player games each had a bug that hit every connection type (fixed below), and linking two
real devices directly on a cable could also lose data and desync the two screens (fixed in
the firmware). Reflash both units; ABI unchanged (**v45**), so single-player games are
untouched and the three older 2-player games (Wolfmote, MotoKart, DeepThumb) need no
rebuild — they just play more reliably on the new firmware.

### All 2-player games (firmware fix)
- Linking two devices directly with a cable used to lose data under load — matches wouldn't
  sync, links dropped, or the two screens disagreed. Fixed in the engine's link layer, so
  every 2-player game benefits at once. (It never showed against Studio / internet play,
  which is why the initial release looked fine there.)

### The four newest 2-player games
- **ThumbyCue** — turns hand off properly: no more breaking off, fouling, and both players
  stuck on "PEER'S TURN". The watching player now also sees the balls **spin** as they roll,
  instead of sliding.
- **Grand Thumb Auto** — both players are in the exact same city (the two maps could differ
  before); you can jack **any** moving car, not just parked ones; you start next to a car;
  and a stolen car actually drives and keeps its speed. Single-player now gets a random
  starting car too.
- **Indemnity Run** — duels start instead of hanging on the loading screen and timing out
  (your opponent used to fly on autopilot); ships collide instead of passing through; you
  choose which saved ship to bring (or get a random balanced ship with no save); and a
  damage warning that stuck on screen is cleared.
- **PaperMote** — the AI bots move and appear in the same place for both players, and
  crossing your opponent's trail always crashes them (it used to sometimes pass through).

## Unreleased

**Two players, one cable.** Engine ABI **v42 → v43**: a 2-player link — reflash both
units to use it.

### Engine (ABI v43 — reflash firmware)
- **2-player link**: `link_start/stop/status/is_host/send/recv` — a raw byte pipe to a
  second unit. On device it's USB CDC **dual-role**: both units flip randomly between
  USB device and USB host roles until one enumerates the other over a single USB-C
  cable (the proven TinyCircuits engine_link scheme). On the host emulator it's a local
  socket — run two instances with the same `MOTE_LINK_SOCK` and they connect, so 2P is
  testable headlessly on one machine. `link_is_host()` is 1 on exactly one side (use it
  to assign white / server). While started, the link owns the USB controller; the
  CLI/log channel yields and returns on `link_stop` (also called on game exit).
- RAM cost on device: the TinyUSB host stack adds ~2 KB net (paid for by trimming the
  unused OS heap) — the **277 KB game arena and the module boundary are unchanged**.

### Studio
- **LAN LINK** (DEVICE tab): connect two Studios across the network — `Host LAN` on one,
  `Join LAN` on the other (zero-config UDP discovery on the subnet; set
  `MOTE_LINK_PEER=<ip>` before launch for a fixed address). Two consumers:
  - **Preview games**: a game running in each Studio's preview links up as if cabled.
  - **Bridge USB**: relays a USB-connected Thumby's 2P-link bytes over the pipe — two
    REAL devices, each docked to a Studio, play each other remotely.
  The LAN session survives game restarts/hot reloads; `MOTE_LINK_HOST=1` /
  `MOTE_LINK_JOIN=<ip|auto>` autostart it.

### Games
- **WolfMote 2P DEATHMATCH** (title menu): same seeded dungeon on both units, no
  monsters, treasure becomes weapons/health/ammo, supplies respawn, every door opens.
  The other player is a live billboard avatar (fire/hit frames, red dot on the automap);
  hits are shooter-decided, health is victim-authoritative, first to 10 frags.
- **GrandThumbAuto 2P DEATHMATCH** (B on the title): the nonce winner rolls the CITY
  seed and shares it — same procedural city on both units, with traffic/pedestrians/
  police removed and 40 respawning weapon caches dotted around. Parked cars are shared
  by index (enter/exit replicates); shoot, burn, blast or RUN OVER the other player.
  The city map shows the opponent as a flashing red dot, edge-clamped so it always
  points the hunt. First to 10 frags.
- Both cap at 30 fps while linked so pacing matches across units.
- **Studio: Vs Device** (DEVICE tab) — link the PREVIEW game to your USB-docked Thumby
  over a local serial pipe: test any 2P game against a real device with one PC.
- **DeepThumb 2P LINK**: pick `OPP: 2P LINK` on both units in the setup screen, connect
  the cable (or bridge over the LAN), and sides are drawn at connect — the hello carries
  a random nonce and the higher one plays white (transport-agnostic: over the Studio
  bridge both devices are USB device-role, so `link_is_host()` can't break the tie).
  Hello handshake (a PC can't be mistaken for an opponent), 5-byte move messages,
  disconnect ("LINK LOST") and quit ("OPPONENT LEFT") handling; undo/save are disabled
  in link games. Also now paces itself at 30 fps.

## 0.14-alpha

**The model editor grows up.** Multi-part models are first-class - a parts list with
hide/rename, per-material import, and a calmer sidebar - plus tooltips across the whole
Studio and a 2D-physics collision fix. **Engine ABI unchanged (v42)** - existing games
and firmware still match; reflash only to pick up the physics fix on device.

### Engine (no ABI change - reflash to get it on device)
- **phys2d: collisions no longer "teleport".** Positional depenetration ran inside the
  velocity-iteration loop, multiplying the push-apart by iterations x contacts - fast,
  deep crashes hurled bodies apart. It's now one gentle pass per contact (sensors
  excluded). Anything using `phys2d_step` (driving games!) feels dramatically better.

### Studio
- **Objects tab in the model editor.** The MODEL EDITOR card gains **Tools | Objects**
  tabs. Objects lists every part of the model as a tree - colour swatch, name, vert/face
  counts, `M` badge on mirrored parts. Click a row to make it the active part, click the
  **eye** to hide a part while you edit another (hidden parts are skipped by picking and
  the viewport but still bake/save/export), **double-click to rename** (names feed baked
  headers + rigs).
- **Multi-material OBJs import as separate parts.** An `.obj` whose parts are `usemtl`
  materials (no `o`/`g` groups - e.g. a chess piece with `body` + `accent`) now imports
  one object per material, named after it and coloured from the `.mtl` `Kd` - matching
  how the baker chunks it. Group-split imports also take per-face material colours now.
- **Clicking a file in the Explorer shows THAT file.** Two "prefer the live model"
  guards fired for every `.obj` once any scene existed - clicking queen.obj showed the
  live king (in the Rig tab AND the Mesh preview). They now apply only to the live
  model's own file. **"Edit this mesh" imports the file you're previewing** when it
  isn't the current scene's source, and a **"Re-import parts"** button refreshes a
  scene from its own (multi-part) file.
- **A calmer model-editor sidebar.** Collapsible sections (SELECT / ADD / EDIT / FACES /
  TEXTURE / BOOLEAN / OBJECT / FILE - fully folded, the whole toolset is visible with no
  scrolling), true segmented pickers for Vert/Edge/Face and Solid/Wire, and Lucide icons
  on the buttons that were a wall of text (6 new glyphs: eye, eye-off, rotate-cw,
  scaling, copy, trash-2).
- **Hover tooltips everywhere.** The 3D editor's tooltip system now covers the whole
  app: toolbar, Pixel/Texture strip, the shared pixel side-panel, Mesh/Rig cards,
  Tiles, Anim, Font, Audio and the Device panel.
- New capture/test hooks: `MOTE_STUDIO_OPEN` (drives the real Explorer routing),
  `MOTE_STUDIO_MESHTAB`, `MOTE_STUDIO_MESHSEC`.

## 0.13-alpha

**Smaller games, richer physics.** Indexed textures and native-quality sounds cut a
typical game's flash by half or more; a new 2D rigid-body solver joins the 3D one; and
the Audio tab gains a Tone view for layered synth SFX. The engine ABI moves **v39 →
v42** — **ships in ThumbyOne firmware 1.30** (flash that to run games built with this
SDK).

### Engine (ABI v42 — firmware reflash required)
- **2D rigid-body physics** (`mote_phys2d`): a top-down planar solver alongside the 3D
  one — circles + oriented boxes, impulse collisions with restitution and Coulomb
  friction, one rotational DOF (bodies pick up spin from off-centre hits), sequential
  impulses, AABB broad phase, sleeping, collision-group masks, sensors, and an
  **anisotropic lateral friction** term (`lat_damp`) that gives cars tyre grip and
  drift for free. Engine-run via one new ABI call (`phys2d_step`); bodies are plain
  game-owned structs (`MoteBody2D` / `MoteWorld2D`) with header-only constructors.

### Tools / SDK (no ABI change)
- **Multi-part OBJ models with per-part colour.** An `.obj` with several materials now
  bakes (obj2mesh, via `mote bake` + Studio Save) to a single `MoteModel` with **one
  chunk per material**, each part coloured from its `.mtl` `Kd` — so a piece modelled as
  one file with `body` + `topper` materials is one clean model in the tree, not two.
  obj2mesh never recentres, so the parts stay aligned on the OBJ's shared origin (base at
  `y=0` just works — no flag/sidecar). New header helper `mote_model_draw_palette(model,
  pos, basis, scale, parts, n)` **recolours each part at draw time** (`0` = keep baked),
  so one model serves both teams / chess sides with no extra assets. Single-material OBJs
  are unchanged (`<name>_mesh`). *(Migrated `games/motokart`'s banana/kshell to the model
  form; `games/deepthumb` king/queen are now single OBJs.)*

### Engine (ABI v40–v41 — firmware reflash required)
- **Indexed (palette) textures.** `MoteImage` gains an optional **4-bit or 8-bit
  palette-indexed** format: a texture with few colours costs **1/4** (≤16 colours) or
  **1/2** (≤256) the flash of RGB565, decoded to RGB565 by a palette lookup at sample
  time. It works for **every** texture path — meshes, sprites, blits, billboards. Old
  `{px,w,h,key[,opaque]}` images are unchanged (format 0 = RGB565). The bakers now do it
  **automatically and losslessly**: `mote bake` / the Studio image bake emit indexed when
  an image has ≤256 colours (else RGB565), and the model editor's textured **Bake .h**
  emits a 4bpp palette (median-cut to 16 colours). E.g. Thumbalaga's enemy sheet went
  **45 KB → 11 KB**, the spaceship texture **32 KB → 8 KB** — no visible change. Detailed
  / gradient art (>256 colours) stays RGB565. *(Re-bake a game with `mote bake <dir>` to
  shrink its low-colour art.)*
- **Low-fi sounds.** `MoteSound` now carries a sample **rate** and **bit depth**, so a
  clip is stored at its native quality — 8-bit and/or sub-22050 Hz — and the mixer
  resamples + expands it on playback. An 8-bit/11025 Hz SFX costs ~1/4 the flash of the
  old 16-bit/22050 form. `wav2snd` (`mote bake` + the Studio Audio tab) now reads the
  WAV's `fmt` chunk and bakes at the source quality (8/16-bit, source rate; only
  downsampled if it exceeds 22050; any bit depth / stereo accepted). The API is
  unchanged — `audio_play(&snd, gain)` still works; legacy `{pcm,count}` headers read as
  16-bit/22050. *(Previously `wav2snd` assumed 16-bit/22050 and baked 8-bit or
  odd-rate WAVs to noise.)*

### Studio + SDK (no reflash)
- **Tone SFX — layered synth sounds you author in the IDE.** A new **Full / Tone**
  toggle in the Audio tab. The **Tone** view builds a sound the way Indemnity Run does:
  a stack of cheap synth voices (Square / Saw / Sine / Noise, a frequency sweep, amp,
  attack, length) with a **live preview**, exported as a `MoteTone[]` the game plays with
  one call. This is the light middle ground between baked WAVs (large flash) and the full
  recipe synth (heavy DSP — what slowed games down). Powered by a new header-only
  **`sdk/mote_synth.h`**: `#define MOTE_SYNTH_IMPL`, wire `audio_set_stream(mote_synth_render)`,
  and `mote_synth_tone(snd, snd_N)`. Costs ~0 flash and ~0 RAM.
- **SFX quality override.** `MOTE_SFX_RATE` / `MOTE_SFX_BITS` force `mote bake` (and the
  Studio's WAV baker) to a target quality — e.g. `MOTE_SFX_RATE=11025 MOTE_SFX_BITS=8
  mote bake <game>` bakes a game's SFX at 1/4 the flash **without editing the source WAVs**.
  Applied across thumbatro / deepthumb / arkanoid3d / fling (~120 KB reclaimed).

## 0.12-alpha

**Texture-paint your models, plus booleans and view modes.** A Studio (dev-tools)
release — **no firmware reflash needed**; the engine and its ABI (v39) are unchanged, so
games already on the device keep working. Full guide in the README (§4 *Texturing a
model* and the *Walkthrough*).

### Studio
- **Region inset.** Insetting a selection of faces now insets the whole region's outer
  boundary as one (sharing the interior), so a quad made of two triangles insets within
  the quad instead of each triangle being inset separately. A single face is unchanged.
- **UV texture painting.** Unwrap a model (box-projection) into a paintable atlas, then
  paint it in a split view — the live textured 3D model beside the atlas — with the full
  pixel toolset (pencil / brush / eraser / fill / pick / line / rect, sizes, palette, HSV)
  and Undo/Redo. Paint on the atlas **or directly on the 3D model** (the cursor raycasts to
  the surface; strokes land on both at once, with the brush cursor mirrored onto the UV
  map). Choose the atlas resolution (64 / 128 / 256) without re-unwrapping, or **Fill from
  face colours** to seed the atlas from per-face paint (with a few pixels of edge bleed so
  islands meet cleanly). A freshly-created atlas is solid grey.
- **Textured bake.** Bake .h now emits a textured `MoteModel` — the atlas as a `MoteImage`
  plus per-face UVs (0–255) — and a flat fallback colour (the atlas average) so a textured
  model never renders black when a game lacks the textured-tri pool. On a textured bake the
  Studio auto-adds `.max_tex_tris = <model>_TRIS` to the game's `game.c` config (never
  overriding a value you set yourself).
- **Booleans (CSG).** Union / Subtract / Intersect two objects via BSP CSG (robust on any
  closed mesh); the active object is `A`, a stepper picks the target `B`.
- **Apply Mirror.** Bake the live-mirrored half into real geometry and turn the modifier
  off — for editing both halves independently, or before a boolean.
- **View modes.** Solid (with per-pixel hidden-line removal) / Wireframe / X-ray, toggled
  in the editor's *View* row or with `Z` / `Shift+Z`.
- **Multi-object editing.** Select-All and click-select are scoped to the active object, so
  an overlapping object can be selected and moved on its own; a discoverable model header
  with **+ New model** and a model switcher; and **Spin / Texture / Reset view / Bake .h**
  controls on the non-edit model preview card.

## 0.11-alpha

**A full 3D model editor, and a much better Pixel Art tab.** This is a Studio (dev-tools)
release — **no firmware reflash needed**; the engine and its ABI (v39) are unchanged, so
games already on the device keep working.

**A model editor in the Mesh tab.** The Studio Mesh tab now has a built-in
Blender-style low-poly modeller alongside the STL/OBJ importer — press **Tab** to
switch. Build a model from primitives (cube / plane / cylinder / cone / sphere) or
turn an imported STL/OBJ into editable topology, then select vertices / edges /
faces and shape it with familiar modal tools. Bake the exact result to a
`MoteModel` header, a multi-part `MoteRig`, or export an OBJ + `.rig` for the Rig
tab. Studio-only — no firmware reflash needed. Full guide in the README
(§4 *Modelling in the Mesh tab*).

- **Select.** Vertex / edge / face modes (1 / 2 / 3); click / Shift-add / drag a box /
  **A** all / **Alt+A** none, plus **Invert** (Ctrl+I), **Linked** island (L), and
  **Grow** / **Shrink** (Ctrl +/−).
- **Transform.** **G** move, **R** rotate, **S** scale, **E** extrude, **I** inset —
  each with **X/Y/Z** axis locks, exact numeric entry, and a drag-able 3-axis gizmo.
  **Ctrl+Z** undo throughout.
- **Build geometry.** **+Face** (F) makes a face from 3–4 selected verts, **Connect**
  (J) splits a face between two verts, **Subdivide** selected faces, **Bridge** two
  faces with a band of quads, **Separate** selected faces into a new object — plus
  Duplicate (Shift+D), Delete (X), Merge verts (M) and Flip normals (Shift+N).
- **Repair topology.** **Recalc outward** orients normals to face out; **Clean**
  (Ctrl+K) welds doubles, removes non-manifold faces and reorients — for cleaning up
  imported or hand-edited meshes.
- **Set origin.** Move an object's origin to the selection or its bounding-box centre
  (the geometry stays put) — handy for sane rig pivots.
- **Per-face Paint** (P) with an in-editor HSV picker (colours bake into a
  `face_colors[]` array), and **Live Mirror (X/Y/Z)** — model one half, the whole
  bakes watertight. Great for symmetric ships and characters.
- **Multiple named models per project.** Each model is a named `<name>.mmesh`; **New**
  (Ctrl+N) names a fresh one, the file tree lists them, and every bake/export follows
  the name (`src/<name>.h`, `<name>_rig.h`, `assets/<name>.obj`).
- **Multi-object import.** Importing an `.obj` with several `o`/`g` groups brings each
  group in as its own editable object, so a multi-part model stays riggable.
- **Edit imported models / bake / export.** Load an `.stl`/`.obj`, pick a triangle
  budget, **Edit this mesh**; **Bake .h** (a `MoteModel`), **Bake rig** (a `MoteRig`,
  one part per object), or **Export OBJ** + `.rig` to animate in the Rig tab.
- **A tidier, scrollable sidebar.** The MODEL EDITOR panel is organised into labelled
  groups (Select / Add / Transform / Edit / Faces / Object / File), scrolls when it
  overflows the dock, and every button shows a hover tooltip.
- **Switching projects now resets every tab** — model objects, the pixel-art and
  texture canvases, tilesets, animations, rig, SFX and font — and loads the new
  project's model, so you never see the previous game's assets.

### Pixel Art

- **Bigger, non-square canvases.** Up to 256×256 with independent width and height
  (the square presets still set W = H, and there are separate W and H − / + steppers).
  Imports keep their real size instead of being forced to a 128×128 square.
- **A real soft brush.** Soft edges now use a proper opacity layer, so a low-hardness
  brush feathers smoothly and overlapping strokes in one drag don't darken or
  saturate. Soft pixels show over a checker and **save as real PNG transparency**.
  There's one brush tool with a **square / round** shape toggle (the separate square
  tool is gone) and a proper brush icon.
- **Undo on the Pixel Art tab.** **Ctrl+Z** undo, **Ctrl+Shift+Z** / **Ctrl+Y** redo.

## 0.10-alpha

**Proper fonts.** You can now draw crisp, anti-aliased, proportional text at any size —
bake a TrueType font, or hand-draw and edit one glyph by glyph right in the Studio, and
draw it in your game with one call. The new `fontdemo` example is a gallery you can flip
through with A / B. **Reflash the firmware** to use the new text in games on the device;
games already on the device keep working, and games that don't use the new text are
unaffected.

### Fonts

- **`mote->text_font()` draws an anti-aliased proportional font.** Each glyph keeps its
  own width and position, so text looks like real type instead of a fixed grid. The
  built-in 3×5 `mote->text()` is still there for tiny UI labels.
- **Two ways to make a font, one way to draw it.** Bake a TrueType `.ttf` (the Studio
  rasterises it), *or* hand-draw a font as a pixel sheet — either way you get a `MoteFont`
  you draw with `text_font`.
- **Fonts are small.** Glyph coverage is packed at the smallest depth that's lossless —
  1-bit for a plain on/off font, 2-bit, or 4-bit (16 levels) for anti-aliased — picked
  automatically by the baker. A typical font is a few kilobytes.

### Studio (the IDE)

- **A Font tab.** Import a `.ttf` (or pick a bundled starter font), set the pixel size,
  see a live preview, and bake it to a header your game includes. Six redistributable
  starter fonts ship in the picker (sans, serif, mono, Ubuntu, a calligraphic Chancery).
- **Edit glyphs in place.** "Edit glyphs" shows every glyph in a grid (like the tileset
  editor) with a pixel editor beside it — paint the selected glyph in its cell with a
  grayscale coverage ramp (white = solid, grey = a soft anti-aliased edge), transparency
  shown as a checkerboard. Guides mark the **baseline**, the **pen origin** (paint left of
  it to make letters connect) and the **advance**. The whole font is one PNG that saves
  and bakes in place — it never touches your Pixel-Art canvas.
- **Resize is safe.** Changing a font's size is preview-only and applies when you Bake;
  if you've hand-edited a glyph sheet it warns before re-rendering from the source.
- **The built-in font is editable too.** The 3×5 system font is available as an editable
  glyph sheet so you can fork and tweak it.

### Engine

- **Hand-drawn fonts match baked TrueType fonts exactly.** A glyph sheet now records the
  pen origin and each glyph's real advance, so cursive/connected scripts keep their
  letter joins through the editor — editing or resizing a font no longer flattens it.

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
