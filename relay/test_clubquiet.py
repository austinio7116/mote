#!/usr/bin/env python3
"""test_clubquiet.py -- a club member gone quiet has gone (Mark, 2026-10-14:
"tables get locked for a long time when people quit without using the leave
club button (just shutting down)"). Over real sockets, --club-quiet 2:

  * a member at practice sets up a table, then sends nothing (its socket left
    open, as a switched-off headset's is): it is taken as gone in about two
    seconds and its table freed for the others;
  * a member who keeps sending is not touched.
(A tournament's watcher sends nothing by design; its place is from WATCH_BASE
up and the quiet rule never applies to it -- test_tourn_watch still passes.)"""
import os, socket, subprocess, sys, tempfile, threading, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_roomn import free_port  # noqa: E402
from test_clubhold import C, GID  # noqa: E402

ok = bad = 0


def check(cond, what):
    global ok, bad
    print(("  [ok ] " if cond else "  [BAD] ") + what)
    ok += bool(cond); bad += not cond


def main():
    store = tempfile.mkdtemp()
    port = free_port()
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--addr", "127.0.0.1", "--port", str(port),
                              "--store", store, "--tourn-trust", "--no-voice", "--club-quiet", "2"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError:
                time.sleep(0.05)
        quiet = C(port, f"MOTE2 CLUB {GID} JOIN 1")
        quiet.say("NAME QUIETY"); quiet.say("TABLE 1 - 0 UK 8-BALL")
        live = C(port, f"MOTE2 CLUB {GID} JOIN 1")
        stop = [False]

        def chatter():                      # a game in the club: a pose-sized frame, often
            while not stop[0]:
                try:
                    live.say("NAME LIVELY")
                except OSError:
                    return
                time.sleep(0.2)
        threading.Thread(target=chatter, daemon=True).start()
        time.sleep(0.5)
        check(any(c.startswith("TABLE 1 ") and c != "TABLE 1 -" for c in live.ctrl), "the quiet member's table is set up")
        t0 = time.time()
        while time.time() - t0 < 6 and "TABLE 1 -" not in live.ctrl:
            time.sleep(0.1)
        took = time.time() - t0
        check("TABLE 1 -" in live.ctrl and "LEFT 0" in live.ctrl, f"the silent member is gone and its table freed ({took:.1f} s)")
        check(1.0 < took < 4.0, "...after about the quiet time, not before")
        time.sleep(2.5)
        check("LEFT 1" not in live.ctrl, "the member who keeps talking stays")
        stop[0] = True
    finally:
        relay.terminate()
        out = relay.communicate(timeout=10)[0].decode(errors="replace")
        if bad:
            print(out[-3000:])
    print(f"{ok} ok, {bad} bad")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
