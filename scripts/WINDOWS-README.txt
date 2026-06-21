Mote Studio for Windows
=======================
mote_studio.exe is a single self-contained executable — no DLLs needed (SDL2 and the
MinGW runtime are statically linked).

It must sit IN the Mote repo root and be run from there (it reads studio/assets,
examples/, engine/ and builds game modules at runtime).

  1. Clone the mote repo on Windows.
  2. Copy mote_studio.exe into the repo root.
  3. Double-click it (or run from a terminal in the repo root).

Works out of the box: the UI, the emulator (loads game .dll via LoadLibrary), and
the Device panel (native USB-CDC to a board on CAFE:4D01, shown as a COM port).

Needs on PATH for full functionality (not bundled):
  * MinGW-w64 gcc      — to build/run game modules and bake meshes
  * arm-none-eabi-gcc  — to build .mote device modules (Push)
  * ffmpeg             — to load/convert audio in the Audio tab
