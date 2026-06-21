# Mote Studio + Mote for VS Code — the host dev environment

Two complementary ways to build Thumby Color games, both thin wrappers over the
same `mote` CLI + engine so they never diverge:

- **Mote Studio** — a bespoke **native C/SDL2 desktop app** (one codebase, builds
  for Linux/Windows/macOS). The *visual creation hub*: game library, **pixel-art
  studio**, the **embedded emulator inside a Thumby Color shell**, asset
  import/preview, a visual engine/arena config builder, and device/USB/log
  management. It does **not** ship a code editor — an "Edit code" button opens the
  project in VS Code. Hand-rolled UI on SDL2, no heavy frameworks.
- **Mote for VS Code** — a lightweight extension for the code-first path: scaffold
  / build / run / push / logs as commands, mote-API IntelliSense, a device-log
  channel, optional emulator webview. The "I live in my editor" option.

Status: **planning + Phase 1 build starting.** Stack confirmed: native C/C++ +
SDL2, fully hand-rolled UI, VS Code for code editing.

---

## 1. Principles

- **Bespoke + native.** Hand-built C/C++ + SDL2 (already our host dep). No
  Electron/Tauri/Rust/WASM. Compile per platform.
- **The emulator IS the engine.** What you see is exactly what ships.
- **Reuse the toolchain.** Builds via `gcc`/`mote build`; assets via the existing
  `obj2mesh`/`stl2mesh`/`img2tex` bakers; USB via the existing push/logs protocol.
- **Don't rebuild a code editor.** Delegate to VS Code (and ship a VS Code
  extension for the code-first crowd).

---

## 2. Architecture

```
            ┌──────────────────────  Mote Studio (native C/C++ + SDL2)  ──────────────────────┐
            │  Hand-rolled UI (panels, widgets, device shell, pixel canvas) on SDL2 + GL       │
            │                                                                                  │
            │  Library │ Pixel-Art Studio │ EMULATOR (device shell) │ Assets │ Config │ Device │
            └──────┬──────────────┬───────────────────┬───────────────────┬──────────┬────────┘
                   │              │                   │                   │          │
        opens →  VS Code      paints PNG         platform/studio      shells the   USB protocol
       (edit code)            → assets/ → bake   backend (offscreen    bakers       (push/logs/
                                                 present + buttons)                  launch)
                   │                                   │                                  │
            ┌──────▼──────────────────────────────────▼──────────────────────────────────▼──────┐
            │  the REAL engine (portable C) + the game .so (dlopen, hot-reloaded on save)         │
            └─────────────────────────────────────────────────────────────────────────────────────┘

   Mote for VS Code (extension)  ─── commands ───►  the same `mote` CLI  (new/build/run/push/logs)
```

The engine already has one platform seam (`mote_platform.h`: host SDL, device
RP2350). Add **`platform/studio/`**: `present` blits the 128×128 RGB565 fb into an
offscreen texture the Studio draws (in the shell, ≤2× pixels); `buttons` reads a
bitfield the Studio fills from keyboard/gamepad/on-screen clicks; audio reuses the
SDL audio path. The game loads via `dlopen` exactly like `mote_host`. So the whole
`mote_os_run` loop — launcher, engine menu, `mote->menu`, audio — runs embedded.

---

## 3. Mote Studio modules

1. **Game Library** — visual grid of projects (baked icon, name, size, last edit):
   open · duplicate · rename · delete · reveal · **Edit in VS Code** · **Push to
   device**. A second tab mirrors what's installed on the device.
2. **New-Game wizard** — pick a template (blank / 3D-mesh / 2D-sprite / physics /
   audio, seeded from `examples/`), name, starting `MoteConfig`; wraps `mote new`.
3. **Embedded emulator (centrepiece)** — an illustrated **Thumby Color body** with
   the screen inset at **1× or 2× pixels max**; on-shell buttons (A/B/D-pad/LB/RB/
   MENU) that **light when pressed**; input from **keyboard (remappable) + Web—no,
   SDL gamepad + on-screen clicks**; transport (play/pause/reset/step), live FPS +
   the engine perf overlay; **hot-reload** on file save (rebuild `.so`, swap module).
4. **Pixel-Art Studio** — hand-rolled canvas in the **RGB565** space with the
   engine's **magenta transparent key**: pencil/fill/line/rect/eraser/picker/select,
   zoom/pan, **frames + onion-skin + animation preview**, layers, palettes →
   export PNG to `assets/` → auto-bake to a `MoteImage`. Import PNG/BMP to edit.
   (Stretch: a tilemap painter.)
5. **Asset pipeline / Import** — drag-drop **PNG/BMP** (→ `*_img`), **OBJ/STL** (→
   mesh; STL shows a **tri-budget slider + live 3D preview** via the embedded
   engine), future **WAV**. An Assets panel: thumbnail + flash size per asset.
6. **Config / Arena builder** — visual `MoteConfig` pool sliders with a live
   **arena-budget meter** (used / 280 KB, red when over).
7. **Device / USB / Logs** — detect **CAFE:4D01**, push / push-&-launch, manage the
   on-device store, **stream live logs + profiler**, a guided **firmware-flash**
   helper, connection status.
8. **Build/Deploy bar + console** — one-click Build · Run · Push; parsed errors.

UI is hand-rolled immediate-mode on SDL2 (+ a small GL/`SDL_Renderer` backend):
panels, buttons, sliders, lists, tabs, drag-drop, the pixel canvas, the device
shell. Text *display* only (labels/console) — no in-app code editing.

---

## 4. Mote for VS Code (the light alternative)

A TypeScript extension that shells the `mote` CLI:
- Commands / palette + buttons: **New · Build · Run (emulator) · Push · Push&Launch
  · Logs · Bake assets**.
- **IntelliSense**: ship a `c_cpp_properties.json` snippet pointing at `sdk/` +
  `engine/` includes so `mote->`, `mote_mesh_*`, etc. autocomplete via the C/C++
  extension; bundle hover docs generated from `mote_api.h`/`mote_build.h`.
- **Device logs** in an Output channel; a status-bar connection indicator.
- Optional: an **emulator webview** (later — would reuse a small frame-stream from
  `mote_host`, or stay out of scope and just launch the native emulator/Studio).
- Tasks/launch templates so F5 builds + runs.

Effort is small (it's CLI + config glue) and it's fully independent of Studio.

---

## 5. Phase 1 — the core loop (building now)

> Open a project, edit `game.c` in VS Code, hit save → the Studio rebuilds and the
> game runs **live in the Thumby Color shell** (≤2× pixels, lit buttons,
> keyboard + gamepad).

1. `platform/studio/mote_plat_studio.c` — offscreen `present` + button bitfield +
   reuse the host audio; a tiny API for the app to drive frames + read the fb.
2. `studio/` — the SDL2 app: window, the **device-shell emulator panel** (shell art
   + 2× screen + lit buttons), keyboard/gamepad input, loads a game `.so` (dlopen),
   runs `mote_os_run` per frame.
3. **Project library** (list + open) and **Edit in VS Code** + **file-watch →
   rebuild → hot-reload**.
4. Build/Run controls + a build-output console.

Later phases: 2) USB device panel + live logs + flash helper, 3) Pixel-Art Studio
+ asset import/preview + the config/arena builder, 4) polish + Linux/Windows
installers + the VS Code extension.

---

## 6. Engine gaps to close in parallel (small)

- **Save API** — `mote->save_write/read` into a flash save-area (+ a Studio saves
  inspector).
- **Sample audio** — `mote->audio_sample(...)` + a WAV baker.
- **Battery/charge** read (HUDs + the shell indicator).
