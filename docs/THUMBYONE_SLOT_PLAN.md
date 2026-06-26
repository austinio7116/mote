# Mote as a ThumbyOne slot — plan

Run the whole **Mote** console-OS as a single **ThumbyOne** slot, replacing the three
standalone game slots (**ThumbyCraft**, **ThumbyRogue**, **Indemnity/Elite**) with one
**Mote** tile that hosts the entire Mote game library. Goal for the default build: **−3
game slots, +1 Mote slot**, more games behind less flash.

## Why this is a natural fit

ThumbyOne boots a slot by pointing QMI **ATRANS slot 0** at the slot's flash offset so a
fixed-virtual-address image runs in place (XIP). Mote loads a `.mote` game by pointing
**ATRANS slot 2** at the module's flash offset so a fixed-virtual-address module runs in
place. **Same technique, nested one level.** Mote already vendors ThumbyOne's
`xip_fast_setup` (`os/device/mote_xip.c`), and ThumbyCraft's standalone build already had a
`THUMBYONE_SLOT_MODE`. So Mote is most of the way to being a well-behaved slot.

Today ThumbyOne spends a **whole slot per game** — each a standalone image carrying its own
drivers + engine (Craft ~512 KB, Rogue ~512 KB, Elite ~256 KB). One Mote slot hosts Craft,
Indemnity, ThumbyCue, papermote and every example as small `.mote` modules over **one**
resident engine — more content, less flash, plus the Mote dev/IDE pipeline behind one tile.

## Architecture

```
ThumbyOne lobby (slot 0)
   │  A on the "Mote" tile → watchdog handoff + rom_reboot → rom_chain_image(MOTE slot)
   ▼
Mote slot (ATRANS[0] = Mote partition)         ← the Mote OS + engine, linked at 0x10000000
   │  Mote launcher (its own lobby)             ← USB composite MSC+CDC live here
   │     ├─ pick a .mote from /.mote/ on the shared FAT
   │     │     → point a spare ATRANS slot at the file's flash clusters → run in place
   │     └─ "Quit to ThumbyOne" → handoff_request_lobby() + reboot
   ▼
Mote game running (USB off / telemetry only — the game owns flash, like any TO slot)
```

## The game library lives on the shared FAT (decided)

`.mote` modules live in **`/.mote/`** on ThumbyOne's shared FAT16 volume. This makes one
storage location reachable three ways:

1. **ThumbyOne lobby USB-MSC** — drag a `.mote` onto the drive (existing TO workflow).
2. **Mote-launcher USB-MSC** — same, without leaving the Mote slot (composite device, below).
3. **Mote-launcher USB-CDC** — `mote push` from the Studio IDE writes straight to `/.mote/`.

### Executing a `.mote` in place from the FAT — and the fragmentation warning

To XIP-execute a module, Mote points an ATRANS slot at its **physical flash offset**, which
must be **4 KB-aligned and contiguous**. FAT16 clusters are 4 KB-aligned, so a
**non-fragmented** `.mote` (e.g. ThumbyCraft ≈ 370 KB ≈ 92 contiguous clusters) can be mapped
and run straight from its cluster run — zero-copy, full XIP speed.

> ⚠️ **Fragmentation warning.** A `.mote` whose FAT clusters are **not contiguous** cannot be
> ATRANS-mapped as one window. The loader must detect this (walk the FAT chain; check every
> cluster is consecutive) and handle it:
> - **Refuse + warn:** show "Defragment needed — run Defrag in the ThumbyOne lobby" (TO
>   already has a defrag-FAT menu item) and skip the game.
> - **Fallback copy:** copy the module into a small **contiguous scratch flash region** in
>   the Mote partition, then map *that*. Slower load, always works. (Bounded to one module's
>   size, so scratch ≈ largest `.mote`.)
> v1 ships the **refuse + warn** path (cheap, honest); v2 adds the scratch fallback so a
> fragmented library still runs. Writing via CDC `mote push` should **allocate contiguously**
> (pre-size the file / pick a free contiguous run) to avoid creating fragmented modules in
> the first place.

## USB: composite MSC + CDC, live only in the Mote launcher

While the Mote launcher is idle (its own "lobby"), expose a **TinyUSB composite MSC + CDC**
device — drag/drop *and* the IDE link at once (the CircuitPython model: a drive + a serial
port simultaneously). When a **game runs**, USB drops to off / telemetry-only — the running
game owns flash, exactly the rule ThumbyOne already applies to slots.

### Two-writers-one-filesystem coordination (the real hazard)

Both the host (MSC) and the device (CDC `mote push`, the launcher) can write the same FAT;
concurrent writes corrupt it. Protocol (one writer at a time):

- **CDC push:** before writing `/.mote/<name>.mote` via on-device FatFs, the device soft-
  ejects MSC ("medium not present"), writes (contiguously — see above), then re-presents so
  the host re-reads. (CircuitPython's `storage.remount` does exactly this.)
- **MSC write:** the device treats the FAT as host-owned while MSC is mounted and
  **re-mounts / invalidates its FatFs cache** after the host finishes.
- **Alternative (simpler v1):** CDC push writes to a staging area; the launcher imports it
  into `/.mote/` only when the host is quiet — no concurrent FAT access at all.

## ATRANS budget — 4 slots, plan the juggling

ThumbyOne boot: `[0]`=running slot, `[1..3]`=identity map of physical 4–16 MB (FAT reach).
The Mote slot **owns the device while running**, so it may reprogram `[1]` or `[3]` to map a
game module, **then restore the identity map before handing back to the lobby**. Every
reprogram must be followed by the QMI **fast-XIP-restore** dance (reset-first sequence — the
one that cost a day on NES; well-documented in `os/device/mote_xip.c` and
`feedback_rp2350_xip_reset_first`). Reads of the FAT (to locate a module's clusters) happen
*before* repurposing its ATRANS slot.

## Shared services

The Mote slot reads `/.volume` + `/.brightness` (and the 4 KB settings mirror) and uses
`thumbyone_led` / `thumbyone_battery` / `thumbyone_rtc`, like every other slot. ThumbyCraft's
standalone `THUMBYONE_SLOT_MODE` already prototyped this bridging — reuse that pattern.

## Memory map

One slot runs at a time with the full ~512 KB SRAM; Mote is built for exactly this (OS region
378 KB + 134 KB module RAM). The watchdog `scratch[0..1]` handoff registers must be preserved
(Mote already avoids the SDK-owned `scratch[4..7]`). No co-residency concerns.

## Build integration (ThumbyOne side)

- `common/slot_layout.h`: drop `THUMBYONE_{CRAFT,ROGUE,ELITE}_SIZE` + enum entries; add
  `THUMBYONE_MOTE_SIZE` + `THUMBYONE_SLOT_MOTE`. Offsets recompute automatically.
- `CMakeLists.txt`: remove the three `add_subdirectory` blocks + `WITH_*` flags; add
  `THUMBYONE_WITH_MOTE` and a Mote slot subproject hook (accepts `THUMBYONE_SLOT_MODE` +
  `THUMBYONE_SLOT_LD`, emits ELF only).
- `tools/gen_pt.py`: MOTE replaces the three in the partition order.
- `lobby/`: remove three tiles, add a **Mote** tile + 48×48 icon.

## Build integration (Mote side)

- A **slot linker variant** of the OS image: link at `0x10000000` (ATRANS[0] relocates),
  preserve the watchdog handoff region, build under `THUMBYONE_SLOT_MODE`.
- **Boot:** consume the watchdog handoff in `main()` before peripheral init; add a "Quit to
  ThumbyOne" exit in the Mote launcher (`handoff_request_lobby()` + reboot) alongside the
  existing per-game "return to Mote launcher".
- **Loader:** add a FAT-backed path — enumerate `/.mote/`, validate cluster contiguity, map
  via a spare ATRANS slot (or scratch-copy fallback), then the existing `.mote` load runs
  unchanged. The store currently in Mote-flash becomes the FAT.
- **USB:** composite MSC+CDC descriptor + the write-coordination protocol above (launcher
  only).
- **Settings:** bridge brightness/volume/battery/LED to the `thumbyone_*` shared modules.

## Flash budget

Mote OS ≈ 300 KB. A Mote slot partition of **~512 KB–1 MB** (OS + loader + a small scratch
region for the defrag fallback) is ample; the *games* live in the shared FAT, not the slot.
Replacing the three game slots (Craft 512 + Rogue 512 + Elite 256 = **1.28 MB**) with one
**~768 KB** Mote slot *frees* ~512 KB — give it back to the FAT.

## Phased plan

- **Phase 0 — Slot boots.** Mote slot linker variant + handoff consume/return; lobby tile;
  `slot_layout.h`/CMake/PT wiring. Boots from lobby into the Mote launcher and back. Games
  bundled in-partition (no FAT yet) to prove the path.
- **Phase 1 — FAT-backed library.** Loader enumerates `/.mote/`, checks contiguity, maps from
  FAT clusters (refuse+warn on fragmentation). Drag-drop in the ThumbyOne lobby → appears in
  the Mote launcher.
- **Phase 2 — Composite USB in the Mote launcher.** MSC+CDC live; `mote push`/`list`/`logs`
  from the IDE write `/.mote/` with the one-writer coordination; MSC drag-drop without leaving
  the slot.
- **Phase 3 — Robustness.** Scratch-copy fallback for fragmented modules; contiguous-allocate
  on CDC push; shared-settings bridge; polish the two-level navigation.

## Risks

- **FAT-cluster XIP + fragmentation** (Phase 1) — the novel part; mitigated by refuse+warn
  then scratch-copy.
- **MSC + CDC + on-device FatFs write races** (Phase 2) — solved class (CircuitPython), but
  fiddly; the staging-area variant is the safe fallback.
- **ATRANS restore correctness** — every reprogram needs the reset-first fast-XIP dance.
- **Can't fully verify off-device** — needs combined-firmware flash + device iteration.
