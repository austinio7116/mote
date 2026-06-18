# ThumbyEngine + ThumbyOS

A native-C game-development platform for the Thumby Color (RP2350). A
consolidated engine (`libte`) extracted from ThumbyElite (rasterizer),
ThumbyCraft (voxel raycaster) and ThumbyCue (physics), plus a dedicated
console OS where developers **compile optimized native game code on the host
and deploy it to the device with one command** — no per-game firmware reflash.

Full design + roadmap: `docs/PLAN.md`.

## Status

**Phase 0 — dual-target build skeleton: host milestone DONE.**
A spinning, lit, depth-tested cube renders through the real 3D pipeline +
rasterizer, driven entirely through the `te_platform` abstraction. The device
half (same code on RP2350) lands with the OS in Phase 1.

## Layout

```
engine/      libte — platform-independent C (math, render, input, ...)
platform/    host/ (SDL2 PC build)   device/ (RP2350 — Phase 1)
os/          ThumbyOS launcher + engine + USB deploy (Phase 1)
sdk/         te_api.h ABI + game linker script (Phase 1)
tools/       asset bakers + the `thumby` CLI (Phase 2)
examples/    sample games (hello-mesh, ...)
docs/        PLAN.md
```

## Build & run (host)

Prereqs: `cmake`, a C toolchain, `libsdl2-dev`.

```bash
cmake -B build_host -S .
cmake --build build_host -j8
./build_host/hello_mesh           # spinning cube; D-pad nudges spin, A resets
```

Controls: D-pad = arrows / WASD, A = `.` / K, B = `,` / J,
LB = LShift, RB = Space, MENU = Enter, quit = Esc.

Headless render check (CI / no display):

```bash
SDL_VIDEODRIVER=dummy TE_SHOT=/tmp/shot.ppm ./build_host/hello_mesh
```

## Conventions (inherited from the source engines)

- Camera-relative world (camera = origin); view +Z forward.
- Depth: `uint16`, **larger = nearer** (`d = K/z`).
- Meshes wind CCW-from-outside; projection mirrors to screen-CW front faces.
- All-float math + `-ffast-math` (RP2350 FPU).
- Dual-core render: core0 builds the draw-list, both cores rasterise their own
  row band (0–63 / 64–127). Lock-free.
