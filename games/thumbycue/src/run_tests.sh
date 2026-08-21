#!/usr/bin/env sh
# EVERY THUMBYCUE HOST TEST, BUILT AND RUN.
#
# These tests were being compiled by hand, one gcc line at a time, which is why
# build_host holds a handful of t_* binaries and not the whole set: a suite you
# have to remember the link line for is a suite that gets run in part. Every
# test_*.c in here links the same four translation units — the game's own
# simulation plus the rigid-body solver the skittles need — so there is nothing
# per-test to remember and adding a test_foo.c is the whole of adding a test.
#
#   ./run_tests.sh            build and run all of them
#   ./run_tests.sh spec bed   just the ones whose names contain these
#
# THE THREE -D FLAGS ARE NOT OPTIONAL. They are the shipping build's own
# settings — see CueVR's CMakeLists.txt and app/jni/Android.mk — and each one
# fails in a way that reads as a bug in the game:
#
#   CUE_JAW_SEGS=10  the pocket opening is tessellation-dependent, so a test
#                    built at the header's default of 3 measures a table nobody
#                    plays on: 84.88 mm at a snooker corner against 84.00.
#   CUE_MAX_SEG=256  ten jaw segments meet a default cap of 128, the cushion
#                    chain is silently truncated, and test_edge reports 500
#                    balls a run lost THROUGH the cushions on every rounded
#                    table. A harness failure that looks exactly like a physics
#                    failure.
#   CUE_ARC_SEGS=20  the same story for the cloth cut round each pocket.
#
# A harness that quietly measures a different table is worse than no harness.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$root/build_host/tests
mkdir -p "$out"

CC=${CC:-cc}
INC="-I$here -I$root/sdk -I$root/engine -I$root/engine/core -I$root/engine/math
     -I$root/engine/render -I$root/engine/assets -I$root/engine/input
     -I$root/engine/physics"
SRC="$here/cue_table.c $here/cue_physics.c $here/cue_rules.c $here/cue_ai.c
     $root/engine/physics/mote_phys.c $root/engine/core/mote_arena.c"
FLAGS="-O1 -g -DCUE_JAW_SEGS=10 -DCUE_ARC_SEGS=20 -DCUE_MAX_SEG=256 -Wall"

pass=0; fail=0; failed=""
for t in "$here"/test_*.c; do
    name=$(basename "$t" .c)
    if [ $# -gt 0 ]; then
        want=0
        for pat in "$@"; do case "$name" in *"$pat"*) want=1 ;; esac; done
        [ "$want" = 1 ] || continue
    fi
    bin=$out/$name
    # shellcheck disable=SC2086
    if ! $CC $FLAGS $INC -o "$bin" "$t" $SRC -lm 2>"$out/$name.build"; then
        printf '%-22s BUILD FAILED\n' "$name"
        sed 's/^/    /' "$out/$name.build" | head -20
        fail=$((fail+1)); failed="$failed $name"; continue
    fi
    if "$bin" >"$out/$name.log" 2>&1; then
        printf '%-22s ok\n' "$name"
        pass=$((pass+1))
    else
        printf '%-22s FAILED (rc=%d)  %s\n' "$name" "$?" "$out/$name.log"
        tail -12 "$out/$name.log" | sed 's/^/    /'
        fail=$((fail+1)); failed="$failed $name"
    fi
done
echo
echo "$pass passed, $fail failed${failed:+ —$failed}"
[ "$fail" = 0 ]
