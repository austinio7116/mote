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
return to launcher), exactly as on the handheld. **RB** in the launcher opens the
online gallery. The Android back gesture opens the shell's own settings.

## Settings

The pip at the top-centre (or the back gesture) opens a panel drawn with the OS's
own UI kit. Every row is a tap target and nothing in it needs a key the phone
doesn't have:

| Row | What it does |
|-----|--------------|
| `LAYOUT` | `CHASSIS` — the LCD is an exact integer multiple of 128, so the frame is pixel-perfect; the chassis is sized to match. `FILL` — chassis scaled to fill the screen (bigger, slightly soft). |
| `SHELL` | the solid product photo, or the see-through chassis. |
| `HAPTICS` | touch ticks + game rumble on/off. |
| `FPS` | the engine's measured frame rate. Tapping it cycles the on-LCD perf overlay (`OFF` / `FPS` / `MINI` / `FULL`) — the touch equivalent of the handheld's LB+RB. |
| `RELAY` | the internet-relay address for multiplayer. Tapping it opens the keyboard *and* moves the panel clear of it, with tappable `OK` / `CANCEL`. |
| `BACK TO GAMES` | leave the running game for the game list (only shown in a game). The handheld's 3-second MENU hold still works too. |

The footer doubles as the selected row's detail line — the relay address, or the
live link state while a match is up. Settings and saves live in the app's private
storage, so an uninstall is the only thing that clears them.

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

Modules are searched for in the app's writable games dir **first**, then the APK's
native-library dir — so a downloaded or side-loaded module shadows the copy baked
into the build, which is what makes updates possible. To side-load one directly:

```bash
./android/tools/build_modules.sh --abi arm64-v8a mygame
adb push build_android/modules/arm64-v8a/libmg_mygame.so \
    /sdcard/Android/data/us.thumby.mote/files/games/
```

## Dock mode: the phone as the Thumby's dock

Plug a **real Thumby Color** into the phone (USB-C to USB-C, or an OTG adapter)
and the app becomes the thing the handheld normally needs a PC and Mote Studio
for. The handheld drives it, from its own screens:

| On the handheld | What the phone does |
|-----------------|---------------------|
| its **multiplayer lobby** → Internet / LAN | performs the relay or LAN room action and splices the byte pipe, so the match runs over **mobile data** |
| its **gallery** (RB in the Mote launcher) | serves the manifest, the 64×64 thumbnails, the descriptions, and the sha256-verified `.mote` to install or update |

**The handheld needs no changes.** It already speaks the MN1 control protocol
over its USB pipe and does not care whether a Studio or a phone answers — that is
the whole point of the auto-proxy design, and it means a phone-docked Thumby and
a PC-docked one land in the same relay room.

Docking is just docking: the manifest matches VID:PID `CAFE:4D01`, so plugging the
handheld in grants USB permission and brings the app up. A green banner appears
under the settings pip showing what the dock is doing (`Thumby docked`,
`online: relaying`, `gallery: installed 128 KB`). The dock idles at no cost when
nothing is attached, so there is no mode to switch into.

Two things to know before relying on it:

- **The phone powers the bus.** In USB-host mode it will not charge, and the
  handheld draws from the phone's battery. A powered OTG hub avoids that.
- **USB host (OTG) support varies** by phone. The feature is declared
  `required="false"`, so the app still installs and works as a console without it.

Under the hood: `MoteUsb.java` claims the CDC data interface and moves bytes with
`bulkTransfer` (no driver, no root) and asserts DTR, which the device's log
channel gates on; `os/android/mote_android_dock.c` is the server, on its own
thread; and the room verbs come from `platform/android/mote_mn1.c`, shared with
the in-app link server so both answer a lobby identically.

### Testing the dock without hardware

The desktop build swaps the cable for a Unix socket, and there is a fake handheld
that speaks the device's half of the protocol:

```bash
python3 android/tools/fake_device.py --sock /tmp/dock.sock gallery &
MOTE_DOCK_SOCK=/tmp/dock.sock ./build_shell/mote_shell
```

`gallery` walks the manifest → thumbnail → description → `.mote` install and
checks each reply (including that the image really starts `MOTE` and matches the
manifest's byte count). `lanhost` / `host` / `quick` / `join` / `browse` exercise
the online path — `lanhost` pairs against a second `mote_shell` running a game,
which is how the whole path was verified.

## The online gallery

**RB** in the launcher opens it. The screen fetches `games.json` straight from the
gallery over HTTPS (no Studio in the loop — that's what the handheld needs a dock
for), reads each game's module entry for this ABI, and shows `GET` / `UPDATE` /
installed against what's already on the phone. Installing downloads the `.so`,
verifies its sha256 against the manifest, drops it in the writable games dir and
hands it to the launcher's catalogue immediately — no relaunch. `A` installs, `RB`
re-fetches, `B` goes back.

Publishing the modules is one command plus the usual manifest step:

```bash
./android/tools/build_modules.sh --abi arm64-v8a --publish   # -> docs/games/android/arm64-v8a/
python3 tools/gen_gallery.py                                 # adds the "android" block
```

`--publish` builds only the ids the manifest lists, strips the symbol tables (the
loader only needs the four ABI symbols), and skips moria. The manifest block is
additive:

```json
"android": { "arm64-v8a": { "file": "games/android/arm64-v8a/libmg_moita.so",
                            "size": 151296, "sha256": "…" } }
```

Consumers that only know about `.mote` ignore it, and a game published without one
shows as "no module yet" rather than breaking the screen. `MOTE_GALLERY_BASE`
overrides the gallery URL for testing.

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
| `MOTE_SHELL_KEYS` | `btn:from-to` frame windows, ~60 frames/s; `panel:from-to` holds the settings panel open so its rows can be scripted too |
| `MOTE_SHELL_SHOT` / `_FRAME` | dump the composited window as a PPM, then quit |
| `MOTE_SHELL_PANEL` | open the settings panel at start (for captures) |
| `MOTE_GALLERY_BASE` | point the gallery at a local manifest (`python3 -m http.server` over a staging dir) |

Two instances on one machine pair over LAN, which is how the multiplayer path is
tested end-to-end.
