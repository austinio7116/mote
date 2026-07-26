#!/bin/sh
# Build and run the content audit. Run from anywhere; paths are resolved from
# this script's location so it cannot pick up a stale binary from a wrong cwd.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=/tmp/roguemote_test_content
if [ "$1" = "--out" ]; then out=$2; shift 2; fi

gcc -O1 -g -DMOTE_HOST=1 \
    -I "$root/engine/core" -I "$root/engine/math" -I "$root/engine/render" \
    -I "$root/engine/assets" -I "$root/engine/input" -I "$root/engine/physics" \
    -I "$root/sdk" -I "$root/games/roguemote/src" \
    -Wall -Wextra -Wno-unused-function \
    "$here/test_content.c" \
    "$root/games/roguemote/src/rl_item.c" \
    "$root/games/roguemote/src/rl_magic.c" \
    "$root/games/roguemote/src/rl_turn.c" \
    "$root/games/roguemote/src/rl_map.c" \
    "$root/games/roguemote/src/rl_world.c" \
    "$root/games/roguemote/src/rl_data.c" \
    -lm -o "$out"

exec "$out" "$@"
