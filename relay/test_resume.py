#!/usr/bin/env python3
"""test_resume.py -- coming back to a frame (7.5, Mark: "give people 5 minutes
to resume the game and get back in", and the forfeit to whoever went: "needs
to be A"). A real relay on a local port with short clocks:

  * a joiner dropped from a frame under way: the host is told AWAY, not LEFT,
    and the room stays; the joiner's account comes back to its own place
  * the HOST dropped: the room stays open (it used to close); back the same way
  * a player whose link backed up and was DROPPED BY THE RELAY (room K6R5,
    2026-10-12) is held the same, not counted as the other's walk-out
  * not back in time: GONE to the room, the forfeit theirs, the stayer's report
    kept with its figures
  * another account cannot take a held place
  * left before the frame was under way: LEFT, as ever

    python3 relay/test_resume.py
"""
import json, os, shutil, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_roomn import free_port, conn, readline, send_frame, recv_frame  # noqa: E402

GID = "1129661807"
ok = bad = 0


def check(cond, what):
    global ok, bad
    print(("  [ok ] " if cond else "  [BAD] ") + what)
    ok += bool(cond); bad += not cond


class P:
    """one end in a room, keeping the relay's words"""
    def __init__(self, port, line, uid=None, read=True):
        self.s = conn(port, line)
        self.seat = readline(self.s)
        self.ctrl = []
        self.uid = uid
        self.s.settimeout(None)
        if read:
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


def start_msg(me):
    return "STARTED " + json.dumps({"v": 1, "t": "start", "frame": 1, "kind": 7, "me": me, "best_of": 1,
                                    "names": ["HOSTY", "GUESTY"], "game": "STRAIGHT POOL"})


def result_msg(me, winner):
    side = {"shots": 9, "pot_shots": 8, "potted": 8, "fouls": 0, "best": 8, "breaks": [8]}
    r = {"v": 1, "t": "frame", "game": "STRAIGHT POOL", "kind": 7, "mode": 0, "best_of": 1, "frame": 1, "winner": winner,
         "match_winner": winner, "bnr": -1, "golden": 0, "me": me, "score": [8, 2], "frames": [0, 0],
         "names": ["HOSTY", "GUESTY"], "sides": [dict(side), dict(side, potted=2, best=2, breaks=[])], "ver": "7.5"}
    return "RESULT " + json.dumps(r)


def room(port, code, started=True, guest_reads=True):
    h = P(port, f"MOTE2 ROOMN {GID} HOST {code} PRIV 2 TEST", "711")
    g = P(port, f"MOTE2 ROOMN {GID} JOIN {code}", "722", read=guest_reads)
    h.say("START"); time.sleep(0.2)
    h.say("PROOF 711 desk711"); g.say("PROOF 722 desk722"); time.sleep(0.3)
    if started:
        h.say(start_msg(0)); g.say(start_msg(1)); time.sleep(0.3)
    return h, g


def main():
    store = tempfile.mkdtemp()
    port = free_port()
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, "mote_relay.py"), "--addr", "127.0.0.1", "--port", str(port),
                              "--store", store, "--tourn-trust", "--no-voice", "--resume-wait", "4", "--forfeit-wait", "2",
                              "--room-backlog", "6000"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
            except OSError:
                time.sleep(0.05)

        # ---- the joiner drops and comes back
        h, g = room(port, "RJA")
        g.cut(); time.sleep(0.4)
        check(any(l.startswith("AWAY 1 ") for l in h.ctrl) and not any(l.startswith(("LEFT", "CLOSED")) for l in h.ctrl),
              f"a joiner dropped mid-frame: the host hears AWAY, not LEFT: {h.ctrl[-2:]}")
        x = P(port, f"MOTE2 ROOMN {GID} REJOIN RJA 799 desk799")
        check(x.seat == "NOTYOU", f"another account cannot take the held place: {x.seat}")
        g2 = P(port, f"MOTE2 ROOMN {GID} REJOIN RJA 722 desk722", "722")
        time.sleep(0.3)
        check(g2.seat == "SEAT 1 2 BACK" and "BACK 1" in h.ctrl, f"the joiner's account is back at its own place: {g2.seat}")
        send_frame(h.s, 0xFF, b"after"); time.sleep(0.3)
        h.cut(); g2.cut(); time.sleep(0.3)

        # ---- the host drops: the room stays, and the host comes back
        h, g = room(port, "RJB")
        h.cut(); time.sleep(0.4)
        check(any(l.startswith("AWAY 0 ") for l in g.ctrl) and "CLOSED" not in g.ctrl,
              f"the host dropped mid-frame: the room stays open, AWAY 0: {g.ctrl[-2:]}")
        h2 = P(port, f"MOTE2 ROOMN {GID} REJOIN RJB 711 desk711", "711")
        time.sleep(0.3)
        check(h2.seat == "SEAT 0 2 BACK" and "BACK 0" in g.ctrl, f"the host is back at place 0: {h2.seat}")
        h2.cut(); g.cut(); time.sleep(0.3)

        # ---- a link that backed up, dropped by the relay (K6R5): held the same
        h, g = room(port, "RJC", guest_reads=False)
        for _ in range(400):
            send_frame(h.s, 0xFF, bytes(200))
        time.sleep(1.0)
        check(any(l.startswith("AWAY 1 ") for l in h.ctrl), f"a joiner the relay dropped for backlog is held, not a walk-out by the host: {h.ctrl[-2:]}")
        # ---- ...and not back in time: GONE, the forfeit theirs, the stayer's figures kept
        time.sleep(4.5)
        check("GONE 1" in h.ctrl, f"not back in time: the room hears GONE: {h.ctrl[-2:]}")
        h.say(result_msg(0, 0)); time.sleep(0.5)
        time.sleep(2.5)
        lines = [json.loads(l) for l in open(os.path.join(store, "results_forfeits.jsonl"))] if os.path.exists(os.path.join(store, "results_forfeits.jsonl")) else []
        f = [l for l in lines if l.get("room") == "RJC"]
        check(f and f[0].get("left") == "GUESTY" and f[0]["result"]["winner"] == 0 and f[0]["result"]["sides"][0]["best"] == 8,
              f"the forfeit is the one who went, the stayer's report kept with its run of 8: {[(x.get('left'), x['result'].get('winner'), x['result']['sides'][0].get('best')) for x in f]}")
        h.cut(); time.sleep(0.3)

        # ---- before the frame was under way: LEFT, as it always was
        h, g = room(port, "RJD", started=False)
        g.cut(); time.sleep(0.4)
        check("LEFT 1" in h.ctrl and not any(l.startswith("AWAY") for l in h.ctrl), f"left before a frame was under way: LEFT, as ever: {h.ctrl[-2:]}")
        h.cut()
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
