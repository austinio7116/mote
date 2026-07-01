#!/usr/bin/env bash
# Build a Mote Studio bundle for Linux (x86-64):
#   mote_studio (native SDL2 binary) + the repo (engine/sdk/examples/games/tools).
# Unlike the Windows bundle, no toolchain is embedded — Linux gets gcc, SDL2,
# ffmpeg and arm-none-eabi-gcc from the distro (see README.txt in the bundle).
#   ./scripts/build-linux.sh          -> dist-linux/MoteStudio-linux-x64.tar.gz
set -e
cd "$(dirname "$0")/.."
ROOT="$PWD"

echo "==> building mote_studio (native)"
cmake -B build_host -S . >/dev/null
cmake --build build_host --target mote_studio -j"$(nproc)"

STAGE=dist-linux/MoteStudio
echo "==> staging bundle -> $STAGE"
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp build_host/mote_studio "$STAGE/"
for d in engine sdk os platform studio examples games tools; do cp -r "$d" "$STAGE/$d"; done
cp CMakeLists.txt README.md "$STAGE/" 2>/dev/null || true
find "$STAGE" -type d -name build -prune -exec rm -rf {} + 2>/dev/null || true
# Release hygiene: drop anything git doesn't track (in-progress games/tools
# in the working tree must never leak into the public bundle).
git ls-files --others --directory --exclude-standard -z -- engine sdk os platform studio examples games tools \
  | while IFS= read -r -d '' f; do rm -rf "$STAGE/$f"; done

cat > "$STAGE/README.txt" <<'TXT'
Mote Studio for Linux (x86-64)
==============================
Untar anywhere and run ./mote_studio from inside this folder.

The engine, SDK, examples and games are all here. The Studio uses your
system toolchain — install once:

  sudo apt install build-essential libsdl2-dev ffmpeg gcc-arm-none-eabi libnewlib-arm-none-eabi

  - gcc + SDL2: build & hot-reload games in the on-screen emulator
  - ffmpeg: load WAV/MP3 in the Audio tab
  - arm-none-eabi-gcc: build device .mote files (Push & Launch over USB)

The mote CLI lives at tools/mote (build/run/bake/push from a terminal).
TXT

echo "==> tarring"
( cd dist-linux && rm -f MoteStudio-linux-x64.tar.gz && tar czf MoteStudio-linux-x64.tar.gz MoteStudio )
du -h dist-linux/MoteStudio-linux-x64.tar.gz | awk '{print "==> dist-linux/MoteStudio-linux-x64.tar.gz ("$1")"}'
