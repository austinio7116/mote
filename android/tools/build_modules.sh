#!/usr/bin/env bash
# Build Mote game modules as libmg_<game>.so.
#
# The APK builds its bundled games through ndk-build (android/app/jni/games), so
# this script is for the two things that sit outside that:
#
#   --abi host                for the desktop build of the shell (`mote_shell`)
#   --abi arm64-v8a|...       a side-loadable module to drop into
#                             Android/data/us.thumby.mote/files/games/
#   --publish                 stage stripped modules into docs/games/android/<abi>
#                             so tools/gen_gallery.py publishes them in games.json
#                             and the app's gallery can install them
#
# A module links NO engine code; it reaches the engine only through the ABI it is
# handed — exactly like the .so the Studio loads and the .mote the handheld runs.
#
#   ./android/tools/build_modules.sh                          # host, every game
#   ./android/tools/build_modules.sh --abi arm64-v8a moita    # one game, for a phone
#   ./android/tools/build_modules.sh --abi arm64-v8a --publish  # stage the gallery
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ABI=host
OUT=""
PUBLISH=0
GAMES=()

while [ $# -gt 0 ]; do
    case "$1" in
        --abi) ABI="$2"; shift 2;;
        --out) OUT="$2"; shift 2;;
        --publish) PUBLISH=1; shift;;
        -h|--help) sed -n '2,20p' "$0"; exit 0;;
        *) GAMES+=("$1"); shift;;
    esac
done

if [ "$PUBLISH" = 1 ]; then
    [ "$ABI" != host ] || { echo "build_modules: --publish needs a real --abi" >&2; exit 1; }
    [ -n "$OUT" ] || OUT="$ROOT/docs/games/android/$ABI"
fi

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
    STRIP="$BIN/llvm-strip"
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
    if [ "$PUBLISH" = 1 ]; then
        # Only what the gallery actually lists: gen_gallery.py matches modules by
        # manifest id, so anything else staged here would just bloat docs/.
        while read -r g; do GAMES+=("$g"); done < <(
            sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/docs/games.json")
        [ ${#GAMES[@]} -gt 0 ] || { echo "build_modules: no ids in docs/games.json" >&2; exit 1; }
    else
        for d in "$ROOT"/games/*/; do
            compgen -G "$d/src/*.c" > /dev/null || continue
            GAMES+=("$(basename "$d")")
        done
    fi
fi

# moria's coroutine layer needs getcontext/swapcontext, which bionic doesn't
# implement — it would build and then fail to dlopen. Keep it in step with
# MOTE_GAME_SKIP in android/app/jni/games/Android.mk.
if [ "$ABI" != host ]; then
    filtered=()
    for g in "${GAMES[@]}"; do [ "$g" = moria ] || filtered+=("$g"); done
    GAMES=("${filtered[@]}")
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
        # Published modules are downloaded over a phone connection, so drop the
        # symbol table — nothing dlsym's a game beyond its four ABI symbols.
        [ "$PUBLISH" = 1 ] && "${STRIP:-true}" --strip-unneeded "$OUT/libmg_$g.so" 2>/dev/null || true
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
