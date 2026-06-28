# MotoKart

A Mario-Kart-style 3D racer for the Thumby Color, built on the Mote engine.

![grid](../../docs/img/motokart-grid.png)

## Play

```bash
./tools/mote run examples/motokart        # SDL emulator
./tools/mote push examples/motokart --launch   # USB -> device
```

**Controls** (emulator key in brackets):

| Action  | Button      |
|---------|-------------|
| Throttle| `A` (`.`)   |
| Brake / reverse | `B` (`,`) |
| Steer   | `LEFT`/`RIGHT` (`A`/`D`) |
| Drift (hold while steering → mini-turbo) | `RB` (`Space`) |
| Use item| `LB` (`Shift`) |

On the title screen press **LEFT/RIGHT** to pick one of three circuits —
**Forest Hills**, **Dust Valley**, **Frost Peak** — each with its own track
shape, colour theme and roadside scenery, and **UP/DOWN** to choose a difficulty
(**Easy / Medium / Hard** — the AI go faster, corner harder, and get less catch-up
help as it rises). Then race 3 laps against 5 colour-coded AI racers (corner-aware:
they brake for bends, drift the hairpins, and grab items).
Drift corners to charge a mini-turbo, and beat your best lap (saved across power-offs).

**Power-ups** — drive through the rows of item boxes (4 across the road, at 5
points each lap). What you get is weighted by your position — leaders get the weak
stuff, stragglers get the big guns:

| Item | Effect |
|------|--------|
| 🍄 **Mushroom** / **Triple** | instant speed burst (triple = 3 charges) |
| 🍌 **Banana** / **Triple** | drop a spin-out hazard behind you (triple = 3) |
| 🟢 **Green shell** | fires straight down the track |
| 🔴 **Red shell** | homes on the kart ahead |
| ⭐ **Star** | invincible + fast; spins out anyone you touch |
| 🚀 **Bullet Bill** | auto-pilot rocket — flies you down the line, invincible |
| ⚡ **Lightning** | shrinks + slows every opponent briefly (no spin) |
| 🍄 **Mega Mushroom** | grow huge, squash rivals, immune to hits |

Use the held item with **LB**.

## How it's built (engine techniques)

- **Tracks** — three closed-loop spline circuits (one `Map` table drives the
  shape, palette and scenery) rendered as an *immediate-mode* triangle ribbon
  (`scene_add_tri`) with self-computed sun shading. No baked mesh → crisp
  geometry at any scale; only the segments in view are submitted each frame.
- **Karts** — one **multi-part OBJ + rig** (`assets/kart.obj` + `assets/kart.rig`
  → `MoteRig kart_rig`, editable in the Studio **Rig tab**). Parts: `hull`,
  `trim`, and four `wheel_*`. Drawn with `mote_rig_draw_locals_palette` so the
  **hull is tinted per-racer** while trim/wheels keep their baked colours, and
  the wheels are **animated procedurally** — all four roll with distance and the
  front pair steers (and leans into drifts). Oriented to heading + pitch, rolled
  for lean/bank.
- **Billboards** — drivers (colour-coded helmets), trees, item boxes, the finish
  banner and corner signs are all `scene_add_billboard` quads.
- **FX** — ground shadows (`scene_add_shadow`), drift-spark particles
  (`scene_add_point`), additive boost glow (`scene_add_disc`), a sky/horizon
  gradient (`set_background_cb`).
- **Audio** — a custom engine drone via `audio_set_stream` (a tiny saw+noise
  synth pitched to the player's speed) plus `audio_play_sfx` recipe SFX for
  boost / hit / item / lap / countdown.
- **Physics** — analytic *rail-relative* arcade model (on-road grip vs grass
  slip, drift + mini-turbo, kart-vs-kart push-apart), AI that races the spline
  with look-ahead steering + rubber-banding.

## Assets (edit in Mote Studio, then Save / `mote bake`)

Everything under `assets/` is an editable source baked to `src/*.h`:

| Source | Edit in Studio tab |
|--------|--------------------|
| `kart.obj` + `kart.mtl` + `kart.rig` | Rig (parts, pivots) |
| `banana.obj` / `shell.obj` (+ `.mtl`) | Mesh (item props) |
| `driver.png` `tree.png` `itembox.png` `banner.png` `sign.png` | Pixel Art |
| `*.sfx` | Audio |
| `icon.png` (game root) | Pixel Art (icon) |

`tools/gen_assets.py` is the one-shot authoring script that wrote the source art;
re-run it only to regenerate from scratch, then `./tools/mote bake examples/motokart`.
