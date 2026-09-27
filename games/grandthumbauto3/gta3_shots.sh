#!/usr/bin/env bash
# gta3_shots.sh — headless gameplay-verification captures for
# games/grandthumbauto3, as PNGs.
#
# Renders three frames of ACTUAL GAMEPLAY (not the OS launcher, not the
# game's own title screen) so a human/reviewer can look at what the camera
# is doing: on foot in the city, driving, and mid-corner (camera swing).
#
# Requires: build/mote_host built (cmake --build build_host --target mote_host)
# and games/grandthumbauto3/build/GrandThumbAutoIII.so built. Output goes to
# ./gta3_shots/*.png (created if missing). Re-run any time with no args:
#
#   games/grandthumbauto3/gta3_shots.sh
#
# --- How this reaches real gameplay -----------------------------------
#
# 1. The launcher bug: mote_host's catalog-fill function (os/host/mote_host.c
#    host_fill()) never set MoteGameEntry.frag, so it came off the stack as
#    garbage and the launcher usually showed a bogus "FRAGMENTED - RUN DEFRAG
#    IN LOBBY" banner that refuses to launch. Fixed by explicitly zeroing it
#    (the host has no FAT/fragmentation concept — every .so path is always
#    "contiguous"). See the same commit for the one-line fix.
#
# 2. Once the launcher can launch, a scripted `a` press at MOTE_KEYS
#    "a:30-33" selects/launches the game (mote_host's launcher waits ~12
#    frames via MOTE_PICK logic before drawing, so the press has to land
#    after the .so is dlopen'd and the launcher is drawing — frame 30-33 is
#    comfortably past that).
#
# 3. The GAME's own title screen ("GRAND THUMBAUTO — PRESS A TO PLAY") then
#    needs a SECOND `a` press to start play (reset_game() + g_state=ST_PLAY).
#    A press around frame 150-153 reliably lands on the title screen.
#
# 4. To get the player into a car: the "jackable car placed next to the
#    player at spawn" (games/grandthumbauto3/src/game.c spawn_world()) has a
#    tight 2.0-3.4 unit placement search radius that's often *smaller* than
#    one map tile (TILE=4.0), so it frequently fails to place a car within
#    reach and can't be relied on. Instead we walk down a road (`down`) until
#    we cross paths with one of the 8 moving-traffic cars spawn_world() seeds
#    12-62 units from spawn, then hammer `a` every ~3 frames in the window we
#    expect to be in range (entering a car needs the player to be within
#    ~3.74 units of it — center to center — when the `a` press's rising edge
#    lands, so a single press is unreliable against a moving target). This
#    was found empirically for MOTE_GTA_SEED=42 (which fixes the city's road
#    layout, though NOT car spawn timing/RNG, so keep the seed if you want
#    these exact frame numbers to keep working) with MOTE_DT_MS=33 giving a
#    deterministic fixed timestep.
#
# 5. Once in the car (confirmed by the absence of the on-foot-only
#    "A ENTER   B ATTACK" HUD hint, and the car rendered from a chase camera
#    behind it), `rb` is throttle (see drive_car() call in game.c:
#    drive_car(c, dt, throttle=RB, brake=LB, steerL=LEFT, steerR=RIGHT)) and
#    `left`/`right` steer. Holding `rb` plus `right` for a stretch produces a
#    mid-corner frame that shows the chase camera swinging with the turn.
#
# Env vars used (see platform/host/mote_plat_host.c and
# games/grandthumbauto3/src/game.c for the full list):
#   SDL_VIDEODRIVER=dummy  no real display needed (headless CI/host)
#   MOTE_DT_MS=33          fixed ~30fps timestep -> deterministic runs
#   MOTE_GTA_SEED=42       fixes the city's road/building layout
#   MOTE_KEYS=...           scripted input: name:from-to (frame numbers),
#                            comma/space separated. Names: up down left right
#                            a b lb rb menu
#   MOTE_SHOT=path.ppm      dump ONE frame (at MOTE_SHOT_FRAME) then quit
#   MOTE_SHOT_FRAME=N       which frame MOTE_SHOT dumps (default 20)
#
set -euo pipefail
cd "$(dirname "$0")/../.."      # repo root: this script lives in the game folder

HOST=./build_host/mote_host
GAME=games/grandthumbauto3/build/GrandThumbAutoIII.so
OUT=games/grandthumbauto3/gta3_shots
mkdir -p "$OUT"

if [ ! -x "$HOST" ]; then
    echo "gta3_shots.sh: $HOST not found — build it first:" >&2
    echo "  cmake --build build_host --target mote_host" >&2
    exit 1
fi
if [ ! -f "$GAME" ]; then
    echo "gta3_shots.sh: $GAME not found — build the game .so first" >&2
    exit 1
fi

# Keys shared by every capture: launcher-select `a`, then title-screen `a`.
BOOT="a:30-33,a:150-153"

# Keys that walk the player down a road from spawn until they cross a moving
# traffic car, then hammer `a` every 3 frames across the window we expect to
# be in range (see header comment, point 4).
ENTER_CAR="down:160-475"
f=476
while [ "$f" -le 539 ]; do
    ENTER_CAR="${ENTER_CAR},a:${f}-$((f+1))"
    f=$((f+3))
done

capture() {
    name=$1; frame=$2; keys=$3
    ppm="$OUT/${name}.ppm"
    png="$OUT/${name}.png"
    SDL_VIDEODRIVER=dummy MOTE_DT_MS=33 MOTE_GTA_SEED=42 \
        MOTE_KEYS="$keys" MOTE_SHOT="$ppm" MOTE_SHOT_FRAME="$frame" \
        "$HOST" "$GAME" > /dev/null
    python3 -c "
from PIL import Image
Image.open('$ppm').save('$png')
"
    rm -f "$ppm"
    echo "wrote $png"
}

# 1. On foot in the city — walking down a road shortly after spawn.
capture on_foot 300 "${BOOT},down:160-300"

# 2. Driving — same as ON_FOOT's walk, then catch a passing car and hold
#    throttle (rb) for a few seconds.
capture driving 560 "${BOOT},${ENTER_CAR},rb:545-750"

# 3. A turn — same as DRIVING, but also steer right, captured mid-corner.
capture turn 650 "${BOOT},${ENTER_CAR},rb:545-750,right:600-700"

echo "done: $OUT/on_foot.png $OUT/driving.png $OUT/turn.png"
