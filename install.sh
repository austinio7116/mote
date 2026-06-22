#!/usr/bin/env bash
#
# Mote — Linux / WSL2 installer. Installs the build dependencies, then builds
# Mote Studio (the IDE + engine + emulator) from source. Run from the repo root:
#
#     ./install.sh
#
# Then launch with:  ./build_host/mote_studio    (or:  ./tools/mote studio)
#
set -euo pipefail
cd "$(dirname "$0")"

SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"

echo "==> Installing dependencies (apt)"
if command -v apt-get >/dev/null 2>&1; then
    $SUDO apt-get update -qq
    # required: gcc/make, cmake, SDL2 (window/input/audio), ImageMagick (image baking)
    $SUDO apt-get install -y build-essential cmake libsdl2-dev imagemagick ffmpeg python3 python3-pip
    # optional: device cross-compiler (for `Push` / .mote builds) — don't fail if unavailable
    $SUDO apt-get install -y gcc-arm-none-eabi || echo "   (gcc-arm-none-eabi not installed — device Push will be disabled)"
else
    echo "!! apt-get not found. Install these yourself, then re-run:"
    echo "   build-essential cmake libsdl2-dev imagemagick ffmpeg python3  (+ gcc-arm-none-eabi for device builds)"
    exit 1
fi
python3 -m pip install --user --quiet pyserial 2>/dev/null || true   # for `mote push` / `mote logs`

echo "==> Building Mote Studio"
cmake -B build_host -S . >/dev/null
cmake --build build_host --target mote_studio -j"$(nproc)"

chmod +x tools/mote 2>/dev/null || true
echo
echo "Done. Launch Mote Studio with:"
echo "    ./build_host/mote_studio"
echo "or use the CLI:"
echo "    ./tools/mote studio        # IDE"
echo "    ./tools/mote run examples/tiledemo   # run a game in the emulator"
