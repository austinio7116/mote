# CueVR

ThumbyCue on a headset: a real table, in your room, with a real cue.

A **separate app** from Mote VR — its own `applicationId` (`us.thumby.cuevr`),
its own icon, its own APK. The two share source, not identity.

It exists because most of it already did. `games/thumbycue` is a 128×128
handheld game, but its guts were written in SI units against real tables:

* **`cue_physics.c`** — 2 kHz fixed-substep integration, full three-axis angular
  velocity per ball, impulse collisions with Coulomb friction so throw and
  cushion english fall out rather than being faked, and a strike entry point
  that already takes a tip offset and a cue elevation:
  `cue_phys_strike_elev(w, ball, dir, speed, tip_side, tip_vert, elev)` — which
  is, near enough, the call a two-handed VR cue wants to make.
* **`cue_table.c`** — seven real tables, 7 ft pub to 12 ft snooker, with true
  cushion, jaw and pocket geometry.
* **`cue_rules.c`** — 8-ball, 9-ball and snooker, including the UK two-shot
  carry, push-out, free ball and foul-and-a-miss.
* **`cue_ai.c`** — an opponent, with eight personas and a resumable planner so
  it can think without stalling a frame.

All four are pure C with no engine dependency, and compile in unchanged.

## Playing

**Setup comes first, every session.** The point of cue sports in passthrough is
that the cloth lands on a surface you can lean on — a real table, a desk, the
end of a bed — so that when you drop your bridge hand it meets something. Get
the height wrong by five centimetres and the illusion is gone.

| Control | |
|---|---|
| **right stick ↕** | raise / lower the cloth (shown in cm — match your real surface) |
| **right stick ↔** | turn the table **about the cue ball** |
| **left stick** | slide the table, in your own view frame |
| **A** / right trigger | done |

Both sticks pivot about the **cue ball**, not the table's centre. The cue ball
is where you are standing and what you are about to hit, so keeping it still
while the table swings and slides underneath is the whole answer to a twelve-foot
table in a room that is not twelve feet long. Hold the **left menu** button for a
second at any time to place it again.

**The cue is unassisted**, as in Unlimited Snooker's natural mode. Your left hand
is the bridge it rests on, your right is the butt — the cue is the line between
them, so raising your back hand elevates it exactly as on a real table. Aim by
pointing. Put side or screw on by moving the line off the ball's centre. Play the
shot by pushing through: the power is the speed the tip is doing when it arrives,
measured along the cue's own axis, so a stroke delivered across the line
contributes only what actually goes into the ball. Too far off centre and the tip
slides off, because it does. No aim line, no power bar, no snapping.

## Build

```bash
cd cuevr && ./gradlew assembleRelease
```

Needs the Android SDK + NDK 26 (`local.properties`) and the vendored OpenXR
loader, which is shared with the Mote VR app at `vr/third_party/openxr`. The APK
lands in `app/build/outputs/apk/release/` — sideload it with SideQuest, Meta
Quest Developer Hub or `adb install -r`.

## Developing without a headset

The whole app except OpenXR builds as a desktop binary — same table, same
physics, same cue geometry, same shaders, same HUD, with mouse-driven hands:

```bash
cmake --build build_host --target cuevr_preview
./build_host/cuevr_preview            # drag to orbit, wheel to dolly
```

Headless, deterministic, and it screenshots itself:

```bash
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  CUEVR_TABLE=0 CUEVR_STROKE=1 MOTE_VR_SHOT=/tmp/break.png \
  MOTE_VR_SHOT_FRAME=1400 MOTE_VR_VIEW=20,42,2.0 ./build_host/cuevr_preview
```

| Variable | |
|---|---|
| `MOTE_VR_SHOT` / `_SHOT_FRAME` | render N frames, write a PNG, exit |
| `MOTE_VR_VIEW=yaw,pitch,dist` | where to put the fake head (pitch **positive** is above the cloth) |
| `CUEVR_TABLE=0..6` | skip the menu and start on this table |
| `CUEVR_STROKE=1` | play a scripted centre-ball break |

Captures use a fixed 1/72 s step, so frame N is always N/72 seconds in and the
same command always produces the same picture. Without that a capture runs at a
thousand frames a second and a break has barely started by the frame you asked
for — which looks exactly like physics that is not running.

Keys: WASD moves the bridge hand, arrows the butt, Q/E and PgUp/PgDn their
heights, SPACE strokes through the ball. Enter = A, M = left menu, IJKL/UO the
sticks.

The two parts that decide whether the game is any good are pure geometry, and
they are tested rather than felt out with a headset on:

```bash
cd cuevr/src
cc -I. -I../../games/thumbycue/src -I../../engine/math -I../../platform/xr \
   -o /tmp/test_cue test_cue.c cuevr_cue.c -lm && /tmp/test_cue
cc -I. -I../../games/thumbycue/src -I../../engine/math -I../../platform/xr \
   -o /tmp/test_setup test_setup.c cuevr_setup.c cuevr_cue.c -lm && /tmp/test_setup
```

## How it is put together

```
cuevr/src/cuevr_cue.c      two controllers -> a cue -> a strike  (26 assertions)
cuevr/src/cuevr_setup.c    putting the table in your room        (21 assertions)
cuevr/src/cuevr_render.c   GLES3: cloth, rails, pockets, balls, cue, HUD
cuevr/src/cuevr_app.c      the four callbacks platform/xr calls, and the flow
platform/xr/               the OpenXR host, shared with Mote VR
games/thumbycue/src/       physics, tables, rules, AI — unchanged
```

The table mesh is built **from the physics world**, not beside it: `cue_table`
fills a `CueWorld` with the cushion nose segments, jaw knuckles and pocket
circles the balls actually collide with, and the geometry is extruded from those
same segments. So the cushion you see and the cushion the ball bounces off are
the same numbers on all seven tables, and a pocket that looks tight is tight.

Balls are one unit sphere drawn many times, each with the orientation matrix the
physics already integrates from its angular velocity — so screw, follow and side
are visible on the ball as it runs. Stripes and spots are computed in object
space in the fragment shader: they roll correctly and cost no texture at all.

## Known gaps

* Not yet run on hardware.
* No sound. The handheld has baked clack/pot/cushion samples
  (`cue_*_pcm.h`) that should be played through an AAudio device, as the
  console app does.
* Ball-in-hand drops the cue ball on its home spot rather than letting you place
  it by hand, which in VR should just be picking it up.
* No snooker decision prompts yet (free ball / play-again after a foul); the
  rules engine has them, the VR UI does not ask.
* The AI persona is fixed at one of the eight — no difficulty picker.
