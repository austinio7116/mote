# TerraMote

A Terraria-style sandbox for the Thumby Color (Mote engine). Dig, build, craft,
fight the Eye of Cthulhu. See `DESIGN.md` for the architecture.

## Run it — use Mote Studio (recommended)

```bash
./tools/mote studio
```

Open **TerraMote** from the game list and hit **Preview** — the game runs in
the Studio with live hot-reload: edit `src/game.c` or any asset tab and **Save**
to rebuild + relaunch instantly. The art lives in the Studio's tabs (Pixel Art
for the sprite PNGs, Tiles for the autotile rulesets, Audio for the .sfx
recipes), and Save auto-bakes the headers.

CLI alternatives (mainly for headless/scripted testing):

```bash
./tools/mote run games/terramote          # host emulator (SDL)
./tools/mote push games/terramote --launch   # device over USB
```

Emulator keys: WASD/arrows = d-pad · `.` = A (jump) · `,` = B (use item) ·
Shift = LB (cycle hotbar) · Space = RB (inventory/craft) · Enter = MENU (pause).

**Aiming**: hold B with a pick/axe/block/bow and the d-pad steers the reticle
(8-way, persists after release). Without B, UP/DOWN taps step the aim
elevation. Holding B while walking tunnels: level aim carves a full-height
corridor, diagonal aim digs stairs.

## Testing hooks (host only)

```bash
TERRA_SKIP=1        # skip title + character creator, straight into a new world
TERRA_TIME=0.7      # day fraction (0.60+ = night)
TERRA_POS=100:210   # spawn at tile col:row
TERRA_GIVE="44:1,63:1"   # grant items ("id:count,..."; ids in src/terra.h)
TERRA_DBG=1         # per-second boss/enemy/hp log + save-band logging
```
Combine with the engine's scripted input for repros, e.g.:
```bash
TERRA_SKIP=1 MOTE_KEYS="right:80-200 b:220-400" MOTE_SHOT=/tmp/f.ppm \
  MOTE_SHOT_FRAME=420 ./tools/mote run games/terramote
```

## Art pipeline — IMPORTANT

**Never edit `src/*.h` baked headers or the checked-in PNGs under `assets/`
that a script generates** — regenerate instead:

```bash
python3 assets/make_tiles.py      # tilesheets + rulesets + canopy/branches
python3 assets/make_sprites.py    # procedural base (zombie, UI, item icons)
python3 assets/extract_sheets.py  # player/enemies/weapons from sources_*.png
./tools/mote bake games/terramote # bake everything to src/*.h
```

- `assets/sources_sheet*.png` are the AI-generated reference sheets;
  `extract_sheets.py` depixelates them (pitch detection) into game sprites.
- Tile look lives in `make_tiles.py` painter functions (one per material);
  tilesets/*.tileset hold the autotile rulesets (Studio Tiles tab can edit).
- The player body/hair are quantized to reserved palette colours so the
  character builder can recolour them — keep those exact RGB values.
- Hand-drawn tweaks are welcome on PNGs that no script overwrites (or move the
  asset out of the script first) — `mote bake` picks up any PNG change.

## Known rough edges

- Untested on real hardware as of the branch point; balance assumes armor
  before fighting the Eye (naked melee = death).
- Music intentionally removed (SFX only).
