# Mote OS + Mote — A Native Game-Dev Platform for Thumby Color (RP2350)

> Working names: **Mote OS** (the console OS) and **Mote** (`libte`, the consolidated engine). Greenfield — a new sibling repo `mote/`. **No existing project in this workspace is modified.** ThumbyElite / ThumbyCraft / ThumbyCue / ThumbyOne are read-only references we *port from*, not edit.

## Context

We have three proven native-C games for the Thumby Color (RP2350: dual Cortex-M33 @280MHz w/ FPU, 520KB SRAM, 16MB XIP flash, 128×128 RGB565 LCD, PWM audio, D-pad+A/B+LB/RB+MENU):
- **ThumbyElite** — software triangle rasterizer + 3D pipeline + procedural worldgen + synth audio (~22.8K LOC).
- **ThumbyCraft** — DDA voxel raycaster + 64³ world + biomes (~19.2K LOC).
- **ThumbyCue** — full 3D rigid-body physics (impulse + friction + CCD) built *on* Elite's rasterizer (~5.8K LOC).

These already share a de-facto engine (vendored `vec.h`, `r3d_raster/pipe.c`, `r3d_mesh.h`, `craft_font/buttons/lcd/audio` drivers) and an identical **game/ (platform-independent) + device/ (RP2350) + host/ (SDL2)** split with a lock-free **dual-core banded-render handshake** (core0 builds + rasters rows 0–63, core1 rasters 64–127). The tech is there; it has never been *packaged* as a reusable engine or given a real developer workflow.

**Goal:** Build (a) a clean reusable engine extracted from those three games, and (b) a **dedicated game-console OS** — *not* a ThumbyOne multiboot slot — that boots straight into a launcher, holds the engine resident, and lets a developer **compile optimized native game code on the host and deploy it to the device with one command**, no firmware reflash per game.

**Decisions locked (via user):**
- **Native C only.** Games are C compiled to optimized native code against the engine ABI. No script VM. (Hybrid Lua can be added later without redesign, but is out of scope now.)
- **Deploy = host CLI + custom USB protocol.** One command builds, uploads, and launches.
- **OS scope = minimal launcher + engine.** Boot → game picker → settings (volume/brightness/battery) → resident engine → USB deploy service → filesystem. One game runs at a time.
- **Game-loading mechanism = the key open technical decision.** Recommendation below (resident engine + ATRANS-mapped native modules), with a flat-binary fallback. **Both are prototyped in Phase 0/1 before committing.**

---

## 1. System overview

```
            ┌───────────────────────── Host (Linux/Windows) ─────────────────────────┐
            │  thumby CLI  ──►  cross-compile game (arm-none-eabi, -O3)  ──►  game.mote │
            │       │            link against libte ABI header + game linker script    │
            │       │            asset bakers: obj2mesh / img2tex / wav2pwm             │
            │       ├──►  SDL2 host build of libte  (play-in-emulator, instant)         │
            │       └──►  USB protocol  ──► deploy + launch on device                   │
            └───────────────────────────────────────────────────────────────────────┘
                                              │ USB
            ┌──────────────────────────────── Device: Mote OS ────────────────────────┐
            │  Boot ROM → Mote OS kernel (flash-resident, low flash region)            │
            │    • Launcher UI (game picker from filesystem, icons)                     │
            │    • Settings (volume/brightness/battery), USB deploy service             │
            │    • Game store filesystem (catalog + contiguous game images)             │
            │    • RESIDENT Mote (libte) exposed via fixed jump-table ABI       │
            │  Launch: map selected game image into execution window, jump to entry     │
            │  Game (native module) calls engine only through the ABI; one at a time    │
            └───────────────────────────────────────────────────────────────────────┘
```

The engine is **resident in the OS** (compiled once into the kernel image). Games are small native modules that call the engine through a stable, versioned ABI — they never re-bundle the engine. New game = push one small module file. Engine improvement = update the OS once (bump ABI, games declare the ABI they target).

---

## 2. The consolidated `Mote` (`libte`) core

This is **packaging + de-vendoring**, not a rewrite. Preserve the zero-`#ifdef` `game/` discipline: all platform behind one header (`mote_platform.h`), engine compiles identically for host (SDL2) and device (RP2350). Module breakdown and where each comes from:

| Module (`engine/...`) | Header / API | Ported from |
|---|---|---|
| `math/mote_vec.h` | `Vec3`, `Mat3` (orthonormal basis), add/sub/scale/dot/cross/lerp/normalize/rotate/orthonormalize, header-only, `-ffast-math` | Elite `vec.h` |
| `render/mote_raster.c` | half-space edge-function filled triangles + uint16 depth (LARGER=NEARER) | Elite `r3d_raster.c` |
| `render/mote_pipe.c` | transform → near-clip → project → flat-shade | Elite `r3d_pipe.c` |
| `render/mote_scene3d.c` | draw-list + **dual-core band split** + volatile go/done handshake | Elite `r3d_scene.c` |
| ~~`render/mote_raycast.c`~~ | ~~DDA voxel raycaster path (`MOTE_RENDER_RAYCAST` flag)~~ — **DESCOPED** (see "Renderer scope", 2026-06-29): the voxel raycaster stays a game-side `render_band` renderer, not an engine module | Craft `craft_render.c` |
| `render/mote_impostor.c` | per-pixel sphere impostors (planets/balls) | Elite/Cue |
| `render/mote_sprite.c` | 2D blit, RGB565 fb ops, uint8 z-occlusion | Craft |
| `render/mote_fx.c` | particle/beam FX pool | Elite `r3d_fx.c` |
| `render/mote_framebuffer.h` | 128×128 RGB565 fb + depth + band region descriptor | Elite/Craft |
| `physics/mote_phys3d.c` | generalized impulse rigid body: pos/vel/avel/orient, ~2kHz substep accumulator, friction phases, CCD, pluggable contact model (billiard cushion/pocket plugs back in) | Cue `cue_physics.c` |
| `audio/mote_audio.c` | 22050Hz PWM procedural synth voice-pool + instrument-param table | Elite/Craft |
| `assets/mote_mesh.h` | int8 quant verts, 8-byte faces (idx+int8 normal+RGB565), LOD | Elite `r3d_mesh.h` |
| ~~`assets/mote_voxel.c`~~ | ~~64³ sliding window + `mote_gen_column`/`mote_gen_stamp` hooks~~ — **DESCOPED** with the raycaster (voxel games own their world buffer via the per-game arena) | Craft worldgen |
| `assets/mote_font.c` | 8×8 bitmap font | Craft `craft_font.c` |
| `assets/mote_image.c` | RGB565 texture/atlas decode | new (small) |
| `input/mote_input.h` | `MoteButtons` (9 bools) + edge/held bookkeeping, platform-independent | Craft `CraftRawButtons` |
| `core/mote_platform.h` | the one abstraction: present fb, poll buttons, rumble, micros, VFS, save | Elite `elite_platform.h` |
| `core/mote_app.c` | engine lifecycle: init / frame / shutdown; the loop game modules drive | new (thin) |

**Render flags** (`mote_config.h`): `MOTE_RENDER_RASTER`, `MOTE_RENDER_RAYCAST`, `MOTE_RENDER_SPRITE`, `MOTE_PHYSICS`. The resident OS engine is built with **all** backends (universal host). A game declares in its manifest which it uses (lets the launcher validate + lets future native-link builds drop unused code).

`mote_platform.h` is implemented twice: `platform/device/` (RP2350 — LCD `gc9107`, buttons, PWM audio, rumble drivers ported from Craft) and `platform/host/` (SDL2, ported from Elite `host/`).

---

## 3. Game loading & the engine ABI — **recommended design + fallback (prototype both first)**

This is the decision flagged "let me think about it." Here is the recommendation, the reasoning against the RP2350 constraints, and the fallback.

### Recommended: resident engine + ATRANS-mapped native modules

- **Engine + OS resident** at the bottom of flash (virtual base `0x10000000`), always mapped.
- **Games are native modules** linked at a **fixed virtual execution window** (e.g. `0x10300000`) with their RAM (`.data`/`.bss`/stack) at a **fixed reserved SRAM region**. They contain only game code + baked assets — *not* the engine.
- **Stored at arbitrary (but contiguous) physical offsets** in the game-store filesystem. On launch, Mote OS programs the RP2350 **ATRANS** address-translation window to map the selected game's physical flash location → the fixed virtual execution window, restores fast QPI XIP, and jumps to the game entry. The game then **executes in place via XIP — no copy to RAM, no relocation, no PIC** (it's linked at the fixed virtual address; ATRANS does the placement).
- **Engine ABI = a versioned jump table** (a `const MoteApi *` struct of function pointers) at a fixed known address in the resident region. Games `#include <mote_api.h>` and call `te->draw_mesh(...)`, `te->phys_step(...)`, etc. The header declares `extern const MoteApi *const te;` resolved by the game linker script to the fixed jump-table address. `MoteApi.abi_version` is checked by the launcher before jumping.

This is exactly the ATRANS + fast-XIP-restore mechanism ThumbyOne already uses to chain slots (`thumbyone_handoff.c`, `thumbyone_xip_fast_setup`) — proven on this silicon — but managed by *our* OS instead of the bootrom, and pointed at a filesystem of small modules rather than fixed partitions.

**Why this model:** games stay tiny (engine resident, not re-bundled); unlimited games limited only by filesystem space; native XIP performance; clean stable ABI; engine updates are a single OS update. **Constraint it imposes:** each game image must be stored **contiguously** in flash (ATRANS maps a contiguous window) — so the game store uses simple contiguous-extent allocation, not a fragmenting general FS.

### Fallback: copy-to-arena flat binaries

If ATRANS remapping proves fiddly (alignment, window-size, XIP-restore edge cases): keep games linked at the fixed virtual window, store them anywhere, and on launch **copy the selected image into a fixed flash "execution arena"** (the OS programs flash) then jump. No PIC, simplest ABI. Costs: flash wear + ~hundreds-of-ms launch latency + the arena reserves its max-game-size region. Engine stays resident either way, so games are still small.

### What to prototype in Phase 0/1 to decide
Build a 2-instruction "game" linked at the fixed window that calls one engine ABI function (clear screen to a color), then: (1) map it via ATRANS and jump; (2) copy-to-arena and jump. Measure launch latency, confirm XIP speed post-jump, confirm the jump-table ABI resolves. Pick the winner; the rest of the platform is identical above the loader.

> **RP2350 gotchas to bake in regardless (learned from ThumbyOne):** restore fast QPI XIP after any flash op or remap (`*_xip_fast_setup` equivalent); place engine hot loops (`mote_raster`, `mote_raycast`, `mote_phys` substep) in IRAM via `.time_critical` (`MOTE_HOT` macro); bump the tiny Pico default heap via `target_compile_definitions`; the second core must never cross into game-launch/flash ops mid-frame.

---

## 4. Game package / on-disk format

A built game is **one file** `game.mote` (Mote game module): a small header + the native module image + baked asset blobs, all contiguous so it can be ATRANS-mapped or arena-copied wholesale.

```
game.mote:
  [header 'MOTE' | abi_version | entry_offset | icon_offset | render_flags | ram_need | name]
  [native .text/.rodata  — linked at fixed virtual window, calls engine via te-> jump table]
  [baked asset blobs: meshes (mote_mesh.h), textures (RGB565), audio (PWM params), voxelgen seeds]
```

Source layout on host (only `game.mote` ships to device):
```
mygame/
├─ game.toml          # manifest: id, name, author, abi, render_flags, ram hint
├─ icon.bmp           # 128x128 RGB565 launcher icon
├─ src/*.c            # game logic in C (calls te-> ABI)
├─ assets/            # obj / png / wav sources
└─ build/game.mote     # produced by `thumby build`
```

The game's C contract (mirrors the existing decoupled game-loop pattern):
```c
void  game_init(void);            // once, after engine ready
void  game_update(float dt);      // per frame: read te->input, move entities
void  game_render(uint16_t *fb, int y0, int y1);  // dual-core banded
void  game_overlay(uint16_t *fb); // HUD/UI on core0
void  game_event(uint32_t ev);    // physics/timer/input-edge events (optional)
```

---

## 5. Mote OS (minimal launcher + engine)

`os/` in the new repo. Responsibilities, all small:
- **Boot**: kernel init, fast-XIP setup, mount game store, init LCD/buttons/audio drivers (shared with engine platform layer).
- **Launcher UI**: scroll the game catalog, render each `icon.bmp`, A = launch. Reuse `mote_font` + `mote_sprite` for the UI itself (the launcher is the engine's first client).
- **Settings**: volume / brightness / battery readout, persisted to a settings sector (port the simple flash-settings pattern from ThumbyOne `thumbyone_settings`).
- **Game store**: a contiguous-extent catalog (name → physical offset → size → icon → abi/render flags). Add/replace/delete entries from the USB service.
- **USB deploy service**: implements the device side of the custom protocol (§6): receive a `.mote`, write it contiguously to the store, update catalog, optionally launch immediately. Also streams back logs / a profiler line (frame ms, core balance, heap) for the CLI.
- **Launch path**: ATRANS-map (or arena-copy) selected `.mote`, set up game RAM region + stack, jump to entry. **Return to launcher**: game calls `te->exit()` → OS reboots/returns to launcher (watchdog-reboot pattern, same family as ThumbyOne handoff but single-target).

One game runs at a time; the full 520KB SRAM (minus the resident OS/engine BSS + the small fixed game-RAM reservation) is the game's to use.

---

## 6. Host SDK & toolchain

`tools/` + `host/` in the new repo. Bootstrap from what already exists (`obj2mesh.c` and the Elite SDL host are working templates).

- **`obj2mesh`** — OBJ → `mote_mesh.h` baked mesh. Port from Elite as-is.
- **`img2tex.py`** — PNG/atlas → RGB565 texture blob. New, small.
- **`wav2pwm.py`** — WAV/instrument spec → PWM synth voice params. New.
- **SDL2 host build of `libte`** — same engine + `mote_plat_host.c`; games compile *natively for the host too* (the host links the game's `src/*.c` directly against the SDL `libte` and a host shim of the ABI jump table) so `thumby run` gives an instant play-in-emulator loop with the real engine. Port the SDL event-loop / keyboard-mapping from Elite `host/host_main.c`.
- **Cross-compile** — `arm-none-eabi-gcc -O3 -mcpu=cortex-m33 -mfpu=fpv5-sp-d16` against the game linker script (fixed virtual window + RAM region) + `mote_api.h`. Produces the native module; `thumby` wraps it + bakes assets → `game.mote`.
- **`thumby` CLI** (Python) — developer entry point:
  - `thumby new <id>` — scaffold `game.toml` + `src/main.c` + sample asset.
  - `thumby build` — bake assets + cross-compile + emit `game.mote`.
  - `thumby run` — build for host + launch SDL emulator (primary iteration loop).
  - `thumby push <id> [--launch]` — build + upload via USB protocol; `--launch` runs it immediately on device.
  - `thumby logs` — stream device logs/profiler.
- **Custom USB protocol** — small framed protocol over USB CDC/vendor interface: `HELLO/ABI`, `PUT <name> <size> <crc>` + data chunks, `LAUNCH <name>`, `DELETE <name>`, `LOG` stream. Device side lives in the OS USB service; no BOOTSEL, no MSC.

**Developer loop:** `thumby new mygame` → edit `src/*.c` → `thumby run` (instant SDL) → `thumby push mygame --launch` (on real hardware in seconds, no reflash of the OS).

---

## 7. Phased roadmap

**Phase 0 — Repo + dual-target build skeleton.**
New `mote/` repo. CMake builds an empty `libte` for host (SDL2) + device (pico-sdk). Stand up `mote_platform.h` both ways. "Hello triangle": port `mote_vec`/`mote_pipe`/`mote_raster`, render one spinning mesh on SDL **and** device. No OS, no loader yet. *Deliverable: identical engine code renders on host + device.*

**Phase 1 — MINIMAL FIRST MILESTONE (proves the whole thesis).**
> *Mote OS boots to a launcher, and one separately-compiled native game module — pushed over USB from the host — loads and runs on the device, plus runs in the SDL emulator. The game calls the engine only through the ABI jump table.*
- Define `mote_api.h` jump table + game linker script (fixed virtual window + RAM region).
- **Prototype both loaders** (ATRANS-map vs copy-to-arena) with a trivial game; pick the winner (§3).
- Minimal OS: boot, one-entry catalog, launch path, return-to-launcher.
- Minimal USB service + `thumby push --launch`.
- One game: D-pad rotates a mesh, calls `te->draw_mesh`/`te->input`.
This validates: engine-as-resident-library, stable native ABI, native module load + execute, USB deploy, host/device parity. **If this works, the platform is real.**

**Phase 2 — Launcher + store + deploy UX.**
Multi-game catalog with icons, settings page, contiguous game store with add/replace/delete, full `thumby new/build/run/push/logs`, profiler line streamed back. A developer can now author → push → play many games with no reflash.

**Phase 3 — Engine breadth.**
Wire the rest of the ABI: `mote_audio`, `mote_phys3d`, `mote_fx`, `mote_impostor`, `mote_sprite`. Ship a real sample per renderer: a polygon shooter (raster), a physics toy (reuse Cue's solver). These double as ABI stress tests + docs. **The raycast path + `mote_voxel` are DESCOPED** — see "Renderer scope" (2026-06-29): the engine's baselines are the high-performance 2D and 3D-rasteriser paths; the voxel raycaster stays a game-side `render_band` renderer.

**Phase 4 — Polish & SDK docs.**
Error/crash overlay on device, host hot-rebuild watch, `mote_api.h` reference docs, sample-driven "Getting Started," ABI-version compatibility checks in launcher + CLI.

**Phase 5 (optional, later) — extensions.**
Hybrid Lua scripting layer for prototyping (the ABI already exists to bind against); system services (shared save manager, screenshot, on-device file mgmt); OTA engine update. Explicitly out of the initial scope.

---

## 8. Risks & where the RP2350 budget bites

- **SRAM (520KB) is the wall.** Voxel raycaster alone wants a 256KB 64³ window; framebuffer 32KB + depth (uint16 32KB / uint8 16KB). The resident OS+engine BSS competes with the game's RAM region. **A game cannot use the full voxel world *and* large working RAM at once** — document as a hard constraint; `mote_config` flags let a game opt out of backends it doesn't use. Keep the resident OS BSS lean.
- **Native module loading is the central technical risk.** ATRANS remap + fast-XIP-restore has known sharp edges (the ThumbyOne notes catalog them). Mitigation: Phase 1 prototypes both loaders behind one interface; the rest of the platform doesn't care which wins.
- **ABI stability.** The jump table is a forever-contract. Version it (`MoteApi.abi_version`), only append, never reorder; launcher + CLI refuse mismatched `.mote`. A botched ABI break orphans every shipped game.
- **No memory protection (no MMU).** A buggy native game can corrupt the OS / brick the store. Accept for a homebrew console; mitigate with a guard region between game RAM and OS BSS + a watchdog that returns to launcher on hang. (True sandboxing is the only thing the rejected script-VM model would have bought — noted, deliberately traded away for native performance.)
- **Determinism / floats.** Preserve `-ffast-math` + camera-relative world model (Elite heritage) and seeded RNG (Elite/Craft worldgen) in the engine API so 3D precision and procedural content stay stable.
- **Dual-core discipline.** Keep the lock-free banded handshake; core1 renders only — never let it touch flash ops, the loader, or launch.
- **Flash wear (fallback loader only).** Copy-to-arena reprograms flash each launch; if chosen, cap launch frequency / consider wear leveling. ATRANS model avoids this entirely.

---

## 9. New repo directory layout (nothing here touches existing repos)

```
mote/
├─ engine/        # libte (§2): math render physics audio assets input core scene
├─ platform/
│  ├─ device/     # RP2350 mote_platform impl + gc9107/buttons/audio/rumble (port from Craft)
│  └─ host/       # SDL2 mote_platform impl (port from Elite host/)
├─ os/            # Mote OS (§5): boot, launcher, settings, game store, USB service, loader
├─ sdk/
│  ├─ mote_api.h    # the ABI jump-table contract (forever-stable)
│  └─ game.ld     # game linker script: fixed virtual window + RAM region
├─ tools/         # obj2mesh (port), img2tex.py, wav2pwm.py, thumby CLI, USB protocol
├─ examples/      # hello-mesh, voxel-sandbox, physics-toy, shooter
└─ docs/
```

Highest-leverage files to design first:
- `sdk/mote_api.h` — the ABI; the entire platform's stability hinges on it.
- `os/loader.c` — ATRANS-map / arena-copy + jump; the flagged decision.
- `engine/core/mote_platform.h` — host/device parity boundary.
- `tools/thumby` + `os/usb_service.c` — the deploy pipeline that makes this a *platform*.

---

## 10. Verification

- **Phase 0:** `thumby`-less manual build renders the same spinning mesh in the SDL window and on the device LCD. Visual parity = engine extraction correct.
- **Phase 1 (the proof):**
  1. Host: `thumby run examples/hello-mesh` → SDL window, D-pad rotates the mesh.
  2. Device: `thumby push hello-mesh --launch` → OS boots to launcher, game loads via the chosen loader, same mesh + control on hardware. Confirm fast XIP (frame ms in profiler), confirm `te->` ABI resolved, confirm return-to-launcher.
  3. Toggle the OTHER loader (ATRANS vs arena), re-measure launch latency + frame ms; record the decision.
- **Phase 2:** push 3+ games; verify catalog/icons/settings persist across reboot; replace and delete games; confirm no reflash of the OS at any point.
- **Phase 3:** each sample exercises a different backend (raster / raycast / physics) at target fps on device; profiler confirms dual-core balance.
- **Ongoing:** every example game is an ABI regression test — a CI host build + SDL smoke-run per example catches ABI drift before it reaches hardware.

---

## 2D sprite + tile subsystem (added 2026-06-18)

Mote supports 2D sprite/tile games *alongside* the 3D engine, not as a separate
mode. A screen-space **2D scene** is rastered AFTER the 3D scene each frame
(both banded across cores), so a game can be:
- **pure-2D** — tilemap + sprites (platformers, RPGs, puzzles)
- **pure-3D** — the mesh pipeline (as before)
- **hybrid** — a 3D world with a 2D HUD/sprite layer on top

"Good sprite management" = the engine owns the sprite list (layers, frames,
flips) and composes it; games manipulate `MoteSprite` structs, not raw blits.

### API (ABI v2, append-only — `sdk/mote_api.h`)
- `mote->scene2d_begin(cam_x, cam_y)` — start the 2D frame, set the scroll camera
- `mote->scene2d_set_tilemap(map, tileset)` — a scrolling background tile layer
- `mote->scene2d_add(sprite)` — add a sprite (drawn in `layer` order)
- `mote->blit(...)` — immediate-mode band-clipped, colour-keyed blit (HUD)

### Data (`engine/render/mote_2d.h`, header-only structs)
- `MoteImage` — RGB565 pixel rect in flash + transparent colour key (magenta)
- `MoteTileset` — an atlas image divided into `tile_w x tile_h` cells
- `MoteTilemap` — `cols*rows` byte indices into a tileset (0xFF = empty)
- `MoteSprite` — image + frame rect + screen pos + layer + h/v flip flags

### Rasteriser (`engine/render/mote_2d.c`)
Tilemap drawn first (only tiles overlapping the band), then sprites in layer
order. Read-only over the sprite list so both cores raster concurrently.
The OS clears both scenes at the start of every frame, so a game only renders
what it adds this frame (no stale state within a game or across games).

Verified on host: `examples/tiledemo` — a scrolling grass/stone/water world
with a player sprite + camera follow, loaded as a separate ABI-v2 module while
the abi-1 3D games still load.

### Roadmap (not yet built)
- **img2tex** baker (PNG/BMP -> `MoteImage` header), wired into `mote bake`
  alongside obj2mesh — so sprite sheets come from art, not code.
- **Sprite animation** helper (frame sequence + fps + loop) over `MoteSprite`.
- **Multiple tile layers** + per-layer parallax scroll.
- **Tilemap collision** helpers (point/AABB vs tile flags) for platformers.
- **Text as sprites** / bitmap-font atlas option beyond the built-in 3x5 font.

---

## Renderer scope — voxel raycaster stays game-side (decided 2026-06-29)

**Decision.** The engine's first-class renderers are the **3D triangle rasteriser**
(`mote_scene3d`/`mote_raster`/`mote_pipe`) and the **2D sprite/tile** path. Both are
high-performance baselines (60 fps at thousands of triangles) and are the recommended
foundation for new games. The **DDA voxel raycaster is NOT promoted into the engine**:
the planned `render/mote_raycast.c` + `assets/mote_voxel.c` (§2) and the raycast half of
Phase 3 (§7) are **descoped** — a deliberate "won't do now", not a deferral with intent.

**Why.** A per-pixel software raycaster fills all 128×128 = 16,384 pixels every frame and
lands at ~15 fps on device with both cores maxed — and that's a *property of the algorithm*,
not a tuning gap: `.ramtext`/IRAM placement of the hot loop barely moved it (measured on
ThumbyRogue), because it's bound by per-pixel work, not flash latency. That's acceptable for
a dedicated voxel game, but a poor *baseline* for the engine — promoting it would normalise a
slow renderer as a starting point. The budget math reinforces it: per
[`render-parity-design.md`](render-parity-design.md) there is only ~19 KB of static OS-BSS
headroom, so any resident renderer's per-frame scratch is a standing tax on **every** game
unless held to ~0 — cost the 2D/3D paths don't impose and the raycaster shouldn't either.

**How voxel games are served instead.** The existing `render_band` hook (a game supplies a
banded `f(fb,y0,y1)` pass; the engine still does dual-core dispatch, overlay, audio, timing).
ThumbyCraft and ThumbyRogue carry their own `craft_render.c` and fill that slot, owning their
world buffer + z-buffer from the **per-game arena**. This costs games that don't use it
**nothing**: no resident raycaster code beyond the hook, no OS-BSS, no arena reservation.

**If revisited later (not now).** Two phases, in order of cost:
- **Phase A (cheap, ~tens of bytes OS-BSS):** let a `render_band` pass *share* the engine
  z-buffer so engine primitives (billboards/particles/meshes) composite into a voxel frame
  with correct depth. The raycaster stays game-side; it just becomes depth-aware.
- **Phase B (the full original plan):** ship `mote_raycast.c` + `mote_voxel.c` + a voxel ABI.
  Reconsider only with ≥2 voxel games or a public voxel ABI, and only under the strict rule
  "no new static arrays — per-frame scratch goes to the arena behind a `MoteConfig` knob."

`MOTE_RENDER_RAYCAST` stays `0` in the resident OS build.
