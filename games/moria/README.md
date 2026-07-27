# Moria for the Thumby Color

A port of **Umoria 5.6.0** — the classic 1980s dungeon-crawling roguelike — to the
[Mote](https://github.com/austinio7116/mote) engine, running on the Thumby Color
handheld (RP2350, 128×128 pixels, nine buttons).

The entire Umoria game core is present and unmodified in behaviour: the same
dungeon generation, monsters, items, spells, stores and combat. Only the display
and the controls are new.

---

## Original authors and copyright

Moria was created by **Robert Alan Koeneke** in 1983. **James E. Wilson** produced
the portable UNIX version (Umoria), and **David J. Grabiner** maintained it and
released Umoria 5.6.0.

    Copyright (C) 1989-2008 James E. Wilson, Robert A. Koeneke, David J. Grabiner

Upstream source: <https://github.com/dungeons-of-moria/umoria> (tag `v5.6.0`)

This port is **not** affiliated with or endorsed by the original authors.

## Licence

Umoria is free software, licensed under the **GNU General Public License, version 3
or (at your option) any later version**. This port, as a whole — including all the
new Mote-specific files listed below — is released under the same terms.

The full licence text is in [`COPYING`](COPYING) alongside this file.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

### Corresponding Source (GPLv3 §6)

The complete corresponding source for any distributed binary (`Moria.mote`) is the
`games/moria/` directory of the Mote repository, publicly available at:

<https://github.com/austinio7116/mote>

### A note on two upstream files

Upstream Umoria 5.6.0 relicensed the codebase to GPLv3, but **two files —
`src/desc.c` and `src/files.c` — still carry the older pre-GPL Moria notice**
("may be copied and distributed for educational, research, and not for profit
purposes"). That inconsistency is inherited verbatim from upstream v5.6.0, not
introduced here; their headers are preserved exactly as received.

The upstream project distributes the work as a whole under GPLv3 (its repository
root carries the GPLv3 as `LICENSE`), and every other file in the tree states
GPLv3 explicitly. The C source line was never revised again — Umoria 5.7 is a
C++ rewrite — so there is no later C release from which to take corrected
headers. Anyone redistributing this port commercially should be aware of the
discrepancy and may wish to seek the copyright holders' confirmation.

### Version note

`src/constant.h` reports the game version as **5.5.2**. This is upstream's own
stale constant in the v5.6.0 tree — the GPLv3 headers, the `1989-2008` copyright
range and upstream's build paths all identify this as 5.6.0.

---

## What was changed for this port (GPLv3 §5(a))

All modifications were made in **2026** by austinio7116, and are marked in the
source with `#ifdef MOTE` / `MOTE_DEVICE` guards. Each modified upstream file
carries a notice under its original header.

**New files** (Mote front-end; no upstream equivalent):

| File | Purpose |
|---|---|
| `game.c` | Mote game module: the controller-native UI, framebuffer renderer, button→keystroke mapping |
| `curses.h`, `mote_term.c` | A small re-implementation of the ~30 curses primitives Umoria uses, backed by an in-memory 80×24 virtual terminal |
| `mote_fiber.c`, `mote_fiber.h` | Runs Umoria's blocking `main()` on its own stack (host: `ucontext`; device: a hand-written Cortex-M33 context switch) so it can live under Mote's per-frame `update()` |
| `mote_glue.c` | Engine binding and device libc stubs |
| `moria_ui.h` | The input-context flag (`moria_ui_mode`) the front-end reads to choose a native UI |
| `moria_color.h` | A 16-colour palette (Umoria 5.6 is monochrome) |
| `icon.h` | The baked launcher icon |

**Modified upstream files** — the changes are confined to platform support and
presentation; no game rule, formula or table was altered:

- `config.h`, `externs.h` — select a self-contained `MOTE` platform (modelled on
  the historical Macintosh port) so the UNIX termios/ioctl/signal code compiles out.
- `io.c`, `signals.c` — input/output routed through the virtual terminal; UNIX
  signal and terminal handling disabled.
- `save.c` — saves read and written through Mote's key-value blob store instead
  of `stdio`, since the device has no filesystem.
- `create.c`, `misc1.c`, `misc3.c`, `moria1.c`, `dungeon.c`, `store2.c` — set the
  input-context flag before blocking for input, and expose the game's own lists
  (items, spells, shop stock, character sheet) so the front-end can draw them
  natively instead of as 80-column text.
- `death.c` — native game-over and game-saved screens.
- `main.c`, `files.c` — entry point and file-access adjustments for a device with
  no filesystem.
- `variable.c`, `treasure.c`, `spells.c`, `recall.c` — colour plumbing and
  const-ing large tables into flash to fit the RAM budget.

Additionally, four `sprintf` calls in `store2.c` that could overflow a fixed
buffer with a long item name were changed to `snprintf`. This is a latent
upstream bug, harmless on a desktop but a stack-smash risk on the device's
24 KB fiber stack.

---

## Playing it

The Thumby Color has nine buttons, and Umoria has a 39-verb command set, so the
port does not emulate a keyboard. Instead the game tells the front-end what kind
of input it is waiting for, and the front-end shows a matching native UI.

| Situation | Controls |
|---|---|
| On the map | **D-pad** walk (8-way) · **A**/**MENU** command menu · **B** take the stairs you are standing on · **LB** character sheet · **RB** inventory |
| Command menu | **D-pad** move · **LB**/**RB** switch category · **A** choose · **B** back |
| Choosing an item | **D-pad** move the cursor · **A** pick · **B** back · **LB** swap inventory/equipment |
| A direction is wanted | **D-pad** aim · **A** here/self · **B** cancel |
| A yes/no question | **A** yes · **B** no |
| A list (race, class, spell, shop stock) | **D-pad** + **A** · hold **RB** to peek behind the panel |
| Typing a name | On-screen keyboard: **A** type · **LB** backspace · **RB**/**MENU** accept · **B** cancel |
| `-more-` | Any button |

**Info → Controls** in the command menu shows this list in-game, and
**Info → Licence** shows the GPL notice.
