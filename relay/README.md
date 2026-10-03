# Mote link relay

A tiny **byte pipe** that lets two Mote Studios link over the internet, extending
the LAN link (`studio/link_net.c`) to:

```
device ──USB── Studio ──┐                         ┌── Studio ──USB── device
                        └──▶  mote_relay (VPS)  ◀──┘
```

Both Studios make **outbound** TCP connections to the relay (so no home-router
port-forwarding is needed — outbound traverses NAT), join the same short **room
code**, and the relay splices their sockets. It forwards raw bytes and never
parses the game protocol — the game's `0xA5` framing stays end-to-end. On the
device side nothing changes; the RP2350 only ever speaks USB to its local Studio.

Single stdlib Python file — **no pip, no build**. Python 3.8+.

## Run locally

```
python3 mote_relay.py --port 42450
```

Flags: `--addr` (default `0.0.0.0`), `--port` (42450), `--join-timeout` (120s a
lone client waits for a partner), `--idle` (45s of silence drops a paired room),
`--max-conns` (2000).

## Deploy (Oracle Cloud free ARM VM, or any VPS)

Python 3 is preinstalled on the Oracle/Ubuntu images, so it's just copy + enable:

```
scp mote_relay.py mote-relay.service  ubuntu@<vps-ip>:
ssh ubuntu@<vps-ip>
sudo mkdir -p /opt/mote-relay && sudo cp mote_relay.py /opt/mote-relay/
sudo cp mote-relay.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now mote-relay
journalctl -u mote-relay -f          # watch it
```

### Open the port in BOTH places (the classic gotcha)

The relay listens on **TCP 42450**. On a cloud box you must allow it twice:

1. **Cloud firewall** — add an ingress rule for TCP 42450 from `0.0.0.0/0`:
   - *Oracle:* VCN ▸ your subnet ▸ Security List ▸ Add Ingress Rule (source
     `0.0.0.0/0`, TCP, dest port 42450). (Or a Network Security Group.)
   - *Hetzner/others:* the panel's firewall, same rule.
2. **OS firewall** — the OCI images ship iptables that block everything but SSH:
   ```
   sudo iptables -I INPUT 6 -p tcp --dport 42450 -j ACCEPT
   sudo netfilter-persistent save      # Ubuntu; persists the rule across reboot
   ```
   (Oracle Linux: `sudo firewall-cmd --permanent --add-port=42450/tcp && sudo firewall-cmd --reload`.)

If it "works locally on the box but nothing connects from outside", it's almost
always step 2.

### Smoke test from your laptop

```
python3 - <<'EOF'
import socket
a=socket.create_connection(("<vps-ip>",42450)); a.sendall(b"MOTE1 TEST\n")
b=socket.create_connection(("<vps-ip>",42450)); b.sendall(b"MOTE1 TEST\n")
print("A:",a.recv(8), "B:",b.recv(8))     # -> A: b'GO H\n'  B: b'GO G\n'
a.sendall(b"\xa5ping"); print("relayed:", b.recv(8))
EOF
```

## Wire protocol (for the `link_net.c` relay client)

One text handshake line, then raw bytes. Every message from the client starts
`MOTE1 <VERB> …`; `CODE` = 1–8 `[A-Z0-9]` (upper-cased, other chars dropped),
`LABEL` = one token ≤16 chars shown in `LIST`.

| Client sends | Purpose |
|---|---|
| `MOTE1 HOST <CODE> <PUB\|PRIV> [LABEL]\n` | create a room and wait. `PUB` = listed by `LIST`; `PRIV` = code-only (share it out-of-band) |
| `MOTE1 JOIN <CODE>\n` | join a specific room (public or private) |
| `MOTE1 LIST\n` | browse open **public** rooms |
| `MOTE1 QUICK [LABEL]\n` | one-tap match: join the oldest open public room, else auto-host a public one and wait |

| Relay replies | Meaning |
|---|---|
| `GO H\n` → host, `GO G\n` → joiner | paired; **raw byte relay begins** |
| `ROOM <CODE> <LABEL>\n` × N, then `END\n` | answer to `LIST` (query only, never relayed, then closed) |
| `NONE\n` | `JOIN` of a code with no waiting host |
| `TAKEN\n` | `HOST` of a code already in use |
| `TIMEOUT\n` | no partner within `--join-timeout` |
| `ERR\n` | malformed handshake |

After `GO`, every byte each side sends is forwarded verbatim to the other. The
`H`/`G` hint feeds `link_net_is_host()`, but the **games break symmetry with their
own random nonce** (never `link_is_host`, which is 0 on both ends over the Studio
bridge), so it's advisory only.

### Rooms of up to eight (`ROOMN`, MOTE2 only)

A hub rather than a splice, for games with more than two players (CueVR 6.1
doubles, Killer, team matches). Additive: no old verb changed, and an old relay
answers `ERR` to it. The exact frames are in the docstring of `mote_relay.py`.

| Client sends | Relay replies |
|---|---|
| `MOTE2 ROOMN <GAMEID> HOST <CODE> <PUB\|PRIV> <MAX 2..8> [LABEL…]\n` | `SEAT 0 <MAX>\n`, then framed; `TAKEN` / `ERR` |
| `MOTE2 ROOMN <GAMEID> JOIN <CODE>\n` | `SEAT <K> <MAX>\n`, then framed; `NONE` / `FULL` / `BUSY` (started) |
| `MOTE2 ROOMN <GAMEID> QUICK <MAX> [LABEL…]\n` | a place in the oldest open public room of that size, else hosts one |

After `SEAT`, every message is a frame (`u16` little-endian length first). A
client sends `to | payload` (`to` = a member, `0xFF` everyone else, `0xFE` the
relay); the relay delivers `from | u32 room_seq | payload`, one sequence for
the whole room, so every member sees one order. The relay's own frames come
`from 0xFE`: `MEMBERS …`, `JOINED k`, `LEFT k`, `START`, `CLOSED`. The host
(member 0) sends `START` to take the room off `LIST` and close it to `JOIN`.
The host leaving closes the room. A member with more than `--room-backlog`
bytes (default 256 KB) unsent is dropped rather than stalling the room. `LIST`
shows open N-rooms as `ROOM <CODE> <LABEL> <HAVE>/<MAX>`.

Bench: `python3 test_roomn.py --old <the previous mote_relay.py>` runs its own
relays on localhost: eight members and one order, unicast, `START`, leaving,
`QUICK`, the backlog drop, and the old verbs byte for byte against the old
relay.

### Voice in the rooms (`VOICE`, CueVR test builds first)

A room's members may talk. It is kept entirely apart from the game's frames:
a client that never asks is never sent any of it, and the game's frames are
numbered exactly as they would be without it (the voice's own frames and
answers take no room sequence number). An old client in the same room as a
talker sees byte for byte what the previous relay sends it; `test_roomn.py
--old` checks exactly that.

| Client sends | Relay replies |
|---|---|
| `VOICE` (to `0xFE`) | `VOICE <TOKEN> <UDPPORT>` to that member alone (16 hex digits); nothing from an old relay or one run `--no-voice` |
| `VOICE TCP` / `VOICE UDP` (to `0xFE`) | nothing: how the member wants voice delivered |
| UDP `'K' 1 TOKEN(8) id(4) ms(4)` | UDP `'P' 1 id ms` — the keepalive and its answer |
| UDP `'A' 1 TOKEN(8) <packet>` | UDP `'A' 1 <from> <packet>` to every other member that asked |
| frame to `0xFD`, payload `<packet>` | frame from `0xFD`, payload `<from> <packet>`, to members on TCP |
| `REPORT <K> <REASON> <REPORTED>\|<REPORTER>` (to `0xFE`) | `REPORTED`, `REPORT OFF` (no `--store`), `REPORT BUSY`; a line in `<store>/reports.tsv` |

**Reports** are one tab-separated line each: time, game, room, reason, the
reported and the reporting display names, their places, and a salted hash of
each address -- HMAC-SHA256 under the relay's own secret (`<store>/reports.salt`,
made on the first report, mode 600), cut to 16 hex digits -- never the address
itself, as the privacy policy says. The same address always hashes the same,
so repeat reports about one connection can be matched. Keep `reports.salt`
with the reports (a new salt matches nothing old), and delete a report's line
no later than 90 days after it has been dealt with.

A token is good only while its member is in the room. The relay sends voice by
UDP to a member whose keepalives it has heard in the last 20 s and who has not
asked for TCP; otherwise down the member's TCP, unless more than
`--voice-backlog` bytes (16 KB) are already queued to it, when the packet is
dropped (it would be stale, and the game's frames come first). A watcher's
voice is never passed on. 100 packets a second per member at most (one talker
is 50). The relay never looks inside a voice packet and never keeps one.

Flags: `--voice-port` (UDP, default the same number as `--port`),
`--no-voice`, `--voice-backlog`.

**Deploying a relay with voice** needs the UDP port open as well as the TCP
one, in both places (the cloud's security list *and* the box's iptables), e.g.
`sudo iptables -I INPUT 6 -p udp --dport 443 -j ACCEPT`. If UDP is not
opened, nothing breaks: every client falls back to voice over its TCP.

**Wired into the Studio.** `link_net.c` has a relay transport alongside Host/Join
LAN: it connects out to the configured `relay_host:port`, does the `MOTE1 …`
handshake, reads the `GO` line, then the same `s_conn` pipe drives send/recv so
the bridge + preview are unchanged. Configure with **`MOTE_RELAY=host[:port]`**
(default port 443) before launching the Studio; the DEVICE tab then shows an
**ONLINE** row: **Quick Match** (`QUICK`), **Host Room** (`HOST <auto-code> PUB
<game>` — the code is shown to share), and **Browse** (`LIST` → click a room →
`JOIN`). Then **Bridge USB** to relay a docked device, or just run the preview.
(Headless test hooks: `MOTE_RELAY_HOST=<code>` / `MOTE_RELAY_JOIN=<code>` /
`MOTE_RELAY_QUICK=1` autostart a room at launch.)

## Scaling later

This runs thousands of concurrent pairs on the cheapest box (traffic is a few
KB/s per pair). If you'd rather not maintain a VM, the same relay maps cleanly
onto **Cloudflare Durable Objects** (a Durable Object = one room; WebSocket
transport), which needs no server — at the cost of adding a WebSocket client to
`link_net.c` (Cloudflare doesn't accept raw TCP). Keep this for the first trial;
port later if it takes off.
