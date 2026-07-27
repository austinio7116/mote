#!/usr/bin/env bash
# Build Mote game modules as libmg_<game>.so.
#
# The APK builds its bundled games through ndk-build (android/app/jni/games), so
# this script is for the two things that sit outside that:
#
#   --abi host                for the desktop build of the shell (`mote_shell`)
#   --abi arm64-v8a|...       a side-loadable module to drop into
#                             Android/data/us.thumby.mote/files/games/
#
# A module links NO engine code; it reaches the engine only through the ABI it is
# handed — exactly like the .so the Studio loads and the .mote the handheld runs.
#
#   ./android/tools/build_modules.sh                       # host, every game
#   ./android/tools/build_modules.sh --abi arm64-v8a moita # one game, for a phone
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ABI=host
OUT=""
GAMES=()

while [ $# -gt 0 ]; do
    case "$1" in
        --abi) ABI="$2"; shift 2;;
        --out) OUT="$2"; shift 2;;
        -h|--help) sed -n '2,16p' "$0"; exit 0;;
        *) GAMES+=("$1"); shift;;
    esac
done

[ -n "$OUT" ] || OUT="$ROOT/build_android/modules${ABI:+/}${ABI#host}"
OUT="${OUT%/}"
mkdir -p "$OUT"

if [ "$ABI" = host ]; then
    CC=${CC:-gcc}
    PIC=-fPIC
else
    NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}"
    if [ -z "$NDK" ]; then
        SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/android-sdk}}"
        NDK=$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1 || true)
    fi
    [ -n "$NDK" ] || { echo "build_modules: no NDK found (set ANDROID_NDK_HOME)" >&2; exit 1; }
    BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
    case "$ABI" in
        arm64-v8a)   TRIPLE=aarch64-linux-android21;;
        armeabi-v7a) TRIPLE=armv7a-linux-androideabi21;;
        x86_64)      TRIPLE=x86_64-linux-android21;;
        x86)         TRIPLE=i686-linux-android21;;
        *) echo "build_modules: unknown abi '$ABI'" >&2; exit 1;;
    esac
    CC="$BIN/$TRIPLE-clang"
    PIC=-fPIC
fi

INCS=(-I"$ROOT/engine/core" -I"$ROOT/engine/math" -I"$ROOT/engine/render"
      -I"$ROOT/engine/assets" -I"$ROOT/engine/input" -I"$ROOT/engine/physics"
      -I"$ROOT/sdk")

# The ancient K&R C in vendored ports (and a few cross-module helper calls that
# gcc only warns about) must not fail the clang build.
LENIENT=(-Wno-implicit-function-declaration -Wno-deprecated-non-prototype
         -Wno-format-truncation)

if [ ${#GAMES[@]} -eq 0 ]; then
    for d in "$ROOT"/games/*/; do
        compgen -G "$d/src/*.c" > /dev/null || continue
        GAMES+=("$(basename "$d")")
    done
fi

fail=0
for g in "${GAMES[@]}"; do
    dir="$ROOT/games/$g"
    [ -d "$dir" ] || dir="$ROOT/examples/$g"
    if ! compgen -G "$dir/src/*.c" > /dev/null; then
        echo "  skip $g (no src/*.c)"; continue
    fi
    cf=()
    [ -f "$dir/cflags" ] && while read -r tok; do cf+=("$tok"); done < <(sed 's/#.*//' "$dir/cflags" | tr -s ' \n' '\n' | sed '/^$/d')
    if "$CC" -shared $PIC -O2 -ffast-math -DMOTE_HOST=1 \
             "${LENIENT[@]}" "${cf[@]}" "${INCS[@]}" -I"$dir/src" \
             "$dir"/src/*.c -lm -o "$OUT/libmg_$g.so" 2> "$OUT/$g.log"; then
        printf '  ok   %-16s %8d bytes\n' "$g" "$(stat -c%s "$OUT/libmg_$g.so")"
        rm -f "$OUT/$g.log"
    else
        echo "  FAIL $g  (see $OUT/$g.log)"
        grep -m3 'error:' "$OUT/$g.log" | sed 's/^/        /' || true
        fail=1
    fi
done
echo "modules -> $OUT"
exit $fail
