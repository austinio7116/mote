#!/usr/bin/env python3
"""
Bench for the relay's ROOMN hub, and a regression check that the old verbs
behave byte for byte as they did.

    python3 test_roomn.py [--old /path/to/old/mote_relay.py]

Starts its own relay(s) on free localhost ports -- never a live one. With
--old, the same old-verb scenarios run against the old relay too and the two
transcripts must be identical.
"""
import argparse
import os
import random
import socket
import subprocess
import sys
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
    check("a started room is off LIST", b"TEST" not in lst)

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
    new, port = start_relay(os.path.join(HERE, "mote_relay.py"), ["--room-backlog", "65536"])
    try:
        roomn_tests(port)
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
    finally:
        new.kill()
        log = new.stdout.read().decode(errors="replace")
        open("/tmp/test_roomn_relay.log", "w").write(log)
    print(f"\n{'PASS' if not FAILS else 'FAIL'} ({len(FAILS)} failed)  relay log: /tmp/test_roomn_relay.log")
    sys.exit(1 if FAILS else 0)

if __name__ == "__main__":
    main()
