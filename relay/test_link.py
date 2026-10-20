#!/usr/bin/env python3
"""test_link.py -- the Mote link relay: HOST, JOIN, QUICK and LIST, MOTE1 and
MOTE2, and the splice between two clients. Starts the relay on a free local
port, talks to it as Studio's link_net.c does, and stops it.

    python3 test_link.py      -> "<n> ok, <m> bad"; exit 1 on any bad
"""
import os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ok = bad = 0


def check(c, what):
    global ok, bad
    print(("  ok    " if c else "  [BAD] ") + what)
    if c: ok += 1
    else: bad += 1


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


PORT = free_port()
relay = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--addr", "127.0.0.1", "--port", str(PORT)],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def conn(line):
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
            break
        except OSError:
            time.sleep(0.1)
    s.sendall(line.encode() + b"\n")
    return s


def readline(s):
    b = b""
    while not b.endswith(b"\n"):
        c = s.recv(1)
        if not c: break
        b += c
    return b.decode().strip()


def lines_until_end(s):
    out = []
    while True:
        l = readline(s)
        out.append(l)
        if l == "END" or not l: return out


try:
    # HOST and JOIN, MOTE2: paired, and bytes both ways untouched
    h = conn("MOTE2 HOST 1234 ABCD PUB a game")
    time.sleep(0.2)
    lst = conn("MOTE2 LIST 1234")
    rows = lines_until_end(lst)
    check("ROOM ABCD a game" in rows and rows[-1] == "END", f"LIST shows the public room ({rows})")
    other = lines_until_end(conn("MOTE2 LIST 9999"))
    check(other == ["END"], "another game's LIST does not")
    check(readline(conn("MOTE2 HOST 1234 ABCD PRIV")) == "TAKEN", "a code in use: TAKEN")
    g = conn("MOTE2 JOIN 1234 ABCD")
    check(readline(h) == "GO H" and readline(g) == "GO G", "paired: GO H to the host, GO G to the joiner")
    payload = bytes(range(256)) * 4
    h.sendall(payload)
    got = b""
    while len(got) < len(payload):
        c = g.recv(65536)
        if not c: break
        got += c
    check(got == payload, "host -> joiner: every byte, unchanged")
    g.sendall(b"\xa5\x00\x01back")
    check(h.recv(64) == b"\xa5\x00\x01back", "joiner -> host the same")
    check(lines_until_end(conn("MOTE2 LIST 1234")) == ["END"], "a paired room is off LIST")
    g.close()
    time.sleep(0.2)
    h.settimeout(3)
    check(h.recv(64) == b"", "the joiner going closes the host's side")
    check(readline(conn("MOTE2 JOIN 1234 NOPE")) == "NONE", "no such room: NONE")

    # QUICK: the first waits as a public room, the second takes it
    q1 = conn("MOTE2 QUICK 1234 QUICKER")
    time.sleep(0.2)
    rows = lines_until_end(conn("MOTE2 LIST 1234"))
    check(len(rows) == 2 and rows[0].startswith("ROOM ") and rows[0].endswith(" QUICKER"), f"QUICK alone hosts a public room ({rows})")
    q2 = conn("MOTE2 QUICK 1234")
    check(readline(q1) == "GO H" and readline(q2) == "GO G", "a second QUICK pairs with it")
    q1.close(); q2.close()

    # MOTE1, the shared pool
    m1 = conn("MOTE1 HOST OLD1 PUB")
    time.sleep(0.2)
    check("ROOM OLD1 GAME" in lines_until_end(conn("MOTE1 LIST")), "MOTE1 HOST and LIST")
    m2 = conn("MOTE1 JOIN OLD1")
    check(readline(m1) == "GO H" and readline(m2) == "GO G", "MOTE1 JOIN pairs")
    m1.close(); m2.close()

    # a host that leaves while waiting frees its code
    w = conn("MOTE2 HOST 1234 GONE PRIV")
    time.sleep(0.2); w.close(); time.sleep(0.3)
    check(readline(conn("MOTE2 JOIN 1234 GONE")) == "NONE", "a host gone while waiting: the room is gone too")

    check(readline(conn("HELLO")) == "ERR", "not MOTE1/MOTE2: ERR")
    check(readline(conn("MOTE2 ROOMN 1234 HOST X PUB 4")) == "ERR", "a verb it does not serve: ERR")
finally:
    relay.terminate()
    try:
        relay.wait(5)
    except subprocess.TimeoutExpired:
        relay.kill()
print(f"{ok} ok, {bad} bad")
sys.exit(1 if bad else 0)
