# Mote Multiplayer — a standard, device-driven lobby

**Status:** design for review · **Author:** proposed · **Scope:** engine, Studio, relay, all 2P games

## 1. The problem

Multiplayer works, but using it is a puzzle:

- The **transport is chosen in the Studio** (Host LAN / Join LAN / Bridge USB / Vs Device / Host Room / Browse / Quick) while the **game runs on the device** — so you operate two things at once, in the right order, to connect.
- Every game rolls its **own** link UI + handshake (each `ST_DMLINK` screen, each nonce exchange).
- The Studio DEVICE tab is a wall of buttons that don't map to "how do I play with my mate."
- **Rooms aren't gated by game** — Browse/Quick can pair WolfMote with GTA (connects, then silently desyncs).

## 2. The headline experience (the dream flow)

> Plug the Thumby into *any* PC running Mote Studio. On the **device**, choose
> **Multiplayer → Internet → Quick Match**. It connects. You never touch the Studio.

The device drives everything; the Studio is a silent network proxy. Same for
Host/Browse/Enter-Code. Two cabled Thumbys (USB) need no PC at all.

## 3. Architecture

One new idea: **the game asks the engine for a connected opponent, and the engine
runs a standard lobby to get one.** The game never sees transports or rooms — it
calls one API and gets back a byte pipe (exactly today's `link_send/recv`).

```
  GAME            "give me an opponent"           mote->net_lobby(&cfg)
   │                                                     │  returns when connected
  ENGINE LOBBY   transport + host/browse/join UI         │  (or cancelled)
   │        ┌───────────────┬───────────────────────────┤
  TRANSPORT │ USB cable     │ LAN / Internet             │
   │        │ (dual-role)   │ (device → Studio proxy)    │
  DEVICE────┴──USB──────────┴──USB──▶ STUDIO ──net──▶ RELAY / LAN peer
```

- **USB cable:** two devices, existing dual-role link (`mote_link.c`). No PC.
- **LAN / Internet:** the device has no network, so the **Studio is its gateway.**
  The device sends *control requests* over USB ("host a public internet room",
  "list rooms", "join ABCD"); the Studio performs them on the network and streams
  back the room code / list, then relays game bytes. The device is in charge; the
  Studio just does what it's told and needs **no manual UI** for this.
- **Emulator/preview** (game running inside the Studio, no device): the same
  engine lobby runs in-process and calls `link_net` directly.

## 4. Game-facing API (engine, ABI v44)

```c
typedef struct {
    const char *game_name;     // "WolfMote" — identity for room gating
    uint16_t    proto_version; // bump when your wire format changes (v1↔v2 won't pair)
    uint8_t     transports;    // MOTE_NET_USB | MOTE_NET_LAN | MOTE_NET_INTERNET (bitmask)
} MoteNetCfg;

// Blocking-style: runs the lobby (engine draws it), returns when connected or cancelled.
// On success the game then uses the existing link_send/recv. Returns:
//   MOTE_NET_CONNECTED (+ *out_is_host set) · MOTE_NET_CANCELLED
int mote->net_lobby(const MoteNetCfg *cfg, int *out_is_host);
```

- The engine derives a **game_id** = `fnv32(game_name) ^ (proto_version<<...)` and uses
  it to gate rooms — the game never handles it.
- `out_is_host` is the **resolved authority** (see §8) so games stop re-implementing
  the nonce handshake. A game can ignore it if it has its own scheme.
- After `net_lobby` returns CONNECTED, `link_send/recv/status` behave exactly as now.
- Games keep full control of their *gameplay* protocol (moves, state packets, etc.) —
  the engine only standardizes **getting connected**.

A game's multiplayer entry becomes, roughly:

```c
if (menu_pick == MP_MULTIPLAYER) {
    int host;
    if (mote->net_lobby(&(MoteNetCfg){"WolfMote", 1, MOTE_NET_ALL}, &host) == MOTE_NET_CONNECTED)
        start_deathmatch(host);   // host = who seeds the map, etc.
}
```

That replaces each game's bespoke `ST_DMLINK` screen + hello/nonce code.

## 5. The device↔Studio control protocol

Today the device↔Studio USB link is a **raw byte pipe** (the game's bytes). We add a
**lobby phase** in front of it. The two phases are temporally separate (control while
in the lobby; raw game bytes after pairing), so no multiplexing is needed.

**Lobby phase** (newline-framed text, device ⇄ Studio):

| Dir | Message | Meaning |
|---|---|---|
| dev→studio | `NET HELLO <gameid> <gamever>` | entering the lobby; Studio enters proxy mode |
| dev→studio | `NET QUICK` | quick-match this game |
| dev→studio | `NET HOST <PUB\|PRIV> <label>` | open a room |
| dev→studio | `NET JOIN <code>` | join a code |
| dev→studio | `NET LIST` | request open rooms |
| dev→studio | `NET CANCEL` | leave the lobby |
| studio→dev | `NET CODE <code>` | your room's code (share it) |
| studio→dev | `NET ROOMS <code> <label>` × N, `NET END` | browse results |
| studio→dev | `NET GO H` / `NET GO G` | paired → **switch to raw game bytes now** |
| studio→dev | `NET ERR <msg>` | failed (no relay, room gone, etc.) |

On `NET GO`, both the device's engine and the Studio's bridge flip from control to
**raw relay** — the game's `link_recv` now yields the peer's game bytes. The Studio
uses **its own configured relay address** (the field we added), so the device never
needs to know the relay IP.

**Auto-proxy:** when the Studio sees `NET HELLO` from a docked device, it silently
becomes that device's network proxy — no Bridge button, no mode-picking. (The USB-logs
channel already knows how to yield the port; this is the same idea.)

## 6. Relay: game-gated rooms

Extend the `MOTE1` protocol with a game-id so Browse/Quick/Join only pair compatible
peers:

| Now | Becomes |
|---|---|
| `MOTE1 HOST <code> <vis> <label>` | `MOTE1 HOST <gameid> <code> <vis> <label>` |
| `MOTE1 JOIN <code>` | `MOTE1 JOIN <gameid> <code>` (relay rejects a game-id mismatch) |
| `MOTE1 LIST` | `MOTE1 LIST <gameid>` (returns only that game's rooms) |
| `MOTE1 QUICK [label]` | `MOTE1 QUICK <gameid> [label]` (matches only that game) |

Relay keys rooms by `(gameid, code)`. `gameid` folds in the game's `proto_version`, so
an old and a new build of the same game won't cross-pair either. Small, self-contained
change — and it's a **prerequisite** for a device-driven Browse/Quick that can't
show/pick the wrong game.

## 7. On-device lobby screens (the UX)

Rendered by the engine in one consistent look, driven by the d-pad + A/B:

1. **Transport** — `USB CABLE` · `LAN` · `INTERNET`
   (LAN/Internet greyed with "connect to a PC" if no Studio proxy is detected;
   the bitmask in `MoteNetCfg` also hides transports a game doesn't want.)
2. **Action** (LAN/Internet) — `QUICK MATCH` · `HOST ROOM` · `BROWSE` · `ENTER CODE`
   - **Host Room** → shows the **CODE big** to share + "waiting for a player…" + B cancel.
   - **Browse** → scrollable list of this game's open rooms → A to join.
   - **Enter Code** → 4-slot d-pad character picker (A-Z0-9) → join.
   - **Quick Match** → "finding a game…" then connects.
   - **USB Cable** → "link a cable to the other Thumby" + auto dual-role discovery.
3. **Connected!** → hands control to the game.

Everything a player needs is on the device. The only PC involvement (for LAN/Internet)
is "have the Studio running" — no clicks in it.

## 8. Standard authority (nonce) handshake

The lobby resolves who's "host" once, centrally, so every game stops re-implementing
it (and stops mis-using `link_is_host`, which is 0 on both ends over the bridge):

- On connect the engine exchanges a random 16-bit nonce (higher = host), retrying on a
  tie, and returns the result as `out_is_host`.
- Games that want it use it (map seed owner / who breaks / server). Games can still run
  their own scheme on top if they prefer.

## 9. Backward compatibility & migration

- The raw `link_*` API stays — nothing breaks.
- `net_lobby` is additive. Games opt in by calling it and deleting their `ST_DMLINK`
  screen + hello/nonce code (net simplification per game).
- Migrate the 7 games one at a time; each keeps its own *gameplay* protocol after connect.
- The Studio keeps a small **Advanced** section (the current manual buttons) for
  power users / debugging, but the default path is the device-driven lobby.

## 10. ABI / firmware impact

- New engine call (`net_lobby`) + `MoteNetCfg` → **ABI v43 → v44**, needs a firmware
  reflash (the lobby UI + control protocol live in the OS/engine).
- Studio update (auto-proxy + control protocol) — ships in the next Studio build.
- Relay update (game-gating) — redeploy `mote_relay.py`.
- Games rebuilt against v44 to use the lobby (older games still run via raw link).

## 11. Suggested build order

1. **Game-gate the relay** (`gameid` in MOTE1) + thread `gameid` through the Studio's
   existing Host/Browse/Quick. Self-contained safety fix; ships immediately.
2. **Engine lobby API + on-device UI** for **USB cable** first (no Studio needed) —
   proves the `net_lobby` shape end to end on hardware.
3. **Device→Studio control protocol + auto-proxy** for LAN/Internet.
4. **Migrate games** to `net_lobby` (start with WolfMote or deepthumb).
5. Emulator lobby polish + retire the manual Studio buttons to Advanced.

## 12. Decisions (locked)

- **On-device code entry: YES — a d-pad character picker** (4 slots, A-Z0-9).
- **Engine owns the nonce/authority handshake** — `net_lobby` resolves it and returns
  `out_is_host`; games stop reimplementing it and never touch `link_is_host`.
- **Auto-proxy: the Studio silently proxies** a docked device that enters its lobby —
  no gateway toggle, no Bridge button in the default path.
- **`net_lobby` is blocking** (a self-contained modal loop, like the engine pause menu).
  Simpler and more robust; a non-blocking variant can come later if a game wants its
  title animating behind the lobby.

Still to revisit later (not blockers): relay abuse/scale once rooms are discoverable
(basic rate limits already exist); a possible non-blocking lobby variant.
