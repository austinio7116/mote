#!/usr/bin/env python3
"""
test_forfeit_leaver.py -- the walk-out is the one who went, not the one whose
connection happened to finish first (2026-10-18; Mark: "Both this matches were
given to orms despite me being the one that stayed in the room").

WLVJ and GVNQ, 2026-10-17: Orms16 hosted a four-place table, lolly11luce joined,
the frame started, and the relay dropped Orms16 for backlog. That drop happens
inside the other player's send; the host gone closed the room, which ended the
OTHER player's connection first -- and the relay wrote HIM down as leaving.

Here the same: a host that stops reading, a guest whose table updates fill the
host's queue, both games having said the frame started. The forfeit must name
the host. And the ordinary case still holds: a guest who walks out is the guest.

    python3 test_forfeit_leaver.py [--relay path/to/mote_relay.py]

Its own relay and store on localhost -- never the live one.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_roomn import conn, readline, send_frame, free_port, GID  # noqa: E402

FAILS = []


def check(cond, what):
    print(("  [ok ] " if cond else "  [BAD] ") + what)
    if not cond:
        FAILS.append(what)


def start_msg(me):
    return ("STARTED " + json.dumps({"v": 1, "t": "start", "frame": 1, "kind": 2, "me": me, "best_of": 1,
                                     "names": ["HOSTY", "GUESTY"], "game": "9-BALL"})).encode()


def table(host, code):
    h = conn(host, f"MOTE2 ROOMN {GID} HOST {code} PRIV 4 TEST", rcvbuf=4096)
    readline(h)
    g = conn(host, f"MOTE2 ROOMN {GID} JOIN {code}")
    readline(g)
    send_frame(h, 0xFE, b"START"); time.sleep(0.2)
    send_frame(h, 0xFE, start_msg(0)); send_frame(g, 0xFE, start_msg(1)); time.sleep(0.3)
    return h, g


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--relay", default=os.path.join(HERE, "mote_relay.py"))
    a = ap.parse_args()
    store = tempfile.mkdtemp()
    port = free_port()
    relay = subprocess.Popen([sys.executable, a.relay, "--addr", "127.0.0.1", "--port", str(port), "--store", store,
                              "--no-voice", "--forfeit-wait", "1", "--room-backlog", "6000", "--forfeits-from", "2000-01-01 00:00:00"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError:
                time.sleep(0.05)

        # ---- the host stops reading; the guest's table updates fill its queue
        h, g = table(port, "WLVJ")
        g.setblocking(False)
        state = bytes([0xC1]) + bytes(900)          # a table: queued, never skipped
        for _ in range(4000):
            try:
                send_frame(g, 0xFF, state)
                while g.recv(65536):
                    pass
            except (BlockingIOError, OSError):
                pass
            time.sleep(0.001)
        time.sleep(2.5)                               # the forfeit is written when the wait runs out
        g.close(); h.close()

        # ---- the ordinary case: the guest walks out
        h2, g2 = table(port, "PLAIN")
        g2.close(); time.sleep(2.5)
        h2.close()
        time.sleep(0.5)
    finally:
        relay.terminate()
        out = relay.stdout.read().decode(errors="replace") if relay.stdout else ""
    lines = out.splitlines()
    left = {code: [l for l in lines if f"/{code}: " in l and "left frame" in l] for code in ("WLVJ", "PLAIN")}
    for code in left:
        for l in left[code]:
            print("   relay:", l.split(" nroom ", 1)[-1])
    check(any("backlog" in l and "/WLVJ:" in l for l in lines), "the host is dropped for backlog")
    check(any("HOSTY left frame" in l for l in left["WLVJ"]) and not any("GUESTY left frame" in l for l in left["WLVJ"]),
          "the host who was dropped is the one who left -- not the guest, still in the room")
    check(any("GUESTY left frame" in l for l in left["PLAIN"]), "a guest who walks out is still the guest")
    print(f"{len(FAILS) == 0 and 'PASS' or 'FAIL'} ({len(FAILS)} bad)")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
