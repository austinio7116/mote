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

    THE GALLERY: a code its owner has chosen to show everyone. Sharing a code
    never lists it; publishing is a second, separate step.
        "MOTE2 PUB <GAMEID> <CODE> <PICLEN> <NAME...>\n" + PICLEN bytes
                                        ->  "OK\n", "NONE\n" (no such code),
                                            "BUSY\n" (too many for now)
        "MOTE2 THUMB <GAMEID> <CODE>\n"  ->  "DATA THUMB <LEN>\n" + LEN bytes,
                                            or "NONE\n"
    The picture is the game's own photograph of the thing, in the game's own
    format, at most 40000 bytes (PICLEN 0 for none); the relay keeps it and
    hands it back, and never looks inside it.
        "MOTE2 GALLERY <GAMEID> <KIND> <FROM> <COUNT> [NEW|TOP] [DEVICE]\n"
            ->  "ITEM <CODE> <UNIXTIME> <LIKES> <MINE> <NAME...>\n" * n ,
                then "END <TOTAL>\n"
    newest first (NEW, the default) or most liked first (TOP, newest breaking a
    tie), COUNT <= 50. MINE is 1 when DEVICE has liked that entry.
        "MOTE2 LIKE <GAMEID> <CODE> <DEVICE>\n"  ->  "LIKES <N> <MINE>\n"
    toggles DEVICE's like of a listed entry; "NONE\n" if it is not listed,
    "BUSY\n" past the per-address hourly cap. DEVICE is the headset's own
    random id: one like per headset per entry, which a script can get round --
    the cap slows that, and a Meta user proof can replace the id later without
    changing the wire. A published code is listed once, under the name
    it was first published with. TAKING ONE DOWN is the operator's: a line with
    the code in <store>/hidden.txt (every game) or <store>/<GAMEID>/hidden.txt
    hides it from every gallery at once -- the file is read again whenever it
    changes, so no restart. The code itself still works for GET.
    Errors: "ERR\n" (malformed), "BUSY\n" (this address is putting too fast),
    "FULL\n" (the store is at its cap), "OFF\n" (no --store).

    ROOMS OF UP TO EIGHT (MOTE2 only): a hub rather than a splice. Every
    member's frames go to the members they name, stamped with who sent them
    and one room-wide sequence number, so every member sees the one order.
        "MOTE2 ROOMN <GAMEID> HOST <CODE> <PUB|PRIV> <MAX 2..8> [LABEL...]\n"
                    ->  "SEAT 0 <MAX>\n", then framed    (or "TAKEN\n", "ERR\n")
        "MOTE2 ROOMN <GAMEID> JOIN <CODE>\n"
                    ->  "SEAT <K> <MAX>\n", then framed
                        or "NONE\n" / "FULL\n" / "BUSY\n" (started)
        "MOTE2 ROOMN <GAMEID> QUICK <MAX> [LABEL...]\n"
                    ->  a place in the oldest open public room of that size,
                        else a new public one hosted: "SEAT <K> <MAX>\n"
    LIST shows these rooms beside the old ones, with the places taken:
        "ROOM <CODE> <LABEL> <HAVE>/<MAX>\n"
    Frames, little-endian lengths:
        client -> relay:  u16 len | u8 to | payload         (len = 1 + payload)
                          to = a member 0..7, 0xFF everyone else, 0xFE the relay
        relay -> client:  u16 len | u8 from | u32 seq | payload  (len = 5 + payload)
                          from 0xFE is the relay itself, payload ASCII:
                          "MEMBERS <k...>" (to a new member: who is here, itself
                          included), "JOINED <k>", "LEFT <k>", "START", "CLOSED"
    To the relay: "START" (the host only) takes the room off LIST and closes it
    to JOIN; everyone hears "START". The host is member 0; a joiner takes the
    lowest free place. The host leaving closes the room ("CLOSED" to all). A
    member whose unsent backlog passes --room-backlog bytes is dropped (LEFT)
    rather than stalling the room. Old verbs are untouched.

    CLUBS (CueVR 7.1, Down the Club): a shared room of up to 16, framed as a
    ROOMN room is, with no host, no START and no watchers -- presence only;
    a table's game is an ordinary ROOMN room beside it.
        "MOTE2 CLUB <GAMEID> JOIN [N]\n"
                    ->  "SEAT <K> 16 <N>\n", then framed (or "FULL\n", "ERR\n")
    Without N: the lowest numbered club with room, a new one opened when all
    are full. To the relay: "NAME <text>" (everyone hears "NAME <k> <text>"),
    "TABLE <slot> <text...>" / "TABLE <slot> -" (a table set up or freed by
    this member; everyone hears "TABLE <slot> <k> <text>" / "TABLE <slot> -";
    a slot another member holds is not taken). A newcomer hears MEMBERS, then
    every NAME and TABLE. A member leaving frees its tables (LEFT <k> after
    them); the club closes when the last one goes. A member that falls behind
    loses frames, never its place. LIST adds, per open club:
        "CLUB <N> <HAVE>/16 <TABLES>\n"  then  "CLUBWHO <N> <name>\n" * HAVE
    Clubs are never listed as ROOM or LIVE. Voice and REPORT work as in a room.

    NEWER (CueVR 7.3): the game ids that are current are known to the relay --
    --latest-game (repeatable: 'CUEn', 0x4355456E or a number) and the file
    <store>/latest_games (one a line, read again whenever it changes, so a
    release needs no restart). A game id below a current one of the same
    family (its first three bytes, 'CUE') is out of date: it is still let in,
    to its own clubs and rooms as ever, and told so -- "SEAT <K> 16 <N> NEWER"
    on a CLUB join and a "NEWER" line before LIST's END. Readers from before
    it take the three numbers and skip the line, as they skip any they do not
    know.

    VOICE (CueVR 6.4, test builds first): a room's members may talk. Nothing
    of it reaches a client that does not ask, so an old client in the same
    room sees exactly the frames it always did, numbered exactly as before.
        to the relay:  "VOICE"  ->  from 0xFE, to that member alone:
                       "VOICE <TOKEN> <UDPPORT>"   (TOKEN: 16 hex digits)
                       (an old relay, or one run with --no-voice, says
                       nothing, and the client simply has no voice)
                       "VOICE TCP" / "VOICE UDP": how this member wants voice
                       delivered (it says TCP when the relay's UDP never
                       reaches it; UDP again when it does)
    UDP, to <relay>:<UDPPORT>, every datagram carrying the member's token:
        client -> relay:  'K' 1 TOKEN(8) u32 id u32 ms   (keepalive; answered)
                          'A' 1 TOKEN(8) <voice packet>  (fanned out)
        relay -> client:  'P' 1 u32 id u32 ms            (the answer)
                          'A' 1 u8 from <voice packet>
    TCP, for a member whose UDP does not get through:
        client -> relay:  frame to 0xFD, payload <voice packet>
        relay -> client:  frame from 0xFD, payload u8 from <voice packet>,
                          stamped with the room's CURRENT sequence (it takes
                          no number of its own, so the game's frames are
                          numbered as if voice did not exist), and dropped
                          rather than queued behind more than --voice-backlog
                          bytes: game traffic first, stale voice never.
    A token is good only while its member is in the room; a watcher's voice
    is never passed on (it listens). The relay never looks inside a voice
    packet and never keeps one.
    RESULTS (CueVR 7.3): "RESULT <json>" to the relay, at the end of each
    frame of a room's match (and a second, "t":"match", at its end), from
    every player in it. A result counts only when the two sides' players send
    the same facts for the same frame -- score, frames, winner, breaks, shots,
    pots, fouls -- so one player cannot make up a result against another: it
    is kept in <store>/results.jsonl with both senders, each salted address
    hash, the room's code and how long the room had been open. Two that
    disagree go to results_disputed.jsonl; one never confirmed (the other
    left) goes to results_unconfirmed.jsonl when the room closes. A result
    that fails the checks (sizes, a snooker break past 155, frames past the
    match) is answered "RESULT BAD" and kept nowhere; too many from one
    address in an hour, "RESULT BUSY". No --store, "RESULT OFF".
    REPORTING SOMEONE (VRC.Content.3): "REPORT <K> <REASON> <REPORTED>|<REPORTER>"
    to the relay is kept, one line, in <store>/reports.tsv -- the time, the
    game and room, the reason, both display names, both places, and a salted
    hash of each address (HMAC-SHA256 under the relay's own secret,
    <store>/reports.salt, made once; never the address itself) -- and
    answered "REPORTED", or "REPORT OFF" (no --store), or "REPORT BUSY" (too
    many from one address in an hour). A report is kept no longer than 90
    days after it is dealt with (the policy): delete its line.

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
import hmac
import json
import os
import socket
import time
import urllib.parse
import urllib.request

def log(*a):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *a, flush=True)

import random

def clean_code(raw: str) -> str:
    out = "".join(c for c in raw.upper() if c.isalnum())
    return out[:8]

def clean_name(raw: str) -> str:
    """A gallery entry's name: what the player typed, echoed to everyone, so
    printable, one line, no tabs (the file's separator), and bounded."""
    out = "".join(c for c in raw.upper() if 32 <= ord(c) < 127 and c != "\t")
    out = " ".join(out.split())
    return out[:32] or "UNTITLED"

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

class Member:
    __slots__ = ("k", "reader", "writer", "q", "backlog", "task", "gone", "peer",
                 "vtok", "uaddr", "useen", "vtcp", "vwin", "vcount", "uid", "proofs")
    def __init__(self, k, reader, writer, peer):
        self.k, self.reader, self.writer, self.peer = k, reader, writer, peer
        self.q = asyncio.Queue()
        self.backlog = 0
        self.task = None
        self.gone = False
        self.uid = None            # the Meta account, once Meta has said the proof is good (7.3)
        self.proofs = 0
        # VOICE: the token it was given, where its UDP comes from and when it
        # was last heard there, whether it asked for voice over TCP, and its
        # packets this second (a cap, so one member cannot flood the room)
        self.vtok = None
        self.uaddr = None
        self.useen = 0.0
        self.vtcp = False
        self.vwin = 0.0
        self.vcount = 0

class RoomN:
    """A room of 2..8 members. Frames are forwarded in the order the relay
    reads them, and each gets the next room-wide sequence number as it is
    queued, so every member receives the frames it is sent in one total order
    (the event loop is single-threaded: numbering and queueing happen in one
    step). Each member has its own write queue and writer task, so a slow
    member cannot stall the others -- it is dropped past the backlog cap."""
    CTRL = 0xFE
    ALL = 0xFF
    def __init__(self, gid, code, public, maxn, label):
        self.gid, self.code, self.public, self.maxn, self.label = gid, code, public, maxn, label
        self.members = {}          # k -> Member
        self.seq = 0
        self.started = False
        self.playing = False       # an open room being played (winner stays on): listed to watch, still open
        self.closed = False
        self.created = time.monotonic()
        self.done = asyncio.Event()
        self.vdropped = 0          # voice packets not queued to a backed-up member
        self.results = {}          # (t, frame) -> results held for the other side's word (7.3)

    def free_place(self):
        for k in range(self.maxn):
            if k not in self.members:
                return k
        return -1

    # WATCHERS (6.3): places WATCH_BASE.. -- never a player's place, never
    # counted in the room's size, so a full or started room can still be
    # watched. A client from before them ignores any place past 7.
    WATCH_BASE, WATCH_MAX = 8, 16
    def free_watch(self):
        for k in range(self.WATCH_BASE, self.WATCH_BASE + self.WATCH_MAX):
            if k not in self.members:
                return k
        return -1
    def players(self):
        return {k: m for k, m in self.members.items() if k < self.WATCH_BASE}
    def watchers(self):
        return [k for k in self.members if k >= self.WATCH_BASE]

    def frame(self, frm, payload):
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        return (5 + len(payload)).to_bytes(2, "little") + bytes([frm]) + \
               self.seq.to_bytes(4, "little") + payload

    def send_to(self, m, data, cap):
        if m.gone:
            return
        m.backlog += len(data)
        m.q.put_nowait(data)
        if m.backlog > cap:
            log(f"nroom {self.gid}/{self.code}: member {m.k} backlog {m.backlog}B, dropped")
            self.drop(m, "backlog")

    def deliver(self, frm, to, payload, cap):
        """One frame from `frm`: to one member, or everyone but the sender.
        A watcher speaks to the host alone, whatever it addresses: it can say
        hello, and nothing it sends can reach the table of anyone playing."""
        if frm >= self.WATCH_BASE:
            to = 0
        if to == self.ALL:
            targets = [m for k, m in sorted(self.members.items()) if k != frm]
        else:
            m = self.members.get(to)
            targets = [m] if m is not None and to != frm else []
        if not targets:
            return
        data = self.frame(frm, payload)      # ONE number for all its receivers
        for m in targets:
            self.send_to(m, data, cap)

    VOICE = 0xFD
    def voice(self, frm, pkt, udp, vcap):
        """One voice packet from member `frm`, to every other member that
        asked for voice: by UDP where its UDP gets through, else down its TCP
        -- unless that is backed up, when the packet is simply dropped (it is
        stale by the time it would arrive, and the game's frames come first).
        A watcher listens only."""
        if frm >= self.WATCH_BASE or self.closed:
            return
        now = time.monotonic()
        dgram = b"A\x01" + bytes([frm]) + pkt
        for k, o in self.members.items():
            if k == frm or o.gone or o.vtok is None:
                continue
            if udp is not None and o.uaddr is not None and not o.vtcp and now - o.useen < 20.0:
                udp.sendto(dgram, o.uaddr)
            elif o.backlog <= vcap:
                data = (6 + len(pkt)).to_bytes(2, "little") + bytes([self.VOICE]) + \
                       self.seq.to_bytes(4, "little") + bytes([frm]) + pkt
                o.backlog += len(data)
                o.q.put_nowait(data)
            else:
                self.vdropped += 1

    def control_quiet(self, text, only):
        """The relay's word to one member that takes no number of its own (the
        voice's answers), so everyone else's sequence runs exactly as before."""
        m = self.members.get(only)
        if m is None or m.gone:
            return
        payload = text.encode()
        data = (5 + len(payload)).to_bytes(2, "little") + bytes([self.CTRL]) + \
               self.seq.to_bytes(4, "little") + payload
        m.backlog += len(data)
        m.q.put_nowait(data)

    def control(self, text, cap, only=None, skip=None):
        payload = text.encode()
        data = self.frame(self.CTRL, payload)
        for k, m in sorted(self.members.items()):
            if (only is not None and k != only) or k == skip:
                continue
            self.send_to(m, data, cap)

    def drop(self, m, why):
        if m.gone:
            return
        m.gone = True
        m.q.put_nowait(None)                 # the writer closes the socket when it gets here
        if self.members.get(m.k) is m:
            del self.members[m.k]
        log(f"nroom {self.gid}/{self.code}: member {m.k} left ({why}), {len(self.members)} remain")
        if m.k >= self.WATCH_BASE:
            if not self.closed:
                self.control(f"UNWATCH {m.k}", 1 << 30, only=0)
            if not self.members:
                self.done.set()
            return
        if m.k == 0 and not self.closed:
            self.closed = True
            self.control("CLOSED", 1 << 30)
            for o in list(self.members.values()):
                o.gone = True
                o.q.put_nowait(None)
            self.members.clear()
        elif not self.closed:
            self.control(f"LEFT {m.k}", 1 << 30)
        if not self.members:
            self.done.set()

class ClubRoom(RoomN):
    """A CLUB (CueVR 7.1, Down the Club): a shared room of up to 16 people,
    each one a member like any other -- nobody hosts it, so nobody leaving
    closes it -- carrying presence (poses, names, voice) and a directory of
    its tables, and never a game: a table's game is an ordinary ROOMN room
    beside it. A member who falls behind loses frames, never its place: a
    pose a second late is simply not wanted. No START, no watchers."""
    WATCH_BASE = 64                 # no place is a watcher's
    MAXN = 16
    def __init__(self, gid, n):
        super().__init__(gid, f"CLUB{n}", True, self.MAXN, f"CLUB {n}")
        self.n = n
        self.club = True
        self.started = True         # frames flow from the first member on
        self.names = {}             # k -> display name
        self.tables = {}            # slot -> (k, text): who set the table up, and what it is
        self.dropped = 0            # frames not queued to a backed-up member

    def send_to(self, m, data, cap):
        if m.gone:
            return
        if m.backlog > cap:
            self.dropped += 1       # behind: this one is not sent, the member stays
            return
        m.backlog += len(data)
        m.q.put_nowait(data)

    def drop(self, m, why):
        if m.gone:
            return
        m.gone = True
        m.q.put_nowait(None)
        if self.members.get(m.k) is m:
            del self.members[m.k]
        self.names.pop(m.k, None)
        log(f"club {self.gid}/{self.n}: member {m.k} left ({why}), {len(self.members)} remain")
        # the tables they set up go back to free
        for slot in [s for s, (k, _) in self.tables.items() if k == m.k]:
            del self.tables[slot]
            self.control(f"TABLE {slot} -", 1 << 30)
        self.control(f"LEFT {m.k}", 1 << 30)
        if not self.members:
            self.closed = True
            self.done.set()

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
        # the gallery: per game, newest last on disk, "time\tcode\tkind\tname"
        self.gal = {}          # gid -> [(time, code, kind, name)]
        self.gal_codes = set() # "gid/CODE" already listed
        self.hidden = {}       # path -> (mtime, set of codes)
        for gid in (os.listdir(root) if os.path.isdir(root) else []):
            gp = os.path.join(root, gid, "gallery.tsv")
            if not os.path.isfile(gp):
                continue
            for ln in open(gp, encoding="ascii", errors="ignore"):
                t = ln.rstrip("\n").split("\t")
                if len(t) == 4 and t[0].isdigit():
                    self.gal.setdefault(gid, []).append((int(t[0]), t[1], t[2], t[3]))
                    self.gal_codes.add(gid + "/" + t[1])
        # likes: "gid/CODE" -> set of device ids, from <gid>/likes.tsv, an
        # append-only log of "time\tcode\tdevice\t+1|-1" replayed at start
        self.likes = {}
        self.like_rate = {}    # ip -> [times]
        for gid in (os.listdir(root) if os.path.isdir(root) else []):
            lp = os.path.join(root, gid, "likes.tsv")
            if not os.path.isfile(lp):
                continue
            for ln in open(lp, encoding="ascii", errors="ignore"):
                t = ln.rstrip("\n").split("\t")
                if len(t) != 4:
                    continue
                st = self.likes.setdefault(gid + "/" + t[1], set())
                if t[3] == "+1": st.add(t[2])
                else: st.discard(t[2])
        log(f"share store {root}: {len(self.codes)} codes, "
            f"{sum(len(v) for v in self.gal.values())} in galleries")

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

    def hidden_codes(self, gid):
        """The operator's take-down lists, global and this game's, re-read
        whenever either file changes."""
        out = set()
        for p in (os.path.join(self.root, "hidden.txt"), os.path.join(self.root, gid, "hidden.txt")):
            try:
                mt = os.path.getmtime(p)
            except OSError:
                continue
            have = self.hidden.get(p)
            if not have or have[0] != mt:
                codes = set()
                for ln in open(p, encoding="ascii", errors="ignore"):
                    c = clean_code(ln.split("#")[0].strip())
                    if c:
                        codes.add(c)
                have = (mt, codes); self.hidden[p] = have
            out |= have[1]
        return out

    def publish(self, gid, code, name, pic=b""):
        """List a code in its game's gallery, with its picture. None if there
        is no such code."""
        got = self.get(gid, code)
        if got is None:
            return None
        key = gid + "/" + code
        d = os.path.join(self.root, gid)
        if pic and not os.path.isfile(os.path.join(d, code + ".thumb")):
            tmp = os.path.join(d, code + ".thumb.tmp")
            with open(tmp, "wb") as f:
                f.write(pic)
            os.replace(tmp, os.path.join(d, code + ".thumb"))
        if key in self.gal_codes:
            return True                          # listed already, under its first name
        kind = got[0]
        ent = (int(time.time()), code, kind, name)
        d = os.path.join(self.root, gid)
        with open(os.path.join(d, "gallery.tsv"), "a", encoding="ascii") as f:
            f.write(f"{ent[0]}\t{code}\t{kind}\t{name}\n")
        self.gal.setdefault(gid, []).append(ent)
        self.gal_codes.add(key)
        return True

    def thumb(self, gid, code):
        if gid + "/" + code not in self.gal_codes:
            return None
        try:
            return open(os.path.join(self.root, gid, code + ".thumb"), "rb").read()
        except OSError:
            return None

    def ok_like_rate(self, ip, per_hour=300):
        now = time.monotonic()
        q = [t for t in self.like_rate.get(ip, []) if now - t < 3600]
        if len(q) >= per_hour:
            self.like_rate[ip] = q
            return False
        q.append(now); self.like_rate[ip] = q
        if len(self.like_rate) > 10000:
            self.like_rate = {k: v for k, v in self.like_rate.items() if v and now - v[-1] < 3600}
        return True

    def like(self, gid, code, dev):
        """Toggle one device's like of a listed entry: (count, mine), or None."""
        key = gid + "/" + code
        if key not in self.gal_codes:
            return None
        st = self.likes.setdefault(key, set())
        on = dev not in st
        if on: st.add(dev)
        else: st.discard(dev)
        with open(os.path.join(self.root, gid, "likes.tsv"), "a", encoding="ascii") as f:
            f.write(f"{int(time.time())}\t{code}\t{dev}\t{'+1' if on else '-1'}\n")
        return len(st), 1 if on else 0

    def gallery(self, gid, kind, start, count, top=False, dev=""):
        hide = self.hidden_codes(gid)
        items = [e for e in reversed(self.gal.get(gid, [])) if e[2] == kind and e[1] not in hide]
        def nl(e): return len(self.likes.get(gid + "/" + e[1], ()))
        if top:
            items.sort(key=lambda e: (-nl(e), -e[0]))
        page = [(e[0], e[1], e[2], e[3], nl(e),
                 1 if dev and dev in self.likes.get(gid + "/" + e[1], ()) else 0)
                for e in items[start:start + count]]
        return len(items), page

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

def parse_gid(v):
    """a game id as a release names it: 'CUEn', 0x4355456E or a number"""
    v = (v or "").strip()
    if not v or v.startswith("#"):
        return 0
    if len(v) == 4 and not v.isdigit():
        return int.from_bytes(v.encode("ascii", "ignore"), "big")
    try:
        return int(v, 0)
    except ValueError:
        return 0


class Relay:
    def __init__(self, args):
        self.args = args
        self.rooms = {}            # "gid/CODE" -> Room  (a host waiting for a partner)
        self.nrooms = {}           # "gid/CODE" -> RoomN (a hub of up to eight)
        self.conns = 0
        self.store = Store(args.store, args.store_cap, args.puts_per_hour) if args.store else None
        # THE META KEY (7.3): the app's secret, read once from its file and
        # never logged; with it a player's proof of their account is checked
        # with Meta and their results are kept against that account
        self.meta_token = None
        if getattr(args, "meta_secret_file", ""):
            try:
                sec = open(args.meta_secret_file).read().strip()
                if sec:
                    self.meta_token = f"OC|{args.meta_app_id}|{sec}"
                    log(f"meta: proofs checked for app {args.meta_app_id}")
            except OSError as e:
                log(f"meta: the secret file could not be read ({type(e).__name__}); proofs off")
        self.latest_args = [x for x in (parse_gid(v) for v in (getattr(args, "latest_game", None) or [])) if x]
        self.latest_file = (None, [])  # (mtime, ids) of <store>/latest_games
        self.vtokens = {}          # voice token (8 bytes) -> (room, member)
        self.udp = None            # the voice's datagram transport, once bound
        self.report_rate = {}      # ip -> [times]
        self.result_rate = {}      # ip -> [times] (RESULT, 7.3)

    # ---- VOICE ------------------------------------------------------------
    def voice_ctrl(self, room, m, cmd, raw):
        """A member's VOICE... to the relay. Answered to that member alone."""
        if self.args.no_voice or self.udp is None:
            return                              # an old relay says nothing either
        if cmd == "VOICE":
            if m.vtok is None:
                tok = os.urandom(8)
                while tok in self.vtokens:
                    tok = os.urandom(8)
                m.vtok = tok
                self.vtokens[tok] = (room, m)
            port = self.args.voice_port or self.args.port
            room.control_quiet(f"VOICE {m.vtok.hex().upper()} {port}", m.k)
            log(f"nroom {room.gid}/{room.code}: member {m.k} has voice")
        elif cmd == "VOICE TCP":
            m.vtcp = True
            log(f"nroom {room.gid}/{room.code}: member {m.k} voice over TCP")
        elif cmd == "VOICE UDP":
            m.vtcp = False

    def voice_forget(self, m):
        if m.vtok is not None and self.vtokens.get(m.vtok, (None, None))[1] is m:
            del self.vtokens[m.vtok]
        m.vtok = None

    def voice_allow(self, m):
        """Fifty packets a second is one talker; a hundred is the cap."""
        now = time.monotonic()
        if now - m.vwin >= 1.0:
            m.vwin, m.vcount = now, 0
        m.vcount += 1
        return m.vcount <= 100

    def datagram(self, data, addr):
        if len(data) < 10 or len(data) > 512 or data[1] != 1:
            return
        got = self.vtokens.get(bytes(data[2:10]))
        if got is None:
            return
        room, m = got
        if m.gone or room.closed:
            self.voice_forget(m)
            return
        m.uaddr, m.useen = addr, time.monotonic()
        kind = data[0]
        if kind == 0x4B and len(data) >= 18:           # 'K': keepalive, answered
            if self.voice_allow(m):
                self.udp.sendto(b"P\x01" + bytes(data[10:18]), addr)
        elif kind == 0x41 and len(data) > 10 + 11:     # 'A': a voice packet
            if self.voice_allow(m):
                room.voice(m.k, bytes(data[10:]), self.udp, self.args.voice_backlog)

    # ---- REPORTS (VRC.Content.3) ---------------------------------------------
    def report_salt(self):
        """THE RELAY'S OWN SECRET, kept beside the reports: an address is never
        written down, only a keyed hash of it, so repeat reports about one
        connection can be matched without the address being kept. Made once,
        readable by the relay alone, and the same across restarts."""
        if getattr(self, "_salt", None):
            return self._salt
        p = os.path.join(self.args.store, "reports.salt")
        try:
            with open(p, "rb") as f:
                salt = f.read()
        except OSError:
            salt = b""
        if len(salt) < 16:
            salt = os.urandom(32)
            fd = os.open(p + ".tmp", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "wb") as f:
                f.write(salt)
            os.replace(p + ".tmp", p)
        self._salt = salt
        return salt

    def addr_hash(self, peer):
        ip = peer[0] if isinstance(peer, tuple) else str(peer)
        return hmac.new(self.report_salt(), ip.encode(), hashlib.sha256).hexdigest()[:16]

    # ---- RESULTS (CueVR 7.3) --------------------------------------------
    # The facts both ends must agree on: everything about the frame, nothing
    # one end alone measures (its own clock, which side is "me", its own
    # clearance, its build).
    RES_SHARED = ("t", "game", "variant", "kind", "mode", "best_of", "frame",
                  "score", "frames", "winner", "match_winner", "bnr", "golden")
    RES_SIDE = ("shots", "pot_shots", "potted", "fouls", "best", "breaks")

    def result_check(self, r):
        """the checks a result must pass to be kept at all"""
        if not isinstance(r, dict) or r.get("v") != 1 or r.get("t") not in ("frame", "match"):
            return False
        def num(x, lo=-100000, hi=100000):
            return isinstance(x, (int, float)) and not isinstance(x, bool) and lo <= x <= hi
        for k in ("kind", "mode", "best_of", "frame", "winner", "match_winner", "bnr", "golden", "me"):
            if not num(r.get(k), -1, 1000):
                return False
        for k in ("score", "frames"):
            v = r.get(k)
            if not (isinstance(v, list) and len(v) == 2 and all(num(x) for x in v)):
                return False
        bo = r["best_of"]
        if bo < 1 or any(f < 0 or f > (bo + 1) // 2 for f in r["frames"]) or r["frame"] > bo:
            return False
        sides = r.get("sides")
        if not (isinstance(sides, list) and len(sides) == 2):
            return False
        snooker = "SNOOKER" in str(r.get("game", "")).upper()
        for sd in sides:
            if not isinstance(sd, dict) or not all(num(sd.get(k), 0) for k in ("shots", "pot_shots", "potted", "fouls", "best")):
                return False
            b = sd.get("breaks")
            if not (isinstance(b, list) and len(b) <= 64 and all(num(x, 0) for x in b)):
                return False
            if snooker and (sd["best"] > 155 or any(x > 155 for x in b)):
                return False
        return True

    def result_key(self, r):
        shared = {k: r.get(k) for k in self.RES_SHARED}
        shared["sides"] = [{k: sd.get(k) for k in self.RES_SIDE} for sd in r["sides"]]
        return json.dumps(shared, sort_keys=True)

    def result_write(self, name, line):
        try:
            os.makedirs(self.args.store, exist_ok=True)
            with open(os.path.join(self.args.store, name), "a", encoding="ascii", errors="replace") as f:
                f.write(json.dumps(line, sort_keys=True) + "\n")
            return True
        except OSError as e:
            log(f"results: {name} could not be written ({type(e).__name__})")
            return False

    def result(self, room, m, raw):
        """RESULT <json>: one end's account of a frame (or the match). Kept as
        a result when the other side's player says the same (see RESULTS)."""
        if self.store is None:
            return "RESULT OFF"
        os.makedirs(self.args.store, exist_ok=True)      # before the salt is made in it
        ip = m.peer[0] if isinstance(m.peer, tuple) else str(m.peer)
        now = time.monotonic()
        q = [t for t in self.result_rate.get(ip, []) if now - t < 3600]
        if len(q) >= 600:
            self.result_rate[ip] = q
            return "RESULT BUSY"
        q.append(now); self.result_rate[ip] = q
        body = raw[7:] if raw.startswith("RESULT ") else ""
        if len(body) > 4000 or any(not (32 <= ord(c) < 127) for c in body):
            return "RESULT BAD"
        try:
            r = json.loads(body)
        except ValueError:
            return "RESULT BAD"
        if not self.result_check(r) or len(room.players()) < 2 or m.k >= room.WATCH_BASE:
            return "RESULT BAD"
        side = r["me"]
        entry = {"k": m.k, "side": side, "addr": self.addr_hash(m.peer), "uid": m.uid, "r": r}
        key = (r["t"], r["frame"])
        pend = room.results.setdefault(key, [])
        for o in pend:
            if o["side"] != side and o["k"] != m.k:
                pend.remove(o)
                same = self.result_key(o["r"]) == self.result_key(r)
                line = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "gid": room.gid, "room": room.code,
                        "room_secs": round(now - room.created), "players": len(room.players()),
                        "agreed": same, "a": o, "b": entry,
                        # both players' Meta accounts proved: the result is theirs, not a name's
                        "verified": bool(o.get("uid")) and bool(entry.get("uid")) and o.get("uid") != entry.get("uid")}
                if same:
                    line["result"] = {k: r.get(k) for k in self.RES_SHARED}
                    line["result"]["names"] = r.get("names")
                    line["result"]["sides"] = [{k: sd.get(k) for k in self.RES_SIDE} for sd in r["sides"]]
                self.result_write("results.jsonl" if same else "results_disputed.jsonl", line)
                log(f"nroom {room.gid}/{room.code}: {r['t']} {r['frame']} {'agreed' if same else 'DISPUTED'}")
                return "RESULT KEPT" if same else "RESULT DISPUTED"
        if len(pend) < 8:
            pend.append(entry)
        return "RESULT HELD"

    def results_flush(self, room):
        """a room closing: whatever was never confirmed, kept apart as such"""
        for key, pend in list(getattr(room, "results", {}).items()):
            for o in pend:
                self.result_write("results_unconfirmed.jsonl",
                                  {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "gid": room.gid, "room": room.code,
                                   "room_secs": round(time.monotonic() - room.created), "a": o})
        if getattr(room, "results", None):
            room.results.clear()

    # ---- WHO IS PLAYING, PROVED (7.3) ---------------------------------------
    def meta_check(self, uid, nonce):
        """Meta's answer to a proof: is this nonce a good one for this account?
        (blocking; run off the loop)"""
        q = urllib.parse.urlencode({"nonce": nonce, "user_id": uid, "access_token": self.meta_token}).encode()
        req = urllib.request.Request(self.args.meta_verify_url, data=q, method="POST")
        with urllib.request.urlopen(req, timeout=8) as r:
            return bool(json.loads(r.read().decode("utf-8", "replace")).get("is_valid"))

    async def proof(self, room, m, raw):
        """PROOF <ACCOUNT ID> <NONCE>: Meta asked whether the nonce proves the
        account; if so the member's results carry it. Answered PROOF OK / PROOF
        BAD / PROOF OFF (no key on this relay) / PROOF BUSY (more than 5)."""
        if not self.meta_token:
            return "PROOF OFF"
        if m.proofs >= 5:
            return "PROOF BUSY"
        m.proofs += 1
        t = raw.split()
        if len(t) != 3 or not t[1].isdigit() or len(t[1]) > 20 or not (8 <= len(t[2]) <= 256) or not t[2].isalnum():
            return "PROOF BAD"
        try:
            ok = await asyncio.get_running_loop().run_in_executor(None, self.meta_check, t[1], t[2])
        except Exception as e:
            log(f"nroom {room.gid}/{room.code}: member {m.k}'s proof could not be checked ({type(e).__name__})")
            return "PROOF OFF"
        if ok:
            m.uid = t[1]
        log(f"nroom {room.gid}/{room.code}: member {m.k} proof {'good' if ok else 'BAD'}")
        return "PROOF OK" if ok else "PROOF BAD"

    def report(self, room, m, raw):
        """REPORT <K> <REASON> <REPORTED NAME>|<REPORTER NAME>: one line in
        <store>/reports.tsv -- the time, the room, the reason, both display
        names, both places, and a salted hash of each address (never the
        address). Kept no longer than 90 days after it is dealt with: that is
        the operator's to do, by deleting the line."""
        if self.store is None:
            return "REPORT OFF"
        ip = m.peer[0] if isinstance(m.peer, tuple) else str(m.peer)
        now = time.monotonic()
        q = [t for t in self.report_rate.get(ip, []) if now - t < 3600]
        if len(q) >= 10:
            self.report_rate[ip] = q
            return "REPORT BUSY"
        q.append(now); self.report_rate[ip] = q
        t = raw.split(None, 3)
        try:
            k = int(t[1]) if len(t) > 1 else -1
        except ValueError:
            k = -1
        reason = clean_code(t[2]) if len(t) > 2 else "OTHER"
        text = "".join(c for c in (t[3] if len(t) > 3 else "") if 32 <= ord(c) < 127 and c != "\t")
        names = (text.split("|", 1) + [""])[:2]
        reported_name, reporter_name = names[0].strip()[:40] or "-", names[1].strip()[:40] or "-"
        who = room.members.get(k)
        # A REPORT NEVER COSTS ITS SENDER THE ROOM: a store that cannot be
        # written says so in the answer, and the room carries on
        try:
            os.makedirs(self.args.store, exist_ok=True)
            line = "\t".join([time.strftime("%Y-%m-%d %H:%M:%S"), room.gid, room.code, reason or "OTHER",
                              reported_name, reporter_name, str(k), str(m.k),
                              self.addr_hash(who.peer) if who else "-", self.addr_hash(m.peer)]) + "\n"
            with open(os.path.join(self.args.store, "reports.tsv"), "a", encoding="ascii", errors="replace") as f:
                f.write(line)
        except OSError as e:
            log(f"nroom {room.gid}/{room.code}: a report could not be kept ({type(e).__name__})")
            return "REPORT OFF"
        log(f"nroom {room.gid}/{room.code}: member {m.k} reported member {k} ({reason})")
        return "REPORTED"

    def gen_code(self, gid) -> str:
        alpha = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"   # no confusable 0/O/1/I
        for _ in range(20):
            c = "".join(random.choice(alpha) for _ in range(4))
            if rkey(gid, c) not in self.rooms and rkey(gid, c) not in self.nrooms:
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

    async def member_writer(self, room, m):
        try:
            while True:
                data = await m.q.get()
                if data is None:
                    break
                m.backlog -= len(data)
                m.writer.write(data)
                await m.writer.drain()
        except (OSError, ConnectionError):
            room.drop(m, "write error")
        finally:
            try:
                m.writer.close()
            except OSError:
                pass

    async def member_reader(self, room, m):
        """Read one member's frames until it goes; forward each as it lands."""
        cap = self.args.room_backlog
        why = "closed"
        try:
            while not m.gone:
                hdr = await asyncio.wait_for(m.reader.readexactly(2), timeout=self.args.idle)
                n = int.from_bytes(hdr, "little")
                if n < 1:
                    why = "empty frame"; break
                body = await asyncio.wait_for(m.reader.readexactly(n), timeout=self.args.idle)
                if m.gone or room.closed:
                    break
                to, payload = body[0], body[1:]
                if to == RoomN.CTRL:
                    raw = payload.decode("ascii", "ignore").strip()
                    cmd = raw.upper()
                    if cmd == "START" and m.k == 0 and not room.started:
                        room.started = True
                        log(f"nroom {room.gid}/{room.code}: started with {len(room.members)}")
                        room.control("START", cap)
                    elif cmd == "PLAYING" and m.k == 0 and not room.playing:
                        # an OPEN room under way (CueVR's winner stays on): still
                        # open to JOIN, and now listed LIVE as well, to WATCH
                        room.playing = True
                        log(f"nroom {room.gid}/{room.code}: playing, open, with {len(room.members)}")
                    elif getattr(room, "club", False) and cmd.startswith("NAME "):
                        nm = clean_label(raw[5:])[:24] or "PLAYER"
                        room.names[m.k] = nm
                        room.control(f"NAME {m.k} {nm}", cap)
                    elif getattr(room, "club", False) and cmd.startswith("TABLE "):
                        # TABLE <slot> <text...> | TABLE <slot> -: a table set
                        # up (or freed) by this member, for everyone in the club
                        parts = raw.split(None, 2)
                        try:
                            slot = int(parts[1])
                        except (IndexError, ValueError):
                            slot = -1
                        if 0 <= slot < 16:
                            text = parts[2] if len(parts) > 2 else "-"
                            held = room.tables.get(slot)
                            if text == "-":
                                if held and held[0] == m.k:
                                    del room.tables[slot]
                                    room.control(f"TABLE {slot} -", cap)
                            elif held is None or held[0] == m.k:
                                text = "".join(c for c in text if 32 <= ord(c) < 127)[:160]
                                room.tables[slot] = (m.k, text)
                                room.control(f"TABLE {slot} {m.k} {text}", cap)
                    elif cmd.startswith("VOICE"):
                        self.voice_ctrl(room, m, cmd, raw)
                    elif cmd.startswith("REPORT "):
                        room.control_quiet(self.report(room, m, raw), m.k)
                    elif cmd.startswith("PROOF ") and not getattr(room, "club", False):
                        # beside the game, never in its way: Meta may take seconds to answer
                        async def check(room=room, m=m, raw=raw):
                            try:
                                ans = await self.proof(room, m, raw)
                            except Exception as e:
                                log(f"nroom {room.gid}/{room.code}: a proof failed ({type(e).__name__})")
                                ans = "PROOF OFF"
                            if not m.gone:
                                room.control_quiet(ans, m.k)
                        asyncio.ensure_future(check())
                    elif cmd.startswith("RESULT ") and not getattr(room, "club", False):
                        # A RESULT NEVER COSTS ITS SENDER THE ROOM, whatever goes wrong keeping it
                        try:
                            ans = self.result(room, m, raw)
                        except Exception as e:
                            log(f"nroom {room.gid}/{room.code}: a result could not be kept ({type(e).__name__}: {e})")
                            ans = "RESULT OFF"
                        room.control_quiet(ans, m.k)
                    continue
                if to == RoomN.VOICE:
                    # voice down the TCP, from a member whose UDP does not get out
                    if m.vtok is not None and not self.args.no_voice and self.voice_allow(m):
                        room.voice(m.k, payload, self.udp, self.args.voice_backlog)
                    continue
                room.deliver(m.k, to, payload, cap)
        except asyncio.TimeoutError:
            why = f"idle >{self.args.idle}s"
        except asyncio.IncompleteReadError:
            why = "peer EOF"
        except (OSError, ConnectionError) as e:
            why = f"net {type(e).__name__}"
        room.drop(m, why)

    async def nroom_member(self, key, room, k, reader, writer, peer):
        m = Member(k, reader, writer, peer)
        others = sorted(room.members)
        room.members[k] = m
        writer.write(f"SEAT {k} {room.maxn}\n".encode())
        m.task = asyncio.create_task(self.member_writer(room, m))
        cap = self.args.room_backlog
        if k >= RoomN.WATCH_BASE:
            # a WATCHER: who is playing (and itself), and the host is told --
            # by a word a client from before watchers does not know, so it
            # never takes one for a player
            room.control("MEMBERS " + " ".join(str(x) for x in sorted(room.players())) + f" {k}", cap, only=k)
            room.control(f"WATCHER {k}", cap, only=0)
        else:
            room.control("MEMBERS " + " ".join(str(x) for x in sorted(room.players())), cap, only=k)
            if others:
                room.control(f"JOINED {k}", cap, skip=k)
        log(f"nroom {key}: member {k} in ({peer}), {len(room.members)}/{room.maxn}")
        await self.member_reader(room, m)
        self.voice_forget(m)
        await asyncio.gather(m.task, return_exceptions=True)
        if room.done.is_set() and self.nrooms.get(key) is room:
            self.nrooms.pop(key, None)
            self.results_flush(room)
            log(f"nroom {key}: closed")

    async def club_member(self, key, club, k, reader, writer, peer):
        m = Member(k, reader, writer, peer)
        others = sorted(club.members)
        club.members[k] = m
        writer.write(f"SEAT {k} {club.maxn} {club.n}{' NEWER' if self.newer(club.gid) else ''}\n".encode())
        m.task = asyncio.create_task(self.member_writer(club, m))
        cap = self.args.room_backlog
        # who is here, what they are called and which tables are set up
        club.control("MEMBERS " + " ".join(str(x) for x in sorted(club.members)), cap, only=k)
        for o, nm in sorted(club.names.items()):
            club.control(f"NAME {o} {nm}", cap, only=k)
        for slot, (o, text) in sorted(club.tables.items()):
            club.control(f"TABLE {slot} {o} {text}", cap, only=k)
        if others:
            club.control(f"JOINED {k}", cap, skip=k)
        log(f"club {key}: member {k} in ({peer}), {len(club.members)}/{club.maxn}")
        await self.member_reader(club, m)
        self.voice_forget(m)
        await asyncio.gather(m.task, return_exceptions=True)
        if club.done.is_set() and self.nrooms.get(key) is club:
            self.nrooms.pop(key, None)
            log(f"club {key}: closed")

    def newer(self, gid):
        """a newer version of this game is current: same family, higher id.
        Never the reason a join fails: any trouble here is "not newer"."""
        try:
            gid = int(gid)
        except (TypeError, ValueError):
            return False
        ids = list(self.latest_args)
        if self.args.store:
            p = os.path.join(self.args.store, "latest_games")
            try:
                mt = os.stat(p).st_mtime
                if mt != self.latest_file[0]:
                    with open(p, encoding="ascii", errors="ignore") as f:
                        self.latest_file = (mt, [x for x in (parse_gid(l) for l in f) if x])
                ids += self.latest_file[1]
            except OSError:
                self.latest_file = (None, [])
        return any((L >> 8) == (gid >> 8) and gid < L for L in ids)

    async def handle_club(self, gid, a, reader, writer, peer):
        """CLUB <GAMEID> JOIN [N]: a place in club N, or with no N the lowest
        numbered club with room, a new one opened when every club is full."""
        sub = a[0].upper() if a else ""
        if sub != "JOIN":
            writer.write(b"ERR\n"); await writer.drain(); return
        want = 0
        if len(a) > 1:
            try:
                want = int(a[1])
            except ValueError:
                want = -1
        if want < 0 or want > 99:
            writer.write(b"ERR\n"); await writer.drain(); return
        n = want
        if n == 0:
            n = 1
            while True:
                c = self.nrooms.get(rkey(gid, f"CLUB{n}"))
                if c is None or (not c.closed and c.free_place() >= 0):
                    break
                n += 1
        key = rkey(gid, f"CLUB{n}")
        club = self.nrooms.get(key)
        if club is None or club.closed:
            club = ClubRoom(gid, n)
            self.nrooms[key] = club
            log(f"club {key}: opened ({peer})")
        k = club.free_place()
        if k < 0:
            writer.write(b"FULL\n"); await writer.drain(); return
        await self.club_member(key, club, k, reader, writer, peer)

    async def handle_roomn(self, gid, a, reader, writer, peer):
        sub = a[0].upper() if a else ""
        if sub == "HOST":                   # HOST <CODE> <PUB|PRIV> <MAX> [LABEL...]
            code = clean_code(a[1]) if len(a) > 1 else ""
            try:
                maxn = int(a[3]) if len(a) > 3 else 0
            except ValueError:
                maxn = 0
            if not code or not 2 <= maxn <= 8:
                writer.write(b"ERR\n"); await writer.drain(); return
            key = rkey(gid, code)
            if key in self.rooms or key in self.nrooms:
                writer.write(b"TAKEN\n"); await writer.drain(); return
            public = a[2].upper() == "PUB"
            label = clean_label(" ".join(a[4:])) if len(a) > 4 else "GAME"
            room = RoomN(gid, code, public, maxn, label)
            self.nrooms[key] = room
            log(f"nroom {key}: hosted, {maxn} places {'(public)' if public else '(private)'} ({peer})")
            await self.nroom_member(key, room, 0, reader, writer, peer)
            return
        if sub == "JOIN":                   # JOIN <CODE>
            code = clean_code(a[1]) if len(a) > 1 else ""
            key = rkey(gid, code)
            room = self.nrooms.get(key) if code else None
            if room is None or room.closed:
                writer.write(b"NONE\n"); await writer.drain(); return
            if room.started:
                writer.write(b"BUSY\n"); await writer.drain(); return
            k = room.free_place()
            if k < 0:
                writer.write(b"FULL\n"); await writer.drain(); return
            await self.nroom_member(key, room, k, reader, writer, peer)
            return
        if sub == "WATCH":                  # WATCH <CODE>: a room being played, to watch
            code = clean_code(a[1]) if len(a) > 1 else ""
            key = rkey(gid, code)
            room = self.nrooms.get(key) if code else None
            if room is None or room.closed or 0 not in room.members:
                writer.write(b"NONE\n"); await writer.drain(); return
            k = room.free_watch()
            if k < 0:
                writer.write(b"FULL\n"); await writer.drain(); return
            await self.nroom_member(key, room, k, reader, writer, peer)
            return
        if sub == "QUICK":                  # QUICK <MAX> [LABEL...]
            try:
                maxn = int(a[1]) if len(a) > 1 else 0
            except ValueError:
                maxn = 0
            if not 2 <= maxn <= 8:
                writer.write(b"ERR\n"); await writer.drain(); return
            best = None
            for key, r in self.nrooms.items():
                if (r.public and r.gid == gid and r.maxn == maxn and not r.started
                        and not r.closed and r.free_place() >= 0
                        and (best is None or r.created < self.nrooms[best].created)):
                    best = key
            if best is not None:
                room = self.nrooms[best]
                await self.nroom_member(best, room, room.free_place(), reader, writer, peer)
                return
            code = self.gen_code(gid)
            key = rkey(gid, code)
            label = clean_label(" ".join(a[2:])) if len(a) > 2 else "QUICK"
            room = RoomN(gid, code, True, maxn, label)
            self.nrooms[key] = room
            log(f"nroom {key}: quick-hosted, {maxn} places ({peer})")
            await self.nroom_member(key, room, 0, reader, writer, peer)
            return
        writer.write(b"ERR\n"); await writer.drain()

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

            if verb in ("PUB", "GALLERY", "THUMB", "LIKE") and t[0] == "MOTE2":
                sg = clean_gid(gid)
                if self.store is None or not sg:
                    writer.write(b"OFF\n" if self.store is None else b"ERR\n"); await writer.drain(); return
                if verb == "LIKE":              # LIKE <CODE> <DEVICE>
                    code = clean_code(a[0]) if len(a) > 0 else ""
                    dev = "".join(c for c in (a[1] if len(a) > 1 else "") if c.isalnum())[:32]
                    ip = peer[0] if isinstance(peer, tuple) else str(peer)
                    if not code or len(dev) < 8:
                        writer.write(b"ERR\n"); await writer.drain(); return
                    if not self.store.ok_like_rate(ip):
                        writer.write(b"BUSY\n"); await writer.drain(); return
                    got = self.store.like(sg, code, dev)
                    writer.write((f"LIKES {got[0]} {got[1]}\n" if got else "NONE\n").encode()); await writer.drain()
                    return
                if verb == "THUMB":             # THUMB <CODE>
                    code = clean_code(a[0]) if len(a) > 0 else ""
                    pic = self.store.thumb(sg, code) if code else None
                    if pic is None:
                        writer.write(b"NONE\n"); await writer.drain(); return
                    writer.write(f"DATA THUMB {len(pic)}\n".encode() + pic); await writer.drain()
                    return
                if verb == "PUB":               # PUB <CODE> <PICLEN> <NAME...>, then the picture
                    code = clean_code(a[0]) if len(a) > 0 else ""
                    try:
                        plen = int(a[1]) if len(a) > 1 else -1
                    except ValueError:
                        plen = -1
                    name = clean_name(" ".join(a[2:])) if len(a) > 2 else "UNTITLED"
                    ip = peer[0] if isinstance(peer, tuple) else str(peer)
                    if not code or plen < 0 or plen > 40000:
                        writer.write(b"ERR\n"); await writer.drain(); return
                    if not self.store.ok_rate(ip):
                        writer.write(b"BUSY\n"); await writer.drain(); return
                    pic = await asyncio.wait_for(reader.readexactly(plen), timeout=15) if plen else b""
                    ok = self.store.publish(sg, code, name, pic)
                    writer.write(b"OK\n" if ok else b"NONE\n"); await writer.drain()
                    log(f"gallery {sg}: publish {code} '{name}' -> {'ok' if ok else 'none'} ({peer})")
                    return
                kind = clean_code(a[0]) if len(a) > 0 else ""
                try:
                    start = max(0, int(a[1])) if len(a) > 1 else 0
                    count = min(50, max(1, int(a[2]))) if len(a) > 2 else 20
                except ValueError:
                    start, count = 0, 20
                if not kind:
                    writer.write(b"ERR\n"); await writer.drain(); return
                top = len(a) > 3 and a[3].upper() == "TOP"
                dev = "".join(c for c in (a[4] if len(a) > 4 else "") if c.isalnum())[:32]
                total, items = self.store.gallery(sg, kind, start, count, top, dev)
                out = bytearray()
                for tm, code, _k, name, nlike, mine in items:
                    out += f"ITEM {code} {tm} {nlike} {mine} {name}\n".encode()
                out += f"END {total}\n".encode()
                writer.write(out); await writer.drain()
                return

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

            if verb == "CLUB" and t[0] == "MOTE2":
                await self.handle_club(gid, a, reader, writer, peer)
                return
            if verb == "ROOMN" and t[0] == "MOTE2":
                await self.handle_roomn(gid, a, reader, writer, peer)
                return

            if verb == "LIST":                  # browse this game's open public rooms
                out = bytearray()
                for k, r in list(self.rooms.items()):
                    if r.public and r.gid == gid:
                        out += f"ROOM {r.code} {r.label}\n".encode()
                        if len(out) > 3500: break
                for k, r in list(self.nrooms.items()):
                    if len(out) > 3500: break
                    if getattr(r, "club", False):
                        # A CLUB (7.1): how many are in, how many tables are
                        # set up, and who -- lines a reader from before clubs
                        # skips, as it skips any it does not know
                        if r.gid == gid and not r.closed and r.members:
                            out += f"CLUB {r.n} {len(r.members)}/{r.maxn} {len(r.tables)}\n".encode()
                            for o, nm in sorted(r.names.items()):
                                out += f"CLUBWHO {r.n} {nm}\n".encode()
                        continue
                    if r.public and r.gid == gid and not r.started and not r.closed:
                        out += f"ROOM {r.code} {r.label} {len(r.players())}/{r.maxn}\n".encode()
                    # ...and games being played, to watch (6.3): a line a
                    # reader from before them skips, as it skips any it does
                    # not know. Public rooms only, started -- or open and being
                    # played (PLAYING), which is listed both ways.
                    if (r.public and r.gid == gid and not r.closed and 0 in r.members
                          and (r.started or r.playing)):
                        out += f"LIVE {r.code} {r.label} {len(r.players())}/{r.maxn} {len(r.watchers())}\n".encode()
                if self.newer(gid):
                    out += b"NEWER\n"           # (7.3) this version is out of date
                out += b"END\n"
                writer.write(out); await writer.drain()
                return

            if verb == "HOST":                  # HOST <CODE> <PUB|PRIV> [LABEL]
                code = clean_code(a[0]) if len(a) > 0 else ""
                if not code:
                    writer.write(b"ERR\n"); await writer.drain(); return
                key = rkey(gid, code)
                if key in self.rooms or key in self.nrooms:
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
    ap.add_argument("--room-backlog", type=int, default=262144, help="bytes a ROOMN member may have unsent before it is dropped")
    ap.add_argument("--store", default="", help="directory for share codes (PUT/GET); empty = share codes off")
    ap.add_argument("--meta-secret-file", default="", help="file holding the Meta app secret: players' account proofs are checked and results kept against the account (7.3)")
    ap.add_argument("--meta-app-id", default="1036582382349280", help="the Meta app the proofs are for (CueVR)")
    ap.add_argument("--meta-verify-url", default="https://graph.oculus.com/user_nonce_validate", help="where proofs are checked (a test points it at a fake)")
    ap.add_argument("--latest-game", action="append", default=[], help="a current game id ('CUEn', 0x4355456E): older ids of its family are told to update (7.3); also <store>/latest_games")
    ap.add_argument("--store-cap", type=int, default=200000, help="most share codes kept")
    ap.add_argument("--puts-per-hour", type=int, default=60, help="share codes one address may make an hour")
    ap.add_argument("--voice-port", type=int, default=0, help="UDP port for room voice (0 = the same number as --port)")
    ap.add_argument("--no-voice", action="store_true", help="answer no VOICE request (clients then have no voice)")
    ap.add_argument("--voice-backlog", type=int, default=16384, help="bytes queued to a member past which voice to it over TCP is dropped")
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
    if not args.no_voice:
        class VoiceUDP(asyncio.DatagramProtocol):
            def connection_made(self, transport):
                relay.udp = transport
            def datagram_received(self, data, addr):
                relay.datagram(data, addr)
            def error_received(self, exc):
                pass
        try:
            await asyncio.get_event_loop().create_datagram_endpoint(
                VoiceUDP, local_addr=(args.addr, args.voice_port or args.port))
            log(f"voice: udp {args.addr}:{args.voice_port or args.port}")
        except OSError as e:
            log(f"voice: no udp port ({e}) -- rooms will have no voice")
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
