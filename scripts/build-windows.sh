#!/usr/bin/env bash
# Build a fully self-contained Windows bundle of Mote Studio:
#   mote_studio.exe + the repo + a portable MinGW (gcc) + ffmpeg.
# The Studio auto-adds toolchain/bin and ffmpeg/bin to PATH at startup, so games
# build and audio decodes with zero setup. Unzip anywhere on a LOCAL drive and run.
#   ./scripts/build-windows.sh        -> full bundle (dist-windows/MoteStudio-win64.zip)
#   ./scripts/build-windows.sh exe    -> just the .exe (dist-windows/mote_studio.exe)
set -e
cd "$(dirname "$0")/.."
ROOT="$PWD"
SDLVER=2.30.9
W64VER=1.23.0
WB=.winbuild
mkdir -p "$WB"

# --- 1. SDL2 (mingw dev) + cross-compile the exe ---------------------------
if [ ! -d "$WB/SDL2-$SDLVER" ]; then
  echo "==> fetching SDL2 $SDLVER (mingw dev)"
  curl -sL "https://github.com/libsdl-org/SDL/releases/download/release-$SDLVER/SDL2-devel-$SDLVER-mingw.tar.gz" -o "$WB/sdl2.tar.gz"
  tar xzf "$WB/sdl2.tar.gz" -C "$WB"
fi
SDL2W="$ROOT/$WB/SDL2-$SDLVER/x86_64-w64-mingw32"
echo "==> cross-compiling mote_studio.exe"
cmake -B build_win -S . -DCMAKE_TOOLCHAIN_FILE="$ROOT/scripts/mingw-toolchain.cmake" -DSDL2_DIR="$SDL2W/lib/cmake/SDL2" >/dev/null
cmake --build build_win --target mote_studio -j8

if [ "$1" = "exe" ]; then
  mkdir -p dist-windows; cp build_win/mote_studio.exe dist-windows/
  echo "==> dist-windows/mote_studio.exe"; exit 0
fi

# --- 2. portable MinGW (w64devkit) -----------------------------------------
if [ ! -d "$WB/w64devkit" ]; then
  echo "==> fetching w64devkit $W64VER (portable gcc, ~76MB)"
  curl -sL "https://github.com/skeeto/w64devkit/releases/download/v$W64VER/w64devkit-$W64VER.zip" -o "$WB/w64devkit.zip"
  unzip -q "$WB/w64devkit.zip" -d "$WB"
fi

# --- 3. ffmpeg (win64) -----------------------------------------------------
if [ ! -d "$WB/ffmpeg" ]; then
  echo "==> fetching ffmpeg (win64, ~80MB)"
  curl -sL "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip" -o "$WB/ffmpeg.zip"
  unzip -q "$WB/ffmpeg.zip" -d "$WB"
  mv "$WB"/ffmpeg-master-latest-win64-gpl "$WB/ffmpeg"
fi

# --- 3b. arm-none-eabi-gcc (win64) — device .mote builds -------------------
ARMVER=13.3.1-1.1
if [ ! -d "$WB/arm" ]; then
  echo "==> fetching arm-none-eabi-gcc $ARMVER (win64, ~278MB)"
  curl -sL "https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v$ARMVER/xpack-arm-none-eabi-gcc-$ARMVER-win32-x64.zip" -o "$WB/arm.zip"
  unzip -q "$WB/arm.zip" -d "$WB"
  mv "$WB"/xpack-arm-none-eabi-gcc-* "$WB/arm"
fi

# --- 4. stage the bundle ---------------------------------------------------
STAGE=dist-windows/MoteStudio
echo "==> staging bundle -> $STAGE"
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp build_win/mote_studio.exe "$STAGE/"
for d in engine sdk os platform studio examples tools; do cp -r "$d" "$STAGE/$d"; done
cp CMakeLists.txt README.md "$STAGE/" 2>/dev/null || true
find "$STAGE" -type d -name build -prune -exec rm -rf {} + 2>/dev/null || true
# portable toolchain (rename w64devkit -> toolchain so add_bundled_toolchain finds toolchain/bin)
cp -r "$WB/w64devkit" "$STAGE/toolchain"
cp -r "$WB/arm" "$STAGE/arm"          # arm-none-eabi-gcc for device .mote builds (Push & Launch)
mkdir -p "$STAGE/ffmpeg/bin"
cp "$WB/ffmpeg/bin/ffmpeg.exe" "$WB/ffmpeg/bin/ffplay.exe" "$WB/ffmpeg/bin/ffprobe.exe" "$STAGE/ffmpeg/bin/" 2>/dev/null || true
cat > "$STAGE/README.txt" <<'TXT'
Mote Studio for Windows (self-contained)
========================================
Unzip this folder anywhere on a LOCAL drive (e.g. C:\MoteStudio) and run
mote_studio.exe from inside it.

Everything is bundled: the engine + examples, a portable MinGW gcc (toolchain\),
arm-none-eabi-gcc for device builds (arm\), and ffmpeg (ffmpeg\). Run / Build / Bake /
Push & Launch / audio Load all work with no setup — just connect a Mote board over USB.
TXT

# --- 5. zip ----------------------------------------------------------------
echo "==> zipping"
( cd dist-windows && rm -f MoteStudio-win64.zip && zip -qr MoteStudio-win64.zip MoteStudio )
du -h dist-windows/MoteStudio-win64.zip | awk '{print "==> dist-windows/MoteStudio-win64.zip ("$1")"}'
