# Gallery & Update Manager — design

Status: **planned** (implementation starts next release). Reference for the in-Studio and
on-device gallery viewer that lets a user browse the online game gallery, see which
installed games have updates, and install/update — with the device docked to Studio over
USB.

---

## 1. Goal

One flow, two front-ends, both requiring the device docked to Studio over USB (the handheld
has no independent internet):

1. **In Mote Studio** — browse the gallery with thumbnails + versions, diff against the
   connected device, one-click install/update.
2. **On the device (Mote slot)** — the same browse-and-update from the handheld, with Studio
   acting as its network bridge.

Non-goals: no accounts, no ratings, no uploads-from-device, no over-the-air (Wi-Fi) updates
(the hardware can't). The device gallery is **only** usable while docked — this is an
inherent hardware limit, stated plainly in the UI, not a bug.

---

## 2. Architecture & data flow

**Studio fetches directly from GitHub Pages over HTTPS. The relay is NOT involved.** Pages
is a free CDN built for this; routing downloads through our small relay VM would be a
bandwidth/cost/availability mistake. The gallery must keep working even when the relay is
down.

Studio has TCP sockets but no TLS. We don't add a TLS stack — we **shell out to `curl`**,
exactly as the Windows bundle already ships and invokes `ffmpeg.exe`:

- Windows: bundle `curl.exe` (Windows also ships one in-box since 1803).
- Linux/Mac: system `curl` / `libcurl`.

Fetch-URL-to-file + verify sha256 is ~a dozen lines, no crypto to maintain.

```
  Studio viewer:   Studio ──HTTPS──> GitHub Pages         (browse + download)
                   Studio ──USB────> Device               (install via existing PUT)

  Device viewer:   Device ──USB────> Studio ──HTTPS──> GitHub Pages
                   (device is a pure client; Studio is the bridge; relay untouched)
```

---

## 3. Prerequisites (none exist today)

### 3.1 Game version numbers

Modules carry name/author/icon/ABI but **no game version**. Add one.

- **Declaration** in `game.c`, next to `MOTE_GAME_META`:
  ```c
  MOTE_GAME_VERSION("1.2.0");     // semver string; optional
  ```
  Implemented as a **weak** `const char *mote_game_version` defaulting to `"0"` — so the ~40
  existing games keep compiling untouched (mirrors the existing weak `mote_game_icon_data`).
- **Readable from the compiled `.mote`, not just source.** The device must report the
  *installed* version without the source. Add `version_vaddr` to `MoteModuleHeader`
  (analogous to `icon_vaddr`); the loader/catalog reads the string straight from flash.
  This is a module-format append → **bump `MOTE_ABI_VERSION` v45 → v46** and rebuild all
  games (the gallery relaunch rebuilds them anyway).
- Tools still text-scan `MOTE_GAME_META`/`MOTE_GAME_VERSION` from `game.c` at build time for
  the manifest and never need to run the game (same mechanism `game_name()` uses today).
- **Semantics:** compared as dotted-integer semver (`1.10.0 > 1.9.0`). The manifest is the
  source of truth for "what's current"; the in-module version is "what's installed." A
  versionless (`"0"`) installed game always shows "update available" once the gallery copy
  carries a real version.

### 3.2 Machine-readable manifest — `games.json`

The gallery is hand-maintained static HTML with no data feed. Add a **generator** (part of
the docs build) that scans `docs/games/*.mote` and emits `docs/games.json` + thumbnail
assets. Generated, never hand-edited — or versions/hashes drift from the files.

```jsonc
{
  "schema": 1,
  "generated": "2026-07-06T09:00:00Z",
  "games": [
    {
      "id": "grandthumbauto",              // stable key == .mote basename (no ext)
      "name": "Grand Thumb Auto",
      "author": "…",
      "version": "1.1.0",
      "abi": 46,                            // min engine ABI required
      "size": 395072,
      "sha256": "…",
      "file": "games/GrandThumbAuto.mote",  // relative to Pages root
      "icon": "games/icons/grandthumbauto.rgb565",  // 60x60, for device streaming
      "thumb": "games/thumbs/grandthumbauto.png",   // larger, for Studio UI
      "desc": "Open-city driving…",          // from arcade_description.txt
      "multiplayer": true,
      "tags": ["driving", "2p"]
    }
  ]
}
```

The generator extracts the 60×60 icon from each `.mote` (the device already has icons for
*installed* games, so only *new* games' icons need streaming) and copies a larger screenshot
for Studio.

### 3.3 Thumbnails

- Studio UI: use the larger `thumb` PNG from the manifest.
- Device UI: reuse the installed game's own 60×60 icon for free; only *new* games' icons are
  streamed from Studio (7.2 KB RGB565 each) — lazy-load on scroll, cache, never preload the
  whole gallery.

---

## 4. Install: in-place, journalled, hash-verified (NO temp copy)

Storage is tight — doubling a game's flash footprint during install is a non-starter, so
**no write-to-temp-and-swap.** Update in place, made safe by a journal:

1. Before overwriting, write one small catalog entry: `{id, replacing, expect_size,
   expect_sha256}` and mark the slot **INSTALLING**.
2. Write the new `.mote` in place (the old version is gone — accepted cost of the storage
   constraint; the source is always reachable while docked).
3. On completion, verify sha256. Match → clear the flag, slot LIVE. Mismatch/interrupted →
   the slot stays **INCOMPLETE**.

**Load-bearing rule:** the launcher/loader MUST refuse to run a slot flagged INSTALLING or
INCOMPLETE, and must offer "re-download" instead. That is what turns an interrupted in-place
write from a corruption into a recoverable re-fetch. The device never bricks; only that one
game is temporarily unavailable until re-fetched. Costs a few bytes of catalog metadata,
never a second copy.

This directly addresses the earlier DeepThumb corruption class: an aborted transfer must
leave a *flagged* slot, and the device-side PUT parser must not wedge on an aborted transfer
(reset cleanly on a new command / timeout).

---

## 5. USB protocol extension (Phase 3, device viewer)

Today the CDC link carries the text CLI (`PUT`/`LIST`/`PING`, protocol N) plus logs and the
multiplayer link. The device gallery viewer lives in the launcher (no in-game multiplayer
session running), but still needs a clean, framed, device-initiated request/response added:

- `PING` → bump the reported protocol version so Studio knows the device speaks "gallery".
- Device → Studio requests (framed, length-prefixed):
  - `GALLERY` — "send me the manifest" → Studio fetches `games.json`, streams it back.
  - `THUMB <id>` — "send this icon" → Studio streams the 60×60 RGB565.
  - `FETCH <id> <version>` — "install this" → Studio downloads + hash-checks the `.mote`,
    then drives the existing in-place install (§4) via `PUT`.
- Studio must run a **gallery-service mode** that listens for these device-initiated requests
  (today Studio only *initiates* pushes). Clear "who's talking" arbitration on the shared
  channel; the launcher context guarantees no multiplayer contention.

---

## 6. Studio gallery viewer (Phase 2)

- New tab/panel. Fetch `games.json` via bundled `curl` (works with no device docked — browse
  is always available; install/update require a device).
- Read the docked device catalog (`LIST`) + each installed game's version (from §3.1) → diff
  into **Installed / Update available / Not installed**.
- Grid with `thumb`, name, version, "Update available" badge, desc.
- Actions: Install / Update / Remove. Download → verify sha256 → in-place install via `PUT`.
- **ABI gate:** if a game's `abi` > the device's engine ABI, show "needs firmware ≥ X —
  update firmware first" and refuse to install (never push a game the engine will reject).
- Handle device disconnect mid-download gracefully (the download to PC is fine; only the push
  leg needs the device — resumable via the INCOMPLETE flag).

---

## 7. On-device viewer (Phase 3)

- New launcher screen: a scrollable grid using installed icons + streamed new-game icons.
- Per row: name, installed version vs gallery version, an "update" affordance.
- Only functional while docked — if Studio isn't answering the gallery protocol, show
  "Dock in Mote Studio to browse the gallery." (Confirmed acceptable: hardware limit.)
- Select → `FETCH` → Studio downloads + installs in place (§4) → the row refreshes.

---

## 8. Phasing

- **Phase 1 — foundations (low risk, ships alone):** version symbol + weak-default compat
  shim; `version_vaddr` header field + ABI v46; manifest generator + `games.json` +
  icon/thumb assets on Pages. No UI — testable with `curl` and the CLI.
- **Phase 2 — Studio viewer:** bundled `curl`, fetch + diff + install/update to the docked
  device, ABI gate. Browsing works with no device.
- **Phase 3 — on-device viewer:** Studio gallery-service mode, CDC protocol extension, device
  launcher screen, icon streaming + cache.

---

## 9. Risks & mitigations

| # | Risk | Mitigation |
|---|------|------------|
| 1 | **Interrupted in-place install** (storage forbids temp copy) — old version already gone | Journal + INSTALLING/INCOMPLETE flag; loader refuses to run a flagged slot; re-fetch from Studio. Harden the device PUT parser against aborted transfers (clean reset on new cmd/timeout). |
| 2 | **ABI mismatch** — gallery game needs newer firmware | Manifest records per-game `abi`; viewer blocks install + prompts firmware update. |
| 3 | **Author-declared versions** — no guarantee higher == newer | Manifest is the authority for "current"; in-module version only for "installed". |
| 4 | **USB bandwidth for thumbnails** | Reuse installed icons; lazy-load + cache new-game icons; never preload the gallery. |
| 5 | **Shared CDC channel arbitration** | Framed device-initiated protocol; launcher context (no multiplayer running); `PING` protocol bump. |
| 6 | **Flash full** | Free-space check before download; graceful "not enough room". |
| 7 | **Manifest/file drift** | `games.json` is generated from the actual `.mote` files; downloads hash-verified. |
| 8 | **`curl` availability** | Bundle `curl.exe` on Windows (like `ffmpeg.exe`); system curl elsewhere; clear error if missing. |
| 9 | **ABI v46 rebuild** — every game must rebuild to gain a version | Acceptable: the gallery relaunch rebuilds all games. Versionless installed games show as `"0"` → "update available". |

---

## 10. Open decisions (resolved)

- **Fetch path:** Studio direct-HTTPS via bundled curl. Relay NOT involved. ✅
- **On-device only while docked:** accepted hardware limit. ✅
- **Install:** in-place + journal + hash-verify + re-fetch. No temp copy. ✅

## 11. Still to decide before Phase 1

- Version-compare edge cases: pre-release tags? downgrades (allow re-install of an older
  version)? Proposal: numeric dotted compare only; allow explicit downgrade/reinstall.
- Manifest hosting: `games.json` at the Pages root vs under `games/`. Proposal: Pages root
  (`/mote/games.json`) so it's a stable well-known URL.
- Whether Phase 3 (on-device) ships with Phase 2 or a release later.
