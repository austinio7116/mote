#!/usr/bin/env bash
# Fast-update an already-unzipped Windows Mote Studio bundle: rebuild mote_studio.exe
# and sync ONLY the exe + repo source (engine/sdk/os/platform/studio/examples/tools),
# skipping the big static toolchain/arm/ffmpeg and build dirs — so you don't have to
# re-unzip the ~500 MB package after a code change.
#
#   ./scripts/sync-windows.sh [DEST]
# DEST defaults to the user's unzipped bundle; override with an arg.
set -e
cd "$(dirname "$0")/.."
ROOT="$PWD"
DEST="${1:-/mnt/c/MoteStudio-0.3-alpha-win64/MoteStudio}"
[ -d "$DEST" ] || { echo "sync-windows: dest not found: $DEST"; exit 1; }

SDLVER=2.30.9
SDL2W="$ROOT/.winbuild/SDL2-$SDLVER/x86_64-w64-mingw32"
[ -d "$SDL2W" ] || { echo "sync-windows: SDL2 mingw dev not fetched — run scripts/build-windows.sh once first"; exit 1; }

echo "==> cross-compiling mote_studio.exe"
[ -f build_win/CMakeCache.txt ] || cmake -B build_win -S . \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/scripts/mingw-toolchain.cmake" -DSDL2_DIR="$SDL2W/lib/cmake/SDL2" >/dev/null
cmake --build build_win --target mote_studio -j8

echo "==> syncing exe + source -> $DEST"
# Windows locks a running .exe (can't overwrite), but renaming it aside is allowed —
# then write the fresh exe; the user picks it up on next launch.
rm -f "$DEST/mote_studio.exe.old" 2>/dev/null || true
mv "$DEST/mote_studio.exe" "$DEST/mote_studio.exe.old" 2>/dev/null || true
cp build_win/mote_studio.exe "$DEST/mote_studio.exe"
if command -v rsync >/dev/null 2>&1; then
  for d in engine sdk os platform studio examples games tools; do
    rsync -a --delete --exclude build/ --exclude '*.stale' --exclude '*.o' "$ROOT/$d/" "$DEST/$d/"
  done
else
  for d in engine sdk os platform studio examples games tools; do
    rm -rf "$DEST/$d"; cp -r "$ROOT/$d" "$DEST/$d"
    find "$DEST/$d" -type d -name build -prune -exec rm -rf {} + 2>/dev/null || true
  done
fi
cp CMakeLists.txt README.md "$DEST/" 2>/dev/null || true
echo "==> done — restart mote_studio.exe in $DEST"
