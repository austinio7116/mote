#!/usr/bin/env bash
# Build Mote for the TrimUI Brick / Brick Hammer on its STOCK TrimUI OS: an app
# folder that drops into the SD card's Apps/ next to everything else, so nothing
# has to be reflashed or wiped.
#
#   ./scripts/build-trimui.sh      -> dist-trimui/Mote/  +  dist-trimui/Mote-trimui-brick.zip
#
# Copy dist-trimui/Mote to <SD card>/Apps/Mote. It shows up under Apps.
#
# The build runs in Docker (scripts/trimui/Dockerfile): LoveRetro's GCC 8.3
# aarch64 compiler over TrimUI's own SDK sysroot, glibc 2.28 + SDL2 2.26.1 --
# the libraries the stock OS ships. Unlike the Steam Deck bundle, SDL2 is NOT
# vendored: the device's libSDL2 carries TrimUI's Mali display backend, which a
# stock SDL2 build would not have.
#
# The same image runs the result under qemu-aarch64 for a headless check:
#   ./scripts/build-trimui.sh --test      (launcher + wolfmote frames -> dist-trimui/test/)
set -e
cd "$(dirname "$0")/.."
ROOT="$PWD"
STAGE=dist-trimui/Mote
ZIP=dist-trimui/Mote-trimui-brick.zip
IMAGE=mote-trimui

echo "==> toolchain image ($IMAGE)"
docker build -q -t "$IMAGE" scripts/trimui >/dev/null

in_tc() {
    docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT":/src -w /src "$IMAGE" bash -c "$1"
}

echo "==> building mote_shell (aarch64, stock TrimUI OS)"
in_tc 'cmake -B build_trimui -S . -DCMAKE_TOOLCHAIN_FILE=/src/scripts/trimui/toolchain.cmake \
             -DCMAKE_BUILD_TYPE=Release -DSDL2_DIR=/src/scripts/trimui/cmake >/dev/null &&
       cmake --build build_trimui --target mote_shell -j'"$(nproc)"

echo "==> building game modules (linux-aarch64)"
in_tc './android/tools/build_modules.sh --abi linux-aarch64 >/dev/null'

echo "==> staging -> $STAGE"
rm -rf "$STAGE" "$ZIP"; mkdir -p "$STAGE/games"
in_tc "\$STRIP -o $STAGE/mote_shell build_trimui/mote_shell"
for so in build_android/modules/linux-aarch64/*.so; do
    in_tc "\$STRIP --strip-unneeded -o $STAGE/games/$(basename "$so") $so"
done
cp android/app/src/main/res/mipmap-xxhdpi/ic_launcher.png "$STAGE/icon.png"

cat > "$STAGE/config.json" <<'JSON'
{
  "label": "Mote",
  "icon": "icon.png",
  "iconsel": "icon.png",
  "launch": "launch.sh",
  "description": "Mote games console"
}
JSON

# The Brick's controls are read by raw joystick number (the numbering
# NextUI/MinUI use for this device), not as an SDL GameController, so the A
# button is the one labelled A whatever mapping SDL would guess.
#   1 A  0 B  4 L1  5 R1  axes 2/5 L2/R2  6 SELECT  7 START  8 MENU
# START and SELECT are the Thumby's MENU; the Brick's own MENU button opens
# Mote's settings, which has Quit Mote to get back to the TrimUI menu.
cat > "$STAGE/launch.sh" <<'SH'
#!/bin/sh
cd "$(dirname "$0")"
export MOTE_SHELL_FULLSCREEN=1
export MOTE_SHELL_BARE=1
export MOTE_SHELL_QUIT=1
export MOTE_SHELL_JOYMAP="a=1,b=0,lb=4,rb=5,lb=a2,rb=a5,menu=7,menu=6,panel=8"
export MOTE_GAME_DIR="$PWD/games"
./mote_shell > mote.log 2>&1
SH

cat > "$STAGE/README.txt" <<'TXT'
Mote for TrimUI Brick / Brick Hammer (stock TrimUI OS)
======================================================
Copy this Mote folder to the SD card as  Apps/Mote  and it appears under Apps.
Nothing else on the card is touched.

Controls
  D-pad / stick   D-pad
  A, B            A, B
  L1 / L2         LB        R1 / R2   RB
  START, SELECT   the Thumby's MENU button
  MENU            Mote settings (Quit Mote is in there)

Saves, settings and a log (mote.log) live in this folder.
TXT

if [ "${1:-}" = --test ]; then
    echo "==> qemu check"
    mkdir -p dist-trimui/test
    in_tc 'cd dist-trimui/test && export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
             MOTE_SHELL_FULLSCREEN=1 MOTE_SHELL_BARE=1 MOTE_SHELL_QUIT=1 MOTE_SHELL_SIZE=1024x768 \
             MOTE_SHELL_JOYMAP="a=1,b=0,panel=8" MOTE_GAME_DIR=/src/'"$STAGE"'/games &&
           MOTE_SHELL_SHOT=launcher.ppm MOTE_SHELL_SHOT_FRAME=90 qemu-aarch64 -L $SYSROOT /src/'"$STAGE"'/mote_shell 2>&1 | grep -E "installed|undefined|wrote" &&
           MOTE_SHELL_AUTORUN=wolfmote MOTE_SHELL_SHOT=wolfmote.ppm MOTE_SHELL_SHOT_FRAME=150 qemu-aarch64 -L $SYSROOT /src/'"$STAGE"'/mote_shell 2>&1 | grep -E "launching|undefined|wrote"'
fi

echo "==> zipping"
( cd dist-trimui && rm -f "$(basename "$ZIP")" && zip -qr "$(basename "$ZIP")" Mote )
echo "==> $ZIP ($(du -h "$ZIP" | cut -f1))"
