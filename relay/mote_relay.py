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

    SHARE CODES (MOTE2 only): a small blob -- a cue design, a ball set -- kept
    on disk under a six-character code, so a player can hand a thing they made
    to someone else by reading out six letters. Nothing is relayed; the
    connection closes after the reply.
        "MOTE2 PUT <GAMEID> <KIND> <LEN>\n" + LEN bytes  ->  "CODE <CODE>\n"
        "MOTE2 GET <GAMEID> <CODE>\n"  ->  "DATA <KIND> <LEN>\n" + LEN bytes,
                                           or "NONE\n"
    KIND = 1-8 [A-Z0-9]. LEN <= 8192. The same blob always gets the same code.
    Errors: "ERR\n" (malformed), "BUSY\n" (this address is putting too fast),
    "FULL\n" (the store is at its cap), "OFF\n" (no --store).

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
import hashlib
import os
import socket
import time

def log(*a):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *a, flush=True)

import random

def clean_code(raw: str) -> str:
    out = "".join(c for c in raw.upper() if c.isalnum())
    return out[:8]

def clean_label(raw: str) -> str:
    """A room's label, as shown by LIST.

    WAS 16 ALPHANUMERIC CHARACTERS, which is a game tag and nothing more. A
    player browsing CueVR's rooms already knows they are CueVR rooms; what they
    want is who is hosting and what is being played, and neither fits. Spaces
    are allowed now and the cap is 48.

    Still stripped hard, because this string is echoed to every other client:
    no control characters, no newline (which would forge a second ROOM line),
    and a bounded length whatever a client sends."""
    out = "".join(c for c in raw if c.isalnum() or c in "-_ .+/'")
    out = " ".join(out.split())          # no runs of spaces, no leading/trailing
    return out[:48] or "GAME"

async def read_line(reader: asyncio.StreamReader, cap: int = 160) -> bytes:
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
    __slots__ = ("reader", "writer", "future", "public", "label", "created", "gid", "code",
                 "claimed", "released")
    def __init__(self, reader, writer, future, public, label, gid, code):
        self.reader, self.writer, self.future = reader, writer, future
        self.public, self.label, self.gid, self.code = public, label, gid, code
        self.created = time.monotonic()
        self.claimed = asyncio.Event()    # a joiner has taken the room
        self.released = asyncio.Event()   # host handler has stopped watching the reader

SHARE_ALPHA = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"   # no confusable 0/O/1/I

class Store:
    """Share codes on disk: <dir>/<gid>/<CODE> holds "KIND\n" then the blob,
    and <dir>/<gid>/index.tsv maps each blob's hash to its code, so putting the
    same thing twice hands back the code it already has. Loaded at startup;
    small files, written whole, so a crash never leaves half a design."""
    def __init__(self, root, cap, per_hour):
        self.root, self.cap, self.per_hour = root, cap, per_hour
        self.by_hash = {}      # "gid/sha" -> code
        self.codes = set()     # "gid/CODE"
        self.puts = {}         # ip -> [times]
        for gid in (os.listdir(root) if os.path.isdir(root) else []):
            idx = os.path.join(root, gid, "index.tsv")
            if not os.path.isfile(idx):
                continue
            for ln in open(idx, encoding="ascii", errors="ignore"):
                t = ln.split()
                if len(t) == 2:
                    self.by_hash[gid + "/" + t[0]] = t[1]
                    self.codes.add(gid + "/" + t[1])
        log(f"share store {root}: {len(self.codes)} codes")

    def ok_rate(self, ip):
        now = time.monotonic()
        q = [t for t in self.puts.get(ip, []) if now - t < 3600]
        if len(q) >= self.per_hour:
            self.puts[ip] = q
            return False
        q.append(now); self.puts[ip] = q
        if len(self.puts) > 10000:          # forget the quiet ones
            self.puts = {k: v for k, v in self.puts.items() if v and now - v[-1] < 3600}
        return True

    def put(self, gid, kind, blob):
        sha = hashlib.sha256(kind.encode() + b"\n" + blob).hexdigest()[:32]
        have = self.by_hash.get(gid + "/" + sha)
        if have:
            return have
        if len(self.codes) >= self.cap:
            return None
        for _ in range(50):
            code = "".join(random.choice(SHARE_ALPHA) for _ in range(6))
            if gid + "/" + code not in self.codes:
                break
        else:
            return None
        d = os.path.join(self.root, gid)
        os.makedirs(d, exist_ok=True)
        tmp = os.path.join(d, code + ".tmp")
        with open(tmp, "wb") as f:
            f.write(kind.encode() + b"\n" + blob)
        os.replace(tmp, os.path.join(d, code))
        with open(os.path.join(d, "index.tsv"), "a", encoding="ascii") as f:
            f.write(f"{sha} {code}\n")
        self.by_hash[gid + "/" + sha] = code
        self.codes.add(gid + "/" + code)
        return code

    def get(self, gid, code):
        if gid + "/" + code not in self.codes:
            return None
        try:
            raw = open(os.path.join(self.root, gid, code), "rb").read()
        except OSError:
            return None
        nl = raw.find(b"\n")
        if nl < 0:
            return None
        return raw[:nl].decode("ascii", "ignore"), raw[nl + 1:]

def clean_gid(raw: str) -> str:
    """A game id as a directory name: letters and digits only."""
    return "".join(c for c in raw.upper() if c.isalnum())[:16]

def rkey(gid, code):
    return gid + "/" + code       # rooms are namespaced per game, so codes don't collide across games

class Relay:
    def __init__(self, args):
        self.args = args
        self.rooms = {}            # "gid/CODE" -> Room  (a host waiting for a partner)
        self.conns = 0
        self.store = Store(args.store, args.store_cap, args.puts_per_hour) if args.store else None

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
        room.claimed.set()                       # host handler: stop watching the reader
        await room.released.wait()               # ...and confirm before we read it
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
        # A waiting room has NO clock: it lives exactly as long as its host's socket.
        # We watch the host's reader for EOF (TCP keepalive surfaces silent deaths in
        # ~a minute), so cancelled/crashed/unplugged hosts reap instantly and a patient
        # host can wait as long as they like. --join-timeout (default: off) remains as
        # an optional operator backstop. History: a mandatory 120s wait_for here used
        # to CANCEL room.future on timeout, unwinding this handler and closing the
        # host's socket EVEN MID-MATCH — every paired session died on that clock.
        watch = asyncio.create_task(room.reader.read(1))
        claim = asyncio.create_task(room.claimed.wait())
        tmo = self.args.join_timeout if self.args.join_timeout > 0 else None
        done, pending = await asyncio.wait({watch, claim}, timeout=tmo,
                                           return_when=asyncio.FIRST_COMPLETED)
        if claim in done:                                # a joiner took the room
            watch.cancel()
            await asyncio.gather(watch, return_exceptions=True)
            room.released.set()                          # reader handed to the relay
            try:
                await room.future                        # hold the socket until the match ends
            except (Exception, asyncio.CancelledError):
                pass
            return
        # not claimed: EOF/error on the host (left / died) — or the optional backstop
        for t in (watch, claim):
            t.cancel()
        await asyncio.gather(watch, claim, return_exceptions=True)
        room.released.set()                              # never leave pair() waiting forever
        if self.rooms.get(key) is room:
            self.rooms.pop(key, None)
        if watch in done:
            log(f"room {key}: host left while waiting")
        else:
            try:
                writer.write(b"TIMEOUT\n"); await writer.drain()
            except OSError:
                pass
            log(f"room {key}: timed out (backstop)")

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

            if verb in ("PUT", "GET") and t[0] == "MOTE2":
                sg = clean_gid(gid)
                if self.store is None or not sg:
                    writer.write(b"OFF\n" if self.store is None else b"ERR\n"); await writer.drain(); return
                if verb == "PUT":               # PUT <KIND> <LEN>, then the blob
                    kind = clean_code(a[0]) if len(a) > 0 else ""
                    try:
                        n = int(a[1]) if len(a) > 1 else -1
                    except ValueError:
                        n = -1
                    if not kind or n <= 0 or n > 8192:
                        writer.write(b"ERR\n"); await writer.drain(); return
                    ip = peer[0] if isinstance(peer, tuple) else str(peer)
                    if not self.store.ok_rate(ip):
                        writer.write(b"BUSY\n"); await writer.drain(); return
                    blob = await asyncio.wait_for(reader.readexactly(n), timeout=15)
                    code = self.store.put(sg, kind, blob)
                    writer.write((f"CODE {code}\n" if code else "FULL\n").encode()); await writer.drain()
                    log(f"share {sg}: put {kind} {n}B -> {code} ({peer})")
                    return
                code = clean_code(a[0]) if len(a) > 0 else ""
                got = self.store.get(sg, code) if code else None
                if got is None:
                    writer.write(b"NONE\n"); await writer.drain(); return
                kind, blob = got
                writer.write(f"DATA {kind} {len(blob)}\n".encode() + blob); await writer.drain()
                log(f"share {sg}: get {code} ({peer})")
                return

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
                # THE REST OF THE LINE, not the third token: a label with spaces
                # in it is the whole point. Old clients send one word and get
                # exactly what they always did.
                label = clean_label(" ".join(a[2:])) if len(a) > 2 else "GAME"
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
                label = clean_label(" ".join(a[0:])) if len(a) > 0 else "QUICK"
                key = rkey(gid, code); my_code = key
                await self.wait_as_host(key, Room(reader, writer, loop.create_future(), True, label, gid, code), writer, peer)
                return

            writer.write(b"ERR\n"); await writer.drain()
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, OSError, ConnectionError):
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
    ap.add_argument("--join-timeout", type=int, default=0, help="optional backstop: seconds a LONE host may wait before the room is reaped (0 = wait as long as the host stays connected; never fires on a paired room)")
    ap.add_argument("--idle", type=int, default=900, help="seconds of TRUE silence before a paired room is dropped (TCP keepalive catches dead peers in ~70s regardless; this is only a backstop for a paused/AFK pair)")
    ap.add_argument("--max-conns", type=int, default=2000)
    ap.add_argument("--store", default="", help="directory for share codes (PUT/GET); empty = share codes off")
    ap.add_argument("--store-cap", type=int, default=200000, help="most share codes kept")
    ap.add_argument("--puts-per-hour", type=int, default=60, help="share codes one address may make an hour")
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
