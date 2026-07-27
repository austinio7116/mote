# Mote for Android

The Thumby Color, on a phone. Not a re-implementation: the **whole Mote engine and
OS** are compiled for Android, and every game is built from the same
`games/<name>/src/*.c` the handheld runs — so the launcher, the engine menu, save
slots, the multiplayer lobby and the games themselves behave exactly as they do on
hardware, at the same 128×128, 60 fps and 22 kHz.

The screen is the real product photo (`studio/assets/thumby_color.png`, with the
Studio's own `screen.cfg` calibration), and the touch targets sit on the buttons
you can see in it.

```
┌──────────────────────────────────────────────────────────────┐
│  MoteActivity (Java)   libraries, module dirs, vibrator      │
├──────────────────────────────────────────────────────────────┤
│  platform/android/mote_shell.c        SDL main thread        │
│    chassis photo · touch · gamepad · settings panel          │
├──────────────────────────────────────────────────────────────┤
│  platform/android/mote_plat_android.c   mote_platform.h      │
│  platform/android/mote_link_android.c   link + MN1 server    │
├──────────────────────────────────────────────────────────────┤
│  os/android/mote_android_os.c    launcher + dlopen loop      │
│  os/  engine/                    UNCHANGED, shared with the  │
│                                  device and host builds      │
├──────────────────────────────────────────────────────────────┤
│  lib/arm64-v8a/libmg_<game>.so   one module per game         │
└──────────────────────────────────────────────────────────────┘
```

## Controls

**Touch** — press the buttons on the photo: d-pad, A, B, LB/RB in the top corners,
MENU below the d-pad. Multi-touch, and sliding between d-pad arms re-triggers, so
diagonals and rolls work. Each press gives a short haptic tick.

**Game controller** (auto-detected, USB or Bluetooth): d-pad *or* left stick =
d-pad, A/B = A/B, L1/R1 = LB/RB, triggers double as A/B, Start = MENU. The touch
highlights fade out while the pad is in use, and rumble goes to the pad instead of
the phone.

**System** — hold MENU alone for 3 s for the engine menu (brightness, volume,
return to launcher), exactly as on the handheld. RB in the launcher opens the
about/gallery screen. The Android back gesture opens the shell's own settings.

## Settings

The pip in the top-right corner (or the back gesture) opens a panel drawn with the
OS's own UI kit:

| Row | What it does |
|-----|--------------|
| `LAYOUT` | `CHASSIS` — the LCD is an exact integer multiple of 128, so the frame is pixel-perfect; the chassis is sized to match. `FILL` — chassis scaled to fill the screen (bigger, slightly soft). |
| `SHELL` | the solid product photo, or the see-through chassis. |
| `HAPTICS` | touch ticks + game rumble on/off. |
| `RELAY` | the internet-relay address for multiplayer (`host` or `host:port`, default port 443). Tapping it opens the keyboard. |

Settings and saves live in the app's private storage, so an uninstall is the only
thing that clears them.

## Multiplayer

Everything the handheld can do except the USB cable, and the cable's slot is
reused rather than wasted:

| Lobby option | On a phone |
|--------------|-----------|
| **Internet** | Quick Match / Host Room / Join Code / Browse Rooms, over the same relay the Studio uses — so a phone and a Studio-docked Thumby can share a room. |
| **LAN (Wi-Fi)** | Host / Join with zero-config UDP discovery on the local network. |
| **USB Cable** | there is no cable, so this becomes **auto-pair on the local network**: the two devices alternate host/join roles with a jittered period until they rendezvous. |

On the handheld the lobby arranges all of this by speaking the MN1 control
protocol over USB to a docked Studio. Android has no Studio to talk to, so
`platform/android/mote_link_android.c` **is** that side: it answers MN1 in
process, drives the same `studio/link_net.c` transport, and then goes transparent
so the game's `link_send`/`link_recv` are the raw peer pipe. Every transition is
logged to logcat under `mote` (`adb logcat -s mote`).

## The game list

Every folder under `games/` with a `src/*.c` becomes `libmg_<folder>.so` in the
APK, and the launcher discovers them by filename — so the whole published gallery
is installed out of the box and adding a game is just adding its folder.

One exclusion: **moria**. The Umoria port's coroutine layer needs
`getcontext`/`makecontext`/`swapcontext`, which bionic does not implement; it needs
a thread- or `setjmp`-based fiber before it can ship here.

To add a module after the fact — a new game, or one you're iterating on — build it
standalone and drop it in:

```bash
./android/tools/build_modules.sh --abi arm64-v8a mygame
adb push build_android/modules/arm64-v8a/libmg_mygame.so \
    /sdcard/Android/data/us.thumby.mote/files/games/
```

The launcher scans that directory after the bundled ones (bundled modules win a
filename tie).

## Building

Prerequisites: Android SDK (platform 35, build-tools 34+), NDK r26+
(`26.3.11579264` is what this was built with), JDK 17+, and a
`local.properties` with `sdk.dir=/path/to/android-sdk` (not committed).

One-time: vendor the SDL2 source (large, not committed — its Java glue is used in
place, so no second copy can drift):

```bash
git clone --depth 1 --branch SDL2 https://github.com/libsdl-org/SDL.git app/jni/SDL
```

Then:

```bash
cd android
ANDROID_HOME=/path/to/android-sdk ./gradlew assembleDebug     # or assembleRelease
# -> app/build/outputs/apk/debug/app-debug.apk
```

Useful properties:

```bash
./gradlew assembleRelease -PmoteAbis=arm64-v8a,x86_64   # add the emulator ABI
./gradlew assembleDebug   -PmoteGames="moita wormote"   # a couple of games, for a fast loop
```

arm64-v8a alone is the default: the APK carries ~30 game modules per ABI, and
every phone shipped since about 2015 is arm64. Release APKs are signed with the
debug key — this is a sideload, not a Play Store upload.

## Developing the shell without a phone

The shell is one source file that also builds for the desktop, so the chassis
layout, touch mapping, launcher, games and multiplayer can all be exercised
without an install:

```bash
./android/tools/build_modules.sh                  # host game modules
cmake -S . -B build_shell -DCMAKE_BUILD_TYPE=Release
cmake --build build_shell --target mote_shell -j8
./build_shell/mote_shell                          # mouse = finger, keyboard = buttons, F1 = settings
```

Headless capture and scripted input work the same way as the host emulator's:

```bash
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
MOTE_SHELL_AUTORUN=papermote \
MOTE_SHELL_KEYS="down:60-66 a:120-126" \
MOTE_SHELL_SHOT=/tmp/shell.ppm MOTE_SHELL_SHOT_FRAME=200 \
./build_shell/mote_shell
```

| Variable | Effect |
|----------|--------|
| `MOTE_GAME_DIR` | where to find `libmg_*.so` (default `build_android/modules`) |
| `MOTE_SHELL_AUTORUN` | boot straight into a module, skipping the launcher |
| `MOTE_SHELL_KEYS` | `btn:from-to` frame windows, ~60 frames/s |
| `MOTE_SHELL_SHOT` / `_FRAME` | dump the composited window as a PPM, then quit |
| `MOTE_SHELL_PANEL` | open the settings panel at start (for captures) |

Two instances on one machine pair over LAN, which is how the multiplayer path is
tested end-to-end.
