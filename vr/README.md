# Mote in VR

The Thumby Color, held in your hands, in your actual room.

Not a re-implementation and not a screen floating in a void: the **whole Mote
engine and OS** are compiled for the headset, every game is built from the same
`games/<name>/src/*.c` the handheld runs, and the console is a 3D object you
hold between two tracked controllers while the room shows through behind it.
The launcher, the engine menu, save slots, the gallery and multiplayer all
behave exactly as they do on hardware, at the same 128×128, 60 fps and 22 kHz.

```
┌──────────────────────────────────────────────────────────────┐
│  MoteVrActivity (Java)   NativeActivity + storage + HTTPS    │
├──────────────────────────────────────────────────────────────┤
│  platform/vr/mote_vr_main_android.c   android_native_app_glue│
│  platform/vr/mote_vr_xr.c             OpenXR: session, hands,│
│                                       swapchains, passthrough│
│  platform/vr/mote_vr_hold.c           hands -> pose + buttons│
│  platform/vr/mote_vr_render.c         GLES3: chassis + LCD   │
├──────────────────────────────────────────────────────────────┤
│  platform/android/mote_plat_android.c   mote_platform.h      │
│  platform/android/mote_link_android.c   link + MN1 server    │
├──────────────────────────────────────────────────────────────┤
│  os/android/mote_android_os.c    launcher + dlopen loop      │
│  os/  engine/                    UNCHANGED, shared with the  │
│                                  phone, desktop and RP2350   │
├──────────────────────────────────────────────────────────────┤
│  lib/arm64-v8a/libmg_<game>.so   one module per game         │
└──────────────────────────────────────────────────────────────┘
```

There is **no SDL** in the headset build. The phone shell needs it for a window,
a touchscreen and an audio device; a headset has none of those. Audio goes
straight to AAudio, the display is OpenXR's own swapchain, and the eleven
portability calls the shared networking code makes into SDL are answered by
`platform/vr/compat/SDL.h`.

## Controls

The console sits **between your two controllers** and follows them: its centre
is the midpoint of your hands, its left-right axis is the line between them, and
its screen always turns to face you — so you can never tilt it away from
yourself by accident.

| On the controller | On the console |
|---|---|
| either thumbstick | d-pad |
| **A** / **X** | A |
| **B** / **Y** | B |
| left trigger / right trigger | LB / RB |
| left **menu** | MENU (hold 3 s for the engine menu) |
| **both grips**, hands apart/together | resize the console |

Rumble goes to both controllers. Whichever button a press landed on lights up on
the console itself, because a controller trigger has no travel to tell you.

**Size.** A real Thumby Color is 51.6 mm across, and at life size its 128×128
screen is about sixty headset pixels — a smudge, not a display. So it is held at
3× by default (155 mm, near enough a Game Boy Advance), which puts the LCD at
roughly 1.5 headset pixels per Mote pixel. Squeeze both grips to change it.

## Build

Needs the Android SDK + NDK 26 (`vr/local.properties` points at the SDK) and the
chassis assets, which are generated rather than checked in:

```bash
python3 vr/tools/gen_vr_chassis.py          # chassis.mvm + chassis.jpg
cd vr && ./gradlew assembleRelease -PmoteGames=all
```

`-PmoteGames` takes `all`, `none` (the default — the app fills itself from the
gallery), or a list: `-PmoteGames="moita wormote"`. The APK lands in
`vr/app/build/outputs/apk/release/`.

## Install

Sideload it — this is not a store build:

* **SideQuest**: drag the APK onto the window with the Quest connected.
* **Meta Quest Developer Hub**: Device → Apps → Install APK.
* **adb**: `adb install -r mote-vr.apk`

The headset needs developer mode on. It appears in the library under
**Unknown Sources** (Horizon OS files sideloaded apps there, not on the main
shelf).

## Developing without a headset

A Quest build cannot be looked at from the machine that writes it, so the whole
app minus OpenXR builds as a desktop binary: the real engine on its real worker
thread, the real launcher, the real mesh, the real shaders and the real hold
logic, with two fake controllers driven by a mouse.

```bash
cmake --build build_host --target mote_vr_preview
./build_host/mote_vr_preview                       # drag to orbit, wheel to dolly
```

It renders headless too, which is how the model, the lighting, the LCD placement
and the button glows were checked before an APK existed:

```bash
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  MOTE_VR_SHOT=/tmp/vr.png MOTE_VR_VIEW=18,-14,0.32 MOTE_VR_KEYS="b up" \
  ./build_host/mote_vr_preview
```

| Variable | |
|---|---|
| `MOTE_VR_SHOT` / `_SHOT_FRAME` | render N frames, write a PNG, exit |
| `MOTE_VR_VIEW=yaw,pitch,dist` | where to put the fake head |
| `MOTE_VR_KEYS="a b up"` | buttons held for the shot |
| `MOTE_VR_TESTCARD=1` | replace the LCD with a known pattern |
| `MOTE_VR_BACKDROP=0` | no floor grid — what passthrough looks like |
| `MOTE_VR_GAME=wormote` | boot straight into a game |
| `MOTE_VR_TILT` / `MOTE_VR_SPAN` | console pitch, hand separation |

Keys: WASD/arrows d-pad, J/K A/B, U/I LB/RB, Enter MENU, `[` `]` resize.

## The model

`vr/assets/chassis.mvm` is a **stand-in**, generated by extruding the alpha
silhouette of the product photo (`studio/assets/thumby_color.png`) into a slab
with a rounded rim and mapping the photo onto its face. Photo-accurate from the
front — every button, the bezel and the print are exactly where they belong,
with the real product's own lighting baked in — but it is a slab: no moulded
buttons, no shoulders you can see round, a flat back.

The renderer loads the mesh **at runtime**, so a real model replaces it with no
code change:

```bash
python3 vr/tools/obj2mvm.py thumby.obj vr/assets/chassis.mvm \
    --up=Y --front=Z --width 51.6 --screen-object screen
```

It normalises units, centres the origin, fixes handedness and prints the
`MOTE_VR_SCREEN_*` defines to paste into `platform/vr/mote_vr_chassis.h`.

## Passthrough

On a Quest the camera feed is an `XR_FB_passthrough` composition layer submitted
*under* the projection layer; the projection layer is cleared fully transparent
so it shows through, and the frame's blend mode stays `OPAQUE` (Meta's runtime
requires that even with passthrough running — the feed is a layer, not a blend
mode). On a runtime without the extension the app draws a horizon and floor grid
instead of a void, so it still makes sense on SteamVR.

## SteamVR / PC VR

The renderer, the hold logic and the OpenXR calls are all shared, and SteamVR's
OpenXR runtime has supported `XR_KHR_opengl_enable` on Windows and Linux since
2020 — so a PC build needs a desktop entry point (window, GL context, and
`XR_KHR_opengl_enable` in place of `XR_KHR_opengl_es_enable`) rather than a
second renderer. That entry point is not written yet; `mote_vr_preview` is the
desktop harness it would grow out of.

## Known gaps

* Not yet run on hardware — everything here is verified by the desktop preview,
  by compiling for arm64, and by inspection of the APK.
* No USB dock: a Quest can host USB-C, so a real Thumby Color could plug into it
  for gallery downloads and online play exactly as it does on a phone
  (`os/android/mote_android_dock.c` is not in the VR build yet).
* No in-headset settings panel — brightness, volume and the relay address are
  reachable through the engine menu (hold MENU) but not through a VR UI.
* Console scale is not yet persisted between runs.
