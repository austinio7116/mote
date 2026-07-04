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

`link_net.c` gets a third transport alongside Host/Join LAN — connect out to a
configured `relay_host:42450`, send the chosen `MOTE1 …` line, read the `GO`
line, then hand the socket to the existing bridge/pipe unchanged. The DEVICE-tab
UI maps naturally: **Host Private** (`HOST <code> PRIV`), **Host Public**
(`HOST <code> PUB <name>`), **Quick Match** (`QUICK`), and a **Browse** list
(`LIST` → pick → `JOIN`).

## Scaling later

This runs thousands of concurrent pairs on the cheapest box (traffic is a few
KB/s per pair). If you'd rather not maintain a VM, the same relay maps cleanly
onto **Cloudflare Durable Objects** (a Durable Object = one room; WebSocket
transport), which needs no server — at the cost of adding a WebSocket client to
`link_net.c` (Cloudflare doesn't accept raw TCP). Keep this for the first trial;
port later if it takes off.
