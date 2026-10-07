#!/usr/bin/env python3
"""
Bench for the relay's ROOMN hub, and a regression check that the old verbs
behave byte for byte as they did.

    python3 test_roomn.py [--old /path/to/old/mote_relay.py]

Starts its own relay(s) on free localhost ports -- never a live one. With
--old, the same old-verb scenarios run against the old relay too and the two
transcripts must be identical -- and a room in which one member talks (VOICE,
UDP and TCP) must look, to the two members that never asked, exactly as it
does on the old relay, which ignores all of it.
"""
import argparse
import os
import random
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = []

def check(name, cond, detail=""):
    print(f"  [{'ok ' if cond else 'FAIL'}] {name}{('  ' + detail) if detail else ''}")
    if not cond:
        FAILS.append(name)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close()
    return p

def start_relay(path, extra=()):
    port = free_port()
    p = subprocess.Popen([sys.executable, path, "--addr", "127.0.0.1", "--port", str(port),
                          "--idle", "30", *extra],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return p, port
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("relay did not start")

def conn(port, line, rcvbuf=None):
    s = socket.socket()
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.connect(("127.0.0.1", port))
    s.settimeout(5)
    s.sendall(line.encode() + b"\n")
    return s

def readline(s):
    b = bytearray()
    while True:
        c = s.recv(1)
        if not c or c == b"\n":
            return b.decode()
        b += c

def readn(s, n):
    b = bytearray()
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return bytes(b)

def send_frame(s, to, payload):
    s.sendall((1 + len(payload)).to_bytes(2, "little") + bytes([to]) + payload)

def recv_frame(s):
    n = int.from_bytes(readn(s, 2), "little")
    body = readn(s, n)
    return body[0], int.from_bytes(body[1:5], "little"), body[5:]

def recv_until_ctrl(s, want, limit=200):
    """frames up to and including a relay control frame starting with `want`"""
    got = []
    for _ in range(limit):
        f = recv_frame(s)
        got.append(f)
        if f[0] == 0xFE and f[2].decode().startswith(want):
            return got
    return got

GID = "43554566"

def roomn_tests(port):
    print("\n--- ROOMN: eight members, one order ---")
    h = conn(port, f"MOTE2 ROOMN {GID} HOST TEST PUB 8 BENCH ROOM")
    check("host gets place 0", readline(h) == "SEAT 0 8")
    check("host is told who is here", recv_frame(h)[2] == b"MEMBERS 0")
    js = []
    for k in range(1, 8):
        j = conn(port, f"MOTE2 ROOMN {GID} JOIN TEST")
        line = readline(j)
        check(f"joiner {k} gets place {k}", line == f"SEAT {k} 8", line)
        mem = recv_frame(j)
        check(f"joiner {k} told the members", mem[2].decode() == "MEMBERS " + " ".join(str(x) for x in range(k + 1)),
              mem[2].decode())
        js.append(j)
    ms = [h] + js
    # everyone already in hears each later JOINED
    for k, m in enumerate(ms):
        joined = []
        for _ in range(7 - max(k, 0) if k > 0 else 7):
            f = recv_frame(m)
            joined.append(f[2].decode())
        want = [f"JOINED {x}" for x in range(max(k, 0) + 1, 8)] if k > 0 else [f"JOINED {x}" for x in range(1, 8)]
        check(f"member {k} heard every later join", joined == want, str(joined))

    ls = conn(port, f"MOTE2 LIST {GID}")
    lst = b""
    while not lst.endswith(b"END\n"):
        c = ls.recv(4096)
        if not c: break
        lst += c
    check("LIST shows the room with its places", b"ROOM TEST BENCH ROOM 8/8\n" in lst, lst.decode().strip())
    ls.close()
    nine = conn(port, f"MOTE2 ROOMN {GID} JOIN TEST")
    check("a ninth is refused FULL", readline(nine) == "FULL")
    nine.close()

    # every member sends 40 broadcasts, interleaved from one thread in a random order
    rnd = random.Random(7)
    order = [k for k in range(8) for _ in range(40)]
    rnd.shuffle(order)
    count = [0] * 8
    for k in order:
        send_frame(ms[k], 0xFF, f"m{k}#{count[k]}".encode())
        count[k] += 1
    recv = [[] for _ in range(8)]
    for k, m in enumerate(ms):
        for _ in range(7 * 40):
            recv[k].append(recv_frame(m))
    ok_stamp = all(p.decode().startswith(f"m{f}#") for k in range(8) for f, _, p in recv[k])
    check("every frame is stamped with its real sender", ok_stamp)
    ok_self = all(f != k for k in range(8) for f, _, _ in recv[k])
    check("nobody hears their own broadcast", ok_self)
    ok_inc = all(all(a[1] < b[1] for a, b in zip(r, r[1:])) for r in recv)
    check("each member's sequence only goes up", ok_inc)
    # total order: every pair of members agrees on the relative order of what both got
    seqof = {}
    agree = True
    for k in range(8):
        for f, sq, p in recv[k]:
            if seqof.setdefault(p, sq) != sq:
                agree = False
    check("one frame has one number at every receiver", agree)
    ok_fifo = True
    for k in range(8):
        last = {}
        for f, sq, p in recv[k]:
            n = int(p.decode().split("#")[1])
            if last.get(f, -1) + 1 != n: ok_fifo = False
            last[f] = n
    check("each sender's frames arrive complete and in order", ok_fifo)

    print("\n--- ROOMN: to one member ---")
    send_frame(ms[3], 5, b"just for five")
    f = recv_frame(ms[5])
    check("member 5 gets it from 3", f[0] == 3 and f[2] == b"just for five")
    send_frame(ms[3], 3, b"to myself")          # dropped
    send_frame(ms[2], 0xFF, b"after")
    got = [recv_frame(ms[k])[2] for k in (0, 1, 3, 4, 5, 6, 7)]
    check("a frame to yourself goes nowhere; the next broadcast is next everywhere", all(g == b"after" for g in got))

    print("\n--- ROOMN: START ---")
    send_frame(js[0], 0xFE, b"START")            # not the host: ignored
    send_frame(h, 0xFE, b"START")
    starts = [recv_frame(m) for m in ms]
    check("only the host's START counts, and everyone hears it once",
          all(x[0] == 0xFE and x[2] == b"START" for x in starts), str([x[2] for x in starts]))
    ls = conn(port, f"MOTE2 LIST {GID}")
    lst = b""
    while not lst.endswith(b"END\n"):
        c = ls.recv(4096)
        if not c: break
        lst += c
    check("a started room is off LIST (no ROOM line)", b"ROOM TEST" not in lst)
    check("...and listed to watch instead (LIVE, which old readers skip)", b"LIVE TEST " in lst, lst.decode())

    print("\n--- ROOMN: watchers (6.3) ---")
    w = conn(port, f"MOTE2 ROOMN {GID} WATCH TEST")
    seat = readline(w)
    check("a started room takes a watcher, at place 8", seat.startswith("SEAT 8 "), seat)
    mem = recv_frame(w)
    check("the watcher hears who plays (and itself)", mem[2].startswith(b"MEMBERS ") and b" 8" in mem[2], mem[2].decode())
    hw = recv_frame(ms[0])
    check("the host alone hears WATCHER 8", hw[0] == 0xFE and hw[2] == b"WATCHER 8", hw[2].decode())
    send_frame(ms[2], 0xFF, b"for all")
    got = recv_frame(w)
    check("the watcher hears what the players send everyone", got[0] == 2 and got[2] == b"for all")
    for k in (0, 1, 3, 4, 5, 6, 7): recv_frame(ms[k])
    send_frame(w, 0xFF, b"from the stands")
    hf = recv_frame(ms[0])
    check("a watcher's broadcast reaches the host alone", hf[0] == 8 and hf[2] == b"from the stands")
    send_frame(ms[1], 0xFF, b"next")
    got = [recv_frame(ms[k])[2] for k in (0, 2, 3, 4, 5, 6, 7)]
    check("...and no player but the host ever saw it", all(g == b"next" for g in got), str(got))
    recv_frame(w)
    w.close()
    uw = recv_frame(ms[0])
    check("the host hears UNWATCH 8, and the room carries on", uw[2] == b"UNWATCH 8", uw[2].decode())

    print("\n--- ROOMN: leaving ---")
    js[3].close()                                # member 4
    for k in (0, 1, 2, 3, 5, 6, 7):
        f = recv_frame(ms[k])
        check(f"member {k} hears LEFT 4", f[0] == 0xFE and f[2] == b"LEFT 4", f[2].decode())
    js[5].shutdown(socket.SHUT_RDWR); js[5].close()   # member 6, mid-flow
    for k in (0, 1, 2, 3, 5, 7):
        f = recv_frame(ms[k])
        check(f"member {k} hears LEFT 6", f[2] == b"LEFT 6", f[2].decode())
    late = conn(port, f"MOTE2 ROOMN {GID} JOIN TEST")
    check("a started room refuses JOIN with BUSY, even with places free", readline(late) == "BUSY")
    late.close()
    h.close()
    for k in (1, 2, 3, 5, 7):
        f = recv_frame(ms[k])
        check(f"member {k} hears CLOSED when the host goes", f[2] == b"CLOSED", f[2].decode())
        try:
            eof = ms[k].recv(1) == b""
        except OSError:
            eof = True
        check(f"member {k}'s socket is closed after", eof)
    time.sleep(0.2)
    again = conn(port, f"MOTE2 ROOMN {GID} HOST TEST PRIV 3")
    check("the code is free again once the room is gone", readline(again) == "SEAT 0 3")
    recv_frame(again)
    ls = conn(port, f"MOTE2 LIST {GID}")
    lst = b""
    while not lst.endswith(b"END\n"):
        c = ls.recv(4096)
        if not c: break
        lst += c
    check("a PRIV room is not listed", b"TEST" not in lst)
    j2 = conn(port, f"MOTE2 ROOMN {GID} JOIN TEST")
    check("...but joins by code", readline(j2) == "SEAT 1 3")
    j2.close(); again.close()

    print("\n--- ROOMN: QUICK ---")
    q1 = conn(port, f"MOTE2 ROOMN {GID} QUICK 4 QUICKIE")
    l1 = readline(q1); recv_frame(q1)
    q2 = conn(port, f"MOTE2 ROOMN {GID} QUICK 4")
    l2 = readline(q2); recv_frame(q2)
    q3 = conn(port, f"MOTE2 ROOMN {GID} QUICK 2")
    l3 = readline(q3); recv_frame(q3)
    check("QUICK hosts when nothing is open", l1 == "SEAT 0 4", l1)
    check("QUICK joins the open room of the same size", l2 == "SEAT 1 4", l2)
    check("QUICK does not put a 2-place request in a 4-place room", l3 == "SEAT 0 2", l3)
    other = conn(port, "MOTE2 ROOMN 99999999 QUICK 4")
    lo = readline(other)
    check("QUICK never crosses game ids", lo == "SEAT 0 4", lo)
    for s in (q1, q2, q3, other): s.close()

    print("\n--- ROOMN: a member that stops reading is dropped, the room goes on ---")
    h = conn(port, f"MOTE2 ROOMN {GID} HOST SLOW PRIV 3")
    readline(h); recv_frame(h)
    a = conn(port, f"MOTE2 ROOMN {GID} JOIN SLOW")
    readline(a); recv_frame(a); recv_frame(h)
    dead = conn(port, f"MOTE2 ROOMN {GID} JOIN SLOW", rcvbuf=4096)
    readline(dead)            # and never reads again
    recv_frame(h); recv_frame(a)
    blob = b"x" * 60000
    t0 = time.time()
    dropped = False
    sent = 0
    for i in range(600):                     # 36 MB at the member that does not read
        send_frame(h, 2, blob); sent += 1
        if i % 20 == 0:
            send_frame(h, 1, b"ping")
            try:
                while True:
                    f = recv_frame(a)
                    if f[0] == 0xFE and f[2] == b"LEFT 2":
                        dropped = True
                        break
                    if f[2] == b"ping":
                        break
            except socket.timeout:
                break
        if dropped:
            break
    check("the member that stopped reading was dropped (LEFT 2)", dropped, f"after {sent} frames, {time.time()-t0:.1f}s")
    send_frame(h, 1, b"still here")
    f = recv_frame(a)
    while f[2] != b"still here":
        f = recv_frame(a)
    check("and the others carry on", f[2] == b"still here")
    for s in (h, a, dead): s.close()

# ---- VOICE ---------------------------------------------------------------
VPKT = lambda n: bytes([n & 0xFF, n >> 8, n & 0xFF, n >> 8, 1 if n == 0 else 0]) + b"\x00\x80" * 3 + bytes([0x78]) + bytes(range(40))

def open_room_tests(port):
    """An OPEN room being played (CueVR's winner stays on, 7.0): PLAYING lists it
    to watch while it stays open to join."""
    print("\n--- ROOMN: an open room being played (PLAYING) ---")
    def listing():
        ls = conn(port, f"MOTE2 LIST {GID}")
        lst = b""
        while not lst.endswith(b"END\n"):
            c = ls.recv(4096)
            if not c: break
            lst += c
        ls.close()
        return lst
    h = conn(port, f"MOTE2 ROOMN {GID} HOST WSO1 PUB 6 WINNER ROOM")
    readline(h); recv_frame(h)
    j = conn(port, f"MOTE2 ROOMN {GID} JOIN WSO1")
    readline(j); recv_frame(j); recv_frame(h)
    lst = listing()
    check("before PLAYING: open to join, not listed to watch",
          b"ROOM WSO1 WINNER ROOM 2/6\n" in lst and b"LIVE WSO1 " not in lst, lst.decode().strip())
    send_frame(j, 0xFE, b"PLAYING")              # not the host: ignored
    time.sleep(0.2)
    check("a joiner's PLAYING is ignored", b"LIVE WSO1 " not in listing())
    send_frame(h, 0xFE, b"PLAYING")
    time.sleep(0.2)
    lst = listing()
    check("after the host's PLAYING: still open to join...", b"ROOM WSO1 WINNER ROOM 2/6\n" in lst, lst.decode().strip())
    check("...and listed to watch", b"LIVE WSO1 WINNER ROOM 2/6 0\n" in lst, lst.decode().strip())
    j2 = conn(port, f"MOTE2 ROOMN {GID} JOIN WSO1")
    line = readline(j2)
    check("it still takes a player", line == "SEAT 2 6", line)
    w = conn(port, f"MOTE2 ROOMN {GID} WATCH WSO1")
    line = readline(w)
    check("...and a watcher", line.startswith("SEAT 8 "), line)
    time.sleep(0.2)
    check("LIVE counts the watcher", b"LIVE WSO1 WINNER ROOM 3/6 1\n" in listing())
    for s_ in (w, j2, j, h): s_.close()


def udp_sock():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("127.0.0.1", 0)); u.settimeout(1.0)
    return u

def udp_recv(u, timeout=1.0):
    u.settimeout(timeout)
    try:
        return u.recv(2048)
    except socket.timeout:
        return None

def voice_ask(s):
    """VOICE to the relay; the answer (from 0xFE), parsed"""
    send_frame(s, 0xFE, b"VOICE")
    f = recv_frame(s)
    t = f[2].decode().split()
    return f, bytes.fromhex(t[1]), int(t[2])

def voice_tests(port, store):
    print("\n--- VOICE: asking, the token, keepalives ---")
    h = conn(port, f"MOTE2 ROOMN {GID} HOST VOIC PRIV 4 VOICE ROOM")
    readline(h); recv_frame(h)
    js = []
    for k in range(1, 4):
        j = conn(port, f"MOTE2 ROOMN {GID} JOIN VOIC"); readline(j); recv_frame(j); js.append(j)
    ms = [h] + js
    for k, m in enumerate(ms):                   # everyone hears the later JOINEDs
        for _ in range(3 if k == 0 else 3 - k):
            recv_frame(m)
    send_frame(ms[2], 0xFF, b"before")           # a game frame, for its number
    seq_before = [recv_frame(ms[k])[1] for k in (0, 1, 3)]
    f1, tok1, uport = voice_ask(ms[1])
    check("VOICE is answered from the relay, with a token and the UDP port",
          f1[0] == 0xFE and len(tok1) == 8 and uport == port, f1[2].decode())
    f2, tok2, _ = voice_ask(ms[2])
    f3, tok3, _ = voice_ask(ms[3])
    check("each member has its own token", len({tok1, tok2, tok3}) == 3)
    send_frame(ms[2], 0xFF, b"after")
    seq_after = [recv_frame(ms[k])[1] for k in (0, 1, 3)]
    check("the answers took no number: the next game frame follows on everywhere",
          all(b == a + 1 for a, b in zip(seq_before, seq_after)), f"{seq_before} -> {seq_after}")
    check("...and member 1's answer carried the room's current number", f1[1] == seq_before[0], f"{f1[1]} vs {seq_before[0]}")
    u1, u2, u3 = udp_sock(), udp_sock(), udp_sock()
    ra = ("127.0.0.1", uport)
    u1.sendto(b"K\x01" + tok1 + (7).to_bytes(4, "little") + (1234).to_bytes(4, "little"), ra)
    pong = udp_recv(u1)
    check("a keepalive is answered with its id and time", pong == b"P\x01" + (7).to_bytes(4, "little") + (1234).to_bytes(4, "little"), repr(pong))
    u2.sendto(b"K\x01" + tok2 + bytes(8), ra); udp_recv(u2)
    u9 = udp_sock()
    u9.sendto(b"K\x01" + bytes(8) + bytes(8), ra)
    check("a keepalive with a token nobody has goes unanswered", udp_recv(u9, 0.4) is None)

    print("\n--- VOICE: fanned out by UDP, by TCP, and to nobody who did not ask ---")
    pk = VPKT(0)
    u1.sendto(b"A\x01" + tok1 + pk, ra)
    got2 = udp_recv(u2)
    check("member 2 (UDP known) hears member 1 by UDP", got2 == b"A\x01\x01" + pk, repr(got2)[:60])
    f = recv_frame(ms[3])
    check("member 3 (asked, no UDP yet) hears it down its TCP, from 0xFD, member 1 first",
          f[0] == 0xFD and f[2] == b"\x01" + pk, f"{f[0]:#x} {f[2][:8]!r}")
    check("...stamped with the room's current number (taking none)", f[1] == seq_after[2], f"{f[1]} vs {seq_after[2]}")
    check("member 1 does not hear itself", udp_recv(u1, 0.3) is None)
    send_frame(ms[2], 0xFF, b"game")
    f0 = recv_frame(ms[0])
    check("member 0 (never asked) gets nothing but the game: the next frame is the game's",
          f0[2] == b"game" and f0[1] == seq_after[0] + 1, f"{f0[2]!r} {f0[1]}")
    recv_frame(ms[1]); recv_frame(ms[3])
    u9.sendto(b"A\x01" + bytes(8) + pk, ra)
    check("voice with a bad token goes nowhere", udp_recv(u2, 0.4) is None)

    print("\n--- VOICE: TCP both ways ---")
    send_frame(ms[3], 0xFD, VPKT(5))
    g1 = udp_recv(u1); g2 = udp_recv(u2)
    check("voice sent down TCP (to 0xFD) reaches the UDP members", g1 == b"A\x01\x03" + VPKT(5) and g2 == g1, repr(g1)[:40])
    send_frame(ms[2], 0xFE, b"VOICE TCP")
    time.sleep(0.1)
    u1.sendto(b"A\x01" + tok1 + VPKT(1), ra)
    f = recv_frame(ms[2])
    check("a member that said VOICE TCP gets it down TCP, though its UDP is known", f[0] == 0xFD and f[2] == b"\x01" + VPKT(1))
    check("...and not by UDP as well", udp_recv(u2, 0.3) is None)
    recv_frame(ms[3])
    send_frame(ms[2], 0xFE, b"VOICE UDP")
    time.sleep(0.1)
    u1.sendto(b"A\x01" + tok1 + VPKT(2), ra)
    check("VOICE UDP puts it back on UDP", udp_recv(u2) == b"A\x01\x01" + VPKT(2))
    recv_frame(ms[3])
    send_frame(ms[0], 0xFD, VPKT(3))
    check("voice down TCP from a member that never asked is dropped", udp_recv(u2, 0.4) is None)

    print("\n--- VOICE: a watcher listens and is never heard ---")
    send_frame(h, 0xFE, b"START")
    for m in ms: recv_frame(m)
    w = conn(port, f"MOTE2 ROOMN {GID} WATCH VOIC"); readline(w); recv_frame(w); recv_frame(h)
    fw, tokw, _ = voice_ask(w)
    uw = udp_sock(); uw.sendto(b"K\x01" + tokw + bytes(8), ra); udp_recv(uw)
    u1.sendto(b"A\x01" + tok1 + VPKT(4), ra)
    check("the watcher hears a player", udp_recv(uw) == b"A\x01\x01" + VPKT(4))
    udp_recv(u2); recv_frame(ms[3])
    uw.sendto(b"A\x01" + tokw + VPKT(9), ra)
    send_frame(w, 0xFD, VPKT(9))
    check("nothing the watcher says reaches a player (UDP or TCP)",
          udp_recv(u1, 0.4) is None and udp_recv(u2, 0.2) is None)
    send_frame(ms[2], 0xFF, b"next")
    check("...and member 3's next frame is the game's", recv_frame(ms[3])[2] == b"next")
    for k in (0, 1): recv_frame(ms[k])
    recv_frame(w)

    print("\n--- VOICE: a cap on the packets, and a member that stops reading ---")
    for i in range(300):
        u1.sendto(b"A\x01" + tok1 + VPKT(i), ra)
    n = 0
    while udp_recv(u2, 0.3) is not None:
        n += 1
    check("one member cannot flood the room (100 a second)", 50 <= n <= 110, f"{n} of 300 passed")
    # a member that asks for voice and then stops reading, behind a small window
    dead = conn(port, f"MOTE2 ROOMN {GID} WATCH VOIC", rcvbuf=4096); readline(dead)
    recv_frame(dead); recv_frame(h)                   # MEMBERS; the host hears WATCHER
    voice_ask(dead)
    for i in range(3000):                             # ~1 MB of voice at it, down TCP
        send_frame(ms[2], 0xFD, VPKT(i & 0xFF) + bytes(300))
        if i % 100 == 99:
            time.sleep(0.12)                          # under the packet cap
    time.sleep(0.3)
    send_frame(ms[2], 0xFF, b"still in")
    got = [recv_frame(ms[k]) for k in (0, 1)]
    check("voice that cannot get through is dropped; the member it was for is not",
          [g[2] for g in got] == [b"still in", b"still in"], str([g[2][:12] for g in got]))
    f = recv_frame(ms[3])
    seen_v = 0
    while f[0] == 0xFD:
        seen_v += 1; f = recv_frame(ms[3])
    check("...and a member that does read gets the game frame behind the voice it was sent", f[2] == b"still in", f"{seen_v} voice first")
    while True:
        g = recv_frame(w)
        if g[0] != 0xFD: break
    check("(the other watcher too)", g[2] == b"still in")
    dead.close()
    uw_ = recv_frame(h)
    check("the backed-up watcher was never dropped: it leaves when it closes (UNWATCH)", uw_[2].startswith(b"UNWATCH"), uw_[2].decode())
    while udp_recv(u1, 0.2) is not None: pass
    while udp_recv(uw, 0.1) is not None: pass

    print("\n--- VOICE: a token dies with its member ---")
    js[0].close()                                 # member 1
    for k in (0, 2, 3):
        recv_frame(ms[k])
    recv_frame(w)
    time.sleep(0.2)
    u1.sendto(b"A\x01" + tok1 + VPKT(8), ra)
    check("a member gone: its token speaks to nobody", udp_recv(u2, 0.4) is None)

    print("\n--- REPORT ---")
    send_frame(ms[2], 0xFE, b"REPORT 3 SPEECH SHOUTER|MARK")
    f = recv_frame(ms[2])
    rep = os.path.join(store, "reports.tsv") if store else None
    if store:
        lines = open(rep).read().splitlines() if os.path.exists(rep) else []
        t = lines[0].split("\t") if lines else []
        check("a report is answered REPORTED and kept, one line", f[2] == b"REPORTED" and len(lines) == 1, f"{f[2]!r}")
        check("...time, room, reason, both names, both places, two hashes",
              len(t) == 10 and t[2] == "VOIC" and t[3] == "SPEECH" and t[4:8] == ["SHOUTER", "MARK", "3", "2"]
              and all(len(h) == 16 and all(c in "0123456789abcdef" for c in h) for h in t[8:10]), "\t".join(t))
        check("...and no address in it", "127.0.0.1" not in lines[0] if lines else False)
        check("one address, one hash (both ends are 127.0.0.1 here)", len(t) == 10 and t[8] == t[9])
        salt = os.path.join(store, "reports.salt")
        check("the relay's secret is its own (mode 600)", os.path.exists(salt) and (os.stat(salt).st_mode & 0o777) == 0o600,
              oct(os.stat(salt).st_mode & 0o777) if os.path.exists(salt) else "missing")
        REPORT_HASH[0] = t[8] if len(t) == 10 else None
    else:
        check("without a store a report says so", f[2] == b"REPORT OFF", f[2].decode())
    send_frame(ms[2], 0xFF, b"x")
    check("...and took no number", recv_frame(ms[0])[1] == f[1] + 1)
    for s_ in (h, ms[2], ms[3], w): s_.close()
    for u in (u1, u2, u3, u9, uw): u.close()

REPORT_HASH = [None]

def report_again(store):
    """a new relay on the same store: the same address hashes the same"""
    p, port = start_relay(os.path.join(HERE, "mote_relay.py"), ["--store", store])
    try:
        h = conn(port, f"MOTE2 ROOMN {GID} HOST AGIN PRIV 2"); readline(h); recv_frame(h)
        j = conn(port, f"MOTE2 ROOMN {GID} JOIN AGIN"); readline(j); recv_frame(j); recv_frame(h)
        send_frame(h, 0xFE, b"REPORT 1 NAME RUDE|ME")
        f = recv_frame(h)
        lines = open(os.path.join(store, "reports.tsv")).read().splitlines()
        t = lines[-1].split("\t")
        print("\n--- REPORT: a restarted relay ---")
        check("a report after a restart is kept too", f[2] == b"REPORTED" and len(lines) == 2)
        check("...and the same address has the same hash (repeat reports can be matched)",
              REPORT_HASH[0] is not None and t[8] == REPORT_HASH[0], f"{t[8]} vs {REPORT_HASH[0]}")
        h.close(); j.close()
    finally:
        p.kill()

def voice_policy():
    """RoomN.voice itself, with members whose queues are as backed up as we
    like: the drop is decided by the bytes queued, and never drops a member."""
    import asyncio, importlib.util
    spec = importlib.util.spec_from_file_location("mote_relay_t", os.path.join(HERE, "mote_relay.py"))
    mr = importlib.util.module_from_spec(spec); spec.loader.exec_module(mr)
    async def run():
        room = mr.RoomN("G", "C", False, 4, "L")
        ms = [mr.Member(k, None, None, ("1.2.3.4", 5)) for k in range(4)]
        for m in ms:
            room.members[m.k] = m
            m.vtok = bytes([m.k]) * 8
        ms[2].backlog = 20000                   # past the cap
        ms[3].backlog = 100
        ms[3].vtcp = True
        room.seq = 41
        room.voice(1, b"v" * 50, None, 16384)
        r = {k: ms[k].q.qsize() for k in range(4)}
        return r, room.vdropped, ms[3].q.get_nowait(), room.seq, [m.gone for m in ms]
    r, dropped, frame, seq, gone = asyncio.run(run())
    print("\n--- VOICE: the backlog rule, on its own ---")
    check("a member backed up past --voice-backlog gets no voice, the rest do", r == {0: 1, 1: 0, 2: 0, 3: 1} and dropped == 1, f"{r} dropped {dropped}")
    check("...the frame is from 0xFD with the room's number, which does not move",
          frame[2] == 0xFD and int.from_bytes(frame[3:7], "little") == 41 and seq == 41 and frame[7] == 1, repr(frame[:8]))
    check("...and nobody is dropped for it", not any(gone))

def room_transcript(port, voice):
    """A ROOMN session as two OLD clients see it -- bytes, numbers and all --
    with a third member that, when `voice`, asks for voice and talks over UDP
    and TCP throughout. Lockstep: each game frame is read where it lands before
    the next is sent, so the order is the script's and not the scheduler's.
    Against the old relay (which ignores all the voice) the transcripts of
    the two old clients must be identical."""
    out = {0: [], 2: []}
    h = conn(port, f"MOTE2 ROOMN {GID} HOST TRAN PRIV 3 OLD CLIENTS")
    out[0].append(readline(h).encode()); out[0].append(recv_frame(h))
    v = conn(port, f"MOTE2 ROOMN {GID} JOIN TRAN"); readline(v); recv_frame(v)
    out[0].append(recv_frame(h))
    o = conn(port, f"MOTE2 ROOMN {GID} JOIN TRAN")
    out[2].append(readline(o).encode()); out[2].append(recv_frame(o))
    out[0].append(recv_frame(h)); recv_frame(v)
    tok, ra = None, None
    u = udp_sock()
    if voice:
        send_frame(v, 0xFE, b"VOICE")
        v.settimeout(0.5)
        try:
            f = recv_frame(v)
            if f[0] == 0xFE and f[2].startswith(b"VOICE "):
                t = f[2].decode().split(); tok = bytes.fromhex(t[1]); ra = ("127.0.0.1", int(t[2]))
        except socket.timeout:
            pass
        v.settimeout(5)
    def talk(i):
        if not voice: return
        if tok: u.sendto(b"A\x01" + tok + VPKT(i), ra)
        send_frame(v, 0xFD, VPKT(i))
        send_frame(v, 0xFE, b"VOICE TCP" if i % 2 else b"VOICE UDP")
        send_frame(v, 0xFE, b"VOICE")
    def land(sender, to=0xFF):
        for m, k in ((h, 0), (o, 2)):
            if m is not sender and (to == 0xFF or to == k):
                out[k].append(recv_frame(m))
        if v is not sender and to == 0xFF:
            f = recv_frame(v)
            while f[0] == 0xFE and f[2].startswith(b"VOICE"):
                f = recv_frame(v)
    for i in range(30):
        talk(i)
        snd = [h, v, o][i % 3]
        send_frame(snd, 0xFF, f"g{i}".encode())
        land(snd)
        if i == 10:
            send_frame(h, 2, b"just for two"); land(h, 2)
        if i == 20:
            send_frame(h, 0xFE, b"START")
            for m, k in ((h, 0), (o, 2)): out[k].append(recv_frame(m))
            recv_frame(v)
    time.sleep(0.2)
    v.close()
    for m, k in ((h, 0), (o, 2)):
        m.settimeout(1.0)
        try:
            out[k].append(recv_frame(m))
        except (socket.timeout, EOFError):
            out[k].append("none")
    h.close()
    try:
        out[2].append(recv_frame(o))
    except (socket.timeout, EOFError):
        out[2].append("none")
    o.close(); u.close()
    return out

def old_transcript(port):
    """The old verbs, recorded as bytes, so two relays can be compared."""
    out = []
    g = "43554565"
    # HOST / JOIN splice
    h = conn(port, f"MOTE2 HOST {g} ABCD PUB Mark 8-ball")
    time.sleep(0.1)
    ls = conn(port, f"MOTE2 LIST {g}")
    d = b""
    while not d.endswith(b"END\n"):
        c = ls.recv(4096)
        if not c: break
        d += c
    out.append(("LIST", d))
    t = conn(port, f"MOTE2 HOST {g} ABCD PRIV")
    out.append(("HOST taken", t.recv(100))); t.close()
    j = conn(port, f"MOTE2 JOIN {g} ABCD")
    out.append(("GO host", readline(h).encode()))
    out.append(("GO join", readline(j).encode()))
    payload = bytes(range(256)) * 40
    h.sendall(payload)
    out.append(("host->join", readn(j, len(payload))))
    j.sendall(payload[::-1])
    out.append(("join->host", readn(h, len(payload))))
    ls = conn(port, f"MOTE2 LIST {g}")
    d = b""
    while not d.endswith(b"END\n"):
        c = ls.recv(4096)
        if not c: break
        d += c
    out.append(("LIST after pair", d))
    j.close()
    try:
        out.append(("host EOF", h.recv(10)))
    except OSError as e:
        out.append(("host EOF", type(e).__name__.encode()))
    h.close()
    n = conn(port, f"MOTE2 JOIN {g} ZZZZ")
    out.append(("JOIN none", n.recv(100))); n.close()
    e = conn(port, "MOTE2 BOGUS X")
    out.append(("ERR", e.recv(100))); e.close()
    # QUICK: first hosts, second pairs
    q1 = conn(port, f"MOTE2 QUICK {g} Q1")
    time.sleep(0.1)
    q2 = conn(port, f"MOTE2 QUICK {g} Q2")
    out.append(("QUICK host", readline(q1).encode()))
    out.append(("QUICK join", readline(q2).encode()))
    q1.sendall(b"hello"); out.append(("quick pipe", readn(q2, 5)))
    q1.close(); q2.close()
    # MOTE1 legacy
    m1 = conn(port, "MOTE1 HOST LEGACY PUB TAG")
    time.sleep(0.1)
    ls = conn(port, "MOTE1 LIST")
    d = b""
    while not d.endswith(b"END\n"):
        c = ls.recv(4096)
        if not c: break
        d += c
    out.append(("MOTE1 LIST", d))
    m2 = conn(port, "MOTE1 JOIN LEGACY")
    out.append(("MOTE1 GO", readline(m1).encode() + b"|" + readline(m2).encode()))
    m1.close(); m2.close()
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", help="the relay before ROOMN, to compare the old verbs against")
    args = ap.parse_args()
    # a store the relay has to make itself: a report must not depend on it existing
    store = os.path.join(tempfile.mkdtemp(prefix="roomn_store_"), "store")
    new, port = start_relay(os.path.join(HERE, "mote_relay.py"), ["--room-backlog", "65536", "--store", store])
    try:
        roomn_tests(port)
        open_room_tests(port)
        voice_tests(port, store)
        report_again(store)
        voice_policy()
        print("\n--- VOICE: an old client's room, with and without a talker in it ---")
        quiet = room_transcript(port, False)
        loud = room_transcript(port, True)
        check("old clients see the same bytes whether or not someone in the room talks",
              quiet == loud, "" if quiet == loud else f"{len(quiet[0])}/{len(loud[0])} frames")
        nv, nvport = start_relay(os.path.join(HERE, "mote_relay.py"), ["--no-voice"])
        try:
            s_ = conn(nvport, f"MOTE2 ROOMN {GID} HOST NOVO PRIV 2"); readline(s_); recv_frame(s_)
            send_frame(s_, 0xFE, b"VOICE"); s_.settimeout(0.5)
            try:
                recv_frame(s_); said = True
            except socket.timeout:
                said = False
            check("a relay run --no-voice says nothing to VOICE (as an old one does)", not said)
            s_.close()
        finally:
            nv.kill()
        print("\n--- old verbs ---")
        tn = old_transcript(port)
        check("old verbs ran", len(tn) > 10)
        if args.old:
            old, oport = start_relay(args.old)
            try:
                to = old_transcript(oport)
            finally:
                old.kill()
            for (a, x), (b, y) in zip(tn, to):
                check(f"old verb '{a}' identical to the old relay", a == b and x == y,
                      "" if x == y else f"new {x[:80]!r} old {y[:80]!r}")
            check("same number of exchanges", len(tn) == len(to))
            old2, oport2 = start_relay(args.old)
            try:
                old_loud = room_transcript(oport2, True)
            finally:
                old2.kill()
            check("ROOMN, old clients beside a talker: identical to the old relay, byte for byte",
                  old_loud == loud, "" if old_loud == loud else f"new {loud[0][:3]} old {old_loud[0][:3]}")
    finally:
        new.kill()
        log = new.stdout.read().decode(errors="replace")
        open("/tmp/test_roomn_relay.log", "w").write(log)
    print(f"\n{'PASS' if not FAILS else 'FAIL'} ({len(FAILS)} failed)  relay log: /tmp/test_roomn_relay.log")
    sys.exit(1 if FAILS else 0)

if __name__ == "__main__":
    main()
