#!/usr/bin/env bash
# Cross-compile Mote Studio for Windows (x86_64) from Linux using MinGW-w64.
set -e
cd "$(dirname "$0")/.."
SDLVER=2.30.9
WB=.winbuild
mkdir -p "$WB"
if [ ! -d "$WB/SDL2-$SDLVER" ]; then
  echo "fetching SDL2 $SDLVER (mingw dev)..."
  curl -sL "https://github.com/libsdl-org/SDL/releases/download/release-$SDLVER/SDL2-devel-$SDLVER-mingw.tar.gz" -o "$WB/sdl2.tar.gz"
  tar xzf "$WB/sdl2.tar.gz" -C "$WB"
fi
SDL2W="$PWD/$WB/SDL2-$SDLVER/x86_64-w64-mingw32"
cmake -B build_win -S . -DCMAKE_TOOLCHAIN_FILE="$PWD/scripts/mingw-toolchain.cmake" -DSDL2_DIR="$SDL2W/lib/cmake/SDL2" >/dev/null
cmake --build build_win --target mote_studio -j8
mkdir -p dist-windows
cp build_win/mote_studio.exe dist-windows/
cp "$SDL2W/bin/SDL2.dll" dist-windows/
cp scripts/WINDOWS-README.txt dist-windows/ 2>/dev/null || true
echo "==> dist-windows/mote_studio.exe (+ SDL2.dll)"
