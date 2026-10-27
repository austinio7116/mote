#!/usr/bin/env bash
# Run Mote.pdx in Panic's Playdate Simulator, in a window on this desktop.
#
#   ./scripts/playdate-sim.sh            (build first: ./scripts/build-playdate.sh)
#
# The Simulator is built against glibc 2.38, so it runs in an Ubuntu 24.04
# container (scripts/playdate/Dockerfile.sim) drawing through the host's X
# server -- WSLg's on Windows -- with sound through WSLg's PulseAudio when it is
# there. Its settings and the game's saves persist in build_playdate/simhome.
set -euo pipefail
cd "$(dirname "$0")/.."
SDK="${PLAYDATE_SDK_PATH:-$(ls -d "$HOME"/PlaydateSDK-* 2>/dev/null | sort -V | tail -1)}"
[ -d dist-playdate/Mote.pdx ] || { echo "no dist-playdate/Mote.pdx -- run ./scripts/build-playdate.sh" >&2; exit 1; }
docker build -q -t mote-playdate scripts/playdate >/dev/null
docker build -q -t mote-playdate-sim -f scripts/playdate/Dockerfile.sim scripts/playdate >/dev/null
mkdir -p build_playdate/simhome

audio=(-e SDL_AUDIODRIVER=dummy)
if [ -S /mnt/wslg/PulseServer ]; then
    audio=(-e SDL_AUDIODRIVER=pulseaudio -e PULSE_SERVER=unix:/mnt/wslg/PulseServer
           -v /mnt/wslg/PulseServer:/mnt/wslg/PulseServer)
fi

exec docker run --rm --user "$(id -u):$(id -g)" \
    -e HOME=/home/u -e DISPLAY="${DISPLAY:-:0}" -e PLAYDATE_SDK_PATH=/sdk "${audio[@]}" \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v "$PWD/build_playdate/simhome":/home/u \
    -v "$SDK":/sdk -v "$PWD":/src \
    mote-playdate-sim /sdk/bin/PlaydateSimulator /src/dist-playdate/Mote.pdx
