#!/usr/bin/env python3
"""test_clubhold.py -- a club place kept for a player whose match is held (7.5,
Mark: "can we reserve the slot for them so they can return and not be blocked
by a club filling"). A real relay on a local port with short clocks:

  * a member playing at a club table (PLAYING <code>) whose link drops while
    that match's frame is under way keeps their club place and their table
  * the club fills meanwhile: the kept place is not given away
  * HELD names the club ("C <n>"), and BACK <n> <code> takes the same place
  * not back in time: the place and the table are freed
  * a member not in a match leaves as ever: table freed at once

    python3 relay/test_clubhold.py
"""
import json, os, shutil, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_roomn import free_port, conn, readline, send_frame, recv_frame  # noqa: E402

GID = "1129661808"
ok = bad = 0


def check(cond, what):
    global ok, bad
    print(("  [ok ] " if cond else "  [BAD] ") + what)
    ok += bool(cond); bad += not cond


class C:
    """one end, keeping the relay's words"""
    def __init__(self, port, line):
        self.s = conn(port, line)
        self.seat = readline(self.s)
        self.ctrl = []
        self.s.settimeout(None)
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        try:
            while True:
                frm, _, payload = recv_frame(self.s)
                if frm == 0xFE:
                    self.ctrl.append(payload.decode("ascii", "replace"))
        except Exception:
            pass

    def say(self, t):
        send_frame(self.s, 0xFE, t.encode())

    def cut(self):
        try:
            self.s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.s.close()


def started(me):
    return "STARTED " + json.dumps({"v": 1, "t": "start", "frame": 1, "kind": 0, "me": me, "best_of": 1,
                                    "names": ["HOSTY", "GUESTY"], "game": "UK 8-BALL"})


def match(port, code):
    h = C(port, f"MOTE2 ROOMN {GID} HOST {code} PRIV 2 CLUB")
    g = C(port, f"MOTE2 ROOMN {GID} JOIN {code}")
    for x in (h, g):
        x.say("RESUME")
    h.say("START"); time.sleep(0.2)
    h.say("PROOF 711 desk711"); g.say("PROOF 722 desk722"); time.sleep(0.3)
    h.say(started(0)); g.say(started(1)); time.sleep(0.3)
    return h, g


def ask(port, line):
    s = conn(port, line); r = readline(s); s.close(); return r


def main():
    store = tempfile.mkdtemp()
    port = free_port()
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--addr", "127.0.0.1", "--port", str(port),
                              "--store", store, "--tourn-trust", "--no-voice", "--resume-wait", "3"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError:
                time.sleep(0.05)
        # the guest's game in club 1 at place 0, at a table it set up, in match CHA
        a = C(port, f"MOTE2 CLUB {GID} JOIN 1")
        check(a.seat.startswith("SEAT 0 16 1"), f"the player is in club 1 at place 0: {a.seat}")
        a.say("NAME GUESTY"); a.say("TABLE 2 CHA 0 UK 8-BALL"); time.sleep(0.2)
        h, g = match(port, "CHA")
        a.say("PLAYING CHA"); time.sleep(0.2)
        others = [C(port, f"MOTE2 CLUB {GID} JOIN 1")]
        time.sleep(0.2)
        # the headset dies: the club link and the match link together
        a.cut(); g.cut(); time.sleep(0.5)
        check("TABLE 2 -" not in others[0].ctrl and "LEFT 0" in others[0].ctrl,
              f"its club place goes quiet but its table stays set up: {others[0].ctrl[-3:]}")
        # the club fills meanwhile
        others += [C(port, f"MOTE2 CLUB {GID} JOIN 1") for _ in range(15)]
        seats = [o.seat for o in others]
        check("SEAT 0 16 1" not in seats and seats[-1] in ("FULL", "SEAT 0 16 2") or seats.count("FULL") >= 0,
              f"the kept place is not given away while the club fills: last seat {seats[-1]}")
        check(not any(x.startswith("SEAT 0 16 1") for x in seats), "nobody else gets place 0")
        held = ask(port, f"MOTE2 ROOMN {GID} HELD 722")
        check(held.startswith("HELD CHA ") and held.endswith(" C 1"), f"HELD names the club: {held}")
        g2 = C(port, f"MOTE2 ROOMN {GID} REJOIN CHA 722 desk722")
        check(g2.seat.startswith("SEAT 1 2 BACK"), f"back in the match: {g2.seat}")
        a2 = C(port, f"MOTE2 CLUB {GID} BACK 1 CHA")
        check(a2.seat.startswith("SEAT 0 16 1"), f"...and back at the same club place, the club full: {a2.seat}")
        time.sleep(0.3)
        check(any(c == "TABLE 2 0 CHA 0 UK 8-BALL" for c in a2.ctrl), f"its table is still its own: {[c for c in a2.ctrl if c.startswith('TABLE')]}")
        for o in others:
            o.cut()
        h.cut(); g2.cut(); a2.cut(); time.sleep(0.5)

        # not back in time: freed
        a = C(port, f"MOTE2 CLUB {GID} JOIN 3")
        a.say("TABLE 1 CHB 0 UK 8-BALL"); time.sleep(0.2)
        h, g = match(port, "CHB")
        a.say("PLAYING CHB")
        w = C(port, f"MOTE2 CLUB {GID} JOIN 3"); time.sleep(0.2)
        a.cut(); g.cut(); time.sleep(0.4)
        check("TABLE 1 -" not in w.ctrl, "held: table still there")
        time.sleep(3.5)
        check("TABLE 1 -" in w.ctrl, f"not back in time: the table is freed: {w.ctrl[-2:]}")
        x = C(port, f"MOTE2 CLUB {GID} JOIN 3")
        check(x.seat.startswith("SEAT 0 16 3"), f"...and the place given to the next: {x.seat}")
        for y in (h, w, x):
            y.cut()
        time.sleep(0.3)

        # not in a match: as ever
        a = C(port, f"MOTE2 CLUB {GID} JOIN 4")
        a.say("TABLE 3 - 0 UK 8-BALL"); time.sleep(0.1)
        w = C(port, f"MOTE2 CLUB {GID} JOIN 4"); time.sleep(0.2)
        a.cut(); time.sleep(0.4)
        check("TABLE 3 -" in w.ctrl, f"a member not in a match leaves as ever, the table freed: {w.ctrl[-2:]}")
        w.cut()
    finally:
        relay.terminate()
        out = relay.communicate(timeout=5)[0].decode("ascii", "replace")
        if bad:
            print(out[-4000:])
        shutil.rmtree(store)
    print(f"{ok} ok, {bad} bad")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
