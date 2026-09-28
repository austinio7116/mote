# Debugging a game that won't start on device

Written after a full day lost to one bug. The point of this document is that
the next person checks the cheap decisive thing FIRST.

## The rule

**When the runner says "map failed", disassemble `mote_game_register` before
forming any other theory.**

```sh
tools/mote build games/<game> --device
E=games/<game>/build/<Name>.elf
A=$(arm-none-eabi-nm $E | awk '/ T mote_game_register/{print $1}')
K=$(arm-none-eabi-nm $E | awk '/ [tT] k_vtbl/{print $1}')
arm-none-eabi-objdump -d $E | grep -A8 "^$A <mote_game_register>:"
echo "k_vtbl is at 0x$K — the literal the function returns must equal it"
```

The function is five instructions. It stashes the api pointer and returns
`&k_vtbl` out of its literal pool. If that literal is `0x00000000`, the module
returns a NULL vtable, `mote_loader_map` returns 0, and the runner prints
"map failed". That is the whole failure, and it is visible in one command
without touching the device.

## Why this took a day

`mote_loader_map` (os/device/mote_loader.c) returns 0 from four places:
bad magic, an ABI newer than the engine, a null `reg`, or `reg()` returning
null. The diag screen prints the filename, the flash offset and the magic —
which covers the first path and none of the other three. So a NULL vtable
displays as a screen whose every value looks correct.

Everything below was proposed, tested and disproved, each costing a device
round trip:

| theory | how it died |
|---|---|
| module too big | a 290,056 B build ran; a 281,720 B build did not |
| `.bss` over some ceiling | a build with a LARGER bss than the failing one ran |
| ABI mismatch | `mote ping` reported engine ABI 47, module was 47 |
| ATRANS `BASE` 12-bit overflow | `off=008bc000`, 4 KB aligned, well inside range |
| FAT fragmentation | survived a lobby defrag unchanged |
| volume full / truncated push | header, version and ABI all read back correctly |
| 1.4 KB stack frame | real bug, fixed separately, was not this |
| a specific feature commit | masking every suspect feature still failed |

The actual cause was in `sdk/game.ld`, not in any game: `.ARM.exidx` and
`.ARM.extab` were being collected into `.mote`. Those carry `SHF_LINK_ORDER`,
and under `--gc-sections` arm-none-eabi ld 10.3.1 mis-resolves `R_ARM_ABS32`
relocations into `.rodata` placed after them — the section is kept, the map
places it, `nm` gives the symbol a correct address, and the reference is
still written as 0. They are now discarded; a module is `-ffreestanding` C
with no exceptions and nothing that unwinds.

It is LAYOUT-dependent, so it appears and disappears as a game grows.
`grandthumbauto` and `grandthumbauto3` were both silently broken;
`thumbycraft` and `thumbyrogue` happened to link fine.

## Lessons that generalise

1. **Check the artifact, not the theory.** Every failed hypothesis was about
   the environment (flash, RAM, FAT, ABI). The answer was four bytes in the
   binary, readable locally at any time, for free.
2. **"Works on host, fails on device" narrows hard.** Reach for the link and
   the codegen — stack frames (`-fstack-usage`), section layout, relocations —
   before reaching for the filesystem.
3. **A correlation that keeps moving is not a cause.** The "size ceiling"
   moved three times. That was the signal to stop bisecting and look at what
   was actually emitted.
4. **Make the harness prove itself.** Stamp `MOTE_GAME_VERSION` per test build
   so `mote list` confirms which bytes are on the device; a bisect that cannot
   prove what it installed produces confident nonsense.

## Other device-only traps found the same day

- **Stack.** Core0 gets `PICO_STACK_SIZE = 0x1000` — 4 KB, in SCRATCH_Y, with
  core1's stack and then `GAME_RAM` below. The host has megabytes, so a frame
  that kills the device looks perfect in every headless capture. Check with
  `-fstack-usage`; `draw_vehicle_mesh` once reached 1472 bytes from macro
  temporaries alone.
- **Silent truncated push.** FatFs reports a full volume as a short write with
  `FR_OK`. `mote_usb.c` ignored the byte count and answered "OK", leaving a
  truncated image with a perfect header. Fixed, but needs a device reflash.
- **Fragmentation.** A `.mote` executes in place through one ATRANS window, so
  it must be physically contiguous. A fragmented file maps its first cluster
  correctly and unrelated flash after it — and `mote list` still looks right,
  because `mote_read_meta` uses `f_read`, which walks the cluster chain.
