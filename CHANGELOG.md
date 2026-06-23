# Changelog

All notable changes to Mote (engine, OS, Studio, CLI, examples).

## 0.2-alpha — unreleased

Engine ABI bumped to **v20** and the OS RAM layout changed, so this release
needs an **OS reflash** (`firmware_mote_os.uf2`), and games must be rebuilt
against the v20 SDK to gain icons.

### Device / OS

- **Per-game launcher icons.** Icons no longer live in a hardcoded firmware
  table — each game carries its own 60×60 icon inside its module
  (`MoteModuleHeader.icon_vaddr` → `mote_game_icon_data[]` in `.rodata`). The
  launcher reads it straight from flash without loading the game (host: via
  `dlsym`). Adding a game no longer means editing/reflashing the OS. Games with
  no icon fall back to the name-coloured tile. *(ABI v20)*
- **More games per device.** Store cap **32 → 56** and launcher catalog cap
  **24 → 56**, so the device holds and *displays* far more games. (Ceiling is
  ~60: the OS RAM region is nearly full; the launcher catalog now lives on the
  stack instead of BSS to spend less of that budget.)

### Studio / CLI

- **New-game wizard.** `mote new` and Studio's *New Game* modal now offer three
  runnable starter templates — **3d** (spinning mesh), **physics** (boxes in a
  pit), **2d** (top-down sprite) — each with its `.config` arena pools pre-sized
  to what it draws. The modal previews each template's estimated arena
  (~39 / ~56 / ~0 KB). CLI: `mote new <name> -t 3d|physics|2d`.
- **Animation baking shrunk dramatically.** The Anim baker dumped the *entire*
  source sprite sheet into the header; it now packs only the cells the clips
  reference into a tight grid and remaps each frame. Example: `itsamemario`
  header **1.85 MB → 408 KB**, its `.mote` **556 KB → 147 KB**.
- **Version in About** (Help ▸ About shows `0.2-alpha`).
- **Fix:** switching projects now re-runs the Tiles/Anim/Mesh auto-load so the
  tabs reflect the newly-opened game instead of the previous one.
- **Fix:** `mote list` no longer truncates long device listings (~25 lines) —
  the serial read window was too short.

### Games / examples

- **`fling` substantially reworked into a real game:** procedurally generated
  levels after level 1; a large, on-the-fly **stitched heightfield terrain**
  that fills the background with rolling hills (one `uint16` MoteMesh collider +
  seamless render chunks sharing a center/scale); flat building pads so forts
  don't topple unshot; five taller, more varied fort archetypes (keep / tower
  row / pyramid / fortress / village); instant "cleared" the moment the last pig
  is down; crimson, larger trajectory dots that read against the sky.
- **Screenshot icons** for `herodemo` and `tiles`; refreshed `tiledemo`'s stale
  icon — each captured from a gameplay frame, downscaled to 60×60.

### Docs

- Added the **Claude Code skill** (`.claude/skills/mote-game-dev`) for building
  Mote games, including the large-mesh stitching and game-owned-geometry
  gotchas; README documents the per-game icon pipeline and the new-game
  templates.

## 0.1-alpha

Initial public alpha: native C engine + console OS + Studio IDE for the Thumby
Color. 3D scene (meshes, splats, voxel-ish terrain), 2D scene (sprites,
autotiling, tilemaps), rigid-body physics, SFXR/`MoteSfx` audio, the asset
pipeline (img2tex / obj+stl2mesh / wav2snd / tiles / anim), STL mesh baking with
decimation + chunking, indexed mesh colours, the Studio tabs (Pixel/Texture/
Tiles/Anim/Mesh/Audio/Console), and Windows + Linux/WSL2 install bundles.
