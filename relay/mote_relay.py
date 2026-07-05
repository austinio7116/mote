#!/usr/bin/env python3
"""
Mote link relay — a dumb byte pipe between two Mote Studios over the internet.

The Studio's LAN link (studio/link_net.c) connects two units on the same subnet.
This relay extends that across the internet WITHOUT port-forwarding: both Studios
make OUTBOUND connections to this server (outbound always traverses home NAT),
join the same short room code, and the server splices their two sockets — it
forwards raw bytes and never parses the game protocol (the 0xA5 framing stays
end-to-end). So the device -> Studio -> [internet] -> Studio -> device path is
just the existing byte pipe with this relay in the middle.

Wire protocol (client <-> relay), one text handshake line then raw bytes:
    HOST a room and wait for a partner:
        "MOTE1 HOST <CODE> <PUB|PRIV> [LABEL]\n"   PUB = listed by LIST, PRIV = code-only
    JOIN a specific room by code (public or private):
        "MOTE1 JOIN <CODE>\n"
    Browse open rooms (query only, never relayed):
        "MOTE1 LIST\n"  ->  "ROOM <CODE> <LABEL>\n" * N , then "END\n" , close
    One-tap matchmaking (join the oldest open room, else auto-host a public one):
        "MOTE1 QUICK [LABEL]\n"
    CODE = 1-8 chars [A-Z0-9] (upper-cased, other chars dropped). LABEL = one
    token, <=16 chars, shown in LIST (a game/player tag).

    When two clients meet, the relay pairs them:
        -> host:   "GO H\n"      -> joiner: "GO G\n"
    then every byte from one is forwarded verbatim to the other until either
    side closes. (H/G is a host/guest hint for link_net; the GAMES break symmetry
    with their own nonce, so it's advisory.)
    error replies (then close):  "ERR\n"  "NONE\n" (no such room)  "TAKEN\n" (code
    in use)  "TIMEOUT\n" (no partner)

Deploy: it's a single stdlib file — no pip, no build. Copy it to the box, run
under systemd (see mote-relay.service). Python 3.8+.

    python3 mote_relay.py [--addr 0.0.0.0] [--port 42450]
                          [--join-timeout 120] [--idle 45] [--max-conns 2000]
"""
import argparse
import asyncio
import socket
import time

def log(*a):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *a, flush=True)

import random

def clean_code(raw: str) -> str:
    out = "".join(c for c in raw.upper() if c.isalnum())
    return out[:8]

def clean_label(raw: str) -> str:
    out = "".join(c for c in raw if c.isalnum() or c in "-_")
    return out[:16] or "GAME"

async def read_line(reader: asyncio.StreamReader, cap: int = 64) -> bytes:
    """Read one '\\n'-terminated line, at most `cap` bytes, WITHOUT over-reading
    into the game stream that follows (so no game bytes are ever swallowed)."""
    buf = bytearray()
    while len(buf) < cap:
        b = await reader.read(1)
        if not b or b == b"\n":
            break
        if b != b"\r":
            buf += b
    return bytes(buf)

def tune(writer: asyncio.StreamWriter):
    """Low latency + AGGRESSIVE keepalive. The keepalive probes are what keep the
    home-router / carrier-grade-NAT mapping alive through quiet moments (a pause,
    a menu, a long turn) — without them the NAT silently drops the TCP connection
    and both players get a 'link lost' with nothing in any log. Probes also detect
    a genuinely dead peer within ~1 minute so rooms don't linger."""
    sock = writer.get_extra_info("socket")
    if sock is None:
        return
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        if hasattr(socket, "TCP_KEEPIDLE"):  sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 30)   # first probe after 30s idle
        if hasattr(socket, "TCP_KEEPINTVL"): sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 10)  # then every 10s
        if hasattr(socket, "TCP_KEEPCNT"):   sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 4)     # dead after ~70s
    except OSError:
        pass

class Room:
    __slots__ = ("reader", "writer", "future", "public", "label", "created", "gid", "code")
    def __init__(self, reader, writer, future, public, label, gid, code):
        self.reader, self.writer, self.future = reader, writer, future
        self.public, self.label, self.gid, self.code = public, label, gid, code
        self.created = time.monotonic()

def rkey(gid, code):
    return gid + "/" + code       # rooms are namespaced per game, so codes don't collide across games

class Relay:
    def __init__(self, args):
        self.args = args
        self.rooms = {}            # "gid/CODE" -> Room  (a host waiting for a partner)
        self.conns = 0

    def gen_code(self, gid) -> str:
        alpha = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"   # no confusable 0/O/1/I
        for _ in range(20):
            c = "".join(random.choice(alpha) for _ in range(4))
            if rkey(gid, c) not in self.rooms:
                return c
        return "R" + "".join(random.choice(alpha) for _ in range(5))

    async def pipe(self, r: asyncio.StreamReader, w: asyncio.StreamWriter, reason, tag=""):
        """Forward r -> w until EOF, error, or `idle` seconds of true silence.
        Records WHY it ended in reason[0] so the room-closed log is diagnosable.
        (Idle is a long backstop now; TCP keepalive catches dead peers far sooner.)
        Logs any forwarding gap >2.5s when traffic RESUMES — the smoking gun for
        'both games said LINK LOST but nothing disconnected'."""
        last = time.monotonic()
        try:
            while True:
                data = await asyncio.wait_for(r.read(4096), timeout=self.args.idle)
                now = time.monotonic()
                if not data:
                    reason[0] = "peer EOF"; break
                if now - last > 2.5:
                    log(f"{tag}: silent {now-last:.1f}s then resumed")
                last = now
                w.write(data)
                await w.drain()
        except asyncio.TimeoutError:
            reason[0] = f"idle >{self.args.idle}s"
        except (OSError, ConnectionError) as e:
            reason[0] = f"net {type(e).__name__}"
        finally:
            try:
                w.close()
            except OSError:
                pass

    async def relay(self, r1, w1, r2, w2, tag=""):
        why = [""]
        t1 = asyncio.create_task(self.pipe(r1, w2, why, f"{tag} host->guest"))
        t2 = asyncio.create_task(self.pipe(r2, w1, why, f"{tag} guest->host"))
        await asyncio.wait({t1, t2}, return_when=asyncio.FIRST_COMPLETED)
        for w in (w1, w2):          # closing either end unblocks the other pipe
            try:
                w.close()
            except OSError:
                pass
        await asyncio.gather(t1, t2, return_exceptions=True)
        return why[0] or "closed"

    async def pair(self, room: "Room", reader, writer, peer):
        """`room` is the waiting host; we're the joiner. Splice them."""
        room.writer.write(b"GO H\n"); await room.writer.drain()
        writer.write(b"GO G\n"); await writer.drain()
        log(f"room {room.gid}/{room.code}: paired ({peer})")
        why = await self.relay(room.reader, room.writer, reader, writer,
                               tag=f"room {room.gid}/{room.code}")
        if not room.future.done():
            room.future.set_result(True)        # release the host's handler
        log(f"room {room.gid}/{room.code}: closed ({why})")

    async def wait_as_host(self, key, room: "Room", writer, peer):
        """Register `room` and hold this connection (untouched) for a partner."""
        self.rooms[key] = room
        log(f"room {key}: waiting {'(public)' if room.public else '(private)'} ({peer})")
        try:
            await asyncio.wait_for(room.future, timeout=self.args.join_timeout)
        except asyncio.TimeoutError:
            if self.rooms.get(key) is room:
                self.rooms.pop(key, None)
                try:
                    writer.write(b"TIMEOUT\n"); await writer.drain()
                except OSError:
                    pass
                log(f"room {key}: timed out")
            else:
                try: await room.future      # a joiner grabbed us; let the relay finish
                except Exception: pass

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        peer = writer.get_extra_info("peername")
        if self.conns >= self.args.max_conns:
            writer.close(); return
        self.conns += 1
        tune(writer)
        my_code = None                          # set only if we register as a host
        try:
            line = await asyncio.wait_for(read_line(reader), timeout=10)
            t = line.decode("ascii", "ignore").split()
            # MOTE2 (game-gated): "MOTE2 <VERB> <GAMEID> ..."  ·  MOTE1 (legacy, ungated): "MOTE1 <VERB> ..."
            if len(t) < 2 or t[0] not in ("MOTE1", "MOTE2"):
                writer.write(b"ERR\n"); await writer.drain(); return
            verb = t[1].upper()
            if t[0] == "MOTE2":
                gid = t[2] if len(t) > 2 else ""
                a = t[3:]                        # verb args after the game-id
                if not gid:
                    writer.write(b"ERR\n"); await writer.drain(); return
            else:
                gid = "*"                        # legacy shared pool
                a = t[2:]
            loop = asyncio.get_event_loop()

            if verb == "LIST":                  # browse this game's open public rooms
                out = bytearray()
                for k, r in list(self.rooms.items()):
                    if r.public and r.gid == gid:
                        out += f"ROOM {r.code} {r.label}\n".encode()
                        if len(out) > 3500: break
                out += b"END\n"
                writer.write(out); await writer.drain()
                return

            if verb == "HOST":                  # HOST <CODE> <PUB|PRIV> [LABEL]
                code = clean_code(a[0]) if len(a) > 0 else ""
                if not code:
                    writer.write(b"ERR\n"); await writer.drain(); return
                key = rkey(gid, code)
                if key in self.rooms:
                    writer.write(b"TAKEN\n"); await writer.drain(); return
                public = (len(a) > 1 and a[1].upper() == "PUB")
                label = clean_label(a[2]) if len(a) > 2 else "GAME"
                my_code = key
                await self.wait_as_host(key, Room(reader, writer, loop.create_future(), public, label, gid, code), writer, peer)
                return

            if verb == "JOIN":                  # JOIN <CODE>  (must be the same game-id)
                code = clean_code(a[0]) if len(a) > 0 else ""
                room = self.rooms.pop(rkey(gid, code), None) if code else None
                if room is None:
                    writer.write(b"NONE\n"); await writer.drain(); return
                await self.pair(room, reader, writer, peer)
                return

            if verb == "QUICK":                 # QUICK [LABEL] — match within this game only
                oldest = None
                for k, r in self.rooms.items():
                    if r.public and r.gid == gid and (oldest is None or r.created < self.rooms[oldest].created):
                        oldest = k
                if oldest is not None:
                    await self.pair(self.rooms.pop(oldest), reader, writer, peer)
                    return
                code = self.gen_code(gid)        # ...else host a public room and wait
                label = clean_label(a[0]) if len(a) > 0 else "QUICK"
                key = rkey(gid, code); my_code = key
                await self.wait_as_host(key, Room(reader, writer, loop.create_future(), True, label, gid, code), writer, peer)
                return

            writer.write(b"ERR\n"); await writer.drain()
        except (asyncio.TimeoutError, OSError, ConnectionError):
            pass
        finally:
            # if we registered as a host and are bailing, don't leak the slot.
            if my_code and self.rooms.get(my_code) is not None and self.rooms[my_code].writer is writer:
                self.rooms.pop(my_code, None)
            try:
                writer.close()
            except OSError:
                pass
            self.conns -= 1

async def main():
    ap = argparse.ArgumentParser(description="Mote link relay (byte pipe + room codes)")
    ap.add_argument("--addr", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=42450)
    ap.add_argument("--join-timeout", type=int, default=120, help="seconds a lone client waits for a partner")
    ap.add_argument("--idle", type=int, default=900, help="seconds of TRUE silence before a paired room is dropped (TCP keepalive catches dead peers in ~70s regardless; this is only a backstop for a paused/AFK pair)")
    ap.add_argument("--max-conns", type=int, default=2000)
    args = ap.parse_args()

    async def watchdog():
        """The one instrument that indicts the VM itself: sleep(0.25) waking late
        means the event loop (or the whole box) stalled — every room froze with it."""
        last = time.monotonic()
        while True:
            await asyncio.sleep(0.25)
            now = time.monotonic()
            lag = now - last - 0.25
            if lag > 1.0:
                log(f"WATCHDOG: event loop stalled {lag:.1f}s (VM starvation?)")
            last = now
    asyncio.get_event_loop().create_task(watchdog())

    relay = Relay(args)
    server = await asyncio.start_server(relay.handle, args.addr, args.port)
    log(f"mote-relay listening on {args.addr}:{args.port} "
        f"(join-timeout={args.join_timeout}s idle={args.idle}s max={args.max_conns})")
    async with server:
        await server.serve_forever()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
